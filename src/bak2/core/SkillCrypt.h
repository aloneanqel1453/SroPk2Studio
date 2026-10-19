// SkillEffect Studio - core/SkillCrypt.h
// skilldata_*ENC.txt (the files the vSRO 1.188 client really loads through skilldataenc.txt):
//   enc[i] = (plain[i] + KEY[i % 165330]) mod 256 over the whole UTF-16LE plaintext (BOM included),
//   followed by the 22 raw bytes "\r\n//encrypt" (UTF-16LE). KEY restarts at 0 for every file.
// (EXPORT_SPEC.md §1.4 / sro_textdb.skilldata_encrypt; byte-exact on all 7 shipped files.)
#pragma once

#include "Text.h"

namespace se {

bool IsSkillEnc(const Bytes& b);                          // ends with the "//encrypt" marker
bool SkillDecrypt(const Bytes& enc, Bytes& plain);        // false when the marker is missing
Bytes SkillEncrypt(const Bytes& plain);                   // plain = BOM + UTF-16LE rows
// plain skilldata text from either form (decrypts when needed)
std::wstring SkillDataText(const Bytes& b);

}  // namespace se
