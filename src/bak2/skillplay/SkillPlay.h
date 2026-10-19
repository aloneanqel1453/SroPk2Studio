// SkillEffect Studio - skillplay/SkillPlay.h
// Skill playback driver: skilleffect.txt rows (characterInfo / skillaniset2 / skilleffectset) + skilldata timing +
// the caster's .bsr clips -> a timeline (READY / WAIT / SHOT phases, StartEvent = N-th type-1 event key of the
// phase clip, bone attach, AT_* placement, MOV_* projectiles to a visible target, CreateCnt, hits with DamageEfp /
// blood, SetHold loops, effectsound + SndBegin/SndEnd sounds, camera shake) and a runtime player that drives any
// renderer through IPlayHost. "Pending rows": rows not yet written (list text) are overlaid on the client's rows with
// the client's own rules, so a list can be previewed before it is merged.
// Specs: TERA\_re\skills\SKILLEFFECT_LOGIC.md (§3 handlers, §5 columns), SRO_SKILLS.md §5.4-5.9,
//        TERA\_re\efp\EFP_RUNTIME.md §9-10 (SetHold, bone attach, projectile -Z). [U] = unverified client detail.
// Math: D3D row vectors (p' = p * M, translation in row 3), same layout as core/Res.h V3/M4. Units: SRO world units,
// milliseconds. Actors: 0 = caster, 1 = target. The caster faces -Z; the preview target stands at (0,0,-distance).
#pragma once

#include "../core/Text.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace se::sp {

// ------------------------------------------------------------------------------------------------------- math
struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(double a, double b, double c) : x(a), y(b), z(c) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    double Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 Cross(const Vec3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double Len() const;
    Vec3 Norm() const;
    bool Zero() const { return x == 0 && y == 0 && z == 0; }
};
struct Mat4 {
    double m[4][4]{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    Mat4 operator*(const Mat4& b) const;  // (a * b) applies a first, then b
    Vec3 T() const { return {m[3][0], m[3][1], m[3][2]}; }
    void SetT(const Vec3& t) {
        m[3][0] = t.x;
        m[3][1] = t.y;
        m[3][2] = t.z;
    }
    Mat4 RotOnly() const;  // translation removed
    static Mat4 Translation(const Vec3& t);
    static Mat4 RotY(double rad);
    static Mat4 Scale(double s);
    // local -Z points along dir (projectile flight, EFP_RUNTIME §10), y up as far as possible
    static Mat4 LookAlongNegZ(const Vec3& dir);
};
Vec3 XfPoint(const Vec3& v, const Mat4& M);
Vec3 XfDir(const Vec3& v, const Mat4& M);

// ------------------------------------------------------------------------------------------------- client enums
// values = sro_client.exe enum tables (out_enum_tables.txt); -1 = not in the table (client: row inert / default)
enum class Phase : int { Ready = 0, Wait, Shot, ActOS, ActOL, ActOE, ActS, ActL, Deact, SReturn, Bad = -1 };
enum class Act : int {
    Stop = 0, OneFollow, Loop, Target, TargetF, Source, DmgPos, Mov1Tar, MovOption, MovApiece, MovSplash, MovPierce, Bad = -1
};
enum class Mov : int { None = 0, Straight, Up, Upr, Round, Hwan, Bad = -1 };
enum class ScaleBase : int { None = 0, Char = 1, Mob = 2, Bad = -1 };
enum class ObjKind : int { None = 0, Bsr = 1, Efp = 2, Wav = 3, Ddj = 4 };
enum DmgKind : int { kNor = 1, kCri = 2, kHwan = 4 };
enum Sct : int {
    kSctArrow = 1, kSctRut = 2, kSctMover = 4, kSctShake0 = 8, kSctShake1 = 16, kSctShakeMov0 = 32, kSctShakeMov1 = 64,
    kSctShake2 = 128, kSctShake3 = 256, kSctShakeMov2 = 512, kSctShakeMov3 = 1024, kSctMat = 2048, kSctCharScale = 4096,
    kSctEffectScale = 8192
};
const wchar_t* PhaseName(Phase p);   // READY ... S_RETURN, "?" for Bad
const wchar_t* ActName(Act a);       // AT_STOP ...
const wchar_t* MovName(Mov m);       // MOV_NONE ...
Phase PhaseFrom(std::wstring_view s);
Act ActFrom(std::wstring_view s);
Mov MovFrom(std::wstring_view s);
// ANI_* token -> ResourceAnimationType role id (ANI_ATTACK1 = 2 ...; "none" / unknown = -1)
int AniRole(std::wstring_view token);
std::wstring AniTokenOf(int role);  // inverse (empty when not a named role)

// ----------------------------------------------------------------------------------------------------- rows
enum class Origin : int { Client = 0, Pending = 1 };

struct CharInfoRow {  // characterInfo (13 cells)
    Origin origin = Origin::Client;
    int line = 0;               // 1-based physical line (client file) or list line (pending)
    std::wstring key;           // CodeName128 or PREFIX###~###
    std::wstring soundObject;   // col 1: effectsound object
    double size = 1;            // col 2 * 0.5 (MOB_BASE scale) [D]
    int rideType = 0;
    std::wstring ride, dieBsr, dieEffect, damageBone;
    Vec3 damagePos;             // col 8: hit effects / numbers
    std::wstring bloodType;     // col 9: hiteffect\<name>.efp ("none" = no blood)
    int deadEffect = 0, env = 0, rangedOffset = 0;
    std::wstring bgm;           // col 11 "2,file.ogg"
    std::vector<std::wstring> cells;
};

struct AniSetRow {  // skillaniset2 (27 cells)
    Origin origin = Origin::Client;
    int line = 0;
    bool service = true;        // col 0: 0 = the client ignores the row (key goes to the disabled set)
    std::wstring label, key;
    int priority = 0, hideWeapon = 0;
    std::wstring aniGroup;      // DEFAULT / player weapon class
    std::wstring readyTok, waitTok, shotTok;  // ANI_* tokens as written
    int ready = -1, wait = -1;  // role ids (-1 none)
    std::vector<int> shot;      // AniShot role ids (comma list = random pick); empty = none
    std::wstring playTiming, actW, actS, defenseEfp, damageEfp;
    int trailLen = 0, trailColor[4] = {0, 0, 0, 0}, trailOp = 6;
    std::wstring trailTex, startTiming, arrowTail, arrowForce, light, skillObject;
    int waistTwist = 0, isAttack = 0, blood = 1;
    std::vector<std::wstring> cells;
};

struct ActOption {  // col 16 "bool,a,b,c,d,bool" [U]: AT_MOV_OPTION radial angle (deg), radius?, count?, life ms?
    bool on = false;
    double angle = 0, b = 0, c = 0, ms = 0;
    bool flag = false;
};

struct EffectRow {  // skilleffectset (28 cells)
    Origin origin = Origin::Client;
    int line = 0;
    std::wstring label, key;
    Phase phase = Phase::Bad;
    int startEvent = 0;
    int dmg = 0;                // 1 TRUE, 0 FALSE, -1 not in the table (FLASE typo) [D]
    int damageTypes = 0;        // kNor | kCri | kHwan
    ScaleBase scale = ScaleBase::None;
    int id = 0, attach = 0, trade = 0, kill = 0, createCnt = 1;
    double fadeIn = 0, fadeOut = 0;
    Act act = Act::Bad;
    Mov mov = Mov::None;
    double movDelay = 0, speedMin = 0, speedMax = 0;
    double param[3] = {0, 0, 0};
    ActOption opt;
    std::wstring objPath, objName;
    ObjKind kind = ObjKind::None;
    std::wstring startBone, targetBone;  // "none" -> empty
    Vec3 startOff, targetOff;
    std::wstring objName2;
    int rotate = 0;
    int script = 0;                      // Sct flags
    std::vector<double> scriptArgs;      // numbers after the flags (SCT_EFFECT_SCALE,2.0 / SCT_MAT,r,g,b,...,ms)
    std::wstring scriptText;
    std::wstring sndBegin, sndEnd;       // relative to Data prim\snd\ ("none" -> empty)
    std::vector<std::wstring> cells;
    std::vector<std::wstring> parseNotes;  // cells that did not parse (client value -1 / 0)

    // archive inner paths: .efp -> Particles, .bsr -> Data (ObjPath "res\..."), .wav -> Data "prim\snd\..."
    std::wstring ResPath() const;
    std::wstring Res2Path() const;  // ObjName2 (Particles, same folder as ObjPath when it has no folder)
    bool Projectile() const;        // flies (MOV_* with speed > 0)
};

CharInfoRow ParseCharInfo(const std::vector<std::wstring>& cells);
AniSetRow ParseAniSet(const std::vector<std::wstring>& cells);
EffectRow ParseEffect(const std::vector<std::wstring>& cells);

// effectsound.txt (CP949, raw TAB cells: 1 object, 2 handle, 3 skill key or '-', 4 event1, 8 folder, 9 file, 10 volume)
struct SoundRow {
    std::wstring object, handle, skill, event1, folder, file;
    int volume = 100;
    int line = 0;
    std::wstring DataPath() const;  // prim\snd\<folder><file>
};

// skilldata (_RefSkill) fields the timeline needs (core/objectdb fills it; ParseSkillRow for raw cells)
struct SkillInfo {
    bool valid = false;
    int id = 0;
    std::wstring code, group;   // Basic_Code (col 3), Basic_Group (col 5, "xxx" = none)
    int original = 0;           // Basic_Original (col 6)
    std::wstring chain;         // Basic_ChainCode (col 9)
    double prepMs = 0, castMs = 0, durMs = 0;  // Action_PreparingTime / CastingTime / ActionDuration (11..13)
    double flySpeed = 0;        // Action_FlyingSpeed (col 16, server units) [U]
    double buffMs = 0;          // ACT_L time: status duration (stns/st/se/... first arg) else "dura"; 0 = unknown
    // Param1..Param50 (cols 68..117), walked with the client grammar (SRO_SKILLS §3.1, tag table = Appendix A)
    int param1 = 0;             // 0 normal, 1 ranged, 3 lasting, 4 passive [D]
    std::map<std::string, std::vector<long long>> params;  // tag -> arguments
    double statusMs = 0, duraMs = 0;
    int hits = 0;               // mc [2, N] -> N
    std::wstring paramError;    // a value the client would not understand as a tag (it loops forever there)
};
SkillInfo ParseSkillRow(const std::vector<std::wstring>& rawCells);  // raw TAB split (empty cells kept)
// the Param walker alone (p = Param1..Param50 values)
void WalkSkillParams(const std::vector<long long>& p, SkillInfo& s);

// ------------------------------------------------------------------------------------------------------ FxDb
// The client's presentation tables + pending rows, read with the client's rules (SKILLEFFECT_LOGIC §3):
// characterInfo last row wins; skillaniset2 last row wins (Service 0 -> disabled); skilleffectset rows accumulate
// per key in file order. Pending rows apply after the client's (as a merge would place them in the active part):
// "#replace KEY" / "#remove KEY" drop the client's rows of KEY first.
class FxDb {
public:
    // skilleffect.txt (UTF-16LE, comment state machine of the client = core SeModel)
    bool LoadSkillEffect(const Bytes& raw, std::wstring& err);
    void LoadEffectSound(const Bytes& raw);  // CP949 (any encoding DecodeText understands)
    // pending rows: list text (#section headers, #replace / #remove, plain rows classified by shape). Returns the
    // number of rows taken; notes get one line per row that could not be used.
    int SetPending(const std::wstring& listText, std::vector<std::wstring>* notes = nullptr);
    void ClearPending();
    bool HasPending() const;

    // effective (client + pending) views
    const AniSetRow* AniSet(const std::wstring& key) const;        // nullptr when none (or removed)
    bool Disabled(const std::wstring& key) const;                  // Service 0 in the effective row
    std::vector<const EffectRow*> Effects(const std::wstring& key) const;  // file order (client rows, then pending)
    const CharInfoRow* CharInfo(const std::wstring& codeName) const;       // exact key, then range keys PREFIX###~###
    std::vector<const SoundRow*> Sounds(const std::wstring& object, const std::wstring& handle,
                                        const std::wstring& skillKey) const;  // random variants
    std::vector<const SoundRow*> SoundsOf(const std::wstring& object, const std::wstring& skillKey) const;
    std::vector<std::wstring> Keys() const;      // every presentation key (skillaniset2 or skilleffectset)
    std::vector<std::wstring> PendingKeys() const;  // keys touched by pending rows
    bool IsPendingKey(const std::wstring& key) const;
    size_t ClientEffectRows() const { return m_seCount; }

    // presentation key of a skill (SRO_SKILLS §5.1): group/code, else through Basic_Original
    std::wstring PresentationKey(const SkillInfo& s, const std::function<const SkillInfo*(int id)>& byId) const;

private:
    struct Set {
        std::map<std::wstring, CharInfoRow> ci;
        std::map<std::wstring, AniSetRow> sa;
        std::map<std::wstring, std::vector<EffectRow>> se;
    };
    Set m_cli, m_pend;
    std::set<std::wstring> m_replace, m_remove;
    std::vector<SoundRow> m_snd;
    std::multimap<std::wstring, size_t> m_sndByObj;  // upper object -> index
    size_t m_seCount = 0;
};

// ---------------------------------------------------------------------------------------------- model info
struct ClipInfo {
    bool found = false;
    int role = -1;
    std::wstring group;           // aniGroup the clip was found in (lower case; "" = unknown)
    std::wstring file;            // .ban (Data inner path)
    double durationMs = 0;        // 0 = unknown
    std::vector<double> events;   // type-1 event key times (ms, ascending) of the .bsr aniGroup entry
};
struct SoundCue {                 // .bsr ModDataSound track of an animation set (handle + time inside the clip)
    std::wstring handle;          // effectsound handle (voc_shout1, snd_swing2 ...)
    std::wstring wav;             // the .bsr's own wav (the effectsound row of the handle is played)
    double ms = 0;
};
// What the timeline needs from a model. BsrModel implements it from .bsr/.ban bytes; the render side can too.
class IModelInfo {
public:
    virtual ~IModelInfo() = default;
    virtual bool Clip(int role, ClipInfo& out) const = 0;  // aniGroup "default" first
    // clip of a role for a skill: the declared aniGroup (skillaniset2, e.g. DEFAULT / SWORD / TWOHAND_STAFF) strictly,
    // then - a DEFAULT skill whose clip is not in "default" - the held weapon's group, then any group
    virtual bool ClipIn(int role, const std::wstring& declared, const std::wstring& weapon, ClipInfo& out) const {
        (void)declared;
        (void)weapon;
        return Clip(role, out);
    }
    virtual bool HasBone(const std::wstring& bone) const {
        (void)bone;
        return true;
    }
    virtual bool BoneRest(const std::wstring& bone, Vec3& modelPos) const {  // bind-pose position (estimates only)
        (void)bone;
        (void)modelPos;
        return false;
    }
    virtual std::vector<SoundCue> SoundCues(int role) const {
        (void)role;
        return {};
    }
    virtual double Height() const { return 18; }  // world units (a human is ~18)
};

// .bsr (JMXVRES 0109) animation list + aniGroup entries with events, .ban durations through a reader (Data paths).
class BsrModel : public IModelInfo {
public:
    using Reader = std::function<bool(const std::wstring& dataPath, Bytes& out)>;
    bool Load(const Bytes& bsr, const Reader& read, std::wstring& err);
    bool Clip(int role, ClipInfo& out) const override;
    bool ClipIn(int role, const std::wstring& declared, const std::wstring& weapon, ClipInfo& out) const override;
    std::vector<SoundCue> SoundCues(int role) const override;   // mod-palette sound tracks of the animation set
    bool HasBone(const std::wstring& bone) const override;
    bool BoneRest(const std::wstring& bone, Vec3& modelPos) const override;  // bind pose from the .bsk
    double Height() const override { return m_height; }
    void SetHeight(double h) { m_height = h; }
    struct Entry {
        std::wstring group;  // lower case
        int role = 0, file = -1;
        std::vector<double> events;  // type 1 only, ascending
        int eventsAll = 0;           // every type
    };
    std::vector<std::wstring> anis;   // .ban paths
    std::vector<double> aniMs;        // durations (0 = unread)
    std::vector<Entry> entries;
    struct Cue {
        int role = -1;                // the set's anim_type (ResourceAnimationType)
        SoundCue cue;
    };
    std::vector<Cue> cues;            // mod palette ModDataSound tracks
    std::wstring skeleton;
    std::set<std::wstring> bones;     // upper-case names from the .bsk (empty = unknown -> HasBone true)

private:
    double m_height = 18;
    std::map<std::wstring, Vec3> m_rest;  // upper bone name -> model-space bind position
};
double BanDurationMs(const Bytes& ban);                      // JMXVBAN header duration (0 = not a .ban)
std::vector<std::wstring> BskBoneNames(const Bytes& bsk);   // JMXVBSK bone names (empty = not parsable)

// --------------------------------------------------------------------------------------------------- timeline
struct PlayOptions {
    std::uint32_t seed = 1;          // random picks (comma AniShot, effectsound variants, speed ranges)
    int shotPick = -1;               // index into a comma AniShot list (-1 = random by seed)
    int hitKind = kNor;              // which damage kind the preview shows (rows with other kinds are skipped)
    double targetDistance = 60;      // caster -> target (world units) for flight estimates
    double buffMs = 3000;            // ACT_L duration when the skill gives none
    double volleyMs = 100;           // CreateCnt copies interval [U]
    double idleTailMs = 600;         // stand after the skill (timeline length)
    double casterScale = 1;          // CHAR_BASE scale (characterdata scale)
    bool playDisabled = true;        // Service 0 rows still previewed (noted)
    bool clampEvents = true;         // StartEvent beyond the clip's events -> last event (noted) else row skipped
    bool partyOnCaster = true;       // ACT_OS/OL/OE (other party members) shown on the caster
    bool effectSounds = true;        // effectsound.txt handles (VOC_SHOUT / SND_SWING / SND_DMG)
    std::wstring casterCi, targetCi; // characterInfo keys (Size, DamagePos, BloodType, sound object)
    std::wstring weaponGroup;        // players: aniGroup of the held weapon (lower case: sword, spear, bow, twohand_staff ...)
};

enum class Place : int {
    Follow = 0,     // attached to an actor bone every frame (AT_ONE_FOLLOW / AT_LOOP)
    Fixed,          // created at an actor bone, then stays in the world (AT_STOP / AT_SOURCE)
    Target,         // at the target (bone + offset) at creation (AT_TARGET)
    TargetFollow,   // follows the target (AT_TARGET_F)
    DmgPos,         // target damage position (characterInfo DamagePos) (AT_DMG_POS)
    Projectile      // flies (AT_MOV_* / MOV_*)
};
const wchar_t* PlaceName(Place p);

struct FxEvent {
    double t = 0;                // ms from the skill start (creation)
    double holdUntil = -1;       // SetHold(1) until this time (AT_LOOP / ACT_L), -1 = no hold
    double releaseAt = -1;       // forced Release (-1 = when the effect ends by itself)
    int row = -1;                // index into Timeline::rows
    int copy = 0;                // CreateCnt index
    ObjKind kind = ObjKind::None;
    std::wstring res;            // archive inner path (Particles for efp, Data for bsr)
    Place place = Place::Follow;
    int actor = 0;               // anchor actor (0 caster, 1 target)
    std::wstring bone;           // empty = actor root
    Vec3 offset;
    double scale = 1;
    double fadeIn = 0, fadeOut = 0;
    int instId = 0;                // skilleffectset ID (col 7): Trade / Kill of later rows address it
    // projectile
    Mov mov = Mov::None;
    int fromActor = 0, toActor = 1;
    std::wstring fromBone, toBone;
    Vec3 fromOff, toOff;
    bool fromTargetSpace = false;  // AT_TARGET falling objects: both ends relative to the target
    double speed = 0;              // units / s
    double flightMs = 0;           // estimate (timeline geometry); the player recomputes from real positions
    double arc = 0;                // MOV_UPR / MOV_ROUND sine height factor (Param[0] / 1000 of the distance) [U]
    double arcRollDeg = 0;         // arc plane roll [U]
    double angleDeg = 0;           // AT_MOV_OPTION radial direction [U]
    double overshoot = 0;          // MOV_PIERCE / AT_MOV_PIERCE: fly past the target (fraction of the distance)
    bool homing = false;           // AT_MOV_1TAR follows the moving target
    std::wstring arrive;           // ObjName2 at the arrival (Particles)
    bool dmg = false;              // shows the damage (at t, or at the arrival for projectiles)
    std::wstring note;             // [U] / estimate remarks
};

struct AnimSeg {
    Phase phase = Phase::Shot;
    int role = -1;                 // -1 = idle / stand
    std::wstring token;
    std::wstring group;            // aniGroup of the clip (players: the weapon class; "" = default / first)
    double start = 0, end = 0;
    bool loop = false;
    bool found = false;            // the model has this role
    double clipMs = 0;
    std::wstring file;             // .ban (Data) when known
};

// skilleffectset ID / Trade / Kill (cols 7, 9, 10) [D, census]: a row with ID n names the instances it creates;
// a later row with Trade n (65 / 66 shipped: the flying arrow replaces the nocked one) or Kill n removes them when it
// fires. The timeline turns these into FxEvent::releaseAt; KillEvent keeps the list for display.
struct KillEvent {
    double t = 0;
    int id = 0;
    int row = -1;
    bool trade = false;
};

enum class ResArc : int { Media = 0, Data = 1, Particles = 2 };  // = core se::Arc values (render's Arc differs!)
struct ResRef {                    // a file the skill needs
    ResArc arc = ResArc::Particles;
    std::wstring path;             // inner path
    std::wstring what;             // "row 3 efp", "DamageEfp", "SndBegin", "clip ANI_ATTACK1" ...
    int row = -1;
};

struct HitEvent {
    double t = 0;
    int index = 0;                 // 0-based hit number
    int row = -1;
    int fx = -1;                   // projectile FxEvent index: the hit happens at its arrival (player)
    int kinds = kNor;
    std::wstring damageEfp;        // skillaniset2 DamageEfp (Particles)
    std::wstring bloodEfp;         // hiteffect\<BloodType>.efp of the target (skillaniset2 Blood = 1)
    Vec3 damagePos;                // target local (characterInfo DamagePos)
    bool estimated = false;        // no DMG Event TRUE row: shown at the attack event as an estimate [U] ("tahmini")
};

struct SoundEvent {
    double t = 0;
    std::wstring path;             // Data inner path (prim\snd\...)
    std::wstring why;              // "SndBegin row 3", "VOC_SHOUT1", ...
    int actor = 0;
    int volume = 100;
    int fx = -1;                   // SndEnd of a projectile: at its arrival
    bool approx = false;           // effectsound timing rule, not a clip sound track [U]
};

struct CamEvent {
    double t = 0;
    int level = 0;                 // SHAKECAM0..3
    bool moving = false;           // SHAKECAM_MOV*
    int row = -1;
    int fx = -1;                   // projectile row: at the arrival
};

struct MatEvent {                  // SCT_MAT,r,g,b,r2,g2,b2,ms: material tint of the affected actor [U]
    double t = 0, ms = 0;
    int actor = 0;
    double rgb[6] = {0, 0, 0, 0, 0, 0};
    int row = -1;
};

struct TlNote {
    int sev = 2;                   // 0 error, 1 warning, 2 info
    std::wstring tr, en;
    int row = -1;
};

struct Timeline {
    std::wstring key;
    bool ok = false;               // something to play
    std::vector<const EffectRow*> rows;  // the key's effective rows (pointers into the FxDb)
    std::vector<bool> rowUsed;     // the row produced an effect / sound / hit
    const AniSetRow* aniSet = nullptr;
    SkillInfo skill;
    double prepMs = 0, shotStart = 0, shotEnd = 0, buffStart = -1, buffEnd = -1, totalMs = 0;
    int shotRole = -1;
    std::vector<AnimSeg> anims;
    std::vector<FxEvent> fx;
    std::vector<HitEvent> hits;
    std::vector<SoundEvent> sounds;
    std::vector<CamEvent> cams;
    std::vector<MatEvent> mats;
    std::vector<TlNote> notes;
    std::vector<double> shotEvents;  // the SHOT clip's type-1 events (absolute ms)
    bool pendingRows = false;        // some rows come from the pending list
    std::vector<KillEvent> kills;
    std::wstring Dump() const;       // readable text (tests, CLI, GUI details)
    std::vector<ResRef> Resources() const;  // unique files (effects, arrival / hit / blood efp, sounds, clips)
};

// Builds the timeline of one presentation key. skill may be invalid (timing then from the clip only).
// plain-language description of a row (GUI row details / tooltips), Turkish or English
std::wstring DescribeRow(const EffectRow& r, bool english);
// v6 P4 (Basit / simple view): the same row in everyday words for beginners. No code names (no ActType / MovType
// tokens, no section names), bones and effect / sound file names in plain words ("sağ el", "lanet isabeti"); what is
// guessed (unknown phase, unknown file-name words, unread cells) is marked "(tahmini)" / "(estimated)".
// Example: "2. vuruş anında hedefin üstünde efekt: lanet isabeti; hasar burada görünür".
std::wstring DescribeRowPlain(const EffectRow& r, bool english);
// plain words of an effect / sound file name ("skill_tigerwoman_curse_hit.efp" -> "lanet isabeti" / "curse hit");
// estimated is set when no word of the name is known (the name is then shown as it is + "(tahmini)")
std::wstring PlainFileName(const std::wstring& file, bool english, bool* estimated = nullptr);
// a caster / target bone in plain words ("Bip01 R Hand" -> "sağ el"); "" = the body
std::wstring PlainBoneName(const std::wstring& bone, bool english, bool target);

Timeline BuildTimeline(const FxDb& db, const std::wstring& key, const SkillInfo& skill, const IModelInfo* caster,
                       const IModelInfo* target, const PlayOptions& opt);

// --------------------------------------------------------------------------------------------------- player
using FxId = std::uint64_t;  // 0 = none

struct FxSpawn {
    ObjKind kind = ObjKind::Efp;
    std::wstring res;     // archive inner path (efp: Particles, bsr: Data)
    double scale = 1;     // character scale x row scale (SetScale)
    bool hold = false;    // SetHold(1) right after creation
    int row = -1;         // timeline row (for highlighting)
};

// The renderer side (wired by the integrator to the render API).
class IPlayHost {
public:
    virtual ~IPlayHost() = default;
    virtual void PlayClip(int actor, int role, bool loop, double startMs) = 0;  // role -1 = idle
    virtual void PlayClipIn(int actor, int role, const std::wstring& group, bool loop, double startMs) {  // in an aniGroup
        (void)group;
        PlayClip(actor, role, loop, startMs);
    }
    virtual Mat4 ActorWorld(int actor) = 0;                                    // rotation + translation
    virtual bool BoneWorld(int actor, const std::wstring& bone, Mat4& out) = 0;  // false = no such bone
    virtual FxId Spawn(const FxSpawn& s) = 0;                                   // 0 = could not load
    virtual void SetWorld(FxId id, const Mat4& m) = 0;
    virtual void SetHold(FxId id, bool hold) = 0;
    virtual void SetAlpha(FxId id, double a) {
        (void)id;
        (void)a;
    }
    virtual bool Alive(FxId id) = 0;  // false once the effect finished by itself
    virtual void Release(FxId id) = 0;
    virtual void Sound(const std::wstring& dataPath, int volume, int actor) {
        (void)dataPath;
        (void)volume;
        (void)actor;
    }
    virtual void Shake(int level, bool moving) {
        (void)level;
        (void)moving;
    }
    virtual void Tint(int actor, const double rgb[6], double ms) {
        (void)actor;
        (void)rgb;
        (void)ms;
    }
    virtual void Damage(int actor, int hitIndex, int kinds) {  // damage number / hit reaction
        (void)actor;
        (void)hitIndex;
        (void)kinds;
    }
};

class SkillPlayer {
public:
    // geometry: caster at the origin facing -Z, target at (0,0,-distance) unless the host moves the actors
    void Start(const Timeline& tl, IPlayHost& host);
    // nowMs = time since Start; call every frame (any rate). Returns false once everything has finished.
    bool Update(double nowMs);
    void Stop();  // releases every live effect
    bool Running() const { return m_host != nullptr; }
    double Now() const { return m_now; }
    int LiveEffects() const;
    double releaseCapMs = 6000;          // a non-hold effect still alive this long after the skill end is released
    bool releaseProjectileOnArrival = true;

private:
    struct Live {
        int ev = -1;
        FxId id = 0;
        bool holding = false;
        double born = 0;
        double freeAt = 0;     // creation, or the end of its hold: the release cap counts from here
        Mat4 fixed;            // Fixed / Target / DmgPos placement captured at creation
        bool hasFixed = false;
        // projectile state
        Vec3 from, to;
        double flight = 0;
        bool arrived = false;
    };
    void SpawnEvent(int idx);
    Mat4 Anchor(int actor, const std::wstring& bone, const Vec3& off, bool rotWithBone) const;
    Vec3 ProjectilePos(const Live& l, const FxEvent& e, double u) const;
    void Arrive(Live& l);
    const Timeline* m_tl = nullptr;
    IPlayHost* m_host = nullptr;
    double m_now = 0;
    std::vector<char> m_fxDone, m_hitDone, m_sndDone, m_camDone, m_matDone, m_animDone;
    std::vector<Live> m_live;
};

// Records every host call as text (tests, CLI "skillplay dump", GUI timeline log). Effects live lifeMs.
class RecordingHost : public IPlayHost {
public:
    double now = 0;
    double lifeMs = 1500;
    double distance = 60;
    std::vector<std::wstring> log;
    std::set<std::wstring> missingBones;  // BoneWorld returns false for these
    void PlayClip(int actor, int role, bool loop, double startMs) override;
    Mat4 ActorWorld(int actor) override;
    bool BoneWorld(int actor, const std::wstring& bone, Mat4& out) override;
    FxId Spawn(const FxSpawn& s) override;
    void SetWorld(FxId id, const Mat4& m) override;
    void SetHold(FxId id, bool hold) override;
    bool Alive(FxId id) override;
    void Release(FxId id) override;
    void Sound(const std::wstring& dataPath, int volume, int actor) override;
    void Shake(int level, bool moving) override;
    void Tint(int actor, const double rgb[6], double ms) override;
    void Damage(int actor, int hitIndex, int kinds) override;
    struct Fx {
        FxSpawn s;
        double born = 0;
        bool hold = false, released = false;
        Mat4 world;
        int moves = 0;
    };
    std::map<FxId, Fx> fx;
    int spawns = 0, releases = 0, sounds = 0, damages = 0, shakes = 0;

private:
    FxId m_next = 1;
};

}  // namespace se::sp
