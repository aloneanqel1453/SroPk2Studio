// ============================================================================
//  SRO Asset Studio - core/AssetCipherGopro.h
//
//  GoPro / VSROPlus (vsroplus_lib.dll) whole-file AES-256-CBC asset protection.
//  Used by modern private shards (e.g. Republic90 and other VSROPlus servers).
//
//  Protected assets (meshes .bms, skeletons .bsk, animations .ban, materials
//  .bmt, resources .bsr) are encrypted with AES-256-CBC.
// ============================================================================
#pragma once

#include "../Common.h"

#include <string>
#include <vector>

namespace sro {

class IArchive;

/// Arms GoPro protection with a specific key and IV.
void GoproSetKey(const u8 key[32], const u8 iv[16]);

/// Is the GoPro cipher currently armed?
bool GoproActive();

/// Clear GoPro key and disarm.
void GoproClear();

/// Status text for UI / logs.
std::wstring GoproStatusTr();

/// Attempt to detect GoPro protector markers (clienthash.txt, encryptedfiles.txt,
/// or vsroplus_lib.dll in client folder / siblings) and arm the cipher.
bool GoproTryInit(IArchive& archive);

/// Decrypt in place if the buffer is GoPro AES-256-CBC encrypted.
/// Verifies structurally via 16-byte probe block (must decrypt to JMXV or known header).
/// Returns true if decrypted in place, false if untouched.
bool GoproDecryptIfEncrypted(const std::wstring& innerPath, std::vector<u8>& buf);

/// True if the data appears to be a GoPro encrypted asset.
bool GoproLooksEncrypted(const u8* data, size_t size);

} // namespace sro
