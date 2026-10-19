// ============================================================================
//  SRO Asset Studio - core/AesCore.h
//
//  A small, fast AES-128/192/256 used by the asset-cipher FINDER.
//
//  Why not BCrypt (which AssetCipherVsro.cpp uses)? The finder expands a key
//  and decrypts two or three blocks for EVERY candidate it harvests, and a real
//  client binary yields millions of candidates. A CNG key handle per candidate
//  costs several microseconds - three orders of magnitude more than the maths.
//  This header does the whole thing in registers with no allocation.
//
//  Decrypt-only on purpose: the finder never needs to encrypt, and shipping an
//  encryptor invites someone to "helpfully" re-encrypt an asset with a key that
//  is only a guess.
// ============================================================================
#pragma once

#include "../Common.h"

#include <cstring>

namespace sro {
namespace aes {

// ---------------------------------------------------------------------------
// Tables. The inverse S-box and the four InvMixColumns multiplication tables
// are built once on first use; a per-byte GF multiply loop is ~5x slower and
// this sits in the finder's innermost loop.
// ---------------------------------------------------------------------------
struct Tables {
    u8 sbox[256];
    u8 rsbox[256];
    u8 mul9[256], mul11[256], mul13[256], mul14[256];

    Tables() {
        static const u8 kS[256] = {
        0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
        0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
        0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
        0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
        0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
        0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
        0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
        0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
        0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
        0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
        0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
        0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
        0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
        0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
        0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
        0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };
        std::memcpy(sbox, kS, 256);
        for (int i = 0; i < 256; ++i) rsbox[sbox[i]] = static_cast<u8>(i);
        for (int i = 0; i < 256; ++i) {
            const u8 a = static_cast<u8>(i);
            mul9[i]  = gf(a, 9);
            mul11[i] = gf(a, 11);
            mul13[i] = gf(a, 13);
            mul14[i] = gf(a, 14);
        }
    }

    static u8 gf(u8 a, u8 b) {
        u8 r = 0;
        while (b) {
            if (b & 1) r ^= a;
            a = static_cast<u8>((a << 1) ^ ((a >> 7) * 0x1b));
            b >>= 1;
        }
        return r;
    }
};

inline const Tables& T() { static const Tables t; return t; }

// ---------------------------------------------------------------------------
// Key schedule. keyLen is 16 / 24 / 32; anything else is rejected.
// Round keys: 16 * (rounds + 1) bytes, at most 240.
// ---------------------------------------------------------------------------
struct Key {
    u8  rk[240];
    int rounds = 0;
    bool valid() const { return rounds != 0; }
};

inline bool Expand(const u8* key, int keyLen, Key& out) {
    int nk;
    switch (keyLen) {
        case 16: nk = 4; out.rounds = 10; break;
        case 24: nk = 6; out.rounds = 12; break;
        case 32: nk = 8; out.rounds = 14; break;
        default: out.rounds = 0; return false;
    }
    const Tables& t = T();
    std::memcpy(out.rk, key, static_cast<size_t>(keyLen));

    const int total = 4 * (out.rounds + 1);          // words
    u8 rcon = 1;
    for (int i = nk; i < total; ++i) {
        u8 w[4] = { out.rk[(i - 1) * 4 + 0], out.rk[(i - 1) * 4 + 1],
                    out.rk[(i - 1) * 4 + 2], out.rk[(i - 1) * 4 + 3] };
        if (i % nk == 0) {
            const u8 tmp = w[0];                      // RotWord + SubWord + Rcon
            w[0] = static_cast<u8>(t.sbox[w[1]] ^ rcon);
            w[1] = t.sbox[w[2]];
            w[2] = t.sbox[w[3]];
            w[3] = t.sbox[tmp];
            rcon = static_cast<u8>((rcon << 1) ^ ((rcon >> 7) * 0x1b));
        } else if (nk > 6 && i % nk == 4) {           // AES-256 only
            for (int k = 0; k < 4; ++k) w[k] = t.sbox[w[k]];
        }
        for (int k = 0; k < 4; ++k)
            out.rk[i * 4 + k] = static_cast<u8>(out.rk[(i - nk) * 4 + k] ^ w[k]);
    }
    return true;
}

// ---------------------------------------------------------------------------
// One inverse-cipher block. `in` and `out` may alias.
// ---------------------------------------------------------------------------
inline void DecryptBlock(const Key& key, const u8* in, u8* out) {
    const Tables& t = T();
    u8 s[16];
    const int last = key.rounds * 16;
    for (int i = 0; i < 16; ++i) s[i] = static_cast<u8>(in[i] ^ key.rk[last + i]);

    for (int round = key.rounds - 1; round >= 0; --round) {
        u8 v[16];
        for (int c = 0; c < 4; ++c) {                 // InvShiftRows + InvSubBytes
            v[c * 4 + 0] = t.rsbox[s[((c + 0) & 3) * 4 + 0]];
            v[c * 4 + 1] = t.rsbox[s[((c + 3) & 3) * 4 + 1]];
            v[c * 4 + 2] = t.rsbox[s[((c + 2) & 3) * 4 + 2]];
            v[c * 4 + 3] = t.rsbox[s[((c + 1) & 3) * 4 + 3]];
        }
        for (int i = 0; i < 16; ++i) v[i] ^= key.rk[round * 16 + i];
        if (round > 0) {
            for (int c = 0; c < 4; ++c) {             // InvMixColumns
                const u8 a0 = v[c * 4 + 0], a1 = v[c * 4 + 1];
                const u8 a2 = v[c * 4 + 2], a3 = v[c * 4 + 3];
                s[c * 4 + 0] = static_cast<u8>(t.mul14[a0] ^ t.mul11[a1] ^ t.mul13[a2] ^ t.mul9 [a3]);
                s[c * 4 + 1] = static_cast<u8>(t.mul9 [a0] ^ t.mul14[a1] ^ t.mul11[a2] ^ t.mul13[a3]);
                s[c * 4 + 2] = static_cast<u8>(t.mul13[a0] ^ t.mul9 [a1] ^ t.mul14[a2] ^ t.mul11[a3]);
                s[c * 4 + 3] = static_cast<u8>(t.mul11[a0] ^ t.mul13[a1] ^ t.mul9 [a2] ^ t.mul14[a3]);
            }
        } else {
            std::memcpy(s, v, 16);
        }
    }
    std::memcpy(out, s, 16);
}

/// CBC over a whole buffer. `n` must be a non-zero multiple of 16.
inline bool CbcDecrypt(const u8* key, int keyLen, const u8 iv[16],
                       const u8* in, size_t n, u8* out) {
    if (!in || !out || n == 0 || (n % 16) != 0) return false;
    Key k;
    if (!Expand(key, keyLen, k)) return false;

    u8 prev[16];
    std::memcpy(prev, iv, 16);
    for (size_t off = 0; off < n; off += 16) {
        u8 cur[16];
        std::memcpy(cur, in + off, 16);               // survives in == out
        DecryptBlock(k, cur, out + off);
        for (int i = 0; i < 16; ++i) out[off + i] ^= prev[i];
        std::memcpy(prev, cur, 16);                   // ciphertext chains
    }
    return true;
}

/// ECB over a whole buffer.
inline bool EcbDecrypt(const u8* key, int keyLen, const u8* in, size_t n, u8* out) {
    if (!in || !out || n == 0 || (n % 16) != 0) return false;
    Key k;
    if (!Expand(key, keyLen, k)) return false;
    for (size_t off = 0; off < n; off += 16) DecryptBlock(k, in + off, out + off);
    return true;
}

} // namespace aes
} // namespace sro
