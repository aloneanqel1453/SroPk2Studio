// bak2::SkillDb + the skill playback glue (bak2/glue/SkillGlue.h). The preview logic is copied
// from BAK-2 gui/gui_object.inc (EnsureDb, ModelInfo, ToSkillInfo, KeyOf, WeaponItem, DefaultBody,
// WeaponTypeFor, PickTarget, DressIx, EquipWeapon, BuildScene, PreviewSkill, PreviewTick,
// PreviewStop, FrameView) and gui_v6_dress.inc (V6FrameFill) with the window code removed. What
// the GUI took from its object page (the opened monster / player and its mastery) comes from the
// skill row here: player skills (SKILL_CH_ / SKILL_EU_) use the race's default body and the
// skill's mastery, other skills the object that lists them (DefaultSkill) or the MOB_<stem> of
// their MSKILL_<stem>_ family.
#include "bak2_skill.hpp"

#include "i18n.hpp"
#include "bak2/glue/SkillGlue.h"
#include "bak2/glue/TextMap.h"

#include "bak2/client/Vfs.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>
#include <initializer_list>
#include <set>
#include <string_view>

namespace bak2 {

namespace {

using se::Bytes;
namespace C = se::client;
namespace R = se::render;
namespace SP = se::sp;

// app ReadFn convention: lower case, '/' separated
std::wstring AppPath(std::wstring p) {
    for (auto& c : p) c = c == L'\\' ? L'/' : static_cast<wchar_t>(std::towlower(c));
    return p;
}

// ------------------------------------------------------------------ masteries and weapons (gui_object.inc)
struct MasteryDef {
    int id, race;
    std::string_view name;   // bak2.inc row
    int weapon;  // usual weapon (TypeID4)
};
const MasteryDef kMasteries[] = {
    {257, 0, "b2_mastery_sword", 2},    {258, 0, "b2_mastery_spear", 4},     {259, 0, "b2_mastery_bow", 6},
    {273, 0, "b2_mastery_cold", 2},     {274, 0, "b2_mastery_lightning", 2}, {275, 0, "b2_mastery_fire", 2},
    {276, 0, "b2_mastery_force", 2},    {513, 1, "b2_mastery_warrior", 7},   {514, 1, "b2_mastery_wizard", 11},
    {515, 1, "b2_mastery_rogue", 12},   {516, 1, "b2_mastery_warlock", 10},  {517, 1, "b2_mastery_bard", 14},
    {518, 1, "b2_mastery_cleric", 15},
};
struct WeaponDef {
    int type;
    const wchar_t* group;  // aniGroup in the character .bsr (skillaniset2 AniGroup, lower case)
    std::string_view name;   // bak2.inc row
};
const WeaponDef kWeapons[] = {
    {2, L"sword", "b2_weapon_sword"},                   {3, L"sword", "b2_weapon_blade"},
    {4, L"spear", "b2_weapon_spear"},                   {5, L"spear", "b2_weapon_glaive"},
    {6, L"bow", "b2_weapon_bow"},                       {7, L"onehand_sword", "b2_weapon_1h_sword"},
    {8, L"twohand_sword", "b2_weapon_2h_sword"},        {9, L"dual_axe", "b2_weapon_dual_axe"},
    {10, L"onehand_staff", "b2_weapon_warlock_staff"},  {11, L"twohand_staff", "b2_weapon_wizard_staff"},
    {12, L"bow", "b2_weapon_crossbow"},                 {13, L"dagger", "b2_weapon_dagger"},
    {14, L"harf", "b2_weapon_harp"},                    {15, L"onehand_staff", "b2_weapon_cleric_staff"},
};
const WeaponDef* WeaponDefOf(int t) {
    for (const auto& w : kWeapons)
        if (w.type == t) return &w;
    return nullptr;
}
const MasteryDef* MasteryOf(int id) {
    for (const auto& m : kMasteries)
        if (m.id == id) return &m;
    return nullptr;
}
int FirstMasteryOfRace(int race) {
    for (const auto& m : kMasteries)
        if (m.race == race) return m.id;
    return 0;
}

int RaceOfCode(const std::wstring& code) {
    const std::wstring u = se::ToUpper(code);
    return u.find(L"_EU_") != std::wstring::npos || u.rfind(L"CHAR_EU", 0) == 0 ? 1 : 0;
}

bool IsPlayerSkill(const C::CatSkill& s) {
    const std::wstring u = se::ToUpper(s.code);
    return u.rfind(L"SKILL_CH_", 0) == 0 || u.rfind(L"SKILL_EU_", 0) == 0;
}

// the weapon type a skill is cast with: its ReqCast_Weapon, else the mastery's usual weapon
int WeaponTypeFor(const C::CatSkill& s, int mastery, int race) {
    for (int w : s.reqWeapon)
        if (w > 0 && w != 255 && WeaponDefOf(w)) return w;
    if (const auto* m = MasteryOf(s.mastery)) return m->weapon;
    if (const auto* m = MasteryOf(mastery)) return m->weapon;
    return race ? 7 : 2;
}

// the body a player skill is shown on (a default character of its race)
const C::CatObject* DefaultBody(const C::Catalog& c, int race) {
    for (const wchar_t* code : race ? std::initializer_list<const wchar_t*>{L"CHAR_EU_MAN_NOBLE", L"CHAR_EU_MAN_ADVENTURER"}
                                    : std::initializer_list<const wchar_t*>{L"CHAR_CH_MAN_ADVENTURER", L"CHAR_CH_MAN_NOBLE"})
        if (const auto* o = c.ObjectByCode(code))
            if (!o->model.empty()) return o;
    for (const auto& o : c.Objects())
        if (o.kind == C::ObjKind::Player && RaceOfCode(o.code) == race && !o.model.empty()) return &o;
    return nullptr;
}

// the weapon item a character of a race holds for a weapon type: the lowest common one with a model
const C::CatObject* WeaponItem(const SkillDb::Impl& d, int race, int wtype) {
    if (wtype <= 0) return nullptr;
    const int k = race << 8 | wtype;
    if (auto it = d.weaponOf.find(k); it != d.weaponOf.end()) return it->second ? d.cat.ObjectById(it->second) : nullptr;
    const C::CatObject* best = nullptr;
    const std::wstring pre = race ? L"ITEM_EU_" : L"ITEM_CH_";
    for (const auto& o : d.cat.Objects()) {
        if (o.kind != C::ObjKind::Item || o.type[2] != 6 || o.type[3] != wtype || o.model.empty() || !o.service) continue;
        if (se::ToUpper(o.code).rfind(pre, 0) != 0) continue;
        if (!best || o.level < best->level || (o.level == best->level && o.code < best->code)) best = &o;
    }
    d.weaponOf[k] = best ? best->id : 0;
    return best;
}

// a target monster for attack previews (a small, common one; any monster with a model as a fallback).
// players are human size: a smaller target first so the caster and its weapon stay readable in the frame.
// (The GUI had run Catalog::CheckFiles; here the model is probed through the reader.)
const C::CatObject* PickTarget(const SkillDb::Impl& d, bool player) {
    static const wchar_t* const kMob[] = {L"MOB_CH_TIGER", L"MOB_CH_MANGNYANG", L"MOB_OA_URUCHI", L"MOB_EU_KOBOLD"};
    static const wchar_t* const kPlayer[] = {L"MOB_CH_MANGNYANG", L"MOB_EU_KOBOLD", L"MOB_CH_TIGER", L"MOB_OA_URUCHI"};
    for (const wchar_t* code : player ? kPlayer : kMob)
        if (const auto* o = d.cat.ObjectByCode(code))
            if (!o->model.empty() && d.Exists(o->model)) return o;
    for (const auto& o : d.cat.Objects())
        if (o.kind == C::ObjKind::Monster && !o.IsUnique() && !o.model.empty() && o.level > 0 && o.level < 30 && d.Exists(o.model)) return &o;
    return nullptr;
}

// glue: the caster of a non-player skill - an object listing it in DefaultSkill (Catalog::UsersOf), else the
// MOB_<stem> whose MSKILL_<stem>_ family holds it (the inverse of Catalog::SkillsOf), longest stem first
const C::CatObject* CasterOf(const SkillDb::Impl& d, const C::CatSkill& cs) {
    auto usable = [&](const C::CatObject* o) { return o && !o->model.empty() && d.Exists(o->model); };
    for (int id : d.cat.UsersOf(cs.id))
        if (const auto* o = d.cat.ObjectById(id); usable(o)) return o;
    const std::wstring up = se::ToUpper(cs.code);
    if (up.rfind(L"MSKILL_", 0) == 0) {
        std::wstring stem = up.substr(7);
        for (;;) {
            if (const auto* o = d.cat.ObjectByCode(L"MOB_" + stem); usable(o)) return o;
            const std::size_t k = stem.find_last_of(L'_');
            if (k == std::wstring::npos || k == 0) break;
            stem.resize(k);
        }
    }
    return nullptr;
}

std::wstring ModelOf(const C::CatObject& o) { return !o.model.empty() ? o.model : o.dropModel; }

// glue (format adapter, not BAK-2 code): clients that ship textdata skilleffect.txt empty and keep the three
// presentation tables as textdata index + part files instead - characterinfo.txt / skillaniset.txt /
// skilleffectset.txt, every row "Service<TAB>ID<TAB><the skilleffect.txt cells>". They are joined into one
// skilleffect.txt-shaped text (UTF-16LE, the client's section headers) so BAK-2's FxDb / Catalog read them with
// their own rules. characterInfo and skilleffectset have no Service cell: their Service 0 rows are left out and
// both leading cells dropped; skillaniset2 starts with its Service cell (FxDb: Service 0 = disabled), only the
// ID cell is dropped. Empty when the tables are not there.
Bytes ComposeSplitSkillEffect(const ReadFn& read, const std::wstring& td, int& parts) {
    struct Table {
        const wchar_t* base;
        const wchar_t* section;
        bool keepService;
    };
    static constexpr Table kTables[] = {{L"characterinfo", L"characterInfo", false},
                                        {L"skillaniset", L"skillaniset2", true},
                                        {L"skilleffectset", L"skilleffectset", false}};
    auto lines = [](const std::wstring& text, auto&& fn) {
        std::size_t a = 0;
        while (a < text.size()) {
            std::size_t z = text.find_first_of(L"\r\n", a);
            if (z == std::wstring::npos) z = text.size();
            fn(std::wstring_view(text).substr(a, z - a));
            a = z + 1;
        }
    };
    std::wstring out = L"﻿";
    int rows = 0;
    parts = 0;
    for (const Table& t : kTables) {
        Bytes idx;
        if (!read(AppPath(td + t.base + L".txt"), idx) || idx.empty()) return {};
        out += std::wstring(L"#section\t") + t.section + L"\r\n";
        lines(se::DecodeText(idx), [&](std::wstring_view l) {
            const std::wstring name = se::Trim(l);
            if (!se::EndsWithI(name, L".txt") || name.find_first_of(L"\\/\t") != std::wstring::npos) return;
            Bytes part;
            if (!read(AppPath(td + name), part)) return;
            ++parts;
            lines(se::DecodeText(part), [&](std::wstring_view row) {
                if (se::Trim(row).empty() || se::StartsWith(se::Trim(row), L"//")) return;
                const auto cells = se::SplitView(row, L'\t');
                if (cells.size() < 3) return;
                if (!t.keepService && se::Trim(cells[0]) == L"0") return;
                std::wstring r;
                for (std::size_t i = 0; i < cells.size(); ++i) {
                    if (i == 1 || (i == 0 && !t.keepService)) continue;
                    if (!r.empty() || (t.keepService && i > 0)) r += L'\t';
                    r += cells[i];
                }
                out += r + L"\r\n";
                ++rows;
            });
        });
    }
    if (rows == 0) return {};
    Bytes b(out.size() * 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        b[i * 2] = static_cast<std::uint8_t>(out[i] & 0xFF);
        b[i * 2 + 1] = static_cast<std::uint8_t>(out[i] >> 8);
    }
    return b;
}

// ------------------------------------------------------------------ BAK-2 texts in the UI language (glue/TextMap.h)
// se::client::Catalog / Vfs load problems (Problem::tr / en): their English wording
constexpr glue::TextMap kCatalogTexts[] = {
    {L"No characterdata: the creature list stays empty.", "b2_cat_no_chardata"},
    {L"Table not readable: {}", "b2_table_unreadable"},
    {L"Skill table not readable: {}", "b2_cat_skill_table_read"},
    {L"No skilldata: the skill list stays empty.", "b2_cat_no_skilldata"},
    {L"No readable skill rows.", "b2_cat_skill_empty"},
    {L"Name table not readable: {}", "b2_cat_name_table_read"},
    {L"No name tables: only codes are shown.", "b2_cat_no_names"},
    {L"No name column found in {}", "b2_cat_name_columns"},
    {L"No skilleffect.txt: no animation / effect info.", "b2_cat_no_skilleffect"},
    {L"skilleffect unreadable: {}", "b2_cat_skilleffect_read"},
    {L"File listed in {}.txt is missing: {}", "b2_cat_index_missing"},
};

// se::sp::BuildTimeline notes (TlNote::tr / en, skillplay/Timeline.cpp): their English wording
constexpr glue::TextMap kTimelineNotes[] = {
    {L"The key has no skillaniset2 or skilleffectset row: nothing to play.", "b2_tn_nothing"},
    {L"No skillaniset2 row: no animation; the effect rows attach to a named object (normal for effect-only keys only).", "b2_tn_no_aniset"},
    {L"skillaniset2 Service = 0: the client ignores the row (the preview still plays it).", "b2_tn_service0"},
    {L"No skilldata row given: timing comes from the clips (PreparingTime 0).", "b2_tn_no_skilldata"},
    {L"The creature has no characterInfo row ({}): MOB_BASE scale 1, no sound object.", "b2_tn_no_charinfo"},
    {L"{}: the model has no {} clip: no animation plays (63 shipped skills do the same).", "b2_tn_no_clip"},
    {L"AniShot list ({} clips): the client picks at random; chosen {}.", "b2_tn_shot_list"},
    {L"AniShot not recognised: {}", "b2_tn_shot_unknown"},
    {L"PreparingTime 0 but READY/WAIT clips are set: each is played once [U].", "b2_tn_prep0"},
    {L"Skill time (cast + action) {} ms, attack animation {} ms: most shipped skills have them equal; the animation plays its own length.",
     "b2_tn_time_diff"},
    {L"PreparingTime without AniReady/AniWait: the creature stands during the preparation.", "b2_tn_prep_stand"},
    {L"StartEvent {}, the clip has {} events: row not played.", "b2_tn_event_missing"},
    {L"StartEvent {} but the clip has only {} type-1 events: moved to the last one [U].", "b2_tn_event_last"},
    {L"The clip has no type-1 event: StartEvent {} estimated at {} ms.", "b2_tn_event_est"},
    {L"The row plays only for another damage kind (DamageType): skipped.", "b2_tn_dmg_kind"},
    {L"Unparsed cell: {}", "b2_tn_unparsed"},
    {L"Bone not in the skeleton: {} (the effect starts at the root).", "b2_tn_bone"},
    {L"Script flag listed only: {}", "b2_tn_script"},
    {L"Buff time {} s: the preview shows it shortened to {} s (start, loop, end).", "b2_tn_buff_short"},
    {L"Buff / status duration unknown: {} ms shown.", "b2_tn_buff_unknown"},
    {L"AniType not recognised: the row never plays.", "b2_tn_anitype"},
    {L"DMG Event TRUE only on READY/WAIT/SHOT/S_RETURN (client assert \"i < 3 || i == 9\").", "b2_tn_dmg_phase"},
    {L"ACT_O*: plays on party members; shown on the caster in the preview.", "b2_tn_act_o"},
    {L"StartEvent is not used in buff phases [U].", "b2_tn_buff_event"},
    {L"{} {}: no earlier instance with ID {} in this key [D].", "b2_tn_no_instance"},
    {L"No hit row (DMG Event TRUE) although the skill deals damage: the preview shows the hit at the attack event as an ESTIMATE [U].",
     "b2_tn_no_hit_row"},
    {L"Sound times from the model's own sound tracks (.bsr): {} sounds.", "b2_tn_snd_tracks"},
    {L"{} sounds have no track in the model: estimated time (shout at the clip start, swing at event k) [U].", "b2_tn_snd_est"},
    {L"Param mc: {} hits, {} DMG Event TRUE rows in skilleffectset (usually one row per hit).", "b2_tn_hits"},
    {L"First hit at {} ms in the clip, CastingTime {} ms (the server applies the damage at CastingTime).", "b2_tn_first_hit"},
    {L"Chain skill: the steps share this one clip; the rows live on the head key.", "b2_tn_chain"},
    {L"Param stream: {}", "b2_tn_param"},
    {L"Weapon trail (mirage) is for armed players: listed only.", "b2_tn_trail"},
    {L"DefenseEfp plays only on a block: {}", "b2_tn_defense"},
    {L"The row produces nothing visible (no object / sound / hit).", "b2_tn_row_nothing"},
};

std::wstring ProblemText(const C::Problem& p) { return p.code + L": " + glue::Localize(p.tr, p.en, kCatalogTexts); }

// one timeline note; the projectile note is put together from optional parts
std::wstring NoteText(const SP::TlNote& n) {
    constexpr std::wstring_view kProjectile = L"Projectile: speed random within the row's range, flight time = preview distance / speed";
    if (n.en.starts_with(kProjectile)) {
        std::wstring s = pk2::L("b2_tn_proj");
        if (n.en.find(L", copies 100 ms apart") != std::wstring::npos) s += pk2::L("b2_tn_proj_volley");
        if (n.en.find(L", arc height from Param") != std::wstring::npos) s += pk2::L("b2_tn_proj_arc");
        return s + pk2::L("b2_tn_proj_end");
    }
    return glue::Localize(n.tr, n.en, kTimelineNotes);
}

// Timeline::Dump (a code-level listing) with its note lines in the UI language (Dump prints TlNote::en)
std::wstring DumpText(SP::Timeline tl) {
    for (SP::TlNote& n : tl.notes) n.en = NoteText(n);
    return tl.Dump();
}

// se::sp::DescribeRow (skillplay/Describe.cpp, TR / EN) in the UI language: the same sentence built from the
// row's cells, its parts from bak2.inc.
std::wstring RowText(const SP::EffectRow& r) {
    std::wstring s;
    switch (r.phase) {
        case SP::Phase::Ready: s = pk2::L("b2_ph_ready"); break;
        case SP::Phase::Wait: s = pk2::L("b2_ph_wait"); break;
        case SP::Phase::Shot: s = pk2::L("b2_ph_shot"); break;
        case SP::Phase::ActOS: s = pk2::L("b2_ph_act_os"); break;
        case SP::Phase::ActOL: s = pk2::L("b2_ph_act_ol"); break;
        case SP::Phase::ActOE: s = pk2::L("b2_ph_act_oe"); break;
        case SP::Phase::ActS: s = pk2::L("b2_ph_act_s"); break;
        case SP::Phase::ActL: s = pk2::L("b2_ph_act_l"); break;
        case SP::Phase::Deact: s = pk2::L("b2_ph_deact"); break;
        case SP::Phase::SReturn: s = pk2::L("b2_ph_sreturn"); break;
        default: s = pk2::L("b2_ph_bad"); break;
    }
    if (r.phase == SP::Phase::Ready || r.phase == SP::Phase::Wait || r.phase == SP::Phase::Shot)
        s += r.startEvent <= 0 ? pk2::L("b2_row_at_start") : pk2::Lf("b2_row_at_event", r.startEvent);
    if (r.movDelay > 0) s += std::format(L" + {:.0f} ms", r.movDelay);
    s += L": ";
    const bool none = r.objName.empty() && r.kind != SP::ObjKind::Wav;
    if (r.kind == SP::ObjKind::Wav) s += pk2::Lf("b2_row_sound", r.objName);
    else if (none) s += pk2::L("b2_row_no_fx");
    else if (r.kind == SP::ObjKind::Bsr) s += pk2::Lf("b2_row_model", r.objName);
    else s += pk2::Lf("b2_row_effect", r.objName);
    const bool sb = !r.startBone.empty(), tb = !r.targetBone.empty();
    if (r.Projectile()) {
        if (r.act == SP::Act::Target || r.act == SP::Act::TargetF || r.act == SP::Act::DmgPos) s += pk2::L("b2_row_falls");
        else if (r.act == SP::Act::MovOption) s += pk2::Lf("b2_row_flies_out", r.opt.angle);
        else
            s += pk2::Lf("b2_row_flies", sb ? pk2::Lf("b2_row_from_bone", r.startBone) : pk2::L("b2_row_from_body"),
                         tb ? pk2::Lf("b2_row_to_bone", r.targetBone) : pk2::L("b2_row_to_target"));
        s += pk2::Lf("b2_row_speed", r.speedMin, r.speedMax);
        if (!r.objName2.empty()) s += pk2::Lf("b2_row_arrival", r.objName2);
    } else if (!none) {
        switch (r.act) {
            case SP::Act::OneFollow: s += sb ? pk2::Lf("b2_row_follow_bone", r.startBone) : pk2::L("b2_row_follow_body"); break;
            case SP::Act::Loop: s += sb ? pk2::Lf("b2_row_loop_bone", r.startBone) : pk2::L("b2_row_loop_body"); break;
            case SP::Act::Stop: s += pk2::Lf("b2_row_stop", sb ? r.startBone : pk2::L("b2_row_body")); break;
            case SP::Act::Source: s += pk2::L("b2_row_on_caster"); break;
            case SP::Act::Target: s += tb ? pk2::Lf("b2_row_on_target_bone", r.targetBone) : pk2::L("b2_row_on_target"); break;
            case SP::Act::TargetF: s += tb ? pk2::Lf("b2_row_follow_target_bone", r.targetBone) : pk2::L("b2_row_follow_target"); break;
            case SP::Act::DmgPos: s += pk2::L("b2_row_dmgpos"); break;
            case SP::Act::Bad: s += pk2::L("b2_row_act_bad"); break;
            default: s += pk2::L("b2_row_at_target"); break;
        }
    }
    if (r.createCnt > 1) s += pk2::Lf("b2_row_copies", r.createCnt);
    if (r.dmg == 1) s += pk2::L("b2_row_hit");
    if (r.dmg < 0) s += pk2::L("b2_row_dmg_typo");
    if (r.scale == SP::ScaleBase::Mob) s += pk2::L("b2_row_grows");
    if (r.script & (SP::kSctShake0 | SP::kSctShake1 | SP::kSctShake2 | SP::kSctShake3 | SP::kSctShakeMov0 | SP::kSctShakeMov1 |
                    SP::kSctShakeMov2 | SP::kSctShakeMov3))
        s += pk2::L("b2_row_shake");
    if (r.script & SP::kSctMat) s += pk2::L("b2_row_tint");
    if ((r.script & SP::kSctEffectScale) && !r.scriptArgs.empty()) s += pk2::Lf("b2_row_fx_scale", r.scriptArgs[0]);
    if (r.trade > 0) s += pk2::Lf("b2_row_replaces", r.trade);
    if (r.kill > 0) s += pk2::Lf("b2_row_removes", r.kill);
    if (!r.sndBegin.empty()) s += pk2::Lf("b2_row_snd_begin", r.sndBegin);
    if (!r.sndEnd.empty()) s += pk2::Lf("b2_row_snd_end", r.sndEnd);
    if (r.origin == SP::Origin::Pending) s += pk2::L("b2_row_pending");
    return s;
}

}  // namespace

// ================================================================== SkillDb::Impl
bool SkillDb::Impl::Exists(const std::wstring& dataPath) const {
    const std::wstring k = AppPath(dataPath);
    if (auto it = exists.find(k); it != exists.end()) return it->second;
    std::vector<std::uint8_t> b;
    const bool ok = read && read(k, b) && !b.empty();
    exists[k] = ok;
    return ok;
}

// the catalog's armour / shield / weapon items for the player's outfit (GUI DressIx)
R::DressIndex& SkillDb::Impl::Dress() const {
    if (!dressBuilt) {
        dress.Clear();
        dress.AddAll(cat.Objects(), [](const C::CatObject& o, R::DressItem& it) {
            if (o.kind != C::ObjKind::Item) return false;
            it.id = o.id;
            it.code = o.code;
            for (int i = 0; i < 4; ++i) it.type[i] = o.type[i];
            it.level = o.level;
            it.service = o.service;
            it.model = o.model;
            return true;
        });
        dressBuilt = true;
    }
    return dress;
}

std::shared_ptr<SP::BsrModel> SkillDb::Impl::ModelInfo(const std::wstring& path) const {
    if (path.empty()) return nullptr;
    const std::wstring k = se::ToUpper(path);
    if (auto it = bsr.find(k); it != bsr.end()) return it->second;
    std::shared_ptr<SP::BsrModel> m;
    Bytes b;
    if (read && read(AppPath(path), b)) {
        auto mm = std::make_shared<SP::BsrModel>();
        std::wstring err;
        const ReadFn r = read;
        if (mm->Load(b, [r](const std::wstring& p, Bytes& o) { return r(AppPath(p), o); }, err)) m = mm;
    }
    bsr[k] = m;
    return m;
}

SP::SkillInfo SkillDb::Impl::ToSkillInfo(const C::CatSkill& cs) const {
    SP::SkillInfo s;
    s.valid = true;
    s.id = cs.id;
    s.code = cs.code;
    s.group = cs.group;
    s.original = cs.original;
    if (cs.chain)
        if (const auto* n = cat.SkillById(cs.chain)) s.chain = n->code;
    s.prepMs = cs.prepare;
    s.castMs = cs.cast;
    s.durMs = cs.duration;
    s.flySpeed = cs.flying;
    std::vector<long long> p(cs.params.begin(), cs.params.end());
    if (!p.empty()) SP::WalkSkillParams(p, s);
    return s;
}

std::wstring SkillDb::Impl::KeyOf(const C::CatSkill& cs) const {
    if (!cs.presKey.empty()) return cs.presKey;
    std::map<int, SP::SkillInfo> cache;
    const SP::SkillInfo si = ToSkillInfo(cs);
    std::wstring k = db.PresentationKey(si, [&](int id) -> const SP::SkillInfo* {
        auto it = cache.find(id);
        if (it != cache.end()) return &it->second;
        const auto* x = cat.SkillById(id);
        if (!x) return nullptr;
        return &(cache[id] = ToSkillInfo(*x));
    });
    return k.empty() ? cs.code : k;
}

std::wstring SkillDb::Impl::Group(const C::CatSkill& cs) const {
    if (const auto* m = MasteryOf(cs.mastery)) return pk2::L(m->name);
    return KeyOf(cs);
}

// ================================================================== SkillDb
SkillDb::SkillDb() : p_(std::make_unique<Impl>()) {}
SkillDb::~SkillDb() = default;

std::unique_ptr<SkillDb> SkillDb::load(ReadFn read, std::wstring& err) {
    err.clear();
    if (!read) {
        err = pk2::L("b2_err_no_reader");
        return nullptr;
    }
    std::unique_ptr<SkillDb> out(new SkillDb());
    Impl& m = *out->p_;
    m.read = read;

    // The catalog reads through a se::client::Vfs. The bridge has no archive of its own: the client text files
    // are read through the app's reader and staged in memory (Vfs::Stage = the AFTER view's front layer).
    // Index files (every line one *.txt name) pull in the files they list, two levels deep (textdataname ->
    // textdata_object.txt -> its parts), the rule Vfs::TextdataParts applies when the catalog asks.
    C::Vfs vfs;
    std::set<std::wstring> staged;
    const std::wstring td = L"server_dep\\silkroad\\textdata\\";
    auto stage = [&](auto& self, C::ArcKind arc, const std::wstring& inner, int depth, Bytes* keep) -> bool {
        const std::wstring key = AppPath(inner);
        if (!staged.insert(key).second) return true;
        Bytes b;
        if (!read(key, b) || b.empty()) return false;
        if (depth > 0) {
            const std::wstring text = se::DecodeText(b);
            std::size_t a = 0;
            while (a < text.size()) {
                std::size_t z = text.find_first_of(L"\r\n", a);
                if (z == std::wstring::npos) z = text.size();
                const std::wstring line = se::Trim(std::wstring_view(text).substr(a, z - a));
                a = z + 1;
                if (line.empty() || se::StartsWith(line, L"//")) continue;
                const std::wstring first = se::Trim(line.substr(0, line.find(L'\t')));
                if (se::EndsWithI(first, L".txt") && first.find_first_of(L"\\/") == std::wstring::npos)
                    self(self, arc, se::DirOf(inner) + L"\\" + first, depth - 1, nullptr);
            }
        }
        if (keep) *keep = b;
        ++m.files;
        m.bytes += b.size();
        vfs.Stage(arc, inner, std::move(b));
        return true;
    };
    Bytes skillEffect, effectSound;
    for (const wchar_t* f : {L"SV.T", L"type.txt", L"DIVISIONINFO.TXT", L"GATEPORT.TXT"}) stage(stage, C::ArcKind::Media, f, 0, nullptr);
    stage(stage, C::ArcKind::Media, td + L"characterdata.txt", 2, nullptr);
    stage(stage, C::ArcKind::Media, td + L"itemdata.txt", 2, nullptr);
    if (!stage(stage, C::ArcKind::Media, td + L"skilldataenc.txt", 2, nullptr))
        stage(stage, C::ArcKind::Media, td + L"skilldata.txt", 2, nullptr);
    stage(stage, C::ArcKind::Media, td + L"textdataname.txt", 2, nullptr);
    for (const wchar_t* f : {L"textdata_object.txt", L"textdata_equip&skill.txt", L"textuisystem.txt"})
        stage(stage, C::ArcKind::Media, td + f, 1, nullptr);
    stage(stage, C::ArcKind::Media, td + L"skilleffect.txt", 1, &skillEffect);
    if (se::DecodeText(skillEffect).find(L"#section") == std::wstring::npos) {   // empty skilleffect.txt: split tables
        int parts = 0;
        if (Bytes composed = ComposeSplitSkillEffect(read, td, parts); !composed.empty()) {
            m.problems.push_back([parts] { return pk2::Lf("b2_prob_split", parts); });
            m.files += parts;
            m.bytes += composed.size();
            skillEffect = composed;
            vfs.Stage(C::ArcKind::Media, td + L"skilleffect.txt", std::move(composed));
        }
    }
    read(AppPath(td + L"effectsound.txt"), effectSound);
    vfs.SetView(C::View::After);

    C::CatalogOptions opt;   // items (weapons / outfit), presentation (hasAni / effect rows), UI texts
    std::vector<C::Problem> probs;
    const bool ok = m.cat.Load(vfs, opt, &probs);
    for (const auto& p : probs)
        if (p.level >= C::Problem::Warn) m.problems.push_back([p] { return ProblemText(p); });
    if (!ok || m.cat.Skills().empty()) {
        err = pk2::L("b2_err_no_skills");
        for (const auto& p : m.problems) err += L"\n  " + p();
        return nullptr;
    }
    // GUI EnsureDb: the presentation tables the timeline reads
    std::wstring e;
    if (skillEffect.empty()) m.problems.push_back([] { return pk2::L("b2_prob_no_se"); });
    else if (!m.db.LoadSkillEffect(skillEffect, e) || !e.empty()) m.problems.push_back([e] { return L"skilleffect.txt: " + e; });
    if (effectSound.empty()) m.problems.push_back([] { return pk2::L("b2_prob_no_sound"); });
    else m.db.LoadEffectSound(effectSound);
    return out;
}

std::size_t SkillDb::size() const { return p_->cat.Skills().size(); }

std::vector<SkillItem> SkillDb::search(const std::wstring& query, std::size_t max) const {
    std::vector<SkillItem> out;
    for (const C::CatSkill* s : p_->cat.SearchSkills(query, max))
        out.push_back({s->id, s->code, p_->cat.DisplayName(*s, glue::Bak2English()), p_->Group(*s)});
    return out;
}

std::wstring SkillDb::summary() const {
    return pk2::Lf("b2_sk_summary", p_->cat.Summary(glue::Bak2English()), p_->files, static_cast<double>(p_->bytes) / (1024.0 * 1024.0));
}

std::vector<std::wstring> SkillDb::problems() const {
    std::vector<std::wstring> out;
    for (const auto& p : p_->problems) out.push_back(p());
    return out;
}

std::wstring SkillDb::describe(int id) const {
    const Impl& m = *p_;
    const C::CatSkill* cs = m.cat.SkillById(id);
    if (!cs) return pk2::Lf("b2_sk_no_id", id);
    const std::wstring key = m.KeyOf(*cs);
    std::wstring s = pk2::Lf("b2_sk_head", m.cat.DisplayName(*cs, glue::Bak2English()), cs->code, cs->id, cs->level) + L"\n";
    const auto rows = m.db.Effects(key);
    const SP::AniSetRow* sa = m.db.AniSet(key);
    s += pk2::Lf("b2_sk_pres", key, sa ? pk2::L("b2_sk_yes") : pk2::L("b2_sk_no"), std::wstring(m.db.Disabled(key) ? L" (Service 0)" : L""),
                 rows.size()) +
         L"\n";
    if (const auto* ms = MasteryOf(cs->mastery)) s += pk2::Lf("b2_sk_mastery", pk2::L(ms->name)) + L"\n";
    for (int w : cs->reqWeapon)
        if (const auto* wd = WeaponDefOf(w)) s += pk2::Lf("b2_sk_weapon", pk2::L(wd->name), wd->group) + L"\n";
    s += pk2::Lf("b2_sk_times", cs->prepare, cs->cast, cs->duration, cs->reuse, cs->cool, cs->range) + L"\n";
    if (!rows.empty()) {
        s += pk2::L("b2_sk_rows") + L"\n";
        for (std::size_t i = 0; i < rows.size(); ++i) s += std::format(L"  {}. {}\n", i + 1, RowText(*rows[i]));
    }
    SP::PlayOptions po;
    po.shotPick = 0;
    const SP::Timeline tl = SP::BuildTimeline(m.db, key, m.ToSkillInfo(*cs), nullptr, nullptr, po);
    s += pk2::L("b2_sk_timeline") + L"\n";
    s += DumpText(tl);
    return s;
}

// ================================================================== playback (bak2::View::play_skill)
namespace glue {

bool StartSkill(const SkillDb::Impl& d, int skillId, R::Scene& scene, R::AssetCache& cache, SkillSession& s, std::wstring& err) {
    const C::Catalog& c = d.cat;
    const C::CatSkill* cs = c.SkillById(skillId);
    if (!cs) {
        err = pk2::Lf("b2_err_skill_missing", skillId);
        return false;
    }
    s.skillId = skillId;
    s.code = cs->code;
    s.title = c.DisplayName(*cs, Bak2English());

    // --- who casts (GUI SetupPlayerCtx / the object page)
    const C::CatObject* body = nullptr;
    if (IsPlayerSkill(*cs)) {
        s.race = RaceOfCode(cs->code);
        body = DefaultBody(c, s.race);
        s.isPlayer = body != nullptr;
        s.mastery = MasteryOf(cs->mastery) ? cs->mastery : FirstMasteryOfRace(s.race);
    }
    if (!s.isPlayer) body = CasterOf(d, *cs);
    if (!body) s.notes.push_back(pk2::Lf("b2_note_no_caster", cs->code));

    // --- GUI BuildScene
    const std::wstring model = body ? ModelOf(*body) : std::wstring();
    s.casterModel = model;
    s.casterCode = body ? body->code : std::wstring();
    s.wtype = s.isPlayer ? WeaponTypeFor(*cs, s.mastery, s.race) : 0;
    std::wstring e;
    if (!model.empty()) {
        R::ModelPlace pc;
        if (const WeaponDef* wd = WeaponDefOf(s.wtype)) pc.idleGroup = wd->group;
        s.caster = scene.AddModel(model, pc, &e);
        if (s.caster <= 0) s.notes.push_back(model + L": " + Localize(e, kRenderTexts));
    }
    R::DressPlan dress;
    if (s.isPlayer && s.caster > 0) {   // armour + shield of the weapon's build, before the weapon
        R::DressRequest rq;
        rq.body = body->code;
        rq.bodyModel = model;
        rq.weaponType = s.wtype;
        rq.mastery = s.mastery;
        dress = d.Dress().Plan(rq, [&d](const std::wstring& p) { return d.Exists(p); });
        std::wstring lg;
        if (auto* cm = scene.Model(s.caster)) R::ApplyDress(*cm, cache, dress, &lg);
        if (!lg.empty()) s.notes.push_back(lg);
    }
    if (s.isPlayer && s.caster > 0) {   // GUI EquipWeapon
        auto* m = scene.Model(s.caster);
        const WeaponDef* wd = WeaponDefOf(s.wtype);
        const C::CatObject* item = nullptr;
        if (!dress.weapon.code.empty())
            if (const auto* dw = c.ObjectByCode(dress.weapon.code))
                if (dw->type[3] == s.wtype && !dw->model.empty()) item = dw;
        if (!item) item = WeaponItem(d, s.race, s.wtype);
        if (m && wd && item && !item->model.empty()) {
            std::wstring e2;
            const std::size_t firstPart = m->Def()->parts.size();
            bool ok = m->Attach(cache, item->model, e);
            if (!ok) {   // daggers / axes: two files, right (main) + left (off hand)
                std::wstring stem = item->model;
                if (se::EndsWithI(stem, L".bsr")) stem.resize(stem.size() - 4);
                ok = m->Attach(cache, stem + L"_r.bsr", e);
                m->Attach(cache, stem + L"_l.bsr", e2);
            }
            if (ok) {
                if (R::FixHeldParts(*m, firstPart)) m->Advance(0);   // women / robes: the weapon on its hand bone
                s.wgroup = wd->group;
                m->Play(cache, 0x00, true, s.wgroup);
                s.notes.push_back(pk2::Lf("b2_note_weapon", item->code, s.wgroup));
            } else {
                s.notes.push_back(pk2::Lf("b2_note_weapon_fail", item->model, Localize(e, kRenderTexts)));
            }
        } else if (wd) {
            s.notes.push_back(pk2::Lf("b2_note_no_weapon_model", pk2::L(wd->name)));
        }
    }
    double rc = 9;
    if (s.caster > 0)
        if (auto* mi = scene.Model(s.caster)) rc = std::max(4.0, mi->Def()->Radius());
    s.dist = 60;
    const bool creature = s.isPlayer || (body && body->kind != C::ObjKind::Item && body->kind != C::ObjKind::Structure);
    if (creature && s.caster > 0) {
        if (const auto* t = PickTarget(d, s.isPlayer)) {
            R::ModelPlace pt;
            double rt = 9;
            pt.yawDeg = 180;
            pt.pos = R::V3(0, 0, -60);
            s.target = scene.AddModel(t->model, pt, &e);
            if (s.target > 0) {
                if (auto* mt = scene.Model(s.target)) rt = std::max(4.0, mt->Def()->Radius());
                s.dist = s.isPlayer ? R::PlayerTargetDistance(s.wtype, rc, rt) : std::clamp(rc + rt + 22.0, 40.0, 600.0);
                if (std::fabs(s.dist - 60) > 0.5) {   // re-place (Scene::Reset returns models to their placement)
                    scene.RemoveModel(s.target);
                    pt.pos = R::V3(0, 0, -s.dist);
                    s.target = scene.AddModel(t->model, pt, &e);
                }
                s.targetCode = t->code;
                s.targetModel = t->model;
            }
        }
    }
    for (const auto& w : cache.TakeWarnings()) s.notes.push_back(Localize(w, kRenderTexts));

    // --- GUI PreviewSkill
    const SP::SkillInfo si = d.ToSkillInfo(*cs);
    s.key = d.KeyOf(*cs);
    SP::PlayOptions po;
    if (s.isPlayer) po.casterCi = body->code;   // the body's characterInfo (players have one: size, sound object)
    else if (body) po.casterCi = body->orgCode.empty() || d.db.CharInfo(body->code) ? body->code : body->orgCode;
    po.weaponGroup = s.wgroup;
    po.targetCi = s.targetCode;
    po.targetDistance = s.dist;
    po.seed = s.seed;
    s.casterInfo = d.ModelInfo(model);
    s.targetInfo = s.targetModel.empty() ? nullptr : d.ModelInfo(s.targetModel);
    s.tl = SP::BuildTimeline(d.db, s.key, si, s.casterInfo.get(), s.targetInfo.get(), po);
    if (!s.tl.ok) s.notes.push_back(pk2::Lf("b2_note_nothing", s.key));
    for (const auto& n : s.tl.notes)
        if (n.sev <= 1) s.notes.push_back(NoteText(n));
    // glue: the timeline's row pointers point into db's FxDb; the player never reads them, so the text is taken
    // now and the pointers dropped - the session does not depend on db staying alive
    s.dump = DumpText(s.tl);
    s.tl.rows.clear();
    s.tl.aniSet = nullptr;
    s.host = std::make_unique<R::ScenePlayHost>(scene, cache);
    s.host->SetActor(0, s.caster > 0 ? s.caster : -1);
    s.host->SetActor(1, s.target > 0 ? s.target : -1);
    s.host->targetDistance = s.dist;
    s.host->idleGroup[0] = s.wgroup;
    s.host->onSound = [&s](const std::wstring& p, int vol, int) {
        if (s.onSound) s.onSound(AppPath(p), vol);
    };
    s.player.Start(s.tl, *s.host);   // GUI StartPlayer
    s.now = 0;
    s.running = true;
    s.restartAt = -1;
    return true;
}

void TickSkill(SkillSession& s, R::Scene& scene, double dt) {
    if (!s.running || !s.host) return;
    s.host->Update();
    s.now += dt;
    if (s.restartAt >= 0) {
        if (s.now >= s.restartAt) {
            s.player.Stop();
            s.host->ReleaseAll();
            scene.Reset();
            ++s.seed;
            s.player.Start(s.tl, *s.host);
            s.now = 0;
            s.restartAt = -1;
        }
    } else if (!s.player.Update(s.now)) {
        if (s.loop) s.restartAt = s.now + 700;
        else s.running = false;
    }
}

void StopSkill(SkillSession& s) {
    s.player.Stop();
    if (s.host) s.host->ReleaseAll();
    s.running = false;
    s.restartAt = -1;
}

void FrameSkill(SkillSession& s, R::Scene& scene, R::PreviewView& view) {
    if (s.isPlayer && s.caster > 0 && view.Hwnd()) {
        RECT r;
        GetClientRect(view.Hwnd(), &r);
        R::PlayerFrameOptions fo;
        fo.aspect = r.bottom > 0 ? static_cast<double>(r.right) / r.bottom : 4.0 / 3;
        fo.weaponType = s.wtype;   // V6FrameFill
        fo.mastery = s.mastery;
        fo.clips = R::FrameClipsOf(s.tl);
        const auto f = R::FramePlayer(scene, s.caster, s.target, fo);
        view.SetCamera(f.at, f.yawDeg, f.pitchDeg, f.dist);
        return;
    }
    view.FrameModels();
}

std::vector<std::wstring> SkillOverlay(const SkillSession& s, const R::Scene& scene) {
    std::vector<std::wstring> l;
    l.push_back(s.title + L"  ·  " + s.key);
    l.push_back(pk2::Lf("b2_sk_time", std::min(s.now, s.tl.totalMs), s.tl.totalMs, scene.EffectCount(), scene.ParticleCount()));
    return l;
}

}  // namespace glue

}  // namespace bak2
