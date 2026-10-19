// SkillEffect Studio - core/Ops.cpp
#include "Ops.h"

#include <algorithm>
#include <unordered_set>

namespace se {

UnitLines FindUnit(const SeDoc& doc, const KnownCodes& known, const std::wstring& code) {
    UnitLines u;
    for (const auto& r : doc.sim.recs) {
        if (r.header || r.cells.size() <= static_cast<size_t>(KeyColumn(r.sec))) continue;
        bool hit = false;
        if (r.sec == Sec::CI) hit = r.cells[0] == code;
        else if (r.sec == Sec::SA || r.sec == Sec::SE) hit = Engine::CreatureOf(r.sec, r.Key(), known) == code;
        if (hit) {
            u.lines[static_cast<int>(r.sec)].push_back(r.line);
            u.rows++;
        }
    }
    for (auto& v : u.lines) {
        if (v.size() < 2) continue;
        std::vector<int> full;
        for (size_t i = 0; i + 1 < v.size(); ++i) {
            full.push_back(v[i]);
            bool interior = v[i + 1] - v[i] > 1;
            for (int k = v[i] + 1; interior && k < v[i + 1]; ++k) {
                const auto l = doc.Line(static_cast<size_t>(k));
                if (doc.sim.recOfLine[static_cast<size_t>(k)] >= 0 || l.find(L"/*") != std::wstring_view::npos ||
                    l.find(L"*/") != std::wstring_view::npos)
                    interior = false;
            }
            if (interior)
                for (int k = v[i] + 1; k < v[i + 1]; ++k) full.push_back(k);
        }
        full.push_back(v.back());
        v = std::move(full);
    }
    return u;
}

std::wstring UnitListText(const SeDoc& doc, const std::vector<UnitLines>& units) {
    const std::wstring term = doc.TermStr(doc.mainTerm);
    std::wstring t;
    for (Sec s : {Sec::CI, Sec::SA, Sec::SE}) {
        bool any = false;
        for (const auto& u : units) any = any || !u.lines[static_cast<int>(s)].empty();
        if (!any) continue;
        t += std::wstring(L"#section ") + SecName(s) + term;
        for (const auto& u : units)
            for (int ln : u.lines[static_cast<int>(s)]) {
                t.append(doc.Line(static_cast<size_t>(ln)));
                t += term;
            }
    }
    return t;
}

std::wstring BodyWithout(const SeDoc& doc, const std::vector<std::uint8_t>& del) {
    std::wstring b;
    b.reserve(doc.text.size());
    for (size_t i = 0; i < doc.lines.size(); ++i) {
        if (i < del.size() && del[i]) continue;
        b.append(doc.Line(i));
        b += doc.TermStr(doc.lines[i].term);
    }
    return b;
}

SelfTestResult LeaveOneOut(const SeDoc& doc, const RefData& ref, const std::wstring& prefix, int limit,
                           const Options& opt) {
    SelfTestResult res;
    KnownCodes known;
    known.Build(doc, ref, nullptr);
    std::vector<Sec> secOfLine(doc.lines.size(), Sec::None);
    {
        Sec cur = Sec::None;
        size_t ri = 0;
        for (size_t i = 0; i < doc.lines.size(); ++i) {
            while (ri < doc.sim.recs.size() && doc.sim.recs[ri].line <= static_cast<int>(i)) {
                if (doc.sim.recs[ri].header) cur = doc.sim.recs[ri].sec;
                ++ri;
            }
            secOfLine[i] = cur;
        }
    }
    std::vector<std::wstring> codes;
    std::unordered_set<std::wstring> seen;
    for (const auto& r : doc.sim.recs)
        if (!r.header && r.sec == Sec::CI && !r.cells.empty() && StartsWith(r.cells[0], prefix) && seen.insert(r.cells[0]).second)
            codes.push_back(r.cells[0]);
    Engine eng;
    for (const auto& code : codes) {
        if (limit > 0 && res.units >= limit) break;
        const UnitLines u = FindUnit(doc, known, code);
        if (!u.rows) continue;
        res.units++;
        std::vector<std::uint8_t> del(doc.lines.size(), 0);
        for (const auto& v : u.lines)
            for (int ln : v) del[static_cast<size_t>(ln)] = 1;
        SeDoc d2;
        std::wstring err;
        if (!d2.Load(doc.Encode(BodyWithout(doc, del)), err)) continue;
        const ParsedList pl = ParseListText(UnitListText(doc, {u}), L"selftest");
        const Plan plan = eng.MakePlan(d2, pl, ref, opt);
        const MergeResult m = eng.Merge(d2, pl, plan, opt);
        const bool all = m.body == doc.text;
        bool secOk[kSecCount];
        for (auto& x : secOk) x = true;
        const auto outLines = SplitPhysical(m.body);
        if (outLines.size() != doc.lines.size()) {
            for (auto& x : secOk) x = false;
        } else {
            for (size_t i = 0; i < outLines.size(); ++i) {
                const std::wstring_view a(m.body.data() + outLines[i].off, outLines[i].len);
                if (a != doc.Line(i) || outLines[i].term != doc.lines[i].term) secOk[static_cast<int>(secOfLine[i])] = false;
            }
        }
        for (Sec s : {Sec::CI, Sec::SA, Sec::SE}) {
            if (u.lines[static_cast<int>(s)].empty()) continue;
            res.tried[static_cast<int>(s)]++;
            if (secOk[static_cast<int>(s)]) res.exact[static_cast<int>(s)]++;
        }
        if (all) {
            res.exactAll++;
            res.exactCodes.push_back(code);
        } else {
            std::wstring note = code + L":";
            for (Sec s : {Sec::CI, Sec::SA, Sec::SE})
                if (!u.lines[static_cast<int>(s)].empty() && !secOk[static_cast<int>(s)]) {
                    note += std::wstring(L" ") + SecName(s) + Fmt(L"@%d", u.lines[static_cast<int>(s)].front() + 1);
                    for (const auto& b : plan.blocks)
                        if (b.sec == s) note += Fmt(L"->%d[%s]", b.before + 1, b.whyEn.substr(0, 70).c_str());
                }
            res.failCodes.push_back(note);
        }
    }
    return res;
}

}  // namespace se
