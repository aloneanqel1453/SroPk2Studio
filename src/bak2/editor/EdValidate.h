// SkillEffect Studio - editor/EdValidate.h
// Per-cell / per-row / cross-table checks of the editor. Cell rules come from the column type (EdSchema); row rules
// are the client traps (SKILLEFFECT_LOGIC R3-R7, V1-V18): empty cells shift columns, '/' in a cell starts a comment,
// a new row inside /* */ is never read, duplicate keys / IDs, ID outside the file's 5000-bucket; references use a
// RefIndex built from the open tables and the client archives (resource files exist?).
#pragma once

#include "EdDoc.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace se::ed {

enum class Sev : int { Info = 0, Warn = 1, Error = 2 };

struct CellIssue {
    int doc = -1;
    std::uint64_t uid = 0;    // row (0 = file level)
    int line = -1;            // physical line (-1 = new row / file level)
    int col = -1;             // -1 = row level
    Sev sev = Sev::Warn;
    std::wstring code;        // "TYPE", "ENUM", "V4", "REF" ...
    std::wstring msgTr, msgEn;
};

// what other tables / archives say (filled from the workspace + client; every part optional)
struct RefIndex {
    std::unordered_set<std::wstring> charCodes, itemCodes, skillCodes, skillGroups, nameStrIds, soundObjs, ciKeys,
        saKeys;
    std::unordered_map<std::wstring, std::wstring> groupOfCode;  // Basic_Code -> Basic_Group
    std::unordered_map<long long, std::wstring> objById, skillById;
    bool hasChars = false, hasItems = false, hasSkills = false, hasNames = false, hasSounds = false, hasSe = false;
    // resource lookup: arc (0 Media, 1 Data, 2 Particles) + inner path -> 1 present / 0 missing / -1 cannot tell
    std::function<int(int arc, const std::wstring& inner)> exists;
    bool checkFiles = true;
    void Clear();
    // characterdata / itemdata / skilldata / name tables / effectsound / skilleffect of the workspace
    void AddDoc(const EdDoc& d);
};
// opens what is missing (index-listed split files, name tables, effectsound, skilleffect) and fills the index
void BuildRefIndex(Workspace& ws, RefIndex& ref, std::wstring* log = nullptr);
// client archive lookup for RefIndex::exists (cached pk2 directory reads)
std::function<int(int, const std::wstring&)> ClientExists(const ClientBase& c);

// cell value check against its column (no cross-table lookups unless ref given)
void CheckCell(const ColDef& c, const std::wstring& v, const std::vector<std::wstring>& row, const RefIndex* ref,
               std::vector<CellIssue>& out);
// one row (cells + row traps + references); doc used for the bucket / block checks
void CheckRow(const EdDoc& d, int docId, const EdRow& r, const RefIndex* ref, std::vector<CellIssue>& out);
// whole document (rows + duplicate keys / IDs + file traps). onlyChanged: new / edited rows only
std::vector<CellIssue> CheckDoc(const EdDoc& d, int docId, const RefIndex* ref, bool onlyChanged = false);
// the resolved archive path of a Path cell ("" when none / not a path)
std::wstring ResolvePath(const ColDef& c, const std::vector<std::wstring>& row, const std::wstring& v, int* arcOut);

const wchar_t* SevName(Sev s, bool en);

}  // namespace se::ed
