// SkillEffect Studio v5 - render/PlayerDress.h
// Player-character skill preview that looks like the game:
//   * a full, coherent armour set of the body's race and gender (CH / EU, man / woman), resolved like the
//     viewer's resolveArmorSets: per slot the best EXISTING model (plain or _set_ look, never a GM _super mix,
//     item\common\ fallback when item\china\ is missing), then one variant tag for the whole set;
//   * a shield on the off hand when the held weapon allows one (CH sword / blade, EU one-hand sword / dark staff /
//     cleric rod), a weapon of the same degree suggested so it is readable at preview size;
//   * a default camera that frames the body, the weapon and the effect area (the target as far as it fits while
//     the character keeps about half of the view height).
// Pure data + scene helpers: no client / GUI types (the caller adapts its catalog rows into DressItem).
// Body parts are never hidden and worn pieces stay opaque (RModel material rules); nothing here writes files.
#pragma once

#include "RScene.h"

#include <functional>
#include <string>
#include <vector>

namespace se::sp {
struct Timeline;
}

namespace se::render {

// one itemdata row as the caller's catalog has it (only TypeID1 3 / TypeID2 1 rows are kept)
struct DressItem {
    int id = 0;
    std::wstring code;      // CodeName128 (ITEM_CH_M_HEAVY_08_BA_A, ITEM_EU_SHIELD_08_A ...)
    int type[4]{};          // TypeID1..4
    int level = 0;          // ReqLevel1
    bool service = true;
    std::wstring model;     // Data inner path of the worn / held .bsr ("res\\item\\...\\x.bsr")
};

// CH garment / protector / armour = EU robe / light / heavy (itemdata TypeID3 1/2/3 and 9/10/11)
enum class ArmorClass : int { Auto = -1, None = 0, Garment = 1, Protector = 2, Armor = 3 };

struct DressRequest {
    std::wstring body;          // body code: CHAR_CH_MAN_* / CHAR_CH_WOMAN_* / CHAR_EU_MAN_* / CHAR_EU_WOMAN_*
    std::wstring bodyModel;     // optional .bsr path (gender / race fallback: chinawoman_..., europeman_...)
    int weaponType = 0;         // TypeID4 of the held weapon (2 sword ... 15 cleric rod); 0 = none
    int mastery = 0;            // optional skill mastery (257 ... 518): picks the armour class of that build
    ArmorClass cls = ArmorClass::Auto;
    int degree = 0;             // 0 = automatic (kPreferDegree, else the nearest complete set)
    bool shield = true;         // off-hand shield when the weapon allows one
};

constexpr int kPreferDegree = 8;   // the automatic outfit degree (complete in every stock client)

struct DressPiece {
    int slot = 0;               // 1 head, 2 shoulders, 3 chest, 4 legs, 5 hands, 6 feet, 7 shield, 8 weapon
    std::wstring code, model;   // model "" = none
    std::wstring model2;        // weapons: the off-hand file of a _r / _l pair (daggers, axes)
};

struct DressPlan {
    int race = 0, gender = 0;               // 0 Chinese / 1 European; 0 man / 1 woman
    ArmorClass cls = ArmorClass::None;
    int degree = 0;
    std::wstring variant;                   // chosen look tag ("" plain, "_set_a" ...)
    std::vector<DressPiece> armor;          // existing pieces, head .. feet
    int missingSlots = 0;                   // slots of the set without an existing file
    bool shieldAllowed = false;
    DressPiece shield;                      // model "" = none (two-hand / dual / bow, or no shield model)
    DressPiece weapon;                      // suggested held weapon of the outfit degree (model "" = none)
    std::wstring note;                      // one line for a log (English)
};

using DressExistsFn = std::function<bool(const std::wstring& dataPath)>;

int BodyRace(const std::wstring& code, const std::wstring& model = L"");      // 0 CH, 1 EU
int BodyGender(const std::wstring& code, const std::wstring& model = L"");    // 0 man, 1 woman
bool ShieldAllowed(int race, int weaponType);                                 // the game's one-hand rule
ArmorClass ArmorClassFor(int race, int weaponType, int mastery);              // build look of a weapon / mastery
const wchar_t* WeaponAniGroup(int weaponType);                                // "sword", "onehand_sword" ... ("" unknown)
const wchar_t* ArmorClassName(int race, ArmorClass c);                        // "heavy" / "light" / "clothes"
// v6: the other gender's body code of a player character (CHAR_CH_MAN_X <-> CHAR_CH_WOMAN_X, EU the same, case
// kept); "" when the code is not a CHAR_CH / CHAR_EU man / woman body
std::wstring OtherGenderCode(const std::wstring& code);

class DressIndex {
public:
    void Clear();
    void Add(const DressItem& it);          // ignores rows that are not armour / shield / weapon
    template <class Range, class Fn>
    void AddAll(const Range& rows, Fn&& toItem) {   // toItem(row, DressItem&) -> bool
        for (const auto& r : rows) {
            DressItem d;
            if (toItem(r, d)) Add(d);
        }
    }
    std::size_t Size() const { return m_armor.size() + m_shields.size() + m_weapons.size(); }
    // the outfit for a body + weapon; exists(dataPath) checks model files (nullptr = trust the table)
    DressPlan Plan(const DressRequest& rq, const DressExistsFn& exists) const;
    // degrees that have at least one existing piece for (race, gender, class), ascending
    std::vector<int> Degrees(int race, int gender, ArmorClass c, const DressExistsFn& exists) const;

private:
    struct Entry {
        DressItem it;
        int race = -1, gender = -1, slot = 0, degree = 0;
        ArmorClass cls = ArmorClass::None;
    };
    std::vector<Entry> m_armor, m_shields, m_weapons;
};

// attaches the plan's armour pieces and shield (and the suggested weapon when withWeapon) to a model instance.
// Returns the number of files attached; failures go to log (one line each). Call before the first Play / Pose or
// accept the re-pose that Attach does.
int ApplyDress(ModelInstance& m, AssetCache& cache, const DressPlan& p, std::wstring* log = nullptr, bool withWeapon = false);

// held items (shield / weapon) after an Attach, parts from firstPart on; returns the parts changed:
//  * attach: a held .bsr whose mesh bones only matched NON-biped body bones (woman / robe skeletons have Bone01..
//    for skirt and hair, the same names as the item skeleton) is made rigid on its attach bone - else the weapon or
//    shield follows a skirt bone and lies at the feet (every female character in v3.1);
//  * material: textures under item\...\weapon|shield reuse the alpha as a sheen mask over a transparent backdrop; an
//    alpha test at 0.5 erodes them into shards. Like the viewer (sroviewer-weapon-opaque) the backdrop is cut by
//    connected components (transparent / dark half-transparent blobs touching the border or large interior panels)
//    and every other texel becomes opaque.
// ApplyDress calls it for the shield (and its weapon); call it after a weapon Attach (then Advance(0) to re-pose).
int FixHeldParts(ModelInstance& m, std::size_t firstPart = 0);

// ------------------------------------------------------------------ default camera for player skill previews
struct ShotFrame {
    V3 at;
    double yawDeg = 215, pitchDeg = 14, dist = 60;
};

// v6: a clip of the skill whose poses must stay in frame (READY / WAIT / SHOT of the timeline)
struct FrameClip {
    int role = -1;                // ResourceAnimationType id (ModelInstance::Play)
    std::wstring group;           // aniGroup ("" = default / first)
    double ms = 0;                // the part of the clip that plays (<= 0 = the whole clip)
};

struct PlayerFrameOptions {
    double aspect = 4.0 / 3.0;    // view width / height
    double fovDeg = 50;           // vertical (RCamera::fovDeg)
    double yawDeg = -1;           // orbit yaw; < 0 = automatic: a front quarter (30..65 deg off the caster's facing)
                                  // of the weapon hand (preferred) or of the shield arm; the yaw whose fit keeps the
                                  // caster big, shows most of the target and lets the target hide least of the body
    double pitchDeg = 12;
    double minCasterFrac = 0.48;  // the caster's height on screen (fraction of the view height) at least
    double maxCasterFrac = 0.80;
    double margin = 0.06;         // NDC margin kept free around the caster
    double fxPad = 3.0;           // world units around the caster's box for weapon trails / auras
    double weaponKeep = 0.70;     // part of a held weapon's reach beyond the body that must stay in frame
    double weaponReachMax = 0.75; // ... at most this many body heights (long spears may lose the far tip)
    // v6 (b): whole target in frame for ranged / magic skills
    int fitTarget = -1;           // -1 auto (FitTargetFor(weaponType, mastery)), 0 off (v5: as much as fits), 1 on
    int weaponType = 0;           // TypeID4 of the held weapon (fitTarget auto)
    int mastery = 0;              // skill mastery (fitTarget auto: CH elements / force, EU casters)
    double minCasterFracFit = 0.30;   // fitTarget: the body at least this part of the view height
    double targetMargin = 0.03;   // fitTarget: NDC margin kept free around the target
    // v6 (b): poses over the skill (READY + WAIT + SHOT): the union box of `samples` poses spread over these clips
    // stays in frame (deep crouch / jump never cut, the camera is fixed for the whole skill). Empty = current pose.
    std::vector<FrameClip> clips;
    int samples = 8;
};

// ranged (bow, crossbow, harp) / magic (staffs, rods; CH elements + force, EU wizard / warlock / bard / cleric)
bool FitTargetFor(int weaponType, int mastery);

// READY / WAIT / SHOT animation segments of a skill timeline (se::sp::BuildTimeline) as frame clips
std::vector<FrameClip> FrameClipsOf(const se::sp::Timeline& tl);

// fits body + weapon (+ effect pad) fully and then as much of the target as possible at the chosen yaw / pitch
// (fitTarget: the whole target, the body >= minCasterFracFit). Caster and target are model ids of the scene
// (target <= 0 = none). The scene's models are not changed (clip poses are sampled on a copy of the caster).
ShotFrame FramePlayer(Scene& sc, int caster, int target, const PlayerFrameOptions& o = {});

// what a frame shows (tests / GUI notes): projected with the frame's camera at aspect / fov
struct FrameReport {
    double bodyFrac = 0;          // body (bones) height on screen / view height, current pose
    double targetIn = 0;          // part of the target's 8 box corners inside the view (1 = whole box)
    bool hasTarget = false;
    std::vector<double> sampleBodyIn;   // per clip sample: part of the body box corners inside the view
    std::vector<double> sampleAllIn;    // per clip sample: part of the full box (weapon included) corners inside
    std::vector<double> samplePelvisY;  // per clip sample: pelvis height above the feet (world units)
    double standPelvisY = 0;      // the current pose's pelvis height
};
FrameReport MeasurePlayerFrame(Scene& sc, int caster, int target, const ShotFrame& f, const PlayerFrameOptions& o = {});
double AutoPlayerYaw(Scene& sc, int caster);      // the automatic yaw alone (degrees)
void ApplyFrame(Scene& sc, const ShotFrame& f);   // sets sc.camera (eye / at); PreviewView: SetCamera(f...)

// a target distance for player previews: melee weapons stand close (game reach), ranged / magic a little further;
// short enough that the hit area fits next to a caster at preview size (v3.1: clamp(rc + rt + 22, 40, 600))
double PlayerTargetDistance(int weaponType, double casterRadius, double targetRadius);

}  // namespace se::render
