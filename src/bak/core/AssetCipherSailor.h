// ============================================================================
//  SRO Asset Studio - core/AssetCipherSailor.h
//
//  Sailor Online ("Arabian Dawn") "Custom Encryption" for server-sensitive
//  textdata files.
//
//  The shard ships a SUBSET of its data files encrypted while leaving the index
//  files and everything else in the clear. TWO families of files are encrypted,
//  both with the SAME cipher and the SAME table:
//
//      TEXTDATA  : a per-build SUBSET of the numbered/split data tables under
//                  server_dep/silkroad/textdata/ - itemdata_NNNN.txt,
//                  skilldata_NNNN.txt, characterdata_NNNN.txt. Which ones are
//                  encrypted is decided at pack time, not by name or number
//                  (in the June-24 Media.pk2 every characterdata_* is encrypted,
//                  every skilldata_* is plain, and itemdata_* is mixed across the
//                  SAME number range), so detection is CONTENT-BASED only: the
//                  fixed encrypted-BOM signature { 0x68, 0x9F }. Never by name.
//                  The plaintext is UTF-16LE beginning with a BOM (ff fe).
//      BINARY    : the whole navmesh set - Data.pk2/navmesh/*.nvm (6183 files in
//                  the June-24 client). The plaintext is a normal JMXV asset
//                  ("JMXVNVM 1000..."), so the encrypted file carries the fixed
//                  4-byte signature { 0xB3, 0xEE, 0xDD, 0xF7 } (= "JMXV" + the
//                  first four key bytes). Same table, same offset-0 start; only
//                  the plaintext magic differs. Other JMXV extensions
//                  (.bms/.bsr/.ban/...) ship PLAIN in this build, so only .nvm is
//                  affected, but the detector is content-based and would pick up
//                  any JMXV asset the shard chose to encrypt this way.
//      PLAIN     : the un-numbered index files (itemdata.txt, skilldata.txt,
//                  characterdata.txt), the rest of the numbered tables, all other
//                  textdata, every plain JMXV asset (raw "JMXV..."), and every
//                  non-encrypted file.
//
//  The cipher is a POSITION-KEYED ADDITIVE stream over a fixed 4096-byte table
//  (period confirmed at exactly 4096 across every encrypted file):
//
//      cipher[i] = (plain[i] + kSailorKey[i % 4096]) & 0xFF
//      plain[i]  = (cipher[i] - kSailorKey[i % 4096]) & 0xFF        // decrypt
//
//  For a UTF-16LE textdata file the plaintext starts ff fe, so the encrypted
//  file starts with (0xff+K0, 0xfe+K1) = { 0x68, 0x9F }. For a JMXV binary the
//  plaintext starts 4a 4d 58 56 ("JMXV"), so the encrypted file starts with
//  ("JMXV" + K0..K3) = { 0xB3, 0xEE, 0xDD, 0xF7 }.
//
//  Only the textdata family carries a trailer: after the encrypted payload the
//  shard appends, UNENCRYPTED, the UTF-16LE bytes of
//  "\r\nSailor Online Custom Encryption" as a signature; that trailer is
//  stripped on decrypt (its final 0x00 high byte may be truncated when the
//  archived length is odd). The binary navmesh files have no trailer.
//
//  An EMPTY numbered table (e.g. itemdata_22800.txt) ships as nothing but the
//  encrypted BOM plus that trailer - a 2-byte payload that decrypts to exactly
//  "ff fe". The self-verify accepts a BOM-only result as a valid empty file, so
//  these are decrypted too instead of being left as ciphertext.
//
//  This is fully STATIC - the table was recovered byte-exact from the client's
//  own (Themida-virtualised) decryptor and verified to reproduce its output
//  with zero differences, so no running client and no samples are needed. It is
//  a different construction from both the Kim Hoa position-keyed XOR keystream
//  (AssetCrypt.h) and the HntSRO byte-substitution S-box (AssetCipherSbox.h);
//  all three live side by side.
// ============================================================================
#pragma once

#include "../Common.h"

#include <string>
#include <vector>

namespace sro {

/// Decrypt `buf` in place when it is a Sailor "Custom Encryption" file and the
/// result is verifiably plaintext - either valid UTF-16LE beginning with a BOM
/// (the textdata family) or a JMXV asset (the binary navmesh family). A no-op
/// that leaves `buf` untouched otherwise, so a normal file is never mangled.
/// Returns true only when it actually replaced the buffer. The trailing
/// "Sailor Online Custom Encryption" marker (textdata only) is removed.
bool SailorDecryptIfEncrypted(const std::wstring& fileName, std::vector<u8>& buf);

/// Content-only test: does this buffer carry a Sailor encrypted signature -
/// the encrypted BOM { 0x68, 0x9F } (textdata) or the encrypted JMXV magic
/// { 0xB3, 0xEE, 0xDD, 0xF7 } (binary)? (Used so the preview can say "still
/// encrypted" instead of showing garbage in the unlikely event decryption is
/// skipped.)
bool SailorLooksEncrypted(const u8* data, size_t size);

/// TEST-ONLY helper. The product NEVER encrypts, but the self-test needs to
/// manufacture ciphertext from known plaintext to prove the decrypt round-trips.
/// Applies the forward additive step  cipher[i] = (plain[i] + K[i % 4096]) & 0xFF
/// using the same (masked-at-rest) table the decryptor uses, so no copy of the
/// key ever lands in the test translation unit. Returns the encrypted bytes.
std::vector<u8> SailorEncryptForTest(const std::vector<u8>& plain);

} // namespace sro
