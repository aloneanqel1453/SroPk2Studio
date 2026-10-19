// ============================================================================
//  SRO Asset Studio - core/VpCrypto.cpp   (see VpCrypto.h)
// ============================================================================
#include "VpCrypto.h"
#include "Obfuscate.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

namespace sro { namespace vp {

// ---------------------------------------------------------------------------
// HMAC-SHA256
// ---------------------------------------------------------------------------
bool HmacSha256(const u8* key, size_t keyLen,
                const u8* data, size_t dataLen, u8 out[32]) {
    BCRYPT_ALG_HANDLE  hAlg  = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != STATUS_SUCCESS)
        return false;
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0,
                         const_cast<PUCHAR>(key), (ULONG)keyLen, 0) == STATUS_SUCCESS) {
        if (BCryptHashData(hHash, const_cast<PUCHAR>(data), (ULONG)dataLen, 0) == STATUS_SUCCESS &&
            BCryptFinishHash(hHash, out, 32, 0) == STATUS_SUCCESS)
            ok = true;
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

// ---------------------------------------------------------------------------
// AES-256-GCM decrypt + verify
// ---------------------------------------------------------------------------
bool AesGcmDecrypt(const u8 key[32], const u8* nonce, size_t nonceLen,
                   const u8* ct, size_t ctLen, const u8* tag, size_t tagLen,
                   std::vector<u8>& out) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != STATUS_SUCCESS)
        return false;
    if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                          (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) == STATUS_SUCCESS &&
        BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                   const_cast<PUCHAR>(key), 32, 0) == STATUS_SUCCESS) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = const_cast<PUCHAR>(nonce);   info.cbNonce = (ULONG)nonceLen;
        info.pbTag   = const_cast<PUCHAR>(tag);      info.cbTag   = (ULONG)tagLen;
        out.resize(ctLen);
        ULONG produced = 0;
        NTSTATUS s = BCryptDecrypt(hKey, const_cast<PUCHAR>(ct), (ULONG)ctLen, &info,
                                   nullptr, 0,
                                   out.empty() ? nullptr : out.data(), (ULONG)out.size(),
                                   &produced, 0);
        if (s == STATUS_SUCCESS) { out.resize(produced); ok = true; }
        BCryptDestroyKey(hKey);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

// ---------------------------------------------------------------------------
bool RandomBytes(u8* out, size_t len) {
    return BCryptGenRandom(nullptr, out, (ULONG)len,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// AES-256-GCM encrypt -> [nonce12][ct][tag16]
// ---------------------------------------------------------------------------
bool AesGcmEncrypt(const u8 key[32], const u8* pt, size_t ptLen, std::vector<u8>& out) {
    u8 nonce[12];
    if (!RandomBytes(nonce, sizeof(nonce))) return false;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != STATUS_SUCCESS)
        return false;
    if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                          (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) == STATUS_SUCCESS &&
        BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                   const_cast<PUCHAR>(key), 32, 0) == STATUS_SUCCESS) {
        u8 tag[16] = {};
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce; info.cbNonce = sizeof(nonce);
        info.pbTag   = tag;   info.cbTag   = sizeof(tag);

        std::vector<u8> ct(ptLen);
        ULONG produced = 0;
        if (BCryptEncrypt(hKey, const_cast<PUCHAR>(pt), (ULONG)ptLen, &info,
                          nullptr, 0,
                          ct.empty() ? nullptr : ct.data(), (ULONG)ct.size(),
                          &produced, 0) == STATUS_SUCCESS) {
            out.clear();
            out.reserve(12 + produced + 16);
            out.insert(out.end(), nonce, nonce + 12);
            out.insert(out.end(), ct.begin(), ct.begin() + produced);
            out.insert(out.end(), tag, tag + 16);
            ok = true;
        }
        BCryptDestroyKey(hKey);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

// ---------------------------------------------------------------------------
// AES-256-ECB keystream XOR  (File_EncryptDecrypt)
// ---------------------------------------------------------------------------
bool AesEcbKeystreamXor(const u8 key[32], u64 nonce,
                        const u8* in, size_t len, std::vector<u8>& out) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != STATUS_SUCCESS)
        return false;
    bool ok = false;
    if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                          (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                          sizeof(BCRYPT_CHAIN_MODE_ECB), 0) == STATUS_SUCCESS &&
        BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                   const_cast<PUCHAR>(key), 32, 0) == STATUS_SUCCESS) {
        out.resize(len);
        u8 ctr[16], ks[16]; ULONG produced = 0; ok = true;
        std::memcpy(ctr, &nonce, 8);
        for (size_t i = 0; i < len; ) {
            u64 block = (u64)(i / 16);
            std::memcpy(ctr + 8, &block, 8);
            if (BCryptEncrypt(hKey, ctr, 16, nullptr, nullptr, 0, ks, 16, &produced, 0) != STATUS_SUCCESS) {
                ok = false; break;
            }
            size_t n = (std::min)((size_t)16, len - i);
            for (size_t j = 0; j < n; ++j) out[i + j] = in[i + j] ^ ks[j];
            i += n;
        }
        BCryptDestroyKey(hKey);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

u64 FileNonce(const std::string& innerPathUtf8) {
    std::string s;
    s.reserve(innerPathUtf8.size());
    for (char c : innerPathUtf8) {
        if (c == '/' || c == '\\') continue;
        s.push_back((char)std::tolower((unsigned char)c));
    }
    u8 digest[32] = {};
    BCRYPT_ALG_HANDLE  hAlg  = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == STATUS_SUCCESS) {
        if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) == STATUS_SUCCESS) {
            BCryptHashData(hHash, (PUCHAR)s.data(), (ULONG)s.size(), 0);
            BCryptFinishHash(hHash, digest, 32, 0);
            BCryptDestroyHash(hHash);
        }
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }
    u64 n; std::memcpy(&n, digest, 8); return n;
}

// ---------------------------------------------------------------------------
// RSA PKCS#1 v1.5 private decrypt via legacy CryptoAPI
// ---------------------------------------------------------------------------
bool RsaPkcs1Decrypt(const std::string& pemText,
                     const u8* in, size_t inLen, std::vector<u8>& out) {
    // 1) PEM (base64 + header) -> DER
    DWORD derLen = 0;
    if (!CryptStringToBinaryA(pemText.c_str(), (DWORD)pemText.size(),
                              CRYPT_STRING_BASE64HEADER, nullptr, &derLen, nullptr, nullptr))
        return false;
    std::vector<u8> der(derLen);
    if (!CryptStringToBinaryA(pemText.c_str(), (DWORD)pemText.size(),
                              CRYPT_STRING_BASE64HEADER, der.data(), &derLen, nullptr, nullptr))
        return false;

    // 2) DER (PKCS#1 RSAPrivateKey) -> CryptoAPI PRIVATEKEYBLOB
    u8*   keyBlob = nullptr; DWORD keyBlobLen = 0;
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                             PKCS_RSA_PRIVATE_KEY, der.data(), derLen,
                             CRYPT_DECODE_ALLOC_FLAG, nullptr, &keyBlob, &keyBlobLen))
        return false;

    bool ok = false;
    HCRYPTPROV hProv = 0; HCRYPTKEY hKey = 0;
    if (CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES,
                             CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        if (CryptImportKey(hProv, keyBlob, keyBlobLen, 0, 0, &hKey)) {
            // CryptoAPI wants the ciphertext little-endian.
            std::vector<u8> buf(in, in + inLen);
            std::reverse(buf.begin(), buf.end());
            DWORD len = (DWORD)buf.size();
            if (CryptDecrypt(hKey, 0, TRUE, 0, buf.data(), &len)) {
                out.assign(buf.begin(), buf.begin() + len);
                ok = true;
            }
            SecureZeroMemory(buf.data(), buf.size());
            CryptDestroyKey(hKey);
        }
        CryptReleaseContext(hProv, 0);
    }
    SecureZeroMemory(der.data(), der.size());
    SecureZeroMemory(keyBlob, keyBlobLen);
    LocalFree(keyBlob);
    return ok;
}

bool FindRsaPem(const u8* data, size_t len, std::string& pemOut) {
    // The BEGIN/END scan markers are obfuscated (Obfuscate.h) so the exe carries
    // no plaintext "BEGIN RSA PRIVATE" needle; rebuilt to the exact text at run time.
    const std::string kBeg = OBF("-----BEGIN RSA PRIVATE KEY-----").str();
    const std::string kEnd = OBF("-----END RSA PRIVATE KEY-----").str();
    const size_t begN = kBeg.size(), endN = kEnd.size();
    if (len < begN + endN) return false;
    for (size_t i = 0; i + begN <= len; ++i) {
        if (std::memcmp(data + i, kBeg.data(), begN) != 0) continue;
        for (size_t j = i + begN; j + endN <= len; ++j) {
            if (std::memcmp(data + j, kEnd.data(), endN) == 0) {
                pemOut.assign(reinterpret_cast<const char*>(data + i), (j + endN) - i);
                return true;
            }
        }
        // Truncated block: keep scanning, a later PEM may still be intact.
    }
    return false;
}

}} // namespace sro::vp
