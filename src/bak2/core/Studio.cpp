// SkillEffect Studio - core/Studio.cpp
#include "Studio.h"

#include "Enums.h"
#include "SkillCrypt.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <set>
#include <unordered_set>

namespace se {

namespace {

Issue Mk(Sev s, const wchar_t* rule, int listLine, const std::wstring& key, const std::wstring& table, std::wstring tr,
         std::wstring en) {
    Issue i;
    i.sev = s;
    i.rule = rule;
    i.listLine = listLine;
    i.key = key;
    i.table = table;
    i.tr = std::move(tr);
    i.en = std::move(en);
    return i;
}

std::wstring StripEol(std::wstring s) {
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    return s;
}

bool LastNumber(const std::wstring& s, size_t& pos, size_t& len, long long& v) {
    size_t e = s.size();
    while (e > 0 && !iswdigit(s[e - 1])) --e;
    if (e == 0) return false;
    size_t b = e;
    while (b > 0 && iswdigit(s[b - 1])) --b;
    pos = b;
    len = e - b;
    return ParseInt(std::wstring_view(s).substr(b, e - b), v);
}

std::wstring BucketName(const std::wstring& example, const std::wstring& pattern, long long bucket) {
    size_t p = 0, n = 0;
    long long v = 0;
    if (!example.empty() && LastNumber(example, p, n, v)) return example.substr(0, p) + std::to_wstring(bucket) + example.substr(p + n);
    return Fmt(pattern.c_str(), bucket);
}

std::wstring KeyOf(const std::vector<std::wstring>& c, const std::vector<int>& cols, const std::wstring& whole) {
    if (cols.empty()) return whole;
    std::wstring k;
    for (int col : cols) {
        k += static_cast<size_t>(col) < c.size() ? c[static_cast<size_t>(col)] : std::wstring();
        k += L'\x1f';
    }
    return k;
}

std::wstring Ext(const std::wstring& p) {
    const size_t d = p.rfind(L'.');
    if (d == std::wstring::npos) return {};
    std::wstring e = p.substr(d + 1);
    for (auto& c : e) c = static_cast<wchar_t>(towlower(c));
    return e;
}

bool IsNoneCell(const std::wstring& s) {
    const std::wstring t = Trim(s);
    return t.empty() || t == L"none" || t == L"xxx" || t == L"0" || t == L"-";
}

// strings of a .bsr that name other resources: u32 length + printable ASCII path with a known extension
std::vector<std::wstring> BsrRefs(const Bytes& b) {
    std::vector<std::wstring> v;
    static const char* exts[] = {".bsk", ".ban", ".bms", ".bmt", ".efp", ".bsr", ".wav", ".ddj"};
    size_t i = 0;
    while (i + 4 < b.size()) {
        const std::uint32_t n = static_cast<std::uint32_t>(b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (static_cast<std::uint32_t>(b[i + 3]) << 24));
        bool hit = false;
        if (n >= 5 && n <= 260 && i + 4 + n <= b.size()) {
            bool printable = true;
            for (size_t k = 0; k < n && printable; ++k) printable = b[i + 4 + k] >= 32 && b[i + 4 + k] < 127;
            if (printable) {
                std::string s(reinterpret_cast<const char*>(b.data() + i + 4), n);
                std::string low = s;
                for (auto& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                for (const char* e : exts) {
                    const size_t el = strlen(e);
                    if (low.size() > el && low.compare(low.size() - el, el, e) == 0) {
                        v.push_back(FromUtf8(s));
                        hit = true;
                        break;
                    }
                }
            }
        }
        i += hit ? 4 + n : 1;
    }
    return v;
}

// column defaults (SKILLEFFECT_LOGIC.md §5, §7.1; the values TeraStudio and shipped monster rows use)
const wchar_t* const kSeDef[28] = {L"-", L"KEY", L"SHOT", L"0", L"FALSE", L"none", L"MOB_BASE", L"0", L"0", L"0",
                                   L"0", L"1", L"0,0", L"AT_ONE_FOLLOW", L"MOV_NONE,0,0,0", L"0,0,0",
                                   L"false,0,0,0,0,false", L"none", L"none", L"none", L"0,0,0", L"none", L"0,0,0",
                                   L"none", L"0", L"none", L"none", L"none"};
const wchar_t* const kSaDef[27] = {L"1", L"-", L"KEY", L"0", L"FALSE", L"0", L"DEFAULT", L"none", L"none", L"none",
                                   L"none", L"none", L"none", L"none", L"hiteffect\\hit_3_normal.efp", L"0", L"0,0,0,0",
                                   L"INVSRCALPHA", L"none", L"none", L"none", L"none", L"none", L"none", L"none", L"0",
                                   L"1"};
const wchar_t* const kCiDef[13] = {L"KEY", L"KEY", L"2", L"none", L"none", L"none", L"none", L"none", L"0,10,0",
                                   L"none", L"none", L"0", L"0"};

std::vector<std::wstring> RowCells(const std::wstring& row) { return SplitTabs(StripEol(row)); }

std::wstring PadTableRow(const std::wstring& row, int want, bool trailingEmpty) {
    auto c = RowCells(row);
    bool hadTrail = false;
    if (trailingEmpty && !c.empty() && c.back().empty()) {
        c.pop_back();
        hadTrail = true;
    }
    const int target = trailingEmpty ? want - 1 : want;
    while (static_cast<int>(c.size()) < target) c.push_back(L"0");
    if (static_cast<int>(c.size()) > target && target > 0) c.resize(static_cast<size_t>(target));
    if (trailingEmpty || hadTrail) c.push_back(L"");
    return JoinTabs(c);
}


bool HasText(const std::wstring& s) {
    const std::wstring t = Trim(s);
    return !t.empty() && t != L"0";
}

// column layout of a textdata_* table: which columns of the client file carry the name, the usual value of the other
// columns, and which column of a package row (another layout: the 17-column 0Media copies) feeds each name column
struct TextCols {
    bool done = false;
    int width = 0;
    std::vector<std::wstring> typical;  // most common value per client column
    std::map<int, int> src;             // client name column -> package row column
    bool fromRef = false;               // src learnt from the package's own full copy of the table
    int compared = 0;
};

TextCols TextLayout(const TableDoc& d, const TableSpec& sp, const Package* pool, int rowWidth) {
    TextCols t;
    t.done = true;
    t.width = d.dominantCells;
    const int W = t.width;
    if (W <= 0) return t;
    std::vector<std::map<std::wstring, int>> freq(static_cast<size_t>(W));
    std::vector<int> filled(static_cast<size_t>(W), 0);
    std::unordered_map<std::wstring, size_t> lineOfKey;
    int n = 0;
    for (size_t i = 0; i < d.Count(); ++i) {
        if (!d.IsData(i)) continue;
        const auto c = d.Cells(i);
        if (static_cast<int>(c.size()) != W) continue;
        lineOfKey.emplace(c[1], i);
        if (++n > 4000) continue;
        for (int k = 0; k < W; ++k) {
            freq[static_cast<size_t>(k)][c[static_cast<size_t>(k)]]++;
            filled[static_cast<size_t>(k)] += HasText(c[static_cast<size_t>(k)]);
        }
    }
    const int sample = std::min(n, 4000);
    t.typical.resize(static_cast<size_t>(W));
    for (int k = 0; k < W; ++k) {
        int best = -1;
        for (const auto& [v, cnt] : freq[static_cast<size_t>(k)])
            if (cnt > best) {
                best = cnt;
                t.typical[static_cast<size_t>(k)] = v;
            }
    }
    std::vector<int> names;
    for (int k = 2; k < W; ++k)
        if (sample > 0 && filled[static_cast<size_t>(k)] * 10 >= sample * 3) names.push_back(k);
    // the package's own full copy of this table: same keys in the package layout -> learn the column mapping from data
    if (pool && !pool->textdataDir.empty() && !names.empty()) {
        Bytes b;
        std::wstring e;
        TableDoc pd;
        // the copy may differ by the trailing empty cell (16 vs 17): compare the columns both have
        if (ReadFileBytes(JoinPath(pool->textdataDir, sp.file), b, e) && pd.Load(b, e) && std::abs(pd.dominantCells - rowWidth) <= 1) {
            std::vector<std::vector<int>> hits(names.size(), std::vector<int>(static_cast<size_t>(rowWidth), 0));
            int cmp = 0;
            for (size_t i = 0; i < pd.Count() && cmp < 5000; ++i) {
                if (!pd.IsData(i)) continue;
                auto pc = pd.Cells(i);
                if (std::abs(static_cast<int>(pc.size()) - rowWidth) > 1 || pc.size() < 3) continue;
                pc.resize(static_cast<size_t>(rowWidth));
                auto it = lineOfKey.find(pc[1]);
                if (it == lineOfKey.end()) continue;
                const auto cc = d.Cells(it->second);
                ++cmp;
                for (size_t j = 0; j < names.size(); ++j) {
                    const std::wstring& v = cc[static_cast<size_t>(names[j])];
                    if (!HasText(v)) continue;
                    for (int k = 2; k < rowWidth; ++k) hits[j][static_cast<size_t>(k)] += pc[static_cast<size_t>(k)] == v;
                }
            }
            t.compared = cmp;
            if (cmp >= 20)
                for (size_t j = 0; j < names.size(); ++j) {
                    const auto mx = std::max_element(hits[j].begin(), hits[j].end());
                    if (*mx * 2 >= cmp) {
                        t.src[names[j]] = static_cast<int>(mx - hits[j].begin());
                        t.fromRef = true;
                    }
                }
        }
    }
    for (int c : names) {
        if (t.src.count(c)) continue;
        // no reference: a client of the full layout (16/17 columns) keeps the same index; the narrow 10-column files put
        // the English name last, which is column 8 of the standard full layout (2 KOR, 8 ENG, 9 VNM)
        t.src[c] = (W >= 16 || rowWidth == W) ? std::min(c, rowWidth - 1) : rowWidth > 8 ? 8 : rowWidth - 1;
    }
    return t;
}

std::wstring MapTextRow(const std::vector<std::wstring>& row, const TextCols& t, int want, std::wstring& how) {
    std::vector<std::wstring> out(static_cast<size_t>(want));
    for (int k = 0; k < want; ++k) out[static_cast<size_t>(k)] = k < static_cast<int>(t.typical.size()) ? t.typical[static_cast<size_t>(k)] : L"";
    if (!row.empty()) out[0] = row[0];
    if (row.size() > 1 && want > 1) out[1] = row[1];
    std::wstring m;
    for (const auto& [c, k] : t.src) {
        if (c >= want) continue;
        out[static_cast<size_t>(c)] = static_cast<size_t>(k) < row.size() ? row[static_cast<size_t>(k)] : L"";
        if (!m.empty()) m += L", ";
        std::wstring v = out[static_cast<size_t>(c)];
        if (v.size() > 32) v = v.substr(0, 31) + L"…";
        m += Fmt(L"%d%s → %d (\"%s\")", k, row.size() == 17 && k == 8 ? L" (ENG)" : L"", c, v.c_str());
    }
    int same = 0;
    for (int k = 2; k < want; ++k) same += !t.src.count(k);
    how = Fmt(L"%zu → %d sütun; ad sütunu %s; diğer %d hücre istemcideki gibi (%s)%s.", row.size(), want, m.c_str(), same,
              same && t.typical.size() > 2 && t.typical[2].empty() ? L"boş" : L"istemcinin değeri",
              t.fromRef ? Fmt(L"; eşleme paketteki tam tablo kopyasıyla %d ortak kayıttan öğrenildi", t.compared).c_str() : L"");
    return JoinTabs(out);
}

}  // namespace

// ------------------------------------------------------------------------------------------- .bsr animation roles
std::wstring AniToken(int t) {
    struct R {
        int id;
        const wchar_t* tok;
    };
    static const R fixed[] = {{2, L"ANI_ATTACK1"}, {5, L"ANI_ATTACK2"}, {16, L"ANI_ATTACK3"}, {17, L"ANI_ATTACK4"},
                              {183, L"ANI_ATTACK5"}, {184, L"ANI_ATTACK6"}, {185, L"ANI_ATTACK7"}, {186, L"ANI_ATTACK8"},
                              {190, L"ANI_ATTACK9"}};
    for (const auto& r : fixed)
        if (r.id == t) return r.tok;
    if (t >= 192 && t <= 198) return L"ANI_ATTACK" + std::to_wstring(t - 182);
    if (t >= 26 && t <= 35) return L"ANI_SKILL_" + std::to_wstring(t - 25);
    if (t >= 68 && t <= 77) return L"ANI_SKILL_" + std::to_wstring(t - 57);
    if (t >= 101 && t <= 120) return L"ANI_SKILL_" + std::to_wstring(t - 80);
    if (t >= 123 && t <= 182) return L"ANI_SKILL_" + std::to_wstring(t - 82);
    return {};
}

std::vector<std::pair<int, std::wstring>> BsrRoles(const Bytes& b) {
    std::vector<std::pair<int, std::wstring>> out;
    if (b.size() < 44 || std::memcmp(b.data(), "JMXVRES 0109", 12) != 0) return out;
    auto u32 = [&](size_t o, std::uint32_t& v) {
        if (o + 4 > b.size()) return false;
        v = static_cast<std::uint32_t>(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24));
        return true;
    };
    std::uint32_t offAnim = 0, offGroup = 0;
    if (!u32(12 + 3 * 4, offAnim) || !u32(12 + 5 * 4, offGroup)) return out;  // header: material mesh skeleton ANIMATION meshGroup ANIGROUP
    std::vector<std::wstring> files;
    size_t o = offAnim + 8;
    std::uint32_t n = 0;
    if (!u32(o, n) || n > 4096) return out;
    o += 4;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t len = 0;
        if (!u32(o, len) || len > 1024 || o + 4 + len > b.size()) return {};
        files.push_back(FromUtf8(std::string(reinterpret_cast<const char*>(b.data() + o + 4), len)));
        o += 4 + len;
    }
    o = offGroup;
    std::uint32_t groups = 0;
    if (!u32(o, groups) || groups > 64) return out;
    o += 4;
    for (std::uint32_t g = 0; g < groups; ++g) {
        std::uint32_t len = 0, ents = 0;
        if (!u32(o, len) || len > 1024) return out;
        o += 4 + len;
        if (!u32(o, ents) || ents > 4096) return out;
        o += 4;
        for (std::uint32_t e = 0; e < ents; ++e) {
            std::uint32_t type = 0, fi = 0, ev = 0, wn = 0;
            if (!u32(o, type) || !u32(o + 4, fi) || !u32(o + 8, ev) || ev > 4096) return out;
            o += 12 + static_cast<size_t>(ev) * 16;
            if (!u32(o, wn) || wn > 100000) return out;
            o += 8 + static_cast<size_t>(wn) * 8;
            if (o > b.size()) return out;
            if (fi < files.size()) out.emplace_back(static_cast<int>(type), files[fi]);
        }
    }
    return out;
}


// ------------------------------------------------------------------------------------------- templates / fixes
std::wstring AniShotForCode(const std::wstring& code) {
    const std::wstring u = ToUpper(code);
    if (u.find(L"SUMMON") != std::wstring::npos) return L"none";
    const size_t p = u.find(L"_ATTACK");
    if (p != std::wstring::npos) {
        size_t a = p + 7, b = a;
        while (b < u.size() && iswdigit(u[b])) ++b;
        long long n = 0;
        if (b > a && ParseInt(std::wstring_view(u).substr(a, b - a), n) && n >= 1 && n <= 16) return L"ANI_ATTACK" + std::to_wstring(n);
    }
    return L"ANI_ATTACK1";
}

std::wstring MakeSaRow(const std::wstring& key, const std::wstring& label, const std::wstring& aniShot) {
    std::wstring lab = Trim(label);
    for (auto& c : lab)
        if (c == L'\t' || c == L'/' || c == L'\r' || c == L'\n') c = L'-';
    if (lab.empty()) lab = key;
    std::vector<std::wstring> c(std::begin(kSaDef), std::end(kSaDef));
    c[1] = lab;
    c[2] = key;
    c[9] = aniShot;
    return JoinTabs(c);
}

std::wstring PadSeRow(Sec s, const std::wstring& row, std::vector<std::wstring>* changes) {
    const wchar_t* const* def = s == Sec::SE ? kSeDef : s == Sec::SA ? kSaDef : kCiDef;
    const size_t n = s == Sec::SE ? 28 : s == Sec::SA ? 27 : 13;
    auto c = RowCells(row);
    while (!c.empty() && IsBlankClient(c.back())) c.pop_back();
    size_t lead = 0;
    while (lead < c.size() && IsBlankClient(c[lead])) ++lead;
    if (lead) {
        c.erase(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(lead));
        if (changes) changes->push_back(L"baştaki boş hücre silindi / leading empty cell removed");
    }
    for (auto& x : c) {
        const std::wstring t = Trim(x);
        if (t != x && !t.empty()) x = t;
    }
    size_t nonEmpty = 0;
    for (const auto& x : c) nonEmpty += !IsBlankClient(x);
    if (nonEmpty == n || (s == Sec::CI && nonEmpty == 14)) {  // stray double TABs: drop the empty cells
        std::vector<std::wstring> k;
        for (auto& x : c)
            if (!IsBlankClient(x)) k.push_back(std::move(x));
        if (k.size() != c.size() && changes) changes->push_back(L"fazla TAB (boş hücre) silindi / stray empty cells removed");
        c = std::move(k);
    } else {
        for (size_t i = 0; i < c.size(); ++i)
            if (IsBlankClient(c[i])) {
                const std::wstring d = i < n ? def[i] : L"none";
                c[i] = d == L"KEY" ? (s == Sec::CI && !c.empty() ? c[0] : L"-") : d;
                if (changes) changes->push_back(Fmt(L"sütun %zu boş → %s / column %zu empty → %s", i, c[i].c_str(), i, c[i].c_str()));
            }
    }
    const size_t want = (s == Sec::CI && c.size() == 14) ? 14 : n;
    if (c.size() < want) {
        for (size_t i = c.size(); i < want; ++i) {
            std::wstring d = def[i];
            if (d == L"KEY") d = (s == Sec::CI && !c.empty()) ? c[0] : L"-";
            c.push_back(d);
            if (changes) changes->push_back(Fmt(L"eksik sütun %zu → %s / missing column %zu → %s", i, d.c_str(), i, d.c_str()));
        }
    } else if (c.size() > want) {
        if (changes) changes->push_back(Fmt(L"%zu fazla sütun kırpıldı / %zu extra columns trimmed", c.size() - want, c.size() - want));
        c.resize(want);
    }
    return JoinTabs(c);
}

// ------------------------------------------------------------------------------------------- base client
bool Studio::OpenBase(const std::wstring& path, const std::string& key, std::wstring& err) {
    ClientBase b;
    if (!b.Open(path, key, err)) return false;
    m_base = b;
    m_cache.clear();
    m_refLoaded = m_seLoaded = m_seOk = m_itemLoaded = false;
    m_ref.Clear();
    m_itemIds.clear();
    m_seErr.clear();
    return true;
}

bool Studio::BaseFile(const std::wstring& name, Bytes& out) {
    const std::wstring k = LowerPath(name);
    auto it = m_cache.find(k);
    if (it != m_cache.end()) {
        out = it->second;
        return true;
    }
    bool ok;
    if (m_base.kind == ClientBase::Kind::File && k == L"skilleffect.txt") {
        std::wstring e;
        ok = ReadFileBytes(m_base.singleFile, out, e);
    } else {
        ok = m_base.ReadTextdata(name, out);
    }
    if (ok) m_cache[k] = out;
    return ok;
}

const SeDoc* Studio::BaseSe(std::wstring* err) {
    if (!m_seLoaded) {
        m_seLoaded = true;
        Bytes b;
        if (!BaseFile(L"skilleffect.txt", b)) m_seErr = L"skilleffect.txt okunamadı / cannot read skilleffect.txt";
        else m_seOk = m_se.Load(b, m_seErr);
    }
    if (!m_seOk && err) *err = m_seErr;
    return m_seOk ? &m_se : nullptr;
}

const RefData& Studio::BaseRef(const Progress& prog) {
    if (!m_refLoaded) {
        m_refLoaded = true;
        if (prog) prog(3, L"characterdata / skilldata");
        LoadRefData(m_ref, [this](const std::wstring& n, Bytes& b) { return BaseFile(n, b); });
        m_ref.desc = m_base.Describe(false);
    }
    return m_ref;
}

const std::unordered_map<int, std::wstring>& Studio::ItemIds() {
    if (!m_itemLoaded) {
        m_itemLoaded = true;
        Bytes ib;
        if (BaseFile(L"itemdata.txt", ib)) {
            TableDoc idx;
            std::wstring e;
            idx.Load(ib, e);
            for (size_t i = 0; i < idx.Count(); ++i) {
                if (!idx.IsData(i)) continue;
                Bytes fb;
                if (!BaseFile(Trim(idx.eff[i]), fb)) continue;
                TableDoc d;
                d.Load(fb, e);
                for (size_t k = 0; k < d.Count(); ++k) {
                    if (!d.IsData(k)) continue;
                    const auto c = d.Cells(k);
                    long long id = 0;
                    if (c.size() > 2 && ParseInt(c[1], id)) m_itemIds[static_cast<int>(id)] = c[2];
                }
            }
        }
    }
    return m_itemIds;
}

// ------------------------------------------------------------------------------------------- bucket tables
void Studio::MergeBucket(const TableSpec& sp, const std::vector<const TableRow*>& rows, const Options& opt, StudioResult& r,
                         TablePlan& tp) {
    Bytes ib;
    if (!BaseFile(sp.file, ib)) {
        tp.bad += static_cast<int>(rows.size());
        r.issues.push_back(Mk(Sev::Error, L"T5", rows.empty() ? -1 : rows.front()->listLine, L"", sp.id,
                              Fmt(L"İstemcide %s yok: %s satırları yerleştirilemez.", sp.file.c_str(), sp.id.c_str()),
                              Fmt(L"The client has no %s: %s rows cannot be placed.", sp.file.c_str(), sp.id.c_str())));
        return;
    }
    TableDoc idx;
    std::wstring e;
    idx.Load(ib, e);
    struct Ent {
        long long num;
        std::wstring name;
        int line;
    };
    std::vector<Ent> ents;
    for (size_t i = 0; i < idx.Count(); ++i) {
        if (!idx.IsData(i)) continue;
        const std::wstring nm = Trim(idx.eff[i]);
        size_t p = 0, n = 0;
        long long v = 0;
        if (LastNumber(nm, p, n, v)) ents.push_back({v, nm, static_cast<int>(i)});
    }
    const std::wstring example = ents.empty() ? std::wstring() : ents.back().name;
    const std::unordered_map<int, std::wstring>* idCode =
        sp.id == L"skilldata" ? &m_ref.skillById : sp.id == L"characterdata" ? &m_ref.charById : &ItemIds();
    std::unordered_map<std::wstring, int> codeId;
    codeId.reserve(idCode->size());
    for (const auto& [id, code] : *idCode) codeId.emplace(code, id);

    struct NR {
        const TableRow* row;
        std::wstring text;
        long long id;
        std::wstring code;
    };
    std::map<long long, std::vector<NR>> byBucket;
    std::set<long long> seenIds;
    std::set<std::wstring> seenCodes;
    for (const auto* row : rows) {
        const std::wstring text = StripEol(row->text);
        const auto c = SplitTabs(text);
        if (static_cast<int>(c.size()) != sp.cells) {
            Issue is = Mk(Sev::Error, L"T1", row->listLine, c.size() > static_cast<size_t>(sp.codeCol) ? c[static_cast<size_t>(sp.codeCol)] : L"", sp.id,
                          Fmt(L"%s satırı %d hücre olmalı, %zu var: istemci sütunları kaydırır.", sp.id.c_str(), sp.cells, c.size()),
                          Fmt(L"A %s row needs %d cells, it has %zu: the client shifts the columns.", sp.id.c_str(), sp.cells, c.size()));
            is.fix = Fix::Pad;
            is.fixArg = std::to_wstring(sp.cells);
            r.issues.push_back(std::move(is));
            ++tp.bad;
            continue;
        }
        long long id = 0;
        if (!ParseInt(c[static_cast<size_t>(sp.idCol)], id) || id <= 0) {
            r.issues.push_back(Mk(Sev::Error, L"T1", row->listLine, c[static_cast<size_t>(sp.codeCol)], sp.id,
                                  L"ID sütunu (1) sayı değil.", L"The ID column (1) is not a number."));
            ++tp.bad;
            continue;
        }
        const std::wstring code = c[static_cast<size_t>(sp.codeCol)];
        if (!seenIds.insert(id).second || !seenCodes.insert(code).second) {
            r.issues.push_back(Mk(Sev::Warn, L"T2", row->listLine, code, sp.id, L"Bu ID/kod listede iki kez: ikincisi atlandı.",
                                  L"This ID/code is twice in the list: the second one is skipped."));
            ++tp.skipped;
            continue;
        }
        auto bc = idCode->find(static_cast<int>(id));
        if (bc != idCode->end() && bc->second != code) {
            r.issues.push_back(Mk(Sev::Error, L"T2", row->listLine, code, sp.id,
                                  Fmt(L"ID %lld istemcide başka bir kayıtta (%s): çakışma, satır yazılmadı.", id, bc->second.c_str()),
                                  Fmt(L"ID %lld belongs to another record in the client (%s): collision, row not written.", id, bc->second.c_str())));
            ++tp.bad;
            continue;
        }
        auto cc = codeId.find(code);
        if (cc != codeId.end() && cc->second != id) {
            r.issues.push_back(Mk(Sev::Error, L"T2", row->listLine, code, sp.id,
                                  Fmt(L"Kod istemcide başka bir ID ile var (%d): çakışma, satır yazılmadı.", cc->second),
                                  Fmt(L"The code exists in the client with another ID (%d): collision, row not written.", cc->second)));
            ++tp.bad;
            continue;
        }
        byBucket[BucketOf(id)].push_back({row, text, id, code});
    }
    std::vector<std::pair<int, std::vector<std::wstring>>> idxIns;
    for (auto& [bucket, nrs] : byBucket) {
        std::sort(nrs.begin(), nrs.end(), [](const NR& a, const NR& b) { return a.id < b.id; });
        const Ent* ent = nullptr;
        for (const auto& en : ents)
            if (en.num == bucket) ent = &en;
        if (ent) {
            Bytes fb;
            if (!BaseFile(ent->name, fb)) {
                r.issues.push_back(Mk(Sev::Error, L"T3", nrs.front().row->listLine, nrs.front().code, sp.id,
                                      Fmt(L"%s dizinde var ama dosya istemcide yok.", ent->name.c_str()),
                                      Fmt(L"%s is listed in the index but the file is missing in the client.", ent->name.c_str())));
                tp.bad += static_cast<int>(nrs.size());
                continue;
            }
            Bytes plain = fb;
            if (sp.encrypted && !SkillDecrypt(fb, plain)) {
                r.issues.push_back(Mk(Sev::Error, L"T3", nrs.front().row->listLine, nrs.front().code, sp.id,
                                      Fmt(L"%s şifresi çözülemedi (//encrypt işareti yok).", ent->name.c_str()),
                                      Fmt(L"%s could not be decrypted (no //encrypt marker).", ent->name.c_str())));
                tp.bad += static_cast<int>(nrs.size());
                continue;
            }
            TableDoc d;
            d.Load(plain, e);
            std::map<long long, int> lineOfId;
            for (size_t i = 0; i < d.Count(); ++i) {
                if (!d.IsData(i)) continue;
                const auto c = d.Cells(i);
                long long v = 0;
                if (c.size() > static_cast<size_t>(sp.idCol) && ParseInt(c[static_cast<size_t>(sp.idCol)], v)) lineOfId[v] = static_cast<int>(i);
            }
            std::vector<std::pair<int, std::vector<std::wstring>>> ins;
            std::vector<std::uint8_t> drop(d.Count(), 0);
            bool changed = false;
            for (const auto& nr : nrs) {
                auto it = lineOfId.find(nr.id);
                if (it != lineOfId.end()) {
                    if (d.text[static_cast<size_t>(it->second)] == nr.text) {
                        ++tp.same;
                    } else if (opt.dup == DupPolicy::Replace || r.list.replaceKeys.count(nr.code)) {
                        drop[static_cast<size_t>(it->second)] = 1;
                        ins.push_back({it->second, {nr.text}});
                        ++tp.replaced;
                        changed = true;
                    } else {
                        Issue is = Mk(Sev::Warn, L"T2", nr.row->listLine, nr.code, sp.id,
                                      Fmt(L"Bu kayıt (ID %lld) istemcide farklı içerikle var: listedeki satır eklenmedi.", nr.id),
                                      Fmt(L"This record (ID %lld) is in the client with different content: the list row was not added.", nr.id));
                        is.fix = Fix::ReplaceKey;
                        is.fixArg = nr.code;
                        r.issues.push_back(std::move(is));
                        ++tp.skipped;
                    }
                    continue;
                }
                auto up = lineOfId.upper_bound(nr.id);
                ins.push_back({up == lineOfId.end() ? d.EndInsert() : up->second, {nr.text}});
                ++tp.added;
                changed = true;
            }
            if (changed) {
                Bytes out = d.Splice(ins, &drop);
                if (sp.encrypted) out = SkillEncrypt(out);
                r.outputs.push_back({ent->name, std::move(out), false, sp.id});
                tp.outFiles.push_back(ent->name);
            }
        } else {
            const std::wstring name = BucketName(example, sp.pattern, bucket);
            TableDoc d;
            d.Load(Bytes{0xFF, 0xFE}, e);
            std::vector<std::wstring> lines;
            for (const auto& nr : nrs) lines.push_back(nr.text);
            Bytes out = d.Splice({{0, lines}});
            if (sp.encrypted) out = SkillEncrypt(out);
            r.outputs.push_back({name, std::move(out), true, sp.id});
            tp.outFiles.push_back(name);
            tp.created = true;
            tp.added += static_cast<int>(nrs.size());
            int pos = idx.EndInsert();
            const Ent* below = nullptr;
            for (const auto& en : ents)
                if (en.num < bucket && (!below || en.num > below->num)) below = &en;
            if (below) pos = below->line + 1;
            else if (!ents.empty()) pos = ents.front().line;
            idxIns.push_back({pos, {name}});
            r.issues.push_back(Mk(Sev::Info, L"T6", nrs.front().row->listLine, name, sp.id,
                                  Fmt(L"Yeni dosya %s (%zu satır, ID %lld-%lld) + %s dizinine satır.", name.c_str(), nrs.size(),
                                      bucket - 5000, bucket - 1, sp.file.c_str()),
                                  Fmt(L"New file %s (%zu rows, IDs %lld-%lld) + a line in the %s index.", name.c_str(), nrs.size(),
                                      bucket - 5000, bucket - 1, sp.file.c_str())));
        }
    }
    if (!idxIns.empty()) {
        r.outputs.push_back({sp.file, idx.Splice(idxIns), false, sp.id});
        tp.outFiles.push_back(sp.file);
    }
}

// ------------------------------------------------------------------------------------------- shared tables
void Studio::MergeShared(const TableSpec& sp, const std::vector<const TableRow*>& rows, const Options& opt, StudioResult& r,
                         TablePlan& tp) {
    Bytes b;
    std::wstring e;
    bool exists = BaseFile(sp.file, b);
    if (!exists) {
        if (sp.cp949) {
            r.issues.push_back(Mk(Sev::Error, L"T5", rows.empty() ? -1 : rows.front()->listLine, L"", sp.id,
                                  Fmt(L"İstemcide %s yok.", sp.file.c_str()), Fmt(L"The client has no %s.", sp.file.c_str())));
            tp.bad = static_cast<int>(rows.size());
            return;
        }
        b = Bytes{0xFF, 0xFE};
        tp.created = true;
        r.issues.push_back(Mk(Sev::Warn, L"T6", rows.empty() ? -1 : rows.front()->listLine, L"", sp.id,
                              Fmt(L"İstemcide %s yok: yeni dosya oluşturulur (istemci bu tabloyu okuyor mu, denetleyin).", sp.file.c_str()),
                              Fmt(L"The client has no %s: a new file is created (check that the client reads this table).", sp.file.c_str())));
    }
    TableDoc d;
    d.Load(b, e);
    std::unordered_map<std::wstring, int> lineOfKey;
    bool trailEmpty = false;
    int trailN = 0;
    for (size_t i = 0; i < d.Count(); ++i) {
        if (!d.IsData(i)) continue;
        const auto c = d.Cells(i);
        lineOfKey.emplace(KeyOf(c, sp.keyCols, d.eff[i]), static_cast<int>(i));
        if (!c.empty() && c.back().empty()) ++trailN;
    }
    trailEmpty = d.dataRows > 0 && trailN * 2 > d.dataRows;
    const int want = d.dataRows > 0 ? d.dominantCells : 0;
    const int pos = d.EndInsert();
    TextCols lay;
    std::vector<std::pair<int, std::vector<std::wstring>>> ins;
    std::vector<std::uint8_t> drop(d.Count(), 0);
    std::vector<std::wstring> tail;
    std::unordered_set<std::wstring> seen;
    for (const auto* row : rows) {
        const std::wstring text = StripEol(row->text);
        const auto c = SplitTabs(text);
        const std::wstring key = KeyOf(c, sp.keyCols, text);
        const std::wstring keyShow = c.size() > 1 ? c[1] : text.substr(0, 40);
        if (want > 0 && static_cast<int>(c.size()) != want) {
            // fewer cells: later columns are missing (error); more: the client's layout is narrower (extra cells unread)
            const bool fewer = static_cast<int>(c.size()) < want;
            std::wstring mapped, how;
            if (StartsWith(sp.id, L"textdata")) {
                if (!lay.done) lay = TextLayout(d, sp, m_pool.get(), static_cast<int>(c.size()));
                mapped = MapTextRow(c, lay, want, how);
                // the row is already in the client in the client's layout (a re-applied / re-validated package)
                auto it = lineOfKey.find(KeyOf(SplitTabs(mapped), sp.keyCols, mapped));
                if (it != lineOfKey.end() && d.text[static_cast<size_t>(it->second)] == mapped && seen.insert(key).second) {
                    ++tp.same;
                    continue;
                }
            }
            Issue is = !mapped.empty()
                ? Mk(fewer ? Sev::Error : Sev::Warn, L"T1", row->listLine, keyShow, sp.id,
                     Fmt(L"Ad satırı %zu sütunlu, istemcinin %s dosyası %d sütunlu: oyun adı başka bir sütundan okur (yanlış dil ya da boş ad).",
                         c.size(), sp.file.c_str(), want),
                     Fmt(L"The name row has %zu columns, the client's %s has %d: the game reads the name from another column (wrong language or empty).",
                         c.size(), sp.file.c_str(), want))
                : Mk(fewer ? Sev::Error : Sev::Warn, L"T1", row->listLine, keyShow, sp.id,
                     Fmt(L"Satırda %zu sütun var, istemcinin %s dosyasında %d: istemci sütunları farklı okur%s.", c.size(), sp.file.c_str(),
                         want, fewer ? L" (eksik sütunlar)" : L" (fazla sütunlar okunmaz)"),
                     Fmt(L"The row has %zu columns, the client's %s has %d: the client reads the columns differently%s.", c.size(),
                         sp.file.c_str(), want, fewer ? L" (missing columns)" : L" (extra columns are not read)"));
            is.fix = Fix::Pad;
            if (!mapped.empty()) {
                is.fixArg = L"row:" + mapped;
                is.howTr = L"Düzelt: satır istemcinin düzenine çevrilir: " + how;
                is.howEn = L"Fix: the row is converted to the client's layout: " + how;
            } else {
                is.fixArg = std::to_wstring(want) + (trailEmpty ? L"t" : L"");
                is.howTr = fewer ? Fmt(L"Düzelt: eksik %d sütun '0' ile tamamlanır (yalnız yeni satır).", want - static_cast<int>(c.size()))
                                 : Fmt(L"Düzelt: fazla %d sütun kırpılır (yalnız yeni satır).", static_cast<int>(c.size()) - want);
                is.howEn = fewer ? Fmt(L"Fix: the %d missing columns are filled with '0' (new row only).", want - static_cast<int>(c.size()))
                                 : Fmt(L"Fix: the %d extra columns are trimmed (new row only).", static_cast<int>(c.size()) - want);
            }
            r.issues.push_back(std::move(is));
            ++tp.bad;
            continue;
        }
        if (!seen.insert(key).second) {
            r.issues.push_back(Mk(Sev::Warn, L"T2", row->listLine, keyShow, sp.id, L"Aynı anahtar listede iki kez: ikincisi atlandı.",
                                  L"Same key twice in the list: the second one is skipped."));
            ++tp.skipped;
            continue;
        }
        auto it = lineOfKey.find(key);
        if (it != lineOfKey.end()) {
            if (d.text[static_cast<size_t>(it->second)] == text) {
                ++tp.same;
            } else if (opt.dup == DupPolicy::Replace || r.list.replaceKeys.count(keyShow)) {
                drop[static_cast<size_t>(it->second)] = 1;
                ins.push_back({it->second, {text}});
                ++tp.replaced;
            } else {
                Issue is = Mk(Sev::Warn, L"T2", row->listLine, keyShow, sp.id,
                              L"Bu satır istemcide farklı içerikle var: listedeki satır eklenmedi.",
                              L"This row is in the client with different content: the list row was not added.");
                is.fix = Fix::ReplaceKey;
                is.fixArg = keyShow;
                r.issues.push_back(std::move(is));
                ++tp.skipped;
            }
            continue;
        }
        tail.push_back(text);
        ++tp.added;
    }
    if (!tail.empty()) ins.push_back({pos, tail});
    if (d.eofInBlock && !tail.empty())
        r.issues.push_back(Mk(Sev::Info, L"T4", -1, L"", sp.id,
                              Fmt(L"%s kapanmamış bir /* bloğuyla bitiyor (satır %d): yeni satırlar bloğun ÖNÜNE kondu, sona değil.", sp.file.c_str(), d.openBlockLine + 1),
                              Fmt(L"%s ends inside an unclosed /* block (line %d): new rows go BEFORE it, not at the end.", sp.file.c_str(), d.openBlockLine + 1)));
    if (!ins.empty()) {
        r.outputs.push_back({sp.file, d.Splice(ins, &drop), tp.created, sp.id});
        tp.outFiles.push_back(sp.file);
    }
}

// ------------------------------------------------------------------------------------------- run
StudioResult Studio::Run(const std::wstring& listText, const std::wstring& listDesc, const Options& opt, bool checkRes,
                         const Progress& prog) {
    StudioResult r;
    if (prog) prog(1, L"liste / list");
    TakeEncLosses();  // v5 M3: only the losses of this run are reported (stale ones from an earlier run are dropped)
    r.list = ParseListText(listText, listDesc);
    r.ref = BaseRef(prog);
    std::map<std::wstring, std::vector<const TableRow*>> byTable;
    for (const auto& t : r.list.tables) byTable[SpecFor(t.table).id].push_back(&t);
    // rows the list adds count as known to the client (V5 / V8 / skill facts for the fixes)
    for (const auto* t : byTable[L"characterdata"])
        if (SplitTabs(StripEol(t->text)).size() == 104) r.ref.AddCharacterData(t->text);
    for (const auto* t : byTable[L"skilldata"])
        if (SplitTabs(StripEol(t->text)).size() == 118) r.ref.AddSkillData(t->text);
    int k = 0;
    for (auto& [id, rows] : byTable) {
        if (rows.empty()) continue;
        if (prog) prog(10 + 40 * k++ / std::max<int>(1, static_cast<int>(byTable.size())), id);
        const TableSpec sp = SpecFor(id);
        TablePlan tp;
        tp.table = sp.id;
        tp.file = sp.file;
        tp.listRows = static_cast<int>(rows.size());
        if (!HasBase()) {
            tp.bad = tp.listRows;
        } else if (sp.bucket) {
            MergeBucket(sp, rows, opt, r, tp);
        } else {
            MergeShared(sp, rows, opt, r, tp);
        }
        if (!sp.known)
            r.issues.push_back(Mk(Sev::Info, L"T6", rows.front()->listLine, L"", sp.id,
                                  Fmt(L"%s: bilinen bir tablo değil; satırlar dosya sonuna (son etkin satırın arkasına), tüm satır anahtar olarak eklendi.", sp.file.c_str()),
                                  Fmt(L"%s: not a known table; rows appended after the last active row, whole row used as the key.", sp.file.c_str())));
        r.tables.push_back(tp);
    }
    if (prog) prog(55, L"skilleffect.txt");
    std::wstring seErr;
    const SeDoc* doc = HasBase() ? BaseSe(&seErr) : nullptr;
    if (doc) {
        r.hasSe = true;
        Engine eng;
        r.issues.insert(r.issues.end(), r.list.issues.begin(), r.list.issues.end());
        if (!r.list.rows.empty()) {
            r.plan = eng.MakePlan(*doc, r.list, r.ref, opt);
            r.merge = eng.Merge(*doc, r.list, r.plan, opt);
            auto v = eng.Validate(*doc, r.list, r.ref, r.plan);
            // Validate repeats the list issues: keep one copy
            v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(std::min(v.size(), r.list.issues.size())));
            r.issues.insert(r.issues.end(), v.begin(), v.end());
            r.issues.insert(r.issues.end(), r.merge.issues.begin(), r.merge.issues.end());
            r.verified = r.merge.verified;
            if (r.merge.verified && r.plan.newRows + r.plan.replacedRows + r.plan.removedRows > 0)
                r.outputs.insert(r.outputs.begin(), OutFile{L"skilleffect.txt", r.merge.bytes, false, L""});
        }
        const auto fh = eng.FileHealth(*doc);
        r.issues.insert(r.issues.end(), fh.begin(), fh.end());
    } else {
        r.issues.insert(r.issues.end(), r.list.issues.begin(), r.list.issues.end());
        if (!r.list.rows.empty() || !HasBase()) {
            r.verified = r.list.rows.empty();
            r.issues.push_back(Mk(Sev::Error, L"V1", -1, L"", L"",
                                  HasBase() ? L"İstemcinin skilleffect.txt dosyası okunamadı: " + seErr
                                            : std::wstring(L"İstemci açılmadı: önce Media.pk2 / istemci klasörü açın."),
                                  HasBase() ? L"The client's skilleffect.txt could not be read: " + seErr
                                            : std::wstring(L"No client opened: open Media.pk2 / the client folder first.")));
        }
    }
    if (HasBase()) CheckIndexes(r);
    if (checkRes && HasBase()) {
        if (prog) prog(75, L"kaynaklar / resources");
        CheckResources(r);
    }
    if (HasBase()) CheckCoverage(r);
    // v5 M3: a table row with a character the client's code page (CP949) does not have would be written changed. The
    // loss is reported (ENC2, Error) and noted again: the writers (WriteFileAtomic / Pk2WriteCopy) refuse those bytes,
    // so Apply cannot write the changed row even when the error is ignored.
    for (auto& l : TakeEncLosses()) {
        int line = -1;
        std::wstring table;
        for (const auto& t : r.list.tables)
            if (StripEol(t.text) == l.text) {
                line = t.listLine;
                table = t.table;
                break;
            }
        std::wstring tr = L"Bu satırda istemcinin yazı tablosunda (CP949) olmayan bir harf var; satır değişerek yazılırdı: " + EncLossText(l.text, l.pos, false);
        std::wstring en = L"This row has a character the client's code page (CP949) does not have; the row would be written changed: " + EncLossText(l.text, l.pos, true);
        if (l.cell >= 0) {
            tr += Fmt(L" (sütun %d)", l.cell + 1);
            en += Fmt(L" (column %d)", l.cell + 1);
        }
        r.issues.push_back(Mk(Sev::Error, L"ENC2", line, L"", table, std::move(tr), std::move(en)));
        NoteEncLoss(std::move(l));
    }
    Annotate(r);
    for (auto& i : r.issues) Explain(i);
    int seActive = (r.merge.verified ? r.merge.newActive : 0) + r.plan.sameRows;
    int tabActive = 0;
    for (const auto& t : r.tables) tabActive += t.added + t.replaced + t.same;
    r.rowsTotal = static_cast<int>(r.list.rows.size() + r.list.tables.size());
    r.rowsActive = seActive + tabActive;
    std::set<int> broken;  // read by the client but useless: the skill / creature they belong to is unknown
    for (const auto& i : r.issues) {
        r.errors += i.sev == Sev::Error;
        r.warns += i.sev == Sev::Warn;
        r.infos += i.sev == Sev::Info;
        if (i.sev == Sev::Error && i.listLine > 0 && (i.rule == L"V8" || i.rule == L"V5")) broken.insert(i.listLine);
    }
    r.rowsWorking = std::max(0, r.rowsActive - static_cast<int>(broken.size()));
    for (const auto& x : r.res) r.missing += x.state == 0;
    if (prog) prog(100, L"");
    return r;
}

// ------------------------------------------------------------------------------------------- fix annotations
namespace {

struct PoolIndex {
    const Package* p = nullptr;
    bool HasSkill(const std::wstring& k) const {
        if (!p) return false;
        for (const auto& t : p->parsed.tables) {
            if (SpecFor(t.table).id != L"skilldata") continue;
            const auto c = RowCells(t.text);
            if (c.size() > 5 && (c[3] == k || c[5] == k)) return true;
        }
        return false;
    }
    int SkillId(const std::wstring& k) const {
        if (!p) return 0;
        for (const auto& t : p->parsed.tables) {
            if (SpecFor(t.table).id != L"skilldata") continue;
            const auto c = RowCells(t.text);
            long long id = 0;
            if (c.size() > 5 && (c[3] == k || c[5] == k) && ParseInt(c[1], id)) return static_cast<int>(id);
        }
        return 0;
    }
    bool HasChar(const std::wstring& k) const { return Char(k) != nullptr; }
    const TableRow* Char(const std::wstring& k) const {
        if (!p) return nullptr;
        for (const auto& t : p->parsed.tables) {
            if (SpecFor(t.table).id != L"characterdata") continue;
            const auto c = RowCells(t.text);
            if (c.size() > 2 && c[2] == k) return &t;
        }
        return nullptr;
    }
    const TableRow* OwnerOf(int skillId) const {  // characterdata row whose DefaultSkill_1..10 holds the skill
        if (!p || skillId <= 0) return nullptr;
        const std::wstring id = std::to_wstring(skillId);
        for (const auto& t : p->parsed.tables) {
            if (SpecFor(t.table).id != L"characterdata") continue;
            const auto c = RowCells(t.text);
            for (size_t k = 83; k <= 92 && k < c.size(); ++k)
                if (c[k] == id) return &t;
        }
        return nullptr;
    }
    const ListRow* Sa(const std::wstring& k) const {
        if (!p) return nullptr;
        for (const auto& r : p->parsed.rows)
            if (r.sec == Sec::SA && r.key == k) return &r;
        return nullptr;
    }
};

bool ClientSkill(const RefData& ref, const std::wstring& k) { return ref.skillCodes.count(k) || ref.skillGroups.count(k); }

}  // namespace

void Studio::Annotate(StudioResult& r) const {
    PoolIndex pool{m_pool.get()};
    // keys of the client's skilleffect.txt (a stale key may already sit there from an earlier apply)
    std::set<std::wstring> fileKeys;
    if (m_seOk)
        for (const auto& rc : m_se.sim.recs)
            if (!rc.header) fileKeys.insert(rc.Key());
    std::set<std::wstring> listSa;
    for (const auto& row : r.list.rows)
        if (row.sec == Sec::SA) listSa.insert(row.key);
    auto knownSkill = [&](const std::wstring& k) { return ClientSkill(r.ref, k) || pool.HasSkill(k); };
    // the list / package defines this key (not a skill or creature the client already ships)
    auto owned = [&](const std::wstring& k) {
        if (ClientSkill(m_ref, k) || m_ref.chars.count(k)) return false;
        return ClientSkill(r.ref, k) || r.ref.chars.count(k) || pool.HasSkill(k) || pool.HasChar(k) || listSa.count(k) != 0;
    };
    for (auto& i : r.issues) {
        if (i.rule == L"V6" && i.fix == Fix::ReplaceKey) {
            i.autoOk = owned(i.fixArg);
            continue;
        }
        if (i.rule == L"T2" && i.fix == Fix::ReplaceKey) {
            std::wstring k = i.fixArg;
            if (StartsWith(k, L"SN_")) k = k.substr(3);
            for (const wchar_t* suf : {L"_TT_DESC", L"_STUDY"})
                if (EndsWithI(k, suf)) k = k.substr(0, k.size() - wcslen(suf));
            i.autoOk = owned(k) || owned(i.fixArg);
            continue;
        }
        if (i.fix != Fix::None) continue;
        if (i.rule == L"V8" && i.sev == Sev::Error && i.listLine > 0) {
            i.fix = pool.HasSkill(i.key) ? Fix::Pool : pool.p ? Fix::None : Fix::PickExport;
            i.fixArg = i.key;
        } else if (i.rule == L"V9" && i.listLine > 0) {
            const std::wstring& k = i.key;
            i.fixArg = k;
            if (pool.Sa(k)) {
                i.fix = Fix::Pool;
            } else if (knownSkill(k)) {
                i.fix = Fix::GenSa;
            } else {
                // a stale key: no skill has it (an older export's name). Never generate a skillaniset2 row for it.
                std::set<std::wstring> cand;
                auto consider = [&](const std::wstring& c) {
                    if (c.size() > k.size() + 1 && StartsWith(c, k + L"_")) cand.insert(c);
                };
                for (const auto& c : listSa) consider(c);
                if (cand.empty()) {
                    for (const auto& c : r.ref.skillCodes) consider(c);
                    for (const auto& c : r.ref.skillGroups) consider(c);
                    if (pool.p)
                        for (const auto& t : pool.p->parsed.tables)
                            if (SpecFor(t.table).id == L"skilldata") {
                                const auto c = RowCells(t.text);
                                if (c.size() > 5) consider(c[3]);
                            }
                }
                const std::wstring sib = cand.size() == 1 ? *cand.begin() : std::wstring();
                bool dup = false;
                if (!sib.empty()) {  // renamed, would the rows just repeat the list's rows of the current key?
                    std::multiset<std::wstring> have;
                    for (const auto& row : r.list.rows)
                        if (row.sec == Sec::SE && row.key == sib) have.insert(JoinTabs(row.cells));
                    dup = true;
                    for (const auto& row : r.list.rows) {
                        if (row.sec != Sec::SE || row.key != k) continue;
                        auto c = row.cells;
                        if (c.size() > 1) c[1] = sib;
                        auto it = have.find(JoinTabs(c));
                        if (it == have.end()) {
                            dup = false;
                            break;
                        }
                        have.erase(it);
                    }
                }
                i.fix = sib.empty() || dup ? Fix::DropRows : Fix::Rekey;
                i.fixArg2 = sib;
                if (sib.empty() && !pool.p) {  // the skill may still come with its export: ask for that folder first
                    i.fix = Fix::PickExport;
                    i.tr = L"Bu efektin skili ne istemcide ne listede tanımlı: satırlar şimdilik hiçbir skile bağlanmaz.";
                    i.en = L"This effect's skill is defined neither in the client nor in the list: for now the rows reach no skill.";
                    continue;
                }
                const bool inFile = fileKeys.count(k) != 0;
                if (!sib.empty() && dup) {
                    i.tr = L"Bu anahtar hiçbir skile ait değil (eski bir dışa aktarmanın adı); aynı efekt satırları güncel anahtar " + sib +
                           L" ile listede zaten var. Bu satırlar fazlalık." + (inFile ? L" İstemcide de bu eski anahtarın satırları var." : L"");
                    i.en = L"This key belongs to no skill (an older export's name); the same effect rows are already in the list under the current key " +
                           sib + L". These rows are redundant." + (inFile ? L" The client also has rows of this old key." : L"");
                } else if (!sib.empty()) {
                    i.tr = L"Bu anahtar hiçbir skile ait değil (eski bir dışa aktarmanın adı); güncel skill anahtarı " + sib +
                           L". Satırlar bu adla kalırsa hiçbir skile bağlanmaz.";
                    i.en = L"This key belongs to no skill (an older export's name); the current skill key is " + sib +
                           L". Under this name the rows reach no skill.";
                } else {
                    i.tr = std::wstring(L"Bu anahtar ne istemcide ne pakette bir skill: satırlar hiçbir skile bağlanmaz, efekt görünmez.") +
                           (cand.size() > 1 ? L" (Birden çok aday anahtar var; doğrusunu elle seçin.)" : L"");
                    i.en = std::wstring(L"This key is not a skill in the client nor in the package: the rows reach no skill, the effect never shows.") +
                           (cand.size() > 1 ? L" (Several candidate keys; pick the right one by hand.)" : L"");
                }
            }
        } else if (i.rule == L"V5" && i.sev == Sev::Error && i.listLine > 0 && i.key.find(L'~') == std::wstring::npos &&
                   i.tr.find(L"characterdata'sında yok") != std::wstring::npos) {
            i.fix = pool.HasChar(i.key) ? Fix::Pool : pool.p ? Fix::None : Fix::PickExport;
            i.fixArg = i.key;
        } else if (i.rule == L"V4" && i.listLine > 0 && i.table.empty() && i.tr.find(L"kontrol") == std::wstring::npos) {
            for (const auto& row : r.list.rows)
                if (row.listLine == i.listLine) {
                    i.fix = Fix::Pad;
                    i.fixArg = SecName(row.sec);
                }
        } else if ((i.rule == L"V2" || i.rule == L"V4") && i.listLine < 0 && i.fileLine > 0 && i.table.empty() && r.hasSe &&
                   i.sev == Sev::Info) {
            i.fix = Fix::Repair;
            i.autoOk = false;
        }
    }
}

// ------------------------------------------------------------------------------------------- client index health
void Studio::CheckIndexes(StudioResult& r) {
    for (const wchar_t* idxName : {L"skilldataenc.txt", L"characterdata.txt", L"itemdata.txt"}) {
        Bytes b;
        if (!BaseFile(idxName, b)) continue;
        TableDoc d;
        std::wstring e;
        if (!d.Load(b, e)) continue;
        std::vector<std::wstring> miss;
        int total = 0;
        for (size_t i = 0; i < d.Count(); ++i) {
            if (!d.IsData(i)) continue;
            const std::wstring nm = Trim(d.eff[i]);
            if (nm.empty()) continue;
            ++total;
            if (!m_base.HasTextdata(nm)) miss.push_back(nm);
        }
        if (miss.empty()) continue;
        std::wstring first;
        for (size_t k = 0; k < miss.size() && k < 3; ++k) first += (k ? L", " : L"") + miss[k];
        if (miss.size() > 3) first += L"…";
        r.issues.push_back(Mk(Sev::Warn, L"T3", -1, idxName, SpecFor(idxName).id,
                              Fmt(L"İstemcinin %s dizini %d dosya listeliyor ama %zu tanesi yok (%s): bu eksik bir istemci kopyası; "
                                  L"\"skill / canavar istemcide var mı\" denetimleri güvenilmez.",
                                  idxName, total, miss.size(), first.c_str()),
                              Fmt(L"The client's %s index lists %d files but %zu are missing (%s): this is an incomplete client copy; "
                                  L"the \"is the skill / creature in the client\" checks cannot be trusted.",
                                  idxName, total, miss.size(), first.c_str())));
    }
}

// ------------------------------------------------------------------------------------------- package coverage (C1 / C2)
void Studio::CheckCoverage(StudioResult& r) {
    const Package* pool = m_pool.get();
    if (!pool || pool->parsed.rows.empty()) return;
    PoolIndex pi{pool};
    std::unordered_map<std::wstring, std::vector<std::wstring>> fileSe;  // the client's effect rows per key
    if (const SeDoc* doc = BaseSe())
        for (const auto& rc : doc->sim.recs)
            if (!rc.header && rc.sec == Sec::SE) fileSe[rc.Key()].push_back(JoinTabs(rc.cells));
    std::map<std::wstring, int> firstLine;
    std::map<std::wstring, std::vector<std::wstring>> listSe;
    for (const auto& row : r.list.rows) {
        if (row.sec != Sec::SA && row.sec != Sec::SE) continue;
        if (!firstLine.count(row.key)) firstLine[row.key] = row.listLine;
        if (row.sec == Sec::SE) listSe[row.key].push_back(JoinTabs(row.cells));
    }
    // C1: effect rows of these keys that the package has but neither the list nor the client
    std::map<std::wstring, std::vector<std::wstring>> poolSe;
    for (const auto& row : pool->parsed.rows)
        if (row.sec == Sec::SE && firstLine.count(row.key)) poolSe[row.key].push_back(JoinTabs(row.cells));
    for (const auto& [k, prs] : poolSe) {
        std::multiset<std::wstring> have(listSe[k].begin(), listSe[k].end());
        for (const auto& f : fileSe[k]) have.insert(f);
        int miss = 0;
        for (const auto& p : prs) {
            auto it = have.find(p);
            if (it != have.end()) have.erase(it);
            else ++miss;
        }
        if (!miss) continue;
        Issue is = Mk(Sev::Warn, L"C1", firstLine[k], k, L"",
                      Fmt(L"Pakette bu efektin %zu satırı var, %d tanesi ne listede ne istemcide: efekt eksik oynar (vuruşlar / parçacıklar / sesler).",
                          prs.size(), miss),
                      Fmt(L"The package has %zu rows of this effect, %d of them are neither in the list nor in the client: the effect plays incomplete.",
                          prs.size(), miss));
        is.fix = Fix::Pool;
        is.fixArg = k;
        r.issues.push_back(std::move(is));
    }
    // C2: the creature that uses these skills (DefaultSkill_n) is in the package but not in the client / list
    std::map<std::wstring, std::pair<int, std::wstring>> owners;  // code -> (char ID, first skill key)
    for (const auto& [k, line] : firstLine) {
        int sid = 0;
        auto sk = r.ref.skills.find(k);
        if (sk != r.ref.skills.end()) sid = sk->second.id;
        if (!sid) sid = pi.SkillId(k);
        if (!sid) continue;
        if (r.ref.ownerOfSkill.count(sid)) continue;  // its creature is in the client (or the list)
        const TableRow* o = pi.OwnerOf(sid);
        if (!o) continue;
        const auto c = RowCells(o->text);
        if (c.size() < 3 || r.ref.chars.count(c[2])) continue;
        long long cid = 0;
        ParseInt(c[1], cid);
        if (!owners.count(c[2])) owners[c[2]] = {static_cast<int>(cid), k};
    }
    for (const auto& [code, v] : owners) {
        Issue is = Mk(Sev::Warn, L"C2", firstLine[v.second], code, L"characterdata",
                      Fmt(L"Bu skilleri kullanan canavar %s (ID %d) ne istemcide ne listede: skiller hiçbir canavara bağlı olmaz, efekt oyunda görünmez. "
                          L"Canavarın satırları pakette var.", code.c_str(), v.first),
                      Fmt(L"The creature that uses these skills, %s (ID %d), is neither in the client nor in the list: the skills belong to no creature "
                          L"and the effect never shows in game. The package has its rows.", code.c_str(), v.first));
        is.fix = Fix::Pool;
        is.fixArg = code;
        r.issues.push_back(std::move(is));
    }
}

// ------------------------------------------------------------------------------------------- AniShot of a generated row
std::wstring Studio::AniShotFor(const StudioResult& r, const std::wstring& key, const std::wstring& code, std::wstring& note) const {
    PoolIndex pi{m_pool.get()};
    const std::wstring guess = AniShotForCode(code);
    int sid = 0;
    auto sk = r.ref.skills.find(code);
    if (sk != r.ref.skills.end()) sid = sk->second.id;
    if (!sid) sid = pi.SkillId(code);
    std::wstring bsr;
    auto bsrOfSkill = [&](int id) {
        std::wstring b;
        auto o = r.ref.ownerOfSkill.find(id);
        if (o != r.ref.ownerOfSkill.end()) {
            auto ch = r.ref.chars.find(o->second);
            if (ch != r.ref.chars.end()) b = ch->second.bsr;
        }
        if (b.empty())
            if (const TableRow* t = pi.OwnerOf(id)) {
                const auto c = RowCells(t->text);
                if (c.size() > 52) b = c[52];
            }
        return b;
    };
    if (sid) bsr = bsrOfSkill(sid);
    if (bsr.empty()) {
        // an extra skill without a DefaultSkill slot: the creature of a sibling skill (same code up to _ATTACKnn / _SKILLnn)
        std::wstring stem = ToUpper(code);
        for (const wchar_t* t : {L"_ATTACK", L"_SKILL", L"_SUMMON", L"_BUFF", L"_MAGIC"}) {
            const size_t p = stem.rfind(t);
            if (p != std::wstring::npos && p > 0) {
                stem = stem.substr(0, p + 1);
                break;
            }
        }
        if (stem.size() < code.size()) {
            for (const auto& [c, sr] : r.ref.skills)
                if (bsr.empty() && StartsWith(c, stem) && c != code) bsr = bsrOfSkill(sr.id);
            if (bsr.empty() && m_pool)
                for (const auto& t : m_pool->parsed.tables) {
                    if (!bsr.empty() || SpecFor(t.table).id != L"skilldata") continue;
                    const auto c = RowCells(t.text);
                    long long id = 0;
                    if (c.size() > 3 && StartsWith(c[3], stem) && c[3] != code && ParseInt(c[1], id)) bsr = bsrOfSkill(static_cast<int>(id));
                }
        }
    }
    Bytes b;
    bool have = false;
    if (!bsr.empty() && bsr != L"xxx") {
        const std::wstring inner = L"res\\" + bsr;
        std::wstring e;
        if (m_pool && m_pool->HasFile(Arc::Data, inner)) have = ReadFileBytes(m_pool->FilePath(Arc::Data, inner), b, e);
        if (!have) have = m_base.ReadResource(Arc::Data, inner, b);
    }
    if (!have) {
        note = L"tahmin: modelin .bsr dosyası okunamadı / guess: model .bsr not readable";
        return guess;
    }
    const auto roles = BsrRoles(b);
    if (m_pool) {
        auto cl = m_pool->skillClip.find(code);
        if (cl == m_pool->skillClip.end()) cl = m_pool->skillClip.find(key);
        if (cl != m_pool->skillClip.end()) {
            const std::wstring want = L"_" + LowerPath(cl->second) + L".ban";
            for (const auto& [t, f] : roles) {
                const std::wstring tok = AniToken(t);
                if (!tok.empty() && EndsWithI(LowerPath(f), want)) {
                    note = L"modelden / from the model: " + cl->second;
                    return tok;
                }
            }
        }
    }
    for (const auto& [t, f] : roles)
        if (AniToken(t) == guess) {
            note = L"tahmin, model bu rolü taşıyor / guess, the model has this role";
            return guess;
        }
    note = L"tahmin: model bu rolü TAŞIMIYOR, elle düzeltin / guess: the model has NO such role, fix by hand";
    return guess;
}

// ------------------------------------------------------------------------------------------- resources
void Studio::CheckResources(StudioResult& r) {
    r.resChecked = true;
    std::set<std::wstring> seen;
    const Package* pool = m_pool.get();
    auto add = [&](Arc a, std::wstring inner, const std::wstring& kind, const std::wstring& usedBy, int line) -> ResRef* {
        inner = LowerPath(Trim(inner));
        while (!inner.empty() && inner.front() == L'\\') inner.erase(inner.begin());
        if (inner.empty() || inner == L"none") return nullptr;
        const std::wstring key = std::to_wstring(static_cast<int>(a)) + L"|" + inner;
        if (!seen.insert(key).second) return nullptr;
        ResRef x;
        x.arc = a;
        x.inner = inner;
        x.kind = kind;
        x.usedBy = usedBy;
        x.listLine = line;
        x.state = pool && pool->HasFile(a, inner) ? 2 : m_base.Exists(a, inner);
        r.res.push_back(x);
        return &r.res.back();
    };
    auto byExt = [&](const std::wstring& path, const std::wstring& usedBy, int line, const std::wstring& sndPrefix) {
        const std::wstring e = Ext(path);
        if (e == L"efp") add(Arc::Particles, path, L"efp", usedBy, line);
        else if (e == L"bsr") add(Arc::Data, path, L"bsr", usedBy, line);
        else if (e == L"wav") add(Arc::Data, sndPrefix + path, L"wav", usedBy, line);
    };
    for (const auto& row : r.list.rows) {
        const auto& c = row.cells;
        if (row.sec == Sec::SE && c.size() == 28) {
            const std::wstring dir = IsNoneCell(c[17]) ? L"" : c[17];
            if (!IsNoneCell(c[18])) byExt(dir + c[18], row.key, row.listLine, L"prim\\snd\\");
            if (!IsNoneCell(c[23])) byExt(c[23].find(L'\\') != std::wstring::npos ? c[23] : dir + c[23], row.key, row.listLine, L"prim\\snd\\");
            for (int col : {26, 27})
                if (!IsNoneCell(c[static_cast<size_t>(col)])) add(Arc::Data, L"prim\\snd\\" + c[static_cast<size_t>(col)], L"wav", row.key, row.listLine);
        } else if (row.sec == Sec::SA && c.size() == 27) {
            for (int col : {13, 14, 20, 21, 23})
                if (!IsNoneCell(c[static_cast<size_t>(col)])) byExt(c[static_cast<size_t>(col)], row.key, row.listLine, L"prim\\snd\\");
        } else if (row.sec == Sec::CI && c.size() >= 13) {
            for (int col : {4, 5})
                if (!IsNoneCell(c[static_cast<size_t>(col)])) add(Arc::Data, c[static_cast<size_t>(col)], L"bsr", row.key, row.listLine);
            if (!IsNoneCell(c[9])) add(Arc::Particles, L"hiteffect\\" + c[9] + L".efp", L"efp", row.key, row.listLine);
        }
    }
    for (const auto& t : r.list.tables) {
        const std::wstring id = SpecFor(t.table).id;
        const auto c = RowCells(t.text);
        if ((id == L"characterdata" || id == L"itemdata") && c.size() > 54) {
            if (EndsWithI(c[52], L".bsr")) add(Arc::Data, L"res\\" + c[52], L"bsr", c[2], t.listLine);
            if (id == L"itemdata" && EndsWithI(c[54], L".ddj")) add(Arc::Media, L"icon\\" + c[54], L"ddj", c[2], t.listLine);
        } else if (id == L"effectsound" && c.size() > 9) {
            if (!IsNoneCell(c[9])) add(Arc::Data, L"prim\\snd\\" + Trim(c[8]) + Trim(c[9]), L"wav", c[1], t.listLine);
        }
    }
    // models: what the .bsr files reference (skeleton, meshes, materials, animations, palette sounds)
    const size_t nBsr = r.res.size();
    for (size_t i = 0; i < nBsr; ++i) {
        if (r.res[i].kind != L"bsr" || r.res[i].state <= 0) continue;
        Bytes b;
        bool ok = false;
        if (r.res[i].state == 2 && pool) {
            std::wstring e;
            ok = ReadFileBytes(pool->FilePath(r.res[i].arc, r.res[i].inner), b, e);
        } else {
            ok = m_base.ReadResource(r.res[i].arc, r.res[i].inner, b);
        }
        if (!ok) continue;
        const std::wstring user = FileNameOf(r.res[i].inner);
        const int line = r.res[i].listLine;
        for (const auto& ref : BsrRefs(b)) {
            const std::wstring e = Ext(ref);
            const Arc a = e == L"efp" ? Arc::Particles : Arc::Data;
            // archive-name / sound-root prefixes some files write in front of the path inside the archive
            // (.bsr sound sets: "sound\prim\snd\..." = Data.pk2 "prim\snd\..."), resolved the way the transfer does
            std::wstring p = LowerPath(Trim(ref));
            for (const wchar_t* lead : {L"sound\\", L"data\\", L"particles\\", L"media\\"})
                if (StartsWith(p, lead)) {
                    const std::wstring cut = p.substr(wcslen(lead));
                    if ((pool && pool->HasFile(a, cut)) || m_base.Exists(a, cut) > 0) p = cut;
                    break;
                }
            add(a, p, e, user, line);
        }
    }
    int unknown = 0;
    for (const auto& x : r.res) {
        if (x.state == -1) {
            ++unknown;
            continue;
        }
        if (x.state != 0) continue;
        const wchar_t* rule = x.kind == L"efp" ? L"R1" : x.kind == L"wav" ? L"R3" : x.kind == L"ddj" ? L"R4" : L"R2";
        // a transfer package carries every file its source client has: a file missing here is missing in the source
        // client too (the transfer plan reported it, X7) - the object never had it, the game skips it
        const bool fromTransfer = pool && pool->transfer;
        Issue i = fromTransfer
                      ? Mk(Sev::Info, rule, x.listLine, x.usedBy, L"",
                           Fmt(L"Kaynak istemcide de yok: %s içinde %s (nesnenin kendi eksiği; oyun bu dosyayı atlar, aktarım planında X7).", ArcName(x.arc), x.inner.c_str()),
                           Fmt(L"Missing in the source client too: %s has no %s (the object's own gap; the game skips the file; X7 in the transfer plan).",
                               ArcName(x.arc), x.inner.c_str()))
                      : Mk(Sev::Warn, rule, x.listLine, x.usedBy, L"",
                           Fmt(L"Eksik kaynak: %s içinde %s yok (ne istemcide ne pakette).", ArcName(x.arc), x.inner.c_str()),
                           Fmt(L"Missing resource: %s has no %s (neither the client nor the package).", ArcName(x.arc), x.inner.c_str()));
        i.fixArg = x.inner;
        if (!pool) i.fix = Fix::PickExport;  // the export folder ships these files: picking it makes them present
        r.issues.push_back(std::move(i));
    }
    if (unknown)
        r.issues.push_back(Mk(Sev::Info, L"R0", -1, L"", L"",
                              Fmt(L"%d kaynak denetlenemedi: istemci klasöründe Data.pk2 / Particles.pk2 bulunamadı.", unknown),
                              Fmt(L"%d resources not checked: Data.pk2 / Particles.pk2 not found next to the client.", unknown)));
    // every resource file of the package is installed by Apply (copied to the output folder, optional pk2 copy);
    // files the client already has byte-identical are skipped
    if (pool) {
        if (m_resSameFor != pool) {
            m_resSame.clear();
            m_resSameFor = pool;
        }
        for (int a = 0; a < kArcCount; ++a)
            for (const auto& [inner, path] : pool->files[a]) {
                const std::wstring key = std::to_wstring(a) + L"|" + inner;
                auto it = m_resSame.find(key);
                int st = 0;
                if (it == m_resSame.end()) {
                    Bytes pb, cb;
                    std::wstring e;
                    const bool inClient = m_base.ReadResource(static_cast<Arc>(a), inner, cb);
                    st = !inClient ? 0 : (ReadFileBytes(path, pb, e) && pb == cb) ? 1 : 2;
                    m_resSame[key] = st;
                } else {
                    st = it->second;
                }
                if (st == 1) {
                    ++r.resSame;
                    continue;
                }
                r.installs.push_back({static_cast<Arc>(a), inner, path, st == 2});
            }
        std::sort(r.installs.begin(), r.installs.end(), [](const ResInstall& x, const ResInstall& y) {
            return std::make_pair(static_cast<int>(x.arc), x.inner) < std::make_pair(static_cast<int>(y.arc), y.inner);
        });
    }
}

// ------------------------------------------------------------------------------------------- explanations
void Explain(Issue& i) {
    auto set = [&](const std::wstring& tr, const std::wstring& en) {
        if (i.howTr.empty()) i.howTr = tr;
        if (i.howEn.empty()) i.howEn = en;
    };
    const std::wstring& r = i.rule;
    const bool client = !i.autoOk;  // a key the client ships: never part of "Otomatik düzelt"
    if (r == L"V8" && i.fix == Fix::Pool)
        set(L"Düzelt: skillin tanım satırı (SkillData_*ENC.txt) ve adı (textdata_equip&skill) paketten listeye eklenir.",
            L"Fix: the skill's definition row (SkillData_*ENC.txt) and its name (textdata_equip&skill) are added from the package.");
    else if (r == L"V8" && i.fix == Fix::PickExport)
        set(L"Skill ne istemcide ne listede tanımlı. Tanımı TeraStudio dışa aktarma klasöründe: 'Klasör seç…' ile o klasörü seçin, satırlar oradan eklenir.",
            L"The skill is defined neither in the client nor in the list. Its definition is in the TeraStudio export folder: use 'Pick folder…' and the rows are added from it.");
    else if (r == L"V8" && i.sev == Sev::Error)
        set(L"Açılan pakette de bu skill yok: doğru dışa aktarma klasörünü 'Paketi aç' ile açın ya da bu satırı listeden çıkarın.",
            L"The opened package does not have this skill either: open the right export folder with 'Open package' or remove this row from the list.");
    else if (r == L"V8")
        set(L"Anahtar sütununa skill kodu yerine grup adını yazın.", L"Write the group name instead of the skill code in the key column.");
    else if (r == L"V9" && i.fix == Fix::Pool)
        set(L"Düzelt: skillin animasyon satırı paketten eklenir; efekt satırları ona bağlanır.",
            L"Fix: the skill's animation row is added from the package; the effect rows attach to it.");
    else if (r == L"V9" && i.fix == Fix::GenSa)
        set(L"Düzelt: skillin animasyon satırı üretilir (oyunla gelen canavarların şablonu; animasyon rolü modelin .bsr dosyasından, okunamazsa ATTACKn tahmini).",
            L"Fix: the skill's animation row is generated (template of shipped monsters; the animation role from the model's .bsr, an ATTACKn guess when it cannot be read).");
    else if (r == L"V9" && i.fix == Fix::DropRows)
        set(L"Düzelt: bu eski anahtarın satırları listeden çıkarılır (başına // konur); istemcide de varsa oradan kaldırılır (#remove).",
            L"Fix: the rows of this old key are taken out of the list (commented with //); if the client has them too they are removed there (#remove).");
    else if (r == L"V9" && i.fix == Fix::Rekey)
        set(L"Düzelt: satırların anahtarı güncel skill anahtarı " + i.fixArg2 + L" yapılır.",
            L"Fix: the rows get the current skill key " + i.fixArg2 + L".");
    else if (r == L"V9" && i.fix == Fix::PickExport)
        set(L"Skili içeren TeraStudio dışa aktarma klasörünü 'Klasör seç…' ile seçin: skillin tanımı ve animasyon satırı oradan eklenir.",
            L"Pick the TeraStudio export folder that has the skill with 'Pick folder…': its definition and animation row are added from it.");
    else if (r == L"V9")
        set(L"Bu efektin animasyon satırını ekleyin.", L"Add the animation row of this effect.");
    else if (r == L"V5" && i.fix == Fix::Pool)
        set(L"Düzelt: canavarın tanım satırı (CharacterData_*.txt) ve adı (textdata_object) paketten eklenir.",
            L"Fix: the creature's definition row (CharacterData_*.txt) and its name (textdata_object) are added from the package.");
    else if (r == L"V5")
        set(L"Canavarın tanımı gerekli: dışa aktarma klasörünü 'Paketi aç' ile açın.", L"The creature's definition is needed: open the export folder with 'Open package'.");
    else if (r == L"V4" && i.fix == Fix::Pad)
        set(L"Düzelt: boş hücreler sütunun olağan değeriyle doldurulur, eksik sütunlar eklenir, fazlası kırpılır (yalnız yeni satır).",
            L"Fix: empty cells get the column's usual value, missing columns are added, extra ones trimmed (new row only).");
    else if ((r == L"V2" || r == L"V4") && i.fix == Fix::Repair)
        set(L"Bir şey gerekmez. İsterseniz 'Onarılmış kopya…' istemci dosyasının onarılmış ayrı bir kopyasını yazar (istemciye dokunmaz).",
            L"Nothing needed. Optionally 'Repaired copy…' writes a separate repaired copy of the client's file (the client is not touched).");
    else if (r == L"V1")
        set(L"Dosyayı UTF-16LE + BOM + CRLF olarak kaydedin; araç yeni satırları bu biçimde yazar.",
            L"Save the file as UTF-16LE + BOM + CRLF; the tool writes new rows that way.");
    else if (r == L"V2")
        set(L"Bir şey gerekmez: araç satırları yorumun dışına koyar.", L"Nothing needed: the tool puts the rows outside the comment.");
    else if (r == L"V3")
        set(L"Yollarda '/' yerine '\\' kullanın; hücrede '//' veya '/*' olmasın.", L"Use '\\' instead of '/' in paths; no '//' or '/*' in a cell.");
    else if (r == L"V4")
        set(L"Hücreyi düzeltin (kontrol karakteri / boşluk).", L"Fix the cell (control character / spaces).");
    else if ((r == L"V6" || r == L"T2") && i.fix == Fix::ReplaceKey)
        set(std::wstring(L"Düzelt ('Değiştir'): istemcideki satır(lar) listedekiyle değiştirilir (listeye '#replace ") + i.fixArg + L"' eklenir)." +
                (client ? L" Bu anahtar istemcinin kendi kaydı: Otomatik düzelt dokunmaz, ancak siz seçerseniz değişir." : L""),
            std::wstring(L"Fix ('Replace'): the client's row(s) are replaced by the list's ('#replace ") + i.fixArg + L"' is added to the list)." +
                (client ? L" This key is the client's own record: Auto-fix leaves it, only your own click replaces it." : L""));
    else if (r == L"V6" || r == L"D1")
        set(L"Bir şey gerekmez.", L"Nothing needed.");
    else if (r == L"V7")
        set(L"Animasyon satırının ilk sütununu (Service) 1 yapın.", L"Set the first column (Service) of the animation row to 1.");
    else if (r == L"V10" || r == L"V11" || r == L"V12" || r == L"V15" || r == L"V16")
        set(L"Liste sekmesinde satırı açıklamadaki sütuna göre düzeltin (çift tık satıra götürür).",
            L"Correct the row in the List tab using the column named in the description (double-click jumps there).");
    else if (r == L"T1" && i.fix == Fix::Pad)
        set(L"Düzelt: satır istemcinin sütun sayısına getirilir (yalnız yeni satır).", L"Fix: the row gets the client's column count (new row only).");
    else if (r == L"T1")
        set(L"Satırın ID sütununu düzeltin.", L"Correct the ID column of the row.");
    else if (r == L"T2")
        set(L"Bu ID / kod başka bir kayda ait ya da listede iki kez: dışa aktarmayı yeni bir ID ile yeniden alın.",
            L"This ID / code belongs to another record or is twice in the list: re-export with a new ID.");
    else if (r == L"T3" && i.sev == Sev::Warn)
        set(L"Tam istemciyi açın (Media.pk2 ya da istemcinin bütün textdata dosyalarını içeren klasör).",
            L"Open the complete client (Media.pk2 or a folder with all of the client's textdata files).");
    else if (r == L"T3" || r == L"T5")
        set(L"İstemci dosyaları eksik ya da bozuk: istemciyi doğru Media.pk2 ile açın.", L"Client files missing or damaged: open the client with the right Media.pk2.");
    else if (r == L"T4")
        set(L"Bir şey gerekmez: araç satırları yorumun önüne yerleştirir.", L"Nothing needed: the tool places the rows before the comment.");
    else if (r == L"T6")
        set(L"Bilgi: bu dosya da çıktıya yazılır ve Media.pk2'ye girer.", L"Info: this file is written to the output too and goes into Media.pk2.");
    else if ((r == L"R1" || r == L"R2" || r == L"R3" || r == L"R4") && i.fix == Fix::PickExport)
        set(L"Dosya istemcide yok. TeraStudio dışa aktarma klasörü bu dosyaları taşır: 'Klasör seç…' ile seçin; Uygula onları çıktı klasörüne kopyalar.",
            L"The file is not in the client. The TeraStudio export folder ships it: use 'Pick folder…'; Apply copies it to the output folder.");
    else if (r == L"R1" || r == L"R2" || r == L"R3" || r == L"R4")
        set(L"Dosyayı içeren dışa aktarma klasörünü 'Paketi aç' ile açın (Uygula onu çıktıya kopyalar) ya da dosyayı kendiniz ilgili pk2'ye ekleyin.",
            L"Open the export folder that has the file with 'Open package' (Apply copies it to the output) or add the file to its pk2 yourself.");
    else if (r == L"R0")
        set(L"Kaynak denetimi için istemci klasörünü (Media.pk2 + Data.pk2 + Particles.pk2) açın.", L"Open the client folder (Media.pk2 + Data.pk2 + Particles.pk2) to check resources.");
    else if (r == L"C1")
        set(L"Düzelt: bu efektin paketteki bütün satırları (paketteki sırayla) listeye alınır; listedeki eksik grup onlarla değişir.",
            L"Fix: all of this effect's rows of the package are taken into the list (in the package's order), replacing the list's partial group.");
    else if (r == L"C2")
        set(L"Düzelt: canavarın satırları paketten eklenir: tanımı (characterdata), adı, görünüşü (characterInfo), doğma yeri (npcpos) ve sesleri (effectsound).",
            L"Fix: the creature's rows are added from the package: definition (characterdata), name, presentation (characterInfo), spawn (npcpos) and sounds (effectsound).");
    else if (!r.empty() && r[0] == L'L')
        set(L"Liste sözdizimini düzeltin (#section / #table / #after ...).", L"Fix the list syntax (#section / #table / #after ...).");
    else if (r == L"P2")
        set(L"Gelenek: istemci dosya sırasıyla okur, değişiklik gerekmez.", L"Convention only: the client keeps file order, no change needed.");
    else if (r == L"S1")
        set(L"Başlığı istemcinin beklediği gibi yazın: ilk hücre küçük harfle tam '#section', ardından TAB ve bölüm adı (örnek: '#section<TAB>skilleffectset').",
            L"Write the header the way the client expects it: the first cell exactly '#section' in lower case, then a TAB and the section name (e.g. '#section<TAB>skilleffectset').");
    else if (r == L"S2")
        set(L"Bu satırları ilk '#section' başlığının altına taşıyın ya da silin; başlıktan önceki satırları istemci hiç okumaz.",
            L"Move these rows under the first '#section' header or delete them; the client never reads rows before the first header.");
    else if (r == L"S3")
        set(L"Satırdaki NUL / tek CR karakterini silin ve satır sonu olarak CRLF kullanın; dosyayı Tablolar sayfasında açıp kaydetmek bunu düzeltir.",
            L"Delete the NUL / lone CR in the line and use CRLF line ends; opening and saving the file on the Tables page fixes it.");
    else if (r == L"ENC2")
        set(L"Bu harfi değiştirin: dosya / klasör adlarında yalnız İngilizce harf, rakam ve '_' kullanın (ş, ğ, ı, ç, ö, ü, İ olmaz). Korece metin sorun değildir.",
            L"Change this character: use only English letters, digits and '_' in file / folder names (no ş, ğ, ı, ç, ö, ü, İ). Korean text is fine.");
    else if (r == L"P3")
        set(L"Bilgi: vSRO istemcisi 2 GB'tan büyük pk2 dosyalarını okur; bir şey yapmanız gerekmez.",
            L"Info: the vSRO client reads pk2 files larger than 2 GB; nothing to do.");
    else
        set(L"-", L"-");
}

// ------------------------------------------------------------------------------------------- fixes on the list
std::wstring Studio::ApplyFixes(const std::wstring& listText, const StudioResult& r, const std::vector<size_t>& which,
                                std::vector<std::wstring>& log, int& applied) const {
    applied = 0;
    // physical lines of the list (CRLF / LF kept per line)
    std::vector<std::wstring> lines;
    std::vector<std::wstring> ends;
    {
        size_t a = 0;
        while (a < listText.size()) {
            size_t b = listText.find(L'\n', a);
            const bool last = b == std::wstring::npos;
            if (last) b = listText.size();
            std::wstring l = listText.substr(a, b - a);
            std::wstring end = last ? L"" : L"\n";
            if (!l.empty() && l.back() == L'\r') {
                l.pop_back();
                end = L"\r\n";
            }
            lines.push_back(l);
            ends.push_back(end);
            a = b + 1;
        }
    }
    std::set<std::wstring> fileKeys;
    if (m_seOk)
        for (const auto& rc : m_se.sim.recs)
            if (!rc.header) fileKeys.insert(rc.Key());
    std::set<std::wstring> poolSkill, poolSa, poolChar, genSa, poolSeKeys, poolOwners, replaceKeys, removeKeys, dropKeys;
    std::map<std::wstring, std::wstring> rekey;
    for (size_t k : which) {
        if (k >= r.issues.size()) continue;
        const Issue& i = r.issues[k];
        switch (i.fix) {
            case Fix::Pool:
                if (i.rule == L"V8") poolSkill.insert(i.fixArg);
                else if (i.rule == L"V9") poolSa.insert(i.fixArg);
                else if (i.rule == L"V5") poolChar.insert(i.fixArg);
                else if (i.rule == L"C1") poolSeKeys.insert(i.fixArg);
                else if (i.rule == L"C2") poolOwners.insert(i.fixArg);
                break;
            case Fix::GenSa: genSa.insert(i.fixArg); break;
            case Fix::ReplaceKey:
                if (!r.list.replaceKeys.count(i.fixArg)) replaceKeys.insert(i.fixArg);
                break;
            case Fix::DropRows: dropKeys.insert(i.fixArg); break;
            case Fix::Rekey: rekey[i.fixArg] = i.fixArg2; break;
            case Fix::Pad: {
                if (i.listLine <= 0 || static_cast<size_t>(i.listLine) > lines.size()) break;
                std::wstring& l = lines[static_cast<size_t>(i.listLine - 1)];
                std::wstring nl;
                std::vector<std::wstring> ch;
                if (StartsWith(i.fixArg, L"row:")) {
                    nl = i.fixArg.substr(4);
                    ch.push_back(L"sütunlar istemcinin düzenine çevrildi / columns converted to the client's layout");
                } else if (!i.table.empty()) {
                    const bool trail = !i.fixArg.empty() && i.fixArg.back() == L't';
                    nl = PadTableRow(l, _wtoi(i.fixArg.c_str()), trail);
                    if (nl != l) ch.push_back(L"sütun sayısı / column count");
                } else {
                    nl = PadSeRow(SecFromName(i.fixArg), l, &ch);
                }
                if (nl != l) {
                    l = nl;
                    ++applied;
                    log.push_back(Fmt(L"liste:%d %s: ", i.listLine, i.key.c_str()) + (ch.empty() ? std::wstring(L"düzeltildi") : ch.front()) +
                                  (ch.size() > 1 ? Fmt(L" (+%zu)", ch.size() - 1) : L""));
                }
                break;
            }
            default: break;
        }
    }
    auto comment = [&](int listLine, const wchar_t* why) {
        if (listLine <= 0 || static_cast<size_t>(listLine) > lines.size()) return false;
        std::wstring& l = lines[static_cast<size_t>(listLine - 1)];
        if (StartsWith(Trim(l), L"//")) return false;
        l = std::wstring(L"// [") + why + L"] " + l;
        return true;
    };
    // stale keys: out of the list (commented, so the user still sees them), and out of the client file
    for (const auto& k : dropKeys) {
        int n = 0;
        for (const auto& row : r.list.rows)
            if (row.key == k && comment(row.listLine, L"SkillEffect Studio: eski anahtar / old key")) ++n;
        if (fileKeys.count(k)) removeKeys.insert(k);
        if (n || fileKeys.count(k)) {
            ++applied;
            log.push_back(Fmt(L"- %s: %d liste satırı çıkarıldı / list rows taken out", k.c_str(), n) +
                          (fileKeys.count(k) ? L" + #remove (istemciden de / from the client too)" : L""));
        }
    }
    for (const auto& [k, sib] : rekey) {
        int n = 0;
        for (const auto& row : r.list.rows) {
            if (row.key != k || row.listLine <= 0 || static_cast<size_t>(row.listLine) > lines.size()) continue;
            auto c = SplitTabs(lines[static_cast<size_t>(row.listLine - 1)]);
            for (size_t q = 1; q < c.size(); ++q)
                if (Trim(c[q]) == k) {
                    c[q] = sib;
                    lines[static_cast<size_t>(row.listLine - 1)] = JoinTabs(c);
                    ++n;
                    break;
                }
        }
        if (fileKeys.count(k)) removeKeys.insert(k);
        if (n) {
            ++applied;
            log.push_back(Fmt(L"~ %s -> %s: %d satır / rows", k.c_str(), sib.c_str(), n));
        }
    }
    // rows to append (from the export pool / generated)
    std::vector<std::wstring> addCi, addSa, addSe, addSkill, addSkillName, addChar, addCharName, addNpc, addSnd;
    std::set<std::wstring> have;
    for (const auto& l : lines) have.insert(StripEol(l));
    auto push = [&](std::vector<std::wstring>& v, const std::wstring& row) {
        const std::wstring t = StripEol(row);
        if (have.insert(t).second) v.push_back(t);
    };
    const Package* pool = m_pool.get();
    PoolIndex pi{pool};
    if (pool) {
        // C1: the package's whole effect group of a key replaces the list's partial one (package order kept)
        for (const auto& k : poolSeKeys) {
            int n = 0;
            for (const auto& row : r.list.rows)
                if (row.sec == Sec::SE && row.key == k && comment(row.listLine, L"SkillEffect Studio: paketteki tam grupla değişti / replaced by the package's full group")) {
                    have.erase(StripEol(row.text));
                }
            for (const auto& row : pool->parsed.rows)
                if (row.sec == Sec::SE && row.key == k) {
                    addSe.push_back(StripEol(row.text));
                    ++n;
                }
            ++applied;
            log.push_back(Fmt(L"+ skilleffectset %s: %d satır (paketteki tam grup / the package's full group)", k.c_str(), n));
        }
        // C2: the creature of the skills (+ its skills the list does not have yet)
        std::set<std::wstring> ownerSkillIds;
        for (const auto& code : poolOwners) {
            const TableRow* ch = pi.Char(code);
            if (!ch) continue;
            const auto c = RowCells(ch->text);
            poolChar.insert(code);
            for (size_t q = 83; q <= 92 && q < c.size(); ++q)
                if (c[q] != L"0" && !c[q].empty()) ownerSkillIds.insert(c[q]);
            std::wstring type;
            for (const auto& row : pool->parsed.rows)
                if (row.sec == Sec::CI && row.key == code) {
                    push(addCi, row.text);
                    if (row.cells.size() > 1) type = row.cells[1];
                }
            for (const auto& t : pool->parsed.tables) {
                const std::wstring id = SpecFor(t.table).id;
                const auto tc = RowCells(t.text);
                if (id == L"npcpos" && !tc.empty() && c.size() > 1 && tc[0] == c[1]) push(addNpc, t.text);
                if (id == L"effectsound" && tc.size() > 1 && !type.empty() && tc[1] == type) push(addSnd, t.text);
            }
            ++applied;
            log.push_back(L"+ canavar / creature " + code + L" (characterdata, textdata_object, characterInfo, npcpos, effectsound)");
        }
        // the creature's own skills the list lacks entirely
        if (!ownerSkillIds.empty()) {
            std::set<std::wstring> listKeys;
            for (const auto& row : r.list.rows) listKeys.insert(row.key);
            for (const auto& t : pool->parsed.tables) {
                if (SpecFor(t.table).id != L"skilldata") continue;
                const auto c = RowCells(t.text);
                if (c.size() < 6 || !ownerSkillIds.count(c[1])) continue;
                const std::wstring k = c[5] != L"xxx" && !c[5].empty() ? c[5] : c[3];
                if (listKeys.count(k) || listKeys.count(c[3])) continue;
                poolSkill.insert(c[3]);
                poolSa.insert(k);
                for (const auto& row : pool->parsed.rows)
                    if (row.sec == Sec::SE && row.key == k) push(addSe, row.text);
                log.push_back(L"+ canavarın skili / creature's skill " + k);
            }
        }
        std::set<std::wstring> nameKeys, charNameKeys;
        for (const auto& t : pool->parsed.tables) {
            const std::wstring id = SpecFor(t.table).id;
            const auto c = RowCells(t.text);
            if (id == L"skilldata" && c.size() > 62 && (poolSkill.count(c[3]) || poolSkill.count(c[5]))) {
                push(addSkill, t.text);
                nameKeys.insert(c[62]);
                nameKeys.insert(L"SN_" + c[3]);
                log.push_back(L"+ skilldata " + c[3]);
                ++applied;
            }
            if (id == L"characterdata" && c.size() > 5 && poolChar.count(c[2])) {
                push(addChar, t.text);
                charNameKeys.insert(c[5]);
                log.push_back(L"+ characterdata " + c[2]);
                ++applied;
            }
        }
        for (const auto& t : pool->parsed.tables) {
            const std::wstring id = SpecFor(t.table).id;
            const auto c = RowCells(t.text);
            if (id == L"textdata_equip&skill" && c.size() > 1 && nameKeys.count(c[1])) push(addSkillName, t.text);
            if (id == L"textdata_object" && c.size() > 1 && charNameKeys.count(c[1])) push(addCharName, t.text);
        }
        for (const auto& k : poolSa)
            if (const ListRow* sa = pi.Sa(k)) {
                push(addSa, sa->text);
                log.push_back(L"+ skillaniset2 " + k + L" (paket / package)");
                ++applied;
            }
    }
    for (const auto& k : genSa) {
        std::wstring label = k, code = k;
        auto it = r.ref.skills.find(k);
        if (it == r.ref.skills.end())  // a Basic_Group key: take one member skill
            for (const auto& [c, s] : r.ref.skills)
                if (s.group == k) {
                    it = r.ref.skills.find(c);
                    break;
                }
        if (it != r.ref.skills.end()) {
            code = it->first;
            if (!it->second.name.empty() && it->second.name != L"xxx") label = it->second.name;
        } else if (pool) {  // a skill of the package (its skilldata row comes with the V8 fix)
            for (const auto& t : pool->parsed.tables)
                if (SpecFor(t.table).id == L"skilldata") {
                    const auto c = RowCells(t.text);
                    if (c.size() > 5 && (c[3] == k || c[5] == k)) {
                        code = c[3];
                        if (c[4] != L"xxx" && !c[4].empty()) label = c[4];
                        break;
                    }
                }
        }
        std::wstring note;
        const std::wstring ani = AniShotFor(r, k, code, note);
        push(addSa, MakeSaRow(k, label, ani));
        log.push_back(L"+ skillaniset2 " + k + L" (üretildi / generated: " + ani + L"; " + note + L")");
        ++applied;
    }
    for (const auto& k : replaceKeys) {
        ++applied;
        log.push_back(L"#replace " + k + L" (istemcideki satırlar listedekiyle değişir / the client's rows are replaced)");
    }
    std::wstring out;
    for (size_t i = 0; i < lines.size(); ++i) out += lines[i] + ends[i];
    if (!out.empty() && out.back() != L'\n') out += L"\r\n";
    const bool any = !addCi.empty() || !addSa.empty() || !addSe.empty() || !addSkill.empty() || !addSkillName.empty() || !addChar.empty() ||
                     !addCharName.empty() || !addNpc.empty() || !addSnd.empty() || !replaceKeys.empty() || !removeKeys.empty();
    if (any) {
        wchar_t ts[32];
        time_t t = time(nullptr);
        tm lt{};
        localtime_s(&lt, &t);
        wcsftime(ts, std::size(ts), L"%Y-%m-%d %H:%M", &lt);
        out += std::wstring(L"// ---- SkillEffect Studio otomatik düzeltme / auto-fix ") + ts + L" ----\r\n";
        for (const auto& k : replaceKeys) out += L"#replace " + k + L"\r\n";
        for (const auto& k : removeKeys)
            if (!r.list.removeKeys.count(k)) out += L"#remove " + k + L"\r\n";
        auto block = [&](const wchar_t* hdr, const std::vector<std::wstring>& v) {
            if (v.empty()) return;
            out += std::wstring(hdr) + L"\r\n";
            for (const auto& x : v) out += x + L"\r\n";
        };
        block(L"#section characterInfo", addCi);
        block(L"#section skillaniset2", addSa);
        block(L"#section skilleffectset", addSe);
        block(L"#table skilldata", addSkill);
        block(L"#table textdata_equip&skill", addSkillName);
        block(L"#table characterdata", addChar);
        block(L"#table textdata_object", addCharName);
        block(L"#table npcpos", addNpc);
        block(L"#table effectsound", addSnd);
    }
    return out;
}

// ------------------------------------------------------------------------------------------- output
namespace {

constexpr const wchar_t* kOutMarker = kOutMarkerName;

bool EnsureDir(const std::wstring& d) {
    if (DirExists(d)) return true;
    const std::wstring up = DirOf(d);
    if (!up.empty() && up != d && !EnsureDir(up)) return false;
    return CreateDirectoryW(d.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring Stamp() {
    wchar_t ts[32];
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    wcsftime(ts, std::size(ts), L"%Y%m%d-%H%M%S", &lt);
    return ts;
}

// a file of the output folder: written atomically, the previous version kept as .bak; identical content is left alone
bool WriteWithBak(const std::wstring& path, const Bytes& data, Studio::WriteResult& wr, std::wstring& err, bool* same = nullptr) {
    std::wstring bak;
    if (same) *same = false;
    if (FileExists(path)) {
        Bytes old;
        if (!ReadFileBytes(path, old, err)) return false;
        if (old == data) {
            if (same) *same = true;
            ++wr.unchanged;
            return true;
        }
        bak = path + L".bak";
        if (FileExists(bak)) bak = path + L"." + Stamp() + L".bak";
        if (!WriteFileAtomic(bak, old, err)) return false;
    }
    if (!WriteFileAtomic(path, data, err)) return false;
    wr.undo.emplace_back(path, bak);
    wr.written.push_back(path);
    return true;
}

Bytes Utf8Bom(const std::wstring& s) {
    const std::string u = ToUtf8(s);
    Bytes b{0xEF, 0xBB, 0xBF};
    b.insert(b.end(), u.begin(), u.end());
    return b;
}

// a game folder: the client executable or its archives. anyPk2: any *.pk2 file counts (the chosen folder itself);
// otherwise only Media.pk2 / Data.pk2 (the folders above it - a stray .pk2 in Downloads does not make it a game folder)
bool LooksLikeGame(const std::wstring& d, bool anyPk2) {
    for (const wchar_t* f : {L"sro_client.exe", L"silkroad.exe", L"Media.pk2", L"Data.pk2"})
        if (FileExists(JoinPath(d, f))) return true;
    if (!anyPk2) return false;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(d, L"*.pk2").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool hit = false;
    do {  // the wildcard also matches 8.3 names ("x.pk2v2"): the real extension must be .pk2
        hit = !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && EndsWithI(fd.cFileName, L".pk2");
    } while (!hit && FindNextFileW(h, &fd));
    FindClose(h);
    return hit;
}

const wchar_t* ArcDir(Arc a) { return a == Arc::Data ? L"Data" : a == Arc::Particles ? L"Particles" : L"Media"; }

}  // namespace

bool Studio::OutDirAllowed(const std::wstring& outDir, std::wstring& why) const {
    auto full = [](const std::wstring& p) {
        wchar_t b[MAX_PATH * 4] = {};
        GetFullPathNameW(p.c_str(), static_cast<DWORD>(std::size(b)), b, nullptr);
        std::wstring s = LowerPath(b);
        while (!s.empty() && s.back() == L'\\') s.pop_back();
        return s;
    };
    const std::wstring o = full(outDir);
    auto inside = [&](const std::wstring& d) {
        if (d.empty()) return false;
        const std::wstring f = full(d);
        return o == f || StartsWith(o, f + L"\\");
    };
    if (inside(m_base.root) || inside(m_base.textdataDir) || (m_pool && inside(m_pool->root))) {
        why = L"Çıktı klasörü istemci ya da paket klasörünün içinde olamaz (oyun dosyalarına yazılmaz): " + outDir +
              L"\nThe output folder cannot be inside the client or package folder (game files are never written).";
        return false;
    }
    // never a game folder (one with .pk2 archives / the client exe), nor a folder inside one; the tool's own output
    // folder (marker file) may hold the pk2 COPIES of an earlier Apply
    std::wstring d = o;
    for (bool first = true; !d.empty(); first = false) {
        const bool mine = first && FileExists(JoinPath(d, kOutMarker));
        if (!mine && LooksLikeGame(d, first)) {
            why = L"Bu klasör bir oyun klasörü ya da onun içinde (.pk2 / sro_client.exe var): " + d +
                  L"\nBoş ya da yeni bir klasör seçin; araç oyun klasörüne asla yazmaz.\n"
                  L"This folder is a game folder or inside one (.pk2 / sro_client.exe): pick an empty or new folder; the tool never writes into a game folder.";
            return false;
        }
        const std::wstring up = DirOf(d);
        if (up.empty() || up.size() >= d.size()) break;
        d = up;
    }
    return true;
}

// ------------------------------------------------------------------ output folder binding
namespace {
std::wstring NormDir(const std::wstring& p) {
    if (p.empty()) return p;
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetFullPathNameW(p.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
    std::wstring s = LowerPath(n && n < std::size(buf) ? std::wstring(buf, n) : p);
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}
}  // namespace

OutBinding ReadOutBinding(const std::wstring& outDir) {
    OutBinding b;
    if (outDir.empty()) return b;
    const std::wstring mk = JoinPath(outDir, kOutMarker);
    Bytes raw;
    std::wstring e;
    if (!FileExists(mk) || !ReadFileBytes(mk, raw, e)) return b;
    b.marker = true;
    const std::wstring t = DecodeText(raw);
    size_t i = 0;
    while (i < t.size()) {
        size_t j = t.find_first_of(L"\r\n", i);
        if (j == std::wstring::npos) j = t.size();
        const std::wstring line = t.substr(i, j - i);
        if (StartsWith(line, L"client=")) b.client = Trim(line.substr(7));
        if (StartsWith(line, L"media=")) b.finger = Trim(line.substr(6));
        i = j + 1;
    }
    return b;
}

std::wstring ClientFinger(const ClientBase& c) {
    std::wstring f = c.arcPath[static_cast<int>(Arc::Media)];
    if (f.empty()) f = c.mediaPk2;
    if (f.empty()) f = c.textdataDir;
    if (f.empty()) f = c.singleFile;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (f.empty() || !GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fa)) return L"";
    const long long size = (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : (static_cast<long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    const unsigned long long t = (static_cast<unsigned long long>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
    return Fmt(L"%lld:%llu", size, t);
}

int OutBindingMatch(const OutBinding& b, const ClientBase& c) {
    if (!b.marker || b.client.empty()) return 0;
    const std::wstring bc = NormDir(b.client);
    for (const std::wstring* p : {&c.path, &c.root})
        if (!p->empty() && NormDir(*p) == bc) return 1;
    // the same client copied / moved elsewhere: same Media.pk2 size and time
    if (!b.finger.empty() && b.finger == ClientFinger(c)) return 1;
    return -1;
}

bool WriteOutBinding(const std::wstring& outDir, const ClientBase& c, bool force, std::wstring& err) {
    const OutBinding old = ReadOutBinding(outDir);
    if (!force && OutBindingMatch(old, c) < 0) {
        err = L"Bu çıktı klasörü başka bir istemci için yazılmış (" + old.client + L"). Bu istemci için başka bir klasör seçin: " + outDir +
              L"\nThis output folder was written for another client (" + old.client + L"). Pick another folder for this client.";
        return false;
    }
    const std::wstring client = c.root.empty() ? c.path : c.root;
    const std::wstring text = L"SkillEffect Studio çıktı klasörü / output folder.\r\n"
                              L"Buradaki dosyalar istemciye sizin tarafınızdan eklenir; istemci dosyaları burada değildir.\r\n"
                              L"The files here go into the client by your hand; no client file lives here.\r\n"
                              L"Bu klasör şu istemci içindir / this folder belongs to:\r\n"
                              L"client=" + client + L"\r\nmedia=" + ClientFinger(c) + L"\r\n";
    const Bytes data = Utf8Bom(text);
    const std::wstring mk = JoinPath(outDir, kOutMarker);
    Bytes cur;
    std::wstring e;
    if (FileExists(mk) && ReadFileBytes(mk, cur, e) && cur == data) return true;
    return WriteFileAtomic(mk, data, err);
}

long long Studio::Pk2CopySize(Arc a) const {
    const std::wstring& p = m_base.arcPath[static_cast<int>(a)];
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (p.empty() || !GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return (static_cast<long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
}

bool Studio::Write(const StudioResult& r, const std::wstring& outDir, unsigned pk2Arcs, bool en, WriteResult& wr, std::wstring& err,
                   const Progress& prog) {
    if (!OutDirAllowed(outDir, err)) return false;
    if (!EnsureDir(outDir)) {
        err = L"Klasör oluşturulamadı / cannot create: " + outDir;
        return false;
    }
    if (!WriteOutBinding(outDir, m_base, false, err)) return false;  // the marker records the client (two clients never mix)
    const std::wstring td = JoinPath(outDir, L"Media\\server_dep\\silkroad\\textdata");
    if (!r.outputs.empty() && !EnsureDir(td)) {
        err = L"Klasör oluşturulamadı / cannot create: " + td;
        return false;
    }
    size_t n = 0;
    const size_t total = r.outputs.size() + r.installs.size();
    for (const auto& o : r.outputs) {
        if (prog) prog(static_cast<int>(2 + 30 * n++ / std::max<size_t>(1, total)), o.name);
        bool same = false;
        if (!WriteWithBak(JoinPath(td, o.name), o.bytes, wr, err, &same)) return false;
        wr.log += L"  " + JoinPath(L"Media\\server_dep\\silkroad\\textdata", o.name) + Fmt(L"  (%zu B)", o.bytes.size()) +
                  (same ? (en ? L"  unchanged" : L"  değişmedi") : L"") + L"\r\n";
    }
    // the package's resources (.bsr / .bms / .ban / .efp / .wav / .ddj ...): <out>\Data|Particles|Media\<path in the pk2>
    std::vector<Pk2PutFile> put[kArcCount];
    for (const auto& o : r.outputs) put[0].push_back({std::wstring(kTextdataInner) + o.name, o.bytes});
    for (const auto& x : r.installs) {
        if (prog) prog(static_cast<int>(2 + 30 * n++ / std::max<size_t>(1, total)), x.inner);
        Bytes b;
        if (!ReadFileBytes(x.src, b, err)) return false;
        const std::wstring dst = JoinPath(JoinPath(outDir, ArcDir(x.arc)), x.inner);
        if (!EnsureDir(DirOf(dst))) {
            err = L"Klasör oluşturulamadı / cannot create: " + DirOf(dst);
            return false;
        }
        bool same = false;
        if (!WriteWithBak(dst, b, wr, err, &same)) return false;
        ++wr.resWritten;
        put[static_cast<int>(x.arc)].push_back({x.inner, std::move(b)});
    }
    if (!r.installs.empty())
        wr.log += Fmt(en ? L"  %d resource files -> %s\\Data | Particles | Media\r\n" : L"  %d kaynak dosyası -> %s\\Data | Particles | Media\r\n",
                      wr.resWritten, outDir.c_str());
    // pk2 COPIES: the client's archive copied, the files written into the copy (never into the original)
    const unsigned bits[kArcCount] = {kPk2Media, kPk2Data, kPk2Particles};
    for (int a = 0; a < kArcCount; ++a) {
        if (!(pk2Arcs & bits[a]) || put[a].empty()) continue;
        const std::wstring src = m_base.arcPath[a];
        if (src.empty() || (a == 0 && m_base.kind != ClientBase::Kind::Pk2)) {
            err = Fmt(en ? L"The %s copy needs the client's %s (open the client folder / Media.pk2)." : L"%s kopyası için istemcinin %s dosyası gerekli (istemci klasörünü / Media.pk2'yi açın).",
                      ArcName(static_cast<Arc>(a)), ArcName(static_cast<Arc>(a)));
            return false;
        }
        const std::wstring dst = JoinPath(outDir, ArcName(static_cast<Arc>(a)));
        std::wstring log;
        Pk2CopyOptions co;
        co.backupExisting = false;
        co.flags = kPk2InPlaceOver2G;  // v5 M4: a changed file goes into its old slot only when appending passes 2 GiB
        Pk2CopyReport cr;
        if (!Pk2WriteCopy(src, dst, m_base.key, put[a],
                          [&](int p) {
                              if (prog) prog(35 + a * 20 + p * 20 / 100, ArcName(static_cast<Arc>(a)));
                          },
                          log, err, co, &cr))
            return false;
        if (cr.p3) wr.p3.push_back(en ? cr.p3En : cr.p3Tr);
        if (a == 0) wr.pk2Path = dst;
        wr.pk2Paths.push_back(dst);
        wr.undo.emplace_back(dst, L"");
        wr.written.push_back(dst);
        wr.log += std::wstring(L"  ") + ArcName(static_cast<Arc>(a)) + (en ? L" (copy)\r\n" : L" (kopya)\r\n") + log;
    }
    // server rows of the list -> safe SQL (one batch, conflict check, free-ID print, re-runnable; never the package's own
    // fixed-ID scripts) through the program's writer
    if (m_sql) {
        bool server = false;
        for (const auto& t : r.list.tables) server = server || t.table == L"characterdata" || t.table == L"itemdata" || t.table == L"skilldata";
        if (server) {
            std::vector<std::pair<std::wstring, Bytes>> files;
            if (!m_sql(r, files, wr.sqlNote)) {
                err = L"SQL üretilemedi / SQL could not be generated: " + wr.sqlNote;
                return false;
            }
            const std::wstring sd = JoinPath(outDir, L"Server\\SQL");
            if (!files.empty() && !EnsureDir(sd)) {
                err = L"Klasör oluşturulamadı / cannot create: " + sd;
                return false;
            }
            for (const auto& [name, data] : files) {
                const std::wstring f = JoinPath(sd, name);
                if (!WriteWithBak(f, data, wr, err)) return false;
                wr.sqlFiles.push_back(f);
            }
        }
    }
    wr.reportPath = JoinPath(outDir, en ? L"SkillEffectStudio_report.txt" : L"SkillEffectStudio_rapor.txt");
    if (!WriteWithBak(wr.reportPath, Utf8Bom(Report(r, en, &wr)), wr, err)) return false;
    if (prog) prog(100, L"");
    return true;
}

std::vector<Issue> Studio::Pk2Check(const StudioResult& r, unsigned pk2Arcs) const {
    std::vector<Issue> out;
    std::vector<Pk2PutFile> put[kArcCount];
    for (const auto& o : r.outputs) put[0].push_back({std::wstring(kTextdataInner) + o.name, o.bytes});
    for (const auto& x : r.installs) {
        Bytes b;
        std::wstring e;
        if (ReadFileBytes(x.src, b, e)) put[static_cast<int>(x.arc)].push_back({x.inner, std::move(b)});
    }
    const unsigned bits[kArcCount] = {kPk2Media, kPk2Data, kPk2Particles};
    for (int a = 0; a < kArcCount; ++a) {
        if (!(pk2Arcs & bits[a]) || put[a].empty() || m_base.arcPath[a].empty()) continue;
        Pk2CopyOptions co;
        co.flags = kPk2InPlaceOver2G;
        Pk2CopyReport rep;
        std::wstring log, err;
        if (Pk2PlanCopy(m_base.arcPath[a], m_base.key, put[a], co, rep, log, err) && rep.p3)
            out.push_back(Mk(Sev::Info, L"P3", -1, ArcName(static_cast<Arc>(a)), L"", rep.p3Tr, rep.p3En));  // v6: size note only, never blocks
    }
    for (auto& i : out) Explain(i);
    return out;
}

bool Studio::WriteRepaired(const std::wstring& outDir, bool en, WriteResult& wr, std::wstring& err) {
    if (!OutDirAllowed(outDir, err)) return false;
    const SeDoc* doc = BaseSe(&err);
    if (!doc) return false;
    std::wstring body;
    int filled = 0, closed = 0, left = 0;
    std::vector<std::wstring> notes;
    std::vector<int> recLine(doc->lines.size(), -1);
    for (size_t k = 0; k < doc->sim.recs.size(); ++k)
        if (!doc->sim.recs[k].header) recLine[static_cast<size_t>(doc->sim.recs[k].line)] = static_cast<int>(k);
    for (size_t i = 0; i < doc->lines.size(); ++i) {
        std::wstring l(doc->Line(i));
        const int rk = recLine[i];
        if (rk >= 0) {
            const Rec& rc = doc->sim.recs[static_cast<size_t>(rk)];
            const int want = ExpectedCells(rc.sec);
            const int have = static_cast<int>(rc.cells.size());
            if (want > 0 && have < want && !(rc.sec == Sec::CI && have == 14)) {
                // keep the trailing TAB padding of the line, repair the cells
                size_t e = l.size();
                while (e > 0 && (l[e - 1] == L'\t' || l[e - 1] == L' ')) --e;
                std::vector<std::wstring> ch;
                std::wstring fixed = PadSeRow(rc.sec, l.substr(0, e), &ch);
                if (ClientTokens(fixed).size() == static_cast<size_t>(want)) {
                    l = fixed + l.substr(e);
                    ++filled;
                    notes.push_back(Fmt(L"satır/line %zu: ", i + 1) + (ch.empty() ? L"" : ch.front()));
                } else {
                    ++left;
                }
            }
        }
        body += l;
        body += doc->TermStr(doc->lines[i].term);
    }
    if (doc->sim.eofInBlock) {
        if (!doc->finalTerm) body += L"\r\n";
        // still inside the block: this note line is dropped by the client; "*/" alone ends the block (R4)
        body += L"SkillEffect Studio: kapanmamis blok burada kapatildi / unclosed block closed here\r\n*/\r\n";
        ++closed;
    }
    const std::wstring dir = JoinPath(outDir, L"onarim");
    if (!EnsureDir(dir)) {
        err = L"Klasör oluşturulamadı / cannot create: " + dir;
        return false;
    }
    const std::wstring f = JoinPath(dir, L"skilleffect.txt");
    if (!WriteWithBak(f, doc->Encode(body), wr, err)) return false;
    std::wstring rep = en ? L"Repaired COPY of the client's skilleffect.txt (the client was not changed).\r\n"
                          : L"İstemcinin skilleffect.txt dosyasının onarılmış KOPYASI (istemci değiştirilmedi).\r\n";
    rep += Fmt(en ? L"rows with missing cells filled: %d, left as they were: %d, unclosed /* block closed at the end: %d\r\n"
                  : L"eksik hücresi doldurulan satır: %d, olduğu gibi kalan: %d, sondaki kapanmamış /* bloğu kapatıldı: %d\r\n",
               filled, left, closed);
    for (const auto& x : notes) rep += L"  " + x + L"\r\n";
    rep += en ? L"The rows inside the block stay comments (closing it at the end changes nothing the client reads).\r\n"
              : L"Bloğun içindeki satırlar yorum olarak kalır (sona kapanış eklemek istemcinin okuduğu hiçbir şeyi değiştirmez).\r\n";
    if (!WriteWithBak(JoinPath(dir, en ? L"repair_report.txt" : L"onarim_raporu.txt"), Utf8Bom(rep), wr, err)) return false;
    wr.log += L"  onarim\\skilleffect.txt\r\n";
    return true;
}

std::wstring Studio::Report(const StudioResult& r, bool en, const WriteResult* wr) const {
    std::wstring s;
    wchar_t ts[32];
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    wcsftime(ts, std::size(ts), L"%Y-%m-%d %H:%M", &lt);
    s += std::wstring(en ? L"SkillEffect Studio - report  " : L"SkillEffect Studio - rapor  ") + ts + L"\r\n";
    s += std::wstring(L"==========================================================================\r\n");
    s += (en ? L"Client (read-only): " : L"İstemci (salt okunur): ") + m_base.Describe(en) + L"\r\n";
    if (m_pool) s += (en ? L"Package: " : L"Paket: ") + (m_pool->root.empty() ? m_pool->path : m_pool->root) + L"\r\n";
    s += Fmt(en ? L"List: %zu skilleffect rows, %zu table rows\r\n" : L"Liste: %zu skilleffect satırı, %zu tablo satırı\r\n",
             r.list.rows.size(), r.list.tables.size());
    s += Fmt(en ? L"Result: %d errors, %d warnings, %d info, %d missing resources; rows that will work %d/%d\r\n\r\n"
                : L"Sonuç: %d hata, %d uyarı, %d bilgi, %d eksik kaynak; çalışacak satır %d/%d\r\n\r\n",
             r.errors, r.warns, r.infos, r.missing, r.rowsWorking, r.rowsTotal);
    s += en ? L"Changed files (Media.pk2 \\server_dep\\silkroad\\textdata\\):\r\n" : L"Değişen dosyalar (Media.pk2 \\server_dep\\silkroad\\textdata\\):\r\n";
    for (const auto& o : r.outputs) {
        int added = 0;
        if (o.table.empty()) added = r.plan.newRows;
        s += L"  " + o.name + (o.created ? (en ? L"  [new file]" : L"  [yeni dosya]") : L"") +
             (o.table.empty() ? Fmt(en ? L"  +%d rows" : L"  +%d satır", added) : L"") + L"\r\n";
    }
    if (r.outputs.empty()) s += en ? L"  (none)\r\n" : L"  (yok)\r\n";
    s += en ? L"\r\nTables:\r\n" : L"\r\nTablolar:\r\n";
    for (const auto& tp : r.tables)
        s += Fmt(en ? L"  %-22s list %d, added %d, already there %d, replaced %d, skipped %d, bad %d\r\n"
                    : L"  %-22s liste %d, eklenen %d, zaten var %d, değişen %d, atlanan %d, hatalı %d\r\n",
                 tp.table.c_str(), tp.listRows, tp.added, tp.same, tp.replaced, tp.skipped, tp.bad);
    if (!r.list.rows.empty())
        s += Fmt(en ? L"  %-22s list %zu, new %d, already there %d, replaced %d, removed %d, active %d/%d\r\n"
                    : L"  %-22s liste %zu, yeni %d, zaten var %d, değişen %d, kaldırılan %d, etkin %d/%d\r\n",
                 L"skilleffect", r.list.rows.size(), r.plan.newRows, r.plan.sameRows, r.plan.replacedRows, r.plan.removedRows,
                 r.merge.newActive, r.merge.newExpected);
    s += en ? L"\r\nIssues:\r\n" : L"\r\nSorunlar:\r\n";
    for (const auto& i : r.issues) {
        const wchar_t* sv = i.sev == Sev::Error ? (en ? L"ERROR" : L"HATA ") : i.sev == Sev::Warn ? (en ? L"WARN " : L"UYARI") : (en ? L"info " : L"bilgi");
        std::wstring where;
        if (i.listLine > 0) where += Fmt(en ? L" list:%d" : L" liste:%d", i.listLine);
        if (i.fileLine > 0) where += Fmt(en ? L" file:%d" : L" dosya:%d", i.fileLine);
        if (!i.table.empty()) where += L" [" + i.table + L"]";
        s += Fmt(L"  [%s] %-4s%s %s  %s\r\n", sv, i.rule.c_str(), where.c_str(), i.key.c_str(), i.Text(en).c_str());
        if (i.sev != Sev::Info && !i.How(en).empty() && i.How(en) != L"-") s += L"        -> " + i.How(en) + L"\r\n";
    }
    if (r.resChecked) {
        int ok = 0, pkg = 0, miss = 0, unk = 0;
        for (const auto& x : r.res) (x.state == 1 ? ok : x.state == 2 ? pkg : x.state == 0 ? miss : unk)++;
        s += Fmt(en ? L"\r\nResources: %zu referenced - %d in the client, %d from the package, %d missing, %d not checkable\r\n"
                    : L"\r\nKaynaklar: %zu başvuru - istemcide %d, paketten %d, eksik %d, denetlenemeyen %d\r\n",
                 r.res.size(), ok, pkg, miss, unk);
        for (const auto& x : r.res)
            if (x.state == 0) s += L"  " + std::wstring(en ? L"MISSING " : L"EKSİK   ") + ArcName(x.arc) + L"  " + x.inner + L"   (" + x.usedBy + L")\r\n";
    }
    if (!r.installs.empty()) {
        s += Fmt(en ? L"\r\nPackage resources Apply installs: %zu (%d more are already identical in the client)\r\n"
                    : L"\r\nUygula'nın kurduğu paket kaynakları: %zu (%d tanesi istemcide zaten aynı)\r\n",
                 r.installs.size(), r.resSame);
        for (const auto& x : r.installs)
            s += std::wstring(L"  ") + ArcDir(x.arc) + L"\\" + x.inner + (x.replaces ? (en ? L"   [replaces the client's file]" : L"   [istemcidekinin yerine]") : L"") + L"\r\n";
    }
    if (wr) {
        s += en ? L"\r\nWritten:\r\n" : L"\r\nYazılanlar:\r\n";
        s += wr->log;
    }
    // ---- next steps (numbered: what to do, in order)
    s += en ? L"\r\nNext steps:\r\n" : L"\r\nSonraki adımlar:\r\n";
    int step = 0;
    auto line = [&](const std::wstring& x) { s += Fmt(L"  %d) ", ++step) + x + L"\r\n"; };
    bool mediaCopy = false, dataCopy = false, partCopy = false;
    if (wr)
        for (const auto& p : wr->pk2Paths) {
            mediaCopy = mediaCopy || EndsWithI(p, L"Media.pk2");
            dataCopy = dataCopy || EndsWithI(p, L"Data.pk2");
            partCopy = partCopy || EndsWithI(p, L"Particles.pk2");
        }
    if (!r.outputs.empty()) {
        if (mediaCopy)
            line((en ? L"Media.pk2 COPY with every change: " : L"Tüm değişiklikleri içeren Media.pk2 KOPYASI: ") + wr->pk2Path +
                 (en ? L" - test it, then use it instead of the client's Media.pk2 (keep the original)."
                     : L" - deneyin, sonra istemcinin Media.pk2'si yerine kullanın (orijinali saklayın)."));
        else
            line(en ? L"Put the files of Media\\server_dep\\silkroad\\textdata\\ into Media.pk2 (same path), or tick 'pk2 copy'."
                    : L"Media\\server_dep\\silkroad\\textdata\\ içindeki dosyaları Media.pk2'ye aynı yola ekleyin ya da 'pk2 kopyası'nı seçin.");
    }
    if (!r.installs.empty()) {
        int nd = 0, np = 0, nm = 0;
        for (const auto& x : r.installs) (x.arc == Arc::Data ? nd : x.arc == Arc::Particles ? np : nm)++;
        std::wstring parts;
        auto part = [&](int cnt, bool copied, const wchar_t* dir, const wchar_t* arc) {
            if (!cnt) return;
            if (!parts.empty()) parts += L"; ";
            parts += copied ? Fmt(en ? L"%s COPY (%d files inside)" : L"%s KOPYASI (%d dosya içinde)", arc, cnt)
                            : Fmt(en ? L"%s\\ (%d files) -> %s" : L"%s\\ (%d dosya) -> %s", dir, cnt, arc);
        };
        part(nd, dataCopy, L"Data", L"Data.pk2");
        part(np, partCopy, L"Particles", L"Particles.pk2");
        part(nm, mediaCopy, L"Media", L"Media.pk2");
        line((en ? L"Resources of the package (models, animations, effects, sounds): " : L"Paketin kaynak dosyaları (model, animasyon, efekt, ses): ") +
             parts + (en ? L" - same paths inside the archive." : L" - arşivin içinde aynı yollar."));
    }
    if (m_pool && !m_pool->root.empty() && !m_pool->textdataDir.empty())
        line(en ? L"Do NOT put the package's own Client\\Media folder into Media.pk2: its tables are FULL copies made from another base client and "
                  L"would overwrite the merged files of step 1. Only the files written here go into the client."
                : L"Paketin kendi Client\\Media klasörünü Media.pk2'ye KOYMAYIN: oradaki tablolar başka bir istemciden üretilmiş TAM kopyalardır ve "
                  L"1. adımdaki birleştirilmiş dosyaların üstüne yazar. İstemciye yalnız burada yazılan dosyalar girer.");
    // server SQL: only the generated, safe scripts are steps (SQL_AUDIT.md section 4: one batch, no USE, conflict check
    // + first free IDs, re-runnable, UNINSTALL by code + ID). A package's own fixed-ID scripts are never listed as steps.
    std::vector<std::wstring> safe, unsafe;
    if (wr) safe = wr->sqlFiles;
    if (m_pool)
        for (const auto& q : m_pool->sql) (IsSafeSqlScript(q) ? safe : unsafe).push_back(q);
    if (!safe.empty()) {
        line(en ? L"Server (any shard DB, select it in SSMS) - the tool never runs SQL. 1) CHECK.sql: changes nothing, lists ID conflicts and the first "
                  L"free IDs; 2) INSTALL.sql: one transaction, re-runnable, writes nothing when an ID belongs to another object; "
                  L"UNINSTALL.sql only to undo (deletes only rows proven to be these: code + ID):"
                : L"Sunucu (hangi shard DB ise SSMS'te onu seçin) - araç SQL çalıştırmaz. 1) CHECK.sql: hiçbir şeyi değiştirmez, ID çakışmalarını "
                  L"ve ilk boş ID'leri yazar; 2) INSTALL.sql: tek işlem, tekrar çalıştırılabilir, bir ID başka nesnenin ise hiçbir şey yazmaz; "
                  L"UNINSTALL.sql yalnız geri almak için (yalnız kod + ID ile kanıtlanan satırları siler):");
        std::sort(safe.begin(), safe.end(), [](const std::wstring& a, const std::wstring& b) {
            auto rank = [](const std::wstring& f) {
                const std::wstring n = LowerPath(FileNameOf(f));
                return n.find(L"check") != std::wstring::npos ? 0 : n.find(L"uninstall") != std::wstring::npos ? 2 : 1;
            };
            return rank(a) < rank(b);
        });
        for (const auto& q : safe) s += L"       " + q + L"\r\n";
        if (wr && !wr->sqlNote.empty()) s += L"       (" + wr->sqlNote + L")\r\n";
    }
    if (!unsafe.empty()) {
        s += en ? L"\r\n  DO NOT RUN the package's own SQL files below: they use fixed IDs (Msg 2627 'duplicate key' on a database that already "
                  L"uses them), a fixed USE, one transaction per file and no conflict check; a rollback script deletes rows by bare ID and "
                  L"can delete your own data (SQL_AUDIT.md section 4). Use the generated scripts above instead.\r\n"
                : L"\r\n  Paketin kendi SQL dosyalarını ÇALIŞTIRMAYIN: sabit ID kullanırlar (o ID'ler veritabanında varsa Msg 2627 'duplicate key'), "
                  L"sabit USE, dosya başına ayrı işlem ve çakışma denetimi yok; rollback betiği satırları yalnız ID ile siler ve sizin kendi "
                  L"verinizi silebilir (SQL_AUDIT.md bölüm 4). Yukarıdaki üretilmiş betikleri kullanın.\r\n";
        bool spawn = false;
        for (const auto& q : unsafe) {
            s += L"       " + q + L"   (" + (en ? L"unsafe, not a step" : L"güvensiz, adım değil") + L")\r\n";
            spawn = spawn || LowerPath(FileNameOf(q)).find(L"spawn") != std::wstring::npos;
        }
        if (spawn)
            s += en ? L"  This list's spawns (Tab_RefTactics / Hive / Nest) are not generated here (the client has no source for them). The "
                      L"Transfer page writes them (step 3, optional spawn points); or place the monster with your server's spawn tool, using "
                      L"the IDs INSTALL.sql printed.\r\n"
                    : L"  Bu listenin doğma noktaları (Tab_RefTactics / Hive / Nest) burada üretilmez (istemcide kaynağı yok). Aktar sayfası "
                      L"bunları yazar (3. adım, isteğe bağlı doğma yerleri); ya da canavarı sunucunuzun spawn aracıyla INSTALL.sql'in yazdığı "
                      L"ID'lerle yerleştirin.\r\n";
    }
    return s;
}

bool IsSafeSqlScript(const std::wstring& path) {
    Bytes b;
    std::wstring e;
    if (!ReadFileBytes(path, b, e)) return false;
    const std::string head(reinterpret_cast<const char*>(b.data()), std::min<size_t>(b.size(), 1200));
    return head.find("-- SkillEffectStudio transfer:") != std::string::npos;  // generated by transfer/XSql (INSTALL / CHECK / UNINSTALL)
}

}  // namespace se
