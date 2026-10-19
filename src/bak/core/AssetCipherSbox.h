// ============================================================================
//  SRO Asset Studio - core/AssetCipherSbox.h
//
//  HntSRO / "SilkroadKit" GFXClientMod per-asset cipher.
//
//  Recovered from the running client: the shard injects a Detours hook DLL that
//  masquerades in the module list as "GFXFiIeManager.dll" (a homoglyph of
//  GFXFileManager.dll - capital I for lowercase l). Its hook of
//  CPFileManager::ReadFile transforms every asset AFTER it is read from the PK2:
//
//      plain[i] = TABLE_ext[ cipher[i] ] ^ K_ext           // POSITION-INDEPENDENT
//
//  i.e. a plain 256-byte byte-substitution (S-box) plus a constant XOR, chosen
//  by file extension:
//
//      .bms -> table@0x8198 ^0xCC     .bsr -> table@0x8398 ^0xDD
//      .ddj -> table@0x8598 ^0xBB     .txt -> table@0x8798 ^0xEE
//      everything else      -> table@0x8998 ^0x88   (default)
//
//  Because the transform depends only on the byte value, one composed decrypt
//  map per extension (MAP[c] = TABLE[c] ^ K) is the WHOLE cipher. This is fully
//  static: no key, no running client and no dumped samples are needed to read a
//  HntSRO archive. This is a DIFFERENT construction from the position-keyed XOR
//  keystream in AssetCrypt.h (Kim Hoa) - both live side by side.
// ============================================================================
#pragma once

#include "../Common.h"

#include <string>
#include <vector>

namespace sro {

/// Decrypt `buf` in place when it is a HntSRO-encrypted asset and the result is
/// verifiably plaintext (a JMXV magic for model formats, readable text for .txt,
/// or a confirmed-HntSRO archive for other binary types). A no-op that leaves
/// `buf` untouched otherwise, so a normal file is never mangled. Returns true
/// only when it actually replaced the buffer.
bool SboxDecryptIfEncrypted(const std::wstring& fileName, std::vector<u8>& buf);

/// Confirm THIS archive uses the HntSRO S-box by decrypting an already-fetched
/// encrypted model sample against the extension implied by `magic`
/// ("JMXVBMS.."/"JMXVBSR.."). Records the answer for the session so ambiguous
/// binary types (.ifo/.mfo/.2dt/...) can then be decrypted too. Returns true on
/// a confirmed match.
bool SboxConfirmFromSample(const u8* sample, size_t sampleLen,
                           const char* magic, size_t magicLen);

/// True once a HntSRO model has decrypted this session (archive confirmed).
bool SboxConfirmed();

} // namespace sro
