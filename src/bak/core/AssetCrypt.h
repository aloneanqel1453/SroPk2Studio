// ============================================================================
//  SRO Asset Studio - core/AssetCrypt.h
//
//  Some shards encrypt INDIVIDUAL assets inside an otherwise normal archive
//  (premium COS / costume meshes). Measured on "Sro Kim Hoa 160" (Data.pk3):
//
//    * The payload is a byte-wise XOR against a GLOBAL, POSITION-KEYED
//      keystream - not a block cipher. Proof: file sizes are not a multiple of
//      16, two ciphertexts XORed together are 27% zero bytes and share a
//      16-byte all-zero prefix, and ks[0..11] derived from the ".bms" magic is
//      byte-identical across all 178 encrypted files (and across .bsr/.ban too).
//    * The keystream has no period (tested 4..1024), so it comes from a PRNG:
//      it cannot be extended by guessing, only observed.
//
//  Consequence: ONE decrypted sample of length L hands us the exact keystream
//  for its first L bytes, which then decrypts EVERY other encrypted asset up to
//  that length. Samples come from the running client (KeyFinder's RAM asset
//  dump) - and each new sample extends the coverage.
//
//  RULE 1 still holds: the keystream lives in memory for the session only and
//  is never written to disk.
// ============================================================================
#pragma once

#include "../Common.h"
#include "../formats/FormatId.h"

#include <mutex>
#include <vector>

namespace sro {

/// True when `data` is an asset whose format demands a "JMXV" signature but
/// does not have one - i.e. the server encrypted it. Content-based on purpose:
/// a shard's own "encrypted file list" is not trustworthy or complete.
bool LooksEncryptedAsset(const u8* data, size_t size, FormatId format);

/// Decrypt in place when this buffer is (or merely looks) encrypted and the
/// keystream covers it. Works for EVERY extension, not just the ones with a
/// JMXV magic: a candidate plaintext is accepted when it starts with a known
/// signature, or is convincingly text, or is simply far more structured than
/// the raw bytes. Returns true when the buffer was replaced.
bool DecryptIfEncrypted(const std::wstring& fileName, std::vector<u8>& buf);

/// True when the buffer still looks like ciphertext (used by the preview to say
/// so out loud instead of showing an empty view).
bool StillLooksEncrypted(const std::wstring& fileName, const u8* data, size_t size);

// ---------------------------------------------------------------------------
// The shard's asset cipher, recovered from its own DevKit_DLL:
//
//     ks[pos] = (TableA[pos % 211] - TableB[pos % 493]) & 0xFF
//     data[i] ^= ks[start + i]
//
// The two tables are addressed by absolute immediates in the decrypt loop
// (`movzx eax, [edx + &A]` / `sub al, [ebx + &B]`), so they can be located by
// pattern rather than hardcoded, and `start` is then confirmed against a real
// ciphertext whose plaintext magic we know. Nothing is trusted unverified.
// ---------------------------------------------------------------------------
struct KeystreamTables {
    std::vector<u8> a, b;
    u32             start = 0;
    bool valid() const { return !a.empty() && !b.empty(); }
};

/// Locate the tables inside a DevKit-style module image and solve for `start`
/// so that the generated keystream reproduces `magic` on `cipherSample`.
bool DiscoverKeystreamTables(const u8* image, size_t imageSize, u64 imageBase,
                             const u8* cipherSample, size_t sampleLen,
                             const u8* magic, size_t magicLen,
                             KeystreamTables& out);

/// Same discovery, run over EVERY module of a live client. This is the path the
/// app actually uses: the shard's crypto DLL is dropped, loaded and deleted, so
/// it exists only in memory - there is nothing to ship or hardcode. `whereTr`
/// receives the module the cipher was found in.
class IArchive;

/// Recover the cipher from the ARCHIVE ALONE - no client, no DevKit module.
///
/// Knowing the construction collapses the problem: the keystream is not 104 023
/// free bytes, it is two small tables, ks[i] = A[i % m] - B[i % n] (the start
/// offset folds into the tables). That is ~704 unknowns, and every encrypted
/// asset contributes thousands of observations per unknown, so each table byte
/// is chosen by pooled maximum likelihood (SRO mesh data is dominated by 0x00).
/// The 12-byte JMXV magic every asset must decrypt to is the crib, and a full
/// structural parse is the accept test - a wrong table set cannot pass.
bool SolveAssetCipherFromArchive(IArchive& archive, std::wstring* howTr);

/// Same discovery, but over module images ALREADY ON DISK - a "SaveDlls" /
/// "Dump" folder the user captured, or the output of --dumpmodules /
/// --watchmodules. This is what removes the running-client requirement without
/// remembering anything: each game is solved from its OWN dumps, so two shards
/// with the same file names can never be mixed up.
bool DiscoverCipherFromDumps(const std::wstring& searchDir,
                             const u8* cipherSample, size_t sampleLen,
                             const u8* magic, size_t magicLen,
                             KeystreamTables& out, std::wstring* whereTr);


/// One call that makes an archive fully readable: find a still-encrypted asset,
/// lift the shard's cipher out of whatever client is running for this game, and
/// seed the vault so preview/export decrypt from then on. Safe to call on every
/// open - it returns immediately when there is nothing encrypted or the cipher
/// is already known. `whereTr` gets the module it came from.
bool RecoverAssetCipher(IArchive& archive, const std::wstring& archiveDir,
                        std::wstring* whereTr, bool* hadEncrypted);

bool DiscoverCipherFromProcess(u32 pid,
                               const u8* cipherSample, size_t sampleLen,
                               const u8* magic, size_t magicLen,
                               KeystreamTables& out, std::wstring* whereTr);

// ---------------------------------------------------------------------------
// KeystreamVault - session-only store of the recovered XOR keystream.
// ---------------------------------------------------------------------------
class KeystreamVault {
public:
    static KeystreamVault& Instance();

    /// Learn from a matched pair (same asset, encrypted in the archive and
    /// decrypted from the client's memory). Returns how many NEW bytes of
    /// keystream this sample contributed.
    size_t learn(const u8* cipher, size_t cipherLen,
                 const u8* plain,  size_t plainLen);

    /// Decrypt in place when the keystream covers the whole buffer.
    /// Returns false (and leaves `data` untouched) when coverage is short.
    bool decrypt(std::vector<u8>& data) const;

    /// How much of `len` bytes we can currently decrypt.
    size_t coveredPrefix(size_t len) const;

    size_t coverage() const;      ///< contiguous bytes known from offset 0
    size_t knownBytes() const;    ///< total known bytes (may be sparse)
    void   clear();

    /// Fill `bytes` of keystream generated by the shard's own table pair:
    ///     ks[i] = (A[(start+i) % A.size()] - B[(start+i) % B.size()]) & 0xFF
    /// This is the WHOLE cipher, so once the tables are known every encrypted
    /// asset decrypts offline - no client, no samples.
    void seedFromTables(const std::vector<u8>& tableA, const std::vector<u8>& tableB,
                        u32 start, size_t bytes);

private:
    KeystreamVault() = default;
    void extendCoverageLocked();      ///< caller holds m_mtx

    mutable std::mutex   m_mtx;
    std::vector<u8>      m_ks;
    std::vector<bool>    m_known;
    size_t               m_coverage = 0;   ///< cached contiguous run from 0
};

} // namespace sro
