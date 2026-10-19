// SkillEffect Studio - editor/EdDoc.cpp
#include "EdDoc.h"

#include "../core/SkillCrypt.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <map>
#include <set>

namespace se::ed {

namespace {

constexpr const wchar_t* kTdInner = L"server_dep\\silkroad\\textdata";

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

Bytes EncodeText(const TableDoc& t, const std::wstring& s) {
    Bytes b;
    if (t.utf16) {
        b.resize(s.size() * 2);
        if (!s.empty()) std::memcpy(b.data(), s.data(), s.size() * 2);
        return b;
    }
    if (!s.empty()) {
        std::string ex;
        size_t bad = 0;
        if (NarrowExact(949, s, ex, &bad)) {  // v5 M3
            b.assign(ex.begin(), ex.end());
            return b;
        }
        // lossy: the old bytes stay the result, the loss is noted - the writers (WriteFileAtomic / Pk2WriteCopy) refuse
        // data holding them (ENC2), so the edited row is never written with a changed character
        const int n = WideCharToMultiByte(949, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        b.resize(static_cast<size_t>(n > 0 ? n : 0));
        if (n > 0) WideCharToMultiByte(949, 0, s.data(), static_cast<int>(s.size()), reinterpret_cast<char*>(b.data()), n, nullptr, nullptr);
        EncLoss l;
        l.where = L"CP949 tablo satırı / table row";
        l.text = s;
        l.pos = bad;
        l.cell = static_cast<int>(std::count(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(std::min(bad, s.size())), L'\t'));
        l.lossy = b;
        NoteEncLoss(std::move(l));
    }
    return b;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ EdDoc

bool EdDoc::Load(const std::wstring& n, const Bytes& raw, std::wstring& err) {
    name = n;
    kind = KindFromName(n);
    if (kind == TKind::Unknown) kind = TKind::Generic;
    loadedRaw = raw;
    Bytes plain;
    enc = false;
    if (kind == TKind::Generic && IsSkillEnc(raw)) kind = TKind::SkillData;  // a renamed / stray ENC file ("_skilldata_10000enc.txt")
    if (kind == TKind::SkillData && IsSkillEnc(raw)) {
        if (!SkillDecrypt(raw, plain)) {
            err = L"skilldata ENC çözülemedi / cannot decrypt: " + n;
            return false;
        }
        enc = true;
    } else {
        plain = raw;
    }
    if (!base.Load(plain, err)) return false;
    rows.clear();
    sections.clear();
    const int nl = static_cast<int>(base.Count());
    rowOfLine.assign(static_cast<size_t>(nl), -1);
    // block state at the start of every line (client rules R3/R4, same as TableDoc)
    blockAt.assign(static_cast<size_t>(nl) + 1, 0);
    {
        bool in = false;
        for (int i = 0; i < nl; ++i) {
            blockAt[static_cast<size_t>(i)] = in ? 1 : 0;
            const std::wstring& s = base.text[static_cast<size_t>(i)];
            // R3: the first '/' outside a block decides; R4: inside a block only the first '*' is looked at,
            // and the rest of an opening line is never scanned (a "/* ... */" on one line keeps the block open)
            if (!in) {
                const size_t j = s.find(L'/');
                if (j != std::wstring::npos && j + 1 < s.size() && s[j + 1] == L'*') in = true;
            } else {
                const size_t j = s.find(L'*');
                if (j != std::wstring::npos && j + 1 < s.size() && s[j + 1] == L'/') in = false;
            }
        }
        blockAt[static_cast<size_t>(nl)] = in ? 1 : 0;
    }
    TKind cur = kind == TKind::SeFile ? TKind::Unknown : kind;
    for (int i = 0; i < nl; ++i) {
        if (!base.IsData(static_cast<size_t>(i))) continue;
        if (kind == TKind::SeFile) {
            const TKind sk = SectionKind(base.eff[static_cast<size_t>(i)]);
            if (sk != TKind::Unknown || StartsWith(base.eff[static_cast<size_t>(i)], L"#section")) {
                if (!sections.empty()) sections.back().endLine = i;
                SecRange r;
                if (sk != TKind::Unknown) cur = sk;  // R8: an unknown name keeps the previous section
                r.kind = cur;
                r.header = i;
                r.endLine = nl;
                sections.push_back(r);
                continue;
            }
        }
        EdRow r;
        r.uid = static_cast<std::uint64_t>(i) + 1;
        r.line = i;
        r.kind = cur == TKind::Unknown ? TKind::Generic : cur;
        r.cells = base.Cells(static_cast<size_t>(i));
        rowOfLine[static_cast<size_t>(i)] = static_cast<int>(rows.size());
        rows.push_back(std::move(r));
    }
    nextUid = static_cast<std::uint64_t>(nl) + 1;
    if (kind == TKind::Generic) {
        std::vector<std::vector<std::wstring>> sample;
        for (size_t i = 0; i < rows.size() && sample.size() < 3000; ++i) sample.push_back(rows[i].cells);
        generic = InferSchema(sample, name);
    }
    ++revision;
    savedRev = revision;
    return true;
}

const Schema& EdDoc::SchemaOf(TKind k) const {
    if (k == TKind::Generic || k == TKind::Unknown) return generic;
    return SchemaFor(k);
}
const Schema& EdDoc::SchemaOf(const EdRow& r) const { return SchemaOf(r.kind); }

void EdDoc::Reindex() const {
    if (m_indexRev == revision && m_index.size() == rows.size()) return;
    m_index.clear();
    m_index.reserve(rows.size() * 2);
    for (size_t i = 0; i < rows.size(); ++i) m_index[rows[i].uid] = static_cast<int>(i);
    m_indexRev = revision;
}

int EdDoc::IndexOf(std::uint64_t uid) const {
    Reindex();
    auto it = m_index.find(uid);
    return it == m_index.end() ? -1 : it->second;
}
EdRow* EdDoc::Row(std::uint64_t uid) {
    const int i = IndexOf(uid);
    return i < 0 ? nullptr : &rows[static_cast<size_t>(i)];
}
const EdRow* EdDoc::Row(std::uint64_t uid) const {
    const int i = IndexOf(uid);
    return i < 0 ? nullptr : &rows[static_cast<size_t>(i)];
}

std::vector<int> EdDoc::Visible(TKind k) const {
    std::vector<int> v;
    v.reserve(rows.size());
    for (size_t i = 0; i < rows.size(); ++i)
        if (!rows[i].deleted && (k == TKind::Unknown || rows[i].kind == k)) v.push_back(static_cast<int>(i));
    return v;
}

std::vector<std::wstring> EdDoc::OrigCells(const EdRow& r) const {
    if (r.line < 0) return {};
    return base.Cells(static_cast<size_t>(r.line));
}

bool EdDoc::RowEdited(const EdRow& r) const {
    if (r.line < 0) return false;
    return r.cells != base.Cells(static_cast<size_t>(r.line));
}

int EdDoc::Changes(int* added, int* edited, int* removed) const {
    int a = 0, e = 0, d = 0;
    for (const auto& r : rows) {
        if (r.line < 0) a += r.deleted ? 0 : 1;
        else if (r.deleted) ++d;
        else if (RowEdited(r)) ++e;
    }
    if (added) *added = a;
    if (edited) *edited = e;
    if (removed) *removed = d;
    return a + e + d;
}

bool EdDoc::Dirty() const { return Changes() != 0; }

std::vector<int> EdDoc::FindKey(const std::wstring& key, TKind k) const {
    std::vector<int> v;
    for (size_t i = 0; i < rows.size(); ++i) {
        const EdRow& r = rows[i];
        if (r.deleted || (k != TKind::Unknown && r.kind != k)) continue;
        const int kc = SchemaOf(r).keyCol;
        if (kc >= 0 && static_cast<size_t>(kc) < r.cells.size() && r.cells[static_cast<size_t>(kc)] == key)
            v.push_back(static_cast<int>(i));
    }
    return v;
}

int EdDoc::InsertPointAfter(const EdRow& a) const {
    if (a.line >= 0) return a.line + 1;
    return a.before;
}

int EdDoc::InsertPointEnd(TKind k) const {
    if (kind == TKind::SeFile) {
        for (auto it = sections.rbegin(); it != sections.rend(); ++it) {  // the last section of that kind
            const auto& s = *it;
            if (s.kind != k) continue;
            int last = -1;
            for (int i = s.header + 1; i < s.endLine; ++i)
                if (base.IsData(static_cast<size_t>(i))) last = i;
            return last >= 0 ? last + 1 : s.header + 1;
        }
    }
    return base.EndInsert();
}

bool EdDoc::InsideBlock(int before) const {
    if (before < 0) return false;
    if (static_cast<size_t>(before) >= blockAt.size()) return !blockAt.empty() && blockAt.back();
    return blockAt[static_cast<size_t>(before)] != 0;
}

std::vector<TKind> EdDoc::Kinds() const {
    std::vector<TKind> v;
    if (kind == TKind::SeFile) {
        for (const auto& s : sections)
            if (s.kind != TKind::Unknown && std::find(v.begin(), v.end(), s.kind) == v.end()) v.push_back(s.kind);
        return v;
    }
    v.push_back(kind);
    return v;
}

Bytes EdDoc::Save() const {
    const int nl = static_cast<int>(base.Count());
    std::map<int, std::vector<const EdRow*>> ins;
    for (const auto& r : rows)
        if (r.line < 0 && !r.deleted) ins[std::clamp(r.before, 0, nl)].push_back(&r);
    Bytes out;
    out.reserve(base.raw.size() + 4096);
    if (base.bom) out.insert(out.end(), base.raw.begin(), base.raw.begin() + 2);
    else if (base.raw.empty() && !SchemaOf(kind).cp949) {
        out.push_back(0xFF);
        out.push_back(0xFE);
    }
    auto emitNew = [&](int at) {
        auto it = ins.find(at);
        if (it == ins.end()) return;
        for (const EdRow* r : it->second) {
            const Bytes b = base.EncodeRow(JoinTabs(r->cells));
            out.insert(out.end(), b.begin(), b.end());
        }
    };
    for (int i = 0; i < nl; ++i) {
        emitNew(i);
        const TLine& L = base.lines[static_cast<size_t>(i)];
        const EdRow* r = base.IsData(static_cast<size_t>(i)) ? Row(static_cast<std::uint64_t>(i) + 1) : nullptr;
        if (r && r->deleted) continue;
        if (r && RowEdited(*r)) {
            const std::wstring& t = base.text[static_cast<size_t>(i)];
            const std::wstring& e = base.eff[static_cast<size_t>(i)];
            std::wstring pre, post;
            if (t.compare(0, e.size(), e) == 0) post = t.substr(e.size());
            else pre = t.substr(0, t.size() - e.size());
            const Bytes b = EncodeText(base, pre + JoinTabs(r->cells) + post);
            out.insert(out.end(), b.begin(), b.end());
            out.insert(out.end(), base.raw.begin() + static_cast<std::ptrdiff_t>(L.off + L.len),
                       base.raw.begin() + static_cast<std::ptrdiff_t>(L.off + L.len + L.term));
            continue;
        }
        out.insert(out.end(), base.raw.begin() + static_cast<std::ptrdiff_t>(L.off),
                   base.raw.begin() + static_cast<std::ptrdiff_t>(L.off + L.len + L.term));
    }
    if (ins.count(nl) && !base.finalTerm && nl > 0) {
        const Bytes b = base.EncodeRow(L"");
        out.insert(out.end(), b.begin(), b.end());
    }
    emitNew(nl);
    if (enc) return SkillEncrypt(out);
    return out;
}

// ------------------------------------------------------------------------------------------------ restore (v5 M9)

int ApplyBytesAsEdits(Workspace& ws, int id, const Bytes& bytes, std::wstring& err) {
    EdDoc* d = ws.Doc(id);
    if (!d) {
        err = L"tablo açık değil / table not open";
        return -1;
    }
    EdDoc rec;
    if (!rec.Load(d->name, bytes, err)) return -1;
    // rows in file order on both sides; a greedy walk matches equal rows (look-ahead in the client's rows), the rest
    // are deleted (client) / inserted (recovered); a delete + insert of the same kind next to each other = cell edits
    std::vector<int> a, b;
    for (size_t i = 0; i < d->rows.size(); ++i)
        if (!d->rows[i].deleted) a.push_back(static_cast<int>(i));
    for (size_t i = 0; i < rec.rows.size(); ++i)
        if (!rec.rows[i].deleted) b.push_back(static_cast<int>(i));
    auto key = [](const EdDoc& doc, int k) {
        const EdRow& r = doc.rows[static_cast<size_t>(k)];
        return std::to_wstring(static_cast<int>(r.kind)) + L"|" + doc.RowText(r);
    };
    std::unordered_map<std::wstring, std::vector<size_t>> posA;  // text -> positions in a (ascending)
    for (size_t i = 0; i < a.size(); ++i) posA[key(*d, a[i])].push_back(i);
    std::unordered_map<std::wstring, size_t> used;
    struct Step {
        int kind;  // 0 keep, 1 delete (client row), 2 insert (recovered row)
        int ia, ib;
    };
    std::vector<Step> steps;
    size_t ia = 0;
    for (size_t jb = 0; jb < b.size(); ++jb) {
        const std::wstring t = key(rec, b[jb]);
        size_t hit = SIZE_MAX;
        auto it = posA.find(t);
        if (it != posA.end()) {
            size_t& u = used[t];
            while (u < it->second.size() && it->second[u] < ia) ++u;
            if (u < it->second.size() && it->second[u] - ia <= 4000) {
                hit = it->second[u];
                ++u;
            }
        }
        if (hit == SIZE_MAX) {
            steps.push_back({2, -1, b[jb]});
            continue;
        }
        for (; ia < hit; ++ia) steps.push_back({1, a[ia], -1});
        steps.push_back({0, a[ia], b[jb]});
        ++ia;
    }
    for (; ia < a.size(); ++ia) steps.push_back({1, a[ia], -1});
    // a run of deletes next to a run of inserts (either order) pairs up as changed rows
    for (size_t k = 0; k < steps.size();) {
        size_t e = k;
        while (e < steps.size() && steps[e].kind != 0) ++e;
        std::vector<size_t> del, ins;
        for (size_t q = k; q < e; ++q) (steps[q].kind == 1 ? del : ins).push_back(q);
        for (size_t q = 0; q < del.size() && q < ins.size(); ++q) {
            Step& sd = steps[del[q]];
            const Step& si = steps[ins[q]];
            if (d->rows[static_cast<size_t>(sd.ia)].kind != rec.rows[static_cast<size_t>(si.ib)].kind) continue;
            sd.kind = 3;  // edit: client row ia gets the cells of recovered row ib
            sd.ib = si.ib;
            steps[ins[q]].kind = 4;  // consumed
        }
        k = e + (e < steps.size() ? 1 : 0);
    }
    // row indexes shift while rows are inserted: the client rows are addressed by uid from here on
    std::vector<std::uint64_t> uidOf(steps.size(), 0);
    for (size_t k = 0; k < steps.size(); ++k)
        if (steps[k].ia >= 0) uidOf[k] = d->rows[static_cast<size_t>(steps[k].ia)].uid;
    ws.Begin(L"Kurtarılan değişiklikler / recovered changes");
    std::uint64_t anchor = 0;  // the last client row kept / written, in file order
    int edits = 0;
    for (size_t k = 0; k < steps.size(); ++k) {
        const Step& s = steps[k];
        if (s.kind == 0) {
            anchor = uidOf[k];
        } else if (s.kind == 3) {
            const std::uint64_t uid = uidOf[k];
            const std::vector<std::wstring> cells = rec.rows[static_cast<size_t>(s.ib)].cells;
            const EdRow* cur = d->Row(uid);
            const size_t n = std::max(cells.size(), cur ? cur->cells.size() : 0);
            for (size_t c = 0; c < n; ++c) {
                cur = d->Row(uid);
                const std::wstring nv = c < cells.size() ? cells[c] : std::wstring();
                const std::wstring ov = cur && c < cur->cells.size() ? cur->cells[c] : std::wstring();
                if (nv != ov) ws.SetCell(id, uid, static_cast<int>(c), nv);
            }
            anchor = uid;
            ++edits;
        } else if (s.kind == 1) {
            ws.DeleteRow(id, uidOf[k]);
            ++edits;
        } else if (s.kind == 2) {
            const EdRow& rb = rec.rows[static_cast<size_t>(s.ib)];
            anchor = anchor ? ws.InsertAfter(id, anchor, rb.cells) : ws.InsertAtEnd(id, rb.kind, rb.cells);
            ++edits;
        }
    }
    ws.End();
    if (d->Save() != bytes) err = L"Kurtarılan tablo birebir aynı değil (biçim farkı) / the recovered table is not byte-identical (format difference)";
    return edits;
}

// ------------------------------------------------------------------------------------------------ helpers

std::vector<std::wstring> ListTextdata(const ClientBase& c) {
    std::vector<std::wstring> all;
    std::set<std::wstring> seen;
    auto add = [&](const std::wstring& n) {
        if (!EndsWithI(n, L".txt")) return;
        if (seen.insert(Lower(n)).second) all.push_back(n);
    };
    auto scan = [&](const std::wstring& dir) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(JoinPath(dir, L"*.txt").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) add(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    };
    if (!c.overlay.empty()) scan(JoinPath(JoinPath(c.overlay, L"Media"), kTdInner));
    if (c.kind == ClientBase::Kind::Pk2) {
        std::vector<std::pair<std::wstring, bool>> v;
        if (c.List(Arc::Media, kTdInner, v))
            for (const auto& [n, dir] : v)
                if (!dir) add(n);
    } else if (!c.textdataDir.empty()) {
        scan(c.textdataDir);
    }
    std::sort(all.begin(), all.end(), [](const std::wstring& a, const std::wstring& b) { return Lower(a) < Lower(b); });
    return all;
}

std::vector<std::wstring> ReadIndexList(const Bytes& raw) {
    std::vector<std::wstring> v;
    const std::wstring t = DecodeText(raw);
    for (auto line : SplitView(t, L'\n')) {
        std::wstring s = Trim(line);
        while (!s.empty() && (s.back() == L'\r' || s.back() == L'\t')) s.pop_back();
        if (s.empty() || StartsWith(s, L"//")) continue;
        v.push_back(s);
    }
    return v;
}

// ------------------------------------------------------------------------------------------------ Workspace

std::vector<std::wstring> Workspace::ListTables() const {
    if (!client) return {};
    return ListTextdata(*client);
}

int Workspace::Find(const std::wstring& n) const {
    for (size_t i = 0; i < docs.size(); ++i)
        if (_wcsicmp(docs[i]->name.c_str(), n.c_str()) == 0) return static_cast<int>(i);
    return -1;
}

int Workspace::Add(std::unique_ptr<EdDoc> d) {
    docs.push_back(std::move(d));
    return static_cast<int>(docs.size()) - 1;
}

int Workspace::Open(const std::wstring& n, std::wstring& err) {
    const int f = Find(n);
    if (f >= 0) return f;
    if (!client) {
        err = L"istemci açık değil / no client";
        return -1;
    }
    Bytes raw;
    if (!client->ReadTextdata(n, raw)) {
        err = L"tablo okunamadı / cannot read: " + n;
        return -1;
    }
    auto d = std::make_unique<EdDoc>();
    if (!d->Load(n, raw, err)) return -1;
    d->origin = client->FromOverlay(Arc::Media, std::wstring(kTdInner) + L"\\" + n) ? L"overlay" : client->Describe(false);
    return Add(std::move(d));
}

std::vector<int> Workspace::OpenIndexed(const std::wstring& indexName, std::wstring& err) {
    std::vector<int> ids;
    if (!client) return ids;
    Bytes raw;
    if (!client->ReadTextdata(indexName, raw)) {
        err = L"dizin dosyası yok / no index file: " + indexName;
        return ids;
    }
    for (const auto& f : ReadIndexList(raw)) {
        std::wstring e;
        const int id = Open(f, e);
        if (id >= 0) ids.push_back(id);
        else err += (err.empty() ? L"" : L"\n") + e;
    }
    return ids;
}

void Workspace::Begin(const std::wstring& label) {
    if (m_depth++ == 0) {
        m_open = Change{};
        m_open.label = label;
    }
}

void Workspace::End() {
    if (m_depth == 0) return;
    if (--m_depth == 0 && !m_open.ops.empty()) {
        undo.push_back(std::move(m_open));
        if (undo.size() > maxUndo) undo.erase(undo.begin());
        redo.clear();
        m_open = Change{};
        if (onChange) onChange();
    }
}

void Workspace::ClearHistory() {
    undo.clear();
    redo.clear();
}

void Workspace::Record(Op op) {
    if (m_depth > 0) {
        m_open.ops.push_back(std::move(op));
        return;
    }
    Change c;
    switch (op.t) {
        case Op::T::Set: c.label = L"Hücre / cell"; break;
        case Op::T::Insert: c.label = L"Satır ekle / add row"; break;
        case Op::T::Delete: c.label = L"Satır sil / delete row"; break;
        case Op::T::Undelete: c.label = L"Satırı geri al / restore row"; break;
    }
    c.ops.push_back(std::move(op));
    undo.push_back(std::move(c));
    if (undo.size() > maxUndo) undo.erase(undo.begin());
    redo.clear();
    if (onChange) onChange();
}

void Workspace::Apply(const Op& op, bool fwd) {
    EdDoc* d = Doc(op.doc);
    if (!d) return;
    switch (op.t) {
        case Op::T::Set: {
            EdRow* r = d->Row(op.uid);
            if (!r) return;
            if (fwd) {
                if (r->cells.size() <= static_cast<size_t>(op.col)) r->cells.resize(static_cast<size_t>(op.col) + 1);
                r->cells[static_cast<size_t>(op.col)] = op.after;
            } else {
                if (static_cast<size_t>(op.col) < r->cells.size()) r->cells[static_cast<size_t>(op.col)] = op.before;
                if (op.size0 >= 0 && r->cells.size() > static_cast<size_t>(op.size0)) r->cells.resize(static_cast<size_t>(op.size0));
            }
            break;
        }
        case Op::T::Insert:
            if (fwd) d->rows.insert(d->rows.begin() + std::clamp(op.pos, 0, static_cast<int>(d->rows.size())), op.row);
            else {
                const int i = d->IndexOf(op.uid);
                if (i >= 0) d->rows.erase(d->rows.begin() + i);
            }
            break;
        case Op::T::Delete:
        case Op::T::Undelete: {
            EdRow* r = d->Row(op.uid);
            if (r) r->deleted = (op.t == Op::T::Delete) == fwd;
            break;
        }
    }
    ++d->revision;
}

bool Workspace::SetCell(int doc, std::uint64_t uid, int col, const std::wstring& v) {
    EdDoc* d = Doc(doc);
    if (!d || col < 0) return false;
    EdRow* r = d->Row(uid);
    if (!r) return false;
    Op op;
    op.t = Op::T::Set;
    op.doc = doc;
    op.uid = uid;
    op.col = col;
    op.size0 = static_cast<int>(r->cells.size());
    op.before = static_cast<size_t>(col) < r->cells.size() ? r->cells[static_cast<size_t>(col)] : L"";
    op.after = v;
    if (op.before == v && static_cast<size_t>(col) < r->cells.size()) return true;
    Apply(op, true);
    Record(std::move(op));
    return true;
}

std::uint64_t Workspace::InsertRow(int doc, int pos, EdRow row) {
    EdDoc* d = Doc(doc);
    if (!d) return 0;
    row.uid = d->nextUid++;
    row.line = -1;
    row.deleted = false;
    Op op;
    op.t = Op::T::Insert;
    op.doc = doc;
    op.uid = row.uid;
    op.pos = std::clamp(pos, 0, static_cast<int>(d->rows.size()));
    op.row = std::move(row);
    Apply(op, true);
    const std::uint64_t uid = op.uid;
    Record(std::move(op));
    return uid;
}

std::uint64_t Workspace::InsertAfter(int doc, std::uint64_t anchorUid, const std::vector<std::wstring>& cells) {
    EdDoc* d = Doc(doc);
    if (!d) return 0;
    const int ai = d->IndexOf(anchorUid);
    if (ai < 0) return 0;
    EdRow r;
    r.kind = d->rows[static_cast<size_t>(ai)].kind;
    r.cells = cells;
    r.before = d->InsertPointAfter(d->rows[static_cast<size_t>(ai)]);
    // after the anchor and after new rows already placed right behind it
    int pos = ai + 1;
    while (pos < static_cast<int>(d->rows.size()) && d->rows[static_cast<size_t>(pos)].line < 0 &&
           d->rows[static_cast<size_t>(pos)].before == r.before)
        ++pos;
    return InsertRow(doc, pos, std::move(r));
}

std::uint64_t Workspace::InsertAtEnd(int doc, TKind k, const std::vector<std::wstring>& cells) {
    EdDoc* d = Doc(doc);
    if (!d) return 0;
    if (k == TKind::Unknown || (d->kind != TKind::SeFile && k != d->kind && !(d->kind == TKind::Generic))) k = d->rows.empty() ? d->kind : d->rows.back().kind;
    if (d->kind != TKind::SeFile && d->kind != TKind::Generic) k = d->kind;
    EdRow r;
    r.kind = k;
    r.cells = cells;
    r.before = d->InsertPointEnd(k);
    int pos = static_cast<int>(d->rows.size());
    if (d->kind == TKind::SeFile) {
        // right after the last row of that section (new rows of the section included)
        int last = -1;
        for (size_t i = 0; i < d->rows.size(); ++i)
            if (d->rows[i].kind == k) last = static_cast<int>(i);
        if (last >= 0) pos = last + 1;
    }
    return InsertRow(doc, pos, std::move(r));
}

bool Workspace::DeleteRow(int doc, std::uint64_t uid) {
    EdDoc* d = Doc(doc);
    EdRow* r = d ? d->Row(uid) : nullptr;
    if (!r || r->deleted) return false;
    Op op;
    op.t = Op::T::Delete;
    op.doc = doc;
    op.uid = uid;
    Apply(op, true);
    Record(std::move(op));
    return true;
}

bool Workspace::RestoreRow(int doc, std::uint64_t uid) {
    EdDoc* d = Doc(doc);
    EdRow* r = d ? d->Row(uid) : nullptr;
    if (!r || !r->deleted) return false;
    Op op;
    op.t = Op::T::Undelete;
    op.doc = doc;
    op.uid = uid;
    Apply(op, true);
    Record(std::move(op));
    return true;
}

bool Workspace::Undo() {
    if (undo.empty() || m_depth) return false;
    Change c = std::move(undo.back());
    undo.pop_back();
    for (auto it = c.ops.rbegin(); it != c.ops.rend(); ++it) Apply(*it, false);
    redo.push_back(std::move(c));
    if (onChange) onChange();
    return true;
}

bool Workspace::Redo() {
    if (redo.empty() || m_depth) return false;
    Change c = std::move(redo.back());
    redo.pop_back();
    for (const auto& op : c.ops) Apply(op, true);
    undo.push_back(std::move(c));
    if (onChange) onChange();
    return true;
}

}  // namespace se::ed
