// ============================================================================
//  SRO Asset Studio - core/Pk2v2Crypto.cpp   (see Pk2v2Crypto.h)
// ============================================================================
#include "Pk2v2Crypto.h"
#include "VpCrypto.h"     // vp::HmacSha256 (CNG HMAC-SHA256) - reuse, no dup
#include "Obfuscate.h"

#include <windows.h>
#include <bcrypt.h>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

namespace sro { namespace pk2v2 {

// ---------------------------------------------------------------------------
// Key derivation: PBKDF2-HMAC-SHA256(password, q16 || "PK2v2-key" || 0*7, 4096, 32)
//
// The salt tail label "PK2v2-key" is the one the client's wrapper 0xED6F20 loads
// from its constant pool (16 bytes: the 9 ascii chars + 7 NULs). We rebuild the
// 32-byte salt = 16 (q16) + 16 (label||pad). Obfuscated so it never sits plain.
// ---------------------------------------------------------------------------
bool DeriveKey(const std::string& password, const u8 q16[16], u8 keyOut[32]) {
    // salt = q16(16) + "PK2v2-key"(9) + 0x00*7  == 32 bytes
    u8 salt[32];
    std::memcpy(salt, q16, 16);
    std::memset(salt + 16, 0, 16);
    {
        const auto lbl = OBF("PK2v2-key");          // 9 chars
        std::memcpy(salt + 16, lbl.data(), lbl.size() < 16 ? lbl.size() : 16);
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != STATUS_SUCCESS)
        return false;

    // password may be empty; BCryptDeriveKeyPBKDF2 accepts a 0-length secret.
    NTSTATUS st = BCryptDeriveKeyPBKDF2(
        hAlg,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()),
        salt, 32,
        kKdfIters,
        keyOut, kKdfDkLen,
        0);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    SecureZeroMemory(salt, sizeof(salt));
    return st == STATUS_SUCCESS;
}

bool VerifyTag(const u8 key[32], u8 tagOut[12]) {
    u8 full[32];
    const auto lbl = OBF("PK2v2-check");             // 11 chars
    if (!vp::HmacSha256(key, 32,
                        reinterpret_cast<const u8*>(lbl.data()), lbl.size(), full))
        return false;
    std::memcpy(tagOut, full, 12);
    return true;
}

bool PasswordMatches(const std::string& password, const u8 q16[16], const u8 vtag[12]) {
    u8 key[32], tag[12];
    if (!DeriveKey(password, q16, key)) return false;
    bool ok = VerifyTag(key, tag) && std::memcmp(tag, vtag, 12) == 0;
    SecureZeroMemory(key, sizeof(key));
    return ok;
}

// ---------------------------------------------------------------------------
// ChaCha20 (RFC 8439 quarter-round, DJB nonce framing: 64-bit counter + 64-bit
// nonce). The client's core 0xED6D30 is the classic DJB layout: constant(4) |
// key(8) | counter(2) | nonce(2) 32-bit words, counter starting at 0, little
// endian throughout. nonce words = [header[0x64..0x68], "2LBT"].
// ---------------------------------------------------------------------------
namespace {
inline u32 rotl32(u32 v, int c) { return (v << c) | (v >> (32 - c)); }
inline u32 rd32le(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
#define QR(a,b,c,d) \
    a += b; d ^= a; d = rotl32(d,16); \
    c += d; b ^= c; b = rotl32(b,12); \
    a += b; d ^= a; d = rotl32(d, 8); \
    c += d; b ^= c; b = rotl32(b, 7)

void chacha_block(const u32 in[16], u8 out[64]) {
    u32 x[16];
    for (int i = 0; i < 16; ++i) x[i] = in[i];
    for (int i = 0; i < 10; ++i) {        // 20 rounds = 10 double rounds
        QR(x[0], x[4], x[ 8], x[12]);
        QR(x[1], x[5], x[ 9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[ 8], x[13]);
        QR(x[3], x[4], x[ 9], x[14]);
    }
    for (int i = 0; i < 16; ++i) {
        u32 v = x[i] + in[i];
        out[i*4+0] = (u8)(v);
        out[i*4+1] = (u8)(v >> 8);
        out[i*4+2] = (u8)(v >> 16);
        out[i*4+3] = (u8)(v >> 24);
    }
}
} // namespace

void ChaCha20Decrypt8B(const u8 key[32], const u8 nonce[8],
                       const u8* in, size_t len, u8* out, u64 counter) {
    // "expand 32-byte k"
    u32 state[16];
    state[0] = 0x61707865u; state[1] = 0x3320646eu;
    state[2] = 0x79622d32u; state[3] = 0x6b206574u;
    for (int i = 0; i < 8; ++i) state[4 + i] = rd32le(key + i*4);
    // counter words 12,13 (64-bit), start at given counter
    state[12] = (u32)counter;
    state[13] = (u32)(counter >> 32);
    // nonce words 14,15: little-endian from 8-byte nonce
    state[14] = rd32le(nonce);
    state[15] = rd32le(nonce + 4);

    u8 ks[64];
    size_t off = 0;
    while (off < len) {
        chacha_block(state, ks);
        size_t n = len - off; if (n > 64) n = 64;
        for (size_t i = 0; i < n; ++i) out[off + i] = in[off + i] ^ ks[i];
        off += n;
        // increment 64-bit counter (words 12 then 13)
        if (++state[12] == 0) ++state[13];
    }
    SecureZeroMemory(ks, sizeof(ks));
    SecureZeroMemory(state, sizeof(state));
}

void ChaCha20Decrypt(const u8 key[32], const u8 nonceWord1[4],
                     const u8* in, size_t len, u8* out) {
    u8 nonce[8];
    std::memcpy(nonce, nonceWord1, 4);
    const u32 nw2 = kNonceWord2;
    std::memcpy(nonce + 4, &nw2, 4);
    ChaCha20Decrypt8B(key, nonce, in, len, out, 0);
}

}} // namespace sro::pk2v2
