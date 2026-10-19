// Bridge implementation: wraps the vendored SRO Asset Studio core (namespace sro).
#include "bak_bridge.hpp"
#include "i18n.hpp"

#include "bak/Common.h"
#include "bak/core/AssetCipherAes.h"
#include "bak/core/AssetCipherGopro.h"
#include "bak/core/AssetCipherSbox.h"
#include "bak/core/AssetCipherVsro.h"
#include "bak/core/AssetCrypt.h"
#include "bak/core/Config.h"
#include "bak/core/FolderArchive.h"
#include "bak/core/KeyFinder.h"
#include "bak/core/KeyRing.h"
#include "bak/core/L33tPayloadKey.h"
#include "bak/core/Pk2Archive.h"
#include "bak/core/Pk2v2Archive.h"
#include "bak/core/SiblingArchives.h"
#include "bak/core/VpArchive.h"
#include "bak/formats/AssetDeps.h"
#include "bak/formats/AvatarSet.h"
#include "bak/formats/Ban.h"
#include "bak/formats/Bms.h"
#include "bak/formats/Bmt.h"
#include "bak/formats/Bsk.h"
#include "bak/formats/Bsr.h"
#include "bak/formats/ClientData.h"
#include "bak/formats/Ddj.h"
#include "bak/formats/DdsDecode.h"
#include "bak/formats/Efp.h"
#include "bak/formats/FormatId.h"
#include "bak/formats/MapFiles.h"
#include "bak/formats/Nif2dt.h"
#include "bak/formats/TextFile.h"
#include "bak/fx/EffectScene.h"
#include "bak/preview/PreviewEngine.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace bakb {

using pk2::L;
using pk2::Lf;

namespace {

// SiblingArchives is a process-wide singleton configured for ONE primary
// archive. Every bridge call that relies on it re-points it at its own archive
// and holds this lock for the whole operation (recursive: loaders re-enter).
std::recursive_mutex g_siblings;

/// Report text built when it is shown, so it follows the current UI language.
using LazyText = std::function<std::wstring()>;

LazyText Text(std::string_view key) {
    return [key] { return L(key); };
}

/// Our table's label for a KeyFinder stage (BAK's KeyFindStageNameTr is Turkish only).
std::string_view StageKey(sro::KeyFindStage stage) {
    using sro::KeyFindStage;
    switch (stage) {
        case KeyFindStage::Stage0SessionMemory:  return "bb_stage_session";
        case KeyFindStage::Stage0SiblingArchive: return "bb_stage_sibling";
        case KeyFindStage::Stage0RunningClient:  return "bb_stage_running";
        case KeyFindStage::Stage1Typed:          return "bb_stage_typed";
        case KeyFindStage::Stage1RawKey:         return "bb_stage_typed_raw";
        case KeyFindStage::Stage2StaticBaseKey:  return "bb_stage_disk_base";
        case KeyFindStage::Stage2StaticAsciiKey: return "bb_stage_disk_ascii";
        case KeyFindStage::Stage3ScannerA:       return "bb_stage_mem_ascii";
        case KeyFindStage::Stage3ScannerB:       return "bb_stage_mem_raw";
        default:                                 return "bb_stage_none";
    }
}

/// Key-material route of a candidate, without the key itself (BAK's privacy
/// rule: say that and how a key works, never which one).
LazyText KeyScheme(const sro::KeyCandidate& k) {
    if (k.useRawState) return Text("bb_key_raw_state");
    if (!k.rawKeyBytes.empty())
        return [n = k.rawKeyBytes.size()] { return Lf("bb_key_raw_bytes", n); };
    const bool named = k.baseId == sro::BaseKeyId::Std || k.baseId == sro::BaseKeyId::Paradise ||
                       k.baseId == sro::BaseKeyId::Zero;   // anything else: BAK's "OZEL" (custom)
    return [named, base = std::wstring(sro::BaseKeyName(k.baseId))] {
        return Lf("bb_key_ascii_base", named ? base : L("bb_key_custom_base"));
    };
}

/// BAK's JMXPACK key acquisition (main.cpp AcquireCipher + the SailSafe carry
/// from SiblingArchives), minus the interactive --basekey route. `scheme` /
/// `source` describe the winning route for protection_report().
sro::Result AcquireCipher(const std::wstring& path, const std::string& key, bool deep,
                          sro::Blowfish& cipher, LazyText& scheme, LazyText& source) {
    using namespace sro;

    // Encrypted = 0 archives: the root block is already plaintext ('.' folder).
    {
        u8 root[kPk2BlockSize] = {};
        if (Pk2Archive::ReadRawRootBlock(path, root).ok() && root[0] == 0x01 && root[1] == 0x2E) {
            KeyCandidate plain;
            plain.asciiKey = KeyRing::DefaultAsciiKey();
            plain.baseId   = BaseKeyId::Std;
            scheme = Text("bb_scheme_plain");
            source = Text("bb_source_none");
            return plain.makeCipher(cipher)
                       ? Result::Ok()
                       : Result::Fail(Status::WrongKey, L("bb_err_cipher"));
        }
    }

    const std::string typed = key.empty() ? std::string(KeyRing::DefaultAsciiKey()) : key;
    const ArchiveFingerprint fp = ArchiveFingerprint::fromFile(path);

    KeyCandidate remembered, sailor;
    const KeyCandidate* carry = nullptr;
    if (SessionKeys::Instance().lookup(fp, remembered)) {
        carry = &remembered;
    } else if (Pk2Archive::IsSailorArchive(path)) {
        sailor.label       = L"SailSafe/Sailor";
        sailor.rawKeyBytes = { 0x00, 0x00, 0x00, 0x00 };
        carry = &sailor;
    }

    const LogFn quiet;
    KeyCandidate used;
    Result r = KeyRing::TryStage1(path, typed, /*rawKeyMode*/ false, carry, used, cipher, quiet);
    if (r.ok()) {
        if (carry && used.label == carry->label && carry == &remembered)
            source = Text("bb_source_session");
        else if (carry && used.label == carry->label)
            source = Text("bb_source_sailsafe");
        else
            source = Text("bb_source_stage1");
    }

    if (!r.ok() && deep) {
        KeyFindOptions opt;
        opt.archivePath       = path;
        opt.typedKey          = typed;
        opt.allowLaunchClient = false;     // never start the game
        KeyFindResult fr;
        if (KeyFinder::FindKey(opt, fr, ProgressFn(), quiet).ok() && fr.found &&
            fr.key.makeCipher(cipher)) {
            used   = fr.key;
            source = [stage = StageKey(fr.stage)] { return Lf("bb_source_keyfinder", L(stage)); };
            r = Result::Ok();
        }
    }
    if (r.ok()) {
        scheme = KeyScheme(used);
        SessionKeys::Instance().remember(path, fp, used, used.describe());
    }
    return r;
}

/// "a\\b/c" -> {a, b, c}; empty components dropped.
std::vector<std::wstring> SplitInner(const std::wstring& p) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (const wchar_t c : p) {
        if (c == L'\\' || c == L'/') {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

std::wstring JoinInner(const std::vector<std::wstring>& parts, size_t count) {
    std::wstring s;
    for (size_t i = 0; i < count && i < parts.size(); ++i) {
        if (!s.empty()) s += L'\\';
        s += parts[i];
    }
    return s;
}

bool Finish(const sro::Result& r, std::wstring& err) {
    if (r.ok()) return true;
    err = r.text();
    return false;
}

/// bakb::Progress -> sro::ProgressFn.
sro::ProgressFn Adapt(const Progress& p) {
    if (!p) return sro::ProgressFn();
    return [p](sro::u64 done, sro::u64 total, const wchar_t* msg) {
        return p(done, total, msg ? std::wstring(msg) : std::wstring());
    };
}

bool IsContainerFile(const std::wstring& file) {
    const std::wstring ext = sro::ToLowerW(sro::PathExtension(file));
    return ext == L".pk2" || ext == L".pk3" || ext == L".lib" || ext == L".vp" ||
           sro::VpArchive::IsVpContainer(file) || sro::Pk2v2Archive::IsPk2v2(file);
}

/// Quaternion whose FromQuat() rows are (X, Y, Z) - sro's camera convention.
/// Same construction as render/Raster.cpp qFromBasis (internal there).
sro::Quat QuatFromBasis(const sro::Vec3& X, const sro::Vec3& Y, const sro::Vec3& Z) {
    sro::Quat q;
    const float t = X.x + Y.y + Z.z;
    if (t > 0.0f) {
        const float s = std::sqrt(t + 1.0f) * 2.0f;
        q.w = 0.25f * s; q.x = (Y.z - Z.y) / s; q.y = (Z.x - X.z) / s; q.z = (X.y - Y.x) / s;
    } else if (X.x > Y.y && X.x > Z.z) {
        const float s = std::sqrt(1.0f + X.x - Y.y - Z.z) * 2.0f;
        q.w = (Y.z - Z.y) / s; q.x = 0.25f * s; q.y = (X.y + Y.x) / s; q.z = (X.z + Z.x) / s;
    } else if (Y.y > Z.z) {
        const float s = std::sqrt(1.0f + Y.y - X.x - Z.z) * 2.0f;
        q.w = (Z.x - X.z) / s; q.x = (X.y + Y.x) / s; q.y = 0.25f * s; q.z = (Y.z + Z.y) / s;
    } else {
        const float s = std::sqrt(1.0f + Z.z - X.x - Y.y) * 2.0f;
        q.w = (X.y - Y.x) / s; q.x = (X.z + Z.x) / s; q.y = (Y.z + Z.y) / s; q.z = 0.25f * s;
    }
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 1e-20f ? sro::Quat(q.x / n, q.y / n, q.z / n, q.w / n) : sro::Quat();
}

std::wstring TextSummary(const sro::u8* d, size_t n) {
    using namespace sro;
    TextDocument doc;
    if (!LoadTextDocument(d, n, doc, 4u << 20).ok()) return {};
    std::wstring s = Lf("bb_desc_text", doc.summaryTr()) + L"\n";
    const TableDelimiter delim = DetectDelimiter(doc.text);
    if (delim != TableDelimiter::None) {
        TableData table;
        if (ParseTable(doc.text, delim, false, 200000, table).ok() && !table.rows.empty())
            s += table.summaryTr() + L"\n";
    }
    return s;
}

} // namespace

// ===========================================================================
// Archive
// ===========================================================================
struct Archive::Impl {
    std::unique_ptr<sro::IArchive> ar;
    sro::Pk2Archive* pk2 = nullptr;     // typed views of `ar` (non-owning)
    sro::VpArchive*  vp  = nullptr;
    std::wstring path;
    std::wstring kind;
    bool         hasCipher = false;
    sro::Blowfish cipher;

    bool         writable  = false;     // write access was requested
    bool         sailsafe  = false;     // SailSafe JMXPACK (root block at 0x33E)
    bool         l33t      = false;     // VP: L33T flavour
    LazyText     keyScheme, keySource;  // protection_report()
    std::string_view writeNote;         // key of why a requested write access was refused
    std::wstring assetNote, vpKeyNote;  // last recover_* outcome

    mutable std::mutex         entriesMtx;
    mutable bool               entriesValid = false;
    mutable std::vector<Entry> entries;

    void invalidate() {
        std::lock_guard<std::mutex> lk(entriesMtx);
        entriesValid = false;
    }

    /// Ok when a write can go through, else the reason.
    sro::Result writeGate() const {
        using sro::Result;
        using sro::Status;
        if (!writable)
            return Result::Fail(Status::AccessDenied, L("bb_err_not_writable"));
        if (kind == L"PK2v2")
            return Result::Fail(Status::Unsupported, L("bb_err_pk2v2_readonly"));
        if (!ar->canWrite())
            return Result::Fail(Status::AccessDenied, L(writeNote.empty() ? "bb_err_write_open" : writeNote));
        if (vp) {
            if (!vp->headerInfo().signatureValid)
                return Result::Fail(Status::AccessDenied, L("bb_err_vp_signature"));
            if (vp->payloadEncrypted() && !vp->payloadKeyReady())
                return Result::Fail(Status::AccessDenied, L("bb_err_vp_payload_key"));
        }
        return Result::Ok();
    }

    /// Walks/creates `parts[0..count)` as folders; `out` = the last one (root for 0).
    sro::Result ensureFolder(const std::vector<std::wstring>& parts, size_t count, sro::Pk2Node** out) {
        using sro::Result;
        using sro::Status;
        *out = nullptr;
        sro::Pk2Node* cur = ar->root();
        if (!cur) return Result::Fail(Status::InvalidArgument, L("bb_err_not_open"));
        for (size_t i = 0; i < count; ++i) {
            const std::wstring prefix = JoinInner(parts, i + 1);
            sro::Pk2Node* n = nullptr;
            if (ar->findNode(prefix, &n).ok() && n) {
                if (!n->isFolder())
                    return Result::Fail(Status::AlreadyExists, Lf("bb_err_file_in_path", prefix));
                cur = n;
                continue;
            }
            sro::Pk2Node* made = nullptr;
            const Result r = ar->addFolder(cur, parts[i], &made);
            if (!made)
                return r.ok() ? Result::Fail(Status::IoError, Lf("bb_err_mkdir", prefix)) : r;
            cur = made;
        }
        *out = cur;
        return Result::Ok();
    }

    sro::Result findExisting(const std::wstring& inner, sro::Pk2Node** out) {
        *out = nullptr;
        const std::wstring p = JoinInner(SplitInner(inner), static_cast<size_t>(-1));
        if (p.empty())
            return sro::Result::Fail(sro::Status::InvalidArgument, L("bb_err_root_op"));
        const sro::Result r = ar->findNode(p, out);
        if (!r.ok() || !*out)
            return sro::Result::Fail(sro::Status::NotFound, Lf("bb_err_not_found", p));
        return sro::Result::Ok();
    }

    void pointSiblings() const {
        sro::SiblingArchives::Instance().configure(path, hasCipher ? &cipher : nullptr);
    }

    /// The other containers next to this one (read-only, assets decrypted).
    bool readSibling(const std::wstring& inner, std::vector<sro::u8>& out) const {
        out.clear();
        std::lock_guard<std::recursive_mutex> lk(g_siblings);
        pointSiblings();
        return sro::SiblingArchives::Instance().read(inner, out);
    }

    /// Primary first, then the siblings.
    bool readAny(const std::wstring& inner, std::vector<sro::u8>& out) const {
        out.clear();
        return ar->readFile(inner, out).ok() || readSibling(inner, out);
    }

    /// SiblingArchives offers no existence query, so a sibling hit costs a read.
    bool exists(const std::wstring& inner) const {
        sro::Pk2Node* node = nullptr;
        if (ar->findFile(inner, &node).ok()) return true;
        std::vector<sro::u8> tmp;
        return readSibling(inner, tmp);
    }

    sro::AssetLoader loader() const {
        return [this](const std::wstring& p, std::vector<sro::u8>& out) {
            return readAny(sro::NormalizeInnerPath(p), out) && !out.empty();
        };
    }
};

Archive::Archive() : p_(std::make_unique<Impl>()) {}
Archive::~Archive() = default;

std::unique_ptr<Archive> Archive::open(const std::wstring& path, const std::string& key,
                                       std::wstring& err, bool deep_key_search) {
    return open(path, key, err, deep_key_search, /*writable*/ false);
}

std::unique_ptr<Archive> Archive::open(const std::wstring& path, const std::string& key,
                                       std::wstring& err, bool deep_key_search, bool writable) {
    using namespace sro;
    err.clear();
    std::unique_ptr<Archive> a(new Archive());
    Impl& m = *a->p_;
    m.path     = path;
    m.writable = writable;
    const LogFn quiet;

    // A container held open by the sibling resolver cannot be swapped by a
    // compact or opened for writing cleanly; it reopens lazily on the next read.
    if (writable) {
        std::lock_guard<std::recursive_mutex> lk(g_siblings);
        SiblingArchives::Instance().closeAll();
    }

    Result r;
    if (DirectoryExists(path)) {
        auto fa = std::make_unique<FolderArchive>();
        r = fa->open(path, /*readOnly*/ !writable);
        m.ar = std::move(fa);
        m.kind = L"Folder";
        m.keyScheme = Text("bb_scheme_folder");
    } else if (!FileExists(path)) {
        err = Lf("bb_err_file_not_found", path);
        return nullptr;
    } else if (Pk2v2Archive::IsPk2v2(path)) {
        auto v2 = std::make_unique<Pk2v2Archive>();
        r = v2->open(path, key, quiet);
        m.ar = std::move(v2);
        m.kind = L"PK2v2";
        m.keyScheme = Text("bb_scheme_pk2v2");
        m.keySource = Text(key.empty() ? "bb_source_pk2v2_default" : "bb_source_pk2v2_given");
        if (writable) m.writeNote = "bb_err_pk2v2_readonly";
    } else if (VpArchive::IsVpContainer(path)) {
        auto vp = std::make_unique<VpArchive>();
        r = vp->open(path, quiet, /*readOnly*/ !writable);
        m.vp = vp.get();
        m.ar = std::move(vp);
        m.kind = L"VP";
        m.l33t = IsL33tContainer(path);
        m.keyScheme = Text(m.l33t ? "bb_scheme_vp_l33t" : "bb_scheme_vp_tools");
        m.keySource = Text("bb_source_vp");
        if (r.ok() && writable && m.vp->readOnly())
            m.writeNote = "bb_note_vp_locked";
    } else {
        r = AcquireCipher(path, key, deep_key_search, m.cipher, m.keyScheme, m.keySource);
        if (r.ok()) {
            auto pk2 = std::make_unique<Pk2Archive>();
            r = pk2->open(path, m.cipher, /*readOnly*/ !writable);
            m.pk2 = pk2.get();
            m.ar = std::move(pk2);
            m.hasCipher = true;
            m.sailsafe = Pk2Archive::IsSailorArchive(path);
            if (r.ok() && writable && m.pk2->readOnly())
                m.writeNote = "bb_note_pk2_locked";
        }
        m.kind = L"PK2";
    }
    if (!r.ok() || !m.ar || !m.ar->isOpen()) {
        err = r.ok() ? L("bb_err_open_failed") : r.text();
        return nullptr;
    }

    // vSRO File Protection / GoPro: armed from markers in this archive or its
    // siblings, exactly as BAK does on every open. Each asset is still decided
    // individually by the JMXV accept test.
    {
        std::lock_guard<std::recursive_mutex> lk(g_siblings);
        m.pointSiblings();
        VsroTryInit(*m.ar);
        GoproTryInit(*m.ar);
    }
    return a;
}

std::wstring Archive::kind() const { return p_->kind; }
bool Archive::can_write() const { return p_->writeGate().ok(); }
const std::wstring& Archive::path() const { return p_->path; }
std::uint64_t Archive::skipped_entries() const { return p_->ar->skippedEntryCount(); }

const std::vector<Entry>& Archive::entries() const {
    std::lock_guard<std::mutex> lk(p_->entriesMtx);
    if (!p_->entriesValid) {
        std::vector<Entry>& v = p_->entries;
        v.clear();
        p_->ar->walkAll([&v](sro::Pk2Node* n) {
            if (!n || n->isRoot()) return true;
            Entry e;
            e.path   = sro::NormalizeInnerPath(n->fullPath);
            if (e.path.empty()) return true;
            e.folder = !n->isFile();
            e.size   = e.folder ? 0 : n->entry.size;
            e.mtime  = static_cast<std::int64_t>(n->entry.modifyTime);
            v.push_back(std::move(e));
            return true;
        }, sro::ProgressFn());
        p_->entriesValid = true;
    }
    return p_->entries;
}

// ---------------------------------------------------------------------------
// Writing - BAK's IArchive mutations, addressed by inner path.
// ---------------------------------------------------------------------------
bool Archive::add_file(const std::wstring& inner, const std::vector<std::uint8_t>& bytes,
                       std::wstring& err) {
    err.clear();
    Impl& m = *p_;
    sro::Result r = m.writeGate();
    if (r.ok()) {
        const std::vector<std::wstring> parts = SplitInner(inner);
        if (parts.empty()) {
            r = sro::Result::Fail(sro::Status::InvalidArgument, L("bb_err_empty_path"));
        } else {
            sro::Pk2Node* parent = nullptr;
            r = m.ensureFolder(parts, parts.size() - 1, &parent);
            if (r.ok()) r = m.ar->addFile(parent, parts.back(), bytes.data(), bytes.size(), /*overwrite*/ true);
            m.invalidate();
        }
    }
    return Finish(r, err);
}

bool Archive::remove(const std::wstring& inner, std::wstring& err) {
    err.clear();
    Impl& m = *p_;
    sro::Result r = m.writeGate();
    sro::Pk2Node* n = nullptr;
    if (r.ok()) r = m.findExisting(inner, &n);
    if (r.ok()) {
        r = m.ar->remove(n);
        m.invalidate();
    }
    return Finish(r, err);
}

bool Archive::add_folder(const std::wstring& inner, std::wstring& err) {
    err.clear();
    Impl& m = *p_;
    sro::Result r = m.writeGate();
    if (r.ok()) {
        const std::vector<std::wstring> parts = SplitInner(inner);
        sro::Pk2Node* made = nullptr;
        if (parts.empty()) r = sro::Result::Fail(sro::Status::InvalidArgument, L("bb_err_empty_folder_path"));
        else r = m.ensureFolder(parts, parts.size(), &made);
        m.invalidate();
    }
    return Finish(r, err);
}

bool Archive::rename(const std::wstring& inner, const std::wstring& newName, std::wstring& err) {
    err.clear();
    Impl& m = *p_;
    sro::Result r = m.writeGate();
    if (r.ok() && (newName.empty() || newName.find_first_of(L"\\/") != std::wstring::npos))
        r = sro::Result::Fail(sro::Status::InvalidArgument, L("bb_err_bad_name"));
    sro::Pk2Node* n = nullptr;
    if (r.ok()) r = m.findExisting(inner, &n);
    if (r.ok()) {
        r = m.ar->rename(n, newName);
        m.invalidate();
    }
    return Finish(r, err);
}

bool Archive::import_folder(const std::wstring& diskDir, const std::wstring& innerDest,
                            const Progress& progress, std::wstring& err) {
    using namespace sro;
    err.clear();
    Impl& m = *p_;
    Result r = m.writeGate();
    if (r.ok() && !DirectoryExists(diskDir))
        r = Result::Fail(Status::NotFound, Lf("bb_err_folder_not_found", diskDir));
    if (!r.ok()) return Finish(r, err);

    // Plan first (for byte progress), then import file by file through the
    // backend's own importFile - exactly what Pk2Archive::importFolder does, but
    // with one target semantic for every backend and node pointers re-resolved
    // per file (VP rebuilds its tree on every write).
    struct Item { std::wstring disk; std::vector<std::wstring> parts; u64 size; };
    std::vector<Item> files;
    std::vector<std::vector<std::wstring>> folders;
    u64 total = 0;
    const std::vector<std::wstring> base = SplitInner(innerDest);
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> stack{ { diskDir, base } };
    while (!stack.empty()) {
        auto [dir, inner] = std::move(stack.back());
        stack.pop_back();
        folders.push_back(inner);
        for (const std::wstring& f : ListDirectory(dir, L"*", true, false)) {
            Item it{ f, inner, GetFileSize64(f) };
            it.parts.push_back(PathFileName(f));
            total += it.size;
            files.push_back(std::move(it));
        }
        for (const std::wstring& d : ListDirectory(dir, L"*", false, true)) {
            std::vector<std::wstring> sub = inner;
            sub.push_back(PathFileName(d));
            stack.emplace_back(d, std::move(sub));
        }
    }

    u64 failed = 0, done = 0;
    Result first = Result::Ok();
    for (const std::vector<std::wstring>& f : folders) {
        Pk2Node* made = nullptr;
        const Result fr = m.ensureFolder(f, f.size(), &made);
        if (!fr.ok() && first.ok()) first = fr;
    }
    for (const Item& it : files) {
        Pk2Node* parent = nullptr;
        Result fr = m.ensureFolder(it.parts, it.parts.size() - 1, &parent);
        if (fr.ok()) fr = m.ar->importFile(parent, it.disk, it.parts.back(), /*overwrite*/ true, ProgressFn());
        if (!fr.ok()) {
            ++failed;
            if (first.ok()) first = Result::Fail(fr.status, it.disk + L": " + fr.text());
        }
        done += it.size;
        if (progress && !progress(done, total, JoinInner(it.parts, it.parts.size()))) {
            m.invalidate();
            return Finish(Result::Fail(Status::Cancelled, L("bb_import_cancelled")), err);
        }
    }
    m.invalidate();
    if (failed)
        return Finish(Result::Fail(first.status, Lf("bb_import_failed", failed, files.size(), first.text())), err);
    return Finish(first, err);
}

bool Archive::compact(std::uint64_t* saved, const Progress& progress, std::wstring& err) {
    err.clear();
    if (saved) *saved = 0;
    Impl& m = *p_;
    sro::Result r = m.writeGate();
    if (r.ok() && m.sailsafe)
        r = sro::Result::Fail(sro::Status::Unsupported, L("bb_err_sailsafe_compact"));
    if (r.ok()) {
        // The rebuild renames the file; no sibling handle may stay open on it.
        std::lock_guard<std::recursive_mutex> lk(g_siblings);
        sro::SiblingArchives::Instance().closeAll();
        sro::u64 s = 0;
        r = m.ar->compact(&s, Adapt(progress));
        if (saved) *saved = s;
        m.invalidate();
    }
    return Finish(r, err);
}

bool Archive::flush(std::wstring& err) {
    err.clear();
    sro::Result r = p_->writeGate();
    if (r.ok()) r = p_->ar->flush();
    return Finish(r, err);
}

// ---------------------------------------------------------------------------
// Keys / protection
// ---------------------------------------------------------------------------
bool Archive::recover_vp_payload_key(std::wstring& how) {
    using namespace sro;
    how.clear();
    Impl& m = *p_;
    if (!m.vp) {
        how = L("bb_vp_not_container");
        return false;
    }
    if (!m.vp->payloadEncrypted()) {
        how = m.vpKeyNote = L("bb_vp_not_encrypted");
        return true;
    }
    if (m.vp->payloadKeyReady()) {
        how = m.vpKeyNote = L("bb_vp_key_ready");
        return true;
    }
    std::wstring log;
    auto plog = [&log](const std::wstring& s) { log += L"\n  " + s; };
    bool ok = RecoverL33tPayloadKey(*m.vp, L"sro_client.exe", plog);
    std::wstring where = L("bb_vp_where_client");
    if (!ok) {
        const std::wstring dir = PathParent(m.path);
        for (const std::wstring& dmp : ListDirectory(dir, L"*.dmp", true, false)) {
            plog(Lf("bb_vp_trying_dump", PathFileName(dmp)));
            if (RecoverL33tPayloadKeyFromDump(*m.vp, dmp, plog)) {
                ok = true;
                where = Lf("bb_vp_where_dump", PathFileName(dmp));
                break;
            }
        }
    }
    how = ok ? Lf("bb_vp_key_found", where) : L("bb_vp_key_missing");
    how += log;
    m.vpKeyNote = how;
    return ok;
}

std::wstring Archive::protection_report() const {
    using namespace sro;
    const Impl& m = *p_;
    std::wstring s;
    auto line = [&s](const std::wstring& k, const std::wstring& v) { s += k + L": " + v + L"\n"; };

    // --- container ---------------------------------------------------------
    const Pk2HeaderInfo hi = m.ar->headerInfo();
    std::wstring kindText;
    if (m.kind == L"PK2") {
        kindText = (m.sailsafe ? L("bb_kind_sailsafe") : std::wstring(L"JMXPACK"))
                 + L" " + ToLowerW(PathExtension(m.path));
        if (!m.sailsafe) {
            std::wstring sig = hi.signature;      // "JoyMax File Manager!\n"
            while (!sig.empty() && std::iswspace(sig.back())) sig.pop_back();
            kindText += Lf("bb_kind_signature", sig, hi.versionText);
        }
        kindText += L((hi.encrypted || m.sailsafe) ? "bb_kind_index_encrypted" : "bb_kind_index_plain");
    } else if (m.kind == L"PK2v2") {
        kindText = L"Sailor PK2v2 " + hi.versionText;
    } else if (m.kind == L"VP") {
        kindText = std::wstring(m.l33t ? L"L33T ENCRYPTED CONTAINER" : L"VSROPLUS TOOLS ENCRYPTED CONTAINER")
                 + Lf("bb_kind_version", hi.versionText);
        if (!m.vp->containerId().empty())
            kindText += Lf("bb_kind_id", std::wstring(m.vp->containerId().begin(), m.vp->containerId().end()));
    } else {
        kindText = L("bb_kind_folder");
    }
    line(L("bb_rep_container"), kindText);
    if (m.ar->fileSize()) line(L("bb_rep_size"), FormatBytesTr(m.ar->fileSize()));
    line(L("bb_rep_key_scheme"), m.keyScheme ? m.keyScheme() : std::wstring(L"-"));
    if (m.keySource) line(L("bb_rep_key_source"), m.keySource());
    if (m.ar->skippedEntryCount())
        line(L("bb_rep_skipped"), Lf("bb_rep_skipped_value", m.ar->skippedEntryCount()));

    // --- write status --------------------------------------------------------
    const Result gate = m.writeGate();
    std::wstring w = gate.ok() ? L("bb_rep_write_on") : Lf("bb_rep_write_off", gate.text());
    if (gate.ok() && m.kind == L"PK2") w += L(m.sailsafe ? "bb_rep_write_pk2_sailsafe" : "bb_rep_write_pk2");
    if (gate.ok() && m.kind == L"VP") w += L("bb_rep_write_vp");
    if (gate.ok() && m.kind == L"Folder") w += L("bb_rep_write_folder");
    line(L("bb_rep_write"), w);

    // --- VP specifics ----------------------------------------------------------
    if (m.vp) {
        line(L("bb_rep_vp_sig"), L(hi.signatureValid ? "bb_rep_vp_sig_ok" : "bb_rep_vp_sig_bad"));
        line(L("bb_rep_vp_payload"), L(!m.vp->payloadEncrypted() ? "bb_rep_vp_plain"
                                       : m.vp->payloadKeyReady() ? "bb_rep_vp_key_ready"
                                                                 : "bb_rep_vp_key_missing"));
        if (!m.vpKeyNote.empty()) line(L("bb_rep_vp_note"), m.vpKeyNote);
    }

    // --- asset ciphers (process-wide, decrypt-only) ----------------------------
    std::wstring ac;
    auto add = [&ac](const std::wstring& t) { ac += L"\n  - " + t; };
    add(L("bb_ac_sailor"));
    add(L("bb_ac_xor"));
    add(L(SboxConfirmed() ? "bb_ac_sbox_verified" : "bb_ac_sbox"));
    const size_t cov = KeystreamVault::Instance().coverage();
    add(cov ? Lf("bb_ac_keystream_on", FormatBytesTr(cov)) : L("bb_ac_keystream_off"));
    add(VsroActive() ? Lf("bb_ac_vsro_on", VsroGatewayTr()) : L("bb_ac_vsro_off"));
    add(GoproActive() ? Lf("bb_ac_gopro_on", GoproStatusTr()) : L("bb_ac_gopro_off"));
    add(AesActive() ? Lf("bb_ac_aes_on", AesStatusTr()) : L("bb_ac_aes_off"));
    if (!m.assetNote.empty()) add(Lf("bb_ac_last", m.assetNote));
    add(L("bb_ac_plain_writes"));
    s += L("bb_rep_asset_ciphers") + ac + L"\n";

    // --- siblings ----------------------------------------------------------------
    std::wstring sib;
    const std::wstring dir = PathParent(m.path);
    for (const std::wstring& f : ListDirectory(dir, L"*", true, false))
        if (!IEqualsW(f, m.path) && IsContainerFile(f)) sib += (sib.empty() ? L"" : L", ") + PathFileName(f);
    line(L("bb_rep_siblings"), sib.empty() ? L("bb_rep_none") : sib);
    {
        std::lock_guard<std::recursive_mutex> lk(g_siblings);
        m.pointSiblings();
        std::wstring opened;
        for (const std::wstring& o : SiblingArchives::Instance().openedTr())
            opened += (opened.empty() ? L"" : L", ") + o;
        if (!opened.empty()) line(L("bb_rep_siblings_open"), opened);
    }
    return s;
}

bool Archive::read(const std::wstring& path, std::vector<std::uint8_t>& out, std::wstring& err,
                   bool siblings) const {
    err.clear();
    out.clear();
    const std::wstring inner = sro::NormalizeInnerPath(path);
    const sro::Result r = p_->ar->readFile(inner, out);
    if (r.ok()) return true;
    if (siblings && p_->readSibling(inner, out)) return true;
    out.clear();
    err = r.text();
    return false;
}

bool Archive::recover_asset_cipher(std::wstring& how) {
    how.clear();
    std::lock_guard<std::recursive_mutex> lk(g_siblings);
    p_->pointSiblings();
    bool hadEncrypted = false;
    if (sro::RecoverAssetCipher(*p_->ar, sro::PathParent(p_->path), &how, &hadEncrypted)) {
        if (how.empty()) how = L("bb_ac_active");
        p_->assetNote = how;
        return true;
    }
    how = L(hadEncrypted ? "bb_ac_not_found" : "bb_ac_none");
    p_->assetNote = how;
    return false;
}

std::vector<std::wstring> Archive::dependencies(const std::wstring& path,
                                                std::vector<std::wstring>& missing) const {
    std::lock_guard<std::recursive_mutex> lk(g_siblings);
    p_->pointSiblings();
    sro::AssetDeps d = sro::CollectAssetDependencies(sro::NormalizeInnerPath(path), p_->loader());
    missing = std::move(d.missingTr);
    return std::move(d.paths);
}

std::vector<std::wstring> Archive::avatar_set(const std::wstring& bsr_path, std::wstring& family) const {
    std::lock_guard<std::recursive_mutex> lk(g_siblings);
    p_->pointSiblings();
    const sro::AvatarCatalog& cat = sro::GetAvatarCatalog(p_->ar.get(), p_->loader());
    sro::AvatarSetPlan plan = sro::PlanAvatarSet(cat, sro::NormalizeInnerPath(bsr_path));
    family = std::move(plan.family);
    return std::move(plan.pieces);
}

// ===========================================================================
// describe / decrypt_asset
// ===========================================================================
std::wstring describe(const std::wstring& name, const std::vector<std::uint8_t>& bytes) {
    using namespace sro;
    const u8* d = bytes.data();
    const size_t n = bytes.size();
    const FormatId id = Identify(name, d, n);
    if (id == FormatId::Unknown || id == FormatId::Binary) return {};

    std::wstring body;
    switch (id) {
        case FormatId::Efp: {
            // A partial parse still carries the referenced assets; BAK previews
            // it either way, so the summary is not gated on the result.
            EfpFile f;
            ParseEfp(d, n, f);
            body = f.summaryTr();
            break;
        }
        case FormatId::Bsr:  { BsrFile f;        if (ParseBsr(d, n, f).ok())             body = f.summaryTr(); break; }
        case FormatId::Bms:  { BmsFile f;        if (ParseBms(d, n, f, true).ok())       body = f.summaryTr(); break; }
        case FormatId::Bmt:  { BmtFile f;        if (ParseBmt(d, n, f).ok())             body = f.summaryTr(); break; }
        case FormatId::Bsk:  { BskFile f;        if (ParseBsk(d, n, f).ok())             body = f.summaryTr(); break; }
        case FormatId::Ban:  { BanFile f;        if (ParseBan(d, n, f, true).ok())       body = f.summaryTr(); break; }
        case FormatId::Nif2dt: { Nif2dtFile f;   if (ParseNif2dt(d, n, f).ok())          body = f.summaryTr(); break; }
        case FormatId::Nvm:  { NvmFile f;        if (ParseNvm(d, n, f, true).ok())       body = f.summaryTr(); break; }
        case FormatId::Dof:  { DofFile f;        if (ParseDof(d, n, f).ok())             body = f.summaryTr(); break; }
        case FormatId::MapM: { MapMeshFile f;    if (ParseMapMesh(d, n, f).ok())         body = f.summaryTr(); break; }
        case FormatId::MapT: { MapTextureFile f; if (ParseMapTexture(d, n, f).ok())      body = f.summaryTr(); break; }
        case FormatId::Mfo:  { MfoFile f;        if (ParseMfo(d, n, f).ok())             body = f.summaryTr(); break; }
        case FormatId::Cpd:  { CpdFile f;        if (ParseCpd(d, n, f).ok())             body = f.summaryTr(); break; }
        case FormatId::Envi: { EnviFile f;       if (ParseEnvi(d, n, f).ok())            body = f.summaryTr(); break; }
        case FormatId::MapO: {
            // Auto-detect first; if that recovers nothing, try both fixed
            // layouts and keep whichever yields the most objects (as BAK does).
            MapObjectFile f;
            Result r = ParseMapObject(d, n, -1, f);
            if (!r.ok() || f.objects.empty()) {
                for (int mode : { 0, 1 }) {
                    MapObjectFile g;
                    if (ParseMapObject(d, n, mode, g).ok() && g.objects.size() > f.objects.size()) {
                        f = std::move(g);
                        r = Result::Ok();
                    }
                }
            }
            if (r.ok()) body = f.summaryTr();
            break;
        }
        case FormatId::Ddj: {
            DdjFile f;
            if (ParseDdj(d, n, f, nullptr))
                body = Lf("bb_desc_ddj", f.dds.width, f.dds.height, f.dds.formatName, f.dds.mipCount,
                          f.textureTypeNameTr()) + L"\n";
            break;
        }
        case FormatId::Dds: {
            DdsInfo f;
            if (ParseDds(d, n, f, nullptr))
                body = Lf("bb_desc_dds", f.width, f.height, f.formatName, f.mipCount) + L"\n";
            break;
        }
        case FormatId::IfoEnvironment:
        case FormatId::IfoObject:
        case FormatId::IfoObjectString:
        case FormatId::IfoObjectExt:
        case FormatId::IfoTile2d: {
            IfoFile f;
            if (ParseIfo(d, n, PathFileName(name), f).ok() && !f.rows.empty()) body = f.summaryTr();
            else body = TextSummary(d, n);
            break;
        }
        case FormatId::DivisionInfo:
        case FormatId::GatePort:
        case FormatId::SilkroadVersion:
        case FormatId::SilkCfg:
        case FormatId::WndPos:
        case FormatId::GmWarpFort:
        case FormatId::AiNavData: {
            ClientDataKind kind = ClientDataKind::None;
            std::wstring props;
            if (DescribeClientData(name, d, n, kind, props).ok())
                body = std::wstring(ClientDataNameTr(kind)) + L"\n" + props;
            break;
        }
        case FormatId::Text:
        case FormatId::Csv:
        case FormatId::Ini:
        case FormatId::Lua:
        case FormatId::Xml:
        case FormatId::Sct:
            body = TextSummary(d, n);
            break;
        default:
            break;
    }

    std::wstring s = std::wstring(FormatDisplayName(id)) + L"\n";
    s += Lf("bb_desc_size", FormatBytesTr(n)) + L"\n";
    s += Lf("bb_desc_signature", DescribeSignature(d, n)) + L"\n";
    if (!body.empty()) s += L"\n" + body;
    return s;
}

bool decrypt_asset(const std::wstring& name, std::vector<std::uint8_t>& bytes) {
    return sro::DecryptIfEncrypted(name, bytes);
}

// ===========================================================================
// build_preview
// ===========================================================================
namespace {

/// sro::Image is RGBA8 (straight alpha, tight rows) -> BGRA8.
void ToBgra(const sro::Image& src, PreviewImage& dst) {
    dst = PreviewImage();
    if (!src.valid()) return;
    dst.width  = static_cast<std::uint32_t>(src.width());
    dst.height = static_cast<std::uint32_t>(src.height());
    dst.bgra.resize(src.sizeBytes());
    const sro::u8* s = src.data();
    std::uint8_t* d = dst.bgra.data();
    for (size_t i = 0, n = src.sizeBytes(); i < n; i += 4) {
        d[i + 0] = s[i + 2];
        d[i + 1] = s[i + 1];
        d[i + 2] = s[i + 0];
        d[i + 3] = s[i + 3];
    }
}

} // namespace

namespace {

/// MeshView's scene -> Mesh3D: transforms applied, so the viewer needs no matrices.
void ToMesh3D(const sro::Scene& scene, Mesh3D& out) {
    out = Mesh3D();
    out.textures.resize(scene.textures.size());
    for (std::size_t i = 0; i < scene.textures.size(); ++i) ToBgra(scene.textures[i], out.textures[i]);
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (const sro::RasterMesh& rm : scene.meshes) {
        if (rm.vertices.empty() || rm.indices.size() < 3) continue;
        Mesh3D::Part part;
        const std::size_t n = rm.vertices.size();
        part.pos.reserve(n * 3); part.nrm.reserve(n * 3); part.uv.reserve(n * 2);
        for (const sro::RasterVertex& v : rm.vertices) {
            const sro::Vec3 p = rm.transform.transformPoint(v.position);
            const sro::Vec3 q = rm.transform.transformVector(v.normal).normalized();
            part.pos.insert(part.pos.end(), {p.x, p.y, p.z});
            part.nrm.insert(part.nrm.end(), {q.x, q.y, q.z});
            part.uv.insert(part.uv.end(), {v.uv.x, v.uv.y});
            mn[0] = std::min(mn[0], p.x); mn[1] = std::min(mn[1], p.y); mn[2] = std::min(mn[2], p.z);
            mx[0] = std::max(mx[0], p.x); mx[1] = std::max(mx[1], p.y); mx[2] = std::max(mx[2], p.z);
        }
        part.idx.assign(rm.indices.begin(), rm.indices.end());
        part.tex = (rm.textureIndex >= 0 && static_cast<std::size_t>(rm.textureIndex) < out.textures.size()) ? rm.textureIndex : -1;
        part.alpha_test = rm.alphaTest;
        part.two_sided = rm.twoSided;
        part.blend = rm.blendAlpha;
        part.additive = rm.additive;
        part.layer = rm.vertexAlphaLayer;
        if (rm.vertexColors) {
            part.col.reserve(n * 4);
            for (const sro::RasterVertex& v : rm.vertices)
                part.col.insert(part.col.end(), {v.color.r / 255.0f, v.color.g / 255.0f, v.color.b / 255.0f, v.color.a / 255.0f});
        }
        out.parts.push_back(std::move(part));
    }
    if (!out.parts.empty())
        for (int k = 0; k < 3; ++k) { out.min[k] = mn[k]; out.max[k] = mx[k]; }
}

} // namespace

bool build_preview(const Archive* assets, const std::wstring& inner_path,
                   const std::vector<std::uint8_t>& bytes, int width, int height,
                   Preview& out, std::wstring& err, bool want_mesh) {
    out = Preview();
    err.clear();

    sro::AssetLoader loader;
    sro::IArchive*   archive = nullptr;
    std::unique_lock<std::recursive_mutex> lk(g_siblings, std::defer_lock);
    if (assets) {
        lk.lock();                       // the decoder reads siblings through the loader
        assets->p_->pointSiblings();
        loader  = assets->p_->loader();
        archive = assets->p_->ar.get();
    }

    sro::PreviewResult pr;
    sro::Scene scene;
    bool has_scene = false;
    const sro::Result r = want_mesh
        ? sro::BuildPreviewScene(sro::NormalizeInnerPath(inner_path), bytes.data(), bytes.size(), loader,
                                 archive, pr, scene, has_scene)
        : sro::BuildPreview(sro::NormalizeInnerPath(inner_path), bytes.data(), bytes.size(), width, height,
                            loader, archive, /*renderEffect*/ false, pr);
    if (!r.ok()) {
        err = r.text();
        return false;
    }
    if (has_scene) {
        ToMesh3D(scene, out.mesh);
        out.is_3d = !out.mesh.empty();
    }

    out.format  = pr.formatNameTr;
    out.summary = pr.infoTr;
    if (pr.textIsSummary && !pr.textTr.empty() && pr.infoTr.find(pr.textTr) == std::wstring::npos)
        out.summary += L"\n" + pr.textTr;
    if (!pr.detailsTr.empty()) out.summary += L"\n" + pr.detailsTr;
    if (!pr.textIsSummary) out.text = std::move(pr.textTr);

    ToBgra(pr.image, out.image);        // frames stay empty: renderEffect is off (BAK-2 owns .efp)
    if (!has_scene) out.is_3d = pr.rendered3d;
    if (pr.uiPages.size() > 1) {
        out.pages.resize(pr.uiPages.size());
        for (std::size_t i = 0; i < pr.uiPages.size(); ++i) ToBgra(pr.uiPages[i], out.pages[i]);
        out.page_labels = pr.uiPageLabels;
        out.page_labels.resize(out.pages.size());
    }
    out.generic = pr.kind == sro::PreviewKind::Hex;
    out.text_guess = pr.format == sro::FormatId::Text || pr.format == sro::FormatId::Csv ||
                     pr.format == sro::FormatId::Ini || pr.format == sro::FormatId::Lua ||
                     pr.format == sro::FormatId::Xml || pr.format == sro::FormatId::Sct;
    return true;
}

// ===========================================================================
// Effect
// ===========================================================================
struct Effect::Impl {
    sro::fx::Effect fx;
    FxSceneInfo     info;
};

Effect::Effect() : p_(std::make_unique<Impl>()) {}
Effect::~Effect() = default;

std::unique_ptr<Effect> Effect::load(const std::vector<std::uint8_t>& efp, std::wstring& err,
                                     const Archive* assets, const std::wstring& efp_path) {
    using namespace sro;
    err.clear();
    if (!IsEfp(efp.data(), efp.size())) {
        err = L("bb_fx_no_signature");
        return nullptr;
    }
    EfpFile file;
    ParseEfp(efp.data(), efp.size(), file);   // partial parses still build (BAK rule)

    std::unique_ptr<Effect> e(new Effect());
    Impl& m = *e->p_;
    const std::wstring base = PathParent(NormalizeInnerPath(efp_path));
    const Archive::Impl* src = assets ? assets->p_.get() : nullptr;

    // Authored path -> readable inner path: as written, then relative to the
    // .efp's folder (BAK PreviewPane LoadAssetBytes order).
    auto resolve = [&](const std::wstring& ref) -> std::wstring {
        const std::wstring direct = NormalizeInnerPath(ref);
        if (!src) return direct;
        if (src->exists(direct)) return direct;
        if (!base.empty()) {
            const std::wstring joined = NormalizeInnerPath(PathJoin(base, ref));
            if (src->exists(joined)) return joined;
        }
        return direct;
    };

    std::unordered_map<std::wstring, int> byPath, byName;
    auto addTexture = [&](const std::wstring& ref) -> int {
        const std::wstring key = ToLowerW(NormalizeInnerPath(ref));
        if (auto it = byPath.find(key); it != byPath.end()) return it->second;
        const int idx = static_cast<int>(m.info.textures.size());
        m.info.textures.push_back(resolve(ref));
        byPath.emplace(key, idx);
        byName.emplace(ToLowerW(PathFileName(key)), idx);   // first one wins, as in BAK
        return idx;
    };
    for (const std::wstring& ref : file.allResourcePaths()) {
        const std::wstring ext = PathExtension(ref);
        if (ext == L".ddj" || ext == L".dds") addTexture(ref);
    }

    std::function<bool(const std::wstring&, BmsFile&)> loadMesh;
    if (src) {
        loadMesh = [&](const std::wstring& ref, BmsFile& bms) -> bool {
            std::vector<u8> bytes;
            const std::wstring direct = NormalizeInnerPath(ref);
            bool got = !direct.empty() && src->readAny(direct, bytes) && !bytes.empty();
            if (!got && !base.empty())
                got = src->readAny(NormalizeInnerPath(PathJoin(base, ref)), bytes) && !bytes.empty();
            return got && ParseBms(bytes.data(), bytes.size(), bms, true).ok();
        };
    }

    m.fx.build(file, [&](const std::wstring& want) -> int {
        const std::wstring key = ToLowerW(NormalizeInnerPath(want));
        if (key.empty()) return -1;
        if (auto it = byPath.find(key); it != byPath.end()) return it->second;
        if (auto it = byName.find(ToLowerW(PathFileName(key))); it != byName.end()) return it->second;
        return addTexture(want);
    }, loadMesh);

    if (m.fx.empty()) {
        err = L("bb_fx_no_emitter");
        return nullptr;
    }
    m.info.duration        = m.fx.durationMs() / 1000.0f;
    m.info.emitters        = m.fx.emitterCount();
    m.info.radius          = m.fx.contentRadius();
    m.info.camera_distance = m.fx.cameraDistance();
    return e;
}

const FxSceneInfo& Effect::info() const { return p_->info; }

void Effect::sample(float t_seconds, const FxView& view, std::vector<FxBatch>& out) const {
    using namespace sro;
    out.clear();

    // Orthonormal camera basis (forward wins, up is re-derived).
    Vec3 fwd = Vec3(view.forward[0], view.forward[1], view.forward[2]).normalized();
    if (fwd.length() < 0.5f) fwd = Vec3(0, 0, 1);
    Vec3 right = Vec3(view.up[0], view.up[1], view.up[2]).cross(fwd).normalized();
    if (right.length() < 0.5f) right = Vec3(view.right[0], view.right[1], view.right[2]).normalized();
    if (right.length() < 0.5f) right = Vec3(1, 0, 0);
    const Vec3 up = fwd.cross(right).normalized();

    Camera cam;
    cam.target      = Vec3(0, 0, 0);
    cam.sceneRadius = p_->info.radius;
    cam.distance    = p_->info.camera_distance;
    cam.orient      = QuatFromBasis(right, up, fwd);

    Scene scene;
    p_->fx.emitScene(t_seconds * 1000.0f, cam, scene);

    out.reserve(scene.meshes.size());
    for (const RasterMesh& rm : scene.meshes) {
        FxBatch b;
        b.vertices.reserve(rm.vertices.size());
        for (const RasterVertex& v : rm.vertices) {
            // emitScene leaves every transform at identity; apply it anyway so
            // the bridge stays correct if that ever changes.
            const Vec3 p = rm.transform.transformPoint(v.position);
            b.vertices.push_back(FxVertex{ { p.x, p.y, p.z }, { v.uv.x, v.uv.y } });
        }
        b.indices   = rm.indices;
        b.texture   = rm.textureIndex;
        b.color[0]  = rm.tint.x;
        b.color[1]  = rm.tint.y;
        b.color[2]  = rm.tint.z;
        b.color[3]  = rm.opacity;
        b.src_blend = rm.srcBlend;
        b.dst_blend = rm.dstBlend;
        b.two_sided = rm.twoSided;
        out.push_back(std::move(b));
    }
}

// ===========================================================================
// .2dt widget table
// ===========================================================================
namespace {

enum class UiCell { Str, U32, I32, Colour };
struct UiCol {
    std::string_view key;                         // column caption (i18n key)
    UiCell kind;
    std::size_t off;                              // offset inside the 976-byte record (Nif2dt.h)
    std::size_t len;                              // string field capacity (NUL included)
    std::wstring sro::Nif2dtEntry::* str;         // BAK's decoded string for Str cells
};
constexpr UiCol kUiCols[] = {
    {"bb_ui_col_name",        UiCell::Str,    0,   sro::kNif2dtNameLen,        &sro::Nif2dtEntry::name},
    {"bb_ui_col_type",        UiCell::U32,    832, 4,                          nullptr},
    {"bb_ui_col_id",          UiCell::U32,    836, 4,                          nullptr},
    {"bb_ui_col_parent",      UiCell::U32,    840, 4,                          nullptr},
    {"bb_ui_col_x",           UiCell::I32,    860, 4,                          nullptr},
    {"bb_ui_col_y",           UiCell::I32,    864, 4,                          nullptr},
    {"bb_ui_col_width",       UiCell::I32,    868, 4,                          nullptr},
    {"bb_ui_col_height",      UiCell::I32,    872, 4,                          nullptr},
    {"bb_ui_col_colour",      UiCell::Colour, 856, 4,                          nullptr},
    {"bb_ui_col_style",       UiCell::U32,    972, 4,                          nullptr},
    {"bb_ui_col_text",        UiCell::Str,    576, sro::kNif2dtTextLen,        &sro::Nif2dtEntry::text},
    {"bb_ui_col_image",       UiCell::Str,    64,  sro::kNif2dtImageLen,       &sro::Nif2dtEntry::image},
    {"bb_ui_col_background",  UiCell::Str,    320, sro::kNif2dtBackgroundLen,  &sro::Nif2dtEntry::background},
    {"bb_ui_col_description", UiCell::Str,    704, sro::kNif2dtDescriptionLen, &sro::Nif2dtEntry::description},
};
constexpr std::size_t kUiRecord = sro::kNif2dtEntrySize;

bool ParseUi(const std::vector<std::uint8_t>& b, sro::Nif2dtFile& f) {
    if (b.size() < 4 || !sro::IsNif2dt(b.data(), b.size())) return false;
    std::uint32_t count = 0;
    std::memcpy(&count, b.data(), 4);
    if (count == 0 || 4 + static_cast<std::size_t>(count) * kUiRecord > b.size()) return false;
    return sro::ParseNif2dt(b.data(), b.size(), f).ok() && f.entries.size() == count;
}

std::wstring UiClean(std::wstring s) {
    for (wchar_t& c : s) if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return s;
}

std::wstring UiCellText(const sro::Nif2dtEntry& e, const std::uint8_t* rec, const UiCol& c) {
    std::uint32_t v = 0;
    if (c.kind != UiCell::Str) std::memcpy(&v, rec + c.off, 4);
    switch (c.kind) {
        case UiCell::Str:    return UiClean(e.*c.str);
        case UiCell::U32:    return std::to_wstring(v);
        case UiCell::I32:    return std::to_wstring(static_cast<std::int32_t>(v));
        case UiCell::Colour: return sro::FormatW(L"%08X", v);
    }
    return {};
}

std::wstring UiTrim(const std::wstring& s) {
    const std::size_t a = s.find_first_not_of(L" ");
    if (a == std::wstring::npos) return {};
    return s.substr(a, s.find_last_not_of(L" ") - a + 1);
}

// Client strings: ASCII as is, otherwise CP949 (Korean client) or CP1252, whichever holds the text exactly.
bool UiEncode(const std::wstring& s, std::string& out) {
    out.clear();
    if (std::all_of(s.begin(), s.end(), [](wchar_t c) { return c < 0x80; })) {
        for (wchar_t c : s) out.push_back(static_cast<char>(c));
        return true;
    }
    for (const UINT cp : {949u, 1252u}) {
        BOOL lossy = FALSE;
        const int n = ::WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, s.data(), static_cast<int>(s.size()),
                                            nullptr, 0, nullptr, &lossy);
        if (n <= 0 || lossy) continue;
        out.resize(static_cast<std::size_t>(n));
        ::WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, &lossy);
        return true;
    }
    return false;
}

bool UiNumber(const std::wstring& text, UiCell kind, std::uint32_t& out) {
    std::wstring s = UiTrim(text);
    int base = 10;
    if (kind == UiCell::Colour) {
        base = 16;
        if (!s.empty() && s[0] == L'#') s.erase(0, 1);
        else if (s.size() > 2 && s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) s.erase(0, 2);
        if (s.empty() || s.size() > 8) return false;
    }
    if (s.empty() || (kind != UiCell::I32 && s[0] == L'-') || s[0] == L'+') return false;
    wchar_t* end = nullptr;
    errno = 0;
    const long long v = std::wcstoll(s.c_str(), &end, base);
    if (errno != 0 || end != s.c_str() + s.size()) return false;
    if (kind == UiCell::I32 ? (v < INT32_MIN || v > INT32_MAX) : (v < 0 || v > static_cast<long long>(UINT32_MAX))) return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}

} // namespace

bool ui2dt_is(const std::vector<std::uint8_t>& bytes) {
    sro::Nif2dtFile f;
    return ParseUi(bytes, f);
}

std::wstring ui2dt_listing(const std::vector<std::uint8_t>& bytes) {
    sro::Nif2dtFile f;
    if (!ParseUi(bytes, f)) return {};
    // BAK's summary (counts, atlases, type histogram) without its 10-entry teaser: every widget follows.
    std::wstring out = f.summaryTr();
    if (const std::size_t cut = out.find(L"\n" + std::wstring(sro::T(L"Girdiler (ilk "))); cut != std::wstring::npos)
        out.resize(cut + 1);

    // Aligned columns: # Name Type Id Parent X Y W H Text Image.
    constexpr std::size_t shown_cols[] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11};
    constexpr std::size_t kMaxWidth = 34;
    std::vector<std::vector<std::wstring>> grid;
    std::vector<std::wstring> head{L"#"};
    for (std::size_t c : shown_cols) head.push_back(L(kUiCols[c].key));
    grid.push_back(std::move(head));
    for (std::size_t i = 0; i < f.entries.size(); ++i) {
        const std::uint8_t* rec = bytes.data() + 4 + i * kUiRecord;
        std::vector<std::wstring> row{std::to_wstring(i + 1)};
        for (std::size_t c : shown_cols) {
            std::wstring s = UiCellText(f.entries[i], rec, kUiCols[c]);
            if (s.size() > kMaxWidth) s = s.substr(0, kMaxWidth - 1) + L"…";
            row.push_back(s.empty() ? L"-" : std::move(s));
        }
        grid.push_back(std::move(row));
    }
    std::vector<std::size_t> width(grid[0].size(), 0);
    for (const auto& row : grid)
        for (std::size_t c = 0; c < row.size(); ++c) width[c] = (std::max)(width[c], row[c].size());
    out += L"\n" + Lf("bb_ui_widgets", f.entries.size()) + L"\n";
    for (const auto& row : grid) {
        std::wstring line = L"  ";
        for (std::size_t c = 0; c < row.size(); ++c) {
            line += row[c];
            if (c + 1 < row.size()) line.append(width[c] - row[c].size() + 2, L' ');
        }
        out += line + L"\n";
    }
    return out;
}

std::wstring ui2dt_table(const std::vector<std::uint8_t>& bytes) {
    sro::Nif2dtFile f;
    if (!ParseUi(bytes, f)) return {};
    std::wstring out = L"#";
    for (const UiCol& c : kUiCols) out += L"\t" + L(c.key);
    out += L"\r\n";
    for (std::size_t i = 0; i < f.entries.size(); ++i) {
        const std::uint8_t* rec = bytes.data() + 4 + i * kUiRecord;
        out += std::to_wstring(i + 1);
        for (const UiCol& c : kUiCols) out += L"\t" + UiCellText(f.entries[i], rec, c);
        out += L"\r\n";
    }
    return out;
}

bool ui2dt_apply(std::vector<std::uint8_t>& bytes, const std::wstring& table, std::size_t& changed,
                 std::wstring& err) {
    changed = 0;
    sro::Nif2dtFile f;
    if (!ParseUi(bytes, f)) { err = L("bb_ui_err_not_2dt"); return false; }
    std::vector<std::uint8_t> out = bytes;
    std::size_t row = 0, line_no = 0, pos = 0;
    while (pos <= table.size()) {
        const std::size_t eol = table.find(L'\n', pos);
        std::wstring line = table.substr(pos, eol == std::wstring::npos ? std::wstring::npos : eol - pos);
        pos = eol == std::wstring::npos ? table.size() + 1 : eol + 1;
        ++line_no;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.find_first_not_of(L" \t") == std::wstring::npos || line[0] == L'#') continue;   // blank / header

        std::vector<std::wstring> cells;
        for (std::size_t a = 0;;) {
            const std::size_t t = line.find(L'\t', a);
            cells.push_back(line.substr(a, t == std::wstring::npos ? std::wstring::npos : t - a));
            if (t == std::wstring::npos) break;
            a = t + 1;
        }
        auto where = [&](std::size_t col) {
            const std::wstring name = col == 0 ? std::wstring(L"#") : L(kUiCols[col - 1].key);
            return Lf("bb_ui_err_where", line_no, name);
        };
        if (cells.size() != std::size(kUiCols) + 1) {
            err = Lf("bb_ui_err_columns", line_no, std::size(kUiCols) + 1, cells.size());
            return false;
        }
        if (row >= f.entries.size()) {
            err = Lf("bb_ui_err_rows_added", line_no, f.entries.size());
            return false;
        }
        if (UiTrim(cells[0]) != std::to_wstring(row + 1)) {
            err = where(0) + Lf("bb_ui_err_row_number", row + 1);
            return false;
        }
        const std::uint8_t* orig = bytes.data() + 4 + row * kUiRecord;
        std::uint8_t* rec = out.data() + 4 + row * kUiRecord;
        for (std::size_t c = 0; c < std::size(kUiCols); ++c) {
            const UiCol& col = kUiCols[c];
            const std::wstring& value = cells[c + 1];
            if (value == UiCellText(f.entries[row], orig, col)) continue;   // untouched: keep the bytes
            if (col.kind == UiCell::Str) {
                std::string enc;
                if (!UiEncode(value, enc)) {
                    err = where(c + 1) + L("bb_ui_err_chars");
                    return false;
                }
                if (enc.size() >= col.len) {
                    err = where(c + 1) + Lf("bb_ui_err_too_long", col.len - 1);
                    return false;
                }
                std::memset(rec + col.off, 0, col.len);
                std::memcpy(rec + col.off, enc.data(), enc.size());
            } else {
                std::uint32_t v = 0;
                if (!UiNumber(value, col.kind, v)) {
                    err = where(c + 1) + L(col.kind == UiCell::Colour ? "bb_ui_err_colour" : "bb_ui_err_number");
                    return false;
                }
                std::memcpy(rec + col.off, &v, 4);
            }
            ++changed;
        }
        ++row;
    }
    if (row != f.entries.size()) {
        err = Lf("bb_ui_err_row_count", f.entries.size(), row);
        return false;
    }
    bytes.swap(out);
    return true;
}

// ===========================================================================
// Language of BAK's own texts
// ===========================================================================
void set_ui_language(std::wstring_view tag) {
    sro::Lang lang = sro::Lang::EN;
    if (!sro::LangFromTag(std::wstring(tag), lang)) lang = sro::Lang::EN;   // BAK's universal fallback
    sro::SetLang(lang);
}

} // namespace bakb
