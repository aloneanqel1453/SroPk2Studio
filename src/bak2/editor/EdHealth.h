// SkillEffect Studio - editor/EdHealth.h
// Whole-client health check (read-only): every textdata table (encoding / line ends / /* */ traps / hidden rows /
// cell types / empty-cell shifts / duplicate keys and IDs), index files (listed files missing, split files not
// listed), cross-table references (characterInfo keys, skill keys V8, SE without SA V9, NameStrID, DefaultSkill IDs,
// sound objects), resource files (models, icons, efp, wav), duplicate IDs / codes across the split files, plain vs
// ENC skilldata, and the .bsr sound-track limit (MAX_SOUND_TRACK_PER_ANI = 15) of every creature model.
#pragma once

#include "EdValidate.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace se::ed {

struct HealthIssue {
    Sev sev = Sev::Warn;
    std::wstring table;      // textdata file / .bsr path / "" = client level
    int line = -1;           // physical line (0-based) or -1
    int col = -1;
    std::wstring colName;
    std::wstring code;       // V4, R3, FILE, REF, SND15 ...
    std::wstring key;        // row key (when known)
    std::wstring msgTr, msgEn;
    bool stock = false;      // the client itself has it (also without the output folder): not one of "my changes"
    bool softened = false;   // an error the client tolerates (reads 0 / -1, skips the row): shown as a warning
    bool isNew = false;      // compared with the baseline (the client without the output folder) and not in it
};

// what a finding does in the game (M2 / M7): Crash blocks Kaydet / Uygula; Shipped = the client ships with it
enum class Impact : int { Crash = 0, Invisible = 1, NoSound = 2, Wrong = 3, Cosmetic = 4, Shipped = 5 };
constexpr int kImpactCount = 6;
// rule-code namespaces: the editor / health codes (this module), the merge engine (core Validate / Studio: V1-V18,
// T1-T6, R0-R4 resources, S1-S3, P1-P3, L*, C1, C2, D1, ENC2) and the transfer plan (X0-X12). R3 / R4 differ:
// editor R3 = '//' in a cell, core R3 = missing .wav.
enum class RuleSet : int { Editor = 0, Core = 1, Transfer = 2 };
// impact of a rule at a severity (unknown code: Wrong for errors, Cosmetic otherwise)
Impact ImpactOf(const std::wstring& code, Sev sev = Sev::Error, RuleSet set = RuleSet::Editor);
// a health finding: stock -> Shipped; FILE / R* by extension (.wav / .ogg -> NoSound); REF to sound objects -> NoSound
Impact ImpactOf(const HealthIssue& i);
// an editor cell / row finding (no stock notion: new / edited rows)
Impact ImpactOf(const CellIssue& i);
bool ImpactKnown(const std::wstring& code, RuleSet set = RuleSet::Editor);  // the table lists the code
const wchar_t* ImpactName(Impact m, bool en);       // "İstemci çöker" / "Client crashes" ...
std::wstring ImpactSentence(Impact m, bool en);     // one plain sentence: what the player sees
// the Crash-class errors of the new / edited rows of every unsaved table (the save gate of the editor)
std::vector<CellIssue> PendingCrashIssues(const Workspace& ws, const RefIndex* ref);

struct HealthOpts {
    bool refs = true;        // cross-table references
    bool files = true;       // resource files exist (Data / Particles / Media)
    bool bsrSounds = true;   // .bsr ModDataSound track counts of the creature models
    int maxBsr = 0;          // 0 = every model
    size_t maxIssues = 300000;
    std::vector<std::wstring> only;  // table names (empty = all)
    std::wstring cacheDir;   // baseline cache folder ("" = %LOCALAPPDATA%\SkillEffectStudio\health)
    std::function<void(int percent, const std::wstring& what)> progress;
    const std::atomic<bool>* cancel = nullptr;
};

struct HealthReport {
    std::wstring client;
    std::vector<HealthIssue> issues;
    int tables = 0, rows = 0, errors = 0, warns = 0, infos = 0, bsrChecked = 0;
    std::map<std::wstring, int> byCode;   // code -> count
    std::map<std::wstring, int> byTable;  // table -> count (errors + warnings)
    bool truncated = false;
    bool compared = false;   // an untouched run (no output folder) marked the client's own findings (stock)
    int mineErrors = 0, mineWarns = 0, mineInfos = 0;   // new findings (isNew) by severity
    int newCrash = 0;        // new findings of the Crash class
    bool baselineCached = false;  // the baseline came from the cache file (no second check run)
    double seconds = 0;
    std::wstring Text(bool en, size_t perCode = 25) const;
    bool Write(const std::wstring& path, bool en, std::wstring& err) const;  // UTF-8 BOM, atomic
};

HealthReport CheckClient(const ClientBase& c, const HealthOpts& o);

// baseline = the findings of the client WITHOUT its output folder, as a multiset of signatures. Match key = table +
// rule + row key + column (never the line number): pass 1 also needs the same text, pass 2 takes the rest (a count
// in a message changed). Cached in <cacheDir>\<client digest>.bin (digest = archive / textdata file names, sizes and
// write times + the check options + this program's own exe; output folder excluded).
struct HealthBaseline {
    std::unordered_map<std::uint64_t, int> exact, coarse;
    int issues = 0;
    std::wstring client;
    static HealthBaseline From(const HealthReport& r);
};
std::wstring HealthClientDigest(const ClientBase& c, const HealthOpts& o);  // 16 hex digits
std::wstring HealthBaselinePath(const ClientBase& c, const HealthOpts& o);  // <cacheDir>\<digest>.bin
bool SaveHealthBaseline(const HealthBaseline& b, const std::wstring& path, std::wstring& err);  // atomic, prunes to 24
bool LoadHealthBaseline(const std::wstring& path, HealthBaseline& b, std::wstring& err);       // magic + version checked
// the baseline of c (its overlay ignored): the cache file, else a check of the client without the overlay (then
// cached). fromCache = it came from the file. cancel (o.cancel) -> empty baseline, false.
bool GetHealthBaseline(const ClientBase& c, const HealthOpts& o, HealthBaseline& out, bool* fromCache, std::wstring* err);

// marks the findings of r against the baseline: matched -> stock, the rest -> isNew; base = nullptr: r IS the
// baseline (no output folder), everything stock. Then softens stock errors the client tolerates (TYPE / ENUM / COLS /
// RANGE / ... -> warning) and recounts (errors / warns / infos, mine* = new, newCrash).
void MarkNew(HealthReport& r, const HealthBaseline* base);
// same with a full base report (kept for callers of v4)
void MarkStock(HealthReport& r, const HealthReport* base);
// code -> plain explanation (what the client does, how to fix)
std::wstring HealthCodeHelp(const std::wstring& code, bool en);

}  // namespace se::ed
