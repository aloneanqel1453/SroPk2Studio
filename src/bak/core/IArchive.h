// ============================================================================
//  SRO Asset Studio - core/IArchive.h
//
//  A tiny read/browse/extract interface shared by every archive backend, so the
//  UI (tree / preview / extract commands) is format-agnostic.  Two backends:
//    * Pk2Archive  - JMXPACK  (.pk2 / .lib), Blowfish, block chains
//    * VpArchive   - VSROPLUS (.vp),         AES-256-GCM directory
//
//  Both materialise the SAME Pk2Node tree, so panes need no per-format code.
//  Write / diagnostic operations stay on the concrete Pk2Archive; a VpArchive
//  is always read-only (canWrite() == false).
// ============================================================================
#pragma once

#include "../Common.h"
#include "Pk2Types.h"

#include <functional>
#include <vector>

namespace sro {

// ---------------------------------------------------------------------------
// Pk2Node - one node of a lazily (Pk2) or eagerly (Vp) materialised tree.
// Owned by the archive that produced it. Never delete one yourself.
// ---------------------------------------------------------------------------
struct Pk2Node {
    Pk2Entry     entry;            ///< the on-disk entry (synthetic for the root)
    std::wstring name;             ///< display name ("" for the root)
    std::wstring fullPath;         ///< "data\\res\\char\\x.bsr"; "" for the root
    Pk2Node*     parent = nullptr;

    /// False until the block chain / children for this folder have been read.
    bool loaded = false;

    /// Children, split so the tree can show folders first without re-sorting.
    std::vector<Pk2Node*> folders;
    std::vector<Pk2Node*> files;

    bool isFolder() const { return entry.isFolder() || parent == nullptr; }
    bool isFile()   const { return entry.isFile(); }
    bool isRoot()   const { return parent == nullptr; }

    size_t childCount() const { return folders.size() + files.size(); }
};

// ---------------------------------------------------------------------------
/// Folder = a plain extracted client directory browsed through this same
/// interface, so a client whose .pk2 will not open in full is still usable.
enum class ArchiveKind { Pk2, Vp, Folder };

// ---------------------------------------------------------------------------
// IArchive - the read-only surface the UI depends on.
// ---------------------------------------------------------------------------
class IArchive {
public:
    virtual ~IArchive() = default;

    virtual ArchiveKind kind()     const = 0;   ///< which backend
    virtual bool        canWrite() const = 0;   ///< false for Vp

    // info ------------------------------------------------------------------
    virtual bool                isOpen()   const = 0;
    virtual bool                readOnly() const = 0;
    virtual const std::wstring& path()     const = 0;
    virtual u64                 fileSize() const = 0;
    virtual Pk2HeaderInfo       headerInfo() const = 0;

    // navigation ------------------------------------------------------------
    virtual Pk2Node* root() const = 0;
    virtual Result   loadChildren(Pk2Node* node) = 0;

    /// Entries the loader had to skip because their payload lies outside the
    /// file - i.e. the archive is truncated or damaged and that part of the
    /// tree is unreachable. Zero for a healthy archive. Exposed so a short
    /// listing is never mistaken for a complete one.
    virtual u64      skippedEntryCount() const { return 0; }
    virtual std::vector<std::wstring> skippedEntrySamples() const { return {}; }
    /// Copy a folder's children UNDER THE ARCHIVE'S LOCK.
    ///
    /// The UI used to copy `node->folders` / `node->files` directly while an
    /// import or delete running on the worker thread was push_back-ing into
    /// those very vectors. A reallocation there frees the buffer the UI thread
    /// is mid-copy from - a torn read or an access violation. Every consumer
    /// that walks children from the UI thread must come through here.
    virtual void childrenOf(Pk2Node* node,
                            std::vector<Pk2Node*>& folders,
                            std::vector<Pk2Node*>& files) = 0;

    virtual Result   findNode(const std::wstring& innerPath, Pk2Node** out) = 0;
    virtual Result   findFile(const std::wstring& innerPath, Pk2Node** out) = 0;

    // reading ---------------------------------------------------------------
    virtual Result readFile(const Pk2Node* node, std::vector<u8>& out) const = 0;
    virtual Result readFile(const std::wstring& innerPath, std::vector<u8>& out) = 0;
    virtual Result readFilePrefix(const Pk2Node* node, size_t maxBytes, std::vector<u8>& out) const = 0;
    /// By-path prefix read. Resolves the node and reads it under ONE lock, so a
    /// concurrent close()/mutation cannot free the node in the gap. Preview and
    /// any other worker-thread reader MUST use this instead of findFile()+
    /// readFilePrefix(node), which spans two lock acquisitions.
    virtual Result readFilePrefix(const std::wstring& innerPath, size_t maxBytes, std::vector<u8>& out) = 0;
    virtual Result extractFile(const Pk2Node* node, const std::wstring& outFilePath) const = 0;
    virtual Result extractTree(Pk2Node* node, const std::wstring& outDir, const ProgressFn& progress) = 0;
    virtual Result extractAll(const std::wstring& outDir, const ProgressFn& progress) = 0;

    // searching / counting --------------------------------------------------
    virtual Result walkAll(const std::function<bool(Pk2Node*)>& visit, const ProgressFn& progress) = 0;
    virtual Result search(const std::wstring& needle, std::vector<Pk2Node*>& results,
                          size_t maxResults, const ProgressFn& progress) = 0;
    virtual Result countEntries(bool deep, u64& folders, u64& files, const ProgressFn& progress) = 0;

    // writing ---------------------------------------------------------------
    // Every backend implements these, so the UI drives PK2/LIB and VP through
    // exactly the same commands. They fail with Status::AccessDenied when the
    // archive was opened read-only.
    virtual Result addFile(Pk2Node* parentFolder, const std::wstring& name,
                           const u8* data, size_t size, bool overwrite) = 0;
    virtual Result importFile(Pk2Node* parentFolder, const std::wstring& diskPath,
                              const std::wstring& nameOverride, bool overwrite,
                              const ProgressFn& progress) = 0;
    virtual Result importFolder(Pk2Node* parentFolder, const std::wstring& diskDir,
                                const ProgressFn& progress) = 0;
    virtual Result addFolder(Pk2Node* parentFolder, const std::wstring& name, Pk2Node** out) = 0;
    virtual Result remove(Pk2Node* node) = 0;
    virtual Result rename(Pk2Node* node, const std::wstring& newName) = 0;
    virtual Result compact(u64* savedBytes, const ProgressFn& progress) = 0;
    virtual Result flush() = 0;

    virtual void close() = 0;

    /// Full inner path of a node ("" for the root). Works for any backend.
    static std::wstring NodePath(const Pk2Node* node);
};

} // namespace sro
