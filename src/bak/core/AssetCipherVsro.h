// ============================================================================
//  SRO Asset Studio - core/AssetCipherVsro.h
//
//  "vSRO File Protection" (vSroAssetGuard.dll) - the per-asset cipher used by
//  KhongTuoc-style servers. Unlike the HntSRO S-box, this one is real crypto:
//
//      serverKey = HMAC-SHA256(ROOT, "vSROFileEncryptor_ServerKey" || gateway)
//      nonce12   = HMAC-SHA256(serverKey, "vSROFileEncryptor_FileNonce" || path)[0..11]
//      keystream = AES-256-ECB(serverKey, nonce12 || BE32(offset >> 4)) ...
//      plain     = cipher XOR keystream                (whole file, from offset 0)
//
//  `gateway` is the server address out of the client's own divisioninfo.txt
//  (e.g. "clientsro.online"), lower-cased and trimmed. `path` is the archive
//  inner path put through the guard's own normalisation (see NormalizeInner).
//  ROOT is a 32-byte constant baked into vSroAssetGuard.dll.
//
//  The construction was recovered by emulating the two VMProtect-virtualised
//  derivation stubs and verified against the DLL's OWN self-test vector, so
//  everything here decrypts fully offline - no client, no server.
//
//  Only the files listed in the shard's GFXProtectFiles.txt are encrypted; a
//  plain asset is left untouched (the JMXV magic check decides).
// ============================================================================
#pragma once

#include "../Common.h"

#include <string>

namespace sro {

class IArchive;

namespace vsro {

/// The guard's path normalisation, reproduced exactly: trim whitespace, '/' to
/// '\', ASCII A-Z lower-cased (bytes >= 0x80 untouched), runs of '\' collapsed,
/// leading ".\" and "\" stripped.
std::string NormalizeInner(const std::string& path);

/// serverKey = HMAC-SHA256(ROOT, label || lower(trim(gateway)))
bool DeriveServerKey(const std::string& gateway, u8 keyOut[32]);

/// nonce = HMAC-SHA256(serverKey, label || NormalizeInner(path))[0..11]
bool DeriveFileNonce(const u8 key[32], const std::string& innerPath, u8 nonceOut[12]);

/// XOR `data` with the AES-256-CTR keystream. Self-inverse. `offset` is the
/// byte position of `data` inside the asset (0 for a whole-file buffer).
bool CryptBuffer(const u8 key[32], const u8 nonce[12], u8* data, size_t len, u64 offset = 0);

} // namespace vsro

// ---------------------------------------------------------------------------
// Session cipher. Set once when an archive is opened (the gateway is read from
// the client's divisioninfo.txt), then applied automatically to any asset whose
// signature is not JMXV.
// ---------------------------------------------------------------------------

/// Look for divisioninfo.txt in `archive` (and its siblings), take the first
/// gateway address and arm the cipher. Returns true when a gateway was found.
bool VsroTryInit(IArchive& archive);

/// Arm/disarm manually. An empty string disarms.
void VsroSetGateway(const std::string& gateway);

/// Armed? And with which gateway (for the log / crypt report).
bool         VsroActive();
std::wstring VsroGatewayTr();

/// Decrypt in place when `buf` is a vSRO-protected asset. The accept test is
/// structural: the first block is trial-decrypted and must come out as "JMXV".
/// A plain or foreign-cipher asset is left untouched and false is returned.
bool VsroDecryptIfEncrypted(const std::wstring& innerPath, std::vector<u8>& buf);

} // namespace sro
