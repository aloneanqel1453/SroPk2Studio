// SkillEffect Studio - editor/EdLang.cpp
// v6 P4 M19: names / translation model (see EdLang.h).
#include "EdLang.h"

#include "../core/Text.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <tuple>
#include <unordered_map>

namespace se::ed {

namespace {

// cell script flags
enum : unsigned {
    F_KO = 1, F_AR = 2, F_ZH = 4, F_JA = 8, F_TH = 16, F_RU = 32, F_VIU = 64, F_TRS = 128, F_TRW = 256, F_QM = 512,
    F_LEG = 1024, F_UML = 2048, F_ES = 4096, F_ASCII = 8192, F_QONLY = 16384
};

bool IsEuroLetter(wchar_t c) {
    static const wchar_t k[] = L"çÇöÖüÜäÄßéèêëáàâãíìîïóòôõúùûñÑÉÈÊÁÀÂÍÓÚ";
    return wcschr(k, c) != nullptr;
}

unsigned CellFlags(const std::wstring& s) {
    unsigned f = 0;
    bool letters = false, nonAscii = false;
    int legacy = 0, qm = 0, other = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        const unsigned o = c;
        if ((o >= 0xAC00 && o <= 0xD7A3) || (o >= 0x1100 && o <= 0x11FF) || (o >= 0x3130 && o <= 0x318F)) f |= F_KO;
        else if (o >= 0x0600 && o <= 0x06FF) f |= F_AR;
        else if (o >= 0x3040 && o <= 0x30FF) f |= F_JA;
        else if ((o >= 0x4E00 && o <= 0x9FFF) || (o >= 0x3400 && o <= 0x4DBF)) f |= F_ZH;
        else if (o >= 0x0E00 && o <= 0x0E7F) f |= F_TH;
        else if (o >= 0x0400 && o <= 0x04FF) f |= F_RU;
        else if ((o >= 0x1EA0 && o <= 0x1EF9) || o == 0x01A1 || o == 0x01B0 || o == 0x0111 || o == 0x0110 || o == 0x01A0 || o == 0x01AF) f |= F_VIU;
        else if (c == L'ğ' || c == L'Ğ' || c == L'ş' || c == L'Ş' || c == L'ı' || c == L'İ') f |= F_TRS;
        else if (c == L'ç' || c == L'Ç' || c == L'ö' || c == L'Ö' || c == L'ü' || c == L'Ü') f |= F_TRW;
        if (c == L'ä' || c == L'Ä' || c == L'ß') f |= F_UML;
        if (c == L'ñ' || c == L'Ñ' || c == L'¿' || c == L'¡') f |= F_ES;
        if (c == L'?' && ((i > 0 && iswalpha(s[i - 1])) || (i + 1 < s.size() && iswalpha(s[i + 1])))) f |= F_QM;
        if (o >= 0xA0 && o <= 0xFF && !IsEuroLetter(c)) ++legacy;
        if (o >= 0x80) nonAscii = true;
        if (iswalpha(c)) letters = true;
        if (c == L'?') ++qm;
        else if (iswalnum(c)) ++other;
    }
    if (qm >= 2 && qm >= other) f |= F_QONLY;  // "?? ?? (??)": text whose letters were lost in a code page conversion
    if (legacy >= 2) f |= F_LEG;
    if (letters && !nonAscii) f |= F_ASCII;
    return f;
}

std::wstring LowerS(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

bool StartsI(const std::wstring& s, const wchar_t* p) {
    const size_t n = wcslen(p);
    return s.size() >= n && _wcsnicmp(s.c_str(), p, n) == 0;
}

bool EndsI(const std::wstring& s, const wchar_t* p) {
    const size_t n = wcslen(p);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, p) == 0;
}

// standard 17-column layout (EdSchema name table: KOR 2, ENG 8, VNM 9, TUR 12, ARA 14; DE 15 seen in clients)
int StandardCol(LangId l) {
    switch (l) {
        case LangId::KR: return 2;
        case LangId::EN: return 8;
        case LangId::VI: return 9;
        case LangId::TR: return 12;
        case LangId::AR: return 14;
        case LangId::DE: return 15;
        default: return -1;
    }
}
LangId StandardLang(int col) {
    switch (col) {
        case 2: return LangId::KR;
        case 8: return LangId::EN;
        case 9: return LangId::VI;
        case 12: return LangId::TR;
        case 14: return LangId::AR;
        case 15: return LangId::DE;
        default: return LangId::Other;
    }
}

const std::wstring& CellOf(const EdRow& r, int c) {
    static const std::wstring empty;
    return c >= 0 && static_cast<size_t>(c) < r.cells.size() ? r.cells[static_cast<size_t>(c)] : empty;
}

std::wstring Pct(double v) { return Fmt(L"%%%d", static_cast<int>(v * 100 + 0.5)); }

}  // namespace

const wchar_t* LangCode(LangId l) {
    static const wchar_t* k[] = {L"KR", L"EN", L"TR", L"AR", L"ZH", L"JA", L"TH", L"RU", L"VI", L"DE", L"ES", L"L"};
    const int i = static_cast<int>(l);
    return i >= 0 && i < static_cast<int>(LangId::Count) ? k[i] : L"L";
}

const wchar_t* LangName(LangId l, bool en) {
    static const wchar_t* tr[] = {L"Korece", L"İngilizce", L"Türkçe", L"Arapça", L"Çince", L"Japonca", L"Tayca", L"Rusça", L"Vietnamca",
                                  L"Almanca", L"İspanyolca", L"Diğer dil"};
    static const wchar_t* e[] = {L"Korean", L"English", L"Turkish", L"Arabic", L"Chinese", L"Japanese", L"Thai", L"Russian", L"Vietnamese",
                                 L"German", L"Spanish", L"Other language"};
    const int i = std::clamp(static_cast<int>(l), 0, static_cast<int>(LangId::Count) - 1);
    return en ? e[i] : tr[i];
}

const wchar_t* NameKindText(NameKind k, bool en) {
    static const wchar_t* tr[] = {L"Canavar", L"NPC", L"Eşya", L"Skill", L"Pet / binek", L"Yapı", L"Görev", L"Diğer"};
    static const wchar_t* e[] = {L"Monster", L"NPC", L"Item", L"Skill", L"Pet / mount", L"Structure", L"Quest", L"Other"};
    const int i = std::clamp(static_cast<int>(k), 0, static_cast<int>(NameKind::Count) - 1);
    return en ? e[i] : tr[i];
}

NameKind NameKindOf(const std::wstring& code, bool* isDesc) {
    std::wstring c = code;
    if (StartsI(c, L"SN_")) c = c.substr(3);
    if (isDesc) *isDesc = EndsI(code, L"_TT_DESC") || EndsI(code, L"_DESC") || code.find(L"_TT_") != std::wstring::npos;
    if (StartsI(c, L"MOB_")) return NameKind::Mob;
    if (StartsI(c, L"NPC_")) return NameKind::Npc;
    if (StartsI(c, L"ITEM_")) return NameKind::Item;
    if (StartsI(c, L"SKILL_") || StartsI(c, L"MSKILL_")) return NameKind::Skill;
    if (StartsI(c, L"COS_")) return NameKind::Pet;
    if (StartsI(c, L"STRUCTURE") || StartsI(c, L"STR_") || StartsI(c, L"GATE")) return NameKind::Structure;
    if (StartsI(c, L"QUEST") || StartsI(c, L"QNO_") || StartsI(c, L"QSP_")) return NameKind::Quest;
    return NameKind::Other;
}

// ------------------------------------------------------------------------------------------------ build

bool LangModel::Build(Workspace& ws, std::wstring& err) {
    std::vector<std::wstring> names;
    if (ws.client) {
        Bytes raw;
        if (ws.client->ReadTextdata(L"textdataname.txt", raw))
            for (const auto& n : ReadIndexList(raw))
                if (StartsI(n, L"textdata_object") || StartsI(n, L"textdata_equip")) names.push_back(n);
    }
    if (names.empty()) names = {L"textdata_object.txt", L"textdata_equip&skill.txt"};
    std::vector<int> docs;
    std::wstring firstErr;
    for (const auto& n : names) {
        std::wstring e;
        const int id = ws.Open(n, e);
        if (id >= 0) {
            if (std::find(docs.begin(), docs.end(), id) == docs.end()) docs.push_back(id);
        } else if (firstErr.empty()) {
            firstErr = n + L": " + e;
        }
    }
    if (docs.empty()) {
        err = firstErr.empty() ? L"ad tablosu yok / no name table" : firstErr;
        return false;
    }
    return BuildFrom(ws, docs);
}

bool LangModel::BuildFrom(Workspace& ws, const std::vector<int>& docs) {
    m_ws = &ws;
    m_tables.clear();
    m_rows.clear();
    m_slots.clear();
    for (int id : docs) {
        const EdDoc* d = ws.Doc(id);
        if (!d) continue;
        LangTable t;
        t.doc = id;
        t.ptr = d;
        t.name = d->name;
        Detect(t, *d);
        m_tables.push_back(std::move(t));
    }
    MakeSlots();
    // rows
    for (size_t ti = 0; ti < m_tables.size(); ++ti) {
        const EdDoc* d = m_tables[ti].ptr;
        std::unordered_map<std::wstring, int> occ;
        for (const auto& r : d->rows) {
            if (r.deleted) continue;
            const std::wstring code = Trim(CellOf(r, 1));
            if (code.empty()) continue;
            LangRow lr;
            lr.table = static_cast<int>(ti);
            lr.uid = r.uid;
            lr.code = code;
            lr.occ = occ[code]++;
            lr.kind = NameKindOf(code, &lr.desc);
            m_rows.push_back(std::move(lr));
        }
    }
    return !m_tables.empty();
}

void LangModel::Detect(LangTable& t, const EdDoc& d) {
    std::map<int, int> cellCounts;
    int maxCells = 0;
    std::vector<const EdRow*> rows;
    rows.reserve(d.rows.size());
    for (const auto& r : d.rows) {
        if (r.deleted || Trim(CellOf(r, 1)).empty()) continue;
        rows.push_back(&r);
        ++cellCounts[static_cast<int>(r.cells.size())];
        maxCells = std::max(maxCells, static_cast<int>(r.cells.size()));
    }
    t.rows = static_cast<int>(rows.size());
    int best = 0;
    for (const auto& [n, c] : cellCounts)
        if (c > best) best = c, t.cellsMax = n;
    struct Stat {
        int filled = 0;
        int n[16] = {};
    };
    std::vector<Stat> st(static_cast<size_t>(std::max(maxCells, 0)));
    for (const EdRow* r : rows)
        for (size_t c = 2; c < r->cells.size() && c < st.size(); ++c) {
            const std::wstring& v = r->cells[c];
            if (!HasText(v)) continue;
            Stat& s = st[c];
            ++s.filled;
            const unsigned f = CellFlags(v);
            for (int b = 0; b < 15; ++b)
                if (f & (1u << b)) ++s.n[b];
        }
    const int minFilled = std::max(3, t.rows / 2000);
    for (size_t c = 2; c < st.size(); ++c) {
        const Stat& s = st[c];
        if (s.filled < minFilled) continue;
        auto sh = [&](unsigned flag) {
            int b = 0;
            while ((1u << b) != flag) ++b;
            return static_cast<double>(s.n[b]) / s.filled;
        };
        LangCol lc;
        lc.col = static_cast<int>(c);
        lc.filled = s.filled;
        auto pick = [&](LangId l, double v, const wchar_t* what) {
            lc.lang = l;
            lc.share = v;
            lc.note = std::wstring(what) + L" " + Pct(v);
        };
        // script-specific letters: a small share is enough (the other cells are often English copies)
        if (sh(F_KO) >= 0.3) pick(LangId::KR, sh(F_KO), L"Hangul");
        else if (sh(F_AR) >= 0.1) pick(LangId::AR, sh(F_AR), L"Arabic");
        else if (sh(F_TH) >= 0.1) pick(LangId::TH, sh(F_TH), L"Thai");
        else if (sh(F_RU) >= 0.1) pick(LangId::RU, sh(F_RU), L"Cyrillic");
        else if (sh(F_JA) >= 0.1) pick(LangId::JA, sh(F_JA), L"Kana");
        else if (sh(F_ZH) >= 0.1) pick(LangId::ZH, sh(F_ZH), L"CJK");
        else if (sh(F_QONLY) >= 0.5 && StandardLang(static_cast<int>(c)) != LangId::Other)
            pick(StandardLang(static_cast<int>(c)), sh(F_QONLY), L"'?' (lost letters, standard column)");
        else if (sh(F_VIU) >= 0.2) pick(LangId::VI, sh(F_VIU), L"Vietnamese");
        else if (sh(F_TRS) >= 0.05) pick(LangId::TR, sh(F_TRS), L"ğşı");
        else if (sh(F_TRW) >= 0.1 && sh(F_QM) >= 0.05) pick(LangId::TR, sh(F_TRW), L"çöü + ?");
        else if (sh(F_LEG) >= 0.15) pick(LangId::VI, sh(F_LEG), L"legacy VNI/TCVN");
        else if (sh(F_UML) >= 0.03) pick(LangId::DE, sh(F_UML), L"äß");
        else if (sh(F_ES) >= 0.03) pick(LangId::ES, sh(F_ES), L"ñ¿¡");
        else if (sh(F_ASCII) >= 0.5) pick(LangId::EN, sh(F_ASCII), L"ASCII");
        else {
            lc.lang = LangId::Other;
            lc.share = 0;
            lc.note = L"?";
        }
        t.cols.push_back(std::move(lc));
    }
    // main EN = the most filled ASCII column; the others: copies of it, a standard-layout language, or EN #2
    int main = -1;
    for (size_t i = 0; i < t.cols.size(); ++i)
        if (t.cols[i].lang == LangId::EN && (main < 0 || t.cols[i].filled > t.cols[static_cast<size_t>(main)].filled)) main = static_cast<int>(i);
    if (main >= 0) {
        const int mc = t.cols[static_cast<size_t>(main)].col;
        for (size_t i = 0; i < t.cols.size(); ++i) {
            LangCol& c = t.cols[i];
            if (static_cast<int>(i) == main || c.lang == LangId::KR || c.lang == LangId::AR || c.lang == LangId::ZH || c.lang == LangId::JA ||
                c.lang == LangId::TH || c.lang == LangId::RU)
                continue;
            int same = 0, both = 0;
            for (const EdRow* r : rows) {
                const std::wstring& v = CellOf(*r, c.col);
                if (!HasText(v)) continue;
                ++both;
                if (v == CellOf(*r, mc)) ++same;
            }
            if (c.lang == LangId::EN && both && same >= both * 6 / 10) {
                c.copyOfEn = true;
                c.note += L", = EN " + Pct(static_cast<double>(same) / both);
            } else if (c.lang == LangId::EN) {
                const LangId sl = StandardLang(c.col);
                bool taken = false;
                for (const auto& o : t.cols)
                    if (o.lang == sl && &o != &c) taken = true;
                if (sl != LangId::Other && sl != LangId::EN && sl != LangId::KR && !taken) {
                    c.lang = sl;
                    c.note += L", standard column (estimated)";
                }
            }
        }
    }
}

void LangModel::MakeSlots() {
    // a plain-ASCII column that another table names as a Latin language (TR / DE / ES / VI) at the same index is
    // that language too (its rows are mostly English copies, the translated ones carry the letters)
    for (size_t ti = 0; ti < m_tables.size(); ++ti)
        for (auto& c : m_tables[ti].cols) {
            if (c.lang != LangId::EN || c.copyOfEn) continue;
            bool isMain = true;
            for (const auto& o : m_tables[ti].cols)
                if (o.lang == LangId::EN && !o.copyOfEn && &o != &c && o.filled > c.filled) isMain = false;
            if (isMain) continue;
            for (size_t o = 0; o < m_tables.size(); ++o)
                if (o != ti)
                    for (const auto& oc : m_tables[o].cols)
                        if (oc.col == c.col && (oc.lang == LangId::TR || oc.lang == LangId::DE || oc.lang == LangId::ES || oc.lang == LangId::VI)) {
                            c.lang = oc.lang;
                            c.note += L", = " + m_tables[o].name;
                        }
        }
    // slots: KR, EN, TR always; other languages found in any table; a second EN (not a copy) when found
    std::vector<std::pair<LangId, int>> want = {{LangId::KR, 0}, {LangId::EN, 0}, {LangId::TR, 0}};
    auto has = [&](LangId l, int nth) {
        for (const auto& w : want)
            if (w.first == l && w.second == nth) return true;
        return false;
    };
    // per table: the language columns in order (main first: the most filled of each language)
    std::vector<std::map<std::pair<int, int>, int>> found(m_tables.size());  // (lang, nth) -> col
    for (size_t ti = 0; ti < m_tables.size(); ++ti) {
        std::vector<const LangCol*> cs;
        for (const auto& c : m_tables[ti].cols)
            if (!c.copyOfEn && c.lang != LangId::Other) cs.push_back(&c);
        std::stable_sort(cs.begin(), cs.end(), [](const LangCol* a, const LangCol* b) {
            return a->lang != b->lang ? static_cast<int>(a->lang) < static_cast<int>(b->lang) : a->filled > b->filled;
        });
        std::map<int, int> nth;
        for (const LangCol* c : cs) {
            const int k = nth[static_cast<int>(c->lang)]++;
            // a second column of a language is a page column only when it holds real text (5 % of the rows)
            if (k > 1 || (k == 1 && c->filled * 20 < m_tables[ti].rows)) continue;
            found[ti][{static_cast<int>(c->lang), k}] = c->col;
            if (!has(c->lang, k)) want.emplace_back(c->lang, k);
        }
    }
    std::stable_sort(want.begin(), want.end(), [](const auto& a, const auto& b) {
        auto rank = [](LangId l) { return l == LangId::KR ? 0 : l == LangId::EN ? 1 : l == LangId::TR ? 2 : 3 + static_cast<int>(l); };
        return rank(a.first) != rank(b.first) ? rank(a.first) < rank(b.first) : a.second < b.second;
    });
    for (const auto& [l, n] : want) {
        LangSlot s;
        s.lang = l;
        s.nth = n;
        s.code = std::wstring(LangCode(l)) + (n ? std::to_wstring(n + 1) : L"");
        s.guess = true;
        for (const auto& f : found)
            if (f.count({static_cast<int>(l), n})) s.guess = false;
        m_slots.push_back(std::move(s));
    }
    for (size_t ti = 0; ti < m_tables.size(); ++ti) {
        LangTable& t = m_tables[ti];
        t.slotCol.assign(m_slots.size(), -1);
        t.slotGuess.assign(m_slots.size(), 0);
        auto usedByOther = [&](int col, LangId l) {
            for (const auto& c : t.cols)
                if (c.col == col && c.lang != l && !c.copyOfEn && c.lang != LangId::Other) return true;
            for (size_t k = 0; k < m_slots.size(); ++k)
                if (t.slotCol[k] == col && m_slots[k].lang != l) return true;
            return false;
        };
        for (size_t k = 0; k < m_slots.size(); ++k) {
            const auto key = std::make_pair(static_cast<int>(m_slots[k].lang), m_slots[k].nth);
            auto it = found[ti].find(key);
            if (it != found[ti].end()) t.slotCol[k] = it->second;
        }
        for (size_t k = 0; k < m_slots.size(); ++k) {
            if (t.slotCol[k] >= 0) continue;
            const auto key = std::make_pair(static_cast<int>(m_slots[k].lang), m_slots[k].nth);
            int col = -1;
            for (size_t o = 0; o < m_tables.size() && col < 0; ++o)
                if (o != ti) {
                    auto it = found[o].find(key);
                    if (it != found[o].end() && !usedByOther(it->second, m_slots[k].lang)) col = it->second;
                }
            if (col < 0 && m_slots[k].nth == 0) {
                const int sc = StandardCol(m_slots[k].lang);
                if (sc >= 0 && !usedByOther(sc, m_slots[k].lang)) col = sc;
            }
            if (col >= 0) {
                t.slotCol[k] = col;
                t.slotGuess[k] = 1;
            }
        }
    }
}

bool LangModel::Valid(const Workspace& ws) const {
    if (m_ws != &ws || m_tables.empty()) return false;
    for (const auto& t : m_tables)
        if (ws.Doc(t.doc) != t.ptr) return false;
    return true;
}

const EdDoc* LangModel::DocOf(int table) const {
    if (!m_ws || table < 0 || table >= static_cast<int>(m_tables.size())) return nullptr;
    const LangTable& t = m_tables[static_cast<size_t>(table)];
    const EdDoc* d = m_ws->Doc(t.doc);
    return d == t.ptr ? d : nullptr;
}

int LangModel::SlotOf(LangId l, int nth) const {
    for (size_t i = 0; i < m_slots.size(); ++i)
        if (m_slots[i].lang == l && m_slots[i].nth == nth) return static_cast<int>(i);
    return -1;
}

std::wstring LangModel::SlotLabel(int slot, bool en) const {
    if (slot < 0 || slot >= static_cast<int>(m_slots.size())) return L"";
    const LangSlot& s = m_slots[static_cast<size_t>(slot)];
    std::wstring l = s.code;
    if (s.guess) l += en ? L" (estimated)" : L" (tahmini)";
    return l;
}

// ------------------------------------------------------------------------------------------------ cells

bool LangModel::HasText(const std::wstring& v) {
    bool any = false;
    for (wchar_t c : v) {
        if (c == L' ' || c == L'\t') continue;
        if (!(c >= L'0' && c <= L'9') && c != L'-' && c != L'.') return true;
        any = true;
    }
    (void)any;
    return false;
}

std::wstring LangModel::Cell(int row, int slot) const {
    if (row < 0 || row >= static_cast<int>(m_rows.size()) || slot < 0 || slot >= static_cast<int>(m_slots.size())) return L"";
    const LangRow& lr = m_rows[static_cast<size_t>(row)];
    const EdDoc* d = DocOf(lr.table);
    if (!d) return L"";
    const EdRow* r = d->Row(lr.uid);
    if (!r) return L"";
    return CellOf(*r, m_tables[static_cast<size_t>(lr.table)].slotCol[static_cast<size_t>(slot)]);
}

bool LangModel::CellEdited(int row, int slot) const {
    if (row < 0 || row >= static_cast<int>(m_rows.size()) || slot < 0 || slot >= static_cast<int>(m_slots.size())) return false;
    const LangRow& lr = m_rows[static_cast<size_t>(row)];
    const EdDoc* d = DocOf(lr.table);
    const EdRow* r = d ? d->Row(lr.uid) : nullptr;
    if (!r) return false;
    const int c = m_tables[static_cast<size_t>(lr.table)].slotCol[static_cast<size_t>(slot)];
    if (c < 0) return false;
    if (r->line < 0) return HasText(CellOf(*r, c));
    const auto orig = d->OrigCells(*r);
    const std::wstring o = static_cast<size_t>(c) < orig.size() ? orig[static_cast<size_t>(c)] : std::wstring();
    return o != CellOf(*r, c);
}

bool LangModel::Match(int row, LangFilter f) const {
    if (f == LangFilter::All) return true;
    const int kr = SlotOf(LangId::KR), en = SlotOf(LangId::EN), tr = SlotOf(LangId::TR);
    switch (f) {
        case LangFilter::MissingTr: {
            if (tr < 0) return false;
            const LangRow& lr = m_rows[static_cast<size_t>(row)];
            if (m_tables[static_cast<size_t>(lr.table)].slotCol[static_cast<size_t>(tr)] < 0) return false;
            return !HasText(Cell(row, tr)) && (HasText(Cell(row, en)) || HasText(Cell(row, kr)));
        }
        case LangFilter::NoName:
            for (size_t s = 0; s < m_slots.size(); ++s)
                if (HasText(Cell(row, static_cast<int>(s)))) return false;
            return true;
        case LangFilter::EnEqKr: {
            const std::wstring e = Cell(row, en);
            return HasText(e) && e == Cell(row, kr);
        }
        default: return true;
    }
}

std::vector<int> LangModel::Filter(LangFilter f, const std::wstring& text) const {
    std::vector<int> v;
    const std::wstring q = LowerS(Trim(text));
    v.reserve(m_rows.size());
    for (size_t i = 0; i < m_rows.size(); ++i) {
        if (!Match(static_cast<int>(i), f)) continue;
        if (!q.empty()) {
            bool hit = LowerS(m_rows[i].code).find(q) != std::wstring::npos;
            for (size_t s = 0; !hit && s < m_slots.size(); ++s) hit = LowerS(Cell(static_cast<int>(i), static_cast<int>(s))).find(q) != std::wstring::npos;
            if (!hit) continue;
        }
        v.push_back(static_cast<int>(i));
    }
    return v;
}

int LangModel::Count(LangFilter f) const {
    int n = 0;
    for (size_t i = 0; i < m_rows.size(); ++i) n += Match(static_cast<int>(i), f);
    return n;
}

std::wstring LangModel::Report(bool en) const {
    std::wstring s;
    for (const auto& t : m_tables) {
        s += Fmt(en ? L"%ls: %d rows, %d columns |" : L"%ls: %d satır, %d sütun |", t.name.c_str(), t.rows, t.cellsMax);
        for (const auto& c : t.cols)
            s += Fmt(L" %ls=%d", c.copyOfEn ? (en ? L"EN-copy" : L"EN-kopya") : LangCode(c.lang), c.col) + L"(" + std::to_wstring(c.filled) + L", " + c.note + L")";
        s += en ? L" | page:" : L" | sayfa:";
        for (size_t k = 0; k < m_slots.size(); ++k)
            s += L" " + m_slots[k].code + L"=" + (t.slotCol[k] < 0 ? std::wstring(L"-") : std::to_wstring(t.slotCol[k])) + (t.slotGuess[k] ? L"?" : L"");
        s += L"\n";
    }
    return s;
}

// ------------------------------------------------------------------------------------------------ edits

bool LangModel::SetText(int row, int slot, const std::wstring& value, std::wstring* whyTr, std::wstring* whyEn) {
    auto fail = [&](const wchar_t* tr, const wchar_t* en) {
        if (whyTr) *whyTr = tr;
        if (whyEn) *whyEn = en;
        return false;
    };
    if (value.find_first_of(L"\t\r\n") != std::wstring::npos) return fail(L"Adda TAB / satır sonu olamaz.", L"A name cannot hold TAB / line breaks.");
    if (row < 0 || row >= static_cast<int>(m_rows.size()) || slot < 0 || slot >= static_cast<int>(m_slots.size()) || !m_ws)
        return fail(L"Satır yok.", L"No such row.");
    const LangRow& lr = m_rows[static_cast<size_t>(row)];
    const LangTable& t = m_tables[static_cast<size_t>(lr.table)];
    const int col = t.slotCol[static_cast<size_t>(slot)];
    if (col < 0 || !DocOf(lr.table)) return fail(L"Bu tabloda bu dilin sütunu yok.", L"This table has no column for this language.");
    if (Cell(row, slot) == value) return true;
    m_ws->Begin(L"Ad / name: " + lr.code);
    const bool ok = m_ws->SetCell(t.doc, lr.uid, col, value);
    m_ws->End();
    return ok ? true : fail(L"Değiştirilemedi.", L"Could not be changed.");
}

int LangModel::SetTexts(const std::vector<std::tuple<int, int, std::wstring>>& edits, const std::wstring& label) {
    if (!m_ws) return 0;
    int n = 0;
    m_ws->Begin(label);
    for (const auto& [row, slot, v] : edits) {
        if (row < 0 || row >= static_cast<int>(m_rows.size()) || slot < 0 || slot >= static_cast<int>(m_slots.size())) continue;
        if (v.find_first_of(L"\t\r\n") != std::wstring::npos) continue;
        const LangRow& lr = m_rows[static_cast<size_t>(row)];
        const LangTable& t = m_tables[static_cast<size_t>(lr.table)];
        const int col = t.slotCol[static_cast<size_t>(slot)];
        if (col < 0 || !DocOf(lr.table) || Cell(row, slot) == v) continue;
        if (m_ws->SetCell(t.doc, lr.uid, col, v)) ++n;
    }
    m_ws->End();
    return n;
}

int LangModel::FindRow(const std::wstring& table, const std::wstring& code, int occ) const {
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const LangRow& r = m_rows[i];
        if (r.occ != occ || _wcsicmp(r.code.c_str(), code.c_str()) != 0) continue;
        if (!table.empty() && _wcsicmp(m_tables[static_cast<size_t>(r.table)].name.c_str(), table.c_str()) != 0) continue;
        return static_cast<int>(i);
    }
    return -1;
}

// ------------------------------------------------------------------------------------------------ CSV / TSV

std::wstring CsvField(const std::wstring& v, wchar_t delim) {
    const bool quote = v.find(delim) != std::wstring::npos || v.find(L'"') != std::wstring::npos || v.find_first_of(L"\r\n") != std::wstring::npos ||
                       (!v.empty() && (v.front() == L' ' || v.back() == L' '));
    if (!quote) return v;
    std::wstring s = L"\"";
    for (wchar_t c : v) {
        if (c == L'"') s += L'"';
        s += c;
    }
    return s + L"\"";
}

std::vector<std::vector<std::wstring>> CsvParse(const std::wstring& text, wchar_t delim) {
    std::vector<std::vector<std::wstring>> out;
    std::vector<std::wstring> row;
    std::wstring f;
    bool inQ = false, any = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (inQ) {
            if (c == L'"') {
                if (i + 1 < text.size() && text[i + 1] == L'"') {
                    f += L'"';
                    ++i;
                } else {
                    inQ = false;
                }
            } else {
                f += c;
            }
            continue;
        }
        if (c == L'"' && f.empty()) {
            inQ = true;
            any = true;
        } else if (c == delim) {
            row.push_back(std::move(f));
            f.clear();
            any = true;
        } else if (c == L'\r' || c == L'\n') {
            if (c == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            if (any || !f.empty()) {
                row.push_back(std::move(f));
                out.push_back(std::move(row));
            }
            row.clear();
            f.clear();
            any = false;
        } else {
            f += c;
            any = true;
        }
    }
    if (any || !f.empty()) {
        row.push_back(std::move(f));
        out.push_back(std::move(row));
    }
    return out;
}

Bytes LangModel::Export(const std::vector<int>& rows, bool tsv, bool en) const {
    const wchar_t d = tsv ? L'\t' : L',';
    std::wstring s = std::wstring(en ? L"table" : L"tablo") + d + (en ? L"code" : L"kod") + d + (en ? L"kind" : L"tür");
    for (const auto& sl : m_slots) s += d + sl.code;
    s += L"\r\n";
    for (int i : rows) {
        if (i < 0 || i >= static_cast<int>(m_rows.size())) continue;
        const LangRow& r = m_rows[static_cast<size_t>(i)];
        const std::wstring code = r.occ ? r.code + L"#" + std::to_wstring(r.occ + 1) : r.code;
        s += CsvField(m_tables[static_cast<size_t>(r.table)].name, d) + d + CsvField(code, d) + d +
             CsvField(std::wstring(NameKindText(r.kind, en)) + (r.desc ? (en ? L" (description)" : L" (açıklama)") : L""), d);
        for (size_t k = 0; k < m_slots.size(); ++k) s += d + CsvField(Cell(i, static_cast<int>(k)), d);
        s += L"\r\n";
    }
    const std::string u = ToUtf8(s);
    Bytes b = {0xEF, 0xBB, 0xBF};
    b.insert(b.end(), u.begin(), u.end());
    return b;
}

LangImport LangModel::Import(const Bytes& data, bool apply) {
    LangImport res;
    auto note = [&](const std::wstring& tr, const std::wstring& en) {
        if (res.notesTr.size() < 20) {
            res.notesTr.push_back(tr);
            res.notesEn.push_back(en);
        }
    };
    const std::wstring text = DecodeText(data);
    // delimiter from the header line (outside quotes): TAB, ';' (Turkish Excel) or ','
    size_t eol = text.find_first_of(L"\r\n");
    const std::wstring head = text.substr(0, eol == std::wstring::npos ? text.size() : eol);
    int nt = 0, ns = 0, nc = 0;
    bool q = false;
    for (wchar_t c : head) {
        if (c == L'"') q = !q;
        if (q) continue;
        nt += c == L'\t';
        ns += c == L';';
        nc += c == L',';
    }
    const wchar_t delim = nt ? L'\t' : (ns > nc ? L';' : L',');
    auto rows = CsvParse(text, delim);
    if (rows.empty()) {
        res.errTr = L"Dosya boş.";
        res.errEn = L"The file is empty.";
        return res;
    }
    // header
    int cTable = -1, cCode = -1;
    std::vector<std::pair<int, int>> langCols;  // (csv column, slot)
    const auto& h = rows[0];
    for (size_t i = 0; i < h.size(); ++i) {
        std::wstring n = Trim(h[i]);
        if (i == 0 && !n.empty() && n[0] == 0xFEFF) n = n.substr(1);
        const std::wstring u = ToUpper(n);
        if (u == L"TABLO" || u == L"TABLE") cTable = static_cast<int>(i);
        else if (u == L"KOD" || u == L"CODE" || u == L"STRID") cCode = static_cast<int>(i);
        else
            for (size_t k = 0; k < m_slots.size(); ++k)
                if (u == ToUpper(m_slots[k].code)) langCols.emplace_back(static_cast<int>(i), static_cast<int>(k));
    }
    if (cCode < 0) {
        res.errTr = L"Başlık satırında \"kod\" sütunu yok.";
        res.errEn = L"The header line has no \"code\" column.";
        return res;
    }
    if (langCols.empty()) {
        res.errTr = L"Başlık satırında dil sütunu (EN, TR ...) yok.";
        res.errEn = L"The header line has no language column (EN, TR ...).";
        return res;
    }
    std::vector<std::tuple<int, int, std::wstring>> edits;
    for (size_t ri = 1; ri < rows.size(); ++ri) {
        const auto& r = rows[ri];
        auto get = [&](int c) { return c >= 0 && static_cast<size_t>(c) < r.size() ? r[static_cast<size_t>(c)] : std::wstring(); };
        std::wstring code = Trim(get(cCode));
        if (code.empty()) continue;
        ++res.rows;
        int occ = 0;
        const size_t hash = code.rfind(L'#');
        const std::wstring table = Trim(get(cTable));
        int row = FindRow(table, code, 0);
        if (row < 0 && hash != std::wstring::npos && hash + 1 < code.size() && iswdigit(code[hash + 1])) {
            occ = std::max(0, _wtoi(code.c_str() + hash + 1) - 1);
            row = FindRow(table, code.substr(0, hash), occ);
        }
        if (row < 0 && !table.empty()) row = FindRow(L"", hash != std::wstring::npos && occ ? code.substr(0, hash) : code, occ);
        if (row < 0) {
            ++res.unknown;
            note(Fmt(L"satır %zu: kod bulunamadı: %ls", ri + 1, code.c_str()), Fmt(L"line %zu: code not found: %ls", ri + 1, code.c_str()));
            continue;
        }
        ++res.matched;
        bool changed = false;
        const LangTable& t = m_tables[static_cast<size_t>(m_rows[static_cast<size_t>(row)].table)];
        for (const auto& [cc, slot] : langCols) {
            if (static_cast<size_t>(cc) >= r.size()) continue;  // a short line: the missing cells are not touched
            const std::wstring& v = r[static_cast<size_t>(cc)];
            if (v == Cell(row, slot)) continue;
            if (v.find_first_of(L"\t\r\n") != std::wstring::npos) {
                ++res.badCells;
                note(Fmt(L"satır %zu: %ls hücresinde TAB / satır sonu var (atlandı)", ri + 1, m_slots[static_cast<size_t>(slot)].code.c_str()),
                     Fmt(L"line %zu: the %ls cell holds TAB / a line break (skipped)", ri + 1, m_slots[static_cast<size_t>(slot)].code.c_str()));
                continue;
            }
            if (t.slotCol[static_cast<size_t>(slot)] < 0) {
                ++res.noColumn;
                continue;
            }
            edits.emplace_back(row, slot, v);
            ++res.changedCells;
            changed = true;
        }
        res.changedRows += changed;
    }
    if (apply && !edits.empty()) SetTexts(edits, Fmt(L"İçe aktar / import: %d", res.changedCells));
    return res;
}

}  // namespace se::ed
