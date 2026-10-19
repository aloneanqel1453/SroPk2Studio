// SkillEffect Studio - core/Source.h
// Opens skilleffect.txt from a file, a Media folder or a Media.pk2 (read-only) and loads the
// reference data next to it (characterdata = codenames/levels, skilldata = skill codes/groups).
#pragma once

#include "Text.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace se {

struct CharRef {
    int id = 0;
    int level = -1;
    int rarity = 0;
    bool service = true;
    std::wstring bsr;          // AssocFileObj128 (column 52, relative to res\)
    std::wstring nameStrId;    // NameStrID128 (column 5)
};

struct SkillRef {
    int id = 0;
    int original = 0;          // Basic_Original (chain step -> head skill id)
    std::wstring group;        // Basic_Group ("xxx" = none)
    std::wstring name;         // Basic_Name (label used by skillaniset2 / skilleffectset)
};

class RefData {
public:
    std::unordered_map<std::wstring, CharRef> chars;
    std::unordered_set<std::wstring> skillCodes, skillGroups;
    std::unordered_map<std::wstring, std::wstring> groupOfCode;  // Basic_Code -> Basic_Group
    std::unordered_map<std::wstring, SkillRef> skills;           // Basic_Code -> row facts
    std::unordered_map<int, std::wstring> skillById, charById;   // ID -> code (collision checks)
    std::unordered_map<int, std::wstring> ownerOfSkill;          // DefaultSkill_1..10 (columns 83..92) -> creature code
    std::wstring desc;  // where it came from
    bool HasChars() const { return !chars.empty(); }
    bool HasSkills() const { return !skillCodes.empty(); }
    void Clear();
    void AddCharacterData(const std::wstring& text);
    void AddSkillData(const std::wstring& text);
    const CharRef* Char(const std::wstring& code) const;
};

// name is relative to the textdata folder ("characterdata.txt")
using TextdataGetter = std::function<bool(const std::wstring& name, Bytes& out)>;
// returns a short log line (counts / what was missing)
std::wstring LoadRefData(RefData& rd, const TextdataGetter& get);

struct SourceInfo {
    enum class Kind { File, Folder, Pk2 } kind = Kind::File;
    std::wstring path;     // what the user opened
    std::wstring filePath; // skilleffect.txt on disk (File/Folder) or the pk2 path
    std::wstring inner;    // path inside the pk2
    std::wstring textdataDir;
    std::wstring refLog;
};

constexpr const wchar_t* kTextdataInner = L"server_dep\\silkroad\\textdata\\";
constexpr const char* kDefaultPk2Key = "169841";

// Opens a source; ref is filled when characterdata/skilldata are found next to it.
bool OpenSource(const std::wstring& path, const std::string& pk2Key, Bytes& data, SourceInfo& info, RefData& ref,
                std::wstring& err);
// Reference data only (folder, textdata folder, pk2 or one plain characterdata/skilldata file); ADDS to ref
bool OpenRefOnly(const std::wstring& path, const std::string& pk2Key, RefData& ref, std::wstring& err);

}  // namespace se
