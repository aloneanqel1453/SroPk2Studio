// SkillEffect Studio v3 - client/Catalog.h
// Everything a client defines that can be browsed: creatures / players / NPCs / pets (characterdata), items
// (itemdata), skills (skilldata ENC, the files the client really loads), their names (textdata, language columns
// learnt per file: KR / EN / TR), icons (Media\icon\...), models (Data\res\...) and whether skilleffect.txt has a
// presentation for them. Read through a Vfs (Before or After view), never writes.
//
// Column layout (vSRO 1.188, shifted automatically when a client inserted / removed leading columns):
//   characterdata / itemdata: 0 Service 1 ID 2 CodeName 4 OrgObjCodeName 5 NameStrID 6 DescStrID 9..12 TypeID1..4
//                             14 Country 15 Rarity 33 ReqLevel1 48 Scale 52 AssocFileObj 53 AssocFileDrop 54 AssocFileIcon
//                             57 Lvl (characters) 83..92 DefaultSkill_1..10 (characters)
//   skilldata:                0 Service 1 ID 2 GroupID 3 Basic_Code 5 Basic_Group 6 Basic_Original 7 Basic_Level
//                             8 Basic_Activity 9 Basic_ChainCode 11..16 Action_* 21 Action_Range 34 ReqCommon_Mastery1
//                             61 UI_IconFile 62 UI_SkillName 64 UI_SkillToolTip_Desc 66 AI_AttackChance 68..117 Param1..50
#pragma once

#include "ClientInfo.h"
#include "Vfs.h"

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace se::client {

enum class ObjKind : int { Player = 0, Monster, Npc, Cos, Structure, OtherChar, Item };
constexpr int kObjKinds = 7;
const wchar_t* ObjKindTr(ObjKind k);
const wchar_t* ObjKindEn(ObjKind k);

struct Names {
    std::wstring kr, en, tr;
    std::wstring game;  // the column the game itself displays (type.txt Language; e.g. Vietnam = key + 8)
    // Turkish UI: tr > en > kr ; English UI: en > tr > kr ; "" when no name
    const std::wstring& Best(bool english) const;
    bool Empty() const { return kr.empty() && en.empty() && tr.empty(); }
};

struct CatObject {
    int id = 0;
    std::wstring code, orgCode;           // orgCode: OrgObjCodeName ("" when xxx)
    std::wstring nameKey, descKey;        // SN_... (textdata keys)
    ObjKind kind = ObjKind::OtherChar;
    int type[4]{};                        // TypeID1..4
    int rarity = 0, level = 0, country = 0;
    bool service = true;
    float scale = 100.f;
    std::wstring model;      // Data inner path ("res\\mob\\...\\x.bsr"), clones resolved through orgCode; "" = none
    std::wstring dropModel;  // items: the dropped look
    std::wstring icon;       // Media inner path ("icon\\...\\x.ddj"); "" = none
    std::vector<int> skills; // DefaultSkill_1..10 (non-zero)
    Names names;
    bool charInfo = false;   // skilleffect characterInfo has this code (monsters / pets / mounts)
    signed char modelOk = -1, iconOk = -1;  // after CheckFiles: 1 the file exists in Data / Media, 0 missing, -1 not checked
    std::wstring file;       // textdata file it came from
    bool IsUnique() const { return kind == ObjKind::Monster && (rarity == 3 || rarity == 8); }
};

struct CatSkill {
    int id = 0, groupId = 0;
    std::wstring code, group, nameKey, descKey;
    int original = 0, level = 0, activity = 0, chain = 0;
    int prepare = 0, cast = 0, duration = 0, reuse = 0, cool = 0, flying = 0, range = 0;
    int mastery = 0, attackChance = 0;
    int race = 3;            // ReqLearn_Race (col 47): 0 Chinese, 1 European, 3 any
    int reqWeapon[2] = {255, 255};  // ReqCast_Weapon1/2 (cols 50/51): item TypeID4 of the weapon (255 = any)
    std::wstring icon;       // Media inner path ("icon\\skill\\...") ; "" = none
    Names names;
    bool service = true;
    std::vector<int> params; // Param1..50
    std::wstring presKey;    // presentation key (SRO_SKILLS 5.1): Basic_Group / Basic_Code, through Basic_Original
    bool hasAni = false;     // skillaniset2 / skillaniset row for presKey
    int effectRows = 0;      // skilleffectset rows for presKey
    signed char iconOk = -1; // after CheckFiles
    std::wstring file;
};

struct NameColumns {
    std::wstring file;
    int keyCol = -1, kr = -1, en = -1, tr = -1, game = -1;
    int rows = 0;
    int trRows = 0;            // rows with Turkish letters in the TR column (or in EN when the server put TR there)
    bool turkishInEn = false;  // a Turkish server that wrote its texts into the English column
};

struct CatalogOptions {
    bool items = true;               // itemdata (largest table)
    bool presentation = true;        // read skilleffect.txt for charInfo / hasAni / effectRows
    bool uiTexts = true;             // textuisystem for skill names like PARAM_*
    int gameNameOffset = -2;         // -2 auto (type.txt Language), -1 none, else column offset from the key column
    std::function<void(int percent, const std::wstring& what)> progress;
    const std::atomic<bool>* cancel = nullptr;
};

struct SearchFilter {
    unsigned kinds = 0xFFFFFFFFu;     // bit (1 << ObjKind)
    bool onlyService = false;
    bool onlyWithModel = false;
    bool onlyWithSkills = false;
    bool onlyWithIcon = false;
    int minLevel = 0, maxLevel = 1000;
    size_t maxResults = 5000;
};

class Catalog {
public:
    // false only when nothing could be read (no characterdata / skilldata at all); problems explain every gap
    bool Load(const Vfs& vfs, const CatalogOptions& opt = {}, std::vector<Problem>* problems = nullptr);
    void Clear();
    // existence of every model (Data) and icon (Media) in the Vfs view (fast: folder lookups, no file reads).
    // Returns the number of missing models; problems get one summary line (Info) when some are missing.
    int CheckFiles(const Vfs& vfs, std::vector<Problem>* problems = nullptr, const std::atomic<bool>* cancel = nullptr);

    const std::vector<CatObject>& Objects() const { return m_objs; }
    const std::vector<CatSkill>& Skills() const { return m_skills; }
    const CatObject* ObjectById(int id) const;
    const CatObject* ObjectByCode(std::wstring_view code) const;  // case-insensitive
    const CatSkill* SkillById(int id) const;
    const CatSkill* SkillByCode(std::wstring_view code) const;
    const Names* NamesOf(std::wstring_view key) const;            // textdata key (SN_...)
    std::wstring DisplayName(const CatObject& o, bool english) const;  // name, else code
    std::wstring DisplayName(const CatSkill& s, bool english) const;
    // readable name of a player character code (textdata has none): CHAR_CH_MAN_ADVENTURER -> "Çinli erkek – Adventurer";
    // "" when the code is not CHAR_*
    static std::wstring PlayerName(std::wstring_view code, bool english);

    // skills of a creature: DefaultSkill_1..10, then the MSKILL_<stem>_ family of a monster / pet, each with its
    // chain steps (Basic_ChainCode). Unique, in that order.
    std::vector<int> SkillsOf(const CatObject& o) const;
    std::vector<int> ChainOf(int skillId) const;   // the skill and its follow-up steps
    std::vector<int> UsersOf(int skillId) const;   // object ids listing it in DefaultSkill
    std::wstring PresentationKey(const CatSkill& s) const;
    bool HasAniKey(std::wstring_view key) const;
    int EffectRows(std::wstring_view key) const;

    // free text: id, code, KR / EN / TR names (case-insensitive substring); sorted: exact code / id first
    std::vector<const CatObject*> Search(std::wstring_view text, const SearchFilter& f = {}) const;
    std::vector<const CatSkill*> SearchSkills(std::wstring_view text, size_t maxResults = 5000) const;

    const std::vector<NameColumns>& NameLayout() const { return m_nameCols; }
    const ClientInfo& Info() const { return m_info; }  // version / language / divisions read during Load
    int CountKind(ObjKind k) const;
    std::wstring Summary(bool english) const;  // "1907 canavar, 312 NPC, ... 31400 skill, 64000 ad"

private:
    std::vector<CatObject> m_objs;
    std::vector<CatSkill> m_skills;
    std::unordered_map<int, size_t> m_objById, m_skillById;
    std::unordered_map<std::wstring, size_t> m_objByCode, m_skillByCode;  // upper-case code
    std::unordered_map<std::wstring, Names> m_names;                       // upper-case key
    std::unordered_map<int, std::vector<int>> m_users;
    std::unordered_set<std::wstring> m_aniKeys, m_ciKeys;                  // upper-case
    std::unordered_map<std::wstring, int> m_effectRows;
    std::vector<NameColumns> m_nameCols;
    ClientInfo m_info;
};

}  // namespace se::client
