// ============================================================================
//  SRO Asset Studio - core/Pk2Archive.h
//
//  Read + write access to a JMXPACK archive (.pk2 / .lib / any extension).
//
//  DESIGN CONTRACT - please honour all of it:
//
//  1. LAZY. open() reads exactly the 256-byte header and the single root block.
//     A folder's block chain is read the first time somebody asks for its
//     children. A 6.26 GB archive must open in under 2 seconds.
//
//  2. 64-BIT everywhere. Offsets are i64. Never truncate to DWORD.
//
//  3. NEVER TRUNCATE A USER FILE. Payload writes go to unused space or to the
//     end of the file; compact() writes a fresh archive and keeps a .bak.
//
//  4. THREADING. All methods are internally serialised by a mutex, so a worker
//     thread may call read/extract/compact while the UI thread only touches
//     already-returned node pointers. Node pointers stay valid until close()
//     or a structural mutation (add/remove/compact) - after a mutation the UI
//     must re-query from root().
//
//  5. CANCELLABLE. Every long operation takes a ProgressFn; returning false
//     from it aborts with Status::Cancelled and leaves the archive consistent.
//
//  Implementation owner: Agent B (core).
// ============================================================================
#pragma once

#include "../Common.h"
#include "IArchive.h"      // Pk2Node + IArchive live here now
#include "Blowfish.h"
#include "Pk2Types.h"

#include <mutex>

namespace sro {

// ---------------------------------------------------------------------------
// Pk2Archive  (Pk2Node is defined in IArchive.h)
// ---------------------------------------------------------------------------
class Pk2Archive : public IArchive {
public:
    // --- IArchive identity -------------------------------------------------
    ArchiveKind kind()     const override { return ArchiveKind::Pk2; }
    bool        canWrite() const override { return !readOnly(); }

    Pk2Archive();
    ~Pk2Archive();

    Pk2Archive(const Pk2Archive&) = delete;
    Pk2Archive& operator=(const Pk2Archive&) = delete;

    // =======================================================================
    // Lifetime
    // =======================================================================

    /// Open an existing archive.
    /// `cipher` must already be initialised (KeyRing/KeyFinder produce it).
    /// The root block is validated with the root-block oracle; a mismatch
    /// returns Status::WrongKey and leaves the object closed.
    /// `readOnly = false` opens with write access; the file stays locked until
    /// close(), which is why the UI has an explicit "Kapat" button.
    Result open(const std::wstring& path, const Blowfish& cipher, bool readOnly);

    /// Open with the header only, no key required. Useful for the key search
    /// and for the diagnostics dialog. The tree is NOT available afterwards.
    static Result ProbeHeader(const std::wstring& path, Pk2RawHeader& out);

    /// Decode a raw header into display form.
    static Pk2HeaderInfo DescribeHeader(const Pk2RawHeader& raw);

    /// Read the 2560 encrypted bytes of the root block without a key.
    /// KeyRing/KeyFinder feed this to the root-block oracle.
    /// The offset is auto-detected: a SailSafe (Sailor) archive reads from
    /// kSailorRootBlockOffset (0x33E), a stock archive from 256. This keeps every
    /// key stage working on both variants without any caller change.
    static Result ReadRawRootBlock(const std::wstring& path, u8 out[kPk2BlockSize]);

    /// The file offset where this archive's root directory block lives:
    /// kSailorRootBlockOffset for a SailSafe archive, kPk2RootBlockOffset (256)
    /// otherwise. Detected by sniffing the first bytes; no key needed.
    static u64 DetectRootBlockOffset(const std::wstring& path);

    /// True when `path` begins with the fixed SailSafe (Sailor) signature block.
    static bool IsSailorArchive(const std::wstring& path);

    /// Create a brand-new, empty archive at `path` and leave it OPEN for writing.
    /// Fails with Status::AlreadyExists unless `overwrite`.
    Result createNew(const std::wstring& path, const Pk2CreateOptions& opt, bool overwrite);

    /// Release the file handle and free the tree. Safe to call when closed.
    void close() override;

    bool                isOpen()   const override;
    bool                readOnly() const override;
    const std::wstring& path()     const override;
    u64                 fileSize() const override;
    const Pk2RawHeader& rawHeader() const;
    Pk2HeaderInfo       headerInfo() const override;

    /// The cipher this archive was opened with (so the UI can persist the key).
    const Blowfish& cipher() const;

    // =======================================================================
    // Navigation (LAZY)
    // =======================================================================

    /// The synthetic root folder. Never null while isOpen().
    Pk2Node* root() const override;

    u64 skippedEntryCount() const override;
    std::vector<std::wstring> skippedEntrySamples() const override;

    /// Ensure `node`'s children are read from disk. Cheap when already loaded.
    /// Only meaningful on folders; a no-op on files.
    Result loadChildren(Pk2Node* node) override;

    /// Walk an inner path such as L"data\\res\\char\\x.bsr" (case-insensitive,
    /// '/' and '\\' both accepted), loading folders on the way.
    /// Returns Status::NotFound when a component is missing.
    void   childrenOf(Pk2Node* node, std::vector<Pk2Node*>& folders,
                      std::vector<Pk2Node*>& files) override;
    Result findNode(const std::wstring& innerPath, Pk2Node** out) override;

    /// Convenience wrapper that also requires the result to be a file.
    Result findFile(const std::wstring& innerPath, Pk2Node** out) override;

    /// Full inner path of a node ("" for the root).
    static std::wstring NodePath(const Pk2Node* node);

    // =======================================================================
    // Reading
    // =======================================================================

    /// Read a file's payload into memory. Payloads are NOT encrypted.
    Result readFile(const Pk2Node* node, std::vector<u8>& out) const override;
    Result readFile(const std::wstring& innerPath, std::vector<u8>& out) override;

    /// Read at most `maxBytes` from the start of a file (preview / sniffing).
    Result readFilePrefix(const Pk2Node* node, size_t maxBytes, std::vector<u8>& out) const override;
    Result readFilePrefix(const std::wstring& innerPath, size_t maxBytes, std::vector<u8>& out) override;

    /// Extract one file to disk, creating parent directories as needed.
    Result extractFile(const Pk2Node* node, const std::wstring& outFilePath) const override;

    /// Recursively extract `node` (file or folder) beneath `outDir`.
    /// Folder structure is recreated. Reports progress in bytes.
    Result extractTree(Pk2Node* node, const std::wstring& outDir, const ProgressFn& progress) override;

    /// Extract the whole archive.
    Result extractAll(const std::wstring& outDir, const ProgressFn& progress) override;

    // =======================================================================
    // Searching / counting
    // =======================================================================

    /// Walk the entire tree, loading every folder. EXPENSIVE on a 6 GB archive
    /// - always run it on a worker thread with a progress callback.
    Result walkAll(const std::function<bool(Pk2Node*)>& visit, const ProgressFn& progress) override;

    /// Case-insensitive substring match on file/folder names.
    /// `results` is filled with matching nodes (folders included).
    Result search(const std::wstring& needle, std::vector<Pk2Node*>& results,
                  size_t maxResults, const ProgressFn& progress) override;

    /// Counts for the toolbar readout. Only counts what has been loaded so far
    /// unless `deep` is true (which forces a full walk).
    Result countEntries(bool deep, u64& folders, u64& files, const ProgressFn& progress) override;

    // =======================================================================
    // Writing  (all fail with Status::AccessDenied when readOnly)
    // =======================================================================

    /// Add or overwrite a file inside `parentFolder`.
    /// Strategy: if an existing entry of the same name has enough allocated
    /// room, write in place; otherwise append the payload at the end of file
    /// and repoint the entry. Never truncates.
    Result addFile(Pk2Node* parentFolder, const std::wstring& name,
                   const u8* data, size_t size, bool overwrite) override;

    /// Import a file from disk into `parentFolder` (name defaults to its basename).
    Result importFile(Pk2Node* parentFolder, const std::wstring& diskPath,
                      const std::wstring& nameOverride, bool overwrite,
                      const ProgressFn& progress) override;

    /// Recursively import a directory tree from disk.
    Result importFolder(Pk2Node* parentFolder, const std::wstring& diskDir,
                        const ProgressFn& progress);

    /// Create an empty sub-folder (with its "." / ".." entries).
    Result addFolder(Pk2Node* parentFolder, const std::wstring& name, Pk2Node** out) override;

    /// Mark an entry Empty. Payload bytes stay behind until compact().
    /// Folders are removed recursively.
    Result remove(Pk2Node* node) override;

    /// Rename in place (name must fit in kPk2MaxNameBytes-1 ANSI bytes).
    Result rename(Pk2Node* node, const std::wstring& newName) override;

    /// Rewrite the archive without dead space.
    /// Writes to `path() + ".new"`, then moves the original to `path() + ".bak"`
    /// and the new file into place; the archive is reopened afterwards.
    /// `savedBytes` receives oldSize - newSize.
    Result compact(u64* savedBytes, const ProgressFn& progress) override;

    /// Flush pending header/block writes to disk.
    Result flush() override;

    // =======================================================================
    // Diagnostics
    // =======================================================================

    /// Full walk producing block/entry statistics and fragmentation.
    Result diagnose(Pk2Diagnostics& out, const ProgressFn& progress);

    // =======================================================================
    // Low-level block access (exposed for the key search and for tests)
    // =======================================================================

    /// Read + decrypt the 2560-byte block at `blockOffset`.
    Result readBlock(i64 blockOffset, Pk2RawBlock& out) const;

    /// Encrypt + write a 2560-byte block back at `blockOffset`.
    Result writeBlock(i64 blockOffset, const Pk2RawBlock& in);

    /// Decode one raw entry into the friendly form, filling in its physical
    /// location. Returns false when the entry is unusable.
    static bool DecodeEntry(const Pk2RawEntry& raw, i64 blockOffset, int slot, Pk2Entry& out);

    /// Encode a friendly entry back into raw bytes.
    static bool EncodeEntry(const Pk2Entry& in, Pk2RawEntry& out);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sro
