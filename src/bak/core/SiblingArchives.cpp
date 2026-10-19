// ============================================================================
//  SRO Asset Studio - core/SiblingArchives.cpp
// ============================================================================
#include "SiblingArchives.h"
#include "FolderArchive.h"

#include "Blowfish.h"
#include "KeyRing.h"
#include "Pk2Archive.h"
#include "Pk2v2Archive.h"
#include "VpArchive.h"

#include <algorithm>
#include <mutex>

namespace sro {

namespace {

/// Containers worth consulting, most likely to hold geometry first. A shard can
/// name them anything, so this only orders the scan - it never filters.
int PreferenceRank(const std::wstring& lowerName) {
    if (lowerName.compare(0, 4, L"data") == 0)      return 0;
    if (lowerName.compare(0, 5, L"media") == 0)     return 1;
    if (lowerName.compare(0, 9, L"particles") == 0) return 2;
    if (lowerName.compare(0, 3, L"map") == 0)       return 3;
    return 4;
}

bool IsContainerExt(const std::wstring& ext) {
    return ext == L".pk2" || ext == L".pk3" || ext == L".lib" || ext == L".vp" || ext == L".ne";
}

} // namespace

// ---------------------------------------------------------------------------
/// Does this directory look like a client container (Data / Media / Particles /
/// Music, whatever it has been renamed to)? Judged by SHAPE, never by name -
/// a repack is free to call them anything, and a name whitelist would silently
/// drop the one folder that mattered.
bool LooksLikeContainerFolder(const std::wstring& dir) {
    static const wchar_t* kMarkers[] = {
        L"res", L"prim", L"server_dep", L"icon", L"interface",
        L"textures", L"meshes", L"animations", L"music", L"navmesh"
    };
    for (const wchar_t* m : kMarkers)
        if (DirectoryExists(PathJoin(dir, m))) return true;

    // No familiar subtree: accept it anyway if an asset file is sitting within
    // two levels, which is what a flattened extraction looks like.
    int budget = 400;
    for (const std::wstring& sub : ListDirectory(dir, L"*", false, true)) {
        for (const wchar_t* pat : { L"*.bsr", L"*.efp", L"*.ddj", L"*.bms", L"*.txt" }) {
            if (!ListDirectory(sub, pat, true, false).empty()) return true;
            if (--budget <= 0) return false;
        }
    }
    for (const wchar_t* pat : { L"*.bsr", L"*.efp", L"*.ddj", L"*.bms" })
        if (!ListDirectory(dir, pat, true, false).empty()) return true;
    return false;
}

struct SiblingArchives::Impl {
    struct Entry {
        std::wstring path;
        std::wstring name;
        bool         isFolder = false;  ///< a sibling CONTAINER can be a directory
        bool         tried  = false;    ///< open attempted (success or not)
        std::unique_ptr<IArchive> ar;   ///< null while unopened or unusable
    };

    mutable std::mutex mtx;
    std::wstring primaryPath;
    std::wstring dir;
    bool         havePrimaryCipher = false;
    Blowfish     primaryCipher;
    bool         scanned = false;
    std::vector<Entry> entries;
    std::wstring lastSource;

    void scanLocked() {
        if (scanned) return;
        scanned = true;
        if (dir.empty()) return;

        const std::wstring primaryLower = ToLowerW(PathFileName(primaryPath));

        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            const std::wstring name = fd.cFileName;
            const std::wstring lower = ToLowerW(name);
            if (lower == primaryLower || lower == L"." || lower == L"..") continue;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // A sibling CONTAINER can be a folder. Skipping directories is
                // what made an extracted client lose the .efp that a .bsr
                // points at: with `client\Data` open there was nothing left to
                // resolve `particles\...` against.
                const std::wstring sub = dir + L"\\" + name;
                if (!LooksLikeContainerFolder(sub)) continue;
                Entry e;
                e.path     = sub;
                e.name     = name;
                e.isFolder = true;
                entries.push_back(std::move(e));
                continue;
            }
            if (!IsContainerExt(PathExtension(lower))) continue;
            Entry e;
            e.path = dir + L"\\" + name;
            e.name = name;
            entries.push_back(std::move(e));
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);

        std::stable_sort(entries.begin(), entries.end(),
                         [](const Entry& a, const Entry& b) {
                             return PreferenceRank(ToLowerW(a.name)) <
                                    PreferenceRank(ToLowerW(b.name));
                         });
    }

    /// Open one sibling. Returns null when no key fits or the file is not a
    /// container we understand - that is normal, not an error worth reporting.
    std::unique_ptr<IArchive> openOne(const Entry& e) {
        if (e.isFolder) {
            auto fa = std::make_unique<FolderArchive>();
            if (!fa->open(e.path, /*readOnly*/ true).ok()) return nullptr;
            return fa;
        }
        const std::wstring ext = PathExtension(e.name);

        if (ext == L".vp") {
            if (!VpArchive::IsVpContainer(e.path)) return nullptr;
            auto vp = std::make_unique<VpArchive>();
            LogFn quiet;
            if (!vp->open(e.path, quiet, /*readOnly*/ true).ok()) return nullptr;
            return vp;
        }

        if (Pk2v2Archive::IsPk2v2(e.path)) {
            auto pk2v2 = std::make_unique<Pk2v2Archive>();
            LogFn quiet;
            if (pk2v2->open(e.path, "", quiet).ok()) return pk2v2;
            return nullptr;
        }

        // JMXPACK. The shard's own cipher is by far the most likely to fit, so
        // try it before spending anything on the stage-1 list.
        if (havePrimaryCipher) {
            auto pk2 = std::make_unique<Pk2Archive>();
            if (pk2->open(e.path, primaryCipher, /*readOnly*/ true).ok()) return pk2;
        }
        // A SailSafe (Sailor) sibling uses a literal zero-byte key; offer it as
        // the carried candidate so a neighbour opens even without a primary cipher.
        KeyCandidate sailorCarry;
        const KeyCandidate* carry = nullptr;
        if (Pk2Archive::IsSailorArchive(e.path)) {
            sailorCarry.label       = L"SailSafe/Sailor";
            sailorCarry.rawKeyBytes = { 0x00, 0x00, 0x00, 0x00 };
            carry = &sailorCarry;
        }
        KeyCandidate found;
        Blowfish     cipher;
        LogFn        quiet;
        if (!KeyRing::TryStage1(e.path, KeyRing::DefaultAsciiKey(), false, carry,
                                found, cipher, quiet).ok())
            return nullptr;
        auto pk2 = std::make_unique<Pk2Archive>();
        if (!pk2->open(e.path, cipher, /*readOnly*/ true).ok()) return nullptr;
        return pk2;
    }
};

// ---------------------------------------------------------------------------
SiblingArchives::SiblingArchives() : m_impl(new Impl()) {}
SiblingArchives::~SiblingArchives() = default;

SiblingArchives& SiblingArchives::Instance() {
    static SiblingArchives inst;
    return inst;
}

void SiblingArchives::configure(const std::wstring& primaryPath, const Blowfish* cipher) {
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    const std::wstring dir = PathParent(primaryPath);
    const bool same = (ToLowerW(primaryPath) == ToLowerW(m_impl->primaryPath));
    if (!same) {
        m_impl->entries.clear();
        m_impl->scanned = false;
        m_impl->lastSource.clear();
    }
    m_impl->primaryPath = primaryPath;
    m_impl->dir         = dir;
    if (cipher) { m_impl->primaryCipher = *cipher; m_impl->havePrimaryCipher = true; }
    else        { m_impl->havePrimaryCipher = false; }
}

void SiblingArchives::closeAll() {
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_impl->entries.clear();
    m_impl->scanned = false;
    m_impl->primaryPath.clear();
    m_impl->dir.clear();
    m_impl->havePrimaryCipher = false;
    m_impl->lastSource.clear();
}

bool SiblingArchives::read(const std::wstring& innerPath, std::vector<u8>& out) {
    if (innerPath.empty()) return false;
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    if (m_impl->dir.empty()) return false;
    m_impl->scanLocked();

    for (Impl::Entry& e : m_impl->entries) {
        if (!e.tried) {
            e.tried = true;
            e.ar    = m_impl->openOne(e);
        }
        if (!e.ar || !e.ar->isOpen()) continue;
        out.clear();
        if (e.ar->readFile(innerPath, out).ok() && !out.empty()) {
            m_impl->lastSource = e.name;
            return true;
        }
    }
    return false;
}

void SiblingArchives::enumerate(const std::function<bool(const std::wstring&)>& visit,
                                const std::wstring& preferNameSubstr) {
    if (!visit) return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    if (m_impl->dir.empty()) return;
    m_impl->scanLocked();

    // When any container matches the hint (e.g. "media"), walk ONLY those -
    // textdata never lives in Data/Map, so this skips the huge geometry archives.
    const std::wstring hint = ToLowerW(preferNameSubstr);
    bool anyPreferred = false;
    if (!hint.empty())
        for (const Impl::Entry& e : m_impl->entries)
            if (ToLowerW(e.name).find(hint) != std::wstring::npos) { anyPreferred = true; break; }

    for (Impl::Entry& e : m_impl->entries) {
        if (anyPreferred && ToLowerW(e.name).find(hint) == std::wstring::npos) continue;
        if (!e.tried) { e.tried = true; e.ar = m_impl->openOne(e); }
        if (!e.ar || !e.ar->isOpen()) continue;
        bool stop = false;
        e.ar->walkAll([&](Pk2Node* n) -> bool {
            if (n && n->isFile() && !visit(n->fullPath)) { stop = true; return false; }
            return true;
        }, ProgressFn{});
        if (stop) return;
    }
}

std::wstring SiblingArchives::lastSourceTr() const {
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    return m_impl->lastSource;
}

std::vector<std::wstring> SiblingArchives::openedTr() const {
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    std::vector<std::wstring> v;
    for (const Impl::Entry& e : m_impl->entries)
        if (e.ar && e.ar->isOpen()) v.push_back(e.name);
    return v;
}

} // namespace sro
