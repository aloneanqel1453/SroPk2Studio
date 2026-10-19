// SkillEffect Studio - editor/EdTemplates.h
// Row templates (one undo step each): "yeni beceri efekti" (skillaniset2 + skilleffectset rows of a key copied under
// a new key, + its effectsound rows), "yeni canavar" (characterdata row with a free ID + name row + characterInfo
// row copied from an existing creature), copy row (unique key / ID), blank row (column defaults). Find / replace
// across every open table (plain, whole cell, case, regex, one column).
#pragma once

#include "EdDoc.h"

#include <string>
#include <vector>

namespace se::ed {

struct RowRef {
    int doc = -1;
    std::uint64_t uid = 0;
};

struct TemplateResult {
    bool ok = false;
    std::vector<RowRef> rows;      // created rows
    std::wstring msgTr, msgEn;     // what was done / why not
};

// next free ID: characterdata + itemdata share one ID space (_RefObjCommon), skilldata has its own
long long NextFreeId(const Workspace& ws, TKind k);
// a key not used in the doc ("BASE_COPY", "BASE_COPY2" ...)
std::wstring UniqueKey(const EdDoc& d, TKind k, const std::wstring& base);

enum class Place { AfterSource = 0, SectionEnd = 1 };

// skillaniset2 + skilleffectset (+ effectsound skill_ID rows when effectsound.txt is open or can be opened)
TemplateResult NewSkillEffect(Workspace& ws, const std::wstring& srcKey, const std::wstring& newKey,
                              const std::wstring& label = L"", Place place = Place::AfterSource, bool withSounds = true);
// characterdata + textdata_object name + skilleffect characterInfo (+ copies the creature's skills when asked)
struct NewMonsterOpts {
    std::wstring nameKor, nameEng;   // display names (empty = copied)
    long long id = 0;                // 0 = NextFreeId
    bool withCharInfo = true;
    bool withName = true;
};
TemplateResult NewMonster(Workspace& ws, const std::wstring& srcCode, const std::wstring& newCode, const NewMonsterOpts& o = {});
// duplicate after the source with a unique key / ID
TemplateResult CopyRow(Workspace& ws, int doc, std::uint64_t uid);
// row of column defaults (at the end of the section / file)
TemplateResult BlankRow(Workspace& ws, int doc, TKind k);

// ---------------------------------------------------------------- find / replace
struct FindOpts {
    std::wstring text;
    bool matchCase = false;
    bool wholeCell = false;
    bool regex = false;
    std::wstring column;        // column name (empty = every column)
    std::vector<int> docs;      // doc ids (empty = every open doc)
    TKind kind = TKind::Unknown;  // row kind filter (a skilleffect section)
    size_t maxHits = 200000;
};
struct Hit {
    int doc = -1;
    std::uint64_t uid = 0;
    int line = -1;
    int col = -1;
    std::wstring cell;
};
std::vector<Hit> FindAll(const Workspace& ws, const FindOpts& o, std::wstring* err = nullptr);
// replaces in every hit (or only in `only`), one undo step; returns the number of cells changed
int ReplaceAll(Workspace& ws, const FindOpts& o, const std::wstring& repl, const std::vector<Hit>* only = nullptr,
               std::wstring* err = nullptr);

}  // namespace se::ed
