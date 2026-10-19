// ============================================================================
//  SRO Asset Studio - core/FolderArchive.cpp
//
//  See FolderArchive.h. The tree mirrors the directory lazily: a Pk2Node is
//  created per entry, its `fullPath` is the inner path with '\' separators
//  (exactly what the .pk2 backend produces, so every consumer - preview,
//  AssetDeps, SiblingArchives, export - works unchanged), and the on-disk path
//  is that inner path appended to the root directory.
// ============================================================================
#include "FolderArchive.h"
#include "AssetCrypt.h"

#include <algorithm>
#include <mutex>

namespace sro {

namespace {

/// The folders a Silkroad asset root is made of. Used only to recognise a
/// likely client directory; opening any other folder still works.
const wchar_t* kWellKnown[] = {
    L"data", L"media", L"particles", L"music", L"map", L"sound",
    L"prim", L"res", L"navmesh", L"interface", L"server_dep", L"item", L"char"
};

std::wstring LowerW(const std::wstring& s) {
    std::wstring o = s;
    for (wchar_t& c : o) c = static_cast<wchar_t>(std::towlower(c));
    return o;
}

/// A single path component that is safe to append to a disk path. Without this
/// "Yeni Klasör" typed as "..\..\pwned" resolved through EnsureDirectory and
/// silently created a directory OUTSIDE the archive root, reporting success.
bool IsUsableName(const std::wstring& n) {
    if (n.empty() || n == L"." || n == L"..") return false;
    if (n.find(L'\\') != std::wstring::npos || n.find(L'/') != std::wstring::npos) return false;
    if (n.find_first_of(L"<>:\"|?*") != std::wstring::npos) return false;
    if (n.back() == L'.' || n.back() == L' ') return false;   // Win32 trims these away
    return true;
}

/// Absolute, normalised, no trailing separator.
std::wstring FullPathW(const std::wstring& p) {
    std::vector<wchar_t> buf(32768);
    const DWORD n = ::GetFullPathNameW(p.c_str(), static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    std::wstring o = (n && n < buf.size()) ? std::wstring(buf.data(), n) : p;
    while (o.size() > 3 && (o.back() == L'\\' || o.back() == L'/')) o.pop_back();
    return o;
}

/// True when `child` is `parent` or lies inside it. The separator check matters:
/// a plain prefix compare would also claim "C:\ClientBackup" is inside "C:\Client".
bool PathIsWithin(const std::wstring& parent, const std::wstring& child) {
    const std::wstring p = LowerW(FullPathW(parent));
    const std::wstring c = LowerW(FullPathW(child));
    if (c.size() < p.size() || c.compare(0, p.size(), p) != 0) return false;
    if (c.size() == p.size()) return true;
    if (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) return true;   // "C:\"
    return c[p.size()] == L'\\';
}

/// Inner path ("res\\char\\x.bsr") -> disk path under `rootDir`.
std::wstring DiskPathOf(const std::wstring& rootDir, const std::wstring& inner) {
    if (inner.empty()) return rootDir;
    std::wstring p = inner;
    for (wchar_t& c : p) if (c == L'/') c = L'\\';
    return rootDir + L"\\" + p;
}

} // namespace

// ---------------------------------------------------------------------------

struct FolderArchive::Impl {
    mutable std::recursive_mutex mtx;

    std::wstring dir;
    bool         open  = false;
    bool         ro    = false;

    // Every node ever created, owned here. Pointers handed to the UI must stay
    // valid for the archive's lifetime (same contract as the .pk2 backend).
    std::vector<std::unique_ptr<Pk2Node>> pool;
    Pk2Node* rootNode = nullptr;

    Pk2Node* makeNode(Pk2Node* parent, const std::wstring& name, bool folder, u64 size,
                      const FILETIME* mtime) {
        pool.push_back(std::unique_ptr<Pk2Node>(new Pk2Node()));
        Pk2Node* n = pool.back().get();
        n->parent     = parent;
        n->name       = name;
        n->entry.type = folder ? Pk2EntryType::Folder : Pk2EntryType::File;
        n->entry.name = name;
        n->entry.size = static_cast<u32>(size > 0xFFFFFFFFull ? 0xFFFFFFFFull : size);
        if (mtime) {
            n->entry.modifyTime = (static_cast<u64>(mtime->dwHighDateTime) << 32) |
                                   mtime->dwLowDateTime;
            n->entry.createTime = n->entry.modifyTime;
        }
        n->fullPath = (parent && !parent->fullPath.empty())
                        ? parent->fullPath + L"\\" + name
                        : (parent ? name : std::wstring());
        n->loaded = !folder;
        if (parent) {
            if (folder) parent->folders.push_back(n);
            else        parent->files.push_back(n);
        }
        return n;
    }

    Result loadDir(Pk2Node* node) {
        if (!node || node->loaded) return Result::Ok();
        node->folders.clear();
        node->files.clear();

        const std::wstring disk = DiskPathOf(dir, node->fullPath);
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW((disk + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            node->loaded = true;                   // empty or unreadable: not fatal
            return Result::Ok();
        }
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            // Skip reparse points: a junction back up the tree would recurse forever.
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const u64  sz    = (static_cast<u64>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            makeNode(node, name, isDir, isDir ? 0 : sz, &fd.ftLastWriteTime);
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);

        auto byName = [](const Pk2Node* a, const Pk2Node* b) {
            return LowerW(a->name) < LowerW(b->name);
        };
        std::sort(node->folders.begin(), node->folders.end(), byName);
        std::sort(node->files.begin(),   node->files.end(),   byName);
        node->loaded = true;
        return Result::Ok();
    }

    /// Resolve an inner path, loading directories along the way.
    Pk2Node* resolve(const std::wstring& innerPath) {
        if (!rootNode) return nullptr;
        if (innerPath.empty()) return rootNode;

        std::wstring p = innerPath;
        for (wchar_t& c : p) if (c == L'/') c = L'\\';
        while (!p.empty() && p.front() == L'\\') p.erase(p.begin());

        Pk2Node* cur = rootNode;
        size_t start = 0;
        while (start <= p.size()) {
            size_t slash = p.find(L'\\', start);
            const std::wstring part = p.substr(start, slash == std::wstring::npos
                                                        ? std::wstring::npos
                                                        : slash - start);
            if (!part.empty()) {
                loadDir(cur);
                const std::wstring want = LowerW(part);
                Pk2Node* next = nullptr;
                for (Pk2Node* f : cur->folders) if (LowerW(f->name) == want) { next = f; break; }
                if (!next)
                    for (Pk2Node* f : cur->files) if (LowerW(f->name) == want) { next = f; break; }
                if (!next) return nullptr;
                cur = next;
            }
            if (slash == std::wstring::npos) break;
            start = slash + 1;
        }
        return cur;
    }

    Result readNode(const Pk2Node* node, size_t maxBytes, std::vector<u8>& out) const {
        out.clear();
        if (!node || !node->isFile())
            return Result::Fail(Status::NotFound, T(L"Dosya degil."));
        const std::wstring disk = DiskPathOf(dir, node->fullPath);
        Result r = ReadEntireFile(disk, out);
        if (!r.ok()) return r;
        if (maxBytes && out.size() > maxBytes) out.resize(maxBytes);
        // Loose client files carry the SAME per-shard asset encryption as the
        // ones inside a .pk2, so run the identical decrypt step - otherwise a
        // protected client would preview as garbage from a folder while it
        // previews fine from the archive.
        DecryptIfEncrypted(node->fullPath, out);
        return Result::Ok();
    }

    void walk(Pk2Node* node, const std::function<bool(Pk2Node*)>& visit, bool& stop) {
        if (stop || !node) return;
        loadDir(node);
        for (Pk2Node* f : node->files) {
            if (stop) return;
            if (!visit(f)) { stop = true; return; }
        }
        for (Pk2Node* d : node->folders) {
            if (stop) return;
            if (!visit(d)) { stop = true; return; }
            walk(d, visit, stop);
        }
    }
};

// ---------------------------------------------------------------------------

FolderArchive::FolderArchive() : m_impl(new Impl()) {}
FolderArchive::~FolderArchive() { close(); }

bool FolderArchive::LooksLikeAssetFolder(const std::wstring& path) {
    if (!DirectoryExists(path)) return false;
    const std::wstring self = LowerW(PathFileName(path));
    for (const wchar_t* k : kWellKnown) if (self == k) return true;

    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool hit = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring n = LowerW(fd.cFileName);
        if (n == L"." || n == L"..") continue;
        for (const wchar_t* k : kWellKnown) if (n == k) { hit = true; break; }
    } while (!hit && ::FindNextFileW(h, &fd));
    ::FindClose(h);
    return hit;
}

Result FolderArchive::open(const std::wstring& d, bool readOnlyIn) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mtx);
    close();
    if (!DirectoryExists(d))
        return Result::Fail(Status::NotFound, T(L"Klasor bulunamadi:\n") + d);

    std::wstring clean = d;
    while (clean.size() > 3 && (clean.back() == L'\\' || clean.back() == L'/')) clean.pop_back();

    m_impl->dir  = clean;
    m_impl->ro   = readOnlyIn;
    m_impl->open = true;
    m_impl->rootNode = m_impl->makeNode(nullptr, std::wstring(), true, 0, nullptr);
    m_impl->rootNode->loaded = false;
    return Result::Ok();
}

ArchiveKind FolderArchive::kind()     const { return ArchiveKind::Folder; }
bool        FolderArchive::canWrite() const { return !m_impl->ro; }
bool        FolderArchive::isOpen()   const { return m_impl->open; }
bool        FolderArchive::readOnly() const { return m_impl->ro; }
const std::wstring& FolderArchive::path() const { return m_impl->dir; }

u64 FolderArchive::fileSize() const { return 0; }   // a directory has no single size

Pk2HeaderInfo FolderArchive::headerInfo() const {
    Pk2HeaderInfo h;
    h.signature = L"KLASOR";
    return h;
}

Pk2Node* FolderArchive::root() const { return m_impl->rootNode; }

Result FolderArchive::loadChildren(Pk2Node* node) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mtx);
    if (!m_impl->open) return Result::Fail(Status::InvalidArgument, T(L"Klasor acik degil."));
    return m_impl->loadDir(node ? node : m_impl->rootNode);
}

void FolderArchive::childrenOf(Pk2Node* node,
                               std::vector<Pk2Node*>& folders,
                               std::vector<Pk2Node*>& files) {
    folders.clear();
    files.clear();
    std::lock_guard<std::recursive_mutex> lock(m_impl->mtx);
    if (!node) return;
    folders = node->folders;
    files   = node->files;
}

Result FolderArchive::findNode(const std::wstring& innerPath, Pk2Node** out) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mtx);
    if (out) *out = nullptr;
    Pk2Node* n = m_impl->resolve(innerPath);
    if (!n) return Result::Fail(Status::NotFound, T(L"Bulunamadi: ") + innerPath);
    if (out) *out = n;
    return Result::Ok();
}

Result FolderArchive::findFile(const std::wstring& innerPath, Pk2Node** out) {
    Pk2Node* n = nullptr;
    Result r = findNode(innerPath, &n);
    if (!r.ok()) return r;
    if (!n->isFile()) return Result::Fail(Status::NotFound, T(L"Bu bir klasor: ") + innerPath);
    if (out) *out = n;
    return Result::Ok();
}

Result FolderArchive::readFile(const Pk2Node* node, std::vector<u8>& out) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mtx);
    return m_impl->readNode(node, 0, out);
}

Result FolderArchive::readFile(const std::wstring& innerPath, std::vector<u8>& out) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mtx);
    Pk2Node* n = m_impl->resolve(innerPath);
    if (!n) return Result::Fail(Status::NotFound, T(L"Bulunamadi: ") + innerPath);
    return m_impl->readNode(n, 0, out);
}

Result FolderArchive::readFilePrefix(const Pk2Node* node, size_t maxBytes,
                                     std::vector<u8>& out) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mtx);
    return m_impl->readNode(node, maxBytes, out);
}

Result FolderArchive::readFilePrefix(const std::wstring& innerPath, size_t maxBytes,
                                     std::vector<u8>& out) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mtx);
    Pk2Node* n = m_impl->resolve(innerPath);
    if (!n) return Result::Fail(Status::NotFound, T(L"Bulunamadi: ") + innerPath);
    return m_impl->readNode(n, maxBytes, out);
}

Result FolderArchive::extractFile(const Pk2Node* node, const std::wstring& outFilePath) const {
    std::vector<u8> data;
    Result r = readFile(node, data);
    if (!r.ok()) return r;
    EnsureDirectory(PathParent(outFilePath));
    return WriteEntireFile(outFilePath, data.data(), data.size());
}

Result FolderArchive::extractTree(Pk2Node* node, const std::wstring& outDir,
                                  const ProgressFn& progress) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mtx);
    if (!node) node = m_impl->rootNode;
    if (!node) return Result::Fail(Status::InvalidArgument, T(L"Dugum yok."));
    if (node->isFile())
        return extractFile(node, PathJoin(outDir, node->name));

    // The output must not live INSIDE the archive: EnsureDirectory would create
    // it, the very next loadDir would enumerate it as a child, and the extractor
    // would copy its own output into itself level after level until the path
    // exceeded MAX_PATH - leaving a deep tree of duplicated client data in the
    // user's own client folder. A .pk2 cannot hit this; a directory can.
    if (PathIsWithin(m_impl->dir, outDir))
        return Result::Fail(Status::InvalidArgument,
                            T(L"Cikis klasoru arsivin kendi icinde olamaz:\n") + outDir);

    m_impl->loadDir(node);
    if (!EnsureDirectory(outDir))
        return Result::Fail(Status::IoError, T(L"Klasor olusturulamadi: ") + outDir);
    for (Pk2Node* f : node->files) {
        if (progress && !progress(0, 0, f->name.c_str())) return Result::Fail(Status::Cancelled, T(L"Iptal edildi."));
        Result r = extractFile(f, PathJoin(outDir, f->name));
        if (!r.ok()) return r;
    }
    for (Pk2Node* d : node->folders) {
        Result r = extractTree(d, PathJoin(outDir, d->name), progress);
        if (!r.ok()) return r;
    }
    return Result::Ok();
}

Result FolderArchive::extractAll(const std::wstring& outDir, const ProgressFn& progress) {
    return extractTree(m_impl->rootNode, outDir, progress);
}

Result FolderArchive::walkAll(const std::function<bool(Pk2Node*)>& visit, const ProgressFn&) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mtx);
    if (!m_impl->rootNode) return Result::Fail(Status::InvalidArgument, T(L"Klasor acik degil."));
    bool stop = false;
    m_impl->walk(m_impl->rootNode, visit, stop);
    return Result::Ok();
}

Result FolderArchive::search(const std::wstring& needle, std::vector<Pk2Node*>& results,
                             size_t maxResults, const ProgressFn& progress) {
    results.clear();
    const std::wstring want = LowerW(needle);
    return walkAll([&](Pk2Node* n) {
        if (n->isFile() && LowerW(n->name).find(want) != std::wstring::npos) {
            results.push_back(n);
            if (maxResults && results.size() >= maxResults) return false;
        }
        return true;
    }, progress);
}

Result FolderArchive::countEntries(bool, u64& folders, u64& files, const ProgressFn& progress) {
    folders = files = 0;
    return walkAll([&](Pk2Node* n) {
        if (n->isFolder()) ++folders; else ++files;
        return true;
    }, progress);
}

// --- writing ---------------------------------------------------------------

Result FolderArchive::addFile(Pk2Node* parentFolder, const std::wstring& name,
                              const u8* data, size_t size, bool overwrite) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mtx);
    if (m_impl->ro) return Result::Fail(Status::AccessDenied, T(L"Klasor salt okunur acildi."));
    if (!parentFolder) parentFolder = m_impl->rootNode;
    if (!parentFolder || !parentFolder->isFolder())
        return Result::Fail(Status::InvalidArgument, T(L"Hedef klasor degil."));
    if (!IsUsableName(name))
        return Result::Fail(Status::InvalidArgument, T(L"Gecersiz dosya adi: ") + name);

    const std::wstring disk = PathJoin(DiskPathOf(m_impl->dir, parentFolder->fullPath), name);
    if (!overwrite && FileExists(disk))
        return Result::Fail(Status::AlreadyExists, T(L"Zaten var: ") + name);
    EnsureDirectory(PathParent(disk));
    Result r = WriteEntireFileAtomic(disk, data, size);
    if (!r.ok()) return r;
    parentFolder->loaded = false;                  // re-enumerate on next open
    m_impl->loadDir(parentFolder);
    return Result::Ok();
}

Result FolderArchive::importFile(Pk2Node* parentFolder, const std::wstring& diskPath,
                                 const std::wstring& nameOverride, bool overwrite,
                                 const ProgressFn&) {
    std::vector<u8> data;
    Result r = ReadEntireFile(diskPath, data);
    if (!r.ok()) return r;
    const std::wstring name = nameOverride.empty() ? PathFileName(diskPath) : nameOverride;
    return addFile(parentFolder, name, data.data(), data.size(), overwrite);
}

Result FolderArchive::importFolder(Pk2Node* parentFolder, const std::wstring& diskDir,
                                   const ProgressFn& progress) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mtx);
    if (m_impl->ro) return Result::Fail(Status::AccessDenied, T(L"Klasor salt okunur acildi."));
    if (!parentFolder) parentFolder = m_impl->rootNode;
    if (!DirectoryExists(diskDir))
        return Result::Fail(Status::NotFound, T(L"Klasor yok: ") + diskDir);

    Pk2Node* sub = nullptr;
    Result r = addFolder(parentFolder, PathFileName(diskDir), &sub);
    if (!r.ok() && r.status != Status::AlreadyExists) return r;
    if (!sub) { r = findNode(parentFolder->fullPath.empty()
                                ? PathFileName(diskDir)
                                : parentFolder->fullPath + L"\\" + PathFileName(diskDir), &sub);
                if (!r.ok()) return r; }

    for (const std::wstring& f : ListDirectory(diskDir, L"*", true, false)) {
        if (progress && !progress(0, 0, PathFileName(f).c_str()))
            return Result::Fail(Status::Cancelled, T(L"Iptal edildi."));
        Result rr = importFile(sub, f, std::wstring(), true, progress);
        if (!rr.ok()) return rr;
    }
    for (const std::wstring& d : ListDirectory(diskDir, L"*", false, true)) {
        Result rr = importFolder(sub, d, progress);
        if (!rr.ok()) return rr;
    }
    return Result::Ok();
}

Result FolderArchive::addFolder(Pk2Node* parentFolder, const std::wstring& name, Pk2Node** out) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mtx);
    if (out) *out = nullptr;
    if (m_impl->ro) return Result::Fail(Status::AccessDenied, T(L"Klasor salt okunur acildi."));
    if (!parentFolder) parentFolder = m_impl->rootNode;
    if (!parentFolder || !parentFolder->isFolder())
        return Result::Fail(Status::InvalidArgument, T(L"Hedef klasor degil."));
    if (!IsUsableName(name))
        return Result::Fail(Status::InvalidArgument, T(L"Gecersiz klasor adi: ") + name);

    const std::wstring disk = PathJoin(DiskPathOf(m_impl->dir, parentFolder->fullPath), name);
    const bool existed = DirectoryExists(disk);
    if (!existed && !EnsureDirectory(disk))
        return Result::Fail(Status::IoError, T(L"Klasor olusturulamadi: ") + disk);

    parentFolder->loaded = false;
    m_impl->loadDir(parentFolder);
    const std::wstring want = LowerW(name);
    Pk2Node* made = nullptr;
    for (Pk2Node* f : parentFolder->folders)
        if (LowerW(f->name) == want) { made = f; break; }
    if (out) *out = made;
    // Reporting Ok with a null node let a silent failure look like success.
    if (!made)
        return Result::Fail(Status::IoError, T(L"Klasor olusturuldu ama agacta bulunamadi: ") + name);
    return existed ? Result::Fail(Status::AlreadyExists, T(L"Zaten var: ") + name) : Result::Ok();
}

Result FolderArchive::remove(Pk2Node* node) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mtx);
    if (m_impl->ro) return Result::Fail(Status::AccessDenied, T(L"Klasor salt okunur acildi."));
    if (!node || node->isRoot())
        return Result::Fail(Status::InvalidArgument, T(L"Silinemez."));

    const std::wstring disk = DiskPathOf(m_impl->dir, node->fullPath);
    if (node->isFile()) {
        if (!::DeleteFileW(disk.c_str()))
            return Result::Fail(Status::IoError, T(L"Silinemedi: ") + disk);
    } else {
        m_impl->loadDir(node);
        // Depth-first: RemoveDirectory only takes an empty directory.
        std::vector<Pk2Node*> kids = node->files;
        kids.insert(kids.end(), node->folders.begin(), node->folders.end());
        for (Pk2Node* k : kids) {
            Result r = remove(k);
            if (!r.ok()) return r;
        }
        if (!::RemoveDirectoryW(disk.c_str()))
            return Result::Fail(Status::IoError, T(L"Klasor silinemedi: ") + disk);
    }
    if (node->parent) {
        node->parent->loaded = false;
        m_impl->loadDir(node->parent);
    }
    return Result::Ok();
}

Result FolderArchive::rename(Pk2Node* node, const std::wstring& newName) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mtx);
    if (m_impl->ro) return Result::Fail(Status::AccessDenied, T(L"Klasor salt okunur acildi."));
    if (!node || node->isRoot() || newName.empty())
        return Result::Fail(Status::InvalidArgument, T(L"Yeniden adlandirilamaz."));
    if (newName.find(L'\\') != std::wstring::npos || newName.find(L'/') != std::wstring::npos)
        return Result::Fail(Status::InvalidArgument, T(L"Ad ayrac iceremez."));

    const std::wstring from = DiskPathOf(m_impl->dir, node->fullPath);
    const std::wstring to   = PathJoin(PathParent(from), newName);
    if (!::MoveFileExW(from.c_str(), to.c_str(), 0))
        return Result::Fail(Status::IoError, T(L"Yeniden adlandirilamadi: ") + from);

    // Re-enumerate the parent rather than patching fullPath on every descendant:
    // a stale fullPath deep in the tree is the classic source of silent data
    // loss on the next write.
    if (node->parent) {
        node->parent->loaded = false;
        m_impl->loadDir(node->parent);
    }
    return Result::Ok();
}

Result FolderArchive::compact(u64* savedBytes, const ProgressFn&) {
    if (savedBytes) *savedBytes = 0;   // a directory has no free list to reclaim
    return Result::Ok();
}

Result FolderArchive::flush() { return Result::Ok(); }   // nothing is buffered

void FolderArchive::close() {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mtx);
    m_impl->pool.clear();
    m_impl->rootNode = nullptr;
    m_impl->dir.clear();
    m_impl->open = false;
}

} // namespace sro
