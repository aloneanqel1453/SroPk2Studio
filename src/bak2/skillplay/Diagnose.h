// SkillEffect Studio - skillplay/Diagnose.h
// "Efekt neden görünmüyor?" doctor (M1) and the pending-row model check (M6).
//
// Diagnose walks the chain the client walks for one skill and stops at the first broken link:
//   enc (skilldata ENC defines the key) -> key (group or code: the key the client looks up) -> comment (the key's rows
//   are outside /* */) -> aniset (skillaniset2 row, Service 1) -> order (skillaniset2 row before the skilleffectset
//   rows, V9) -> clip (AniShot / AniReady / AniWait clip in the .bsr) -> event (StartEvent k <= type-1 events of the
//   clip) -> bone (StartBone / TargetBone in the .bsk) -> files (.efp / .bsr exist) -> efplive (the .efp emits
//   something in its first 2 s, optional callback) -> charinfo (characterInfo row) -> sound (.bsr sound sets <= 15
//   tracks, .wav exist) -> scale (> 0).
// Each step: id, state (ok / fail / skip / n.a.), severity, a plain TR / EN sentence and, when it can be fixed by
// rows, the fix kind and the list text that goes into the pending list (ApplyFix replaces the key's rows there).
// The comment step is checked before aniset: a row the client never reads cannot be judged by its Service cell.
// Nothing here reads files or renders: every input is a callback or a pointer (render-independent, read-only).
// Specs: TERA\_re\skills\SKILLEFFECT_LOGIC.md §2-3, §8 (V2, V7-V9, V13, V14, V17), SRO_SKILLS.md §5.1 (key).
#pragma once

#include "SkillPlay.h"

#include "../core/List.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace se::sp {

// ------------------------------------------------------------------------------------------------ hidden rows
// Rows of skilleffect.txt that the client's comment reader drops (inside /* */), kept with their section and key.
struct HiddenRow {
    Sec sec = Sec::None;
    int line = 0;                     // 1-based physical line
    bool eofBlock = false;            // inside the block that is still open at the end of the file (the EOF bug)
    std::wstring key;
    std::vector<std::wstring> cells;  // client tokens
};
class HiddenRows {
public:
    bool Build(const Bytes& rawSkillEffect, std::wstring& err);  // UTF-16LE skilleffect.txt (same reader as FxDb)
    std::vector<const HiddenRow*> Of(const std::wstring& key) const;
    size_t Count() const { return m_rows.size(); }
    bool EofInBlock() const { return m_eofLine > 0; }
    int EofBlockLine() const { return m_eofLine; }  // 1-based line that opened the unclosed block (0 = none)

private:
    std::vector<HiddenRow> m_rows;
    std::multimap<std::wstring, size_t> m_byKey;
    int m_eofLine = 0;
};

// --------------------------------------------------------------------------------------------- .bsr sound sets
struct SoundSetInfo {
    int animType = -1;   // the palette set's anim_type (ResourceAnimationType role)
    std::wstring set;    // palette set name
    std::wstring name;   // SndSet name
    int tracks = 0;      // nTrackNum: every slot, empty ones included (MAX_SOUND_TRACK_PER_ANI counts these)
};
constexpr int kMaxSoundTrackPerAni = 15;  // client assert
constexpr int kShippedMaxSoundTracks = 11;
// every ModDataSound SndSet of a JMXVRES 0109 .bsr (empty = none / not a .bsr); ok=false when the palette is damaged
std::vector<SoundSetInfo> BsrSoundSets(const Bytes& bsr, bool* ok = nullptr);

// --------------------------------------------------------------------------------------------------- doctor
enum class DiagStep : int { Enc = 0, Key, Comment, AniSet, Order, Clip, Event, Bone, Files, EfpLive, CharInfo, Sound, Scale, Count };
enum class DiagState : int { Ok = 0, Fail, Skip, NotApplicable };  // Skip = the input needed for the check was not given
enum class DiagFix : int {
    None = 0,
    ServiceOn,         // skillaniset2 Service 0 -> 1
    MoveOutOfComment,  // the hidden rows go to the pending list (the merge writes them into the active part)
    Reorder,           // "#remove KEY" + skillaniset2 row + skilleffectset rows (V9)
    ClampEvent,        // StartEvent -> the clip's event count
    ReplaceBone,       // StartBone / TargetBone -> the nearest bone of the skeleton
    Rekey,             // rows under the code key -> the group key the client looks up
    GenSa,             // a minimal skillaniset2 row
    PickClip,          // AniShot -> a clip the model has
    ReplaceFile,       // ObjName / ObjName2 / DamageEfp -> an existing file of the same folder
    AddCharInfo,       // a characterInfo row
    FixScale,          // characterInfo size / SCT_EFFECT_SCALE argument > 0
    Manual             // only by hand (file to add, .bsr sound set to shorten ...)
};
const wchar_t* DiagStepId(DiagStep s);   // "enc", "key", "comment", "aniset", "order", "clip", "event", "bone", ...
std::wstring DiagStepName(DiagStep s, bool english);
const wchar_t* DiagFixId(DiagFix f);     // "service-on", "move-out", ...

struct DiagItem {
    DiagStep step = DiagStep::Enc;
    DiagState state = DiagState::Skip;
    Sev sev = Sev::Info;              // Fail: Error = nothing shows / crash, Warn = shows wrong or partly
    std::wstring rule;                // V2, V7, V8, V9, V13, V14, V17, FILE, EFP, CI, SCALE ...
    std::wstring tr, en;              // one plain sentence
    int row = -1;                     // first affected row (index into the key's effective skilleffectset rows)
    int fileLine = -1, listLine = -1; // where (1-based; -1 = none)
    int count = 0;                    // affected rows / files
    DiagFix fix = DiagFix::None;
    std::wstring fixTr, fixEn;        // what the fix does
    std::wstring fixKey;              // key whose rows the fix rewrites
    int fixSecs = 0;                  // bit (1 << Sec) of the sections the fix rewrites for fixKey
    std::wstring fixList;             // list text (#section / #replace / #remove + rows); empty = no row fix
    bool Broken() const { return state == DiagState::Fail && sev != Sev::Info; }
    const std::wstring& Text(bool english) const { return english ? en : tr; }
};

struct ChainReport {
    std::wstring key;                 // presentation key that was checked
    std::wstring code;                // skill code (empty = no skill given)
    std::vector<DiagItem> steps;      // one per DiagStep, chain order
    std::vector<DiagItem> notes;      // further findings (info, second failures of a step)
    int firstBroken = -1;             // index into steps (-1 = sağlam / healthy)
    bool Healthy() const { return firstBroken < 0; }
    const DiagItem* First() const { return firstBroken < 0 ? nullptr : &steps[static_cast<size_t>(firstBroken)]; }
    const DiagItem& Step(DiagStep s) const { return steps[static_cast<size_t>(s)]; }
    std::wstring Text(bool english) const;  // readable (✓ / ✗ / – per step)
};

using FileExistsFn = std::function<bool(ResArc arc, const std::wstring& innerPath)>;
// .efp probe (render side, optional): how many things (particles, meshes, sub-effects) the effect shows in its first
// windowMs; return false when the file could not be simulated (step -> skip for that file)
struct EfpLive {
    int emitted = 0;
    std::wstring note;
};
using EfpProbeFn = std::function<bool(const std::wstring& particlesPath, double windowMs, EfpLive& out)>;
// a replacement for a missing file: an existing file of the same folder (empty = none)
using SimilarFileFn = std::function<std::wstring(ResArc arc, const std::wstring& missingPath)>;

struct DiagInput {
    const FxDb* db = nullptr;
    std::wstring key;                               // empty = PresentationKey(skill)
    const SkillInfo* skill = nullptr;               // the selected skill (skilldata ENC row)
    std::function<const SkillInfo*(int id)> byId;   // Basic_Original chain
    std::function<const SkillInfo*(const std::wstring& codeOrGroup)> byName;  // enc step without `skill`
    const IModelInfo* caster = nullptr;             // the caster's model (clip / event / bone steps)
    const IModelInfo* target = nullptr;             // optional: bones of target-anchored rows
    std::vector<std::wstring> casterBones;          // optional: skeleton bone names as written (bone fix)
    const Bytes* casterBsr = nullptr;               // optional: the caster's .bsr bytes (sound step, V17)
    std::wstring casterCi;                          // characterInfo key of the caster (CodeName128; empty = player / none)
    FileExistsFn fileExists;                        // files / clip-file / sound-file checks
    EfpProbeFn efpProbe;                            // optional (.efp emits in the first 2 s)
    double efpWindowMs = 2000;
    SimilarFileFn similarFile;                      // optional (ReplaceFile fix)
    const HiddenRows* hidden = nullptr;             // comment step (rows the client does not read)
    PlayOptions play;                               // timeline options (casterCi / weaponGroup are taken from here too)
};
ChainReport Diagnose(const DiagInput& in);
// the plan's short form: key + model + file check + optional .efp probe
ChainReport Diagnose(const FxDb& db, const std::wstring& key, const IModelInfo* model, const FileExistsFn& fileExists,
                     const EfpProbeFn& efpProbe = {});

// The pending list text after a fix: the key's rows of the sections the fix rewrites are taken out of the list
// (other keys, directives and comments stay), then the fix's list text is appended.
std::wstring ApplyFix(const std::wstring& listText, const DiagItem& fix);

// -------------------------------------------------------------------------------------- pending rows (M6)
// Model checks the merge cannot do from text: for every new / changed skilleffectset row and skillaniset2 row of the
// pending list - V13 (StartEvent k <= type-1 events of the phase clip, AniShot / AniReady / AniWait role in the
// declared aniGroup, DMG rows vs the attack clip events), V14 (StartBone / TargetBone in the .bsk), V17 (.bsr sound sets: > 15 tracks
// Error = client assert, > 11 Info SND11) for the .bsr files that come with a package / transfer.
struct PendingCheckOptions {
    std::function<const IModelInfo*(const std::wstring& key)> modelOf;     // caster model of a key (nullptr = unknown)
    std::function<std::wstring(const std::wstring& key)> weaponOf;          // players: held weapon aniGroup (optional)
    std::function<const SkillInfo*(const std::wstring& key)> skillOf;       // optional: skill timing for the timeline
    std::vector<std::wstring> bsrPaths;                                      // .bsr files of the package (Data paths)
    std::function<bool(const std::wstring& dataPath, Bytes& out)> readData;  // reads them (package overlay first)
};
// Issues in the merge issue list format (se::Issue; rule V13 / V14 / V17 / SND11; listLine = the pending row)
std::vector<Issue> CheckPendingRows(const FxDb& db, const PendingCheckOptions& o = {});

}  // namespace se::sp
