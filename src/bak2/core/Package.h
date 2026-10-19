// SkillEffect Studio - core/Package.h
// "Paketi aç": a TeraStudio export folder (Client\Media textdata copies + _merge fragments + Client\Data /
// Particles resources + Server\SQL), a _merge folder, or any list file. Everything it carries becomes one list
// text: "#section" blocks for skilleffect.txt and "#table <id>" blocks for the other client tables.
#pragma once

#include "Client.h"
#include "List.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace se {

// how one client text table is merged
struct TableSpec {
    std::wstring id;            // "#table" name: skilldata, characterdata, itemdata, textdata_object ...
    std::wstring file;          // shared table file, or the index file of a bucket table
    std::wstring pattern;       // bucket file name when the index has no example (%lld = bucket)
    bool bucket = false;        // index file + bucket files of 5000 IDs (bucket N holds N-5000 <= ID < N)
    bool encrypted = false;     // SkillData_*ENC.txt
    bool cp949 = false;         // effectsound.txt
    bool known = false;
    int idCol = -1, codeCol = -1, cells = 0;
    std::vector<int> keyCols;   // shared tables: duplicate key (raw columns); empty = the whole row
};
TableSpec SpecFor(const std::wstring& idOrFile);
// table id of a textdata file name ("SkillData_120000ENC.txt" -> skilldata); empty for skilleffect.txt
std::wstring TableIdOfFile(const std::wstring& file);
long long BucketOf(long long id);  // (id / 5000 + 1) * 5000

struct Package {
    std::wstring path, root, desc;
    std::wstring listText;                                            // canonical list (#section + #table)
    std::unordered_map<std::wstring, std::wstring> files[kArcCount];  // lower inner path -> absolute file
    std::vector<std::wstring> sql;                                    // Server\SQL\*.sql (never run by the tool)
    std::vector<std::wstring> notes;                                  // what was read, from where
    int seRows = 0;
    std::map<std::wstring, int> tableRows;
    ParsedList parsed;                                                // parsed listText (fix lookups)
    std::wstring textdataDir;                                         // the package's textdata copies (column layout reference)
    bool transfer = false;                                            // a SkillEffect Studio transfer package (aktarim_listesi.txt)
    std::map<std::wstring, std::wstring> skillClip;                   // _report\skills.txt: skill code -> animation clip name
    std::set<long long> ownIds;                                       // _report\ids.json: client IDs this export created

    bool Empty() const { return parsed.rows.empty() && parsed.tables.empty(); }
    bool HasFile(Arc a, const std::wstring& inner) const;
    std::wstring FilePath(Arc a, const std::wstring& inner) const;
    int FileCount() const;
};

// base (optional) is used to keep only the rows a full table copy adds to the client
bool LoadPackage(const std::wstring& path, const ClientBase* base, Package& pkg, std::wstring& err);

std::wstring LowerPath(std::wstring s);  // lower case, '/' -> '\'

}  // namespace se
