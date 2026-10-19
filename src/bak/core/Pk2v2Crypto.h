// ============================================================================
//  SRO Asset Studio - core/Pk2v2Crypto.h
//
//  Crypto primitives for the Sailor "Arabian Dawn" PK2v2 container (the new
//  format that replaced the old SailSafe DJ&- / Blowfish@0x33E wrapper).
//
//  The whole recipe was reverse-engineered byte-for-byte from the client's own
//  SRO_Client.exe (Themida-unpacked) and validated against it:
//    * key derive  : wrapper VA 0xED6F20 -> PBKDF2 core 0xED6830  (4096 iters, 32B)
//    * verify tag  : HMAC-SHA256(key,"PK2v2-check")[:12]  (sub 0xED67E0)
//    * dir cipher  : dispatcher 0xED7F80 -> ChaCha20 core 0xED6D30  (DJB, 8B nonce)
//    * dir compress: ZSTD  (0xF3FF50 createDCtx / 0xF413B0 decompressDCtx)
//    * entry parser: sub 0xED7CB0 (0x40-byte records + a string pool)
//
//  NO SERVER KEY IS BAKED IN.  Only the FORMAT-level labels are constants
//  ("PK2v2-key", "PK2v2-check", the "2LBT" nonce word); those are held through
//  OBF() so a strings/disassembly dump of our exe shows noise, exactly like the
//  JoyMax signature and the VSROPLUS/L33T labels.  The per-client PASSWORD is a
//  runtime secret; it is supplied at open time (never stored here in the clear).
// ============================================================================
#pragma once

#include "../Common.h"
#include <string>
#include <vector>

namespace sro { namespace pk2v2 {

// --- fixed format parameters (confirmed by emulation) ----------------------
constexpr u32    kHeaderMagic0  = 0x32764b50;   // "PK2v"  (header[0x00])
constexpr u32    kHeaderMagic1  = 0x00003250;   // "2\0\0\0" low word; full check uses 8 bytes
constexpr u32    kNonceWord2    = 0x54424c32;   // "2LBT" little-endian, nonce bytes [4:8]
constexpr u32    kDirMagic      = 0x324c4254;   // "2LBT" at directory blob offset 0
constexpr u32    kKdfIters      = 4096;
constexpr u32    kKdfDkLen      = 32;
constexpr u32    kHeaderSize    = 0x80;
constexpr u32    kVerifyTagLen  = 12;

/// PBKDF2-HMAC-SHA256(password, salt = q16 || "PK2v2-key" || 0x00*7, 4096, 32).
/// `q16` is header[0x40:0x50]. Writes 32 bytes to `keyOut`. false only on a CNG
/// failure. The salt label is obfuscated in our binary; it is reconstructed here.
bool DeriveKey(const std::string& password, const u8 q16[16], u8 keyOut[32]);

/// HMAC-SHA256(key, "PK2v2-check")[:12] -> tagOut. Format verify tag.
bool VerifyTag(const u8 key[32], u8 tagOut[12]);

/// true when DeriveKey(password,q16) reproduces header's verify tag. This is the
/// self-validating oracle: a wrong password can never match (2^-96).
bool PasswordMatches(const std::string& password, const u8 q16[16], const u8 vtag[12]);

/// ChaCha20 (DJB variant: 8-byte nonce + 64-bit block counter starting at 0).
/// nonce = nonceWord1(header[0x64:0x68], as-is) || LE32(kNonceWord2). Decrypts
/// in place. Pure, portable implementation (no system dependency).
void ChaCha20Decrypt(const u8 key[32], const u8 nonceWord1[4],
                     const u8* in, size_t len, u8* out);

/// ChaCha20 with arbitrary 8-byte nonce and 64-bit block counter starting at `counter`.
/// Used for both archive directory and individual PK2v2 file decryption.
void ChaCha20Decrypt8B(const u8 key[32], const u8 nonce[8],
                       const u8* in, size_t len, u8* out, u64 counter = 0);

/// ZSTD single-frame decompress. `expectedSize` is header[0x28] (the frame's
/// decompressed size). Returns true and fills `out` on success. Implemented in
/// Pk2v2Zstd.cpp (a self-contained decompressor; no libzstd dependency).
bool ZstdDecompress(const u8* in, size_t inLen, size_t expectedSize,
                    std::vector<u8>& out);

}} // namespace sro::pk2v2
