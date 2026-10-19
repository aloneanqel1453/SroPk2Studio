// SkillEffect Studio - core/Ops.h
// Unit extraction/removal (tests, "export a creature to a list") and the leave-one-out self test.
#pragma once

#include "Engine.h"

#include <string>
#include <vector>

namespace se {

struct UnitLines {
    std::vector<int> lines[kSecCount];  // physical lines (rows + interior comment lines), ascending
    int rows = 0;
};

// rows of one creature: characterInfo key == code, skillaniset2/skilleffectset keys whose creature == code
UnitLines FindUnit(const SeDoc& doc, const KnownCodes& known, const std::wstring& code);
// list text (#section headers + verbatim lines), UTF-16 body with the file's line end
std::wstring UnitListText(const SeDoc& doc, const std::vector<UnitLines>& units);
// body of the file without the given lines (byte-exact otherwise)
std::wstring BodyWithout(const SeDoc& doc, const std::vector<std::uint8_t>& del);

struct SelfTestResult {
    int units = 0, exactAll = 0;
    int tried[kSecCount]{}, exact[kSecCount]{};
    std::vector<std::wstring> exactCodes;  // fully identical
    std::vector<std::wstring> failCodes;
};
SelfTestResult LeaveOneOut(const SeDoc& doc, const RefData& ref, const std::wstring& prefix, int limit,
                           const Options& opt);

}  // namespace se
