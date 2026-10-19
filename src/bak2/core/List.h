// SkillEffect Studio - core/List.h
// The user's list: rows of characterInfo / skillaniset2 / skilleffectset (any mix), optionally with
// "#section <name>" headers and placement directives, or a TeraStudio export "_merge" folder.
#pragma once

#include "SeModel.h"

#include <set>
#include <string>
#include <vector>

namespace se {

enum class Sev { Error = 0, Warn = 1, Info = 2 };

// what the "Düzelt" (fix) action of an issue does
enum class Fix : int {
    None = 0,
    Pool,        // add the missing rows (skilldata / characterdata / skillaniset2 ...) from the export package
    GenSa,       // generate the minimal skillaniset2 row from the skilldata row (template of shipped monsters)
    Pad,         // pad / trim the cells of a NEW row (column defaults)
    PickExport,  // nothing to add from: ask for the TeraStudio export folder
    Repair,      // explicit "mevcut dosyayı onar": a repaired COPY of the existing file (never default)
    ReplaceKey,  // V6 / T2: the key exists with other content -> "#replace KEY" (the list's rows replace the client's)
    DropRows,    // stale key (no skill anywhere): the list rows are commented out (+ "#remove KEY" when the file has it)
    Rekey,       // stale key whose current skill exists (KEY -> KEY_4): the key cell is renamed (fixArg2)
};

struct Issue {
    Sev sev = Sev::Info;
    std::wstring rule;   // V1..V18, L.., P..
    int listLine = -1;   // 1-based line in the list (-1 = none)
    int fileLine = -1;   // 1-based physical line in skilleffect.txt (-1 = none)
    std::wstring key;
    std::wstring tr, en;
    std::wstring table;           // other text table (skilldata ...) the issue is about; empty = skilleffect.txt
    std::wstring howTr, howEn;    // "Nasıl düzelir" (filled by Explain)
    Fix fix = Fix::None;
    std::wstring fixArg, fixArg2;
    bool autoOk = true;           // part of "Otomatik düzelt" (false: only by its own button)
    const std::wstring& Text(bool english) const { return english ? en : tr; }
    const std::wstring& How(bool english) const { return english ? howEn : howTr; }
};

// a row of another client table carried by the list ("#table skilldata" ...)
struct TableRow {
    std::wstring table;  // table id (skilldata, characterdata, itemdata, textdata_object, ...)
    std::wstring text;   // verbatim row (TABs kept)
    int listLine = 0;    // 1-based
};

enum class Dir { Auto = 0, After = 1, Before = 2, End = 3 };

struct ListRow {
    Sec sec = Sec::None;
    std::wstring text;                // verbatim row text (no line terminator)
    std::vector<std::wstring> cells;  // client tokens
    std::wstring key;
    int listLine = 0;                 // 1-based
    std::vector<std::wstring> pre;    // safe comment/blank lines directly above (kept only inside a block)
    bool guessed = false;             // section derived from the row shape
    Dir dir = Dir::Auto;
    std::wstring dirKey;
};

struct ParsedList {
    std::vector<ListRow> rows;
    std::vector<TableRow> tables;
    std::vector<Issue> issues;
    std::wstring desc;
    std::set<std::wstring> replaceKeys;  // "#replace KEY": the list's rows of KEY replace the client's (skilleffect + tables)
    std::set<std::wstring> removeKeys;   // "#remove KEY": the client's skilleffect rows of KEY are removed
};

// Parses list text (any line ending). Rows without a #section header are classified by shape.
ParsedList ParseListText(const std::wstring& text, const std::wstring& desc);

// Loads a list from a file, a TeraStudio "_merge" folder (or its merge_plan.json / an export root)
// and returns canonical list text (with #section headers) for the editor.
bool LoadListPath(const std::wstring& path, std::wstring& listText, std::wstring& desc, std::wstring& err);

// guess the section of a row from its tokens (None when unknown)
Sec GuessSection(const std::vector<std::wstring>& tok);

}  // namespace se
