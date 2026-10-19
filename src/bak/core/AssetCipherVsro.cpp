// ============================================================================
//  SRO Asset Studio - core/AssetCipherVsro.cpp   (see AssetCipherVsro.h)
// ============================================================================
#include "AssetCipherVsro.h"

#include "IArchive.h"
#include "SiblingArchives.h"
#include "VpCrypto.h"                 // vp::HmacSha256 (BCrypt-backed)
#include "../formats/ClientData.h"    // ParseDivisionInfo

#include <windows.h>
#include <bcrypt.h>
#include <mutex>
#pragma comment(lib, "bcrypt.lib")

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

namespace sro {

namespace {

// The 32-byte root key baked into vSroAssetGuard.dll. This is a FORMAT
// constant of that protection, not a user secret - it is identical in every
// shard that ships the guard, and without it the format simply cannot be read.
const u8 kRoot[32] = {
    0x5e, 0x8f, 0xe0, 0x05, 0x92, 0x42, 0xe3, 0xf8,
    0x0e, 0x6c, 0xa2, 0xea, 0x7a, 0x28, 0x37, 0xca,
    0x2d, 0x53, 0x67, 0xf7, 0x20, 0x51, 0x9d, 0xa9,
    0x7f, 0x42, 0x31, 0xfb, 0xd6, 0x41, 0x90, 0x41,
};

const char kLabelKey[]   = "vSROFileEncryptor_ServerKey";
const char kLabelNonce[] = "vSROFileEncryptor_FileNonce";

/// AES-256-ECB over a whole buffer of counter blocks (each block independent).
bool AesEcbEncryptBlocks(const u8 key[32], u8* blocks, size_t bytes) {
    if (bytes == 0 || (bytes % 16) != 0) return false;
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != STATUS_SUCCESS)
        return false;
    bool ok = false;
    if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                          (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                          sizeof(BCRYPT_CHAIN_MODE_ECB), 0) == STATUS_SUCCESS) {
        BCRYPT_KEY_HANDLE hKey = nullptr;
        if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                       const_cast<PUCHAR>(key), 32, 0) == STATUS_SUCCESS) {
            ULONG done = 0;
            ok = BCryptEncrypt(hKey, blocks, (ULONG)bytes, nullptr, nullptr, 0,
                               blocks, (ULONG)bytes, &done, 0) == STATUS_SUCCESS &&
                 done == bytes;
            BCryptDestroyKey(hKey);
        }
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

std::string TrimAscii(const std::string& s) {
    size_t a = 0, b = s.size();
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (a < b && ws(s[a])) ++a;
    while (b > a && ws(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// --- session state ---------------------------------------------------------
std::mutex   g_mtx;
bool         g_active = false;
u8           g_key[32] = {};
std::string  g_gateway;

} // namespace

// ---------------------------------------------------------------------------
namespace vsro {

std::string NormalizeInner(const std::string& path) {
    const std::string t = TrimAscii(path);
    std::string out;
    out.reserve(t.size());
    bool prevSep = false;
    for (char raw : t) {
        char c = raw;
        if (c == '/') c = '\\';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
        if (c == '\\') {
            if (prevSep) continue;      // collapse runs of separators
            prevSep = true;
        } else {
            prevSep = false;
        }
        out.push_back(c);
    }
    while (out.size() >= 2 && out[0] == '.' && out[1] == '\\') out.erase(0, 2);
    while (!out.empty() && out[0] == '\\')                     out.erase(0, 1);
    return out;
}

bool DeriveServerKey(const std::string& gateway, u8 keyOut[32]) {
    std::string gw = TrimAscii(gateway);
    for (char& c : gw) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);

    std::vector<u8> msg;
    msg.reserve(sizeof(kLabelKey) - 1 + gw.size());
    msg.insert(msg.end(), kLabelKey, kLabelKey + sizeof(kLabelKey) - 1);
    msg.insert(msg.end(), gw.begin(), gw.end());
    return vp::HmacSha256(kRoot, sizeof(kRoot), msg.data(), msg.size(), keyOut);
}

bool DeriveFileNonce(const u8 key[32], const std::string& innerPath, u8 nonceOut[12]) {
    const std::string p = NormalizeInner(innerPath);
    std::vector<u8> msg;
    msg.reserve(sizeof(kLabelNonce) - 1 + p.size());
    msg.insert(msg.end(), kLabelNonce, kLabelNonce + sizeof(kLabelNonce) - 1);
    msg.insert(msg.end(), p.begin(), p.end());

    u8 full[32] = {};
    if (!vp::HmacSha256(key, 32, msg.data(), msg.size(), full)) return false;
    std::memcpy(nonceOut, full, 12);
    return true;
}

bool CryptBuffer(const u8 key[32], const u8 nonce[12], u8* data, size_t len, u64 offset) {
    if (!data || len == 0) return true;

    const size_t phase0 = static_cast<size_t>(offset & 15);
    u64 counter = offset >> 4;

    // Work in chunks so a 100 MB asset never needs a 100 MB keystream buffer.
    constexpr size_t kChunkBlocks = 4096;              // 64 KB of keystream
    std::vector<u8> ks;
    size_t pos = 0, phase = phase0;
    while (pos < len) {
        const size_t want   = len - pos + phase;       // bytes of keystream needed
        const size_t blocks = (std::min)(kChunkBlocks, (want + 15) / 16);
        ks.resize(blocks * 16);
        for (size_t b = 0; b < blocks; ++b) {
            u8* blk = ks.data() + b * 16;
            std::memcpy(blk, nonce, 12);
            const u32 c = static_cast<u32>((counter + b) & 0xFFFFFFFFull);
            blk[12] = static_cast<u8>(c >> 24);        // BIG-ENDIAN counter
            blk[13] = static_cast<u8>(c >> 16);
            blk[14] = static_cast<u8>(c >> 8);
            blk[15] = static_cast<u8>(c);
        }
        if (!AesEcbEncryptBlocks(key, ks.data(), ks.size())) return false;

        const size_t avail = ks.size() - phase;
        const size_t n     = (std::min)(avail, len - pos);
        for (size_t i = 0; i < n; ++i) data[pos + i] ^= ks[phase + i];
        pos     += n;
        counter += blocks;
        phase    = 0;
    }
    return true;
}

} // namespace vsro

// ---------------------------------------------------------------------------
void VsroSetGateway(const std::string& gateway) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (gateway.empty()) {
        g_active = false;
        g_gateway.clear();
        std::memset(g_key, 0, sizeof(g_key));
        return;
    }
    g_active = vsro::DeriveServerKey(gateway, g_key);
    g_gateway = g_active ? gateway : std::string();
}

bool VsroActive() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_active;
}

std::wstring VsroGatewayTr() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return MultiByteToWide(g_gateway, CP_ACP);
}

bool VsroTryInit(IArchive& archive) {
    // divisioninfo.txt sits at the archive root in Media; when the user opened
    // Data, the sibling containers answer instead.
    std::vector<u8> raw;
    const wchar_t* kCandidates[] = { L"divisioninfo.txt",
                                     L"server_dep\\silkroad\\divisioninfo.txt" };
    bool got = false;
    for (const wchar_t* c : kCandidates) {
        raw.clear();
        if (archive.isOpen() && archive.readFile(c, raw).ok() && !raw.empty()) { got = true; break; }
        raw.clear();
        if (SiblingArchives::Instance().read(c, raw) && !raw.empty()) { got = true; break; }
    }
    if (!got) return false;

    DivisionInfoFile div;
    if (!ParseDivisionInfo(raw.data(), raw.size(), div).ok()) return false;
    for (const DivisionEntry& d : div.divisions) {
        for (const std::wstring& gw : d.gatewayAddresses) {
            if (gw.empty()) continue;
            VsroSetGateway(WideToUtf8(gw));            // gateway is plain ASCII
            if (VsroActive()) return true;
        }
    }
    return false;
}

bool VsroDecryptIfEncrypted(const std::wstring& innerPath, std::vector<u8>& buf) {
    if (buf.size() < 4) return false;
    if (buf[0] == 'J' && buf[1] == 'M' && buf[2] == 'X' && buf[3] == 'V')
        return false;                                   // already plain

    u8 key[32];
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_active) return false;
        std::memcpy(key, g_key, sizeof(key));
    }

    u8 nonce[12];
    const std::string path = WideToUtf8(innerPath);
    if (!vsro::DeriveFileNonce(key, path, nonce)) return false;

    // Structural accept test first: decrypt only the leading block and require
    // the JMXV magic, so a foreign-cipher asset is never mangled.
    u8 probe[16] = {};
    const size_t n = (std::min<size_t>)(sizeof(probe), buf.size());
    std::memcpy(probe, buf.data(), n);
    if (!vsro::CryptBuffer(key, nonce, probe, n, 0)) return false;
    if (!(probe[0] == 'J' && probe[1] == 'M' && probe[2] == 'X' && probe[3] == 'V'))
        return false;

    return vsro::CryptBuffer(key, nonce, buf.data(), buf.size(), 0);
}

} // namespace sro
