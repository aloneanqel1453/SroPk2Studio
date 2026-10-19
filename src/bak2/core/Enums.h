// SkillEffect Studio - core/Enums.h
// Client enum tables (sro_client.exe 0xEC7138..0xEC7528, SKILLEFFECT_LOGIC.md out_enum_tables.txt).
#pragma once

#include <cwchar>
#include <string_view>

namespace se::enums {

inline constexpr std::wstring_view kAniType[] = {L"READY", L"WAIT",  L"SHOT", L"ACT_OS", L"ACT_OL",
                                                 L"ACT_OE", L"ACT_S", L"ACT_L", L"DEACT",  L"S_RETURN"};
inline constexpr std::wstring_view kBool[] = {L"FALSE", L"false", L"TRUE", L"true"};
inline constexpr std::wstring_view kDmgType[] = {L"none", L"NOR", L"CRI", L"HWAN"};
inline constexpr std::wstring_view kScale[] = {L"none", L"CHAR_BASE", L"MOB_BASE"};
inline constexpr std::wstring_view kWaist[] = {L"none", L"Roll", L"Yaw", L"Pitch", L"RollR", L"YawR", L"PitchR"};
inline constexpr std::wstring_view kRide[] = {L"none", L"RT_FIXED", L"RT_DUMMY"};
inline constexpr std::wstring_view kActType[] = {L"AT_STOP",       L"AT_ONE_FOLLOW", L"AT_LOOP",       L"AT_TARGET",
                                                 L"AT_TARGET_F",   L"AT_SOURCE",     L"AT_DMG_POS",    L"AT_MOV_1TAR",
                                                 L"AT_MOV_OPTION", L"AT_MOV_APIECE", L"AT_MOV_SPLASH", L"AT_MOV_PIERCE"};
inline constexpr std::wstring_view kMov[] = {L"MOV_NONE", L"MOV_STRAIGHT", L"MOV_UP", L"MOV_UPR", L"MOV_ROUND", L"MOV_HWAN"};
inline constexpr std::wstring_view kObjExt[] = {L"bsr", L"efp", L"wav", L"ddj"};
inline constexpr std::wstring_view kDeadEffect[] = {L"none", L"DEFFECT_FLASH"};
inline constexpr std::wstring_view kOp[] = {L"INVSRCALPHA", L"ONE"};
inline constexpr std::wstring_view kTiming[] = {L"none", L"before", L"after"};
inline constexpr std::wstring_view kScript[] = {
    L"none",          L"SCT_ARROW",         L"SCT_RUT",           L"SCT_MOVER",         L"SCT_SHAKECAM0",
    L"SCT_SHAKECAM1", L"SCT_SHAKECAM_MOV0", L"SCT_SHAKECAM_MOV1", L"SCT_SHAKECAM2",     L"SCT_SHAKECAM3",
    L"SCT_SHAKECAM_MOV2", L"SCT_SHAKECAM_MOV3", L"SCT_MAT",       L"SCT_CHAR_SCALE",    L"SCT_EFFECT_SCALE"};
inline constexpr std::wstring_view kSystemKeys[] = {
    L"SYSTEM_CH_HWANMODE", L"SYSTEM_HPPOTION", L"SYSTEM_MPPOTION", L"SYSTEM_LIFE", L"SYSTEM_RETURNSCROLL",
    L"SYSTEM_RETURNSCROLLRESULT", L"SYSTEM_LEVELUP", L"SYSTEM_APPEAR", L"SYSTEM_UNTOUCHABLE",
    L"SYSTEM_EXCLAMATION_START", L"SYSTEM_EXCLAMATION_END", L"SYSTEM_EXCLAMATION_GOING", L"SYSTEM_HELPERMARK",
    L"SYSTEM_KNOCKBACK", L"SYSTEM_QUEST_MARK", L"SYSTEM_CAPTURE_MARK", L"SYSTEM_PET_APPEAR", L"SYSTEM_PET_LEVEL_UP",
    L"SYSTEM_COS_HPPOTION", L"SYSTEM_COS_MPPOTION", L"SYSTEM_COS_HGPPOTION", L"STATUS_CURE_COS",
    L"SYSTEM_QUEST_MARK2", L"SYSTEM_GUILD_WAR_MARK", L"SYSTEM_SKILL_EU_ROG_STEALTHA_CHANGE_A",
    L"EFFECT_NASRUN_NASRUN_RUN_01", L"EFFECT_NASRUN_NASRUN_RUN_02", L"SYSTEM_FREEPVP_RED", L"SYSTEM_FREEPVP_GRAY",
    L"SYSTEM_FREEPVP_BLUE", L"SYSTEM_FREEPVP_WHITE", L"SYSTEM_FREEPVP_YELLOW", L"SYSTEM_FREEPVP_CHANGE_RED",
    L"SYSTEM_FREEPVP_CHANGE_GRAY", L"SYSTEM_FREEPVP_CHANGE_BLUE", L"SYSTEM_FREEPVP_CHANGE_WHITE",
    L"SYSTEM_FREEPVP_CHANGE_YELLOW"};

template <size_t N>
inline bool In(const std::wstring_view (&t)[N], std::wstring_view s) {
    for (const auto& x : t)
        if (x == s) return true;
    return false;
}

// case-insensitive membership (the client lookup is case-sensitive: such a value reads as -1)
template <size_t N>
inline bool InCI(const std::wstring_view (&t)[N], std::wstring_view s) {
    for (const auto& x : t)
        if (x.size() == s.size() && _wcsnicmp(x.data(), s.data(), s.size()) == 0) return true;
    return false;
}

inline int AniTypeIndex(std::wstring_view s) {
    for (int i = 0; i < 10; ++i)
        if (kAniType[i] == s) return i;
    return -1;
}

// ANI_* names of table 0xEC7138 (plus none/before/after which share that table)
inline bool IsAniName(std::wstring_view s) {
    if (s == L"none" || s == L"ANI_HAMMER" || s == L"ANI_HANDLOOF" || s == L"ANI_TROW" || s == L"ANI_SHOT") return true;
    auto num = [](std::wstring_view d, int lo, int hi) {
        if (d.empty() || d.size() > 3) return false;
        int v = 0;
        for (wchar_t c : d) {
            if (c < L'0' || c > L'9') return false;
            v = v * 10 + (c - L'0');
        }
        return v >= lo && v <= hi && !(d.size() > 1 && d[0] == L'0');
    };
    if (s.substr(0, 10) == L"ANI_ATTACK") return num(s.substr(10), 1, 16);
    if (s.substr(0, 10) == L"ANI_SKILL_") return num(s.substr(10), 1, 100);
    if (s.size() == 11 && (s.substr(0, 9) == L"ANI_READY")) return s[9] == L'0' && s[10] >= L'1' && s[10] <= L'4';
    if (s.size() == 10 && (s.substr(0, 8) == L"ANI_WAIT")) return s[8] == L'0' && s[9] >= L'1' && s[9] <= L'4';
    return false;
}

}  // namespace se::enums
