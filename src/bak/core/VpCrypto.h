// ============================================================================
//  SRO Asset Studio - core/VpCrypto.h
//
//  Crypto primitives for the Electus / "VSROPLUS TOOLS ENCRYPTED CONTAINER"
//  (.vp) format.  Zero third-party code: everything uses the Windows CNG
//  (bcrypt) and legacy CryptoAPI (advapi32 + crypt32) system libraries.
//
//  These back the .vp key-discovery + metadata decryption.  NO KEY IS BAKED
//  IN: the RSA private key is discovered from the client binaries at run time
//  (exactly like the PK2/LIB key search), then the container's own header
//  blob is unwrapped to obtain the per-license key, and the AES-GCM tag is the
//  self-validating oracle that tells us when a discovered key is correct.
// ============================================================================
#pragma once

#include "../Common.h"
#include <string>
#include <vector>

namespace sro { namespace vp {

/// HMAC-SHA256(key, data) -> 32 bytes.  false only on a CNG failure.
bool HmacSha256(const u8* key, size_t keyLen,
                const u8* data, size_t dataLen, u8 out[32]);

/// AES-256-GCM authenticated decrypt.  Returns true ONLY when the 16-byte tag
/// verifies - this is the oracle that validates a candidate key.
bool AesGcmDecrypt(const u8 key[32], const u8* nonce, size_t nonceLen,
                   const u8* ct, size_t ctLen, const u8* tag, size_t tagLen,
                   std::vector<u8>& out);

/// AES-256-GCM encrypt.  Produces the container's on-disk framing directly:
/// out = [12-byte random nonce][ciphertext][16-byte tag].
bool AesGcmEncrypt(const u8 key[32], const u8* pt, size_t ptLen, std::vector<u8>& out);

/// Cryptographically strong random bytes (BCryptGenRandom).
bool RandomBytes(u8* out, size_t len);

/// AES-256-ECB keystream XOR (the format's per-file "File_EncryptDecrypt").
/// key must be 32 bytes; counter = nonce(8) || blockIndex(8).  In == out size.
bool AesEcbKeystreamXor(const u8 key[32], u64 nonce,
                        const u8* in, size_t len, std::vector<u8>& out);

/// The format's file nonce:  first 8 bytes of SHA256(path without '/' '\\',
/// lower-cased), little-endian.
u64 FileNonce(const std::string& innerPathUtf8);

/// RSA private-key PKCS#1 v1.5 decrypt.  `pemText` is a full
/// "-----BEGIN RSA PRIVATE KEY----- ... -----END-----" PEM (PKCS#1).
/// `in` is the 256-byte big-endian wrapped blob from the .vp header.
/// Returns the unwrapped bytes (32 for the key, 16 for the password).
bool RsaPkcs1Decrypt(const std::string& pemText,
                     const u8* in, size_t inLen, std::vector<u8>& out);

/// Scan a byte buffer for the first embedded PKCS#1 RSA private-key PEM
/// (BEGIN..END inclusive).  Used to harvest the key from client binaries.
bool FindRsaPem(const u8* data, size_t len, std::string& pemOut);

}} // namespace sro::vp
