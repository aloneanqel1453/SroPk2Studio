// ============================================================================
//  SRO Asset Studio - core/L33tPayloadKey.cpp   (see L33tPayloadKey.h)
// ============================================================================
#include "L33tPayloadKey.h"
#include "VpArchive.h"
#include "VpCrypto.h"
#include "Obfuscate.h"

#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <vector>
#include <array>
#include <cstring>
#include <thread>
#include <atomic>
#include <mutex>

#pragma comment(lib, "bcrypt.lib")

namespace sro {

namespace {

using Crib = PayloadCrib;

// AES-256-ECB single block via BCrypt (keystream block for counter).
struct AesEcb {
    BCRYPT_ALG_HANDLE alg = nullptr;
    bool init() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return false;
        BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                          sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
        return true;
    }
    ~AesEcb() { if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
    bool block(const u8 key[32], const u8 in[16], u8 out[16]) const {
        BCRYPT_KEY_HANDLE k = nullptr;
        if (BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, (PUCHAR)key, 32, 0) < 0) return false;
        ULONG n = 0;
        NTSTATUS s = BCryptEncrypt(k, (PUCHAR)in, 16, nullptr, nullptr, 0, out, 16, &n, 0);
        BCryptDestroyKey(k);
        return s >= 0;
    }
};

// counter for block 0: <LE u64 nonce><LE u64 0>
void counter0(u64 nonce, u8 out[16]) {
    std::memcpy(out, &nonce, 8);
    std::memset(out + 8, 0, 8);
}

// Does 32-byte window `key` reproduce the crib's keystream prefix?
bool keyMatchesCrib(const AesEcb& aes, const u8* key, const Crib& c) {
    u8 ctr[16], ks[16];
    counter0(c.nonce, ctr);
    if (!aes.block(key, ctr, ks)) return false;
    return std::memcmp(ks, c.ks, (size_t)c.ksLen) == 0;
}

DWORD findPid(const std::wstring& name) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do { if (_wcsicmp(pe.szExeFile, name.c_str()) == 0) { pid = pe.th32ProcessID; break; } }
        while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// The strongest recovery: the client keeps a security-context object per container
// laid out as [perLicenseKey 32][metadataKey 32][mid 32][xorKey 32]. We can compute
// metadataKey OFFLINE (HMAC-SHA256(plk, containerId)); finding those 32 bytes in RAM
// and reading +64 yields the xorKey directly - no signature/crib guess, so it works
// for EVERY container (text-only ones like Settings included). Scans a process handle
// (findMeta) or a flat buffer (findMetaBuf). Returns true and fills xorKeyOut on hit.
bool findMetaInBuffer(const uint8_t* data, size_t n, const u8 metaKey[32], u8 xorKeyOut[32]) {
    if (n < 96) return false;
    for (size_t o = 0; o + 96 <= n; ++o) {
        if (std::memcmp(data + o, metaKey, 32) == 0) {
            std::memcpy(xorKeyOut, data + o + 64, 32);   // xorKey is 64 bytes past metadataKey
            return true;
        }
    }
    return false;
}

bool findMetaInProcess(HANDLE h, const u8 metaKey[32], u8 xorKeyOut[32]) {
    std::vector<uint8_t> buf;
    u64 addr = 0; MEMORY_BASIC_INFORMATION mbi;
    while (addr < 0x7FFFFFFF0000ULL && VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT && mbi.Protect != 0 &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            mbi.RegionSize <= 512ull * 1024 * 1024) {
            buf.resize(mbi.RegionSize);
            SIZE_T got = 0;
            if (ReadProcessMemory(h, mbi.BaseAddress, buf.data(), mbi.RegionSize, &got) && got >= 96) {
                if (findMetaInBuffer(buf.data(), got, metaKey, xorKeyOut)) return true;
            }
        }
        addr = (u64)mbi.BaseAddress + mbi.RegionSize;
        if (!mbi.RegionSize) addr += 0x1000;
    }
    return false;
}

// Scan committed memory of an open process handle for a 32-byte window matching
// any crib. On a hit, calls tryInstall(window) which validates+installs (returns
// true to stop). `cribs` drives both the fast candidate test and the count.
bool scanProcess(HANDLE h, const std::vector<Crib>& cribs,
                 const std::function<bool(const u8*)>& tryInstall) {
    AesEcb aes; if (!aes.init()) return false;
    std::vector<u8> buf;
    u64 addr = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < 0x7FFFFFFF0000ULL && VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi))) {
        const bool ok = mbi.State == MEM_COMMIT && mbi.Protect != 0 &&
                        !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                        mbi.RegionSize <= 512ull * 1024 * 1024;
        if (ok) {
            buf.resize(mbi.RegionSize);
            SIZE_T got = 0;
            if (ReadProcessMemory(h, mbi.BaseAddress, buf.data(), mbi.RegionSize, &got) && got >= 32) {
                const size_t N = got - 32;
                for (size_t off = 0; off <= N; ++off) {
                    const u8* w = buf.data() + off;
                    // cheap gate: match the first crib's keystream via one AES block
                    bool any = false;
                    for (const Crib& c : cribs) {
                        if (keyMatchesCrib(aes, w, c)) { any = true; break; }
                    }
                    if (any && tryInstall(w)) return true;
                }
            }
        }
        addr = (u64)mbi.BaseAddress + mbi.RegionSize;
        if (!mbi.RegionSize) addr += 0x1000;
    }
    return false;
}

// Scan a flat on-disk buffer (a full-memory dump) the same way.
bool scanBuffer(const std::vector<u8>& d, const std::vector<Crib>& cribs,
                const std::function<bool(const u8*)>& tryInstall) {
    if (d.size() < 32) return false;
    AesEcb aes; if (!aes.init()) return false;
    const size_t N = d.size() - 32;
    for (size_t off = 0; off <= N; ++off) {
        const u8* w = d.data() + off;
        for (const Crib& c : cribs) {
            if (keyMatchesCrib(aes, w, c)) {
                if (tryInstall(w)) return true;
                break;
            }
        }
    }
    return false;
}

} // namespace

// Build cribs from an open container: pick a handful of files whose extension
// has a known signature, read their raw (still-encrypted) head, and record the
// keystream prefix = cipher XOR knownPlain. Implemented as a VpArchive method
// because it needs raw reads + the record list (declared in VpArchive.h).

bool RecoverL33tPayloadKey(VpArchive& vp, const std::wstring& procName,
                           const std::function<void(const std::wstring&)>& log) {
    if (vp.payloadKeyReady()) return true;              // already good
    if (!vp.payloadEncrypted()) return true;            // nothing to decrypt

    std::vector<Crib> cribs;
    DWORD pid = findPid(procName);
    if (!pid) { if (log) log(T(L"Calisan istemci yok - yuk sifresi cozulemedi.")); return false; }
    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) { if (log) log(T(L"Istemci bellegine erisilemedi.")); return false; }

    bool ok = false;

    // Primary: the metadataKey+64 struct-offset method (crib-free, works for every
    // container). metadataKey is computed offline; find it in RAM, read xorKey at +64,
    // then installPayloadKey validates by trial-decrypting real files.
    u8 metaKey[32], xorCand[32];
    if (vp.metadataKey(metaKey) && findMetaInProcess(h, metaKey, xorCand)) {
        if (vp.installPayloadKey(xorCand)) ok = true;
    }

    // Fallback: known-plaintext crib scan (for containers/layouts where the struct
    // offset differs). Only attempted if the struct method did not resolve it.
    if (!ok && vp.buildPayloadCribs(cribs) && !cribs.empty()) {
        auto tryInstall = [&](const u8* w) -> bool { return vp.installPayloadKey(w); };
        ok = scanProcess(h, cribs, tryInstall);
    }
    CloseHandle(h);
    if (ok && log) log(T(L"Yuk sifresi cozuldu."));
    else if (!ok && log) log(T(L"Yuk sifresi cozulemedi."));
    return ok;
}

bool RecoverL33tPayloadKeyFromDump(VpArchive& vp, const std::wstring& dumpPath,
                                   const std::function<void(const std::wstring&)>& log) {
    if (vp.payloadKeyReady()) return true;
    if (!vp.payloadEncrypted()) return true;

    HANDLE f = CreateFileW(dumpPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    std::vector<u8> d;
    bool read = false;
    try {
        d.resize((size_t)sz.QuadPart);
        DWORD got = 0; size_t done2 = 0; read = true;
        while (done2 < d.size()) {
            DWORD want = (DWORD)(std::min<size_t>(1u << 24, d.size() - done2));
            if (!ReadFile(f, d.data() + done2, want, &got, nullptr) || got == 0) { read = false; break; }
            done2 += got;
        }
    } catch (...) { read = false; }
    CloseHandle(f);
    if (!read) return false;

    bool ok = false;
    // Primary: metadataKey+64 struct offset (crib-free).
    u8 metaKey[32], xorCand[32];
    if (vp.metadataKey(metaKey) && findMetaInBuffer(d.data(), d.size(), metaKey, xorCand)) {
        if (vp.installPayloadKey(xorCand)) ok = true;
    }
    // Fallback: crib scan.
    std::vector<Crib> cribs;
    if (!ok && vp.buildPayloadCribs(cribs) && !cribs.empty()) {
        auto tryInstall = [&](const u8* w) -> bool { return vp.installPayloadKey(w); };
        ok = scanBuffer(d, cribs, tryInstall);
    }
    if (ok && log) log(T(L"Yuk sifresi cozuldu (dump)."));
    return ok;
}

bool L33tKnownPlain(const std::wstring& name, unsigned char out[16], int& outLen) {
    auto ends = [&](const wchar_t* ext) {
        size_t n = wcslen(ext);
        return name.size() >= n && _wcsicmp(name.c_str() + name.size() - n, ext) == 0;
    };
    struct { const wchar_t* ext; const char* sig; } table[] = {
        { L".bms", "JMXVBMS " }, { L".bsr", "JMXVRES " }, { L".bmt", "JMXVBMT " },
        { L".ban", "JMXVBAN " }, { L".efp", "JMXVEFP " }, { L".ddj", "JMXVDDJ " },
        { L".cpd", "JMXVCPD " }, { L".nvm", "JMXVNVM " }, { L".dof", "JMXVDOF " },
        { L".ogg", "OggS" }, { L".wav", "RIFF" },
    };
    for (auto& e : table) {
        if (ends(e.ext)) {
            int L = (int)strlen(e.sig);
            std::memcpy(out, e.sig, (size_t)L);
            outLen = L;
            return true;
        }
    }
    return false;
}

bool IsL33tContainer(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    u8 buf[64] = {}; DWORD got = 0;
    bool r = ReadFile(h, buf, sizeof(buf), &got, nullptr) && got >= 26;
    CloseHandle(h);
    if (!r) return false;
    u16 idlen; std::memcpy(&idlen, buf, 2);
    const std::string kL33T = OBF("L33T ENCRYPTED CONTAINER").str();
    return idlen == (u16)kL33T.size() &&
           (size_t)2 + idlen <= got &&
           std::memcmp(buf + 2, kL33T.data(), idlen) == 0;
}

} // namespace sro
