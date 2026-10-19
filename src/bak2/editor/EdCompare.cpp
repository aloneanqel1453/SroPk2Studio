// SkillEffect Studio - editor/EdCompare.cpp
#include "EdCompare.h"

#include "EdDoc.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace se::ed {

namespace {

// key column of a row: schema key; for generic tables the first column (1 or 2) that is unique in the file
int GenericKeyCol(const EdDoc& d) {
    for (int c : {1, 2, 0}) {
        std::unordered_set<std::wstring> seen;
        size_t n = 0, dup = 0;
        for (const auto& r : d.rows) {
            if (static_cast<size_t>(c) >= r.cells.size()) continue;
            ++n;
            if (!seen.insert(r.cells[static_cast<size_t>(c)]).second) ++dup;
        }
        if (n > 0 && n >= d.rows.size() * 9 / 10 && dup * 50 <= n) return c;
    }
    return -1;
}

std::vector<std::wstring> RowKeys(const EdDoc& d, int genericKey) {
    std::vector<std::wstring> keys(d.rows.size());
    std::unordered_map<std::wstring, int> occ;
    for (size_t i = 0; i < d.rows.size(); ++i) {
        const EdRow& r = d.rows[i];
        const Schema& s = d.SchemaOf(r);
        int kc = s.keyCol;
        if (d.kind == TKind::Generic) kc = genericKey;
        std::wstring k = std::to_wstring(static_cast<int>(r.kind)) + L"|";
        if (kc >= 0 && static_cast<size_t>(kc) < r.cells.size()) k += Trim(r.cells[static_cast<size_t>(kc)]);
        else k += JoinTabs(r.cells);
        const int n = occ[k]++;
        keys[i] = n ? k + L"#" + std::to_wstring(n) : k;
    }
    return keys;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

}  // namespace

TableDiff CompareTable(const std::wstring& name, const Bytes& a, const Bytes& b, size_t maxRows) {
    TableDiff t;
    t.name = name;
    t.kind = KindFromName(name);
    if (a == b) {
        t.st = TableDiff::St::Same;
        t.byteSame = true;
        return t;
    }
    EdDoc da, db;
    std::wstring err;
    if (!da.Load(name, a, err) || !db.Load(name, b, err)) {
        t.st = TableDiff::St::Error;
        t.err = err;
        return t;
    }
    t.colsA = da.base.dominantCells;
    t.colsB = db.base.dominantCells;
    const int gk = da.kind == TKind::Generic ? GenericKeyCol(da) : -1;
    const auto ka = RowKeys(da, gk), kb = RowKeys(db, gk);
    std::unordered_map<std::wstring, size_t> ia;
    ia.reserve(ka.size() * 2);
    for (size_t i = 0; i < ka.size(); ++i) ia[ka[i]] = i;
    std::unordered_set<std::wstring> inB;
    inB.reserve(kb.size() * 2);
    auto push = [&](RowDiff&& r) {
        if (t.rows.size() < maxRows) t.rows.push_back(std::move(r));
        else t.truncated = true;
    };
    auto keyText = [](const std::wstring& k) { return k.substr(k.find(L'|') + 1); };
    for (size_t j = 0; j < kb.size(); ++j) {
        inB.insert(kb[j]);
        const EdRow& rb = db.rows[j];
        auto it = ia.find(kb[j]);
        if (it == ia.end()) {
            ++t.added;
            RowDiff r;
            r.op = L'+';
            r.key = keyText(kb[j]);
            r.kind = rb.kind;
            r.lineB = rb.line;
            r.b = JoinTabs(rb.cells);
            push(std::move(r));
            continue;
        }
        const EdRow& ra = da.rows[it->second];
        auto trimmed = [](const std::vector<std::wstring>& v) {  // padding TABs at the row end carry no data
            size_t n = v.size();
            while (n > 0 && v[n - 1].empty()) --n;
            return n;
        };
        const size_t na = trimmed(ra.cells), nb = trimmed(rb.cells);
        if (na == nb && std::equal(ra.cells.begin(), ra.cells.begin() + static_cast<std::ptrdiff_t>(na), rb.cells.begin())) {
            ++t.same;
            continue;
        }
        ++t.changed;
        RowDiff r;
        r.op = L'~';
        r.key = keyText(kb[j]);
        r.kind = rb.kind;
        r.lineA = ra.line;
        r.lineB = rb.line;
        const size_t n = std::max(ra.cells.size(), rb.cells.size());
        for (size_t c = 0; c < n; ++c) {
            const std::wstring x = c < ra.cells.size() ? ra.cells[c] : L"";
            const std::wstring y = c < rb.cells.size() ? rb.cells[c] : L"";
            if (x != y) r.cols.push_back(static_cast<int>(c));
        }
        r.a = JoinTabs(ra.cells);
        r.b = JoinTabs(rb.cells);
        push(std::move(r));
    }
    for (size_t i = 0; i < ka.size(); ++i) {
        if (inB.count(ka[i])) continue;
        ++t.removed;
        RowDiff r;
        r.op = L'-';
        r.key = keyText(ka[i]);
        r.kind = da.rows[i].kind;
        r.lineA = da.rows[i].line;
        r.a = JoinTabs(da.rows[i].cells);
        push(std::move(r));
    }
    t.st = t.added + t.removed + t.changed ? TableDiff::St::Changed : TableDiff::St::Same;
    return t;
}

void WalkArchive(const ClientBase& c, Arc a, const std::wstring& dir, std::vector<std::wstring>& out, size_t cap,
                 const std::atomic<bool>* cancel) {
    std::vector<std::wstring> stack{dir};
    while (!stack.empty() && out.size() < cap) {
        if (cancel && cancel->load()) return;
        const std::wstring d = stack.back();
        stack.pop_back();
        std::vector<std::pair<std::wstring, bool>> v;
        if (!c.List(a, d, v)) continue;
        for (const auto& [n, isDir] : v) {
            if (n == L"." || n == L"..") continue;
            const std::wstring p = d.empty() ? n : d + L"\\" + n;
            if (isDir) stack.push_back(p);
            else if (out.size() < cap) out.push_back(p);
        }
    }
}

ClientDiff CompareClients(const ClientBase& A, const ClientBase& B, const CompareOpts& o) {
    const auto t0 = std::chrono::steady_clock::now();
    ClientDiff r;
    r.descA = A.Describe(false);
    r.descB = B.Describe(false);
    auto prog = [&](int p, const std::wstring& w) {
        if (o.progress) o.progress(p, w);
    };
    if (o.tables) {
        const auto la = ListTextdata(A), lb = ListTextdata(B);
        std::map<std::wstring, std::pair<std::wstring, std::wstring>> names;  // lower -> (name in A, name in B)
        for (const auto& n : la) names[Lower(n)].first = n;
        for (const auto& n : lb) names[Lower(n)].second = n;
        std::unordered_set<std::wstring> only;
        for (const auto& n : o.only) only.insert(Lower(n));
        size_t i = 0;
        for (const auto& [low, p] : names) {
            ++i;
            if (o.cancel && o.cancel->load()) break;
            if (!only.empty() && !only.count(low)) continue;
            const std::wstring& name = p.first.empty() ? p.second : p.first;
            prog(static_cast<int>(i * (o.files ? 60 : 100) / std::max<size_t>(names.size(), 1)), name);
            if (p.first.empty() || p.second.empty()) {
                TableDiff t;
                t.name = name;
                t.kind = KindFromName(name);
                t.st = p.first.empty() ? TableDiff::St::OnlyB : TableDiff::St::OnlyA;
                (p.first.empty() ? r.onlyB : r.onlyA)++;
                r.tables.push_back(std::move(t));
                continue;
            }
            Bytes ba, bb;
            const bool okA = A.ReadTextdata(p.first, ba), okB = B.ReadTextdata(p.second, bb);
            TableDiff t;
            if (!okA || !okB) {
                t.name = name;
                t.st = TableDiff::St::Error;
                t.err = L"okunamadı / unreadable";
            } else {
                t = CompareTable(name, ba, bb, o.maxRows);
            }
            if (t.st == TableDiff::St::Same) ++r.tablesSame;
            else if (t.st == TableDiff::St::Changed) ++r.tablesChanged;
            r.rowsAdded += t.added;
            r.rowsRemoved += t.removed;
            r.rowsChanged += t.changed;
            r.tables.push_back(std::move(t));
        }
    }
    if (o.files) {
        int step = 0;
        for (Arc a : {Arc::Data, Arc::Particles, Arc::Media}) {
            ++step;
            if (o.cancel && o.cancel->load()) break;
            if (!A.HasArchive(a) || !B.HasArchive(a)) continue;
            prog(60 + step * 12, ArcName(a));
            std::vector<std::wstring> fa, fb;
            WalkArchive(A, a, L"", fa, o.maxFiles, o.cancel);
            WalkArchive(B, a, L"", fb, o.maxFiles, o.cancel);
            if (fa.size() >= o.maxFiles || fb.size() >= o.maxFiles) r.filesTruncated = true;
            std::unordered_set<std::wstring> sa, sb;
            for (const auto& f : fa) sa.insert(Lower(f));
            for (const auto& f : fb) sb.insert(Lower(f));
            for (const auto& f : fb)
                if (!sa.count(Lower(f))) r.files.push_back({static_cast<int>(a), f, L'+'});
            for (const auto& f : fa)
                if (!sb.count(Lower(f))) r.files.push_back({static_cast<int>(a), f, L'-'});
        }
        std::sort(r.files.begin(), r.files.end(), [](const FileDiff& x, const FileDiff& y) {
            return x.arc != y.arc ? x.arc < y.arc : x.path < y.path;
        });
    }
    prog(100, L"");
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

std::wstring ClientDiff::Report(bool en, size_t perTable) const {
    std::wstring s;
    s += en ? L"CLIENT COMPARE\r\n" : L"İSTEMCİ KARŞILAŞTIRMA\r\n";
    s += L"A: " + descA + L"\r\nB: " + descB + L"\r\n";
    s += Fmt(en ? L"tables: %d same, %d changed, %d only in A, %d only in B; rows +%d -%d ~%d (%.1f s)\r\n"
                : L"tablolar: %d aynı, %d farklı, %d yalnız A'da, %d yalnız B'de; satır +%d -%d ~%d (%.1f sn)\r\n",
             tablesSame, tablesChanged, onlyA, onlyB, rowsAdded, rowsRemoved, rowsChanged, seconds);
    for (const auto& t : tables) {
        if (t.st == TableDiff::St::Same) continue;
        s += L"\r\n== " + t.name + L"  ";
        switch (t.st) {
            case TableDiff::St::OnlyA: s += en ? L"only in A\r\n" : L"yalnız A'da\r\n"; continue;
            case TableDiff::St::OnlyB: s += en ? L"only in B\r\n" : L"yalnız B'de\r\n"; continue;
            case TableDiff::St::Error: s += L"ERROR " + t.err + L"\r\n"; continue;
            default: break;
        }
        s += Fmt(L"+%d -%d ~%d (=%d)", t.added, t.removed, t.changed, t.same);
        if (t.colsA && t.colsB && t.colsA != t.colsB)
            s += Fmt(en ? L"  [column layout differs: %d <> %d cells]" : L"  [sütun düzeni farklı: %d <> %d hücre]", t.colsA, t.colsB);
        s += L"\r\n";
        size_t n = 0;
        const Schema& sch = SchemaFor(t.kind == TKind::SeFile ? TKind::SeAniSet : t.kind);
        for (const auto& r : t.rows) {
            if (n++ >= perTable) {
                s += Fmt(en ? L"  ... %zu more\r\n" : L"  ... %zu satır daha\r\n", t.rows.size() - perTable);
                break;
            }
            std::wstring cols;
            for (int c : r.cols) {
                const Schema& rs = t.kind == TKind::SeFile ? SchemaFor(r.kind) : sch;
                const ColDef* cd = rs.Col(static_cast<size_t>(c));
                cols += (cols.empty() ? L"" : L",") + (cd ? cd->name : L"C" + std::to_wstring(c));
            }
            s += Fmt(L"  %lc %ls", r.op, r.key.c_str());
            if (!cols.empty()) s += L"  [" + cols + L"]";
            s += L"\r\n";
        }
        if (t.truncated) s += en ? L"  (list truncated)\r\n" : L"  (liste kırpıldı)\r\n";
    }
    if (!files.empty()) {
        s += Fmt(en ? L"\r\nFILES: %zu differ%ls\r\n" : L"\r\nDOSYALAR: %zu fark%ls\r\n", files.size(), filesTruncated ? L" (truncated)" : L"");
        static const wchar_t* an[] = {L"Media", L"Data", L"Particles"};
        size_t n = 0;
        for (const auto& f : files) {
            if (n++ >= 400) {
                s += L"  ...\r\n";
                break;
            }
            s += Fmt(L"  %lc %ls\\%ls\r\n", f.op, an[f.arc], f.path.c_str());
        }
    }
    return s;
}

}  // namespace se::ed
