// SkillEffect Studio - editor/EdSchema.h
// Named + typed columns of every client text table the editor touches (SPEC.md 4.4-4.9, SKILLEFFECT_LOGIC.md 5/8,
// SRO_SKILLS.md 2). characterdata / itemdata / skilldata column lists are generated from the read-only
// SRO_VT_SHARD schema dump (gen\gen_columns.ps1 -> EdColumnsGen.inc); the skilleffect sections, name tables,
// effectsound and index files are written by hand. Unknown tables get an inferred schema (int / real / text).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace se::ed {

enum class SqlT : std::uint8_t { I32, U8, I16, I64, Real, Str };
struct ColGen {
    const wchar_t* name;
    SqlT t;
    short maxLen;
};

// cell type
enum class CT : std::uint8_t {
    Any,        // free text (still checked for client traps: '//', '/*', control chars)
    Int,        // integer in [lo, hi]
    Real,       // number (SQL real: fmt_real form)
    Str,        // text, maxLen (0 = no limit); "xxx" = none
    Code,       // codename: A-Z 0-9 _ (no spaces); "xxx" allowed when !Required
    Label,      // designer label (any language, no TAB)
    BoolTF,     // TRUE / FALSE (case-sensitive; true/false also read)
    Enum,       // one of enumv (case-sensitive)
    EnumPipe,   // enum tokens joined by '|'
    Comma,      // commaN comma-separated numbers
    MovSpeed,   // MOV_x,a,b,c
    ActOption,  // bool,a,b,c,d,bool
    Script,     // none | SCT_*(|SCT_*)(,n...)
    AniName,    // ANI_* or none
    AniList,    // ANI_* or a comma list of ANI_*
    Path,       // file path under arc + prefix, or none / xxx
    Folder,     // folder ending in '\' or none
    Bone,       // bone name, none or *
    Env,        // characterInfo env: n or 'flags,file.ogg'
    SeKey,      // characterInfo key: code or PREFIX###~###
    Empty,      // must stay empty (trailing column)
};

enum ColFlag : std::uint32_t {
    CF_Key = 1,          // main key of the table (unique)
    CF_Id = 2,           // numeric ID (unique, bucket range)
    CF_NameStr = 4,      // a textdata name StrID (SN_...)
    CF_SkillIdRef = 8,   // skill ID (0 = none)
    CF_CharCode = 16,    // a characterdata codename
    CF_SkillKey = 32,    // a skill presentation key (Basic_Group / Basic_Code)
    CF_SoundObj = 64,    // effectsound object (characterInfo col 1)
    CF_Required = 128,   // never empty / none
    CF_Readonly = 256,   // shown, not edited in the grid (index lines)
    CF_Label = 512,      // row label (skilleffect col 1 / 0)
};

enum class Arc4 : int { None = -1, Media = 0, Data = 1, Particles = 2, Music = 3 };

struct ColDef {
    std::wstring name;
    CT type = CT::Any;
    long long lo = 0, hi = 0;  // Int range (lo > hi = no range)
    int maxLen = 0;
    int commaN = 0;
    const std::wstring_view* enumv = nullptr;
    size_t enumN = 0;
    std::uint32_t flags = 0;
    Arc4 arc = Arc4::None;     // Path / Folder root archive
    std::wstring prefix;       // root inside the archive ("res\\", "icon\\", "prim\\snd\\" ...)
    std::wstring ext;          // expected extension(s) "bsr|efp" (empty = any); a bare name gets the first one
    int dirCol = -1;           // Path: the folder comes from this column (skilleffectset ObjPath, effectsound folder)
    std::wstring def;          // default for a blank row
    std::wstring tipTr, tipEn;
    std::vector<std::wstring> Choices() const;  // enum values (Enum / BoolTF), empty otherwise
};

enum class TKind : int {
    Unknown = 0,
    SeFile,         // skilleffect.txt as a whole (rows carry their section kind)
    SeCharInfo,     // #section characterInfo
    SeAniSet,       // #section skillaniset2
    SeEffectSet,    // #section skilleffectset
    CharacterData,  // characterdata_N.txt
    ItemData,       // itemdata_N.txt
    SkillData,      // skilldata_N(ENC).txt
    NameTable,      // textdata_object / textdata_equip&skill / textquest_* / textuisystem / textzonename
    EffectSound,    // effectsound.txt (CP949)
    IndexFile,      // characterdata.txt / itemdata.txt / skilldataenc.txt / textdataname.txt ...
    NpcPos,         // npcpos.txt
    Generic,        // anything else (inferred columns)
    Count
};

struct Schema {
    TKind kind = TKind::Unknown;
    std::wstring nameTr, nameEn;
    std::vector<ColDef> cols;
    int keyCol = -1;          // CF_Key column
    int idCol = -1;           // CF_Id column
    int labelCol = -1;        // CF_Label column
    int clientCells = 0;      // expected non-empty client tokens (V4: CI 13, SA 27, SE 28), 0 = not fixed
    bool keyUnique = true;    // skilleffectset: several rows per key
    bool trailingTab = false; // rows end with TAB (name tables, skilleffect)
    bool cp949 = false;       // effectsound
    const ColDef* Col(size_t i) const { return i < cols.size() ? &cols[i] : nullptr; }
    int Find(std::wstring_view name) const;  // case-insensitive, -1 when absent
};

const Schema& SchemaFor(TKind k);
// file name (any case, with or without folder) -> kind; skilleffect.txt -> SeFile
TKind KindFromName(std::wstring_view fileName);
const wchar_t* KindName(TKind k, bool en);
// "#section\tcharacterInfo" header text -> section kind (Unknown when not a header)
TKind SectionKind(std::wstring_view lineText);
// inferred schema (Generic): int / real / text per column from sample rows (raw cells)
Schema InferSchema(const std::vector<std::vector<std::wstring>>& rows, const std::wstring& name);
// ANI_* token -> .bsr role id (SPEC 2.8), -1 when unknown
int AniRoleId(std::wstring_view token);
std::wstring AniRoleName(int roleId);  // inverse (first token), "role N" when unknown

}  // namespace se::ed
