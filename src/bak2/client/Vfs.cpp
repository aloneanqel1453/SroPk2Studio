// SkillEffect Studio v3 - client/Vfs.cpp
#include "Vfs.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace se::client {

namespace {

constexpr const wchar_t* kTd = L"server_dep\\silkroad\\textdata";

std::wstring Lower(std::wstring_view s) {
    std::wstring r(s);
    if (!r.empty()) CharLowerBuffW(r.data(), static_cast<DWORD>(r.size()));
    return r;
}

bool IStarts(std::wstring_view s, std::wstring_view p) {
    return s.size() >= p.size() && Lower(s.substr(0, p.size())) == Lower(p);
}

bool IsDir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool FileInfo(const std::wstring& p, std::uint64_t* size, std::uint64_t* mtime) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    if (size) *size = (static_cast<std::uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    if (mtime) *mtime = (static_cast<std::uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    return true;
}

void ListDir(const std::wstring& dir, std::vector<VfsItem>& out, Source src, bool after) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        VfsItem it;
        it.name = n;
        it.folder = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        it.size = (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        it.src = src;
        it.after = after;
        out.push_back(std::move(it));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// every file below dir (relative paths); bounded
void WalkFiles(const std::wstring& dir, const std::wstring& prefix, std::vector<std::wstring>& out, size_t cap, int depth = 0) {
    if (out.size() >= cap || depth > 24) return;
    std::vector<VfsItem> items;
    ListDir(dir, items, Source::None, false);
    for (const auto& it : items) {
        if (out.size() >= cap) return;
        const std::wstring rel = prefix.empty() ? it.name : prefix + L"\\" + it.name;
        if (it.folder) WalkFiles(JoinPath(dir, it.name), rel, out, cap, depth + 1);
        else out.push_back(rel);
    }
}

bool HasPk2(const std::wstring& dir) {
    for (const wchar_t* n : {L"Media.pk2", L"Data.pk2", L"Particles.pk2"})
        if (!FindChildI(dir, n, false).empty()) return true;
    return false;
}

bool LooksTextdataDir(const std::wstring& dir) {
    if (EndsWithI(dir, L"\\textdata") || EndsWithI(dir, L"\\textdata\\")) return true;
    for (const wchar_t* n : {L"skilleffect.txt", L"characterdata.txt", L"skilldataenc.txt", L"textdata_object.txt", L"itemdata.txt"})
        if (!FindChildI(dir, n, false).empty()) return true;
    return false;
}

std::wstring StripTrailing(std::wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

std::wstring ExtOf(const std::wstring& p) {
    const size_t d = p.find_last_of(L'.');
    const size_t s = p.find_last_of(L"\\/");
    if (d == std::wstring::npos || (s != std::wstring::npos && d < s)) return {};
    return Lower(p.substr(d));
}

}  // namespace

// ------------------------------------------------------------------------------------------------ helpers
const wchar_t* ArcFileName(ArcKind a) {
    switch (a) {
        case ArcKind::Media: return L"Media.pk2";
        case ArcKind::Data: return L"Data.pk2";
        case ArcKind::Particles: return L"Particles.pk2";
        case ArcKind::Map: return L"Map.pk2";
        case ArcKind::Music: return L"Music.pk2";
    }
    return L"?";
}

const wchar_t* ArcFolderName(ArcKind a) {
    switch (a) {
        case ArcKind::Media: return L"Media";
        case ArcKind::Data: return L"Data";
        case ArcKind::Particles: return L"Particles";
        case ArcKind::Map: return L"Map";
        case ArcKind::Music: return L"Music";
    }
    return L"?";
}

const wchar_t* SourceNameTr(Source s) {
    switch (s) {
        case Source::Staged: return L"bekleyen (bellekte)";
        case Source::OverlayFolder: return L"çıktı klasörü";
        case Source::OverlayPk2: return L"pk2 kopyası";
        case Source::Pk2: return L"pk2";
        case Source::LooseFolder: return L"klasör";
        case Source::TextdataFolder: return L"textdata klasörü";
        default: return L"-";
    }
}

const wchar_t* SourceNameEn(Source s) {
    switch (s) {
        case Source::Staged: return L"pending (memory)";
        case Source::OverlayFolder: return L"output folder";
        case Source::OverlayPk2: return L"pk2 copy";
        case Source::Pk2: return L"pk2";
        case Source::LooseFolder: return L"folder";
        case Source::TextdataFolder: return L"textdata folder";
        default: return L"-";
    }
}

std::wstring FindChildI(const std::wstring& dir, const std::wstring& name, bool wantDir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring hit;
    const std::wstring want = Lower(name);
    do {
        const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir != wantDir) continue;
        if (Lower(fd.cFileName) == want) {
            hit = fd.cFileName;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return hit;
}

bool GlobMatchI(std::wstring_view pattern, std::wstring_view text) {
    const std::wstring p = Lower(pattern), t = Lower(text);
    size_t pi = 0, ti = 0, star = std::wstring::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == L'?' || p[pi] == t[ti])) {
            ++pi;
            ++ti;
        } else if (pi < p.size() && p[pi] == L'*') {
            star = pi++;
            mark = ti;
        } else if (star != std::wstring::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == L'*') ++pi;
    return pi == p.size();
}

std::wstring Vfs::Norm(std::wstring_view inner) {
    std::wstring r;
    r.reserve(inner.size());
    for (wchar_t c : inner) {
        if (c == L'/') c = L'\\';
        if (c == L'\\' && (r.empty() || r.back() == L'\\')) continue;
        r += c;
    }
    while (r.size() >= 2 && r[0] == L'.' && r[1] == L'\\') r.erase(0, 2);
    while (!r.empty() && r.back() == L'\\') r.pop_back();
    return r;
}

const wchar_t* Vfs::TextdataDir() { return kTd; }

// ------------------------------------------------------------------------------------------------ layers
struct Vfs::Layer {
    Source src = Source::None;
    ArcKind arc = ArcKind::Media;
    bool after = false;
    std::wstring dir;                   // folder layers
    std::shared_ptr<Pk2Archive> pk2;    // pk2 layers
};

Vfs::Vfs() = default;
Vfs::~Vfs() { Close(); }

void Vfs::Close() {
    std::unique_lock lk(m_mx);
    m_layers.clear();
    m_staged.clear();
    m_overlays.clear();
    m_problems.clear();
    m_kind = OpenedAs::None;
    m_root.clear();
    m_opened.clear();
    if (!m_keySpec.empty()) SecureZeroMemory(m_keySpec.data(), m_keySpec.size());
    m_keySpec.clear();
    for (auto& a : m_arc) a = ArcState{};
    m_view = View::Before;
}

bool Vfs::IsOpen() const {
    std::shared_lock lk(m_mx);
    return !m_layers.empty();
}

void Vfs::AddProblem(Problem::Level lv, const wchar_t* code, std::wstring tr, std::wstring en) {
    Problem p;
    p.level = lv;
    p.code = code;
    p.tr = std::move(tr);
    p.en = std::move(en);
    m_problems.push_back(std::move(p));
}

int Vfs::ErrorCount() const {
    int n = 0;
    for (const auto& p : m_problems) n += p.level == Problem::Error;
    return n;
}

ArcState Vfs::Arc(ArcKind a) const {
    std::shared_lock lk(m_mx);
    return m_arc[static_cast<int>(a)];
}

std::shared_ptr<const Pk2Archive> Vfs::Archive(ArcKind a) const {
    std::shared_lock lk(m_mx);
    for (const auto& L : m_layers)
        if (!L->after && L->arc == a && L->pk2) return L->pk2;
    return nullptr;
}

bool Vfs::LayerRead(const Layer& L, const std::wstring& inner, Bytes* out, std::uint64_t* size) const {
    if (L.pk2) {
        if (out) {
            if (!L.pk2->Read(inner, *out)) return false;
            if (size) *size = out->size();
            return true;
        }
        return L.pk2->Stat(inner, size);
    }
    std::wstring file;
    if (L.src == Source::TextdataFolder) {
        const std::wstring td = std::wstring(kTd) + L"\\";
        if (!IStarts(inner, td)) return false;
        const std::wstring rest = inner.substr(td.size());
        if (rest.empty() || rest.find(L'\\') != std::wstring::npos) return false;
        file = JoinPath(L.dir, rest);
    } else {
        file = JoinPath(L.dir, inner);
    }
    std::uint64_t sz = 0;
    if (!FileInfo(file, &sz, nullptr)) return false;
    if (size) *size = sz;
    if (out) {
        std::wstring e;
        if (!ReadFileBytes(file, *out, e)) return false;
    }
    return true;
}

bool Vfs::LayerList(const Layer& L, const std::wstring& dir, std::vector<VfsItem>& out) const {
    if (L.pk2) {
        std::vector<DirItem> items;
        if (!L.pk2->List(dir, items)) return false;
        for (auto& d : items) out.push_back({std::move(d.name), d.folder, d.size, L.src, L.after});
        return true;
    }
    if (L.src == Source::TextdataFolder) {
        // virtual tree: server_dep\silkroad\textdata\<files>
        const std::wstring ld = Lower(dir);
        if (ld.empty()) {
            out.push_back({L"server_dep", true, 0, L.src, L.after});
            return true;
        }
        if (ld == L"server_dep") {
            out.push_back({L"silkroad", true, 0, L.src, L.after});
            return true;
        }
        if (ld == L"server_dep\\silkroad") {
            out.push_back({L"textdata", true, 0, L.src, L.after});
            return true;
        }
        if (ld != Lower(kTd)) return false;
        std::vector<VfsItem> items;
        ListDir(L.dir, items, L.src, L.after);
        for (auto& it : items)
            if (!it.folder) out.push_back(std::move(it));
        return true;
    }
    const std::wstring full = dir.empty() ? L.dir : JoinPath(L.dir, dir);
    if (!IsDir(full)) return false;
    ListDir(full, out, L.src, L.after);
    return true;
}

// ------------------------------------------------------------------------------------------------ open
bool Vfs::Open(const std::wstring& pathIn, const VfsOptions& opt) {
    Close();
    std::unique_lock lk(m_mx);
    const std::wstring path = StripTrailing(pathIn);
    m_opened = path;
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (path.empty() || attr == INVALID_FILE_ATTRIBUTES) {
        AddProblem(Problem::Error, L"NO_PATH", L"Seçilen klasör / dosya bulunamadı: " + path,
                   L"The selected folder / file does not exist: " + path);
        return false;
    }
    std::wstring forcedMedia;  // an extracted Media folder the user picked: read in front of the client's Media
    std::wstring textdataDir;
    std::wstring root;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        const std::wstring ext = ExtOf(path);
        if (ext == L".pk2" || ext == L".lib") {
            m_kind = OpenedAs::Pk2File;
            root = DirOf(path);
        } else if (ext == L".txt") {
            m_kind = OpenedAs::TextdataFolder;
            textdataDir = DirOf(path);
        } else {
            AddProblem(Problem::Error, L"NOT_SUPPORTED",
                       L"Bu dosya türü açılamaz: " + FileNameOf(path) +
                           L". Bir istemci klasörü, bir .pk2 dosyası, bir Media / textdata klasörü ya da bir textdata .txt seçin.",
                       L"Unsupported file: " + FileNameOf(path) +
                           L". Pick a client folder, a .pk2, a Media / textdata folder or a textdata .txt.");
            return false;
        }
    } else if (HasPk2(path)) {
        m_kind = OpenedAs::ClientFolder;
        root = path;
    } else if (!FindChildI(path, L"server_dep", true).empty()) {
        m_kind = OpenedAs::MediaFolder;
        forcedMedia = path;
        const std::wstring parent = DirOf(path);
        if (!parent.empty() && parent != path && (HasPk2(parent) || !FindChildI(parent, L"Data", true).empty() ||
                                                  !FindChildI(parent, L"Particles", true).empty()))
            root = parent;
    } else if (LooksTextdataDir(path)) {
        // ...\Media\server_dep\silkroad\textdata -> the Media folder
        if (EndsWithI(path, L"\\server_dep\\silkroad\\textdata")) {
            const std::wstring media = path.substr(0, path.size() - std::wstring(L"\\server_dep\\silkroad\\textdata").size());
            m_kind = OpenedAs::MediaFolder;
            forcedMedia = media;
            const std::wstring parent = DirOf(media);
            if (!parent.empty() && (HasPk2(parent) || !FindChildI(parent, L"Data", true).empty())) root = parent;
        } else {
            m_kind = OpenedAs::TextdataFolder;
            textdataDir = path;
        }
    } else if (!FindChildI(path, L"Media", true).empty() || !FindChildI(path, L"Data", true).empty() ||
               !FindChildI(path, L"Particles", true).empty()) {
        m_kind = OpenedAs::LooseRoot;
        root = path;
    } else {
        // one level down: a parent folder that holds a client
        std::vector<std::wstring> cands;
        std::vector<VfsItem> kids;
        ListDir(path, kids, Source::None, false);
        for (const auto& k : kids) {
            if (!k.folder) continue;
            const std::wstring c = JoinPath(path, k.name);
            if (HasPk2(c)) cands.push_back(c);
        }
        if (cands.size() == 1) {
            m_kind = OpenedAs::ClientFolder;
            root = cands[0];
            AddProblem(Problem::Info, L"SUBFOLDER", L"İstemci alt klasörde bulundu ve o açıldı: " + cands[0],
                       L"The client was found in a subfolder and opened: " + cands[0]);
        } else if (cands.size() > 1) {
            std::wstring names;
            for (size_t i = 0; i < cands.size() && i < 8; ++i) names += (i ? L", " : L"") + FileNameOf(cands[i]);
            AddProblem(Problem::Error, L"MANY_CLIENTS",
                       L"Bu klasörde birden çok istemci var (" + names + L"). Hangisini açacağınızı seçin.",
                       L"This folder holds several clients (" + names + L"). Pick one.");
            return false;
        } else {
            AddProblem(Problem::Error, L"NO_CLIENT",
                       L"Bu klasörde Silkroad istemcisi bulunamadı: Media.pk2 / Data.pk2 / Particles.pk2 yok, Media / Data / "
                       L"Particles klasörü yok, textdata dosyası yok. İstemcinin kurulu olduğu klasörü (sro_client.exe'nin "
                       L"yanı) seçin.",
                       L"No Silkroad client here: no Media/Data/Particles .pk2, no loose Media/Data/Particles folder and no "
                       L"textdata file. Pick the folder the client is installed in.");
            return false;
        }
    }

    if (!textdataDir.empty()) {
        auto L = std::make_shared<Layer>();
        L->src = Source::TextdataFolder;
        L->arc = ArcKind::Media;
        L->dir = textdataDir;
        m_layers.push_back(L);
        m_arc[0] = {true, Source::TextdataFolder, textdataDir, L"", false};
        AddProblem(Problem::Info, L"TEXTDATA_ONLY",
                   L"Yalnız textdata açıldı: modeller, efektler ve ikonlar gösterilemez (Data.pk2 / Particles.pk2 / Media.pk2 yok).",
                   L"Textdata only: models, effects and icons cannot be shown (no Data / Particles / Media archive).");
    } else if (!root.empty()) {
        m_root = root;
        lk.unlock();
        OpenClientFolder(root, opt);
        lk.lock();
    }
    if (!forcedMedia.empty()) {
        auto L = std::make_shared<Layer>();
        L->src = Source::LooseFolder;
        L->arc = ArcKind::Media;
        L->dir = forcedMedia;
        m_layers.insert(m_layers.begin(), L);
        m_arc[0].present = true;
        m_arc[0].src = Source::LooseFolder;
        m_arc[0].path = forcedMedia;
        AddProblem(Problem::Info, L"MEDIA_FOLDER",
                   L"Seçilen Media klasörü okunuyor" + std::wstring(m_root.empty() ? L"" : L" (Media.pk2'nin önünde)") + L": " +
                       forcedMedia,
                   L"Reading the selected Media folder" + std::wstring(m_root.empty() ? L"" : L" (in front of Media.pk2)") +
                       L": " + forcedMedia);
    }
    for (const auto& o : opt.overlays) {
        lk.unlock();
        std::wstring why;
        if (!AddOverlay(o, &why))
            AddProblem(Problem::Warn, L"OVERLAY_FAIL", why, L"Overlay folder not usable: " + o);
        lk.lock();
    }
    // textdata essentials
    if (m_arc[0].present) {
        lk.unlock();
        struct Need {
            const wchar_t* a;
            const wchar_t* b;
            const wchar_t* tr;
        };
        const Need needs[] = {
            {L"characterdata.txt", nullptr, L"canavar / karakter listesi"},
            {L"skilldataenc.txt", L"skilldata.txt", L"skill listesi"},
            {L"skilleffect.txt", nullptr, L"skill animasyon / efekt tablosu"},
            {L"textdata_object.txt", L"textdataname.txt", L"nesne adları"},
            {L"itemdata.txt", nullptr, L"eşya listesi"},
        };
        std::vector<Problem> add;
        for (const auto& n : needs) {
            if (HasTextdata(n.a) || (n.b && HasTextdata(n.b))) continue;
            // split-only clients (skilleffect_100.txt ...) are fine
            std::wstring base(n.a);
            base = base.substr(0, base.size() - 4);
            std::vector<Problem> tmp;
            if (!TextdataParts(base, &tmp).empty()) continue;
            Problem p;
            p.level = Problem::Warn;
            p.code = L"TEXTDATA_MISSING";
            p.tr = std::wstring(L"textdata dosyası yok: ") + n.a + L" (" + n.tr + L" okunamaz)";
            p.en = std::wstring(L"textdata file missing: ") + n.a;
            add.push_back(std::move(p));
        }
        lk.lock();
        for (auto& p : add) m_problems.push_back(std::move(p));
    }
    const bool any = !m_layers.empty();
    if (!any && m_problems.empty())
        AddProblem(Problem::Error, L"NOTHING", L"İstemciden okunabilir hiçbir şey bulunamadı.", L"Nothing readable was found.");
    return any;
}

bool Vfs::OpenClientFolder(const std::wstring& root, const VfsOptions& opt) {
    std::vector<std::string> found;  // keys that opened archives of this client (sibling stage)
    std::vector<std::shared_ptr<Layer>> layers;
    std::vector<Problem> probs;
    auto prob = [&](Problem::Level lv, const wchar_t* code, std::wstring tr, std::wstring en) {
        probs.push_back({lv, code, std::move(tr), std::move(en)});
    };
    ArcState st[kArcKinds];
    std::string firstKey;
    for (int ai = 0; ai < kArcKinds; ++ai) {
        const ArcKind a = static_cast<ArcKind>(ai);
        if (a == ArcKind::Map && !opt.openMap) continue;
        if (a == ArcKind::Music && !opt.openMusic) continue;
        if (opt.cancel && opt.cancel->load()) break;
        if (opt.progress) opt.progress(ai * 18, std::wstring(L"Açılıyor: ") + ArcFileName(a));
        const std::wstring pkName = FindChildI(root, ArcFileName(a), false);
        std::wstring looseName = FindChildI(root, ArcFolderName(a), true);
        std::wstring looseDir = looseName.empty() ? L"" : JoinPath(root, looseName);
        if (a == ArcKind::Media && !looseDir.empty() && FindChildI(looseDir, L"server_dep", true).empty() &&
            FindChildI(looseDir, L"icon", true).empty())
            looseDir.clear();  // a "media" folder without server_dep / icon is not an extracted Media
        std::shared_ptr<Pk2Archive> arc;
        if (!pkName.empty()) {
            const std::wstring pkPath = JoinPath(root, pkName);
            KeyFindOptions ko;
            ko.typed = opt.key;
            ko.extra = found;
            ko.readHintFiles = opt.autoKey;
            ko.scanBinaries = opt.autoKey && opt.scanBinaries;
            ko.cancel = opt.cancel;
            if (opt.progress)
                ko.progress = [&](int p, const std::wstring& w) { opt.progress(ai * 18 + p * 18 / 100, w); };
            KeyReport kr;
            if (!opt.autoKey) {
                Pk2Probe pr;
                const std::string spec = opt.key.empty() ? "169841" : opt.key;
                if (pr.Load(pkPath) && pr.TestSpec(spec)) {
                    kr.found = true;
                    kr.spec = spec;
                    kr.stage = 1;
                    kr.howTr = L"yazılan anahtar";
                } else {
                    kr.whyTr = pr.ok ? L"Yazılan pk2 anahtarı " + pkName + L" arşivini açmıyor (otomatik anahtar bulma kapalı)."
                                     : pr.whyTr;
                    kr.whyEn = pr.ok ? L"The typed pk2 key does not open " + pkName + L" (automatic key search is off)." : pr.whyEn;
                }
            } else {
                kr = FindKey(pkPath, ko);
            }
            if (kr.found) {
                auto p = std::make_shared<Pk2Archive>();
                std::wstring wt, we;
                if (p->Open(pkPath, kr.spec, wt, we)) {
                    arc = p;
                    if (std::find(found.begin(), found.end(), kr.spec) == found.end()) found.push_back(kr.spec);
                    if (firstKey.empty()) firstKey = kr.spec;
                    st[ai].keyFound = true;
                    st[ai].keyHowTr = kr.howTr;
                    st[ai].keyHowEn = kr.howEn;
                    const bool typedOk = !opt.key.empty() && kr.spec == opt.key;
                    if (kr.stage == 2 || (!opt.key.empty() && !typedOk && kr.stage >= 1 && !kr.unencrypted))
                        prob(Problem::Info, L"KEY_FOUND",
                             std::wstring(ArcFileName(a)) + L": " +
                                 (opt.key.empty() || typedOk ? L"" : L"yazılan anahtar açmadı; ") + L"anahtar bulundu (" +
                                 kr.howTr + L").",
                             std::wstring(ArcFileName(a)) + L": key found (" + kr.howEn + L").");
                } else {
                    prob(Problem::Error, L"PK2_FAIL", wt, we);
                }
            } else {
                prob(Problem::Error, kr.cancelled ? L"CANCELLED" : L"KEY_FAIL", kr.whyTr, kr.whyEn);
            }
        }
        // layer order for this archive
        auto addLoose = [&] {
            auto L = std::make_shared<Layer>();
            L->src = Source::LooseFolder;
            L->arc = a;
            L->dir = looseDir;
            layers.push_back(L);
        };
        auto addPk2 = [&] {
            auto L = std::make_shared<Layer>();
            L->src = Source::Pk2;
            L->arc = a;
            L->pk2 = arc;
            layers.push_back(L);
        };
        if (opt.loose == LooseMode::Front && !looseDir.empty()) addLoose();
        if (arc) addPk2();
        if (opt.loose == LooseMode::Fallback && !arc && !looseDir.empty()) addLoose();
        if (arc) {
            st[ai].present = true;
            st[ai].src = Source::Pk2;
            st[ai].path = JoinPath(root, pkName);
            if (opt.loose == LooseMode::Front && !looseDir.empty()) {
                st[ai].src = Source::LooseFolder;
                prob(Problem::Info, L"LOOSE_FRONT",
                     std::wstring(L"'") + looseName + L"' klasörü " + ArcFileName(a) + L" arşivinin önünde okunuyor.",
                     std::wstring(L"Folder '") + looseName + L"' is read in front of " + ArcFileName(a) + L".");
            } else if (!looseDir.empty() && opt.loose == LooseMode::Fallback) {
                prob(Problem::Info, L"LOOSE_IGNORED",
                     std::wstring(L"'") + looseName + L"' klasörü var ama oyun " + ArcFileName(a) +
                         L" arşivini okur; klasör kullanılmıyor (istenirse 'klasörleri öne al').",
                     std::wstring(L"Folder '") + looseName + L"' exists but the game reads " + ArcFileName(a) +
                         L"; the folder is not used (option: loose folders in front).");
            }
        } else if (!looseDir.empty() && opt.loose != LooseMode::Ignore) {
            st[ai].present = true;
            st[ai].src = Source::LooseFolder;
            st[ai].path = looseDir;
            prob(Problem::Info, L"LOOSE_USED",
                 std::wstring(ArcFileName(a)) + L" yerine '" + looseName + L"' klasörü okunuyor.",
                 std::wstring(L"Reading folder '") + looseName + L"' instead of " + ArcFileName(a) + L".");
        } else {
            switch (a) {
                case ArcKind::Media:
                    if (pkName.empty())
                        prob(Problem::Error, L"NO_MEDIA",
                             L"Media.pk2 yok: adlar, skill tabloları, skilleffect ve ikonlar okunamaz. Yalnız modeller / "
                             L"efektler gezilebilir.",
                             L"No Media.pk2: names, skill tables, skilleffect and icons are unavailable.");
                    break;
                case ArcKind::Data:
                    if (pkName.empty())
                        prob(Problem::Warn, L"NO_DATA",
                             L"Data.pk2 yok: modeller (.bsr / .bms), dokular ve animasyonlar gösterilemez.",
                             L"No Data.pk2: models, textures and animations cannot be shown.");
                    break;
                case ArcKind::Particles:
                    if (pkName.empty())
                        prob(Problem::Warn, L"NO_PARTICLES", L"Particles.pk2 yok: skill efektleri (.efp) gösterilemez.",
                             L"No Particles.pk2: skill effects (.efp) cannot be shown.");
                    break;
                case ArcKind::Map:
                    if (pkName.empty())
                        prob(Problem::Info, L"NO_MAP", L"Map.pk2 yok: harita / arazi gösterilemez (skill önizlemesi etkilenmez).",
                             L"No Map.pk2: terrain is unavailable (skill preview is not affected).");
                    break;
                case ArcKind::Music: break;
            }
        }
    }
    std::unique_lock lk(m_mx);
    for (auto& L : layers) m_layers.push_back(L);
    for (int i = 0; i < kArcKinds; ++i)
        if (st[i].present || !m_arc[i].present) m_arc[i] = st[i];
    for (auto& p : probs) m_problems.push_back(std::move(p));
    m_keySpec = firstKey;
    if (opt.progress) opt.progress(100, L"Hazır");
    return !layers.empty();
}

// ------------------------------------------------------------------------------------------------ overlays / staging
bool Vfs::AddOverlay(const std::wstring& dirIn, std::wstring* whyTr) {
    const std::wstring dir = StripTrailing(dirIn);
    if (!IsDir(dir)) {
        if (whyTr) *whyTr = L"Çıktı klasörü bulunamadı: " + dir;
        return false;
    }
    std::vector<std::shared_ptr<Layer>> add;
    std::string key;
    {
        std::shared_lock lk(m_mx);
        key = m_keySpec;
    }
    for (int ai = 0; ai < kArcKinds; ++ai) {
        const ArcKind a = static_cast<ArcKind>(ai);
        const std::wstring sub = FindChildI(dir, ArcFolderName(a), true);
        if (!sub.empty()) {
            auto L = std::make_shared<Layer>();
            L->src = Source::OverlayFolder;
            L->arc = a;
            L->after = true;
            L->dir = JoinPath(dir, sub);
            add.push_back(L);
        }
        const std::wstring pk = FindChildI(dir, ArcFileName(a), false);
        if (!pk.empty()) {
            auto p = std::make_shared<Pk2Archive>();
            std::wstring wt, we;
            std::string spec = key;
            if (spec.empty() || !p->Open(JoinPath(dir, pk), spec, wt, we)) {
                KeyFindOptions ko;
                ko.typed = key;
                ko.scanBinaries = false;
                const KeyReport kr = FindKey(JoinPath(dir, pk), ko);
                if (!kr.found || !p->Open(JoinPath(dir, pk), kr.spec, wt, we)) {
                    if (whyTr) *whyTr = L"Çıktı klasöründeki pk2 kopyası açılamadı: " + pk + L" — " + (kr.found ? wt : kr.whyTr);
                    continue;
                }
            }
            auto L = std::make_shared<Layer>();
            L->src = Source::OverlayPk2;
            L->arc = a;
            L->after = true;
            L->pk2 = p;
            add.push_back(L);
        }
    }
    if (add.empty()) {
        if (!FindChildI(dir, L"server_dep", true).empty()) {
            auto L = std::make_shared<Layer>();
            L->src = Source::OverlayFolder;
            L->arc = ArcKind::Media;
            L->after = true;
            L->dir = dir;
            add.push_back(L);
        } else if (LooksTextdataDir(dir)) {
            auto L = std::make_shared<Layer>();
            L->src = Source::TextdataFolder;
            L->arc = ArcKind::Media;
            L->after = true;
            L->dir = dir;
            add.push_back(L);
        }
    }
    if (add.empty()) {
        if (whyTr && whyTr->empty())
            *whyTr = L"Bu klasör bir çıktı klasörü değil: içinde Media / Data / Particles klasörü ya da pk2 kopyası yok: " + dir;
        return false;
    }
    std::unique_lock lk(m_mx);
    // keep the pk2-copy layers behind the folder layers of the same overlay (folders are newer)
    std::stable_sort(add.begin(), add.end(), [](const auto& x, const auto& y) {
        return (x->src == Source::OverlayFolder || x->src == Source::TextdataFolder) && y->src == Source::OverlayPk2;
    });
    m_layers.insert(m_layers.begin(), add.begin(), add.end());
    m_overlays.insert(m_overlays.begin(), dir);
    return true;
}

void Vfs::ClearOverlays() {
    std::unique_lock lk(m_mx);
    m_layers.erase(std::remove_if(m_layers.begin(), m_layers.end(), [](const auto& L) { return L->after; }), m_layers.end());
    m_overlays.clear();
}

std::vector<std::wstring> Vfs::Overlays() const {
    std::shared_lock lk(m_mx);
    return m_overlays;
}

void Vfs::Stage(ArcKind a, std::wstring_view inner, Bytes data) {
    std::unique_lock lk(m_mx);
    m_staged[{static_cast<int>(a), Lower(Norm(inner))}] = std::move(data);
}

bool Vfs::Unstage(ArcKind a, std::wstring_view inner) {
    std::unique_lock lk(m_mx);
    return m_staged.erase({static_cast<int>(a), Lower(Norm(inner))}) > 0;
}

void Vfs::ClearStaged() {
    std::unique_lock lk(m_mx);
    m_staged.clear();
}

size_t Vfs::StagedCount() const {
    std::shared_lock lk(m_mx);
    return m_staged.size();
}

// ------------------------------------------------------------------------------------------------ reading
bool Vfs::ReadIn(View v, ArcKind a, std::wstring_view innerIn, Bytes& out, Hit* hit) const {
    out.clear();
    const std::wstring inner = Norm(innerIn);
    if (inner.empty()) return false;
    std::vector<std::shared_ptr<Layer>> layers;
    {
        std::shared_lock lk(m_mx);
        if (v == View::After) {
            auto it = m_staged.find({static_cast<int>(a), Lower(inner)});
            if (it != m_staged.end()) {
                out = it->second;
                if (hit) *hit = {a, Source::Staged, L"", true};
                return true;
            }
        }
        layers = m_layers;
    }
    for (const auto& L : layers) {
        if (L->arc != a || (L->after && v == View::Before)) continue;
        if (LayerRead(*L, inner, &out, nullptr)) {
            if (hit) *hit = {a, L->src, L->pk2 ? L->pk2->Path() : L->dir, L->after};
            return true;
        }
    }
    out.clear();
    return false;
}

bool Vfs::Read(ArcKind a, std::wstring_view inner, Bytes& out, Hit* hit) const { return ReadIn(m_view, a, inner, out, hit); }

bool Vfs::ReadText(ArcKind a, std::wstring_view inner, std::wstring& out, Hit* hit) const {
    Bytes b;
    if (!Read(a, inner, b, hit)) return false;
    out = DecodeText(b);
    return true;
}

bool Vfs::ExistsIn(View v, ArcKind a, std::wstring_view innerIn, Hit* hit) const {
    const std::wstring inner = Norm(innerIn);
    if (inner.empty()) return false;
    std::vector<std::shared_ptr<Layer>> layers;
    {
        std::shared_lock lk(m_mx);
        if (v == View::After && m_staged.count({static_cast<int>(a), Lower(inner)})) {
            if (hit) *hit = {a, Source::Staged, L"", true};
            return true;
        }
        layers = m_layers;
    }
    for (const auto& L : layers) {
        if (L->arc != a || (L->after && v == View::Before)) continue;
        if (LayerRead(*L, inner, nullptr, nullptr)) {
            if (hit) *hit = {a, L->src, L->pk2 ? L->pk2->Path() : L->dir, L->after};
            return true;
        }
    }
    return false;
}

bool Vfs::Exists(ArcKind a, std::wstring_view inner, Hit* hit) const { return ExistsIn(m_view, a, inner, hit); }

bool Vfs::Size(ArcKind a, std::wstring_view innerIn, std::uint64_t& size) const {
    const std::wstring inner = Norm(innerIn);
    std::vector<std::shared_ptr<Layer>> layers;
    const View v = m_view;
    {
        std::shared_lock lk(m_mx);
        if (v == View::After) {
            auto it = m_staged.find({static_cast<int>(a), Lower(inner)});
            if (it != m_staged.end()) {
                size = it->second.size();
                return true;
            }
        }
        layers = m_layers;
    }
    for (const auto& L : layers) {
        if (L->arc != a || (L->after && v == View::Before)) continue;
        if (LayerRead(*L, inner, nullptr, &size)) return true;
    }
    return false;
}

bool Vfs::ReadAny(std::wstring_view innerIn, Bytes& out, Hit* hit) const {
    std::wstring inner = Norm(innerIn);
    for (int ai = 0; ai < kArcKinds; ++ai) {  // explicit archive prefix
        const std::wstring pre = std::wstring(ArcFolderName(static_cast<ArcKind>(ai))) + L"\\";
        if (IStarts(inner, pre)) {
            const std::wstring rest = inner.substr(pre.size());
            if (Read(static_cast<ArcKind>(ai), rest, out, hit)) return true;
        }
    }
    for (ArcKind a : {ArcKind::Data, ArcKind::Particles, ArcKind::Media, ArcKind::Map, ArcKind::Music})
        if (Read(a, inner, out, hit)) return true;
    return false;
}

bool Vfs::List(ArcKind a, std::wstring_view dirIn, std::vector<VfsItem>& out) const {
    out.clear();
    const std::wstring dir = Norm(dirIn);
    std::vector<std::shared_ptr<Layer>> layers;
    std::vector<std::wstring> stagedHere;
    const View v = m_view;
    {
        std::shared_lock lk(m_mx);
        layers = m_layers;
        if (v == View::After) {
            const std::wstring ld = Lower(dir);
            for (const auto& kv : m_staged) {
                if (kv.first.first != static_cast<int>(a)) continue;
                const std::wstring& p = kv.first.second;
                const size_t s = p.find_last_of(L'\\');
                const std::wstring pd = s == std::wstring::npos ? L"" : p.substr(0, s);
                if (pd == ld) stagedHere.push_back(s == std::wstring::npos ? p : p.substr(s + 1));
            }
        }
    }
    bool any = false;
    std::unordered_set<std::wstring> seen;
    for (const auto& n : stagedHere) {
        if (seen.insert(n).second) out.push_back({n, false, 0, Source::Staged, true});
        any = true;
    }
    for (const auto& L : layers) {
        if (L->arc != a || (L->after && v == View::Before)) continue;
        std::vector<VfsItem> items;
        if (!LayerList(*L, dir, items)) continue;
        any = true;
        for (auto& it : items)
            if (seen.insert(Lower(it.name)).second) out.push_back(std::move(it));
    }
    std::sort(out.begin(), out.end(), [](const VfsItem& x, const VfsItem& y) {
        if (x.folder != y.folder) return x.folder;
        return Lower(x.name) < Lower(y.name);
    });
    return any;
}

size_t Vfs::Find(ArcKind a, std::wstring_view globIn, std::vector<std::wstring>& out, size_t maxResults,
                 const std::function<void(int)>& progress, const std::atomic<bool>* cancel) const {
    out.clear();
    const std::wstring glob = Norm(globIn);
    const bool byName = glob.find(L'\\') == std::wstring::npos;
    std::vector<std::shared_ptr<Layer>> layers;
    std::vector<std::wstring> staged;
    const View v = m_view;
    {
        std::shared_lock lk(m_mx);
        layers = m_layers;
        if (v == View::After)
            for (const auto& kv : m_staged)
                if (kv.first.first == static_cast<int>(a)) staged.push_back(kv.first.second);
    }
    std::unordered_map<std::wstring, std::wstring> hits;  // lower -> display
    auto consider = [&](const std::wstring& p) -> bool {
        const size_t s = p.find_last_of(L'\\');
        const std::wstring_view nm = s == std::wstring::npos ? std::wstring_view(p) : std::wstring_view(p).substr(s + 1);
        if (GlobMatchI(glob, byName ? nm : std::wstring_view(p))) hits.emplace(Lower(p), p);
        return hits.size() < maxResults && !(cancel && cancel->load());
    };
    for (const auto& p : staged) consider(p);
    int li = 0;
    for (const auto& L : layers) {
        ++li;
        if (L->arc != a || (L->after && v == View::Before)) continue;
        if (cancel && cancel->load()) break;
        if (L->pk2) {
            L->pk2->BuildIndex(progress, cancel);
            L->pk2->ForEachFile([&](const std::wstring& p, std::uint64_t) { return consider(p); });
        } else if (L->src == Source::TextdataFolder) {
            std::vector<VfsItem> items;
            ListDir(L->dir, items, L->src, L->after);
            for (const auto& it : items)
                if (!it.folder && !consider(std::wstring(kTd) + L"\\" + it.name)) break;
        } else {
            std::vector<std::wstring> files;
            WalkFiles(L->dir, L"", files, 2000000);
            for (const auto& f : files)
                if (!consider(f)) break;
        }
        if (hits.size() >= maxResults) break;
    }
    out.reserve(hits.size());
    for (auto& kv : hits) out.push_back(kv.second);
    std::sort(out.begin(), out.end(), [](const std::wstring& x, const std::wstring& y) { return Lower(x) < Lower(y); });
    if (progress) progress(100);
    return out.size();
}

// ------------------------------------------------------------------------------------------------ before / after
Change Vfs::Compare(ArcKind a, std::wstring_view inner) const {
    Bytes before, after;
    const bool hb = ReadIn(View::Before, a, inner, before);
    const bool ha = ReadIn(View::After, a, inner, after);
    if (!ha && !hb) return Change::Missing;
    if (!hb) return Change::Added;
    if (!ha) return Change::Missing;
    return before == after ? Change::Same : Change::Modified;
}

std::vector<Vfs::Changed> Vfs::ChangedFiles() const {
    std::vector<Changed> out;
    std::vector<std::pair<int, std::wstring>> staged;
    std::vector<std::shared_ptr<Layer>> layers;
    {
        std::shared_lock lk(m_mx);
        for (const auto& kv : m_staged) staged.push_back(kv.first);
        layers = m_layers;
    }
    std::set<std::pair<int, std::wstring>> seen;
    for (const auto& s : staged) {
        if (!seen.insert(s).second) continue;
        out.push_back({static_cast<ArcKind>(s.first), s.second, Compare(static_cast<ArcKind>(s.first), s.second), Source::Staged});
    }
    for (const auto& L : layers) {
        if (!L->after || L->pk2) continue;
        std::vector<std::wstring> files;
        if (L->src == Source::TextdataFolder) {
            std::vector<VfsItem> items;
            ListDir(L->dir, items, L->src, true);
            for (const auto& it : items)
                if (!it.folder) files.push_back(std::wstring(kTd) + L"\\" + it.name);
        } else {
            WalkFiles(L->dir, L"", files, 200000);
        }
        for (const auto& f : files) {
            if (EndsWithI(f, L".bak") || EndsWithI(f, L".tmp")) continue;
            const std::pair<int, std::wstring> k{static_cast<int>(L->arc), Lower(f)};
            if (!seen.insert(k).second) continue;
            out.push_back({L->arc, f, Compare(L->arc, f), L->src});
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ textdata
bool Vfs::ReadTextdata(std::wstring_view name, Bytes& out, Hit* hit) const {
    return Read(ArcKind::Media, std::wstring(kTd) + L"\\" + std::wstring(name), out, hit);
}

bool Vfs::HasTextdata(std::wstring_view name) const {
    return Exists(ArcKind::Media, std::wstring(kTd) + L"\\" + std::wstring(name));
}

std::vector<std::wstring> Vfs::TextdataFiles() const {
    std::vector<VfsItem> items;
    std::vector<std::wstring> out;
    if (!List(ArcKind::Media, kTd, items)) return out;
    for (const auto& it : items)
        if (!it.folder) out.push_back(it.name);
    return out;
}

std::vector<std::wstring> Vfs::TextdataParts(std::wstring_view baseIn, std::vector<Problem>* problems) const {
    std::vector<std::wstring> out;
    const std::wstring base(baseIn);
    Bytes b;
    if (ReadTextdata(base + L".txt", b)) {
        const std::wstring text = DecodeText(b);
        std::vector<std::wstring> listed;
        bool index = true;
        int lines = 0;
        size_t a = 0;
        while (a <= text.size()) {
            size_t z = text.find_first_of(L"\r\n", a);
            if (z == std::wstring::npos) z = text.size();
            std::wstring line = Trim(std::wstring_view(text).substr(a, z - a));
            a = z + 1;
            if (line.empty() || StartsWith(line, L"//")) continue;
            ++lines;
            // first cell; every other cell must be empty
            const size_t t = line.find(L'\t');
            std::wstring first = Trim(line.substr(0, t));
            if (t != std::wstring::npos && !Trim(line.substr(t)).empty()) {
                index = false;
                break;
            }
            if (!EndsWithI(first, L".txt") || first.find_first_of(L"\\/") != std::wstring::npos) {
                index = false;
                break;
            }
            if (std::find_if(listed.begin(), listed.end(), [&](const std::wstring& x) { return Lower(x) == Lower(first); }) ==
                listed.end())
                listed.push_back(first);
        }
        if (index && lines > 0) {
            for (const auto& n : listed) {
                if (HasTextdata(n)) {
                    out.push_back(n);
                } else if (problems) {
                    problems->push_back({Problem::Warn, L"INDEX_MISSING",
                                         base + L".txt dizininde listelenen dosya yok: " + n + L" (oyun bu parçayı okuyamaz)",
                                         L"File listed in " + base + L".txt is missing: " + n});
                }
            }
            return out;
        }
        if (lines == 0 && b.size() <= 16) {
            // empty base file (SilkroadTR skilleffect.txt = 4 bytes): fall through to the numbered parts
        } else {
            out.push_back(base + L".txt");
            return out;
        }
    }
    // numbered parts <base>_<N>.txt
    std::vector<std::pair<long long, std::wstring>> parts;
    const std::wstring lb = Lower(base) + L"_";
    for (const auto& n : TextdataFiles()) {
        const std::wstring ln = Lower(n);
        if (ln.size() <= lb.size() + 4 || ln.compare(0, lb.size(), lb) != 0 || !EndsWithI(ln, L".txt")) continue;
        const std::wstring mid = ln.substr(lb.size(), ln.size() - lb.size() - 4);
        long long v = 0;
        if (mid.empty() || !ParseInt(mid, v)) continue;
        parts.push_back({v, n});
    }
    std::sort(parts.begin(), parts.end());
    for (auto& p : parts) out.push_back(p.second);
    if (!out.empty() && problems)
        problems->push_back({Problem::Info, L"SPLIT_PARTS",
                             base + L".txt yok ya da boş; numaralı parçalar okunuyor (" + std::to_wstring(out.size()) + L" dosya).",
                             base + L".txt missing or empty; reading numbered parts."});
    return out;
}

// ------------------------------------------------------------------------------------------------ copying out
bool Vfs::IsSafeOutputDir(const std::wstring& dirIn, const std::vector<std::wstring>& clientRoots, std::wstring* whyTr) {
    auto fail = [&](const std::wstring& w) {
        if (whyTr) *whyTr = w;
        return false;
    };
    wchar_t full[MAX_PATH * 2]{};
    if (dirIn.empty() || !GetFullPathNameW(dirIn.c_str(), MAX_PATH * 2, full, nullptr)) return fail(L"Çıktı klasörü geçersiz: " + dirIn);
    const std::wstring dir = StripTrailing(full);
    const std::wstring ld = Lower(dir) + L"\\";
    for (const auto& r : clientRoots) {
        if (r.empty()) continue;
        wchar_t rf[MAX_PATH * 2]{};
        if (!GetFullPathNameW(r.c_str(), MAX_PATH * 2, rf, nullptr)) continue;
        const std::wstring lr = Lower(StripTrailing(rf)) + L"\\";
        if (ld.compare(0, lr.size(), lr) == 0)
            return fail(L"Çıktı klasörü bir istemci klasörünün içinde olamaz (istemci dosyaları asla yazılmaz): " + dir);
    }
    // the folder and its ancestors must not be a game folder
    std::wstring cur = dir;
    for (int depth = 0; depth < 32 && !cur.empty(); ++depth) {
        if (IsDir(cur)) {
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW(JoinPath(cur, L"*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                std::wstring hit;
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    const std::wstring n = Lower(fd.cFileName);
                    // exact extension (FindFirstFile "*.pk2" would also match 8.3 names like "x.pk2x")
                    if ((n.size() > 4 && n.compare(n.size() - 4, 4, L".pk2") == 0) || n == L"sro_client.exe" || n == L"silkroad.exe")
                        hit = fd.cFileName;
                } while (hit.empty() && FindNextFileW(h, &fd));
                FindClose(h);
                if (!hit.empty())
                    return fail((depth == 0 ? L"Çıktı klasöründe oyun dosyası var (" : L"Çıktı klasörü bir oyun klasörünün içinde (") + hit +
                                L"): " + cur + L". Boş, ayrı bir klasör seçin.");
            }
        }
        const std::wstring up = DirOf(cur);
        if (up.empty() || up == cur || up.size() < 3) break;
        cur = up;
    }
    return true;
}

bool Vfs::ExtractTo(ArcKind a, std::wstring_view inner, const std::wstring& outRoot, std::wstring* whyTr) const {
    std::vector<std::wstring> roots;
    {
        std::shared_lock lk(m_mx);
        roots.push_back(m_root);
        roots.push_back(m_opened);
        for (const auto& L : m_layers)
            if (!L->after) roots.push_back(L->pk2 ? DirOf(L->pk2->Path()) : L->dir);
    }
    if (!IsSafeOutputDir(outRoot, roots, whyTr)) return false;
    Bytes b;
    const std::wstring n = Norm(inner);
    if (!Read(a, n, b)) {
        if (whyTr) *whyTr = std::wstring(L"İstemcide yok: ") + ArcFolderName(a) + L"\\" + n;
        return false;
    }
    const std::wstring dst = JoinPath(JoinPath(outRoot, ArcFolderName(a)), n);
    std::wstring cur;
    for (size_t i = 0; i < dst.size(); ++i) {  // create folders
        if (dst[i] == L'\\' && i > 2) {
            cur = dst.substr(0, i);
            CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    std::wstring e;
    if (!WriteFileAtomic(dst, b, e)) {
        if (whyTr) *whyTr = L"Yazılamadı: " + dst + L" (" + e + L")";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ info
std::wstring Vfs::Describe(bool en) const {
    std::shared_lock lk(m_mx);
    if (m_layers.empty()) return en ? L"no client" : L"istemci yok";
    std::wstring s;
    for (int i = 0; i < kArcKinds; ++i) {
        const ArcState& st = m_arc[i];
        if (!st.present) continue;
        if (!s.empty()) s += L" + ";
        if (st.src == Source::Pk2) s += ArcFileName(static_cast<ArcKind>(i));
        else if (st.src == Source::TextdataFolder) s += L"textdata";
        else s += std::wstring(ArcFolderName(static_cast<ArcKind>(i))) + (en ? L" folder" : L" klasörü");
    }
    s += L"  (" + (m_root.empty() ? m_opened : m_root) + L")";
    if (!m_overlays.empty() || !m_staged.empty())
        s += Fmt(en ? L"  [after: %d overlay, %d pending]" : L"  [sonrası: %d çıktı klasörü, %d bekleyen]",
                 static_cast<int>(m_overlays.size()), static_cast<int>(m_staged.size()));
    return s;
}

std::wstring Vfs::Fingerprint() const {
    std::shared_lock lk(m_mx);
    std::wstring f;
    for (const auto& L : m_layers) {
        std::uint64_t sz = 0, mt = 0;
        if (L->pk2) {
            FileInfo(L->pk2->Path(), &sz, &mt);
            f += Fmt(L"P%d:%s:%llx:%llx|", static_cast<int>(L->arc), Lower(L->pk2->Path()).c_str(),
                     static_cast<unsigned long long>(sz), static_cast<unsigned long long>(mt));
        } else {
            WIN32_FILE_ATTRIBUTE_DATA d{};
            if (GetFileAttributesExW(L->dir.c_str(), GetFileExInfoStandard, &d))
                mt = (static_cast<std::uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
            f += Fmt(L"F%d:%s:%llx|", static_cast<int>(L->arc), Lower(L->dir).c_str(), static_cast<unsigned long long>(mt));
        }
    }
    f += Fmt(L"S%d", static_cast<int>(m_staged.size()));
    return f;
}

}  // namespace se::client
