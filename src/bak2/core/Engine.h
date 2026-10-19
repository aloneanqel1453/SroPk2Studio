// SkillEffect Studio - core/Engine.h
// Placement (where every new row goes and why), validation (SKILLEFFECT_LOGIC.md section 8),
// merge (byte-exact splice) and post-merge verification with the client model.
#pragma once

#include "List.h"
#include "SeModel.h"
#include "Source.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace se {

// Safe (default, "Güvenli (önerilen)") = the placement of the user's working in-game file (TESTA, 2026-10-11):
//   characterInfo rows right before the line that starts with "#section<TAB>skillaniset2",
//   skillaniset2 rows right before the line that starts with "#section<TAB>skilleffectset",
//   skilleffectset rows right before the FIRST line starting with "/*" after that header (EOF when there is none);
//   line starts only, new rows keep the list order, no header / blank lines are written, a key that already exists
//   is replaced in place (identical rows stay as they are).
// Smart (family / level) and SectionEnd (after the last active row) are optional modes.
enum class PlaceMode { Smart = 0, SectionEnd = 1, Safe = 2 };
enum class DupPolicy { Skip = 0, Replace = 1 };
const wchar_t* PlaceModeId(PlaceMode m);           // "safe" / "smart" / "end"
bool PlaceModeFromId(const std::wstring& s, PlaceMode& out);

struct Options {
    PlaceMode mode = PlaceMode::Safe;
    DupPolicy dup = DupPolicy::Skip;
    bool padTabs = true;                          // pad new rows with trailing TABs like the file
    std::wstring header = L"SkillEffectStudio";   // text of the '//' header written above new groups
};

// codenames known to the client: characterdata + characterInfo keys (file + list) + range bases
struct KnownCodes {
    std::unordered_set<std::wstring> set;
    void Build(const SeDoc& doc, const RefData& ref, const ParsedList* list);
    bool Has(const std::wstring& c) const { return set.count(c) != 0; }
};

// one contiguous group of existing rows of one creature/unit in a section
struct ExBlock {
    std::wstring code;      // creature/unit code
    std::wstring firstKey;  // key of the first row
    std::wstring type;      // characterInfo ResourceTypeName (sound object) of the group
    int style = 0;          // characterInfo group: +1 base row first, -1 variant (_CLON) row first
    int firstLine = 0, lastLine = 0;  // physical lines (0-based)
    int level = -1, id = 0x7fffffff;
    bool hasLevel = false;
};

struct Block {
    Sec sec = Sec::None;
    std::wstring code;
    std::vector<int> rows;               // list row indexes
    int before = -1;                     // insert before this physical line of the original (lines.size() = EOF)
    bool newGroup = false;               // gets a '//' header (new family or section-end mode)
    bool header = false;                 // this block writes the header
    std::wstring groupKey;               // family used for the header
    std::wstring regionLabel;            // nearest group comment above the insertion point
    std::wstring whyTr, whyEn;           // reason
    std::wstring anchorCode;             // existing unit it was placed next to
    bool skipped = false;
    std::vector<int> replaceLines;       // replace policy: original lines removed
    int level = -1, id = 0x7fffffff;
    bool hasLevel = false;
    int prevCode = -1;                   // internal (SA follow)
};

struct Plan {
    std::vector<Block> blocks;           // sorted by (section, before, level)
    std::vector<Issue> issues;
    std::vector<std::uint8_t> deleted;   // per original line
    int newRows = 0, skippedRows = 0, replacedRows = 0, sameRows = 0, removedRows = 0;
    int conflictKeys = 0;                // V6 errors: keys whose rows differ from the client
};

struct OutLine {
    int origLine = -1;  // >= 0: unchanged original line
    int block = -1;     // >= 0: new line of plan.blocks[block]
    int listRow = -1;   // >= 0: the list row it carries
    bool header = false;
};

struct MergeResult {
    std::wstring body;
    Bytes bytes;
    std::vector<OutLine> map;         // per output physical line
    std::vector<int> outOfOrig;       // original line -> output line (-1 deleted)
    std::vector<Issue> issues;        // verification results
    bool verified = false;
    int newActive = 0, newExpected = 0;
};

class Engine {
public:
    Plan MakePlan(const SeDoc& doc, const ParsedList& list, const RefData& ref, const Options& opt) const;
    MergeResult Merge(const SeDoc& doc, const ParsedList& list, const Plan& plan, const Options& opt) const;
    std::vector<Issue> Validate(const SeDoc& doc, const ParsedList& list, const RefData& ref, const Plan& plan) const;
    // whole-file health (BOM, line ends, EOF inside /* */, malformed shipped rows)
    std::vector<Issue> FileHealth(const SeDoc& doc) const;
    // rows the client cannot see (doctor): keys whose rows are ALL inside /* */ (V2), active characterInfo rows whose
    // code is not in the client's characterdata (V5, the client drops them silently); the counts go to `counts`
    struct Unseen {
        int hiddenKeys = 0, hiddenKeysEof = 0, ciDropped = 0, ciNear = 0;  // ciNear: code differs from a client code only in "_N"
    };
    std::vector<Issue> Visibility(const SeDoc& doc, const RefData& ref, Unseen* counts = nullptr) const;
    // Safe mode anchors (0-based line, -1 = none): where new rows of `s` go
    static int SafeAnchor(const SeDoc& doc, Sec s);

    static std::wstring CreatureOf(Sec s, const std::wstring& key, const KnownCodes& known);
    static std::vector<ExBlock> ExistingBlocks(const SeDoc& doc, Sec s, const RefData& ref, const KnownCodes& known);
    static std::wstring RegionLabelAt(const SeDoc& doc, int before);
    static int LabelNumber(Sec s, const std::vector<std::wstring>& cells);
};

}  // namespace se
