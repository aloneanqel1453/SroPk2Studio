// SkillEffect Studio - editor/EdTemplates.cpp
#include "EdTemplates.h"

#include <algorithm>
#include <cwctype>
#include <regex>

namespace se::ed {

namespace {

std::wstring Up(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towupper(c));
    return s;
}

long long BucketOf(const std::wstring& name) {
    std::wstring digits;
    for (wchar_t ch : name)
        if (ch >= L'0' && ch <= L'9') digits += ch;
        else if (!digits.empty()) break;
    long long v = 0;
    return ParseInt(digits, v) ? v : 0;
}

TemplateResult Fail(const std::wstring& tr, const std::wstring& en) {
    TemplateResult r;
    r.msgTr = tr;
    r.msgEn = en;
    return r;
}

}  // namespace

long long NextFreeId(const Workspace& ws, TKind k) {
    long long mx = 0;
    for (const auto& d : ws.docs) {
        const bool same = k == TKind::SkillData ? d->kind == TKind::SkillData
                                                : (d->kind == TKind::CharacterData || d->kind == TKind::ItemData);
        if (!same) continue;
        for (const auto& r : d->rows) {
            long long id = 0;
            if (!r.deleted && r.cells.size() > 1 && ParseInt(r.cells[1], id)) mx = std::max(mx, id);
        }
    }
    return mx + 1;
}

std::wstring UniqueKey(const EdDoc& d, TKind k, const std::wstring& base) {
    std::wstring b = base.empty() ? L"NEW_KEY" : base;
    std::wstring c = b + L"_COPY";
    for (int n = 2; !d.FindKey(c, k).empty(); ++n) c = b + L"_COPY" + std::to_wstring(n);
    return c;
}

TemplateResult NewSkillEffect(Workspace& ws, const std::wstring& srcKey, const std::wstring& newKey, const std::wstring& label,
                              Place place, bool withSounds) {
    std::wstring err;
    const int se = ws.Open(L"skilleffect.txt", err);
    if (se < 0) return Fail(L"skilleffect.txt açılamadı: " + err, L"cannot open skilleffect.txt: " + err);
    EdDoc* d = ws.Doc(se);
    if (newKey.empty() || newKey.find_first_of(L" \t/") != std::wstring::npos)
        return Fail(L"Yeni anahtar boş olamaz, boşluk ve '/' içeremez.", L"The new key must not be empty or hold spaces / '/'.");
    const auto sa = d->FindKey(srcKey, TKind::SeAniSet);
    const auto sr = d->FindKey(srcKey, TKind::SeEffectSet);
    if (sa.empty() && sr.empty()) return Fail(L"Kaynak anahtar bulunamadı: " + srcKey, L"Source key not found: " + srcKey);
    if (!d->FindKey(newKey, TKind::SeAniSet).empty() || !d->FindKey(newKey, TKind::SeEffectSet).empty())
        return Fail(L"Bu anahtar zaten var (V6): " + newKey, L"Key already exists (V6): " + newKey);
    // capture before inserting (indexes move)
    std::vector<std::wstring> saCells;
    std::uint64_t saAnchor = 0;
    if (!sa.empty()) {
        saCells = d->rows[static_cast<size_t>(sa.back())].cells;
        saAnchor = d->rows[static_cast<size_t>(sa.back())].uid;
    } else {
        for (const auto& c : SchemaFor(TKind::SeAniSet).cols) saCells.push_back(c.def);
    }
    std::vector<std::vector<std::wstring>> seCells;
    for (int i : sr) seCells.push_back(d->rows[static_cast<size_t>(i)].cells);
    const std::uint64_t seAnchor = sr.empty() ? 0 : d->rows[static_cast<size_t>(sr.back())].uid;

    TemplateResult res;
    ws.Begin(L"Yeni beceri efekti / new skill effect: " + newKey);
    if (saCells.size() > 2) saCells[2] = newKey;
    if (!label.empty() && saCells.size() > 1) saCells[1] = label;
    std::uint64_t u = place == Place::AfterSource && saAnchor ? ws.InsertAfter(se, saAnchor, saCells) : ws.InsertAtEnd(se, TKind::SeAniSet, saCells);
    if (u) res.rows.push_back({se, u});
    std::uint64_t anchor = seAnchor;
    for (size_t i = 0; i < seCells.size(); ++i) {
        auto c = seCells[i];
        if (c.size() > 1) c[1] = newKey;
        if (i == 0 && !label.empty() && !c.empty()) c[0] = label;
        u = place == Place::AfterSource && anchor ? ws.InsertAfter(se, anchor, c) : ws.InsertAtEnd(se, TKind::SeEffectSet, c);
        if (u) {
            res.rows.push_back({se, u});
            anchor = u;
        }
    }
    int sounds = 0;
    if (withSounds) {
        std::wstring e2;
        const int es = ws.Open(L"effectsound.txt", e2);
        if (es >= 0) {
            EdDoc* s = ws.Doc(es);
            std::vector<std::pair<std::uint64_t, std::vector<std::wstring>>> src;
            for (const auto& r : s->rows)
                if (!r.deleted && r.cells.size() > 3 && r.cells[3] == srcKey) src.push_back({r.uid, r.cells});
            std::uint64_t a = src.empty() ? 0 : src.back().first;
            for (auto& [uid, c] : src) {
                (void)uid;
                c[3] = newKey;
                const std::uint64_t nu = ws.InsertAfter(es, a, c);
                if (nu) {
                    res.rows.push_back({es, nu});
                    a = nu;
                    ++sounds;
                }
            }
        }
    }
    ws.End();
    res.ok = !res.rows.empty();
    res.msgTr = Fmt(L"%ls → %ls: skillaniset2 1, skilleffectset %zu, effectsound %d satır eklendi.", srcKey.c_str(), newKey.c_str(),
                    seCells.size(), sounds);
    res.msgEn = Fmt(L"%ls -> %ls: skillaniset2 1, skilleffectset %zu, effectsound %d rows added.", srcKey.c_str(), newKey.c_str(),
                    seCells.size(), sounds);
    return res;
}

TemplateResult NewMonster(Workspace& ws, const std::wstring& srcCode, const std::wstring& newCode, const NewMonsterOpts& o) {
    std::wstring err;
    if (newCode.empty() || newCode.find_first_of(L" \t/") != std::wstring::npos)
        return Fail(L"Yeni kod boş olamaz, boşluk ve '/' içeremez.", L"The new code must not be empty or hold spaces / '/'.");
    auto chars = ws.OpenIndexed(L"characterdata.txt", err);
    ws.OpenIndexed(L"itemdata.txt", err);  // shared ID space
    int srcDoc = -1;
    std::uint64_t srcUid = 0;
    for (int id : chars) {
        const EdDoc* d = ws.Doc(id);
        const auto h = d->FindKey(srcCode);
        if (!h.empty()) {
            srcDoc = id;
            srcUid = d->rows[static_cast<size_t>(h.back())].uid;
        }
        if (!d->FindKey(newCode).empty()) return Fail(L"Bu kod zaten var: " + newCode, L"Code already exists: " + newCode);
    }
    if (srcDoc < 0) return Fail(L"Kaynak canavar characterdata'da yok: " + srcCode, L"Source creature not in characterdata: " + srcCode);
    const long long id = o.id > 0 ? o.id : NextFreeId(ws, TKind::CharacterData);
    // the split file whose 5000-bucket holds the ID (else the source's file)
    int target = srcDoc;
    for (int d : chars) {
        const long long b = BucketOf(ws.Doc(d)->name);
        if (b >= 5000 && id >= b - 5000 && id < b) target = d;
    }
    auto cells = ws.Doc(srcDoc)->Row(srcUid)->cells;
    const std::wstring oldName = cells.size() > 5 ? cells[5] : L"";
    if (cells.size() > 2) {
        cells[1] = std::to_wstring(id);
        cells[2] = newCode;
    }
    const bool ownName = !oldName.empty() && oldName != L"xxx";
    if (ownName && cells.size() > 5) cells[5] = L"SN_" + newCode;

    TemplateResult res;
    ws.Begin(L"Yeni canavar / new monster: " + newCode);
    std::uint64_t u = ws.InsertAtEnd(target, TKind::CharacterData, cells);
    if (u) res.rows.push_back({target, u});
    std::wstring notes;
    if (o.withName && ownName) {
        int nt = ws.Open(L"textdata_object.txt", err);
        if (nt >= 0) {
            EdDoc* n = ws.Doc(nt);
            const auto h = n->FindKey(oldName);
            std::vector<std::wstring> nc;
            if (!h.empty()) nc = n->rows[static_cast<size_t>(h.back())].cells;
            else {
                for (const auto& c : SchemaFor(TKind::NameTable).cols) nc.push_back(c.def);
                notes += L" (isim satırı şablondan / name row from defaults)";
            }
            if (nc.size() > 1) nc[1] = L"SN_" + newCode;
            if (!o.nameKor.empty() && nc.size() > 2) nc[2] = o.nameKor;
            if (!o.nameEng.empty()) {
                // the ENG column: 8 in the 17-column layout, else the last non-empty name cell
                size_t eng = nc.size() >= 16 ? 8 : (nc.size() >= 3 ? nc.size() - 2 : 2);
                if (eng < nc.size()) nc[eng] = o.nameEng;
                if (nc.size() >= 16) nc[9] = o.nameEng;
            }
            u = ws.InsertAtEnd(nt, TKind::NameTable, nc);
            if (u) res.rows.push_back({nt, u});
        }
    }
    if (o.withCharInfo) {
        const int se = ws.Open(L"skilleffect.txt", err);
        if (se >= 0) {
            EdDoc* s = ws.Doc(se);
            const auto h = s->FindKey(srcCode, TKind::SeCharInfo);
            if (!h.empty()) {
                auto c = s->rows[static_cast<size_t>(h.back())].cells;
                c[0] = newCode;
                u = ws.InsertAfter(se, s->rows[static_cast<size_t>(h.back())].uid, c);
                if (u) res.rows.push_back({se, u});
            } else {
                notes += L" (kaynağın characterInfo satırı yok / source has no characterInfo row)";
            }
        }
    }
    ws.End();
    res.ok = !res.rows.empty();
    res.msgTr = Fmt(L"%ls → %ls (ID %lld): %zu satır eklendi.", srcCode.c_str(), newCode.c_str(), id, res.rows.size()) + notes;
    res.msgEn = Fmt(L"%ls -> %ls (ID %lld): %zu rows added.", srcCode.c_str(), newCode.c_str(), id, res.rows.size()) + notes;
    return res;
}

TemplateResult CopyRow(Workspace& ws, int doc, std::uint64_t uid) {
    EdDoc* d = ws.Doc(doc);
    const EdRow* r = d ? d->Row(uid) : nullptr;
    if (!r) return Fail(L"Satır yok.", L"No row.");
    const Schema& s = d->SchemaOf(*r);
    auto cells = r->cells;
    const TKind k = r->kind;
    std::wstring oldKey, newKey;
    if (s.keyCol >= 0 && s.keyUnique && static_cast<size_t>(s.keyCol) < cells.size()) {
        oldKey = cells[static_cast<size_t>(s.keyCol)];
        newKey = UniqueKey(*d, k, Trim(oldKey));
        cells[static_cast<size_t>(s.keyCol)] = newKey;
    }
    if (s.idCol >= 0 && static_cast<size_t>(s.idCol) < cells.size()) cells[static_cast<size_t>(s.idCol)] = std::to_wstring(NextFreeId(ws, k));
    const int ns = s.Find(L"NameStrID128");
    if (ns >= 0 && static_cast<size_t>(ns) < cells.size() && !oldKey.empty() && cells[static_cast<size_t>(ns)] == L"SN_" + oldKey)
        cells[static_cast<size_t>(ns)] = L"SN_" + newKey;
    ws.Begin(L"Satır kopyala / copy row");
    const std::uint64_t u = ws.InsertAfter(doc, uid, cells);
    ws.End();
    TemplateResult res;
    res.ok = u != 0;
    if (u) res.rows.push_back({doc, u});
    res.msgTr = newKey.empty() ? L"Satır kopyalandı." : L"Satır kopyalandı: " + newKey;
    res.msgEn = newKey.empty() ? L"Row copied." : L"Row copied: " + newKey;
    return res;
}

TemplateResult BlankRow(Workspace& ws, int doc, TKind k) {
    EdDoc* d = ws.Doc(doc);
    if (!d) return Fail(L"Tablo yok.", L"No table.");
    if (k == TKind::Unknown) k = d->kind == TKind::SeFile ? TKind::SeAniSet : d->kind;
    const Schema& s = d->SchemaOf(k);
    std::vector<std::wstring> cells;
    for (const auto& c : s.cols) cells.push_back(c.def);
    if (s.keyCol >= 0 && s.keyUnique) cells[static_cast<size_t>(s.keyCol)] = UniqueKey(*d, k, L"NEW_KEY");
    else if (s.keyCol >= 0) cells[static_cast<size_t>(s.keyCol)] = L"NEW_KEY";
    if (s.idCol >= 0) cells[static_cast<size_t>(s.idCol)] = std::to_wstring(NextFreeId(ws, k));
    ws.Begin(L"Boş satır / blank row");
    const std::uint64_t u = ws.InsertAtEnd(doc, k, cells);
    ws.End();
    TemplateResult res;
    res.ok = u != 0;
    if (u) res.rows.push_back({doc, u});
    res.msgTr = L"Varsayılan değerli satır eklendi.";
    res.msgEn = L"Row of defaults added.";
    return res;
}

// ---------------------------------------------------------------- find / replace

namespace {

struct Matcher {
    const FindOpts& o;
    std::wregex re;
    std::wstring upText;
    bool ok = true;
    explicit Matcher(const FindOpts& f, std::wstring* err) : o(f) {
        if (o.regex) {
            try {
                re = std::wregex(o.text, o.matchCase ? std::regex_constants::ECMAScript
                                                     : std::regex_constants::ECMAScript | std::regex_constants::icase);
            } catch (const std::regex_error&) {
                ok = false;
                if (err) *err = L"Geçersiz düzenli ifade / invalid regex";
            }
        }
        upText = o.matchCase ? o.text : Up(o.text);
    }
    bool Match(const std::wstring& cell) const {
        if (!ok || o.text.empty()) return false;
        if (o.regex) return o.wholeCell ? std::regex_match(cell, re) : std::regex_search(cell, re);
        const std::wstring c = o.matchCase ? cell : Up(cell);
        return o.wholeCell ? c == upText : c.find(upText) != std::wstring::npos;
    }
    std::wstring Replace(const std::wstring& cell, const std::wstring& repl) const {
        if (o.regex) {
            if (o.wholeCell) return std::regex_match(cell, re) ? std::regex_replace(cell, re, repl) : cell;
            return std::regex_replace(cell, re, repl);
        }
        if (o.wholeCell) return repl;
        std::wstring out;
        const std::wstring c = o.matchCase ? cell : Up(cell);
        size_t a = 0;
        for (;;) {
            const size_t b = c.find(upText, a);
            if (b == std::wstring::npos) break;
            out += cell.substr(a, b - a) + repl;
            a = b + upText.size();
        }
        return out + cell.substr(a);
    }
};

}  // namespace

std::vector<Hit> FindAll(const Workspace& ws, const FindOpts& o, std::wstring* err) {
    std::vector<Hit> hits;
    Matcher m(o, err);
    if (!m.ok || o.text.empty()) return hits;
    std::vector<int> ids = o.docs;
    if (ids.empty())
        for (size_t i = 0; i < ws.docs.size(); ++i) ids.push_back(static_cast<int>(i));
    for (int id : ids) {
        const EdDoc* d = ws.Doc(id);
        if (!d) continue;
        for (const auto& r : d->rows) {
            if (r.deleted || (o.kind != TKind::Unknown && r.kind != o.kind)) continue;
            int only = -1;
            if (!o.column.empty()) {
                only = d->SchemaOf(r).Find(o.column);
                if (only < 0) continue;
            }
            for (size_t c = 0; c < r.cells.size(); ++c) {
                if (only >= 0 && static_cast<int>(c) != only) continue;
                if (!m.Match(r.cells[c])) continue;
                hits.push_back({id, r.uid, r.line, static_cast<int>(c), r.cells[c]});
                if (hits.size() >= o.maxHits) return hits;
            }
        }
    }
    return hits;
}

int ReplaceAll(Workspace& ws, const FindOpts& o, const std::wstring& repl, const std::vector<Hit>* only, std::wstring* err) {
    Matcher m(o, err);
    if (!m.ok) return 0;
    std::vector<Hit> all;
    if (!only) all = FindAll(ws, o, err);
    const std::vector<Hit>& hs = only ? *only : all;
    int n = 0;
    ws.Begin(L"Değiştir / replace: " + o.text + L" → " + repl);
    for (const auto& h : hs) {
        EdDoc* d = ws.Doc(h.doc);
        const EdRow* r = d ? d->Row(h.uid) : nullptr;
        if (!r || h.col < 0 || static_cast<size_t>(h.col) >= r->cells.size()) continue;
        const std::wstring cur = r->cells[static_cast<size_t>(h.col)];
        if (!m.Match(cur)) continue;
        const std::wstring nv = m.Replace(cur, repl);
        if (nv != cur && ws.SetCell(h.doc, h.uid, h.col, nv)) ++n;
    }
    ws.End();
    return n;
}

}  // namespace se::ed
