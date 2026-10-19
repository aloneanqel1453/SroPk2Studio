// SkillEffect Studio v6 P5 (M28) - skillplay/SideFx.cpp (see SideFx.h)
#include "SideFx.h"

#include "../core/SeModel.h"

#include <algorithm>
#include <chrono>
#include <map>

namespace se::sp {
namespace {

std::wstring Up(const std::wstring& s) { return ToUpper(s); }

std::wstring Joined(const std::vector<std::wstring>& c) {
    std::wstring s;
    for (const auto& x : c) s += x + L'\t';
    return s;
}

// active records of one section: key -> every row (file order) joined
std::map<std::wstring, std::wstring> SectionRows(const Bytes& raw, Sec sec) {
    std::map<std::wstring, std::wstring> out;
    SeDoc d;
    std::wstring err;
    if (!d.Load(raw, err)) return out;
    for (const auto& r : d.sim.recs) {
        if (r.header || r.sec != sec) continue;
        out[r.Key()] += Joined(r.cells) + L'\n';
    }
    return out;
}

void DiffKeys(const std::map<std::wstring, std::wstring>& a, const std::map<std::wstring, std::wstring>& b,
              std::set<std::wstring>& out) {
    for (const auto& [k, v] : a) {
        auto it = b.find(k);
        if (it == b.end() || it->second != v) out.insert(k);
    }
    for (const auto& [k, v] : b)
        if (!a.count(k)) out.insert(k);
}

bool IsRangeKey(const std::wstring& k, std::wstring& prefix, long long& lo, long long& hi) {
    if (k.size() < 8 || k[k.size() - 4] != L'~') return false;
    prefix = k.substr(0, k.size() - 7);
    return ParseInt(std::wstring_view(k).substr(prefix.size(), 3), lo) && ParseInt(std::wstring_view(k).substr(k.size() - 3), hi) &&
           lo <= hi;
}

bool Explicit(const FxDb& db, const std::wstring& code) {
    const CharInfoRow* r = db.CharInfo(code);
    return r && r->key == code;
}

}  // namespace

std::vector<std::wstring> ChangedCharInfoKeys(const Bytes& seBefore, const Bytes& seAfter) {
    std::set<std::wstring> s;
    DiffKeys(SectionRows(seBefore, Sec::CI), SectionRows(seAfter, Sec::CI), s);
    return {s.begin(), s.end()};
}

std::set<std::wstring> ChangedSeKeys(const Bytes& seBefore, const Bytes& seAfter) {
    std::set<std::wstring> s;
    DiffKeys(SectionRows(seBefore, Sec::SA), SectionRows(seAfter, Sec::SA), s);
    DiffKeys(SectionRows(seBefore, Sec::SE), SectionRows(seAfter, Sec::SE), s);
    return s;
}

std::vector<std::wstring> ListCharInfoKeys(const std::wstring& listText) {
    const ParsedList pl = ParseListText(listText, L"sidefx");
    std::set<std::wstring> s;
    for (const auto& r : pl.rows)
        if (r.sec == Sec::CI && !r.key.empty()) s.insert(r.key);
    return {s.begin(), s.end()};
}

std::set<std::wstring> ListKeys(const std::wstring& listText) {
    const ParsedList pl = ParseListText(listText, L"sidefx");
    std::set<std::wstring> s;
    for (const auto& r : pl.rows)
        if (!r.key.empty()) s.insert(r.key);
    for (const auto& k : pl.replaceKeys) s.insert(k);
    for (const auto& k : pl.removeKeys) s.insert(k);
    return s;
}

SideFxReport SideEffects(const GoldenSource& before, const GoldenSource& after, const std::set<std::wstring>& targetKeys,
                         const std::vector<std::wstring>& ciChanged) {
    const auto t0 = std::chrono::steady_clock::now();
    SideFxReport r;
    std::set<std::wstring> targets;
    for (const auto& k : targetKeys) targets.insert(Up(k));
    auto isTarget = [&](const std::wstring& k) { return !k.empty() && targets.count(Up(k)) > 0; };

    // 1. keys whose effective rows / timeline changed
    std::set<std::wstring> keys;
    for (const auto& k : before.db.Keys()) keys.insert(k);
    for (const auto& k : after.db.Keys()) keys.insert(k);
    std::set<std::wstring> changed;
    for (const auto& k : keys)
        if (KeyDigest(before.db, k) != KeyDigest(after.db, k)) changed.insert(k);
    r.changedKeys.assign(changed.begin(), changed.end());

    // 2. skills: a key that resolves differently or a changed key -> compare the golden lines
    for (const auto& [id, i] : after.byId) {
        const SkillInfo& sa = after.skills[i];
        const SkillInfo* sb = before.ById(id);
        if (!sb) {
            ++r.addedSkills;
            continue;
        }
        ++r.checkedSkills;
        const std::wstring kb = before.db.PresentationKey(*sb, before.Lookup());
        const std::wstring ka = after.db.PresentationKey(sa, after.Lookup());
        if (kb == ka && !changed.count(ka)) continue;
        const GoldenLine gb = GoldenOf(before, *sb), ga = GoldenOf(after, sa);
        if (gb.SameAs(ga)) continue;
        if (isTarget(kb) || isTarget(ka)) {
            ++r.targetSkills;
            continue;
        }
        GoldenCompare c = CompareGolden({gb}, {ga});
        r.side.push_back({id, sa.code, kb, ka, c.diffs.empty() ? std::wstring() : c.diffs[0].what});
    }
    for (const auto& [id, i] : before.byId) {
        (void)i;
        if (!after.byId.count(id)) ++r.removedSkills;
    }

    // 3. characterInfo: changed rows outside the targets; range rows over explicit codes
    for (const auto& k : ciChanged) {
        std::wstring prefix;
        long long lo = 0, hi = 0;
        if (IsRangeKey(k, prefix, lo, hi)) {
            RangeCover rc;
            rc.rangeKey = k;
            for (long long n = lo; n <= hi && n <= 999; ++n) {
                const std::wstring code = prefix + Fmt(L"%03lld", n);
                if (Explicit(after.db, code) || Explicit(before.db, code)) rc.explicitCodes.push_back(code);
            }
            if (!rc.explicitCodes.empty()) r.ranges.push_back(std::move(rc));
        }
        if (!isTarget(k)) r.ciSide.push_back(k);
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

std::wstring SideFxReport::Text(bool en, size_t max) const {
    std::wstring s = Fmt(en ? L"side effects: Y1=%zu skills (keys changed %zu, target skills %d, checked %d), characterInfo outside targets %zu, range rows over explicit codes %zu, skills added %d removed %d (%.1f s)\r\n"
                            : L"yan etki: Y1=%zu skill (değişen anahtar %zu, hedef skill %d, denetlenen %d), hedef dışı characterInfo %zu, açık kodu kapsayan aralık satırı %zu, eklenen skill %d silinen %d (%.1f sn)\r\n",
                         side.size(), changedKeys.size(), targetSkills, checkedSkills, ciSide.size(), ranges.size(), addedSkills,
                         removedSkills, seconds);
    size_t n = 0;
    for (const auto& e : side) {
        if (n++ >= max) {
            s += Fmt(en ? L"  ... %zu more\r\n" : L"  ... %zu tane daha\r\n", side.size() - max);
            break;
        }
        s += Fmt(L"  Y1 %d %ls [%ls%ls%ls] %ls\r\n", e.id, e.code.c_str(), e.keyBefore.c_str(), e.keyBefore == e.keyAfter ? L"" : L" -> ",
                 e.keyBefore == e.keyAfter ? L"" : e.keyAfter.c_str(), e.what.c_str());
    }
    for (const auto& k : ciSide) s += L"  CI " + k + L"\r\n";
    for (const auto& rc : ranges) {
        s += L"  RANGE " + rc.rangeKey + L" ->";
        for (size_t i = 0; i < rc.explicitCodes.size() && i < 12; ++i) s += L" " + rc.explicitCodes[i];
        if (rc.explicitCodes.size() > 12) s += Fmt(L" (+%zu)", rc.explicitCodes.size() - 12);
        s += L"\r\n";
    }
    return s;
}

std::vector<Issue> SideFxIssues(const SideFxReport& r) {
    std::vector<Issue> out;
    std::map<std::wstring, std::vector<const SideEffect*>> byKey;
    for (const auto& e : r.side) byKey[e.keyAfter.empty() ? e.keyBefore : e.keyAfter].push_back(&e);
    for (const auto& [key, v] : byKey) {
        Issue i;
        i.sev = Sev::Warn;
        i.rule = L"Y1";
        i.key = key;
        std::wstring ids;
        for (size_t k = 0; k < v.size() && k < 8; ++k) ids += (k ? L", " : L"") + v[k]->code;
        if (v.size() > 8) ids += Fmt(L" (+%zu)", v.size() - 8);
        i.tr = Fmt(L"Bu değişiklik hedeflenmeyen %zu skilin görünüşünü de değiştiriyor (anahtar %ls): %ls", v.size(), key.c_str(), ids.c_str());
        i.en = Fmt(L"This change also changes how %zu skills it does not target look (key %ls): %ls", v.size(), key.c_str(), ids.c_str());
        i.howTr = L"Bu skilleri değiştirmek istemiyorsanız satırları kendi yeni anahtarınıza taşıyın ya da bu anahtarı değiştiren satırı kaldırın.";
        i.howEn = L"If you do not mean to change these skills, move the rows to a key of your own or drop the row that changes this key.";
        i.autoOk = false;
        out.push_back(std::move(i));
    }
    for (const auto& k : r.ciSide) {
        Issue i;
        i.sev = Sev::Warn;
        i.rule = L"Y1";
        i.key = k;
        i.tr = L"Hedeflenmeyen bir canavarın / karakterin görünüş satırı (characterInfo) değişiyor: " + k;
        i.en = L"The look row (characterInfo) of a creature that is not a target changes: " + k;
        i.howTr = L"Bu canavarı değiştirmek istemiyorsanız satırı listeden çıkarın.";
        i.howEn = L"If you do not mean to change this creature, drop the row from the list.";
        i.autoOk = false;
        out.push_back(std::move(i));
    }
    for (const auto& rc : r.ranges) {
        Issue i;
        i.sev = Sev::Warn;
        i.rule = L"Y1";
        i.key = rc.rangeKey;
        std::wstring codes;
        for (size_t k = 0; k < rc.explicitCodes.size() && k < 8; ++k) codes += (k ? L", " : L"") + rc.explicitCodes[k];
        if (rc.explicitCodes.size() > 8) codes += Fmt(L" (+%zu)", rc.explicitCodes.size() - 8);
        i.tr = L"Aralık satırı " + rc.rangeKey + L", kendi satırı olan kodları da kapsıyor: " + codes +
               L". İstemci aralıktaki her kodu ayrı kaydeder; dosyada sonra gelen satır öncekini ezer.";
        i.en = L"Range row " + rc.rangeKey + L" also covers codes that have a row of their own: " + codes +
               L". The client registers every code of the range; the later row in the file wins.";
        i.howTr = L"Aralığı daraltın ya da bu kodları aralığın dışında bırakın.";
        i.howEn = L"Narrow the range or keep these codes out of it.";
        i.autoOk = false;
        out.push_back(std::move(i));
    }
    return out;
}

}  // namespace se::sp
