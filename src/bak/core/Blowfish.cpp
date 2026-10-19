// ============================================================================
//  SRO Asset Studio - core/Blowfish.cpp
//
//  The SRO variant of Blowfish used on JMXPACK block tables.
//
//  Ported line for line from the reference C# implementation at
//  ../SROPK2-API-SRC/SRO.PK2API/SRO.PK2API/SRO.PK2API.Security/Blowfish.cs.
//  Two details are what make this the "SRO variant" rather than textbook
//  Blowfish, and both are easy to get wrong:
//
//    * The 8-byte block is split into two words read LITTLE ENDIAN
//      (l = LE32(block+0), r = LE32(block+4)) and written back the same way.
//    * Blowfish_encipher / _decipher SWAP the halves on the way out, so the
//      classic "undo the final swap" step is baked into the routine itself.
//
//  The key schedule itself is textbook: the 18+1024 words of pi (BlowfishTables.h),
//  key bytes folded in BIG ENDIAN, then 521 enciphers over the whole state.
//
//  VERIFIED vectors (SPEC.md §1), exercised by --selftest:
//    initializeGameKey("169841", BaseKeyId::Std)
//      -> encodeString("Joymax Pack File")[0..2] == d8 da 30
//      -> decrypting the 2560 bytes at offset 256 of a stock archive yields
//         01 2e 00 00 00 00 00 00
// ============================================================================
#include "Blowfish.h"
#include "BlowfishTables.h"

namespace sro {

// --- VERIFIED base keys (SPEC.md §1) ---------------------------------------
const u8 kBaseKeyStd[kBaseKeyLength] =
    { 0x03, 0xF8, 0xE4, 0x44, 0x88, 0x99, 0x3F, 0x64, 0xFE, 0x35 };

const u8 kBaseKeyParadise[kBaseKeyLength] =
    { 0xA8, 0x19, 0x72, 0xBF, 0x87, 0xD2, 0x0A, 0xFD, 0x37, 0x54 };

const u8 kBaseKeyZero[kBaseKeyLength] =
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

const u8* BaseKeyBytes(BaseKeyId id) {
    switch (id) {
        case BaseKeyId::Std:      return kBaseKeyStd;
        case BaseKeyId::Paradise: return kBaseKeyParadise;
        case BaseKeyId::Zero:     return kBaseKeyZero;
        default:                  return nullptr;
    }
}

const wchar_t* BaseKeyName(BaseKeyId id) {
    switch (id) {
        case BaseKeyId::Std:      return L"STD";
        case BaseKeyId::Paradise: return L"PARADISE";
        case BaseKeyId::Zero:     return L"ZERO";
        default:                  return L"OZEL";
    }
}

// ---------------------------------------------------------------------------
// Little-endian word access. The archives are x86 files, so this is fixed
// regardless of the host - we never rely on the CPU's own byte order.
// ---------------------------------------------------------------------------
namespace {

inline u32 LoadLe32(const u8* p) {
    return static_cast<u32>(p[0])
         | (static_cast<u32>(p[1]) << 8)
         | (static_cast<u32>(p[2]) << 16)
         | (static_cast<u32>(p[3]) << 24);
}

inline void StoreLe32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v & 0xFF);
    p[1] = static_cast<u8>((v >> 8) & 0xFF);
    p[2] = static_cast<u8>((v >> 16) & 0xFF);
    p[3] = static_cast<u8>((v >> 24) & 0xFF);
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
Blowfish::Blowfish() : m_state{}, m_valid(false) {}

// ---------------------------------------------------------------------------
// Core primitives
// ---------------------------------------------------------------------------
u32 Blowfish::feistel(u32 x) const {
    const u32 a = m_state.S[0][(x >> 24) & 0xFF];
    const u32 b = m_state.S[1][(x >> 16) & 0xFF];
    const u32 c = m_state.S[2][(x >> 8) & 0xFF];
    const u32 d = m_state.S[3][x & 0xFF];
    return (((a + b) ^ c) + d);
}

void Blowfish::encipher(u32& xl, u32& xr) const {
    u32 l = xl;
    u32 r = xr;

    l ^= m_state.P[0];
    // 16 rounds, alternating halves - mirrors ROUND(ref a, b, n) in the C#.
    r ^= feistel(l) ^ m_state.P[1];
    l ^= feistel(r) ^ m_state.P[2];
    r ^= feistel(l) ^ m_state.P[3];
    l ^= feistel(r) ^ m_state.P[4];
    r ^= feistel(l) ^ m_state.P[5];
    l ^= feistel(r) ^ m_state.P[6];
    r ^= feistel(l) ^ m_state.P[7];
    l ^= feistel(r) ^ m_state.P[8];
    r ^= feistel(l) ^ m_state.P[9];
    l ^= feistel(r) ^ m_state.P[10];
    r ^= feistel(l) ^ m_state.P[11];
    l ^= feistel(r) ^ m_state.P[12];
    r ^= feistel(l) ^ m_state.P[13];
    l ^= feistel(r) ^ m_state.P[14];
    r ^= feistel(l) ^ m_state.P[15];
    l ^= feistel(r) ^ m_state.P[16];
    r ^= m_state.P[17];

    // The halves come out swapped - this is part of the algorithm here.
    xl = r;
    xr = l;
}

void Blowfish::decipher(u32& xl, u32& xr) const {
    u32 l = xl;
    u32 r = xr;

    l ^= m_state.P[17];
    r ^= feistel(l) ^ m_state.P[16];
    l ^= feistel(r) ^ m_state.P[15];
    r ^= feistel(l) ^ m_state.P[14];
    l ^= feistel(r) ^ m_state.P[13];
    r ^= feistel(l) ^ m_state.P[12];
    l ^= feistel(r) ^ m_state.P[11];
    r ^= feistel(l) ^ m_state.P[10];
    l ^= feistel(r) ^ m_state.P[9];
    r ^= feistel(l) ^ m_state.P[8];
    l ^= feistel(r) ^ m_state.P[7];
    r ^= feistel(l) ^ m_state.P[6];
    l ^= feistel(r) ^ m_state.P[5];
    r ^= feistel(l) ^ m_state.P[4];
    l ^= feistel(r) ^ m_state.P[3];
    r ^= feistel(l) ^ m_state.P[2];
    l ^= feistel(r) ^ m_state.P[1];
    r ^= m_state.P[0];

    xl = r;
    xr = l;
}

// ---------------------------------------------------------------------------
// Key schedule
// ---------------------------------------------------------------------------
bool Blowfish::initializeRaw(const u8* key, size_t len) {
    m_valid = false;
    if (!key || len == 0 || len > kMaxKeyBytes) return false;

    std::memcpy(m_state.P, bftables::kInitP, sizeof(m_state.P));
    std::memcpy(m_state.S, bftables::kInitS, sizeof(m_state.S));

    // Fold the key into P, big-endian, cycling over the key bytes.
    size_t j = 0;
    for (size_t i = 0; i < 18; ++i) {
        const u32 data = (static_cast<u32>(key[j]) << 24)
                       | (static_cast<u32>(key[(j + 1) % len]) << 16)
                       | (static_cast<u32>(key[(j + 2) % len]) << 8)
                       | (static_cast<u32>(key[(j + 3) % len]));
        m_state.P[i] ^= data;
        j = (j + 4) % len;
    }

    // 521 enciphers rewrite the whole state.
    u32 l = 0, r = 0;
    for (size_t i = 0; i < 18; i += 2) {
        encipher(l, r);
        m_state.P[i]     = l;
        m_state.P[i + 1] = r;
    }
    for (size_t box = 0; box < 4; ++box) {
        for (size_t k = 0; k < 256; k += 2) {
            encipher(l, r);
            m_state.S[box][k]     = l;
            m_state.S[box][k + 1] = r;
        }
    }

    m_valid = true;
    return true;
}

size_t Blowfish::DeriveGameKey(const std::string& asciiKey,
                               const u8* baseKey, size_t baseLen,
                               u8* out) {
    if (!out || asciiKey.empty()) return 0;
    const size_t len = (std::min)(asciiKey.size(), kMaxKeyBytes);
    for (size_t i = 0; i < len; ++i) {
        const u8 b = (baseKey && i < baseLen) ? baseKey[i] : static_cast<u8>(0);
        out[i] = static_cast<u8>(static_cast<u8>(asciiKey[i]) ^ b);
    }
    return len;
}

bool Blowfish::initializeGameKey(const std::string& asciiKey,
                                 const u8* baseKey, size_t baseLen) {
    m_valid = false;
    u8 derived[kMaxKeyBytes] = {};
    const size_t len = DeriveGameKey(asciiKey, baseKey, baseLen, derived);
    if (len == 0) return false;
    const bool ok = initializeRaw(derived, len);
    ::SecureZeroMemory(derived, sizeof(derived));
    return ok;
}

bool Blowfish::initializeGameKey(const std::string& asciiKey, BaseKeyId baseId) {
    const u8* base = BaseKeyBytes(baseId);
    if (!base) { m_valid = false; return false; }   // Custom needs the pointer overload
    return initializeGameKey(asciiKey, base, kBaseKeyLength);
}

bool Blowfish::initializeFromState(const BlowfishState& st) {
    m_state = st;
    m_valid = true;
    return true;
}

bool Blowfish::initializeFromStateBytes(const u8* bytes, size_t len, bool reversedLayout) {
    m_valid = false;
    if (!bytes || len != kBlowfishStateSize) return false;

    if (!reversedLayout) {
        std::memcpy(&m_state, bytes, kBlowfishStateSize);
    } else {
        // Some builds lay the object out as (S then P).
        const size_t sBytes = sizeof(m_state.S);
        const size_t pBytes = sizeof(m_state.P);
        std::memcpy(m_state.S, bytes, sBytes);
        std::memcpy(m_state.P, bytes + sBytes, pBytes);
    }
    m_valid = true;
    return true;
}

void Blowfish::exportState(u8* dst) const {
    if (dst) std::memcpy(dst, &m_state, sizeof(m_state));
}

// ---------------------------------------------------------------------------
// Crypt
// ---------------------------------------------------------------------------
void Blowfish::encodeBlock(u8* block8) const {
    if (!m_valid || !block8) return;
    u32 l = LoadLe32(block8);
    u32 r = LoadLe32(block8 + 4);
    encipher(l, r);
    StoreLe32(block8, l);
    StoreLe32(block8 + 4, r);
}

void Blowfish::decodeBlock(u8* block8) const {
    if (!m_valid || !block8) return;
    u32 l = LoadLe32(block8);
    u32 r = LoadLe32(block8 + 4);
    decipher(l, r);
    StoreLe32(block8, l);
    StoreLe32(block8 + 4, r);
}

void Blowfish::encodeInPlace(u8* p, size_t n) const {
    if (!m_valid || !p || (n % 8) != 0) return;
    for (size_t i = 0; i < n; i += 8) encodeBlock(p + i);
}

void Blowfish::decodeInPlace(u8* p, size_t n) const {
    if (!m_valid || !p || (n % 8) != 0) return;
    for (size_t i = 0; i < n; i += 8) decodeBlock(p + i);
}

bool Blowfish::encode(const u8* in, size_t n, std::vector<u8>& out) const {
    out.clear();
    if (!m_valid || !in || n == 0) return false;
    const size_t padded = OutputLength(n);
    try { out.resize(padded, 0); }
    catch (...) { out.clear(); return false; }
    std::memcpy(out.data(), in, n);          // tail stays zero-padded
    encodeInPlace(out.data(), padded);
    return true;
}

bool Blowfish::decode(const u8* in, size_t n, std::vector<u8>& out) const {
    out.clear();
    if (!m_valid || !in || n == 0 || (n % 8) != 0) return false;
    try { out.resize(n); }
    catch (...) { out.clear(); return false; }
    std::memcpy(out.data(), in, n);
    decodeInPlace(out.data(), n);
    return true;
}

bool Blowfish::encodeString(const char* text, std::vector<u8>& out) const {
    out.clear();
    if (!text) return false;
    const size_t n = std::strlen(text);
    if (n == 0) return false;
    return encode(reinterpret_cast<const u8*>(text), n, out);
}

} // namespace sro
