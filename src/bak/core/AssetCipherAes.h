// ============================================================================
//  SRO Asset Studio - core/AssetCipherAes.h
//
//  A whole-file AES asset cipher whose KEY IS DISCOVERED, never compiled in.
//
//  ==========================================================================
//   WHY THIS FILE REPLACED THE HARDCODED ONE
//  ==========================================================================
//  The first Origin Online support shipped the key and the IV as constants,
//  which breaks KeyFinder.h RULE 0 ("nothing server-specific is hardcoded") and,
//  worse, is simply wrong: the shard re-keys on the next client build and the
//  constant becomes a lie that silently corrupts every asset it touches. The
//  binaries the key was originally read out of are also named randomly per
//  build, so NOTHING here may key off a file name.
//
//  ==========================================================================
//   HOW A KEY IS PROVEN WITHOUT KNOWING THE IV
//  ==========================================================================
//  In CBC, P0 = D_k(C0) XOR IV. Every asset of one extension starts with the
//  same 12-byte signature ("JMXVRES 0109"), and the shard uses ONE fixed IV, so
//  for a correct k the raw block decryption D_k(C0) must come out IDENTICAL in
//  its first 12 bytes for every sample of that extension - the unknown IV and
//  the unknown version digits cancel. That is a 96-bit test that needs no
//  known plaintext at all, and it is what separates the real key from millions
//  of candidates. A second extension then has to agree on IV[0..7] = D_k(C0)
//  XOR "JMXVRES ", which is an independent 64-bit confirmation.
//
//  ==========================================================================
//   HOW THE IV FALLS OUT OF THE ARCHIVE ITSELF
//  ==========================================================================
//    IV[0..7]   = D_k(C0) XOR the 8-byte tag, exactly.
//    IV[8..11]  = the four version digits. Per byte, keep the values that make
//                 EVERY extension's digit land in '0'..'9'; two extensions
//                 collapse 256 candidates to one.
//    IV[12..15] = a .ddj states its own size: plaintext[12..15] = texSize with
//                 20 + texSize == plaintext length, and the length is the
//                 ciphertext length minus a 0..15 byte pad. Sixteen candidates
//                 per sample, intersected across samples -> exactly one.
//  So once the key is known the IV needs no binary at all. Harvesting it from
//  the scanned buffers is only the fallback, and a needed one: on the client
//  this was built for, the IV is not a string anywhere - it is assembled by 16
//  separate `mov byte ptr [esp+disp], imm8` instructions, i.e. the bytes sit in
//  the code stream 8 apart. That is why the harvester walks STRIDES, not just
//  contiguous runs; a plain string scan finds the key and misses the IV.
//
//  RULE 1 still holds: a discovered key lives in this process only, is never
//  written to disk, and is never printed to the log (see
//  [[sro-asset-studio-key-privacy]] - the log is screenshotted).
// ============================================================================
#pragma once

#include "../Common.h"

#include <string>
#include <vector>

namespace sro {

class IArchive;

// ---------------------------------------------------------------------------
// The cipher a shard turned out to be using.
// ---------------------------------------------------------------------------
struct AesAssetSpec {
    int  keyLen = 16;          ///< 16 / 24 / 32
    u8   key[32] = {};
    u8   iv[16]  = {};
    bool hasIv   = false;      ///< false => ECB (no IV involved)

    bool valid() const { return keyLen == 16 || keyLen == 24 || keyLen == 32; }
};

/// One encrypted asset to reason about. `sig8` is the 8-byte JMXV tag its
/// extension demands ("JMXVRES "), which is the only plaintext we assume.
struct AesSample {
    std::string     sig8;
    std::vector<u8> data;      ///< raw ciphertext, size % 16 == 0
    std::wstring    nameTr;    ///< for diagnostics only
    /// The four version digits ("0109"), when a PLAINTEXT file of this same
    /// extension was found in the archive. That pins IV[8..11] exactly instead
    /// of leaving it to the digit-plausibility solve, and costs nothing: mixed
    /// archives, where only some assets are encrypted, are the common case.
    std::string     knownVersion;
};

/// What the search did. Deliberately split from the user-facing log: `howTr`
/// and `sourceFile` name the route to the key and are therefore only ever shown
/// behind an explicit --showkey.
struct AesFindReport {
    bool         found = false;
    u64          candidates = 0;     ///< unique windows harvested
    u64          tested = 0;         ///< oracle runs
    int          sampleGroups = 0;
    std::wstring sourceFile;
    u64          sourceOffset = 0;
    bool         ivFromArchive = false;  ///< true when cryptanalysis gave the IV
    std::wstring howTr;
};

// ---------------------------------------------------------------------------
// The search
// ---------------------------------------------------------------------------

/// Harvest key candidates out of `scanFiles` (any binary - dll/exe/dump; names
/// are never interpreted) and prove one against `samples`. Returns false and
/// leaves `out` untouched when nothing is proven; it NEVER guesses.
bool AesFindCipher(const std::vector<AesSample>& samples,
                   const std::vector<std::wstring>& scanFiles,
                   AesAssetSpec& out,
                   AesFindReport& report,
                   const ProgressFn& progress,
                   const LogFn& log);

/// Same, over a memory buffer already in hand (a process dump, a module read
/// out of a running client).
/// `depth` 0 = whole printable tokens (a stored key is a complete NUL-delimited
/// string, so this finds it for thousands of candidates instead of millions);
/// 1 = every contiguous window; 2 = strided windows and the wider key lengths;
/// 3 = every raw 16-byte window. AesFindCipher walks ALL files at one depth
/// before spending the next depth on any of them, which is what keeps an
/// archive open interactive.
bool AesFindCipherInBuffer(const std::vector<AesSample>& samples,
                           const u8* buf, size_t size,
                           AesAssetSpec& out,
                           AesFindReport& report,
                           const ProgressFn& progress,
                           int depth = 3);

/// Diagnostic: report, for a key the caller already knows, exactly which stage
/// accepts or rejects it. Purely for --testkey; nothing in the product path
/// calls this.
std::wstring AesDiagnoseKey(const std::vector<AesSample>& samples,
                            const u8* key, int keyLen);

/// Collect encrypted assets from an open archive, grouped by extension. Picks
/// at most `perGroup` per extension and prefers .ddj (it is the one format that
/// states its own decrypted length, which is what pins IV[12..15]).
void AesCollectSamples(IArchive& archive, std::vector<AesSample>& out,
                       size_t perGroup = 8, size_t maxGroups = 10);

/// Binaries worth scanning next to an archive: every .dll/.exe/.bin in the
/// archive's folder and its parent, largest first. No name is special-cased -
/// the modules this was developed against have randomly generated names.
void AesScanCandidates(const std::wstring& archivePath,
                       std::vector<std::wstring>& out,
                       u64 maxBytesEach = 192ull * 1024 * 1024);

// ---------------------------------------------------------------------------
// Session cipher (process lifetime only - never persisted)
// ---------------------------------------------------------------------------
void         AesSetSpec(const AesAssetSpec& spec);
bool         AesActive();
void         AesClearSpec();                 ///< SecureZeroMemory over the key
/// Contentless by design: says a cipher is armed, never which one.
std::wstring AesStatusTr();
/// The real parameters. CLI --showkey only.
std::wstring AesStatusSensitive();

/// Try the armed cipher on `buf`. Structural accept test: the plaintext has to
/// come out with the JMXV signature its extension demands, so a plain or
/// foreign-cipher asset is left untouched and false is returned.
bool AesDecryptIfEncrypted(const std::wstring& innerPath, std::vector<u8>& buf);

/// Arm on archive open: collect samples, and if any are encrypted, scan the
/// sibling binaries. A no-op (returns false) when nothing is encrypted, which
/// is the normal case for an unprotected client.
bool AesTryInit(IArchive& archive, const LogFn& log);

} // namespace sro
