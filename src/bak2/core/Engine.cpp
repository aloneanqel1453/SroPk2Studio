// SkillEffect Studio - core/Engine.cpp
// Placement + merge + verification. Default placement "safe" (user's working in-game file, see Engine.h): characterInfo /
// skillaniset2 rows at the end of their section (right before the next '#section' line), skilleffectset rows right
// before the first '/*' line after its header; list order kept, no extra lines, existing keys replaced in place.
// Optional placement ("smart"): a new unit goes into the run of existing
// units of its family (same first two code tokens, e.g. MOB_CH = //중국 몬스터), ordered by level/ID
// from characterdata; skilleffectset follows the skillaniset2 order; unknown families start a new
// group at the end of their kind (MOB_/SKILL_ ...). Every position is checked against the client's
// comment state machine; the merged file is re-read with the client model before it can be written.
#include "Engine.h"

#include "Enums.h"

#include <algorithm>
#include <climits>
#include <map>
#include <tuple>
#include <unordered_map>

namespace se {

namespace {

std::vector<std::wstring> Tok(const std::wstring& c) {
    std::vector<std::wstring> v;
    size_t a = 0;
    while (a <= c.size()) {
        size_t b = c.find(L'_', a);
        if (b == std::wstring::npos) b = c.size();
        if (b > a) v.push_back(c.substr(a, b - a));
        a = b + 1;
    }
    return v;
}

std::wstring Join(const std::vector<std::wstring>& t, size_t n) {
    std::wstring r;
    for (size_t i = 0; i < n && i < t.size(); ++i) {
        if (i) r += L'_';
        r += t[i];
    }
    return r;
}

int CommonPrefix(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b) {
    int n = 0;
    while (static_cast<size_t>(n) < a.size() && static_cast<size_t>(n) < b.size() && a[static_cast<size_t>(n)] == b[static_cast<size_t>(n)]) ++n;
    return n;
}

bool IsSkillToken(const std::wstring& t) {
    static const wchar_t* heads[] = {L"ATTACK", L"SUMMON", L"SKILL", L"BUFF", L"DEBUFF", L"HEAL", L"MAGIC", L"SPELL"};
    for (const auto* h : heads) {
        const std::wstring hs(h);
        if (!StartsWith(t, hs)) continue;
        size_t i = hs.size();
        while (i < t.size() && t[i] >= L'0' && t[i] <= L'9') ++i;
        if (i == t.size() || (i + 1 == t.size() && t[i] >= L'A' && t[i] <= L'Z')) return true;
    }
    return false;
}

// "COS_P_WOLF_001~039" -> "COS_P_WOLF"; "" when not a range key (SKILLEFFECT_LOGIC 3.1)
std::wstring RangeBase(const std::wstring& k) {
    const size_t n = k.size();
    if (n < 7 || k[n - 4] != L'~') return {};
    for (size_t i : {n - 7, n - 6, n - 5, n - 3, n - 2, n - 1})
        if (k[i] < L'0' || k[i] > L'9') return {};
    std::wstring b = k.substr(0, n - 7);
    while (!b.empty() && b.back() == L'_') b.pop_back();
    return b;
}

std::wstring Family(const std::wstring& code) { return Join(Tok(code), 2); }

Issue Mk(Sev s, const wchar_t* rule, int listLine, int fileLine, const std::wstring& key, std::wstring tr,
         std::wstring en) {
    Issue i;
    i.sev = s;
    i.rule = rule;
    i.listLine = listLine;
    i.fileLine = fileLine;
    i.key = key;
    i.tr = std::move(tr);
    i.en = std::move(en);
    return i;
}

using LvKey = std::pair<int, int>;

}  // namespace

void KnownCodes::Build(const SeDoc& doc, const RefData& ref, const ParsedList* list) {
    set.clear();
    set.reserve(ref.chars.size() + 2048);
    for (const auto& kv : ref.chars) set.insert(kv.first);
    auto add = [&](const std::wstring& k) {
        set.insert(k);
        const auto b = RangeBase(k);
        if (!b.empty()) set.insert(b);
    };
    for (const auto& r : doc.sim.recs)
        if (!r.header && r.sec == Sec::CI && !r.cells.empty()) add(r.cells[0]);
    if (list)
        for (const auto& r : list->rows)
            if (r.sec == Sec::CI && !r.key.empty()) add(r.key);
}

std::wstring Engine::CreatureOf(Sec s, const std::wstring& key, const KnownCodes& known) {
    if (s == Sec::CI) {
        const auto b = RangeBase(key);
        return b.empty() ? key : b;
    }
    if (StartsWith(key, L"EFFECT_AM_")) {
        const std::wstring k = key.substr(10);
        const auto b = RangeBase(k);
        return b.empty() ? k : b;
    }
    static const std::pair<const wchar_t*, const wchar_t*> pre[] = {
        {L"MSKILL_", L"MOB_"}, {L"PSKILL_", L"COS_"}, {L"CSKILL_", L""}};
    for (const auto& [p, q] : pre) {
        const std::wstring ps(p);
        if (!StartsWith(key, ps)) continue;
        const auto t = Tok(std::wstring(q) + key.substr(ps.size()));
        for (size_t n = t.size(); n >= 2; --n) {
            const auto c = Join(t, n);
            if (known.Has(c)) return c;
        }
        std::vector<std::wstring> f;
        for (const auto& x : t)
            if (!IsSkillToken(x)) f.push_back(x);
        for (size_t n = f.size(); n >= 2; --n) {
            const auto c = Join(f, n);
            if (known.Has(c)) return c;
        }
        return Join(f, f.size());
    }
    return key;
}

int Engine::LabelNumber(Sec s, const std::vector<std::wstring>& cells) {
    const size_t c = s == Sec::SA ? 1 : (s == Sec::SE ? 0 : 99);
    if (c >= cells.size()) return -1;
    const auto& l = cells[c];
    if (l.size() < 4) return -1;
    for (size_t i = 0; i < 3; ++i)
        if (l[i] < L'0' || l[i] > L'9') return -1;
    if (l[3] != L' ' && l[3] != L'_') return -1;
    return (l[0] - L'0') * 100 + (l[1] - L'0') * 10 + (l[2] - L'0');
}

std::vector<ExBlock> Engine::ExistingBlocks(const SeDoc& doc, Sec s, const RefData& ref, const KnownCodes& known) {
    std::vector<ExBlock> v;
    std::wstring prevType;
    for (const auto& r : doc.sim.recs) {
        if (r.header || r.sec != s || r.cells.size() <= static_cast<size_t>(KeyColumn(s))) continue;
        const std::wstring& key = r.Key();
        const std::wstring code = CreatureOf(s, key, known);
        const std::wstring type = (s == Sec::CI && r.cells.size() > 1) ? r.cells[1] : std::wstring();
        bool same = false;
        if (!v.empty()) {
            if (s == Sec::CI)
                same = type == prevType && type != L"PLAYER" && CommonPrefix(Tok(code), Tok(v.back().code)) >= 3;
            else
                same = code == v.back().code;
        }
        const CharRef* cr = ref.Char(s == Sec::CI ? key : code);
        if (same) {
            if (s == Sec::CI && v.back().style == 0) {
                if (StartsWith(code, v.back().code + L"_")) v.back().style = 1;
                else if (StartsWith(v.back().code, code + L"_")) v.back().style = -1;
            }
            v.back().lastLine = r.line;
            if (cr && cr->level >= 0 && (!v.back().hasLevel || LvKey(cr->level, cr->id) > LvKey(v.back().level, v.back().id))) {
                v.back().level = cr->level;
                v.back().id = cr->id;
                v.back().hasLevel = true;
            }
        } else {
            ExBlock b;
            b.code = code;
            b.firstKey = key;
            b.type = type;
            b.firstLine = b.lastLine = r.line;
            if (cr && cr->level >= 0) {
                b.level = cr->level;
                b.id = cr->id;
                b.hasLevel = true;
            } else {
                const int n = LabelNumber(s, r.cells);
                if (n >= 0) {
                    b.level = n;
                    b.hasLevel = true;
                }
            }
            v.push_back(std::move(b));
        }
        prevType = type;
    }
    return v;
}

std::wstring Engine::RegionLabelAt(const SeDoc& doc, int before) {
    for (int i = before - 1, k = 0; i >= 0 && k < 3000; --i, ++k) {
        const auto l = doc.Line(static_cast<size_t>(i));
        size_t a = 0;
        while (a < l.size() && IsClientWs(l[a])) ++a;
        if (StartsWith(l.substr(a), L"#section")) {
            auto t = ClientTokens(l);
            return t.size() > 1 ? L"#section " + t[1] : L"#section";
        }
        if (!StartsWith(l.substr(a), L"//")) continue;
        auto cells = ClientTokens(l.substr(a + 2));
        std::vector<std::wstring> keep;
        for (auto& c : cells)
            if (c != L"-" && !c.empty()) keep.push_back(c);
        if (keep.empty() || keep.size() > 4 || cells.size() > 4) continue;
        std::wstring lab;
        for (auto& c : keep) {
            if (!lab.empty()) lab += L" · ";
            lab += c;
        }
        if (lab.size() > 80) lab = lab.substr(0, 80) + L"…";
        return L"//" + lab;
    }
    return {};
}

Plan Engine::MakePlan(const SeDoc& doc, const ParsedList& list, const RefData& ref, const Options& opt) const {
    Plan plan;
    const int N = static_cast<int>(doc.lines.size());
    plan.deleted.assign(doc.lines.size(), 0);
    KnownCodes known;
    known.Build(doc, ref, &list);

    std::unordered_map<std::wstring, std::vector<int>> exKeys[kSecCount];
    int lastRecLine[kSecCount];
    for (auto& x : lastRecLine) x = -1;
    for (const auto& r : doc.sim.recs) {
        if (r.header) continue;
        exKeys[static_cast<int>(r.sec)][r.Key()].push_back(r.line);
        lastRecLine[static_cast<int>(r.sec)] = r.line;
    }
    std::vector<ExBlock> ex[kSecCount];
    std::vector<std::vector<std::wstring>> exTok[kSecCount];
    for (Sec s : {Sec::CI, Sec::SA, Sec::SE}) {
        ex[static_cast<int>(s)] = ExistingBlocks(doc, s, ref, known);
        for (const auto& b : ex[static_cast<int>(s)]) exTok[static_cast<int>(s)].push_back(Tok(b.code));
    }
    auto validAt = [&](int before) {
        return before >= 0 && before <= N && (before == 0 || doc.sim.blockAfter[static_cast<size_t>(before - 1)] == 0);
    };

    // ---- 1. blocks: consecutive list rows of one unit
    std::wstring lastType;
    for (int i = 0; i < static_cast<int>(list.rows.size()); ++i) {
        const auto& r = list.rows[static_cast<size_t>(i)];
        if (r.sec != Sec::CI && r.sec != Sec::SA && r.sec != Sec::SE) continue;
        const std::wstring code = CreatureOf(r.sec, r.key, known);
        const std::wstring type = (r.sec == Sec::CI && r.cells.size() > 1) ? r.cells[1] : std::wstring();
        bool join = false;
        if (!plan.blocks.empty()) {
            const Block& nb = plan.blocks.back();
            const auto& fr = list.rows[static_cast<size_t>(nb.rows.front())];
            if (nb.sec == r.sec && fr.dir == r.dir && fr.dirKey == r.dirKey) {
                if (r.sec == Sec::CI)
                    join = type == lastType && (code == nb.code || StartsWith(code, nb.code + L"_"));
                else
                    join = code == nb.code;
            }
        }
        if (!join) {
            Block b;
            b.sec = r.sec;
            b.code = code;
            plan.blocks.push_back(std::move(b));
        }
        plan.blocks.back().rows.push_back(i);
        lastType = type;
    }

    // ---- 2. level / id of the new units
    for (auto& b : plan.blocks) {
        for (int ri : b.rows) {
            const auto& r = list.rows[static_cast<size_t>(ri)];
            const CharRef* cr = ref.Char(b.sec == Sec::CI ? r.key : b.code);
            if (cr && cr->level >= 0 && (!b.hasLevel || LvKey(cr->level, cr->id) > LvKey(b.level, b.id))) {
                b.level = cr->level;
                b.id = cr->id;
                b.hasLevel = true;
            }
        }
        if (!b.hasLevel) {
            const int n = LabelNumber(b.sec, list.rows[static_cast<size_t>(b.rows.front())].cells);
            if (n >= 0) {
                b.level = n;
                b.hasLevel = true;
            }
        }
    }

    // ---- 3. keys that already exist (V6)
    // characterInfo / skillaniset2: one row per key. skilleffectset: rows ACCUMULATE per key across the whole file
    // (SKILLEFFECT_LOGIC 3.3), so a key's list rows are compared as a GROUP with the file's rows of that key:
    //   file rows = an ordered subset of the list rows -> the missing list rows are added inside/after the group
    //   list rows = an ordered subset of the file rows -> nothing to add (same rows)
    //   otherwise                                      -> conflict (V6 error, "#replace KEY" replaces the group)
    const bool replaceAll = opt.dup == DupPolicy::Replace;
    // safe mode: a key that is already in the file with OTHER content is replaced in place (identical rows stay)
    const bool safeUpd = opt.mode == PlaceMode::Safe && !replaceAll;
    auto wantsReplace = [&](const std::wstring& k) {
        return replaceAll || list.replaceKeys.count(k) != 0 || list.removeKeys.count(k) != 0;
    };
    // v6 P1: "#remove KEY" + list rows of KEY (the doctor's V9 Reorder fix) while the client's skillaniset2 row of KEY
    // sits BELOW one of its skilleffectset rows: the client's rows go (#remove, below) and the list rows are placed like
    // NEW rows. Replacing in place would keep the skillaniset2 row exactly there (the V9 order stayed wrong and the
    // merge failed its own V9 check). A key whose order is already right keeps the in-place replacement (so applying
    // the same list twice changes nothing).
    auto removedKey = [&](const std::wstring& k) {
        if (!list.removeKeys.count(k)) return false;
        const auto sa = exKeys[static_cast<int>(Sec::SA)].find(k);
        const auto se = exKeys[static_cast<int>(Sec::SE)].find(k);
        if (sa == exKeys[static_cast<int>(Sec::SA)].end() || se == exKeys[static_cast<int>(Sec::SE)].end() || sa->second.empty() ||
            se->second.empty())
            return false;
        return *std::max_element(sa->second.begin(), sa->second.end()) > *std::min_element(se->second.begin(), se->second.end());
    };
    std::unordered_map<int, std::vector<std::wstring>> fileTok;
    auto tokOf = [&](int ln) -> const std::vector<std::wstring>& {
        auto it = fileTok.find(ln);
        if (it == fileTok.end()) it = fileTok.emplace(ln, ClientTokens(doc.Line(static_cast<size_t>(ln)))).first;
        return it->second;
    };
    struct SeGroup {
        std::vector<int> rows;   // list rows of the key (list order)
        std::vector<int> match;  // file line matched by each list row (-1 = not in the file)
        int mode = 0;            // 0 new key, 1 add the missing rows, 2 all there, 3 conflict, 4 replace
        bool autoRepl = false;   // 4 because of the safe mode (rows differed)
    };
    std::unordered_map<std::wstring, SeGroup> seg;
    for (int i = 0; i < static_cast<int>(list.rows.size()); ++i)
        if (list.rows[static_cast<size_t>(i)].sec == Sec::SE) seg[list.rows[static_cast<size_t>(i)].key].rows.push_back(i);
    auto& exSe = exKeys[static_cast<int>(Sec::SE)];
    for (auto& [k, g] : seg) {
        auto it = exSe.find(k);
        if (it == exSe.end()) continue;
        const auto& fl = it->second;
        g.match.assign(g.rows.size(), -1);
        if (removedKey(k)) continue;  // mode 0: placed as new rows (the old rows are removed with #remove)
        if (wantsReplace(k)) {
            g.mode = 4;
            continue;
        }
        size_t f = 0;
        std::vector<int> m(g.rows.size(), -1);
        for (size_t j = 0; j < g.rows.size() && f < fl.size(); ++j)
            if (tokOf(fl[f]) == list.rows[static_cast<size_t>(g.rows[j])].cells) m[j] = fl[f++];
        if (f == fl.size()) {
            g.mode = 1;
            g.match = m;
            continue;
        }
        size_t j = 0;
        std::vector<int> m2(g.rows.size(), -1);
        for (size_t q = 0; q < fl.size() && j < g.rows.size(); ++q)
            if (tokOf(fl[q]) == list.rows[static_cast<size_t>(g.rows[j])].cells) m2[j++] = fl[q];
        if (j == g.rows.size()) {
            g.mode = 2;
            g.match = m2;
            continue;
        }
        if (safeUpd) {
            g.mode = 4;
            g.autoRepl = true;
            continue;
        }
        g.mode = 3;
    }
    // one issue per existing skilleffectset key
    for (const auto& [k, g] : seg) {
        if (g.mode == 4 && g.autoRepl) {
            const auto& fl0 = exSe[k];
            plan.issues.push_back(Mk(Sev::Info, L"V6", list.rows[static_cast<size_t>(g.rows.front())].listLine, fl0.front() + 1, k,
                                     Fmt(L"İstemcide bu efektin %zu satırı farklıydı: listedeki %zu satır onların yerine yazılır (yerinde güncelleme).",
                                         fl0.size(), g.rows.size()),
                                     Fmt(L"The client's %zu rows of this effect differed: the %zu list rows replace them in place.", fl0.size(),
                                         g.rows.size())));
        }
        if (!g.mode || g.mode == 4) continue;
        const auto& fl = exSe[k];
        const auto& fr = list.rows[static_cast<size_t>(g.rows.front())];
        int matched = 0;
        for (int x : g.match) matched += x >= 0;
        const int miss = static_cast<int>(g.rows.size()) - matched;
        if (g.mode == 1 && miss == 0) {
            plan.issues.push_back(Mk(Sev::Info, L"V6", fr.listLine, fl.front() + 1, k,
                                     Fmt(L"Bu efektin %zu satırı istemcide zaten aynen var: eklenmedi, sorun yok.", g.rows.size()),
                                     Fmt(L"The %zu rows of this effect are already in the client: not added, nothing to do.", g.rows.size())));
        } else if (g.mode == 1) {
            plan.issues.push_back(Mk(Sev::Info, L"V6", fr.listLine, fl.front() + 1, k,
                                     Fmt(L"İstemcide bu efektin %zu satırı var; listedeki %d eksik satır onların arasına eklenir (bir anahtarın efekt satırları birikir).",
                                         fl.size(), miss),
                                     Fmt(L"The client has %zu rows of this effect; the %d missing list rows are added among them (effect rows of one key accumulate).",
                                         fl.size(), miss)));
        } else if (g.mode == 2) {
            plan.issues.push_back(Mk(Sev::Info, L"V6", fr.listLine, fl.front() + 1, k,
                                     Fmt(L"Listedeki %zu efekt satırının hepsi istemcide var (istemcide bu anahtarın %zu satırı daha var, dokunulmadı).",
                                         g.rows.size(), fl.size() - g.rows.size()),
                                     Fmt(L"All %zu effect rows of the list are in the client (it has %zu more rows of this key, left alone).",
                                         g.rows.size(), fl.size() - g.rows.size())));
        } else {
            Issue is = Mk(Sev::Error, L"V6", fr.listLine, fl.front() + 1, k,
                          Fmt(L"İstemcide bu efektin %zu satırı var ve listedeki %zu satırla uyuşmuyor: listedeki satırlar eklenmedi.",
                              fl.size(), g.rows.size()),
                          Fmt(L"The client has %zu rows of this effect and they differ from the %zu list rows: the list rows were not added.",
                              fl.size(), g.rows.size()));
            is.fix = Fix::ReplaceKey;
            is.fixArg = k;
            plan.issues.push_back(std::move(is));
            plan.conflictKeys++;
        }
    }
    std::vector<Block> inGroup;  // rows added inside an existing skilleffectset key group
    for (auto& [k, g] : seg) {
        if (g.mode != 1) continue;
        const auto& fl = exSe[k];
        for (size_t j = 0; j < g.rows.size(); ++j) {
            if (g.match[j] >= 0) continue;
            int before = fl.back() + 1;
            for (size_t q = j + 1; q < g.rows.size(); ++q)
                if (g.match[q] >= 0) {
                    before = g.match[q];
                    break;
                }
            if (!inGroup.empty() && inGroup.back().before == before && inGroup.back().anchorCode == k) {
                inGroup.back().rows.push_back(g.rows[j]);
                continue;
            }
            Block nb;
            nb.sec = Sec::SE;
            nb.code = CreatureOf(Sec::SE, k, known);
            nb.rows.push_back(g.rows[j]);
            nb.before = before;
            nb.anchorCode = k;
            const bool after = before == fl.back() + 1;
            nb.whyTr = Fmt(L"%s anahtarının istemcideki satırlarının %s (bir anahtarın efekt satırları birikir; liste sırası korunur).", k.c_str(),
                           after ? L"arkasına" : L"arasına");
            nb.whyEn = Fmt(L"%s the client's rows of %s (rows of one key accumulate; list order kept).", after ? L"After" : L"Among", k.c_str());
            inGroup.push_back(std::move(nb));
        }
    }
    for (auto& b : plan.blocks) {
        std::vector<int> keep;
        std::vector<int> repl;
        for (int ri : b.rows) {
            const auto& r = list.rows[static_cast<size_t>(ri)];
            if (b.sec == Sec::SE) {
                auto gi = seg.find(r.key);
                const int mode = gi == seg.end() ? 0 : gi->second.mode;
                if (mode == 0) {
                    keep.push_back(ri);
                } else if (mode == 1 || mode == 2) {
                    const auto& g = gi->second;
                    const size_t j = static_cast<size_t>(std::find(g.rows.begin(), g.rows.end(), ri) - g.rows.begin());
                    if (j < g.match.size() && g.match[j] >= 0) {
                        plan.skippedRows++;
                        plan.sameRows++;
                    }  // else: carried by an in-group block
                } else if (mode == 3) {
                    plan.skippedRows++;
                } else {
                    keep.push_back(ri);
                    for (int ln : exSe[r.key]) repl.push_back(ln);
                }
                continue;
            }
            auto it = exKeys[static_cast<int>(b.sec)].find(r.key);
            if (it == exKeys[static_cast<int>(b.sec)].end() || removedKey(r.key)) {
                keep.push_back(ri);
                continue;
            }
            if (wantsReplace(r.key)) {
                keep.push_back(ri);
                for (int ln : it->second) repl.push_back(ln);
                continue;
            }
            // the very same row is already there (re-applying an export): nothing to do, not an error
            bool same = false;
            for (int ln : it->second) same = same || tokOf(ln) == r.cells;
            if (safeUpd && !same) {
                keep.push_back(ri);
                for (int ln : it->second) repl.push_back(ln);
                plan.issues.push_back(Mk(Sev::Info, L"V6", r.listLine, it->second.front() + 1, r.key,
                                         Fmt(L"Bu anahtar istemcide farklı içerikle vardı (satır %d): listedeki satır onun yerine yazılır (yerinde güncelleme).",
                                             it->second.front() + 1),
                                         Fmt(L"This key was in the client with other content (line %d): the list row replaces it in place.",
                                             it->second.front() + 1)));
                continue;
            }
            plan.skippedRows++;
            if (same) {
                plan.sameRows++;
                plan.issues.push_back(Mk(Sev::Info, L"V6", r.listLine, it->second.front() + 1, r.key,
                                         Fmt(L"Aynı satır istemcide zaten var (satır %d): eklenmedi, sorun yok.", it->second.front() + 1),
                                         Fmt(L"The same row is already in the client (line %d): not added, nothing to do.", it->second.front() + 1)));
                continue;
            }
            Issue is = Mk(Sev::Error, L"V6", r.listLine, it->second.front() + 1, r.key,
                          Fmt(L"Bu anahtar istemcide farklı içerikle var (satır %d): listedeki satır eklenmedi.", it->second.front() + 1),
                          Fmt(L"This key is in the client with different content (line %d): the list row was not added.", it->second.front() + 1));
            is.fix = Fix::ReplaceKey;
            is.fixArg = r.key;
            plan.issues.push_back(std::move(is));
            plan.conflictKeys++;
        }
        b.rows = keep;
        if (b.rows.empty()) b.skipped = true;
        if (!repl.empty() && !b.skipped) {
            std::sort(repl.begin(), repl.end());
            repl.erase(std::unique(repl.begin(), repl.end()), repl.end());
            for (int ln : repl)
                if (!plan.deleted[static_cast<size_t>(ln)]) {
                    plan.deleted[static_cast<size_t>(ln)] = 1;
                    plan.replacedRows++;
                }
            b.replaceLines = repl;
            b.before = repl.front();
            b.anchorCode = b.code;
            b.whyTr = Fmt(L"Var olan %zu satırın yerine (yerinde güncelleme).", repl.size());
            b.whyEn = Fmt(L"Replaces %zu existing rows in place.", repl.size());
        }
    }
    for (auto& nb : inGroup) plan.blocks.push_back(std::move(nb));
    // "#remove KEY": the client's rows of a stale key go (every section)
    for (const auto& k : list.removeKeys) {
        int n = 0, first = -1;
        for (Sec s : {Sec::CI, Sec::SA, Sec::SE}) {
            auto it = exKeys[static_cast<int>(s)].find(k);
            if (it == exKeys[static_cast<int>(s)].end()) continue;
            for (int ln : it->second)
                if (!plan.deleted[static_cast<size_t>(ln)]) {
                    plan.deleted[static_cast<size_t>(ln)] = 1;
                    plan.removedRows++;
                    ++n;
                    if (first < 0) first = ln;
                }
        }
        if (n)
            plan.issues.push_back(Mk(Sev::Info, L"D1", -1, first + 1, k,
                                     Fmt(L"Bu anahtarın istemcideki %d satırı kaldırılıyor (#remove: hiçbir skile bağlı değil).", n),
                                     Fmt(L"%d client rows of this key are removed (#remove: it belongs to no skill).", n)));
    }

    // ---- 4. placement
    auto sectionEnd = [&](Block& b) {
        const int L = lastRecLine[static_cast<int>(b.sec)];
        if (L < 0) {
            b.before = -1;
            return;
        }
        b.before = L + 1;
        b.newGroup = true;
        b.groupKey = SecName(b.sec);
        b.whyTr = std::wstring(L"Bölüm sonu: ") + SecName(b.sec) + Fmt(L" bölümünün okunan son satırının (%d) arkasına.", L + 1);
        b.whyEn = std::wstring(L"Section end: after the last ACTIVE ") + SecName(b.sec) + Fmt(L" row (line %d).", L + 1);
    };
    // safe mode (default): the user's rule, checked against the client's reader (active, right section)
    int safeAt[kSecCount];
    for (auto& x : safeAt) x = -1;
    if (opt.mode == PlaceMode::Safe)
        for (Sec s : {Sec::CI, Sec::SA, Sec::SE}) safeAt[static_cast<int>(s)] = SafeAnchor(doc, s);
    auto secBefore = [&](int before) {  // the section a row inserted before `before` is read into
        const auto& lk = doc.sim.lineKind;
        for (int i = before - 1; i >= 0; --i) {
            if (static_cast<size_t>(i) >= lk.size() || static_cast<LineKind>(lk[static_cast<size_t>(i)]) != LineKind::Header) continue;
            const auto t = ClientTokens(doc.Line(static_cast<size_t>(i)));
            return t.size() > 1 ? SecFromName(t[1]) : Sec::None;
        }
        return Sec::None;
    };
    auto safePlace = [&](Block& b, int listLine, const std::wstring& key) {
        const int a = safeAt[static_cast<int>(b.sec)];
        if (a >= 0 && validAt(a) && secBefore(a) == b.sec) {
            b.before = a;
            b.newGroup = false;
            if (b.sec == Sec::CI) {
                b.whyTr = Fmt(L"Güvenli yerleşim: '#section skillaniset2' satırının (%d) hemen önü = characterInfo bölümünün sonu.", a + 1);
                b.whyEn = Fmt(L"Safe placement: right before the '#section skillaniset2' line (%d) = end of characterInfo.", a + 1);
            } else if (b.sec == Sec::SA) {
                b.whyTr = Fmt(L"Güvenli yerleşim: '#section skilleffectset' satırının (%d) hemen önü = skillaniset2 bölümünün sonu.", a + 1);
                b.whyEn = Fmt(L"Safe placement: right before the '#section skilleffectset' line (%d) = end of skillaniset2.", a + 1);
            } else if (a >= N) {
                b.whyTr = L"Güvenli yerleşim: skilleffectset başlığından sonra '/*' yok: dosya sonu.";
                b.whyEn = L"Safe placement: no '/*' after the skilleffectset header: end of the file.";
            } else {
                b.whyTr = Fmt(L"Güvenli yerleşim: skilleffectset başlığından sonraki ilk '/*' satırının (%d) hemen önü (yorum bloklarının dışı).", a + 1);
                b.whyEn = Fmt(L"Safe placement: right before the first '/*' line (%d) after the skilleffectset header (outside the comment blocks).", a + 1);
            }
            return;
        }
        sectionEnd(b);
        plan.issues.push_back(Mk(Sev::Warn, L"P2", listLine, b.before, key,
                                 std::wstring(L"Güvenli yerleşim noktası bu dosyada okunan bir ") + SecName(b.sec) +
                                     L" konumu değil (bölüm başlığı / '/*' düzeni farklı): satırlar bölümün son etkin satırının arkasına kondu.",
                                 std::wstring(L"The safe insertion point is not a readable ") + SecName(b.sec) +
                                     L" position in this file (unusual header / '/*' layout): rows placed after the last active row of the section."));
    };
    auto placeInRun = [&](Block& b, const std::vector<int>& run, const std::vector<ExBlock>& E, const std::wstring& what) {
        const std::wstring fam = Family(b.code);
        if (b.hasLevel) {
            // backbone = longest non-decreasing (level,id) subsequence of the run; hand-placed outliers are ignored
            std::vector<int> known;
            for (int j : run)
                if (E[static_cast<size_t>(j)].hasLevel) known.push_back(j);
            std::vector<int> tailIdx, prev(known.size(), -1);
            auto keyOf = [&](int k) { const auto& e = E[static_cast<size_t>(known[static_cast<size_t>(k)])]; return LvKey(e.level, e.id); };
            for (int k = 0; k < static_cast<int>(known.size()); ++k) {
                int lo = 0, hi = static_cast<int>(tailIdx.size());
                while (lo < hi) {
                    const int mid = (lo + hi) / 2;
                    if (keyOf(tailIdx[static_cast<size_t>(mid)]) <= keyOf(k)) lo = mid + 1; else hi = mid;
                }
                if (lo > 0) prev[static_cast<size_t>(k)] = tailIdx[static_cast<size_t>(lo - 1)];
                if (lo == static_cast<int>(tailIdx.size())) tailIdx.push_back(k); else tailIdx[static_cast<size_t>(lo)] = k;
            }
            std::vector<int> spine;
            for (int k = tailIdx.empty() ? -1 : tailIdx.back(); k >= 0; k = prev[static_cast<size_t>(k)]) spine.push_back(known[static_cast<size_t>(k)]);
            std::reverse(spine.begin(), spine.end());
            int last = -1;
            for (int j : spine) {
                const auto& e = E[static_cast<size_t>(j)];
                if (LvKey(e.level, e.id) <= LvKey(b.level, b.id)) last = j;
            }
            if (last >= 0) {
                const auto& e = E[static_cast<size_t>(last)];
                b.before = e.lastLine + 1;
                b.anchorCode = e.code;
                b.whyTr = Fmt(L"%s %s, seviye %d: %s (sv %d) bloğunun hemen arkası.", what.c_str(), fam.c_str(), b.level,
                              e.code.c_str(), e.level);
                b.whyEn = Fmt(L"%s %s, level %d: right after %s (lv %d).", what == L"Bölge" ? L"Region" : L"Relative of",
                              fam.c_str(), b.level, e.code.c_str(), e.level);
            } else {
                const auto& e = E[static_cast<size_t>(spine.empty() ? run.front() : spine.front())];
                b.before = e.firstLine;
                b.anchorCode = e.code;
                b.whyTr = Fmt(L"%s %s, seviye %d (en düşük): %s bloğunun önü.", what.c_str(), fam.c_str(), b.level,
                              e.code.c_str());
                b.whyEn = Fmt(L"%s %s, level %d (lowest): before %s.", what == L"Bölge" ? L"Region" : L"Relative of",
                              fam.c_str(), b.level, e.code.c_str());
            }
        } else {
            const auto& e = E[static_cast<size_t>(run.back())];
            b.before = e.lastLine + 1;
            b.anchorCode = e.code;
            b.whyTr = Fmt(L"%s %s, seviye bilinmiyor: grubun sonuna (%s arkası).", what.c_str(), fam.c_str(), e.code.c_str());
            b.whyEn = Fmt(L"%s %s, level unknown: end of the group (after %s).", what == L"Bölge" ? L"Region" : L"Relative of",
                          fam.c_str(), e.code.c_str());
        }
    };
    auto smart = [&](Block& b) {
        const auto& E = ex[static_cast<int>(b.sec)];
        const auto& ET = exTok[static_cast<int>(b.sec)];
        if (E.empty()) return sectionEnd(b);
        const auto ct = Tok(b.code);
        if (b.sec == Sec::CI) {  // same sound object (ResourceTypeName) = same group: base + _CLON rows
            const auto& fr = list.rows[static_cast<size_t>(b.rows.front())];
            const std::wstring type = fr.cells.size() > 1 ? fr.cells[1] : std::wstring();
            int hit = -1, hitCp = 0;
            for (int j = 0; j < static_cast<int>(E.size()); ++j) {
                const int c2 = CommonPrefix(ct, ET[static_cast<size_t>(j)]);
                if (!type.empty() && type != L"PLAYER" && E[static_cast<size_t>(j)].type == type && c2 >= 3 && c2 > hitCp) {
                    hit = j;
                    hitCp = c2;
                }
            }
            if (hit >= 0) {
                const auto& e = E[static_cast<size_t>(hit)];
                // inside a sound-object group: base first or variant (_CLON) first - learnt from the neighbours
                int style = 0;
                for (int j = 0; j < static_cast<int>(E.size()); ++j)
                    if (CommonPrefix(ct, ET[static_cast<size_t>(j)]) >= 2) style += E[static_cast<size_t>(j)].style;
                const bool isBase = StartsWith(e.code, b.code + L"_");
                const bool first = style >= 0 ? isBase : !isBase;
                b.before = first ? e.firstLine : e.lastLine + 1;
                b.anchorCode = e.code;
                b.whyTr = Fmt(L"Aynı ses nesnesi %s grubu: %s satırlarının %s.", type.c_str(), e.code.c_str(), first ? L"önü (taban kod)" : L"arkası");
                b.whyEn = Fmt(L"Same sound object %s group: %s the %s rows.", type.c_str(), first ? L"before (base code)" : L"after", e.code.c_str());
                return;
            }
        }
        std::vector<int> cp(E.size());
        int best = 0;
        for (size_t j = 0; j < E.size(); ++j) {
            cp[j] = CommonPrefix(ct, ET[j]);
            best = std::max(best, cp[j]);
        }
        auto largestRun = [&](auto pred) {
            std::vector<int> bestRun, cur;
            for (int j = 0; j < static_cast<int>(E.size()); ++j) {
                if (pred(j)) {
                    if (!cur.empty() && cur.back() != j - 1) {
                        if (cur.size() > bestRun.size()) bestRun = cur;
                        cur.clear();
                    }
                    cur.push_back(j);
                }
            }
            if (cur.size() > bestRun.size()) bestRun = cur;
            return bestRun;
        };
        if (best >= 2) {
            // the run of the family (2 tokens, e.g. MOB_CH); with a close relative (>= 3 tokens) the run that holds it
            std::vector<int> run;
            if (best >= 3) {
                std::vector<int> cur;
                size_t bestSize = 0;
                for (int j = 0; j <= static_cast<int>(E.size()); ++j) {
                    const bool in = j < static_cast<int>(E.size()) && cp[static_cast<size_t>(j)] >= 2;
                    if (in) cur.push_back(j);
                    if ((!in || j == static_cast<int>(E.size())) && !cur.empty()) {
                        bool rel = false;
                        for (int x : cur) rel = rel || cp[static_cast<size_t>(x)] == best;
                        if (rel && cur.size() > bestSize) {
                            run = cur;
                            bestSize = cur.size();
                        }
                        cur.clear();
                    }
                }
            }
            if (run.empty()) run = largestRun([&](int j) { return cp[static_cast<size_t>(j)] >= 2; });
            placeInRun(b, run, E, best >= 3 ? L"Akraba" : L"Bölge");
            return;
        }
        if (!ct.empty()) {
            // new family: a new group between the existing regional groups of the same kind (MOB_, SKILL_ ...),
            // after the region whose highest level is the closest one not above the new unit's level
            std::map<std::wstring, std::vector<int>> famRun;
            {
                std::map<std::wstring, std::vector<int>> cur;
                for (int j = 0; j < static_cast<int>(E.size()); ++j) {
                    const auto& t = ET[static_cast<size_t>(j)];
                    if (t.size() < 2 || t[0] != ct[0]) continue;
                    const std::wstring f = t[0] + L"_" + t[1];
                    auto& c = cur[f];
                    if (!c.empty() && c.back() != j - 1) {
                        if (c.size() > famRun[f].size()) famRun[f] = c;
                        c.clear();
                    }
                    c.push_back(j);
                }
                for (auto& [f, c] : cur)
                    if (c.size() > famRun[f].size()) famRun[f] = c;
            }
            const std::vector<int>* pick = nullptr;
            int pickLv = -1;
            bool before = false;
            for (const auto& [f, run] : famRun) {
                if (run.size() < 3) continue;
                int mx = -1;
                for (int j : run)
                    if (E[static_cast<size_t>(j)].hasLevel) mx = std::max(mx, E[static_cast<size_t>(j)].level);
                if (mx < 0) continue;
                const bool fits = !b.hasLevel || mx <= b.level;
                if (fits && (!pick || before || mx > pickLv || (mx == pickLv && run.back() > pick->back()))) {
                    pick = &run;
                    pickLv = mx;
                    before = false;
                } else if (!fits && (!pick || (before && mx < pickLv))) {
                    pick = &run;
                    pickLv = mx;
                    before = true;
                }
            }
            if (pick) {
                const auto& e = E[static_cast<size_t>(before ? pick->front() : pick->back())];
                const std::wstring other = Family(e.code);
                b.before = before ? E[static_cast<size_t>(pick->front())].firstLine : e.lastLine + 1;
                b.anchorCode = e.code;
                b.newGroup = true;
                b.groupKey = Family(b.code);
                const std::wstring lv = b.hasLevel ? Fmt(L"%d", b.level) : std::wstring(L"?");
                b.whyTr = Fmt(L"Yeni grup %s (sv %s): dosyada bu aile yok; %s bölgesinin (en yüksek sv %d) %s.", b.groupKey.c_str(),
                              lv.c_str(), other.c_str(), pickLv, before ? L"önüne" : L"arkasına");
                b.whyEn = Fmt(L"New group %s (lv %s): family not in the file; %s the %s region (max lv %d).", b.groupKey.c_str(),
                              lv.c_str(), before ? L"before" : L"after", other.c_str(), pickLv);
                return;
            }
            auto run = largestRun([&](int j) { return !ET[static_cast<size_t>(j)].empty() && ET[static_cast<size_t>(j)][0] == ct[0]; });
            if (!run.empty()) {
                const auto& e = E[static_cast<size_t>(run.back())];
                b.before = e.lastLine + 1;
                b.anchorCode = e.code;
                b.newGroup = true;
                b.groupKey = Family(b.code);
                b.whyTr = Fmt(L"Yeni grup %s: dosyada bu aile yok; %s_ türünün ana grubunun sonuna (%s arkası).",
                              b.groupKey.c_str(), ct[0].c_str(), e.code.c_str());
                b.whyEn = Fmt(L"New group %s: family not in the file; at the end of the main %s_ run (after %s).",
                              b.groupKey.c_str(), ct[0].c_str(), e.code.c_str());
                return;
            }
        }
        sectionEnd(b);
    };

    struct SaInfo {
        std::wstring prev, next, groupKey;
        bool newGroup = false;
    };
    std::unordered_map<std::wstring, SaInfo> saInfo;
    std::vector<size_t> order;
    for (Sec s : {Sec::CI, Sec::SA, Sec::SE})
        for (size_t i = 0; i < plan.blocks.size(); ++i)
            if (plan.blocks[i].sec == s) order.push_back(i);

    for (size_t oi : order) {
        Block& b = plan.blocks[oi];
        if (b.skipped) continue;
        const auto& fr = list.rows[static_cast<size_t>(b.rows.front())];
        const auto& E = ex[static_cast<int>(b.sec)];
        if (b.before < 0) {
            bool done = false;
            if (fr.dir == Dir::After || fr.dir == Dir::Before) {
                std::vector<int> lines;
                auto it = exKeys[static_cast<int>(b.sec)].find(fr.dirKey);
                if (it != exKeys[static_cast<int>(b.sec)].end()) lines = it->second;
                for (const auto& e : E)
                    if (e.code == fr.dirKey) {
                        lines.push_back(e.firstLine);
                        lines.push_back(e.lastLine);
                    }
                if (!lines.empty()) {
                    const auto [mn, mx] = std::minmax_element(lines.begin(), lines.end());
                    b.before = fr.dir == Dir::After ? *mx + 1 : *mn;
                    b.anchorCode = fr.dirKey;
                    b.whyTr = Fmt(L"Liste komutu #%s %s.", fr.dir == Dir::After ? L"after" : L"before", fr.dirKey.c_str());
                    b.whyEn = b.whyTr;
                    done = true;
                } else {
                    plan.issues.push_back(Mk(Sev::Warn, L"P1", fr.listLine, -1, fr.dirKey,
                                             std::wstring(L"#after/#before anahtarı ") + SecName(b.sec) +
                                                 L" bölümünde bulunamadı; otomatik yerleşim kullanıldı.",
                                             std::wstring(L"#after/#before key not found in ") + SecName(b.sec) +
                                                 L"; automatic placement used."));
                }
            }
            if (!done) {
                if (opt.mode == PlaceMode::SectionEnd || fr.dir == Dir::End) {
                    sectionEnd(b);
                } else if (opt.mode == PlaceMode::Safe) {
                    safePlace(b, fr.listLine, fr.key);
                } else if (b.sec == Sec::SE && saInfo.count(b.code)) {
                    const auto& si = saInfo[b.code];
                    const auto& code = si.prev.empty() ? si.next : si.prev;
                    int pick = -1;
                    for (int j = 0; j < static_cast<int>(E.size()); ++j)
                        if (E[static_cast<size_t>(j)].code == code) {
                            if (!si.prev.empty()) pick = j;            // last block of the predecessor
                            else if (pick < 0) pick = j;               // first block of the successor
                        }
                    if (!code.empty() && pick >= 0) {
                        const auto& e = E[static_cast<size_t>(pick)];
                        b.before = si.prev.empty() ? e.firstLine : e.lastLine + 1;
                        b.anchorCode = e.code;
                        b.newGroup = si.newGroup;
                        b.groupKey = si.groupKey;
                        b.whyTr = Fmt(L"skillaniset2 sırasını izler: %s bloğunun %s.", e.code.c_str(),
                                      si.prev.empty() ? L"önü" : L"arkası");
                        b.whyEn = Fmt(L"Follows the skillaniset2 order: %s %s.", si.prev.empty() ? L"before" : L"after",
                                      e.code.c_str());
                    } else {
                        smart(b);
                    }
                } else {
                    smart(b);
                }
            }
        }
        if (!validAt(b.before)) {
            const std::wstring wt = b.whyTr;
            sectionEnd(b);
            if (!validAt(b.before)) {
                b.skipped = true;
                plan.issues.push_back(Mk(Sev::Error, L"V2", fr.listLine, -1, fr.key,
                                         std::wstring(L"Güvenli ekleme noktası yok: ") + SecName(b.sec) +
                                             L" bölümünün son etkin satırı bir /* bloğu açıyor. Dosyayı elle düzeltin.",
                                         std::wstring(L"No safe insertion point: the last active ") + SecName(b.sec) +
                                             L" row opens a /* block. Fix the file by hand."));
                continue;
            }
            plan.issues.push_back(Mk(Sev::Warn, L"V2", fr.listLine, b.before, fr.key,
                                     L"Hesaplanan yer bir /* */ bloğunun içindeydi; bölüm sonuna alındı. (" + wt + L")",
                                     L"Computed position was inside a /* */ block; moved to the section end."));
        }
        b.regionLabel = RegionLabelAt(doc, b.before);
        if (b.sec == Sec::SA) {
            SaInfo si;
            for (const auto& e : E) {
                if (e.lastLine + 1 == b.before) si.prev = e.code;
                if (e.firstLine == b.before) si.next = e.code;
            }
            si.newGroup = b.newGroup;
            si.groupKey = b.groupKey;
            saInfo[b.code] = si;
        }
    }

    // ---- 5. order: section, position, level (unknown last, stable = list order); safe mode: list order only
    const bool listOrder = opt.mode == PlaceMode::Safe;
    std::stable_sort(plan.blocks.begin(), plan.blocks.end(), [listOrder](const Block& a, const Block& b) {
        auto k = [listOrder](const Block& x) {
            return std::make_tuple(x.skipped ? 1 : 0, static_cast<int>(x.sec), x.before, !listOrder && x.hasLevel ? x.level : INT_MAX,
                                   !listOrder && x.hasLevel ? x.id : INT_MAX);
        };
        return k(a) < k(b);
    });
    for (size_t i = 0; i < plan.blocks.size(); ++i) {
        auto& b = plan.blocks[i];
        if (b.skipped) continue;
        plan.newRows += static_cast<int>(b.rows.size());
        if (opt.mode == PlaceMode::Safe) b.newGroup = false;  // safe mode writes no '//' header / blank line
        if (!b.newGroup) continue;
        const bool first = i == 0 || plan.blocks[i - 1].skipped || plan.blocks[i - 1].before != b.before ||
                           !plan.blocks[i - 1].newGroup || plan.blocks[i - 1].groupKey != b.groupKey;
        b.header = first;
    }
    return plan;
}

const wchar_t* PlaceModeId(PlaceMode m) {
    return m == PlaceMode::Smart ? L"smart" : m == PlaceMode::SectionEnd ? L"end" : L"safe";
}

bool PlaceModeFromId(const std::wstring& s, PlaceMode& out) {
    if (s == L"safe" || s == L"guvenli" || s == L"güvenli" || s == L"user") out = PlaceMode::Safe;
    else if (s == L"smart" || s == L"akilli" || s == L"akıllı") out = PlaceMode::Smart;
    else if (s == L"end" || s == L"sectionend") out = PlaceMode::SectionEnd;
    else return false;
    return true;
}

// The user's rule (TESTA inject_skilleffect.py + the working in-game file), on LINE STARTS only:
//   CI -> the first active line that starts with "#section<TAB>skillaniset2"
//   SA -> the first active line that starts with "#section<TAB>skilleffectset"
//   SE -> the first line after that header whose text (after leading client blanks) starts with "/*", or the next
//         '#section' line, whichever comes first; the end of the file when there is none
int Engine::SafeAnchor(const SeDoc& doc, Sec s) {
    const auto& lk = doc.sim.lineKind;
    const size_t n = doc.lines.size();
    auto isHeader = [&](size_t i) { return i < lk.size() && static_cast<LineKind>(lk[i]) == LineKind::Header; };
    auto headerAt = [&](const wchar_t* name) {
        const std::wstring pre = std::wstring(L"#section\t") + name;
        for (size_t i = 0; i < n; ++i) {
            if (!isHeader(i)) continue;
            const auto l = doc.Line(i);
            if (!StartsWith(l, pre) || (l.size() > pre.size() && !IsClientWs(l[pre.size()]))) continue;
            return static_cast<int>(i);
        }
        return -1;
    };
    if (s == Sec::CI) return headerAt(L"skillaniset2");
    if (s == Sec::SA) return headerAt(L"skilleffectset");
    if (s != Sec::SE) return -1;
    const int h = headerAt(L"skilleffectset");
    if (h < 0) return -1;
    for (size_t i = static_cast<size_t>(h) + 1; i < n; ++i) {
        if (isHeader(i)) return static_cast<int>(i);
        const auto l = doc.Line(i);
        size_t a = 0;
        while (a < l.size() && IsClientWs(l[a])) ++a;
        if (l.size() >= a + 2 && l[a] == L'/' && l[a + 1] == L'*') return static_cast<int>(i);
    }
    return static_cast<int>(n);
}

MergeResult Engine::Merge(const SeDoc& doc, const ParsedList& list, const Plan& plan, const Options& opt) const {
    MergeResult m;
    const size_t N = doc.lines.size();
    std::vector<std::vector<int>> ins(N + 1);
    for (size_t i = 0; i < plan.blocks.size(); ++i) {
        const auto& b = plan.blocks[i];
        if (!b.skipped && b.before >= 0 && static_cast<size_t>(b.before) <= N) ins[static_cast<size_t>(b.before)].push_back(static_cast<int>(i));
    }
    const std::wstring term = doc.TermStr(doc.mainTerm);
    m.outOfOrig.assign(N, -1);
    std::wstring& body = m.body;
    body.reserve(doc.text.size() + 4096);
    auto pad = [&](std::wstring s, Sec sec) {
        if (!opt.padTabs) return s;
        const int want = doc.padTabs[static_cast<size_t>(sec)];
        int have = static_cast<int>(std::count(s.begin(), s.end(), L'\t'));
        while (have < want) {
            s += L'\t';
            ++have;
        }
        return s;
    };
    auto emit = [&](const std::wstring& s, OutLine ol) {
        if (!body.empty() && body.back() != L'\n' && body.back() != L'\r') body += term;  // last line had no terminator
        body += s;
        body += term;
        m.map.push_back(ol);
    };
    for (size_t i = 0; i <= N; ++i) {
        for (int bi : ins[i]) {
            const auto& b = plan.blocks[static_cast<size_t>(bi)];
            if (b.header) {
                OutLine h;
                h.block = bi;
                h.header = true;
                emit(pad(L"", b.sec), h);
                emit(pad(L"//" + b.groupKey + L" - " + opt.header, b.sec), h);
            }
            for (size_t k = 0; k < b.rows.size(); ++k) {
                const auto& r = list.rows[static_cast<size_t>(b.rows[k])];
                if (k > 0 && opt.mode != PlaceMode::Safe)
                    for (const auto& p : r.pre) {
                        OutLine pl;
                        pl.block = bi;
                        pl.header = true;
                        emit(p, pl);
                    }
                OutLine ol;
                ol.block = bi;
                ol.listRow = b.rows[k];
                emit(pad(r.text, b.sec), ol);
            }
        }
        if (i == N) break;
        if (!plan.deleted.empty() && plan.deleted[i]) continue;
        m.outOfOrig[i] = static_cast<int>(m.map.size());
        OutLine ol;
        ol.origLine = static_cast<int>(i);
        m.map.push_back(ol);
        body.append(doc.Line(i));
        body += doc.TermStr(doc.lines[i].term);
    }
    m.bytes = doc.Encode(body);

    // ---- verification with the client model
    const auto outLines = SplitPhysical(body);
    const SimResult sim = Simulate(body, outLines);
    bool ok = outLines.size() == m.map.size();
    if (!ok)
        m.issues.push_back(Mk(Sev::Error, L"X1", -1, -1, L"", L"İç hata: satır eşlemesi tutmadı.", L"Internal: line map mismatch."));
    std::unordered_map<std::wstring, int> firstSa, firstSe;
    for (size_t ri = 0; ri < sim.recs.size(); ++ri) {
        const auto& r = sim.recs[ri];
        if (r.header) continue;
        if (r.sec == Sec::SA && !firstSa.count(r.Key())) firstSa[r.Key()] = static_cast<int>(ri);
        if (r.sec == Sec::SE && !firstSe.count(r.Key())) firstSe[r.Key()] = static_cast<int>(ri);
    }
    for (size_t li = 0; ok && li < m.map.size(); ++li) {
        const auto& ol = m.map[li];
        if (ol.listRow < 0) {
            if (ol.block >= 0 && sim.recOfLine[li] >= 0) {
                ok = false;
                m.issues.push_back(Mk(Sev::Error, L"X2", -1, static_cast<int>(li) + 1, L"",
                                      L"Eklenen yorum/başlık satırı veri olarak okunuyor.", L"An inserted comment/header line is read as data."));
            }
            continue;
        }
        const auto& row = list.rows[static_cast<size_t>(ol.listRow)];
        const auto& b = plan.blocks[static_cast<size_t>(ol.block)];
        m.newExpected++;
        const int ri = sim.recOfLine[li];
        if (ri < 0 || sim.recs[static_cast<size_t>(ri)].sec != b.sec || sim.recs[static_cast<size_t>(ri)].cells != row.cells) {
            ok = false;
            m.issues.push_back(Mk(Sev::Error, L"V2", row.listLine, static_cast<int>(li) + 1, row.key,
                                  std::wstring(L"Yeni satırı istemci ") + SecName(b.sec) + L" kaydı olarak OKUMUYOR (yorum içinde/bölüm dışı).",
                                  std::wstring(L"The client does NOT read the new row as a ") + SecName(b.sec) + L" record (comment/out of section)."));
        } else {
            m.newActive++;
        }
    }
    // original records unchanged and in the same order
    {
        std::vector<const Rec*> a, c;
        for (const auto& r : doc.sim.recs)
            if (plan.deleted.empty() || !plan.deleted[static_cast<size_t>(r.line)]) a.push_back(&r);
        for (const auto& r : sim.recs)
            if (static_cast<size_t>(r.line) < m.map.size() && m.map[static_cast<size_t>(r.line)].origLine >= 0) c.push_back(&r);
        bool same = a.size() == c.size();
        for (size_t i = 0; same && i < a.size(); ++i)
            same = a[i]->sec == c[i]->sec && a[i]->header == c[i]->header && a[i]->cells == c[i]->cells;
        if (!same) {
            ok = false;
            m.issues.push_back(Mk(Sev::Error, L"X3", -1, -1, L"", L"Var olan kayıtlar değişti (sıra/içerik) - yazılmaz.",
                                  L"Existing records changed (order/content) - not written."));
        }
        if (sim.eofInBlock != doc.sim.eofInBlock) {
            ok = false;
            m.issues.push_back(Mk(Sev::Error, L"X4", -1, -1, L"", L"Dosya sonu yorum durumu değişti.", L"EOF comment state changed."));
        }
    }
    // V9: skillaniset2 row of a key before its skilleffectset rows
    for (const auto& b : plan.blocks) {
        if (b.skipped || b.sec != Sec::SE) continue;
        for (int ri : b.rows) {
            const auto& key = list.rows[static_cast<size_t>(ri)].key;
            auto sa = firstSa.find(key);
            auto sev = firstSe.find(key);
            if (sa != firstSa.end() && sev != firstSe.end() && sa->second > sev->second) {
                ok = false;
                m.issues.push_back(Mk(Sev::Error, L"V9", list.rows[static_cast<size_t>(ri)].listLine, -1, key,
                                      L"skilleffectset satırı skillaniset2 satırından ÖNCE okunuyor (assert g_sSkillEffMapStr).",
                                      L"skilleffectset row is read BEFORE its skillaniset2 row (assert g_sSkillEffMapStr)."));
                break;
            }
        }
    }
    m.verified = ok;
    if (ok)
        m.issues.push_back(Mk(Sev::Info, L"OK", -1, -1, L"",
                              Fmt(L"Yerleşim denetlendi: %d/%d yeni satır istemcinin okuyacağı yerde; var olan %d kayıt aynen korunuyor.",
                                  m.newActive, m.newExpected, static_cast<int>(doc.sim.recs.size())),
                              Fmt(L"Placement checked: %d/%d new rows are where the client reads them; the %d existing records are unchanged.",
                                  m.newActive, m.newExpected, static_cast<int>(doc.sim.recs.size()))));
    return m;
}

}  // namespace se
