// SkillEffect Studio - editor/EdValidate.cpp
#include "EdValidate.h"

#include "../core/Enums.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace se::ed {

namespace {

bool IsNoneV(const std::wstring& v) { return v.empty() || v == L"none" || v == L"xxx" || v == L"-" || v == L"0"; }

bool IsNumV(std::wstring_view s) {
    double d = 0;
    long long i = 0;
    return ParseInt(s, i) || ParseFloat(s, d);
}

bool BoolWordV(std::wstring_view s) { return s == L"true" || s == L"false" || s == L"TRUE" || s == L"FALSE"; }

std::wstring ExtOf(const std::wstring& p) {
    const size_t d = p.find_last_of(L'.');
    const size_t s = p.find_last_of(L"\\/");
    if (d == std::wstring::npos || (s != std::wstring::npos && d < s)) return L"";
    std::wstring e = p.substr(d + 1);
    for (auto& c : e) c = static_cast<wchar_t>(std::towlower(c));
    return e;
}

bool ExtAllowed(const std::wstring& allowed, const std::wstring& e) {
    if (allowed.empty()) return true;
    for (auto x : SplitView(allowed, L'|'))
        if (x == e) return true;
    return false;
}

struct Add {
    std::vector<CellIssue>& out;
    int col;
    void operator()(Sev s, const wchar_t* code, const std::wstring& tr, const std::wstring& en) const {
        CellIssue i;
        i.col = col;
        i.sev = s;
        i.code = code;
        i.msgTr = tr;
        i.msgEn = en;
        out.push_back(std::move(i));
    }
};

template <size_t N>
void CheckEnumTokens(const std::wstring_view (&t)[N], const std::wstring& v, bool soft, const Add& add) {
    if (enums::In(t, v)) return;
    if (enums::InCI(t, v)) {
        add(Sev::Warn, L"ENUM", L"Büyük/küçük harf farklı: istemci -1 okur ('" + v + L"').",
            L"Case differs: the client reads -1 ('" + v + L"').");
        return;
    }
    add(soft ? Sev::Warn : Sev::Error, L"ENUM", L"Tanımsız değer '" + v + L"' (istemci tablosunda yok).",
        L"Unknown value '" + v + L"' (not in the client table).");
}

void CheckEnumC(const ColDef& c, const std::wstring& v, const Add& add) {
    for (size_t i = 0; i < c.enumN; ++i)
        if (c.enumv[i] == v) return;
    for (size_t i = 0; i < c.enumN; ++i)
        if (c.enumv[i].size() == v.size() && _wcsnicmp(c.enumv[i].data(), v.data(), v.size()) == 0) {
            add(Sev::Warn, L"ENUM", L"Büyük/küçük harf farklı: istemci -1 okur ('" + v + L"' yerine '" + std::wstring(c.enumv[i]) + L"').",
                L"Case differs: the client reads -1 ('" + std::wstring(c.enumv[i]) + L"').");
            return;
        }
    std::wstring list;
    for (size_t i = 0; i < c.enumN && i < 8; ++i) list += (i ? L" / " : L"") + std::wstring(c.enumv[i]);
    if (c.enumN > 8) list += L" ...";
    const bool soft = c.name == L"AniGroup";
    add(soft ? Sev::Warn : Sev::Error, L"ENUM", L"'" + v + L"' geçersiz. Olabilir: " + list, L"'" + v + L"' invalid. Allowed: " + list);
}

// reference / duplicate findings on rows exactly as shipped are dead or legacy rows: warnings, not errors (a new or
// edited row keeps the error)
void Soften(const EdDoc& d, std::vector<CellIssue>& v, size_t from) {
    for (size_t k = from; k < v.size(); ++k) {
        CellIssue& i = v[k];
        if (i.sev != Sev::Error || !i.uid) continue;
        if (i.code != L"V5" && i.code != L"V6" && i.code != L"V8" && i.code != L"V9") continue;
        const EdRow* r = d.Row(i.uid);
        if (r && r->line >= 0 && !d.RowEdited(*r)) i.sev = Sev::Warn;
    }
}

}  // namespace

const wchar_t* SevName(Sev s, bool en) {
    switch (s) {
        case Sev::Error: return en ? L"Error" : L"Hata";
        case Sev::Warn: return en ? L"Warning" : L"Uyarı";
        default: return en ? L"Info" : L"Bilgi";
    }
}

void RefIndex::Clear() { *this = RefIndex{}; }

void RefIndex::AddDoc(const EdDoc& d) {
    auto cell = [](const EdRow& r, size_t i) -> const std::wstring& {
        static const std::wstring empty;
        return i < r.cells.size() ? r.cells[i] : empty;
    };
    switch (d.kind) {
        case TKind::CharacterData:
        case TKind::ItemData:
            for (const auto& r : d.rows) {
                if (r.deleted) continue;
                long long id = 0;
                const std::wstring& code = cell(r, 2);
                if (code.empty()) continue;
                (d.kind == TKind::CharacterData ? charCodes : itemCodes).insert(code);
                if (ParseInt(cell(r, 1), id)) objById[id] = code;
            }
            (d.kind == TKind::CharacterData ? hasChars : hasItems) = true;
            break;
        case TKind::SkillData:
            for (const auto& r : d.rows) {
                if (r.deleted) continue;
                const std::wstring& code = cell(r, 3);
                if (code.empty()) continue;
                skillCodes.insert(code);
                const std::wstring& g = cell(r, 5);
                if (!g.empty() && g != L"xxx") skillGroups.insert(g);
                groupOfCode[code] = g;
                long long id = 0;
                if (ParseInt(cell(r, 1), id)) skillById[id] = code;
            }
            hasSkills = true;
            break;
        case TKind::NameTable:
            for (const auto& r : d.rows)
                if (!r.deleted && !cell(r, 1).empty()) nameStrIds.insert(cell(r, 1));
            hasNames = true;
            break;
        case TKind::EffectSound:
            for (const auto& r : d.rows)
                if (!r.deleted && !cell(r, 1).empty()) soundObjs.insert(cell(r, 1));
            hasSounds = true;
            break;
        case TKind::SeFile:
            for (const auto& r : d.rows) {
                if (r.deleted) continue;
                if (r.kind == TKind::SeCharInfo) ciKeys.insert(Trim(cell(r, 0)));
                else if (r.kind == TKind::SeAniSet) saKeys.insert(cell(r, 2));
            }
            hasSe = true;
            break;
        default: break;
    }
}

std::function<int(int, const std::wstring&)> ClientExists(const ClientBase& c) {
    auto cache = std::make_shared<std::unordered_map<std::wstring, int>>();
    auto mu = std::make_shared<std::mutex>();
    const ClientBase* pc = &c;
    return [pc, cache, mu](int arc, const std::wstring& inner) -> int {
        if (arc < 0 || arc > 2) return -1;
        std::wstring k = std::to_wstring(arc) + L":" + ToUpper(inner);
        std::lock_guard<std::mutex> lk(*mu);
        auto it = cache->find(k);
        if (it != cache->end()) return it->second;
        const int r = pc->Exists(static_cast<Arc>(arc), inner);
        (*cache)[k] = r;
        return r;
    };
}

void BuildRefIndex(Workspace& ws, RefIndex& ref, std::wstring* log) {
    std::wstring err;
    auto note = [&](const std::wstring& s) {
        if (log) *log += s + L"\n";
    };
    for (const wchar_t* idx : {L"characterdata.txt", L"itemdata.txt"}) {
        err.clear();
        const auto ids = ws.OpenIndexed(idx, err);
        note(Fmt(L"%ls: %zu dosya", idx, ids.size()) + (err.empty() ? L"" : L" (" + err + L")"));
    }
    {
        err.clear();
        auto ids = ws.OpenIndexed(L"skilldataenc.txt", err);
        if (ids.empty()) ids = ws.OpenIndexed(L"skilldata.txt", err);
        note(Fmt(L"skilldata: %zu dosya", ids.size()));
    }
    {
        err.clear();
        auto ids = ws.OpenIndexed(L"textdataname.txt", err);
        if (ids.empty())
            for (const wchar_t* n : {L"textdata_object.txt", L"textdata_equip&skill.txt"}) {
                const int id = ws.Open(n, err);
                if (id >= 0) ids.push_back(id);
            }
        note(Fmt(L"isim tabloları: %zu", ids.size()));
    }
    ws.Open(L"effectsound.txt", err);
    ws.Open(L"skilleffect.txt", err);
    for (const auto& d : ws.docs) ref.AddDoc(*d);
    if (ws.client && !ref.exists) ref.exists = ClientExists(*ws.client);
}

std::wstring ResolvePath(const ColDef& c, const std::vector<std::wstring>& row, const std::wstring& v, int* arcOut) {
    if (arcOut) *arcOut = static_cast<int>(c.arc);
    if (c.type != CT::Path || IsNoneV(v) || v == L"noneweapon") return L"";
    std::wstring s = v;
    std::wstring prefix = c.prefix;
    int arc = static_cast<int>(c.arc);
    if (c.dirCol >= 0) {
        const std::wstring dir = static_cast<size_t>(c.dirCol) < row.size() ? row[static_cast<size_t>(c.dirCol)] : L"";
        if (IsNoneV(dir)) {
            if (c.prefix.empty() && c.arc == Arc4::Particles) return L"";  // skilleffectset: ObjName without folder
        } else {
            s = dir + v;
        }
        if (StartsWith(ToUpper(dir), L"RES\\")) arc = static_cast<int>(Arc4::Data);
    }
    if (c.dirCol < 0 && c.arc == Arc4::Particles && StartsWith(ToUpper(s), L"RES\\")) {  // a flying / trap .bsr from Data
        arc = static_cast<int>(Arc4::Data);
        prefix.clear();
    }
    const size_t comma = s.find(L',');
    if (comma != std::wstring::npos) s = s.substr(0, comma);  // set items 'a.bsr,a_fullset.bsr'
    for (auto& ch : s)
        if (ch == L'/') ch = L'\\';
    if (ExtOf(s).empty() && !c.ext.empty()) s += L"." + std::wstring(SplitView(c.ext, L'|')[0]);
    if (arcOut) *arcOut = arc;
    return prefix + s;
}

void CheckCell(const ColDef& c, const std::wstring& v, const std::vector<std::wstring>& row, const RefIndex* ref,
               std::vector<CellIssue>& out) {
    // column index is filled in by the caller (CheckRow); here col = -2 placeholder
    Add add{out, -2};
    if (c.type == CT::Empty) {
        if (!v.empty())
            add(Sev::Info, L"EMPTY", L"Bu sütun normalde boş (satır sonu).", L"This column is normally empty (row end).");
        return;
    }
    if (v.empty()) {
        if (c.flags & CF_Required) add(Sev::Error, L"REQ", L"Boş olamaz.", L"Must not be empty.");
        return;
    }
    // client traps on every text cell (R3/R6)
    for (size_t i = 0; i + 1 < v.size(); ++i)
        if (v[i] == L'/' && (v[i + 1] == L'/' || v[i + 1] == L'*')) {
            add(Sev::Error, L"R3", L"Hücrede '" + v.substr(i, 2) + L"': istemci satırın kalanını yorum sayar.",
                L"Cell holds '" + v.substr(i, 2) + L"': the client reads the rest as a comment.");
            break;
        }
    // R6/R7: leading U+0001..U+0010 are skipped by the client tokenizer; inside a text cell they stay (name tables
    // hold line breaks), inside a code / number they break the value
    if (v.front() >= 1 && v.front() <= 0x10)
        add(Sev::Warn, L"R6", L"Hücre kontrol karakteriyle başlıyor: istemci onu atlar.", L"Cell starts with a control character: the client skips it.");
    else if (c.type != CT::Label && c.type != CT::Any)
        for (wchar_t ch : v)
            if (ch >= 1 && ch <= 0x10) {
                add(Sev::Warn, L"R6", L"Kod / sayı hücresinde kontrol karakteri.", L"Control character in a code / number cell.");
                break;
            }
    if (c.type != CT::Label && c.type != CT::Any && c.type != CT::Bone && (v.front() == L' ' || v.back() == L' '))
        add(Sev::Warn, L"V4", L"Başta/sonda boşluk: değerin parçası olur.", L"Leading/trailing space becomes part of the value.");
    long long iv = 0;
    double dv = 0;
    switch (c.type) {
        case CT::Int:
            // an Int column's enumv lists placeholder words the client accepts (effectsound Blank / Volume '-')
            if (std::find(c.enumv, c.enumv + c.enumN, std::wstring_view(v)) != c.enumv + c.enumN) break;
            if (!ParseInt(v, iv)) add(Sev::Error, L"TYPE", L"Tam sayı olmalı.", L"Must be an integer.");
            else if (c.lo <= c.hi && (iv < c.lo || iv > c.hi))
                add(Sev::Warn, L"RANGE", Fmt(L"Aralık dışı (%lld..%lld).", c.lo, c.hi), Fmt(L"Out of range (%lld..%lld).", c.lo, c.hi));
            break;
        case CT::Real:
            if (!ParseInt(v, iv) && !ParseFloat(v, dv)) add(Sev::Error, L"TYPE", L"Sayı olmalı.", L"Must be a number.");
            break;
        case CT::Str:
            if (c.maxLen > 0 && static_cast<int>(v.size()) > c.maxLen)
                add(Sev::Warn, L"LEN", Fmt(L"%d karakterden uzun (SQL varchar).", c.maxLen), Fmt(L"Longer than %d characters.", c.maxLen));
            if (!c.ext.empty() && ExtOf(v) != c.ext) add(Sev::Warn, L"EXT", L"." + c.ext + L" bekleniyor.", L"." + c.ext + L" expected.");
            break;
        case CT::Code:
            for (wchar_t ch : v)
                if (ch == L' ' || ch == L'\t') {
                    add(Sev::Info, L"CODE", L"Kod adında boşluk (istemci boşluğu değere katar).", L"Space in a codename (kept in the value).");
                    break;
                }
            if (c.maxLen > 0 && static_cast<int>(v.size()) > c.maxLen)
                add(Sev::Warn, L"LEN", Fmt(L"%d karakterden uzun.", c.maxLen), Fmt(L"Longer than %d characters.", c.maxLen));
            break;
        case CT::BoolTF:
            if (v != L"TRUE" && v != L"FALSE") add(Sev::Error, L"ENUM", L"TRUE ya da FALSE.", L"TRUE or FALSE.");
            break;
        case CT::Enum:
            if (v == L"FLASE" || v == L"flase") add(Sev::Error, L"ENUM", L"Yazım hatası (FALSE).", L"Typo (FALSE).");
            else CheckEnumC(c, v, add);
            break;
        case CT::EnumPipe:
            for (auto f : SplitView(v, L'|')) {
                ColDef one = c;
                std::vector<CellIssue> tmp;
                Add a2{tmp, -2};
                CheckEnumC(one, std::wstring(f), a2);
                if (!tmp.empty()) {
                    out.insert(out.end(), tmp.begin(), tmp.end());
                    break;
                }
            }
            break;
        case CT::Comma: {
            auto p = SplitView(v, L',');
            bool ok = static_cast<int>(p.size()) == c.commaN;
            for (auto& x : p) ok = ok && IsNumV(x);
            if (!ok)
                add(Sev::Error, L"V12", Fmt(L"%d virgüllü sayı olmalı (istemci assert).", c.commaN),
                    Fmt(L"Must be %d comma-separated numbers (client assert).", c.commaN));
            break;
        }
        case CT::MovSpeed: {
            auto p = SplitView(v, L',');
            bool ok = p.size() == 4 && enums::In(enums::kMov, p[0]);
            for (size_t i = 1; ok && i < 4; ++i) ok = IsNumV(p[i]);
            if (!ok) add(Sev::Error, L"V12", L"'MOV_x,a,b,c' olmalı.", L"Must be 'MOV_x,a,b,c'.");
            break;
        }
        case CT::ActOption: {
            auto p = SplitView(v, L',');
            bool ok = p.size() == 6 && BoolWordV(p[0]) && BoolWordV(p[5]);
            for (size_t i = 1; ok && i < 5; ++i) ok = IsNumV(p[i]);
            if (!ok) add(Sev::Error, L"V12", L"'bool,a,b,c,d,bool' olmalı.", L"Must be 'bool,a,b,c,d,bool'.");
            break;
        }
        case CT::Script: {
            auto p = SplitView(v, L',');
            bool ok = true;
            for (auto& f : SplitView(p[0], L'|')) ok = ok && enums::In(enums::kScript, f);
            for (size_t i = 1; ok && i < p.size(); ++i) ok = IsNumV(p[i]);
            if (!ok) add(Sev::Error, L"ENUM", L"none ya da SCT_* (+ ',sayılar').", L"none or SCT_* (+ ',numbers').");
            break;
        }
        case CT::AniName:
            if (!enums::IsAniName(v)) add(Sev::Error, L"ENUM", L"ANI_* adı ya da none.", L"ANI_* name or none.");
            break;
        case CT::AniList:
            for (auto a : SplitView(v, L','))
                if (!enums::IsAniName(a)) {
                    add(Sev::Error, L"ENUM", L"ANI_* adı / virgüllü liste ya da none.", L"ANI_* name / comma list or none.");
                    break;
                }
            break;
        case CT::Folder:
            if (!IsNoneV(v) && v.back() != L'\\')
                add(Sev::Error, L"V15", L"Klasör '\\' ile bitmeli (ya da none).", L"Folder must end in '\\' (or none).");
            if (v.find(L'/') != std::wstring::npos) add(Sev::Warn, L"V3", L"'/' yerine '\\' kullanın.", L"Use '\\' instead of '/'.");
            break;
        case CT::Env: {
            auto p = SplitView(v, L',');
            long long e = 0;
            if (!ParseInt(p[0], e) || p.size() > 2) add(Sev::Error, L"V12", L"sayı ya da 'bayrak,dosya.ogg'.", L"number or 'flags,file.ogg'.");
            else if (p.size() == 2 && !(e & 2)) add(Sev::Warn, L"V12", L"'.ogg' yalnız bit 2 ile okunur.", L"'.ogg' is read only with bit 2.");
            else if (p.size() == 1 && (e & 2)) add(Sev::Error, L"V12", L"bit 2 var ama ',dosya.ogg' yok.", L"bit 2 set but ',file.ogg' missing.");
            break;
        }
        case CT::SeKey: {
            const std::wstring k = Trim(v);
            if (k.find(L'~') != std::wstring::npos) {
                const size_t n = k.size();
                bool ok = n >= 7 && k[n - 4] == L'~';
                long long a = 0, b = 0;
                if (ok) ok = ParseInt(std::wstring_view(k).substr(n - 7, 3), a) && ParseInt(std::wstring_view(k).substr(n - 3), b) && a <= b;
                if (!ok) add(Sev::Error, L"V5", L"Aralık 'ÖNEK###~###' olmalı.", L"Range must be 'PREFIX###~###'.");
            } else if (ref && ref->hasChars && !ref->charCodes.count(k)) {
                add(Sev::Error, L"V5", L"characterdata'da böyle bir kod yok: istemci satırı atar.",
                    L"No such code in characterdata: the client drops the row.");
            }
            break;
        }
        case CT::Path: {
            if (IsNoneV(v) || v == L"noneweapon") break;
            if (v.find(L'/') != std::wstring::npos) add(Sev::Warn, L"V3", L"'/' yerine '\\' kullanın.", L"Use '\\' instead of '/'.");
            int arc = -1;
            const std::wstring p = ResolvePath(c, row, v, &arc);
            if (p.empty()) break;
            if (!ExtAllowed(c.ext, ExtOf(p)))
                add(Sev::Warn, L"EXT", L"Uzantı beklenen değil (" + c.ext + L").", L"Unexpected extension (" + c.ext + L").");
            if (p.size() >= 500) add(Sev::Error, L"V15", L"Yol ≥ 512 karakter olabilir (wcscat_s).", L"Path may reach 512 characters.");
            if (ref && ref->checkFiles && ref->exists) {
                int r = ref->exists(arc, p);
                if (r == 0 && EndsWithI(p, L".bsr")) {  // dual weapons: <name>_l.bsr + <name>_r.bsr
                    const std::wstring stem = p.substr(0, p.size() - 4);
                    if (ref->exists(arc, stem + L"_l.bsr") == 1 || ref->exists(arc, stem + L"_r.bsr") == 1) r = 1;
                }
                if (r == 0) {
                    static const wchar_t* an[] = {L"Media.pk2", L"Data.pk2", L"Particles.pk2", L"Music.pk2"};
                    const std::wstring where = arc >= 0 && arc < 4 ? an[arc] : L"?";
                    add(Sev::Warn, L"FILE", L"Dosya istemcide yok: " + where + L"\\" + p, L"File not in the client: " + where + L"\\" + p);
                }
            }
            break;
        }
        default: break;
    }
    if (!ref) return;
    if ((c.flags & CF_NameStr) && ref->hasNames && !IsNoneV(v) && !ref->nameStrIds.count(v))
        add(Sev::Warn, L"REF",
            L"İsim tablosunda bu StrID yok (oyunda adı boş görünür). Düzeltmek için: Tablolar > textdata_object.txt'de bir ad satırını "
            L"çoğaltın (Ctrl+D), kodunu " + v + L" yapıp adını yazın; sonra Ad ve çeviri sayfasında diğer dilleri doldurun.",
            L"No such StrID in the name tables (the name shows empty in the game). To fix: in Tables > textdata_object.txt duplicate a "
            L"name row (Ctrl+D), set its code to " + v + L" and type the name; then fill the other languages on the Names page.");
    if ((c.flags & CF_SkillIdRef) && ref->hasSkills && ParseInt(v, iv) && iv != 0 && !ref->skillById.count(iv))
        add(Sev::Warn, L"REF", L"Bu ID'de skill yok (skilldata ENC).", L"No skill with this ID (skilldata ENC).");
    if ((c.flags & CF_SoundObj) && !(c.flags & CF_Key) && ref->hasSounds && !IsNoneV(v) && !ref->soundObjs.count(v))
        add(Sev::Info, L"REF", L"effectsound.txt'de bu nesne yok: canavar sessiz.", L"Object not in effectsound.txt: no sounds.");
}

void CheckRow(const EdDoc& d, int docId, const EdRow& r, const RefIndex* ref, std::vector<CellIssue>& out) {
    const Schema& s = d.SchemaOf(r);
    const size_t first = out.size();
    auto rowIssue = [&](Sev sev, const wchar_t* code, const std::wstring& tr, const std::wstring& en, int col = -1) {
        CellIssue i;
        i.col = col;
        i.sev = sev;
        i.code = code;
        i.msgTr = tr;
        i.msgEn = en;
        out.push_back(std::move(i));
    };
    const size_t n = std::max(r.cells.size(), s.cols.size());
    for (size_t c = 0; c < n && c < r.cells.size(); ++c) {
        const ColDef* cd = s.Col(c);
        const size_t before = out.size();
        if (cd) CheckCell(*cd, r.cells[c], r.cells, ref, out);
        for (size_t k = before; k < out.size(); ++k) out[k].col = static_cast<int>(c);
    }
    // empty cells shift columns in the client tokenizer (R7)
    if (s.clientCells > 0) {
        const auto toks = ClientTokens(JoinTabs(r.cells));
        int lastNonEmpty = -1;
        for (size_t c = 0; c < r.cells.size(); ++c)
            if (!r.cells[c].empty()) lastNonEmpty = static_cast<int>(c);
        for (int c = 0; c < lastNonEmpty; ++c)
            if (Trim(r.cells[static_cast<size_t>(c)]).empty()) {
                rowIssue(Sev::Error, L"V4", L"Boş hücre: istemci atlar, sonraki tüm sütunlar sola kayar (L2687 hatası).",
                         L"Empty cell: the client skips it, every later column shifts left (L2687 bug).", c);
                break;
            }
        if (static_cast<int>(toks.size()) < s.clientCells || (static_cast<int>(toks.size()) > s.clientCells && r.kind != TKind::SeCharInfo))
            rowIssue(toks.size() < static_cast<size_t>(s.clientCells) ? Sev::Error : Sev::Warn, L"V4",
                     Fmt(L"İstemci %zu hücre okur, %d olmalı.", toks.size(), s.clientCells),
                     Fmt(L"The client reads %zu cells, expected %d.", toks.size(), s.clientCells));
    } else if (d.kind != TKind::Generic && !s.cols.empty()) {
        size_t need = 0;
        for (size_t c = 0; c < s.cols.size(); ++c)
            if (s.cols[c].type != CT::Empty) need = c + 1;
        const int dom = d.base.dominantCells;
        // a client whose files use another layout (dominant count differs from the schema): compare with the file
        const bool layoutMatches = dom == 0 || static_cast<size_t>(dom) >= need;
        // P8: a loaded row that was already short in the client and did not get shorter (e.g. a name typed into an
        // existing short name row: the language cell is appended) is not a new problem - the client reads that row today.
        // Only a row made shorter or a new short row is an error.
        const bool sameAsLoaded = r.line >= 0 && static_cast<size_t>(r.line) < d.base.lines.size() && d.base.IsData(static_cast<size_t>(r.line)) &&
                                  d.base.Cells(static_cast<size_t>(r.line)).size() < need && r.cells.size() >= d.base.Cells(static_cast<size_t>(r.line)).size();
        if (layoutMatches && r.cells.size() < need && !sameAsLoaded)
            rowIssue(Sev::Error, L"COLS", Fmt(L"%zu sütun var, en az %zu gerekir.", r.cells.size(), need),
                     Fmt(L"%zu columns, at least %zu needed.", r.cells.size(), need));
        else if (!layoutMatches && dom > 0 && static_cast<int>(r.cells.size()) != dom)
            rowIssue(Sev::Warn, L"COLS", Fmt(L"%zu sütun; dosyanın çoğu satırı %d.", r.cells.size(), dom),
                     Fmt(L"%zu columns; most rows of the file have %d.", r.cells.size(), dom));
    }
    auto cell = [&](size_t i) -> std::wstring { return i < r.cells.size() ? r.cells[i] : L""; };
    if (r.kind == TKind::SeAniSet) {
        if (cell(0) == L"0") rowIssue(Sev::Warn, L"V7", L"Service 0: anahtar kapalı, skilleffectset satırları düşer.", L"Service 0: key disabled.", 0);
        const std::wstring k = cell(2);
        const bool fixed = enums::In(enums::kSystemKeys, k) || StartsWith(k, L"EFFECT_") || StartsWith(k, L"PARAM_");
        if (ref && ref->hasSkills && !fixed && !k.empty()) {
            if (!ref->skillGroups.count(k)) {
                if (!ref->skillCodes.count(k))
                    rowIssue(Sev::Error, L"V8", L"Bu anahtar istemcinin skill tablosunda yok: efekt hiç görünmez.",
                             L"Key not in the client's skill table: the effect never shows.", 2);
                else {
                    auto g = ref->groupOfCode.find(k);
                    if (g != ref->groupOfCode.end() && g->second != L"xxx" && !g->second.empty() && g->second != k)
                        rowIssue(Sev::Warn, L"V8", L"Skill bir gruba üye (" + g->second + L"): anahtar grup adı olmalı.",
                                 L"Skill is in group " + g->second + L": the key must be the group.", 2);
                }
            }
        }
    } else if (r.kind == TKind::SeEffectSet) {
        const int at = enums::AniTypeIndex(cell(2));
        const bool dmg = cell(4) == L"TRUE" || cell(4) == L"true";
        if (dmg && at >= 0 && !(at < 3 || at == 9))
            rowIssue(Sev::Error, L"V11", L"DMG TRUE yalnız READY/WAIT/SHOT/S_RETURN (istemci assert).", L"DMG TRUE only on READY/WAIT/SHOT/S_RETURN.", 4);
        // V9 for new / edited rows here; CheckDoc covers the loaded rows in one pass
        if ((r.line < 0 || d.RowEdited(r)) && !cell(1).empty() && d.FindKey(cell(1), TKind::SeAniSet).empty())
            rowIssue(Sev::Error, L"V9", L"Bu anahtarın skillaniset2 satırı yok: satırlar sahipsiz.", L"No skillaniset2 row for this key.", 1);
    }  // the 5000-bucket rule of characterdata / itemdata / skilldata is checked once per file (CheckDoc)
    if (r.line < 0 && d.InsideBlock(r.before))
        rowIssue(Sev::Error, L"V2", L"Yeni satır /* */ yorum bloğunun içine düşüyor: istemci okumaz.",
                 L"New row lands inside a /* */ block: the client never reads it.");
    for (size_t k = first; k < out.size(); ++k) {
        out[k].doc = docId;
        out[k].uid = r.uid;
        out[k].line = r.line;
    }
    Soften(d, out, first);
}

std::vector<CellIssue> CheckDoc(const EdDoc& d, int docId, const RefIndex* ref, bool onlyChanged) {
    std::vector<CellIssue> out;
    auto fileIssue = [&](Sev s, const wchar_t* code, const std::wstring& tr, const std::wstring& en, int line = -1) {
        CellIssue i;
        i.doc = docId;
        i.line = line;
        i.sev = s;
        i.code = code;
        i.msgTr = tr;
        i.msgEn = en;
        out.push_back(std::move(i));
    };
    if (!onlyChanged) {
        if (d.base.utf16 && !d.base.bom && !d.base.raw.empty())
            fileIssue(Sev::Error, L"V1", L"UTF-16 ama BOM (FF FE) yok: istemci ilk karakteri kaybeder.", L"UTF-16 without BOM: first char lost.");
        if (d.base.lfOnly) fileIssue(Sev::Error, L"V1", L"Satır sonları yalnız LF (CRLF olmalı).", L"LF-only line ends (CRLF expected).");
        if (!d.base.finalTerm && d.base.Count())
            fileIssue(Sev::Info, L"V1", L"Son satırda satır sonu yok.", L"No line end after the last line.");
        if (d.base.eofInBlock)
            fileIssue(Sev::Error, L"V2",
                      Fmt(L"Dosya kapanmamış /* bloğu içinde bitiyor (satır %d): sona eklenen her satır görünmez.", d.base.openBlockLine + 1),
                      Fmt(L"File ends inside an unclosed /* block (line %d): rows appended at the end are never read.", d.base.openBlockLine + 1),
                      d.base.openBlockLine);
        if (d.base.hiddenRows > 0)
            fileIssue(Sev::Warn, L"R4", Fmt(L"/* */ blokları içinde %d veri görünümlü satır var (istemci okumaz).", d.base.hiddenRows),
                      Fmt(L"%d data-looking rows inside /* */ blocks (never read).", d.base.hiddenRows));
        if (SchemaFor(d.kind).cp949 && d.base.utf16) fileIssue(Sev::Error, L"ENC", L"CP949 olmalı (UTF-16 bulundu).", L"Must be CP949 (UTF-16 found).");
        if (!SchemaFor(d.kind).cp949 && !d.base.utf16 && d.kind != TKind::Generic && d.kind != TKind::IndexFile)
            fileIssue(Sev::Error, L"ENC", L"UTF-16LE olmalı.", L"Must be UTF-16LE.");
    }
    // duplicate keys / IDs
    std::unordered_map<std::wstring, std::vector<int>> keys;
    std::unordered_map<long long, std::vector<int>> ids;
    for (size_t i = 0; i < d.rows.size(); ++i) {
        const EdRow& r = d.rows[i];
        if (r.deleted) continue;
        const Schema& s = d.SchemaOf(r);
        if (s.keyCol >= 0 && s.keyUnique && static_cast<size_t>(s.keyCol) < r.cells.size())
            keys[std::to_wstring(static_cast<int>(r.kind)) + L"|" + Trim(r.cells[static_cast<size_t>(s.keyCol)])].push_back(static_cast<int>(i));
        long long id = 0;
        if (s.idCol >= 0 && static_cast<size_t>(s.idCol) < r.cells.size() && ParseInt(r.cells[static_cast<size_t>(s.idCol)], id))
            ids[id].push_back(static_cast<int>(i));
    }
    auto dup = [&](const std::vector<int>& v, const wchar_t* code, const std::wstring& what, bool isKey) {
        if (v.size() < 2) return;
        for (size_t j = 0; j < v.size(); ++j) {
            const EdRow& r = d.rows[static_cast<size_t>(v[j])];
            if (onlyChanged && r.line >= 0 && !d.RowEdited(r)) continue;
            CellIssue i;
            i.doc = docId;
            i.uid = r.uid;
            i.line = r.line;
            const Schema& s = d.SchemaOf(r);
            i.col = isKey ? s.keyCol : s.idCol;
            i.sev = r.kind == TKind::SeCharInfo || r.kind == TKind::SeAniSet ? Sev::Error : Sev::Warn;
            i.code = code;
            i.msgTr = Fmt(L"%ls %zu kez geçiyor (%ls).", what.c_str(), v.size(),
                          r.kind == TKind::SeCharInfo ? L"istemci SONUNCUYU kullanır" : r.kind == TKind::SeAniSet ? L"alanlar üzerine yazılır" : L"çakışma");
            i.msgEn = Fmt(L"%ls appears %zu times.", what.c_str(), v.size());
            out.push_back(std::move(i));
        }
    };
    for (const auto& [k, v] : keys) dup(v, L"V6", L"Anahtar '" + k.substr(k.find(L'|') + 1) + L"'", true);
    for (const auto& [k, v] : ids) dup(v, L"ID", Fmt(L"ID %lld", k), false);
    for (const auto& r : d.rows) {
        if (r.deleted) continue;
        if (onlyChanged && r.line >= 0 && !d.RowEdited(r)) continue;
        CheckRow(d, docId, r, ref, out);
    }
    // V9 inside skilleffect: SE key needs an SA row (when no RefIndex gave the keys)
    if (d.kind == TKind::SeFile) {
        std::unordered_map<std::wstring, int> sa;
        for (const auto& r : d.rows)
            if (!r.deleted && r.kind == TKind::SeAniSet && r.cells.size() > 2) sa[r.cells[2]] = 1;
        for (const auto& r : d.rows) {
            if (r.deleted || r.kind != TKind::SeEffectSet || r.cells.size() < 2) continue;
            if (onlyChanged || r.line < 0 || d.RowEdited(r)) continue;  // CheckRow did those
            if (!sa.count(r.cells[1])) {
                CellIssue i;
                i.doc = docId;
                i.uid = r.uid;
                i.line = r.line;
                i.col = 1;
                i.sev = Sev::Error;
                i.code = L"V9";
                i.msgTr = L"Bu anahtarın skillaniset2 satırı yok: satırlar sahipsiz.";
                i.msgEn = L"No skillaniset2 row for this key.";
                out.push_back(std::move(i));
            }
        }
    }
    // once per file: IDs outside the 5000-bucket of the file name [U: the client may not care], generic tables whose
    // rows have another cell count than most rows
    if (!onlyChanged) {
        if (d.kind == TKind::CharacterData || d.kind == TKind::ItemData || d.kind == TKind::SkillData) {
            std::wstring digits;
            for (wchar_t ch : d.name)
                if (ch >= L'0' && ch <= L'9') digits += ch;
                else if (!digits.empty()) break;
            long long bucket = 0, firstBad = 0;
            int bad = 0;
            if (ParseInt(digits, bucket) && bucket >= 5000)
                for (const auto& r : d.rows) {
                    long long id = 0;
                    if (!r.deleted && r.cells.size() > 1 && ParseInt(r.cells[1], id) && (id < bucket - 5000 || id >= bucket) && bad++ == 0) firstBad = id;
                }
            if (bad)
                fileIssue(Sev::Info, L"BUCKET", Fmt(L"%d satırın ID'si dosya aralığı dışında (%lld..%lld; ör. %lld).", bad, bucket - 5000, bucket - 1, firstBad),
                          Fmt(L"%d rows have an ID outside the file's range (%lld..%lld; e.g. %lld).", bad, bucket - 5000, bucket - 1, firstBad));
        }
        if (d.kind == TKind::Generic && d.base.dominantCells > 0) {
            int bad = 0, line = -1;
            for (const auto& r : d.rows)
                if (!r.deleted && static_cast<int>(r.cells.size()) != d.base.dominantCells && bad++ == 0) line = r.line;
            if (bad)
                fileIssue(Sev::Info, L"COLS", Fmt(L"%d satırın sütun sayısı çoğunluktan (%d) farklı; ilki satır %d.", bad, d.base.dominantCells, line + 1),
                          Fmt(L"%d rows differ from the usual %d columns; first at line %d.", bad, d.base.dominantCells, line + 1), line);
        }
    }
    Soften(d, out, 0);
    return out;
}

}  // namespace se::ed
