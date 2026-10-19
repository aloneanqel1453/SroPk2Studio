// ============================================================================
//  SRO Asset Studio - core/L33tPayloadKey.h
//
//  Recovery of the L33T container payload keystream key (the per-container
//  AES-256 xorKey that decrypts file BYTES). The archive directory opens fully
//  offline from the embedded RSA key, but the payload xorKey is produced by the
//  client at run time inside a VMProtect'd routine and is NOT derivable from the
//  archive. So - exactly like the PK3 asset cipher and the vSRO protection - we
//  LIFT it once from a running client (or a full-process .dmp), prove it against
//  the container's own known-plaintext, install it, and cache it in the session
//  vault. After that every file previews/exports as plaintext with no further
//  scanning. Nothing is ever written to disk (RULE 1, [[sro-asset-studio-key-privacy]]).
// ============================================================================
#pragma once
#include "IArchive.h"
#include <string>
#include <functional>
#include <vector>

namespace sro {

class VpArchive;   // the L33T/VSROPLUS backend

/// A payload "crib": for one file we know the first plaintext bytes, so we can
/// recover the keystream prefix (cipher XOR knownPlain) and search RAM for the
/// 32-byte AES key that produces it. Built by VpArchive::buildPayloadCribs.
struct PayloadCrib {
    std::string path;           ///< inner path (drives the file nonce)
    unsigned long long nonce = 0;
    unsigned char cipher[16] = {};
    unsigned char ks[16] = {};  ///< recovered keystream prefix
    int           ksLen = 0;    ///< trustworthy keystream bytes (>=4 to be useful)
};

/// Try to resolve the payload key for one already-open L33T container.
///   - If it is already resolved (classic constant, or vault hit), returns true.
///   - Otherwise scans a running client process for the 32-byte xorKey, proven by
///     trial-decrypting this container's own files to a known signature, installs
///     it, and caches it in the session vault.
/// `procName` defaults to sro_client.exe. Returns true when the payload became
/// decryptable. Read-only w.r.t. the target process. `log` is optional.
bool RecoverL33tPayloadKey(VpArchive& vp,
                           const std::wstring& procName = L"sro_client.exe",
                           const std::function<void(const std::wstring&)>& log = {});

/// Same, but scans an on-disk full-memory dump (.dmp / raw) instead of a live
/// process - so a capture taken earlier still works with no client running.
bool RecoverL33tPayloadKeyFromDump(VpArchive& vp,
                                   const std::wstring& dumpPath,
                                   const std::function<void(const std::wstring&)>& log = {});

/// Is `path` an L33T container specifically (vs classic VSROPLUS)? Cheap header
/// magic check; used to decide whether payload-key recovery is even needed.
bool IsL33tContainer(const std::wstring& path);

/// Expected first plaintext bytes for a file, by extension (JMXV sigs / OggS).
/// Returns false when the extension has no known signature (that file cannot be
/// used as a crib). `out` holds up to 16 bytes, `outLen` the count.
bool L33tKnownPlain(const std::wstring& fileName, unsigned char out[16], int& outLen);

} // namespace sro
