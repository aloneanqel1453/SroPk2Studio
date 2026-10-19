// ============================================================================
//  SRO Asset Studio - core/KeyRing.h
//
//  Owns key CANDIDATES and the two VALIDATION ORACLES from SPEC.md §1.
//
//  Oracle 1 (checksum, weak):
//      Encode("Joymax Pack File")[0..2] == header.Checksum[0..2]
//      Cheap, but some servers changed the probe string.
//
//  Oracle 2 (root block, strong, PREFERRED):
//      Decrypt the 2560 bytes at offset 256; entry[0].Type == 1 (Folder)
//      and entry[0].Name == ".".
//      This is the only oracle that works when the key derivation is unknown,
//      which is exactly the raw-Blowfish-state case.
//
//  RULE: always confirm with oracle 2; fall back to oracle 1 only when
//  oracle 2 is inconclusive (e.g. an archive with no root block yet).
//
//  ==========================================================================
//   SPEC RULE 0 - NOTHING SERVER-SPECIFIC IS HARDCODED
//  ==========================================================================
//  There is deliberately NO table of per-server keys in this program, and no
//  special case for Origin, Golden or any other shard. The only constants
//  allowed are format-level ones: the default ascii key "169841" and the three
//  published base keys (STD / PARADISE / ZERO), which are properties of the
//  Silkroad format itself.
//
//  "Key Bul" must work on a shard nobody has ever seen. If it only works
//  because a value was baked in, the feature is broken. Any concrete key in
//  SPEC.md is a TEST EXPECTATION, never a shortcut to embed.
//  Do not add a KnownServerKeys() list here. It was removed on purpose.
//
//  Stage 1 of the key search lives here (instant, no side effects):
//    1. the key typed in the UI (default "169841") x {STD, PARADISE, ZERO}
//    2. "raw key" interpretation of the typed key (ZERO base) - what the long
//       40-60 character keys use
//  A candidate carried in from Stage 0 (session memory / sibling archive) is
//  tried before both, but it is discovered data, not a constant.
//
//  Stage 0 (session + sibling + already-running client) and stages 2 and 3
//  (binary scan / process memory) live in KeyFinder.h.
//
//  Implementation owner: Agent B (core).
// ============================================================================
#pragma once

#include "../Common.h"
#include "Blowfish.h"
#include "Pk2Types.h"

namespace sro {

// ---------------------------------------------------------------------------
// KeyCandidate - one thing worth trying, plus where it came from.
// ---------------------------------------------------------------------------
struct KeyCandidate {
    /// Turkish label for the log, e.g. L"Yazilan key x STD" or
    /// L"GFXFileManager.dll icinde bulunan base key".
    std::wstring label;

    // --- derivation route A: ascii key + base key --------------------------
    std::string asciiKey;                  ///< empty when rawState is used
    BaseKeyId   baseId = BaseKeyId::Std;
    std::vector<u8> customBase;            ///< used when baseId == Custom

    // --- derivation route B: raw scheduled cipher state --------------------
    /// When true the ascii/base fields are ignored and `rawState` (4168 bytes)
    /// is adopted verbatim. This is what Scanner B recovers.
    bool             useRawState = false;
    std::vector<u8>  rawState;

    // --- derivation route C: raw KEY bytes (standard key schedule) ----------
    /// When non-empty (and useRawState is false), these bytes are the Blowfish
    /// key fed through the STANDARD key schedule (initializeRaw), ignoring
    /// ascii/base. Needed for keys that are not printable ASCII - e.g. the
    /// SailSafe/Sailor archive whose key is literal zero bytes.
    std::vector<u8>  rawKeyBytes;

    /// Build the cipher this candidate describes. Returns false when the
    /// candidate is malformed (empty key, wrong-size raw state, ...).
    bool makeCipher(Blowfish& out) const;

    /// Short one-line description for the log / config file.
    /// Safe for the log, the window header and a screenshot: says that a key
    /// was proven, never which one nor how it was found. See KeyRing.cpp.
    std::wstring describe() const;

    /// The real key material and the path that found it. ONLY for an explicit
    /// user request (CLI --showkey). Never call this from the UI or the log.
    std::wstring describeSensitive() const;
};

// ---------------------------------------------------------------------------
// KeyRing
// ---------------------------------------------------------------------------
class KeyRing {
public:
    // =======================================================================
    // Oracles
    // =======================================================================

    /// Oracle 1. Encrypts Pk2ChecksumProbe() and compares the first
    /// kPk2ChecksumCompareBytes against header.Checksum.
    static bool ChecksumOracle(const Blowfish& bf, const Pk2RawHeader& header);

    /// Oracle 2. `encryptedRootBlock` is the raw 2560 bytes at file offset 256.
    /// Decrypts a copy and checks entry[0] is a folder named ".".
    static bool RootBlockOracle(const Blowfish& bf, const u8* encryptedRootBlock /*[2560]*/);

    /// THE HOT-PATH ORACLE. Decrypts only the FIRST 8 BYTES of the root block
    /// and requires them to start 01 2E ('.' folder entry) before any further
    /// work. The scanners run this millions of times, so it must stay at one
    /// Blowfish block. Returns false fast on a wrong key.
    static bool RootBlockOracleFast(const Blowfish& bf, const u8* encryptedRootBlock /*[8+]*/);

    /// Combined check. Runs oracle 2 first; if the archive has no usable root
    /// block it falls back to oracle 1. `usedRootOracle` (optional) reports
    /// which one decided.
    static bool Validate(const Blowfish& bf,
                         const Pk2RawHeader& header,
                         const u8* encryptedRootBlock /*[2560] or nullptr*/,
                         bool* usedRootOracle = nullptr);

    /// Convenience: validate directly against a file on disk.
    /// EVERY key must pass this against the TARGET archive before use, no
    /// matter where it came from - typed, session memory, sibling archive or
    /// memory scan. That is what makes it safe to keep no cache at all.
    static bool ValidateAgainstFile(const Blowfish& bf, const std::wstring& archivePath);

    // =======================================================================
    // Format-level constants (the ONLY key constants allowed - see RULE 0)
    // =======================================================================

    /// The stock Silkroad ascii key, and the default contents of the UI field.
    /// This is a format default, not a server key.
    static const char* DefaultAsciiKey() { return "169841"; }

    /// The three published base keys with their labels.
    static const std::vector<std::pair<BaseKeyId, const wchar_t*>>& KnownBaseKeys();

    // =======================================================================
    // Stage 1 - instant, no side effects
    // =======================================================================

    /// Build the ordered stage-1 candidate list:
    /// typedKey x {STD, PARADISE, ZERO}, plus the raw-key reading.
    ///   `rawKeyMode` puts the ZERO base first
    ///   `carried`    an optional candidate from Stage 0 (session memory or a
    ///                sibling archive). Tried first when present. It is
    ///                discovered data, never a built-in constant.
    static void BuildStage1Candidates(const std::string& typedKey,
                                      bool rawKeyMode,
                                      const KeyCandidate* carried,
                                      std::vector<KeyCandidate>& out);

    /// Try every stage-1 candidate against the archive at `archivePath`.
    /// On success `found` holds the winning candidate and `cipher` is ready.
    /// Returns Status::WrongKey when nothing matched (this is NOT an error the
    /// user should see as a crash - the UI then offers "Key Bul").
    static Result TryStage1(const std::wstring& archivePath,
                            const std::string& typedKey,
                            bool rawKeyMode,
                            const KeyCandidate* carried,
                            KeyCandidate& found,
                            Blowfish& cipher,
                            const LogFn& log);

    /// Try an explicit list of candidates (Stage 0 uses this for the session
    /// key and every sibling-archive key). Stops at the first that validates.
    static Result TryCandidates(const std::wstring& archivePath,
                                const std::vector<KeyCandidate>& candidates,
                                KeyCandidate& found,
                                Blowfish& cipher,
                                const LogFn& log);

    /// Test a single candidate. Cheap enough to call in a tight loop; used by
    /// KeyFinder's scanners too.
    static bool TestCandidate(const KeyCandidate& cand,
                              const Pk2RawHeader& header,
                              const u8* encryptedRootBlock /*[2560]*/,
                              Blowfish& cipherOut);
};

} // namespace sro
