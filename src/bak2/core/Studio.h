// SkillEffect Studio - core/Studio.h
// The whole v2 pipeline: the client (read-only) + the list / export package -> every client text table the
// skill effects depend on is merged by its own rules (skilleffect sections, ID buckets + index files, shared
// tables after their last active row), validated (skilleffect rules V1..V18, table rules T1..T6, resources
// R1..R4), fixed on request ("Düzelt" / "Otomatik düzelt") and written to an output folder (+ optional Media.pk2 COPY).
#pragma once

#include "Engine.h"
#include "Package.h"
#include "Table.h"

#include <map>
#include <functional>
#include <memory>

namespace se {

struct TablePlan {
    std::wstring table, file;              // table id + its index / shared file
    std::vector<std::wstring> outFiles;    // files of this table that change
    int listRows = 0, added = 0, same = 0, skipped = 0, replaced = 0, bad = 0;
    bool created = false;                  // a new bucket / table file
};

struct OutFile {
    std::wstring name;     // textdata file name
    Bytes bytes;
    bool created = false;  // the client has no such file yet
    std::wstring table;    // table id ("" = skilleffect.txt)
};

struct ResRef {
    Arc arc = Arc::Data;
    std::wstring inner, kind, usedBy;
    int listLine = -1;
    int state = 0;         // 1 in the client, 2 in the export package (installed by Apply), 0 missing, -1 archive not available
};

// a resource file of the package that Apply installs (copied under <out>\Data|Particles|Media\ + optional pk2 copy)
struct ResInstall {
    Arc arc = Arc::Data;
    std::wstring inner;    // path inside the archive (lower case)
    std::wstring src;      // file of the package
    bool replaces = false; // the client has a different file with this name
};

struct StudioResult {
    ParsedList list;
    Plan plan;
    MergeResult merge;
    RefData ref;
    bool hasSe = false;            // skilleffect.txt of the client was read
    std::vector<Issue> issues;
    std::vector<TablePlan> tables;
    std::vector<OutFile> outputs;
    std::vector<ResRef> res;
    std::vector<ResInstall> installs;  // package resources Apply copies (identical client files are skipped)
    int resSame = 0;                   // package resources the client already has byte-identical
    int rowsTotal = 0, rowsActive = 0;
    int rowsWorking = 0;               // active AND without a blocking error (V8 skill unknown, V5 creature unknown)
    int errors = 0, warns = 0, infos = 0, missing = 0;
    bool verified = true;          // skilleffect client-model check (true when no skilleffect rows)
    bool resChecked = false;
};

class Studio {
public:
    bool OpenBase(const std::wstring& path, const std::string& key, std::wstring& err);
    bool HasBase() const { return m_base.IsOpen(); }
    const ClientBase& Base() const { return m_base; }
    const SeDoc* BaseSe(std::wstring* err = nullptr);
    const RefData& BaseRef(const Progress& prog = {});
    bool BaseFile(const std::wstring& name, Bytes& out);  // cached textdata read

    void SetPool(std::shared_ptr<Package> p) { m_pool = std::move(p); }
    // server SQL of the list's characterdata / itemdata / skilldata rows (set by the programs: transfer XSql, SQL_AUDIT
    // section 4): files = (file name, content) written by Write() under <out>\Server\SQL\; note = what was skipped
    using SqlWriter = std::function<bool(const StudioResult& r, std::vector<std::pair<std::wstring, Bytes>>& files, std::wstring& note)>;
    void SetSqlWriter(SqlWriter w) { m_sql = std::move(w); }
    const Package* Pool() const { return m_pool.get(); }

    StudioResult Run(const std::wstring& listText, const std::wstring& listDesc, const Options& opt, bool checkRes,
                     const Progress& prog = {});

    // "Düzelt" (which = issue indexes) / "Otomatik düzelt" (all fixable): returns the new list text
    std::wstring ApplyFixes(const std::wstring& listText, const StudioResult& r, const std::vector<size_t>& which,
                            std::vector<std::wstring>& log, int& applied) const;

    struct WriteResult {
        std::vector<std::pair<std::wstring, std::wstring>> undo;  // (written file, its backup or "")
        std::vector<std::wstring> written;
        std::wstring reportPath, log, pk2Path;
        std::vector<std::wstring> pk2Paths;  // every pk2 copy written
        int resWritten = 0, unchanged = 0;
        std::vector<std::wstring> sqlFiles;  // generated INSTALL / CHECK / UNINSTALL.sql (safe; the tool never runs them)
        std::wstring sqlNote;
        std::vector<std::wstring> p3;        // v5 M4: pk2 copies that end past 2 GiB (warning text, UI language)
    };
    enum Pk2Arcs : unsigned { kPk2Media = 1, kPk2Data = 2, kPk2Particles = 4 };
    // only the changed files, mirrored under <out>\Media\server_dep\silkroad\textdata\ + the package resources under
    // <out>\Data|Particles|Media\ + report; optional pk2 COPIES (pk2Arcs); never into the client
    bool Write(const StudioResult& r, const std::wstring& outDir, unsigned pk2Arcs, bool en, WriteResult& wr, std::wstring& err,
               const Progress& prog = {});
    // size in bytes of the client archive a pk2 copy duplicates (0: no such archive)
    long long Pk2CopySize(Arc a) const;
    // v5 M4: dry run of the pk2 copies Write would make (read-only, nothing copied): one P3 size note (information) per
    // copy past 2 GiB (v6: the client reads any size; the note never blocks)
    std::vector<Issue> Pk2Check(const StudioResult& r, unsigned pk2Arcs) const;
    // "mevcut dosyayı onar": a repaired COPY of the client's skilleffect.txt (<out>\onarim\skilleffect.txt)
    bool WriteRepaired(const std::wstring& outDir, bool en, WriteResult& wr, std::wstring& err);
    std::wstring Report(const StudioResult& r, bool en, const WriteResult* wr) const;
    bool OutDirAllowed(const std::wstring& outDir, std::wstring& why) const;

private:
    void MergeBucket(const TableSpec& sp, const std::vector<const TableRow*>& rows, const Options& opt, StudioResult& r, TablePlan& tp);
    void MergeShared(const TableSpec& sp, const std::vector<const TableRow*>& rows, const Options& opt, StudioResult& r, TablePlan& tp);
    void Annotate(StudioResult& r) const;
    void CheckResources(StudioResult& r);
    void CheckCoverage(StudioResult& r);   // C1 / C2: what the package has for these skills but the list lacks
    void CheckIndexes(StudioResult& r);    // T3: index files list bucket files the client does not have
    std::wstring AniShotFor(const StudioResult& r, const std::wstring& key, const std::wstring& code, std::wstring& note) const;
    const std::unordered_map<int, std::wstring>& ItemIds();

    ClientBase m_base;
    std::shared_ptr<Package> m_pool;
    SqlWriter m_sql;
    std::map<std::wstring, Bytes> m_cache;
    bool m_refLoaded = false, m_seLoaded = false, m_seOk = false, m_itemLoaded = false;
    RefData m_ref;
    SeDoc m_se;
    std::wstring m_seErr;
    std::unordered_map<int, std::wstring> m_itemIds;
    std::map<std::wstring, int> m_resSame;  // "arc|inner" -> 1 identical in the client, 0 differs/missing (package resources)
    const Package* m_resSameFor = nullptr;
};

// ---- output folder binding: the marker file SkillEffectStudio_cikti.txt records the client a folder was written for
// (path + Media.pk2 size and time), so the files of one client never show up as the AFTER view of another
struct OutBinding {
    bool marker = false;   // the folder has the marker file
    std::wstring client;   // client path written in it (empty: an older marker without it)
    std::wstring finger;   // "size:mtime" of the client's Media.pk2 (or textdata folder)
};
OutBinding ReadOutBinding(const std::wstring& outDir);
std::wstring ClientFinger(const ClientBase& c);
// 1 = written for this client, 0 = unknown (no marker / an older marker), -1 = written for another client
int OutBindingMatch(const OutBinding& b, const ClientBase& c);
// writes (or refreshes) the marker for client c; false + err when the folder belongs to another client and !force
bool WriteOutBinding(const std::wstring& outDir, const ClientBase& c, bool force, std::wstring& err);
constexpr const wchar_t* kOutMarkerName = L"SkillEffectStudio_cikti.txt";
// a server SQL file generated by this program (safe: SQL_AUDIT section 4) - a package's own fixed-ID scripts are not
bool IsSafeSqlScript(const std::wstring& path);

void Explain(Issue& i);   // "Nasıl düzelir" text by rule
// minimal correct skillaniset2 row (template of shipped monster skills, SKILLEFFECT_LOGIC.md §7.1)
std::wstring MakeSaRow(const std::wstring& key, const std::wstring& label, const std::wstring& aniShot);
std::wstring AniShotForCode(const std::wstring& code);
// animation role token of a .bsr ResourceAnimationType id (ANI_ATTACK1, ANI_SKILL_4 ...; empty when not a role)
std::wstring AniToken(int type);
// (role id, animation file) pairs of a .bsr (JMXVRES 0109 aniGroup section); empty when not parsable
std::vector<std::pair<int, std::wstring>> BsrRoles(const Bytes& b);
// pad / trim a skilleffect row to its section width with the column defaults
std::wstring PadSeRow(Sec s, const std::wstring& row, std::vector<std::wstring>* changes = nullptr);

}  // namespace se
