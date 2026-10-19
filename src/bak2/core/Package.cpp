// SkillEffect Studio - core/Package.cpp
#include "Package.h"

#include "SeModel.h"
#include "SkillCrypt.h"
#include "Source.h"
#include "Table.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <functional>
#include <set>
#include <unordered_set>

namespace se {

std::wstring LowerPath(std::wstring s) {
    for (auto& c : s) {
        if (c == L'/') c = L'\\';
        c = static_cast<wchar_t>(towlower(c));
    }
    return s;
}

long long BucketOf(long long id) { return (id / 5000 + 1) * 5000; }

TableSpec SpecFor(const std::wstring& idOrFile) {
    std::wstring k = LowerPath(idOrFile);
    if (EndsWithI(k, L".txt")) {
        const std::wstring t = TableIdOfFile(k);
        if (!t.empty()) k = t;
        else k = k.substr(0, k.size() - 4);
    }
    if (EndsWithI(k, L".append.frag")) k = k.substr(0, k.size() - 16);
    if (k == L"textdata_equip_and_skill") k = L"textdata_equip&skill";
    TableSpec s;
    s.id = k;
    s.known = true;
    if (k == L"skilldata") {
        s.file = L"skilldataenc.txt";
        s.pattern = L"SkillData_%lldENC.txt";
        s.bucket = s.encrypted = true;
        s.idCol = 1;
        s.codeCol = 3;
        s.cells = 118;
    } else if (k == L"characterdata") {
        s.file = L"characterdata.txt";
        s.pattern = L"CharacterData_%lld.txt";
        s.bucket = true;
        s.idCol = 1;
        s.codeCol = 2;
        s.cells = 104;
    } else if (k == L"itemdata") {
        s.file = L"itemdata.txt";
        s.pattern = L"ItemData_%lld.txt";
        s.bucket = true;
        s.idCol = 1;
        s.codeCol = 2;
        s.cells = 160;
    } else if (k == L"textdata_object" || k == L"textdata_equip&skill") {
        s.file = k + L".txt";
        s.keyCols = {1};
    } else if (k == L"effectsound") {
        s.file = L"effectsound.txt";
        s.cp949 = true;
        s.keyCols = {1, 2, 3, 8, 9};
    } else if (k == L"npcpos") {
        s.file = L"npcpos.txt";  // spawn points: the whole row is the key
    } else {
        s.file = k + L".txt";
        s.known = false;
    }
    return s;
}

std::wstring TableIdOfFile(const std::wstring& file) {
    const std::wstring f = LowerPath(FileNameOf(file));
    auto bucketFile = [&](const wchar_t* pre, const wchar_t* post) {
        const std::wstring p(pre), q(post);
        if (f.size() <= p.size() + q.size() || f.compare(0, p.size(), p) != 0 || !EndsWithI(f, q)) return false;
        for (size_t i = p.size(); i < f.size() - q.size(); ++i)
            if (!iswdigit(f[i])) return false;
        return true;
    };
    if (bucketFile(L"skilldata_", L"enc.txt") || f == L"skilldataenc.txt") return L"skilldata";
    if (bucketFile(L"characterdata_", L".txt") || f == L"characterdata.txt") return L"characterdata";
    if (bucketFile(L"itemdata_", L".txt") || f == L"itemdata.txt") return L"itemdata";
    if (f == L"skilleffect.txt") return L"";
    if (EndsWithI(f, L".txt")) return f.substr(0, f.size() - 4);
    return L"";
}

bool Package::HasFile(Arc a, const std::wstring& inner) const {
    return files[static_cast<int>(a)].count(LowerPath(inner)) != 0;
}

std::wstring Package::FilePath(Arc a, const std::wstring& inner) const {
    auto it = files[static_cast<int>(a)].find(LowerPath(inner));
    return it == files[static_cast<int>(a)].end() ? std::wstring() : it->second;
}

int Package::FileCount() const {
    int n = 0;
    for (const auto& m : files) n += static_cast<int>(m.size());
    return n;
}

namespace {

void Walk(const std::wstring& dir, const std::wstring& rel, const std::function<void(const std::wstring&, const std::wstring&)>& fn,
          int depth = 0) {
    if (depth > 24) return;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        const std::wstring full = JoinPath(dir, n), r = rel.empty() ? n : rel + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) Walk(full, r, fn, depth + 1);
        else fn(r, full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::vector<std::wstring> FilesLike(const std::wstring& dir, const wchar_t* pattern) {
    std::vector<std::wstring> v;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) v.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(v.begin(), v.end());
    return v;
}

bool ReadText(const std::wstring& p, std::wstring& out) {
    Bytes b;
    std::wstring e;
    if (!ReadFileBytes(p, b, e)) return false;
    out = DecodeText(b);
    return true;
}

// data rows of a table file (comments / blocks follow the client rules)
std::vector<std::wstring> DataRows(const Bytes& b) {
    std::vector<std::wstring> v;
    TableDoc d;
    std::wstring e;
    if (!d.Load(b, e)) return v;
    for (size_t i = 0; i < d.Count(); ++i)
        if (d.IsData(i)) v.push_back(d.text[i]);
    return v;
}

std::vector<std::wstring> NonEmptyLines(const std::wstring& t) {
    std::vector<std::wstring> v;
    size_t a = 0;
    while (a < t.size()) {
        size_t b = t.find(L'\n', a);
        if (b == std::wstring::npos) b = t.size();
        std::wstring l = t.substr(a, b - a);
        a = b + 1;
        while (!l.empty() && l.back() == L'\r') l.pop_back();
        if (!IsBlankClient(l)) v.push_back(std::move(l));
    }
    return v;
}

// "target"/"fragment" pairs of merge_plan.json (flat objects written by TeraStudio)
std::vector<std::pair<std::wstring, std::wstring>> PlanPairs(const std::wstring& json) {
    std::vector<std::pair<std::wstring, std::wstring>> v;
    size_t a = 0;
    auto value = [&](size_t from, const wchar_t* key, size_t end) -> std::wstring {
        const std::wstring k = std::wstring(L"\"") + key + L"\"";
        size_t p = json.find(k, from);
        if (p == std::wstring::npos || p > end) return {};
        p = json.find(L'"', json.find(L':', p + k.size()) + 1);
        if (p == std::wstring::npos) return {};
        std::wstring s;
        for (size_t i = p + 1; i < json.size() && json[i] != L'"'; ++i) {
            if (json[i] == L'\\' && i + 1 < json.size()) {
                ++i;
                s += json[i] == L't' ? L'\t' : json[i];
            } else {
                s += json[i];
            }
        }
        return s;
    };
    while ((a = json.find(L'{', a)) != std::wstring::npos) {
        const size_t e = json.find(L'}', a);
        if (e == std::wstring::npos) break;
        const std::wstring t = value(a, L"target", e), f = value(a, L"fragment", e);
        if (!t.empty() && !f.empty()) v.emplace_back(t, f);
        a = e + 1;
    }
    return v;
}

}  // namespace

bool LoadPackage(const std::wstring& path, const ClientBase* base, Package& pkg, std::wstring& err) {
    pkg = Package{};
    pkg.path = path;
    pkg.transfer = DirExists(path) && FileExists(JoinPath(path, L"aktarim_listesi.txt"));
    std::wstring root, mergeDir;
    if (DirExists(path)) {
        if (DirExists(JoinPath(path, L"_merge")) || DirExists(JoinPath(path, L"Client"))) {
            root = path;
        } else if (!FilesLike(path, L"*.frag").empty() || FileExists(JoinPath(path, L"merge_plan.json"))) {
            mergeDir = path;
            const std::wstring up = DirOf(path);
            root = DirExists(JoinPath(up, L"Client")) ? up : L"";
        } else {
            root = path;  // any folder: textdata files directly inside
        }
    } else if (FileExists(path)) {
        if (EndsWithI(path, L"merge_plan.json") || EndsWithI(path, L".frag")) {
            mergeDir = DirOf(path);
            const std::wstring up = DirOf(mergeDir);
            root = DirExists(JoinPath(up, L"Client")) ? up : L"";
        } else {
            // a list file: verbatim (may already hold #section / #table blocks)
            std::wstring t;
            if (!ReadText(path, t)) {
                err = L"Okunamadı / cannot read: " + path;
                return false;
            }
            pkg.listText = t;
            pkg.desc = FileNameOf(path);
            pkg.parsed = ParseListText(t, pkg.desc);
            pkg.seRows = static_cast<int>(pkg.parsed.rows.size());
            for (const auto& r : pkg.parsed.tables) pkg.tableRows[r.table]++;
            return true;
        }
    } else {
        err = L"Bulunamadı / not found: " + path;
        return false;
    }
    pkg.root = root;
    if (!root.empty()) {
        // the export's own client IDs (_report\ids.json: "common_main": 70005001, "skill_01": 115001 ...): their rows
        // are the export's even when the client already has them (re-applying / re-validating an output)
        std::wstring js;
        if (ReadText(JoinPath(root, L"_report\\ids.json"), js)) {
            size_t a = 0;
            while ((a = js.find(L'"', a)) != std::wstring::npos) {
                const size_t e = js.find(L'"', a + 1);
                if (e == std::wstring::npos) break;
                const std::wstring k = js.substr(a + 1, e - a - 1);
                a = e + 1;
                if (!(StartsWith(k, L"common_") || StartsWith(k, L"skill_") || StartsWith(k, L"item_"))) continue;
                size_t p = js.find(L':', e);
                if (p == std::wstring::npos) break;
                ++p;
                while (p < js.size() && (js[p] == L' ' || js[p] == L'	')) ++p;
                size_t q = p;
                while (q < js.size() && iswdigit(js[q])) ++q;
                long long v = 0;
                if (q > p && ParseInt(std::wstring_view(js).substr(p, q - p), v) && v > 0) pkg.ownIds.insert(v);
            }
        }
        // _report\skills.txt: "SKILL <code>  ID n  ...  clip <clip>" -> which animation clip a skill plays
        std::wstring sk;
        if (ReadText(JoinPath(root, L"_report\\skills.txt"), sk))
            for (const auto& l : NonEmptyLines(sk)) {
                const auto t = SplitView(l, L' ');
                std::vector<std::wstring> w;
                for (const auto& x : t)
                    if (!x.empty()) w.emplace_back(x);
                if (w.size() < 2 || w[0] != L"SKILL") continue;
                for (size_t i = 2; i + 1 < w.size(); ++i)
                    if (w[i] == L"clip") {
                        pkg.skillClip[w[1]] = w[i + 1];
                        break;
                    }
            }
    }
    if (mergeDir.empty() && !root.empty() && DirExists(JoinPath(root, L"_merge"))) mergeDir = JoinPath(root, L"_merge");
    std::wstring td;
    if (!root.empty()) {
        const wchar_t* cands[] = {L"Client\\Media\\server_dep\\silkroad\\textdata", L"Media\\server_dep\\silkroad\\textdata",
                                  L"server_dep\\silkroad\\textdata", L"textdata", L""};
        for (const auto* c : cands) {
            const std::wstring d = *c ? JoinPath(root, c) : root;
            if (DirExists(d) && (FileExists(JoinPath(d, L"skilleffect.txt")) || FileExists(JoinPath(d, L"skilldataenc.txt")) ||
                                 FileExists(JoinPath(d, L"characterdata.txt")) || !FilesLike(d, L"SkillData_*ENC.txt").empty() ||
                                 !FilesLike(d, L"CharacterData_*.txt").empty())) {
                td = d;
                break;
            }
        }
    }

    // ---------------- skilleffect.txt rows
    std::wstring seText;
    const wchar_t* secs[] = {L"characterInfo", L"skillaniset2", L"skilleffectset"};
    int fragFound = 0;
    if (!mergeDir.empty()) {
        for (const auto* s : secs) {
            const std::wstring f = JoinPath(mergeDir, std::wstring(L"skilleffect.txt.") + s + L".frag");
            std::wstring t;
            if (!FileExists(f) || !ReadText(f, t)) continue;
            const auto rows = NonEmptyLines(t);
            if (rows.empty()) continue;
            ++fragFound;
            seText += std::wstring(L"#section ") + s + L"\r\n";
            for (const auto& r : rows) seText += r + L"\r\n";
        }
        if (fragFound) pkg.notes.push_back(Fmt(L"_merge: skilleffect.txt.*.frag (%d)", fragFound));
    }
    if (!fragFound && !td.empty() && FileExists(JoinPath(td, L"skilleffect.txt"))) {
        // a full skilleffect.txt copy (old exporter: rows appended at EOF): keep the lines the client lacks
        Bytes pb, bb;
        std::wstring e;
        if (ReadFileBytes(JoinPath(td, L"skilleffect.txt"), pb, e)) {
            std::unordered_set<std::wstring> baseLines;
            const bool haveBase = base && base->IsOpen() && base->ReadTextdata(L"skilleffect.txt", bb);
            if (haveBase) {
                const std::wstring bt = DecodeText(bb);
                for (const auto& l : SplitPhysical(bt)) baseLines.insert(bt.substr(l.off, l.len));
            }
            const std::wstring pt = DecodeText(pb);
            std::wstring cur;
            std::wstring last;
            int n = 0;
            for (const auto& l : SplitPhysical(pt)) {
                const std::wstring s = pt.substr(l.off, l.len);
                const auto tok = ClientTokens(s);
                if (!tok.empty() && tok[0] == L"#section" && tok.size() > 1) {
                    cur = tok[1];
                    continue;
                }
                if (!haveBase || baseLines.count(s)) continue;
                size_t a = 0;
                while (a < s.size() && IsClientWs(s[a])) ++a;
                if (StartsWith(std::wstring_view(s).substr(a), L"//") || StartsWith(std::wstring_view(s).substr(a), L"/*")) continue;
                const size_t nt = tok.size();
                if (nt < 13 || cur.empty()) continue;
                if (cur != last) {
                    seText += L"#section " + cur + L"\r\n";
                    last = cur;
                }
                seText += s + L"\r\n";
                ++n;
            }
            pkg.notes.push_back(haveBase ? Fmt(L"Client\\Media\\...\\skilleffect.txt: istemcide olmayan %d satır / %d new rows", n, n)
                                         : std::wstring(L"skilleffect.txt kopyası var ama istemci açık değil: fark alınamadı / client not open, no diff"));
        }
    }

    // ---------------- other tables
    std::map<std::wstring, std::vector<std::wstring>> tables;
    std::set<std::wstring> fragTables;
    if (!mergeDir.empty()) {
        std::wstring json;
        std::vector<std::pair<std::wstring, std::wstring>> pairs;
        if (ReadText(JoinPath(mergeDir, L"merge_plan.json"), json)) pairs = PlanPairs(json);
        if (pairs.empty())
            for (const auto& f : FilesLike(mergeDir, L"*.append.frag")) pairs.emplace_back(f.substr(0, f.size() - 12), L"_merge/" + f);
        for (const auto& [target, frag] : pairs) {
            const std::wstring tf = FileNameOf(LowerPath(target));
            if (tf == L"skilleffect.txt") continue;
            std::wstring t;
            if (!ReadText(JoinPath(mergeDir, FileNameOf(LowerPath(frag))), t)) continue;
            const TableSpec sp = SpecFor(tf);
            auto& v = tables[sp.id];
            for (auto& r : NonEmptyLines(t)) v.push_back(std::move(r));
            fragTables.insert(sp.id);
        }
    }
    if (!td.empty()) {
        // bucket tables: new bucket files (+ rows of copies the client lacks)
        for (const wchar_t* id : {L"skilldata", L"characterdata", L"itemdata"}) {
            const TableSpec sp = SpecFor(id);
            std::vector<std::wstring> names;
            std::wstring it;
            if (ReadText(JoinPath(td, sp.file), it))
                for (const auto& l : NonEmptyLines(it))
                    if (!StartsWith(Trim(l), L"//")) names.push_back(Trim(l));
            for (const auto& f : FilesLike(td, sp.encrypted ? L"SkillData_*ENC.txt" : sp.id == L"characterdata" ? L"CharacterData_*.txt" : L"ItemData_*.txt"))
                names.push_back(f);
            std::set<std::wstring> seen;
            auto& v = tables[sp.id];
            for (const auto& n : names) {
                const std::wstring ln = LowerPath(n);
                if (!seen.insert(ln).second) continue;
                const std::wstring f = JoinPath(td, n);
                Bytes b;
                std::wstring e;
                if (!FileExists(f) || !ReadFileBytes(f, b, e)) continue;
                Bytes plain = b;
                if (sp.encrypted) {
                    if (!SkillDecrypt(b, plain)) {
                        pkg.notes.push_back(L"! " + n + L": //encrypt işareti yok / no //encrypt marker");
                        continue;
                    }
                }
                std::unordered_set<std::wstring> baseRows;
                Bytes bb;
                if (base && base->IsOpen() && base->ReadTextdata(n, bb)) {
                    Bytes bp = bb;
                    if (sp.encrypted) SkillDecrypt(bb, bp);
                    for (auto& r : DataRows(bp)) baseRows.insert(std::move(r));
                }
                int k = 0, own = 0;
                for (auto& r : DataRows(plain)) {
                    bool mine = !baseRows.count(r);
                    if (!mine && !pkg.ownIds.empty()) {  // already in the client, but the export created it: keep ("zaten var")
                        const auto c = SplitTabs(r);
                        long long rid = 0;
                        mine = c.size() > 1 && ParseInt(c[1], rid) && pkg.ownIds.count(rid);
                        own += mine;
                    }
                    if (mine) {
                        v.push_back(std::move(r));
                        ++k;
                    }
                }
                if (k) pkg.notes.push_back(Fmt(L"%s: %d satır / rows", n.c_str(), k) + (own ? Fmt(L" (%d istemcide zaten var / already in the client)", own) : L""));
            }
            if (sp.encrypted && v.empty() && !mergeDir.empty()) {  // review copies only when no ENC file was shipped
                for (const auto& f : FilesLike(mergeDir, L"plain_SkillData_*.txt")) {
                    Bytes b;
                    std::wstring e;
                    if (!ReadFileBytes(JoinPath(mergeDir, f), b, e)) continue;
                    for (auto& r : DataRows(b)) v.push_back(std::move(r));
                    pkg.notes.push_back(L"_merge\\" + f + L" (düz kopya / plain copy)");
                }
            }
            if (v.empty()) tables.erase(sp.id);
        }
        // shared tables without a fragment: full copies -> rows the client lacks
        for (const wchar_t* f : {L"textdata_object.txt", L"textdata_equip&skill.txt", L"effectsound.txt"}) {
            const TableSpec sp = SpecFor(f);
            if (fragTables.count(sp.id) || !FileExists(JoinPath(td, f))) continue;
            Bytes pb, bb;
            std::wstring e;
            if (!ReadFileBytes(JoinPath(td, f), pb, e)) continue;
            if (!(base && base->IsOpen() && base->ReadTextdata(f, bb))) {
                pkg.notes.push_back(std::wstring(f) + L": istemci açık değil, fark alınamadı / client not open, no diff");
                continue;
            }
            std::unordered_set<std::wstring> baseRows;
            for (auto& r : DataRows(bb)) baseRows.insert(std::move(r));
            auto& v = tables[sp.id];
            int k = 0;
            for (auto& r : DataRows(pb))
                if (!baseRows.count(r)) {
                    v.push_back(std::move(r));
                    ++k;
                }
            if (!k) tables.erase(sp.id);
            else pkg.notes.push_back(Fmt(L"%s (tam kopya / full copy): %d yeni satır / new rows", f, k));
        }
    }

    // ---------------- resources shipped with the export (count as present)
    if (!root.empty()) {
        const wchar_t* arcDirs[kArcCount] = {L"Client\\Media", L"Client\\Data", L"Client\\Particles"};
        for (int a = 0; a < kArcCount; ++a) {
            const std::wstring d = JoinPath(root, arcDirs[a]);
            if (!DirExists(d)) continue;
            Walk(d, L"", [&](const std::wstring& rel, const std::wstring& full) {
                const std::wstring l = LowerPath(rel);
                if (a == 0 && StartsWith(l, L"server_dep\\silkroad\\textdata\\")) return;
                pkg.files[a][l] = full;
            });
        }
        const std::wstring sq = JoinPath(root, L"Server\\SQL");
        for (const auto& f : FilesLike(sq, L"*.sql")) pkg.sql.push_back(JoinPath(sq, f));
    }

    // ---------------- canonical list text
    std::wstring out = L"// SkillEffect Studio paket / package: " + (root.empty() ? path : root) + L"\r\n";
    for (const auto& n : pkg.notes) out += L"//   " + n + L"\r\n";
    out += seText;
    for (const auto& [id, rows] : tables) {
        if (rows.empty()) continue;
        out += L"#table " + id + L"\r\n";
        for (const auto& r : rows) out += r + L"\r\n";
    }
    if (seText.empty() && tables.empty()) {
        err = L"Pakette eklenecek satır bulunamadı (skilleffect frag / textdata dosyası yok) / nothing to merge in: " + path;
        if (pkg.FileCount() == 0) return false;
    }
    pkg.listText = out;
    pkg.textdataDir = td;
    pkg.desc = FileNameOf(root.empty() ? path : root);
    pkg.parsed = ParseListText(out, pkg.desc);
    pkg.seRows = static_cast<int>(pkg.parsed.rows.size());
    for (const auto& r : pkg.parsed.tables) pkg.tableRows[r.table]++;
    return true;
}

}  // namespace se
