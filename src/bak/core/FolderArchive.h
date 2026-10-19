// ============================================================================
//  SRO Asset Studio - core/FolderArchive.h
//
//  A PLAIN DIRECTORY exposed through IArchive, so an extracted client tree
//  (Data\ , Media\ , Particles\ , Music\ , Map\ ...) browses, previews and
//  exports exactly like a .pk2.
//
//  Why this exists: a repacked or server-modified .pk2 does not always open in
//  full even with the correct key - the block chain can be truncated, a folder
//  can point outside the file, or a shard can use a container this build cannot
//  read. People then extract the client with whatever tool DOES work and are
//  left with loose folders that no viewer will open. Pointing the program at
//  the folder gives them the whole preview pipeline back.
//
//  It also makes SIBLING lookups trivial: pick "Data", and Media / Particles /
//  Music sitting next to it are found through the same code path the .pk2 build
//  uses (SiblingArchives), because a folder answers readFile() the same way.
//
//  The tree is built LAZILY, one directory per loadChildren(), so pointing at a
//  40 GB extracted client is as instant as opening a .pk2.
//
//  Writing is supported and maps to ordinary file operations; compact() is a
//  no-op because a directory has no free-list to reclaim.
// ============================================================================
#pragma once

#include "IArchive.h"

#include <memory>
#include <string>
#include <vector>

namespace sro {

class FolderArchive : public IArchive {
public:
    FolderArchive();
    ~FolderArchive() override;

    FolderArchive(const FolderArchive&) = delete;
    FolderArchive& operator=(const FolderArchive&) = delete;

    /// True when `path` is a directory that looks like a Silkroad asset root -
    /// i.e. it directly contains at least one of the well-known top folders
    /// (data, media, particles, music, map, prim, res...) or IS one of them.
    static bool LooksLikeAssetFolder(const std::wstring& path);

    /// Open a directory as an archive. `readOnly` mirrors the .pk2 flag.
    Result open(const std::wstring& dir, bool readOnly);

    // --- IArchive ----------------------------------------------------------
    ArchiveKind kind()     const override;
    bool        canWrite() const override;
    bool        isOpen()   const override;
    bool        readOnly() const override;
    const std::wstring& path() const override;
    u64           fileSize() const override;
    Pk2HeaderInfo headerInfo() const override;

    Pk2Node* root() const override;
    Result   loadChildren(Pk2Node* node) override;
    void     childrenOf(Pk2Node* node, std::vector<Pk2Node*>& folders,
                        std::vector<Pk2Node*>& files) override;
    Result   findNode(const std::wstring& innerPath, Pk2Node** out) override;
    Result   findFile(const std::wstring& innerPath, Pk2Node** out) override;

    Result readFile(const Pk2Node* node, std::vector<u8>& out) const override;
    Result readFile(const std::wstring& innerPath, std::vector<u8>& out) override;
    Result readFilePrefix(const Pk2Node* node, size_t maxBytes, std::vector<u8>& out) const override;
    Result readFilePrefix(const std::wstring& innerPath, size_t maxBytes, std::vector<u8>& out) override;
    Result extractFile(const Pk2Node* node, const std::wstring& outFilePath) const override;
    Result extractTree(Pk2Node* node, const std::wstring& outDir, const ProgressFn& progress) override;
    Result extractAll(const std::wstring& outDir, const ProgressFn& progress) override;

    Result walkAll(const std::function<bool(Pk2Node*)>& visit, const ProgressFn& progress) override;
    Result search(const std::wstring& needle, std::vector<Pk2Node*>& results,
                  size_t maxResults, const ProgressFn& progress) override;
    Result countEntries(bool deep, u64& folders, u64& files, const ProgressFn& progress) override;

    Result addFile(Pk2Node* parentFolder, const std::wstring& name,
                   const u8* data, size_t size, bool overwrite) override;
    Result importFile(Pk2Node* parentFolder, const std::wstring& diskPath,
                      const std::wstring& nameOverride, bool overwrite,
                      const ProgressFn& progress) override;
    Result importFolder(Pk2Node* parentFolder, const std::wstring& diskDir,
                        const ProgressFn& progress) override;
    Result addFolder(Pk2Node* parentFolder, const std::wstring& name, Pk2Node** out) override;
    Result remove(Pk2Node* node) override;
    Result rename(Pk2Node* node, const std::wstring& newName) override;
    Result compact(u64* savedBytes, const ProgressFn& progress) override;
    Result flush() override;
    void   close() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sro
