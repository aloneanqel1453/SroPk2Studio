// SkillEffect Studio v3 - client/Pk2Crypto.cpp
#include "Pk2Crypto.h"

#include "../core/BlowfishTables.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace se::client {

namespace {

constexpr std::uint8_t kBaseStd[10] = {0x03, 0xF8, 0xE4, 0x44, 0x88, 0x99, 0x3F, 0x64, 0xFE, 0x35};
constexpr std::uint8_t kBaseParadise[10] = {0xA8, 0x19, 0x72, 0xBF, 0x87, 0xD2, 0x0A, 0xFD, 0x37, 0x54};

inline std::uint32_t Le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline void PutLe32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseHex(const std::string& h, std::vector<std::uint8_t>& out) {
    out.clear();
    std::string t;
    for (char c : h)
        if (c != ' ' && c != ':' && c != '-') t += c;
    if (t.empty() || t.size() % 2) return false;
    for (size_t i = 0; i < t.size(); i += 2) {
        const int a = HexVal(t[i]), b = HexVal(t[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(static_cast<std::uint8_t>(a * 16 + b));
    }
    return true;
}

bool IEq(const std::string& a, const char* b) {
    const size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
        if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
        if (x != y) return false;
    }
    return true;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ Blowfish
bool SroBlowfish::Init(const std::uint8_t* key, size_t len) {
    if (!key || len == 0 || len > 56) return false;
    std::memcpy(P, sro::bftables::kInitP, sizeof(P));
    std::memcpy(S, sro::bftables::kInitS, sizeof(S));
    size_t j = 0;
    for (auto& p : P) {
        const std::uint32_t d = (static_cast<std::uint32_t>(key[j]) << 24) |
                                (static_cast<std::uint32_t>(key[(j + 1) % len]) << 16) |
                                (static_cast<std::uint32_t>(key[(j + 2) % len]) << 8) |
                                static_cast<std::uint32_t>(key[(j + 3) % len]);
        p ^= d;
        j = (j + 4) % len;
    }
    std::uint32_t l = 0, r = 0;
    for (size_t i = 0; i < 18; i += 2) {
        Enc(l, r);
        P[i] = l;
        P[i + 1] = r;
    }
    for (auto& box : S) {
        for (size_t k = 0; k < 256; k += 2) {
            Enc(l, r);
            box[k] = l;
            box[k + 1] = r;
        }
    }
    return true;
}

std::uint32_t SroBlowfish::F(std::uint32_t x) const {
    return ((S[0][(x >> 24) & 0xFF] + S[1][(x >> 16) & 0xFF]) ^ S[2][(x >> 8) & 0xFF]) + S[3][x & 0xFF];
}

void SroBlowfish::Enc(std::uint32_t& xl, std::uint32_t& xr) const {
    std::uint32_t l = xl ^ P[0], r = xr;
    for (int i = 1; i <= 16; i += 2) {
        r ^= F(l) ^ P[i];
        l ^= F(r) ^ P[i + 1];
    }
    r ^= P[17];
    xl = r;
    xr = l;
}

void SroBlowfish::Dec(std::uint32_t& xl, std::uint32_t& xr) const {
    std::uint32_t l = xl ^ P[17], r = xr;
    for (int i = 16; i >= 1; i -= 2) {
        r ^= F(l) ^ P[i];
        l ^= F(r) ^ P[i - 1];
    }
    r ^= P[0];
    xl = r;
    xr = l;
}

void SroBlowfish::EncodeBlock(std::uint8_t* b) const {
    std::uint32_t l = Le32(b), r = Le32(b + 4);
    Enc(l, r);
    PutLe32(b, l);
    PutLe32(b + 4, r);
}

void SroBlowfish::DecodeBlock(std::uint8_t* b) const {
    std::uint32_t l = Le32(b), r = Le32(b + 4);
    Dec(l, r);
    PutLe32(b, l);
    PutLe32(b + 4, r);
}

void SroBlowfish::Decode(std::uint8_t* b, size_t n) const {
    for (size_t i = 0; i + 8 <= n; i += 8) DecodeBlock(b + i);
}

// ------------------------------------------------------------------------------------------------ key specs
bool KeySpecBytes(const std::string& spec, std::vector<std::uint8_t>& key) {
    key.clear();
    if (spec.rfind("raw:", 0) == 0) return ParseHex(spec.substr(4), key) && !key.empty() && key.size() <= 56;
    std::string ascii = spec, base = "STD";
    const size_t at = spec.rfind('@');
    if (at != std::string::npos && at > 0) {
        ascii = spec.substr(0, at);
        base = spec.substr(at + 1);
    }
    if (ascii.empty() || ascii.size() > 56) return false;
    std::vector<std::uint8_t> b;
    if (IEq(base, "STD")) b.assign(kBaseStd, kBaseStd + 10);
    else if (IEq(base, "PARADISE")) b.assign(kBaseParadise, kBaseParadise + 10);
    else if (IEq(base, "ZERO")) b.clear();
    else if (!ParseHex(base, b) || b.empty() || b.size() > 56) return false;
    for (size_t i = 0; i < ascii.size(); ++i)
        key.push_back(static_cast<std::uint8_t>(static_cast<std::uint8_t>(ascii[i]) ^ (i < b.size() ? b[i] : 0)));
    return true;
}

bool KeySpecIsPlainAscii(const std::string& spec) {
    return !spec.empty() && spec.find('@') == std::string::npos && spec.rfind("raw:", 0) != 0;
}

std::wstring KeySpecFormTr(const std::string& spec) {
    if (spec.rfind("raw:", 0) == 0) return L"anahtar baytları doğrudan";
    const size_t at = spec.rfind('@');
    if (at == std::string::npos || at == 0) return L"standart biçim";
    const std::string base = spec.substr(at + 1);
    if (IEq(base, "PARADISE")) return L"PARADISE biçimi";
    if (IEq(base, "ZERO")) return L"tabansız biçim";
    return L"istemci dosyasından okunan biçim";
}

// ------------------------------------------------------------------------------------------------ probe + oracles
bool Pk2Probe::Load(const std::wstring& path) {
    ok = false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        whyTr = L"Dosya açılamadı (başka bir program kilitlemiş ya da erişim yok): " + path;
        whyEn = L"Cannot open the file (locked or no access): " + path;
        return false;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    fileSize = static_cast<std::uint64_t>(sz.QuadPart);
    FILETIME ft{};
    GetFileTime(h, nullptr, nullptr, &ft);
    lastWrite = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    DWORD got = 0;
    const bool hdrOk = ::ReadFile(h, header, kPk2HeaderSize, &got, nullptr) && got == kPk2HeaderSize;
    const bool rootOk = hdrOk && ::ReadFile(h, root, kPk2BlockSize, &got, nullptr) && got == kPk2BlockSize;
    CloseHandle(h);
    if (!hdrOk || std::memcmp(header, "JoyMax File Manager!", 20) != 0) {
        whyTr = L"Bu dosya bir pk2 arşivi değil (JoyMax imzası yok): " + path;
        whyEn = L"Not a pk2 archive (no JoyMax signature): " + path;
        return false;
    }
    if (!rootOk) {
        whyTr = L"Pk2 arşivi kesik / bozuk (kök dizin bloğu okunamadı): " + path;
        whyEn = L"Truncated / damaged pk2 (root directory block missing): " + path;
        return false;
    }
    encrypted = header[34] != 0;
    std::memcpy(checksum, header + 35, 3);
    ok = true;
    return true;
}

bool Pk2Probe::RootOracle(const SroBlowfish& bf) const {
    std::uint8_t b[8];
    std::memcpy(b, root, 8);
    bf.DecodeBlock(b);
    if (b[0] != 1 || b[1] != '.' || b[2] != 0) return false;
    std::uint8_t plain[kPk2BlockSize];
    std::memcpy(plain, root, kPk2BlockSize);
    bf.Decode(plain, kPk2BlockSize);
    for (std::uint32_t k = 0; k < 20; ++k)
        if (plain[k * kPk2EntrySize] > 2) return false;
    return true;
}

bool Pk2Probe::ChecksumOracle(const SroBlowfish& bf) const {
    std::uint8_t chk[16];
    std::memcpy(chk, "Joymax Pack File", 16);
    bf.EncodeBlock(chk);
    bf.EncodeBlock(chk + 8);
    return std::memcmp(chk, checksum, 3) == 0;
}

bool Pk2Probe::TestKey(const std::uint8_t* key, size_t len) const {
    if (!ok) return false;
    if (!encrypted) return true;
    SroBlowfish bf;
    return bf.Init(key, len) && RootOracle(bf);
}

bool Pk2Probe::TestSpec(const std::string& spec) const {
    std::vector<std::uint8_t> k;
    return KeySpecBytes(spec, k) && TestKey(k.data(), k.size());
}

std::string Pk2Probe::Fingerprint() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%llx-%02x%02x%02x-%llx", static_cast<unsigned long long>(fileSize), checksum[0],
                  checksum[1], checksum[2], static_cast<unsigned long long>(lastWrite));
    return buf;
}

}  // namespace se::client
