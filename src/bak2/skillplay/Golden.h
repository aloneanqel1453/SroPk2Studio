// SkillEffect Studio v6 P5 (M28) - skillplay/Golden.h
// "Altın referans": what every skill ID of a client shows, as one short line per skill, so a change can be compared
// with the state before it (golden file) and with the state after it (side effects, SideFx.h).
//   skill ID (skilldata ENC / plain buckets) -> presentation key (FxDb::PresentationKey: group / code, Basic_Original)
//   -> effective rows (skillaniset2 + skilleffectset with the client's rules: last SA wins, SE rows accumulate,
//   Service 0, pending rows) -> timeline digest (BuildTimeline without models: event time, efp / bsr path, bone,
//   placement, sound, hits, length) -> one TSV line per skill ID.
// Nothing here writes into a client; the golden file is written atomically (temp + move).
#pragma once

#include "SkillPlay.h"

#include "../core/Client.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace se::sp {

// the presentation data of one client (+ its overlay when the ClientBase has one): FxDb + every skill row
struct GoldenSource {
    FxDb db;
    std::vector<SkillInfo> skills;        // file order of the buckets, one per ID (a later duplicate ID wins)
    std::map<int, size_t> byId;           // ID -> index in skills
    std::wstring skillIndex;              // skilldataenc.txt / skilldata.txt (the index that was used)
    int buckets = 0;
    const SkillInfo* ById(int id) const;
    std::function<const SkillInfo*(int)> Lookup() const;
};
// reads skilleffect.txt, effectsound.txt and every skilldata bucket of c (overlay first when set) - read-only
bool LoadGoldenSource(const ClientBase& c, GoldenSource& out, std::wstring& err);
// same from bytes (tests: a changed skilleffect.txt without an output folder); skill rows come from c
bool LoadGoldenSourceWith(const ClientBase& c, const Bytes* skillEffect, const Bytes* effectSound, GoldenSource& out,
                          std::wstring& err);

struct GoldenLine {
    int id = 0;
    std::wstring code, key;
    int service = -1;                     // 1 SA row with Service 1, 0 Service 0 (disabled), -1 no SA row
    int rows = 0;                         // effective skilleffectset rows of the key
    std::uint32_t rowHash = 0;            // SA cells + SE cells of the key (file order, line numbers ignored)
    std::uint32_t tlHash = 0;             // timeline events (time 0.1 ms, file, bone, place, sound, hit, length)
    int fx = 0, snd = 0, hit = 0;
    int totalMs = 0;
    std::wstring Tsv() const;             // id code key svc rows rowHash tlHash fx snd hit ms (TAB separated)
    bool SameAs(const GoldenLine& o) const {
        return id == o.id && key == o.key && service == o.service && rows == o.rows && rowHash == o.rowHash && tlHash == o.tlHash;
    }
};

struct GoldenOptions {
    std::uint32_t seed = 1;               // timeline random picks (fixed: two runs give the same lines)
    std::function<void(int percent)> progress;
};

// one skill (key resolved through src)
GoldenLine GoldenOf(const GoldenSource& src, const SkillInfo& s, const GoldenOptions& o = {});
// the digest of one key alone (row + timeline hashes with an invalid skill): SideFx changed-key filter
std::uint64_t KeyDigest(const FxDb& db, const std::wstring& key);
// every skill ID, ascending
std::vector<GoldenLine> GoldenAll(const GoldenSource& src, const GoldenOptions& o = {});

// golden file: UTF-8 with BOM, CRLF, header "# SkillEffect Studio golden v1 <client> skills=N" + column line
std::wstring GoldenText(const std::vector<GoldenLine>& lines, const std::wstring& client);
bool WriteGolden(const std::wstring& path, const std::vector<GoldenLine>& lines, const std::wstring& client, std::wstring& err);
bool ReadGolden(const std::wstring& path, std::vector<GoldenLine>& lines, std::wstring& err);

struct GoldenDiff {
    enum Kind : int { Added = 0, Removed = 1, Changed = 2 };
    Kind kind = Changed;
    GoldenLine before, after;
    std::wstring what;                    // "key", "service", "rows", "timeline" (comma list)
};
struct GoldenCompare {
    std::vector<GoldenDiff> diffs;
    int same = 0, added = 0, removed = 0, changed = 0;
    std::wstring Text(bool en, size_t max = 40) const;
};
GoldenCompare CompareGolden(const std::vector<GoldenLine>& before, const std::vector<GoldenLine>& after);

}  // namespace se::sp
