// SkillEffect Studio v6 P5 (M28) - skillplay/SideFx.h
// "Yan etki": which skills a change touches that it did not mean to. Before / after = two GoldenSource views (the
// client, the client + output folder or + pending rows). Every skill ID whose golden line (key, Service, rows,
// timeline digest) changes is either on a TARGET key (the keys the change is for: expected) or not (rule Y1, warning,
// impact "Yanlış görünür" / Looks wrong). characterInfo rows changed outside the targets are side effects of creatures;
// a characterInfo RANGE row (PREFIX###~###) covering a code that has its own explicit row is reported on its own: the
// client registers PREFIX%03d for every n of the range in the same map, so whichever row comes later wins.
#pragma once

#include "Golden.h"

#include "../core/List.h"

#include <set>
#include <string>
#include <vector>

namespace se::sp {

struct SideEffect {
    int id = 0;                    // skill ID
    std::wstring code;             // Basic_Code
    std::wstring keyBefore, keyAfter;
    std::wstring what;             // "key", "service", "rows", "timeline" (CompareGolden words)
};

struct RangeCover {
    std::wstring rangeKey;         // PREFIXlo~hi
    std::vector<std::wstring> explicitCodes;  // PREFIX%03d with an explicit characterInfo row of their own
};

struct SideFxReport {
    std::vector<std::wstring> changedKeys;    // presentation keys whose rows / sounds changed
    std::vector<SideEffect> side;             // Y1: changed skills whose key (before and after) is not a target
    int targetSkills = 0;                     // changed skills on a target key (expected)
    int addedSkills = 0, removedSkills = 0;   // skill IDs only on one side (skilldata changed)
    std::vector<std::wstring> ciSide;         // characterInfo keys changed outside the targets
    std::vector<RangeCover> ranges;           // range rows covering explicit codes
    int checkedSkills = 0;
    double seconds = 0;
    bool Clean() const { return side.empty() && ciSide.empty() && ranges.empty(); }
    std::wstring Text(bool en, size_t max = 30) const;
};

// targetKeys: presentation / characterInfo keys the change is meant for (compared case-insensitively);
// ciChanged: characterInfo keys the change adds / edits / removes (ChangedCharInfoKeys / ListCharInfoKeys)
SideFxReport SideEffects(const GoldenSource& before, const GoldenSource& after, const std::set<std::wstring>& targetKeys,
                         const std::vector<std::wstring>& ciChanged = {});

// characterInfo keys whose active rows differ between two skilleffect.txt texts (client comment rules, SeDoc)
std::vector<std::wstring> ChangedCharInfoKeys(const Bytes& seBefore, const Bytes& seAfter);
// characterInfo keys of a list (rows of the characterInfo section, #replace / #remove keys)
std::vector<std::wstring> ListCharInfoKeys(const std::wstring& listText);
// every presentation key (skillaniset2 / skilleffectset / characterInfo) a list touches: the natural target set
std::set<std::wstring> ListKeys(const std::wstring& listText);
// presentation keys whose active SA / SE rows differ between two skilleffect.txt texts
std::set<std::wstring> ChangedSeKeys(const Bytes& seBefore, const Bytes& seAfter);

// Y1 issues for the merge / transfer issue list (Sev::Warn, rule "Y1", one per side-effect key and one per range row)
std::vector<Issue> SideFxIssues(const SideFxReport& r);

}  // namespace se::sp
