// SkillEffect Studio - editor/EdHealth.cpp
#include "EdHealth.h"

#include "EdAssets.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace se::ed {

namespace {

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

struct Sink {
    HealthReport& r;
    const HealthOpts& o;
    void Add(HealthIssue&& i) {
        if (i.sev == Sev::Error) ++r.errors;
        else if (i.sev == Sev::Warn) ++r.warns;
        else ++r.infos;
        ++r.byCode[i.code];
        if (i.sev != Sev::Info) ++r.byTable[i.table];
        if (r.issues.size() >= o.maxIssues) {
            r.truncated = true;
            return;
        }
        r.issues.push_back(std::move(i));
    }
    void Add(Sev s, const std::wstring& table, const wchar_t* code, const std::wstring& tr, const std::wstring& en, int line = -1,
             const std::wstring& key = L"") {
        HealthIssue i;
        i.sev = s;
        i.table = table;
        i.code = code;
        i.msgTr = tr;
        i.msgEn = en;
        i.line = line;
        i.key = key;
        Add(std::move(i));
    }
};

bool PrefixDigits(const std::wstring& lowName, const std::wstring& prefix) {
    if (lowName.rfind(prefix, 0) != 0) return false;
    size_t i = prefix.size(), d = 0;
    while (i < lowName.size() && iswdigit(lowName[i])) ++i, ++d;
    return d > 0;
}

// ---- impact classes (M2 / M7) ------------------------------------------------------------------------------------
constexpr unsigned kEd = 1, kCo = 2, kXf = 4, kAll = 7;
using I = Impact;
struct ImpactRow {
    const wchar_t* code;
    unsigned sets;
    Impact err, warn, info;  // by severity
};
// V1-V18 = SKILLEFFECT_LOGIC.md section 8 ("failure if broken"); the Crash set is V4 column shift, V9 assert,
// V11 / V12 asserts, V15 wcscat_s, V17 / SND15 MAX_SOUND_TRACK_PER_ANI and X12 (a file format the target cannot read)
const ImpactRow kImpact[] = {
    {L"V1", kEd | kCo, I::Invisible, I::Wrong, I::Cosmetic},       // first char lost / lines merged
    {L"V2", kEd | kCo, I::Invisible, I::Invisible, I::Cosmetic},   // rows inside /* */ never read
    {L"V3", kEd | kCo, I::Wrong, I::Wrong, I::Cosmetic},           // '/' in a path
    {L"V4", kEd | kCo, I::Crash, I::Wrong, I::Cosmetic},           // empty / missing cell: columns shift (L2687)
    {L"V5", kEd | kCo, I::Invisible, I::Invisible, I::Cosmetic},   // characterInfo row dropped
    {L"V6", kEd | kCo, I::Wrong, I::Wrong, I::Cosmetic},           // another creature's presentation changes
    {L"V7", kEd | kCo, I::Invisible, I::Invisible, I::Cosmetic},   // Service 0: key disabled
    {L"V8", kEd | kCo, I::Invisible, I::Invisible, I::Cosmetic},   // key not resolvable: effects never shown
    {L"V9", kEd | kCo, I::Crash, I::Invisible, I::Cosmetic},       // orphaned SE rows + g_sSkillEffMapStr assert
    {L"V10", kCo, I::Wrong, I::Wrong, I::Cosmetic},                // enum value -1
    {L"V11", kEd | kCo, I::Crash, I::Wrong, I::Cosmetic},          // DMG TRUE on a wrong phase: assert i < 3 || i == 9
    {L"V12", kEd | kCo, I::Crash, I::Wrong, I::Cosmetic},          // comma field: assert pStr->GetData() == L','
    {L"V13", kEd | kCo, I::Invisible, I::Invisible, I::Cosmetic},  // effect / damage never fires
    {L"V14", kEd | kCo, I::Wrong, I::Wrong, I::Cosmetic},          // effect at the origin
    {L"V15", kEd | kCo, I::Crash, I::Wrong, I::Cosmetic},          // path >= 512: wcscat_s failure
    {L"V16", kCo, I::Invisible, I::Invisible, I::Cosmetic},        // aura not linked
    {L"V17", kAll, I::Crash, I::Cosmetic, I::Cosmetic},            // .bsr sound set > 15 tracks: assert
    {L"V18", kEd | kCo, I::NoSound, I::NoSound, I::Cosmetic},      // effectsound object: no sounds
    {L"S1", kCo, I::Invisible, I::Invisible, I::Cosmetic},         // unknown #section: rows not read where meant
    {L"S2", kCo, I::Invisible, I::Invisible, I::Cosmetic},         // data before the first header
    {L"S3", kCo, I::Invisible, I::Invisible, I::Cosmetic},         // lines joined by a lone CR / NUL
    {L"T1", kCo, I::Wrong, I::Wrong, I::Cosmetic},
    {L"T2", kCo, I::Wrong, I::Wrong, I::Cosmetic},
    {L"T3", kCo, I::Invisible, I::Wrong, I::Cosmetic},
    {L"T4", kCo, I::Cosmetic, I::Cosmetic, I::Cosmetic},
    {L"T5", kCo, I::Invisible, I::Invisible, I::Cosmetic},
    {L"T6", kCo, I::Cosmetic, I::Cosmetic, I::Cosmetic},
    {L"R0", kCo, I::Cosmetic, I::Cosmetic, I::Cosmetic},           // core R0-R4: resource files of the package
    {L"P1", kCo, I::Cosmetic, I::Cosmetic, I::Cosmetic},
    {L"P2", kCo, I::Cosmetic, I::Cosmetic, I::Cosmetic},
    {L"P3", kCo | kXf, I::Cosmetic, I::Cosmetic, I::Cosmetic},     // size note only (v6: the client reads any size)
    {L"C1", kCo, I::Invisible, I::Invisible, I::Cosmetic},
    {L"C2", kCo, I::Invisible, I::Invisible, I::Cosmetic},
    {L"D1", kCo, I::Cosmetic, I::Cosmetic, I::Cosmetic},
    {L"ENC2", kAll, I::Wrong, I::Wrong, I::Cosmetic},              // lossy CP949: a name / path changes
    {L"X0", kXf, I::Cosmetic, I::Cosmetic, I::Cosmetic},           // transfer plan (nothing reaches the game yet)
    {L"X1", kXf, I::Invisible, I::Invisible, I::Cosmetic},
    {L"X2", kXf, I::Wrong, I::Wrong, I::Cosmetic},
    {L"X3", kXf, I::Wrong, I::Wrong, I::Cosmetic},
    {L"X4", kXf, I::Cosmetic, I::Cosmetic, I::Cosmetic},
    {L"X5", kXf, I::Wrong, I::Wrong, I::Cosmetic},
    {L"X6", kXf, I::Cosmetic, I::Cosmetic, I::Cosmetic},
    {L"X7", kXf, I::Invisible, I::Invisible, I::Cosmetic},
    {L"X8", kXf, I::Wrong, I::Wrong, I::Cosmetic},
    {L"X9", kXf, I::Wrong, I::Wrong, I::Cosmetic},
    {L"X10", kXf, I::Wrong, I::Wrong, I::Cosmetic},
    {L"X11", kXf, I::Cosmetic, I::Cosmetic, I::Cosmetic},
    {L"X12", kXf, I::Crash, I::Wrong, I::Cosmetic},
    {L"X13", kXf, I::Wrong, I::Wrong, I::Cosmetic},            // spawn point: cannot be written / suspicious (server only)
    {L"Y1", kAll, I::Wrong, I::Wrong, I::Cosmetic},            // v6 M28 side effect: a skill / creature not targeted changes
    {L"SND15", kAll, I::Crash, I::Crash, I::Cosmetic},
    {L"SND11", kAll, I::Cosmetic, I::Cosmetic, I::Cosmetic},
    {L"IDX", kEd, I::Invisible, I::Invisible, I::Cosmetic},
    {L"IDX2", kEd | kCo, I::Invisible, I::Invisible, I::Cosmetic},
    {L"TYPE", kEd | kCo, I::Wrong, I::Wrong, I::Cosmetic},         // the client reads 0
    {L"ENUM", kEd, I::Wrong, I::Wrong, I::Cosmetic},
    {L"RANGE", kEd, I::Wrong, I::Wrong, I::Cosmetic},
    {L"LEN", kEd, I::Cosmetic, I::Cosmetic, I::Cosmetic},
    {L"FILE", kEd, I::Invisible, I::Invisible, I::Cosmetic},       // by extension in ImpactOf(issue)
    {L"REF", kEd, I::Wrong, I::Wrong, I::Cosmetic},                // sound object -> NoSound in ImpactOf(issue)
    {L"COLS", kEd, I::Wrong, I::Cosmetic, I::Cosmetic},
    {L"ID", kEd, I::Wrong, I::Wrong, I::Cosmetic},
    {L"DUPID", kEd, I::Wrong, I::Wrong, I::Cosmetic},
    {L"DUPCODE", kEd, I::Wrong, I::Wrong, I::Cosmetic},
    {L"BUCKET", kEd, I::Cosmetic, I::Cosmetic, I::Cosmetic},
    {L"SKENC", kEd, I::Invisible, I::Invisible, I::Cosmetic},
    {L"BSR", kEd, I::Invisible, I::Invisible, I::Cosmetic},
    {L"ENC", kEd, I::Invisible, I::Invisible, I::Cosmetic},
    {L"EMPTY", kEd, I::Cosmetic, I::Cosmetic, I::Cosmetic},
    {L"REQ", kEd, I::Wrong, I::Wrong, I::Cosmetic},
    {L"CODE", kEd, I::Cosmetic, I::Cosmetic, I::Cosmetic},
    {L"EXT", kEd, I::Wrong, I::Wrong, I::Cosmetic},
};
// codes whose meaning differs per rule set
struct ImpactSplit {
    const wchar_t* code;
    RuleSet set;
    Impact err, warn, info;
};
const ImpactSplit kImpactSplit[] = {
    {L"R3", RuleSet::Editor, I::Wrong, I::Wrong, I::Cosmetic},          // '//' or '/*' in a cell
    {L"R4", RuleSet::Editor, I::Invisible, I::Invisible, I::Cosmetic},  // data rows hidden in /* */
    {L"R6", RuleSet::Editor, I::Wrong, I::Wrong, I::Cosmetic},          // control character
    {L"R1", RuleSet::Core, I::Invisible, I::Invisible, I::Cosmetic},    // .efp missing
    {L"R2", RuleSet::Core, I::Invisible, I::Invisible, I::Cosmetic},    // .bsr / other resource missing
    {L"R3", RuleSet::Core, I::NoSound, I::NoSound, I::Cosmetic},        // .wav missing
    {L"R4", RuleSet::Core, I::Wrong, I::Wrong, I::Cosmetic},            // .ddj missing (icon / texture)
};

Impact BySev(Sev s, Impact e, Impact w, Impact i) { return s == Sev::Error ? e : s == Sev::Warn ? w : i; }

bool FindImpact(const std::wstring& code, RuleSet set, Sev sev, Impact& out) {
    for (const auto& r : kImpactSplit)
        if (r.set == set && code == r.code) {
            out = BySev(sev, r.err, r.warn, r.info);
            return true;
        }
    const unsigned bit = 1u << static_cast<int>(set);
    for (const auto& r : kImpact)
        if ((r.sets & bit) && code == r.code) {
            out = BySev(sev, r.err, r.warn, r.info);
            return true;
        }
    // L1..L9: syntax of the merge list (the row is not applied)
    if (set == RuleSet::Core && code.size() >= 2 && code[0] == L'L' && iswdigit(code[1])) {
        out = BySev(sev, I::Invisible, I::Invisible, I::Cosmetic);
        return true;
    }
    return false;
}

// resource kind from a message / path: .wav / .ogg -> sound, .ddj -> picture
Impact ByFileKind(const std::wstring& text, Impact other) {
    const std::wstring l = Lower(text);
    if (l.find(L".wav") != std::wstring::npos || l.find(L".ogg") != std::wstring::npos) return I::NoSound;
    if (l.find(L".ddj") != std::wstring::npos || l.find(L".dds") != std::wstring::npos) return I::Wrong;
    return other;
}

// ---- baseline signatures / file ----------------------------------------------------------------------------------
constexpr std::uint32_t kBaseMagic = 0x42484553;  // "SEHB"
constexpr std::uint32_t kBaseVersion = 1;
constexpr std::uint32_t kHealthRules = 5;  // bump when a rule changes what it reports (old caches then miss)

struct Fnv {
    std::uint64_t h = 1469598103934665603ULL;
    void Raw(const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ULL;
        }
    }
    void Str(const std::wstring& s) {
        Raw(s.data(), s.size() * sizeof(wchar_t));
        const wchar_t sep = 0x1F;
        Raw(&sep, sizeof(sep));
    }
    template <class T>
    void Pod(const T& v) {
        Raw(&v, sizeof(v));
    }
};

std::uint64_t SigCoarse(const HealthIssue& i) {
    Fnv f;
    f.Str(Lower(i.table));
    f.Str(i.code);
    f.Str(i.key);
    f.Str(i.colName);
    f.Pod(i.col);
    return f.h;
}
std::uint64_t SigExact(const HealthIssue& i) {
    Fnv f;
    f.h = SigCoarse(i);
    f.Str(i.msgEn);
    return f.h;
}

// size + last write time of a file (a marker when missing)
void HashFileStamp(Fnv& f, const std::wstring& path) {
    f.Str(Lower(path));
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) {
        f.Pod(a.nFileSizeHigh);
        f.Pod(a.nFileSizeLow);
        f.Pod(a.ftLastWriteTime.dwHighDateTime);
        f.Pod(a.ftLastWriteTime.dwLowDateTime);
    } else {
        const std::uint32_t none = 0xFFFFFFFFu;
        f.Pod(none);
    }
}
// every file of a folder (names, sizes, write times), sorted
void HashFolder(Fnv& f, const std::wstring& dir) {
    std::vector<std::wstring> rows;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            rows.push_back(Lower(fd.cFileName) + Fmt(L"|%lu|%lu|%lu|%lu", fd.nFileSizeHigh, fd.nFileSizeLow, fd.ftLastWriteTime.dwHighDateTime,
                                                       fd.ftLastWriteTime.dwLowDateTime));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(rows.begin(), rows.end());
    f.Str(Lower(dir));
    for (const auto& r : rows) f.Str(r);
}

bool MakeDirs(const std::wstring& dir) {
    if (dir.empty() || DirExists(dir)) return true;
    const std::wstring parent = DirOf(dir);
    if (!parent.empty() && parent != dir && !MakeDirs(parent)) return false;
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring DefaultCacheDir() {
    wchar_t b[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", b, static_cast<DWORD>(std::size(b)));
    std::wstring base = n && n < std::size(b) ? std::wstring(b, n) : std::wstring();
    if (base.empty()) {
        const DWORD m = GetTempPathW(static_cast<DWORD>(std::size(b)), b);
        base = m && m < std::size(b) ? std::wstring(b, m) : std::wstring(L".");
    }
    return JoinPath(JoinPath(base, L"SkillEffectStudio"), L"health");
}

void PruneCache(const std::wstring& dir, const std::wstring& keep, size_t maxFiles) {
    struct F {
        std::wstring path;
        FILETIME t;
    };
    std::vector<F> files;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*.bin").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back({JoinPath(dir, fd.cFileName), fd.ftLastWriteTime});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (files.size() <= maxFiles) return;
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return CompareFileTime(&a.t, &b.t) > 0; });  // newest first
    for (size_t i = maxFiles; i < files.size(); ++i)
        if (_wcsicmp(files[i].path.c_str(), keep.c_str()) != 0) DeleteFileW(files[i].path.c_str());
}

struct Writer {
    Bytes b;
    template <class T>
    void Pod(const T& v) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
        b.insert(b.end(), p, p + sizeof(T));
    }
    void Str(const std::wstring& s) {
        Pod(static_cast<std::uint32_t>(s.size()));
        const auto* p = reinterpret_cast<const std::uint8_t*>(s.data());
        b.insert(b.end(), p, p + s.size() * sizeof(wchar_t));
    }
};
struct Reader {
    const Bytes& b;
    size_t at = 0;
    bool ok = true;
    template <class T>
    T Pod() {
        T v{};
        if (at + sizeof(T) > b.size()) {
            ok = false;
            return v;
        }
        std::memcpy(&v, b.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    std::wstring Str() {
        const auto n = Pod<std::uint32_t>();
        if (!ok || n > (b.size() - at) / sizeof(wchar_t)) {
            ok = false;
            return {};
        }
        std::wstring s(n, L'\0');
        std::memcpy(s.data(), b.data() + at, n * sizeof(wchar_t));
        at += n * sizeof(wchar_t);
        return s;
    }
};

}  // namespace

// ---- impact ---------------------------------------------------------------------------------------------------------
Impact ImpactOf(const std::wstring& code, Sev sev, RuleSet set) {
    Impact m = Impact::Wrong;
    if (FindImpact(code, set, sev, m)) return m;
    return sev == Sev::Error ? Impact::Wrong : Impact::Cosmetic;
}

bool ImpactKnown(const std::wstring& code, RuleSet set) {
    Impact m = Impact::Wrong;
    return FindImpact(code, set, Sev::Error, m);
}

namespace {
Impact Refine(const std::wstring& code, Sev sev, const std::wstring& msgEn, Impact m) {
    if (code == L"REF" && sev == Sev::Info && Lower(msgEn).find(L"sound") != std::wstring::npos) return Impact::NoSound;  // silent creature
    if (code == L"FILE" && m != Impact::Cosmetic) return ByFileKind(msgEn, m);
    return m;
}
}  // namespace

Impact ImpactOf(const HealthIssue& i) {
    if (i.stock) return Impact::Shipped;
    return Refine(i.code, i.sev, i.msgEn, ImpactOf(i.code, i.sev, RuleSet::Editor));
}

Impact ImpactOf(const CellIssue& i) { return Refine(i.code, i.sev, i.msgEn, ImpactOf(i.code, i.sev, RuleSet::Editor)); }

const wchar_t* ImpactName(Impact m, bool en) {
    switch (m) {
        case Impact::Crash: return en ? L"Client crashes" : L"İstemci çöker";
        case Impact::Invisible: return en ? L"Not shown" : L"Efekt görünmez";
        case Impact::NoSound: return en ? L"No sound" : L"Ses yok";
        case Impact::Wrong: return en ? L"Looks wrong" : L"Yanlış görünür";
        case Impact::Cosmetic: return en ? L"Minor" : L"Önemsiz";
        case Impact::Shipped: return en ? L"Harmless (ships with the game)" : L"Zararsız (oyunla gelir)";
    }
    return L"";
}

std::wstring ImpactSentence(Impact m, bool en) {
    switch (m) {
        case Impact::Crash:
            return en ? L"The client closes while it reads this (assert / buffer check). Fix it before saving."
                      : L"İstemci bunu okurken kapanır (assert / tampon denetimi). Kaydetmeden önce düzeltin.";
        case Impact::Invisible:
            return en ? L"The game skips the row or file: the effect, animation or object does not show."
                      : L"Oyun satırı ya da dosyayı atlar: efekt, animasyon ya da nesne görünmez.";
        case Impact::NoSound: return en ? L"The sound does not play; nothing else changes." : L"Ses çalmaz; başka bir şey değişmez.";
        case Impact::Wrong:
            return en ? L"The game reads another value (0, -1 or a shifted column): something looks or works wrong."
                      : L"Oyun başka bir değer okur (0, -1 ya da kaymış sütun): bir şey yanlış görünür ya da yanlış çalışır.";
        case Impact::Cosmetic:
            return en ? L"No visible effect in the game: a layout or information note." : L"Oyunda görünür etkisi yok: düzen ya da bilgi notu.";
        case Impact::Shipped:
            return en ? L"The client itself has it and the game runs with it: not one of your changes."
                      : L"İstemcinin kendisinde de var ve oyun bununla çalışıyor: sizin değişikliğiniz değil.";
    }
    return L"";
}

std::vector<CellIssue> PendingCrashIssues(const Workspace& ws, const RefIndex* ref) {
    std::vector<CellIssue> out;
    for (size_t k = 0; k < ws.docs.size(); ++k) {
        const EdDoc& d = *ws.docs[k];
        if (!d.Unsaved()) continue;
        for (auto& i : CheckDoc(d, static_cast<int>(k), ref, true))
            if (i.sev == Sev::Error && ImpactOf(i) == Impact::Crash) out.push_back(std::move(i));
    }
    return out;
}

// ---- baseline -------------------------------------------------------------------------------------------------------
HealthBaseline HealthBaseline::From(const HealthReport& r) {
    HealthBaseline b;
    b.client = r.client;
    b.issues = static_cast<int>(r.issues.size());
    for (const auto& i : r.issues) {
        ++b.exact[SigExact(i)];
        ++b.coarse[SigCoarse(i)];
    }
    return b;
}

std::wstring HealthClientDigest(const ClientBase& c, const HealthOpts& o) {
    Fnv f;
    f.Pod(kBaseVersion);
    f.Pod(kHealthRules);
    f.Pod(static_cast<int>(c.kind));
    f.Pod(o.refs);
    f.Pod(o.files);
    f.Pod(o.bsrSounds);
    f.Pod(o.maxBsr);
    std::vector<std::wstring> only;
    for (const auto& n : o.only) only.push_back(Lower(n));
    std::sort(only.begin(), only.end());
    for (const auto& n : only) f.Str(n);
    // this program: a new build may report differently
    wchar_t exe[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (n && n < std::size(exe)) HashFileStamp(f, exe);
    // the client without its output folder
    f.Str(Lower(c.path));
    f.Str(Lower(c.root));
    if (!c.mediaPk2.empty()) HashFileStamp(f, c.mediaPk2);
    for (const auto& a : c.arcPath)
        if (!a.empty()) HashFileStamp(f, a);
    if (!c.textdataDir.empty()) HashFolder(f, c.textdataDir);
    if (!c.singleFile.empty()) HashFileStamp(f, c.singleFile);
    for (const auto& m : c.missing) f.Str(Lower(m));
    return Fmt(L"%016llx", static_cast<unsigned long long>(f.h));
}

std::wstring HealthBaselinePath(const ClientBase& c, const HealthOpts& o) {
    return JoinPath(o.cacheDir.empty() ? DefaultCacheDir() : o.cacheDir, HealthClientDigest(c, o) + L".bin");
}

bool SaveHealthBaseline(const HealthBaseline& base, const std::wstring& path, std::wstring& err) {
    Writer w;
    w.Pod(kBaseMagic);
    w.Pod(kBaseVersion);
    w.Pod(static_cast<std::int32_t>(base.issues));
    w.Str(base.client);
    for (const auto* m : {&base.exact, &base.coarse}) {
        w.Pod(static_cast<std::uint32_t>(m->size()));
        for (const auto& [h, n] : *m) {
            w.Pod(h);
            w.Pod(static_cast<std::int32_t>(n));
        }
    }
    Fnv f;
    f.Raw(w.b.data(), w.b.size());
    w.Pod(f.h);
    if (!MakeDirs(DirOf(path))) {
        err = L"cannot create " + DirOf(path);
        return false;
    }
    if (!WriteFileAtomic(path, w.b, err)) return false;
    PruneCache(DirOf(path), path, 24);
    return true;
}

bool LoadHealthBaseline(const std::wstring& path, HealthBaseline& base, std::wstring& err) {
    Bytes b;
    if (!FileExists(path) || !ReadFileBytes(path, b, err)) {
        if (err.empty()) err = L"no baseline file";
        return false;
    }
    if (b.size() < 32) {
        err = L"baseline file too short";
        return false;
    }
    Fnv f;
    f.Raw(b.data(), b.size() - 8);
    std::uint64_t sum = 0;
    std::memcpy(&sum, b.data() + b.size() - 8, 8);
    if (sum != f.h) {
        err = L"baseline file damaged (checksum)";
        return false;
    }
    Reader r{b};
    const auto magic = r.Pod<std::uint32_t>();
    const auto ver = r.Pod<std::uint32_t>();
    if (magic != kBaseMagic || ver != kBaseVersion) {
        err = L"not a baseline file / other version";
        return false;
    }
    HealthBaseline out;
    out.issues = r.Pod<std::int32_t>();
    out.client = r.Str();
    for (auto* m : {&out.exact, &out.coarse}) {
        const auto n = r.Pod<std::uint32_t>();
        if (!r.ok || n > (b.size() - r.at) / 12) {
            err = L"baseline file damaged";
            return false;
        }
        m->reserve(n);
        for (std::uint32_t k = 0; k < n && r.ok; ++k) {
            const auto h = r.Pod<std::uint64_t>();
            const auto c = r.Pod<std::int32_t>();
            (*m)[h] = c;
        }
    }
    if (!r.ok || r.at + 8 != b.size()) {
        err = L"baseline file damaged";
        return false;
    }
    base = std::move(out);
    return true;
}

bool GetHealthBaseline(const ClientBase& c, const HealthOpts& o, HealthBaseline& out, bool* fromCache, std::wstring* err) {
    if (fromCache) *fromCache = false;
    const std::wstring path = HealthBaselinePath(c, o);
    std::wstring e;
    if (LoadHealthBaseline(path, out, e)) {
        if (fromCache) *fromCache = true;
        return true;
    }
    ClientBase base;  // the same client without the output folder
    e.clear();
    if (!base.Open(c.path, c.key, e)) {
        if (err) *err = e;
        return false;
    }
    const HealthReport r = CheckClient(base, o);
    if (o.cancel && o.cancel->load()) return false;
    out = HealthBaseline::From(r);
    std::wstring we;
    if (!SaveHealthBaseline(out, path, we) && err) *err = we;  // not cached: still a valid baseline
    return true;
}

std::wstring HealthCodeHelp(const std::wstring& c, bool en) {
    struct H {
        const wchar_t* code;
        const wchar_t* tr;
        const wchar_t* en;
    };
    static const H k[] = {
        {L"V1", L"Kodlama / satır sonu: UTF-16LE + BOM + CRLF olmalı.", L"Encoding / line ends: UTF-16LE + BOM + CRLF."},
        {L"V2", L"Satır /* */ bloğunun içinde: istemci okumaz (dosya sonundaki açık blok = EOF tuzağı).", L"Row inside /* */: never read (open block at EOF = EOF trap)."},
        {L"R3", L"Hücrede // ya da /*: satırın kalanı yorum olur.", L"// or /* in a cell: the rest of the line becomes a comment."},
        {L"R4", L"/* */ içinde veri görünümlü satırlar (gizli).", L"Data-looking rows inside /* */ (hidden)."},
        {L"R6", L"Kontrol karakteri U+0001..U+0010.", L"Control character U+0001..U+0010."},
        {L"V4", L"Boş hücre / eksik hücre: istemci boş hücreyi atlar, sütunlar kayar.", L"Empty / missing cell: the client skips it and columns shift."},
        {L"V5", L"characterInfo anahtarı istemcide tanımlı bir canavar değil.", L"characterInfo key is not a client creature."},
        {L"V6", L"Aynı anahtar birden çok kez (CI: sonuncusu geçerli, SA: üzerine yazılır).", L"Same key more than once."},
        {L"V7", L"skillaniset2 Service 0: anahtar kapalı.", L"skillaniset2 Service 0: key disabled."},
        {L"V8", L"Sunum anahtarı skilldata'da yok: efekt hiç görünmez.", L"Presentation key not in skilldata: effect never shows."},
        {L"V9", L"skilleffectset satırının skillaniset2 satırı yok.", L"skilleffectset row without a skillaniset2 row."},
        {L"V11", L"DMG Event TRUE yanlış fazda (istemci assert).", L"DMG Event TRUE on a wrong phase (client assert)."},
        {L"V12", L"Virgüllü alan eksik / bozuk (istemci assert).", L"Comma field incomplete (client assert)."},
        {L"V13", L"Becerinin AniShot animasyonu modelde (.bsr) yok: animasyon oynamaz.", L"The skill's AniShot animation is not in the model."},
        {L"V15", L"Klasör '\\' ile bitmeli / yol çok uzun.", L"Folder must end in '\\' / path too long."},
        {L"ENUM", L"Tanımsız ya da büyük/küçük harfi yanlış değer (istemci -1 okur).", L"Unknown or wrong-case value (client reads -1)."},
        {L"TYPE", L"Sayı beklenen hücrede metin.", L"Text where a number is expected."},
        {L"RANGE", L"Sayı SQL tipinin aralığı dışında.", L"Number outside its SQL type range."},
        {L"LEN", L"Metin SQL varchar sınırından uzun.", L"Text longer than its SQL varchar."},
        {L"FILE", L"Başvurulan dosya istemci arşivinde yok.", L"Referenced file missing in the client archives."},
        {L"REF", L"Başka tabloya başvuru bulunamadı (isim, skill ID, ses nesnesi).", L"Cross-table reference not found."},
        {L"COLS", L"Sütun sayısı farklı.", L"Column count differs."},
        {L"ID", L"Aynı ID iki satırda.", L"Same ID twice."},
        {L"DUPID", L"Aynı ID farklı dosyalarda (characterdata + itemdata ortak ID alanı).", L"Same ID in different files (shared ID space)."},
        {L"DUPCODE", L"Aynı kod adı farklı dosyalarda.", L"Same codename in different files."},
        {L"BUCKET", L"ID dosya aralığının dışında (dosya N: N-5000..N-1).", L"ID outside the file's range."},
        {L"IDX", L"Dizin dosyasındaki tablo yok.", L"Table listed in an index file is missing."},
        {L"IDX2", L"Bölünmüş tablo dizin dosyasında yok: istemci yüklemez.", L"Split table not in its index file: never loaded."},
        {L"SKENC", L"Düz skilldata ENC'den farklı: istemci yalnız ENC okur.", L"Plain skilldata differs from ENC: the client reads ENC only."},
        {L"SND15", L".bsr ses setinde 15'ten fazla iz: istemci assert (MAX_SOUND_TRACK_PER_ANI).", L".bsr sound set over 15 tracks: client assert."},
        {L"SND11", L".bsr ses setinde 11'den fazla iz (sevkiyat en çok 11).", L".bsr sound set over 11 tracks (shipped max 11)."},
        {L"BSR", L".bsr okunamadı.", L".bsr unreadable."},
        {L"ENC", L"Dosya kodlaması yanlış.", L"Wrong file encoding."},
        {L"EMPTY", L"Normalde boş kalan sütun dolu.", L"A normally empty column holds a value."},
        {L"REQ", L"Zorunlu hücre boş.", L"Required cell empty."},
        {L"CODE", L"Kod adında boşluk.", L"Space in a codename."},
        {L"EXT", L"Beklenmeyen uzantı.", L"Unexpected extension."},
        {L"V3", L"Yolda '/' (\\ olmalı).", L"'/' in a path (use \\)."},
    };
    for (const auto& h : k)
        if (c == h.code) return en ? h.en : h.tr;
    return L"";
}

void MarkNew(HealthReport& r, const HealthBaseline* base) {
    std::vector<char> matched(r.issues.size(), 0);
    if (base) {
        auto exact = base->exact;  // counts are consumed
        auto coarse = base->coarse;
        for (size_t k = 0; k < r.issues.size(); ++k) {  // pass 1: same table / rule / key / column / text
            auto it = exact.find(SigExact(r.issues[k]));
            if (it == exact.end() || it->second <= 0) continue;
            --it->second;
            matched[k] = 1;
            auto c = coarse.find(SigCoarse(r.issues[k]));
            if (c != coarse.end() && c->second > 0) --c->second;
        }
        for (size_t k = 0; k < r.issues.size(); ++k) {  // pass 2: the text changed (a count, a line number in it)
            if (matched[k]) continue;
            auto c = coarse.find(SigCoarse(r.issues[k]));
            if (c == coarse.end() || c->second <= 0) continue;
            --c->second;
            matched[k] = 1;
        }
    }
    // errors the client tolerates: a cell read as 0 / -1, a value it ignores, a row it skips; the game runs with them
    static const std::set<std::wstring> tolerated = {L"TYPE", L"ENUM", L"COLS", L"RANGE", L"CODE", L"R6", L"V15", L"V13", L"REF", L"FILE",
                                                     L"V5", L"V6", L"V7", L"V8", L"V9", L"SKENC", L"R4"};
    r.errors = r.warns = r.infos = 0;
    r.mineErrors = r.mineWarns = r.mineInfos = 0;
    r.newCrash = 0;
    r.compared = base != nullptr;
    for (size_t k = 0; k < r.issues.size(); ++k) {
        HealthIssue& i = r.issues[k];
        i.stock = !base || matched[k];
        i.isNew = !i.stock;
        if (i.stock && i.sev == Sev::Error && tolerated.count(i.code)) {
            i.sev = Sev::Warn;
            i.softened = true;
        }
        (i.sev == Sev::Error ? r.errors : i.sev == Sev::Warn ? r.warns : r.infos)++;
        if (i.isNew) {
            (i.sev == Sev::Error ? r.mineErrors : i.sev == Sev::Warn ? r.mineWarns : r.mineInfos)++;
            if (ImpactOf(i) == Impact::Crash) ++r.newCrash;
        }
    }
}

void MarkStock(HealthReport& r, const HealthReport* base) {
    if (!base) {
        MarkNew(r, nullptr);
        return;
    }
    const HealthBaseline b = HealthBaseline::From(*base);
    MarkNew(r, &b);
}

HealthReport CheckClient(const ClientBase& c, const HealthOpts& o) {
    const auto t0 = std::chrono::steady_clock::now();
    HealthReport rep;
    rep.client = c.Describe(false);
    Sink sink{rep, o};
    auto prog = [&](int p, const std::wstring& w) {
        if (o.progress) o.progress(p, w);
    };
    auto cancelled = [&] { return o.cancel && o.cancel->load(); };
    Workspace ws(&c);
    RefIndex ref;
    std::wstring log;
    prog(1, L"başvuru tabloları / reference tables");
    if (o.refs) BuildRefIndex(ws, ref, &log);
    ref.checkFiles = o.files;
    if (o.files) ref.exists = ClientExists(c);
    else ref.exists = nullptr;
    const auto names = ListTextdata(c);
    std::unordered_set<std::wstring> only;
    for (const auto& n : o.only) only.insert(Lower(n));
    std::unordered_map<std::wstring, size_t> rowsOf;  // lower name -> rows
    size_t idx = 0;
    for (const auto& name : names) {
        ++idx;
        if (cancelled()) break;
        if (!only.empty() && !only.count(Lower(name))) continue;
        prog(5 + static_cast<int>(idx * 75 / std::max<size_t>(names.size(), 1)), name);
        EdDoc temp;
        const EdDoc* d = nullptr;
        const int id = ws.Find(name);
        if (id >= 0) d = ws.Doc(id);
        else {
            Bytes raw;
            std::wstring err;
            if (!c.ReadTextdata(name, raw)) {
                sink.Add(Sev::Error, name, L"IDX", L"Tablo okunamadı.", L"Table unreadable.");
                continue;
            }
            if (!temp.Load(name, raw, err)) {
                sink.Add(Sev::Error, name, L"ENC", L"Yüklenemedi: " + err, L"Cannot load: " + err);
                continue;
            }
            d = &temp;
        }
        ++rep.tables;
        rep.rows += static_cast<int>(d->rows.size());
        rowsOf[Lower(name)] = d->rows.size();
        const auto issues = CheckDoc(*d, -1, o.refs ? &ref : nullptr);
        for (const auto& ci : issues) {
            HealthIssue h;
            h.sev = ci.sev;
            h.table = name;
            h.line = ci.line;
            h.col = ci.col;
            h.code = ci.code;
            h.msgTr = ci.msgTr;
            h.msgEn = ci.msgEn;
            if (ci.uid) {
                const EdRow* r = d->Row(ci.uid);
                if (r) {
                    const Schema& s = d->SchemaOf(*r);
                    if (ci.col >= 0 && s.Col(static_cast<size_t>(ci.col))) h.colName = s.cols[static_cast<size_t>(ci.col)].name;
                    if (s.keyCol >= 0 && static_cast<size_t>(s.keyCol) < r->cells.size()) h.key = Trim(r->cells[static_cast<size_t>(s.keyCol)]);
                }
            }
            sink.Add(std::move(h));
        }
        if (d->kind == TKind::IndexFile) {
            for (const auto& r : d->rows) {
                if (r.cells.empty()) continue;
                const std::wstring f = Trim(r.cells[0]);
                if (!f.empty() && !c.HasTextdata(f))
                    sink.Add(Sev::Error, name, L"IDX", L"Listelenen tablo yok: " + f, L"Listed table missing: " + f, r.line, f);
            }
        }
    }
    // split files that their index does not list (never loaded by the client)
    prog(82, L"dizinler / index files");
    struct IdxRule {
        const wchar_t* index;
        const wchar_t* prefix;
        bool enc;
    };
    const IdxRule rules[] = {{L"characterdata.txt", L"characterdata_", false},
                             {L"itemdata.txt", L"itemdata_", false},
                             {L"skilldataenc.txt", L"skilldata_", true}};
    for (const auto& ru : rules) {
        Bytes raw;
        if (!c.ReadTextdata(ru.index, raw)) continue;
        std::unordered_set<std::wstring> listed;
        for (const auto& f : ReadIndexList(raw)) listed.insert(Lower(f));
        for (const auto& n : names) {
            const std::wstring l = Lower(n);
            if (!PrefixDigits(l, ru.prefix)) continue;
            if (EndsWithI(l, L"enc.txt") != ru.enc) continue;
            if (!listed.count(l))
                sink.Add(Sev::Warn, n, L"IDX2", std::wstring(L"Bu dosya ") + ru.index + L" içinde yok: istemci yüklemez.",
                         std::wstring(L"Not listed in ") + ru.index + L": never loaded.");
        }
    }
    // plain skilldata vs ENC (row counts)
    {
        size_t plain = 0, enc = 0;
        for (const auto& [n, k] : rowsOf) {
            if (!PrefixDigits(n, L"skilldata_")) continue;
            (EndsWithI(n, L"enc.txt") ? enc : plain) += k;
        }
        if (plain && enc && plain != enc)
            sink.Add(Sev::Info, L"skilldata_*.txt", L"SKENC",
                     Fmt(L"Düz skilldata %zu satır, ENC %zu satır: istemci yalnız ENC okur; düzenlemeyi ENC'de yapın.", plain, enc),
                     Fmt(L"Plain skilldata %zu rows, ENC %zu: the client reads ENC only.", plain, enc));
    }
    // duplicate IDs / codes across split files (characterdata + itemdata share _RefObjCommon.ID)
    if (o.refs) {
        prog(85, L"ID / kod çakışmaları / ID collisions");
        std::unordered_map<long long, std::wstring> idFile;
        std::unordered_map<std::wstring, std::wstring> codeFile;
        for (const auto& d : ws.docs) {
            if (d->kind != TKind::CharacterData && d->kind != TKind::ItemData) continue;
            for (const auto& r : d->rows) {
                if (r.cells.size() < 3) continue;
                long long id = 0;
                if (ParseInt(r.cells[1], id)) {
                    auto [it, ins] = idFile.emplace(id, d->name);
                    if (!ins && it->second != d->name)
                        sink.Add(Sev::Error, d->name, L"DUPID", Fmt(L"ID %lld %ls içinde de var.", id, it->second.c_str()),
                                 Fmt(L"ID %lld also in %ls.", id, it->second.c_str()), r.line, r.cells[2]);
                }
                auto [it2, ins2] = codeFile.emplace(r.cells[2], d->name);
                if (!ins2 && it2->second != d->name)
                    sink.Add(Sev::Error, d->name, L"DUPCODE", L"Kod " + r.cells[2] + L" " + it2->second + L" içinde de var.",
                             L"Code " + r.cells[2] + L" also in " + it2->second + L".", r.line, r.cells[2]);
            }
        }
    }
    // creature models: .bsr sound tracks (<= 15) and the AniShot roles of their skills (V13)
    if (o.bsrSounds && c.HasArchive(Arc::Data) && !cancelled()) {
        std::unordered_map<std::wstring, std::vector<std::wstring>> aniShotOf;  // presentation key -> ANI tokens
        const int se = ws.Find(L"skilleffect.txt");
        if (se >= 0)
            for (const auto& r : ws.Doc(se)->rows)
                if (!r.deleted && r.kind == TKind::SeAniSet && r.cells.size() > 9) {
                    std::vector<std::wstring> t;
                    for (auto a : SplitView(r.cells[9], L','))
                        if (a != L"none" && !a.empty()) t.emplace_back(a);
                    aniShotOf[r.cells[2]] = std::move(t);
                }
        struct Model {
            std::wstring path;
            std::vector<std::wstring> owners;
            std::vector<long long> skills;
        };
        std::vector<Model> models;
        std::unordered_map<std::wstring, size_t> at;
        for (const auto& d : ws.docs) {
            if (d->kind != TKind::CharacterData) continue;
            for (const auto& r : d->rows) {
                if (r.cells.size() < 93) continue;
                std::wstring p = r.cells[52];
                if (p.empty() || p == L"xxx" || !EndsWithI(p, L".bsr")) continue;
                if (p.find(L',') != std::wstring::npos) p = p.substr(0, p.find(L','));
                p = L"res\\" + p;
                auto [it, ins] = at.emplace(Lower(p), models.size());
                if (ins) models.push_back({p, {}, {}});
                Model& m = models[it->second];
                if (m.owners.size() < 4) m.owners.push_back(r.cells[2]);
                for (size_t k = 83; k <= 92 && k < r.cells.size(); ++k) {
                    long long sid = 0;
                    if (ParseInt(r.cells[k], sid) && sid) m.skills.push_back(sid);
                }
            }
        }
        size_t n = 0;
        for (const auto& m : models) {
            if (cancelled()) break;
            if (o.maxBsr > 0 && static_cast<int>(n) >= o.maxBsr) break;
            ++n;
            if ((n & 63) == 0) prog(86 + static_cast<int>(n * 13 / std::max<size_t>(models.size(), 1)), m.path);
            Bytes b;
            if (!c.ReadResource(Arc::Data, m.path, b)) continue;  // FILE issues come from the table pass
            BsrInfo bi;
            ++rep.bsrChecked;
            if (!ParseBsrInfo(b, bi) || !bi.err.empty()) {
                sink.Add(Sev::Warn, m.path, L"BSR", L".bsr ayrıştırılamadı: " + bi.err, L".bsr parse failed: " + bi.err, -1,
                         m.owners.empty() ? L"" : m.owners[0]);
                if (!bi.ok) continue;
            }
            for (const auto& s : bi.sounds) {
                if (s.tracks > kMaxSoundTrackPerAni)
                    sink.Add(Sev::Error, m.path, L"SND15",
                             Fmt(L"Ses seti '%ls' (%ls %ls) %d iz > 15: istemci assert verir.", s.sndName.c_str(), s.setName.c_str(),
                                 AniRoleName(s.aniType).c_str(), s.tracks),
                             Fmt(L"Sound set '%ls' has %d tracks > 15: client assert.", s.sndName.c_str(), s.tracks), -1,
                             m.owners.empty() ? L"" : m.owners[0]);
                else if (s.tracks > 11)
                    sink.Add(Sev::Info, m.path, L"SND11", Fmt(L"Ses seti '%ls' %d iz (sevkiyat en çok 11).", s.sndName.c_str(), s.tracks),
                             Fmt(L"Sound set '%ls' has %d tracks (shipped max 11).", s.sndName.c_str(), s.tracks), -1,
                             m.owners.empty() ? L"" : m.owners[0]);
            }
            if (o.refs && !aniShotOf.empty()) {
                std::set<int> roles;
                for (const auto& r : bi.roles) roles.insert(static_cast<int>(r.role));
                for (long long sid : m.skills) {
                    auto sc = ref.skillById.find(sid);
                    if (sc == ref.skillById.end()) continue;
                    std::wstring key = sc->second;
                    auto g = ref.groupOfCode.find(key);
                    if (g != ref.groupOfCode.end() && g->second != L"xxx" && !g->second.empty()) key = g->second;
                    auto a = aniShotOf.find(key);
                    if (a == aniShotOf.end()) continue;
                    for (const auto& tok : a->second) {
                        const int rid = AniRoleId(tok);
                        if (rid >= 0 && !roles.empty() && !roles.count(rid))
                            sink.Add(Sev::Warn, m.path, L"V13",
                                     L"Beceri " + key + L" AniShot " + tok + L": modelde bu animasyon yok (" +
                                         (m.owners.empty() ? L"" : m.owners[0]) + L").",
                                     L"Skill " + key + L" AniShot " + tok + L": no such animation in the model.", -1, key);
                    }
                }
            }
        }
    }
    std::stable_sort(rep.issues.begin(), rep.issues.end(), [](const HealthIssue& a, const HealthIssue& b) {
        if (a.code != b.code) return a.code < b.code;
        if (a.sev != b.sev) return a.sev > b.sev;
        if (a.table != b.table) return a.table < b.table;
        return a.line < b.line;
    });
    prog(100, L"");
    rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return rep;
}

std::wstring HealthReport::Text(bool en, size_t perCode) const {
    std::wstring s;
    s += en ? L"CLIENT HEALTH REPORT\r\n" : L"İSTEMCİ SAĞLIK RAPORU\r\n";
    s += (en ? L"client: " : L"istemci: ") + client + L"\r\n";
    s += Fmt(en ? L"%d tables, %d rows, %d models checked; %d errors, %d warnings, %d notes (%.1f s)%ls\r\n"
                : L"%d tablo, %d satır, %d model denetlendi; %d hata, %d uyarı, %d bilgi (%.1f sn)%ls\r\n",
             tables, rows, bsrChecked, errors, warns, infos, seconds, truncated ? (en ? L" [list truncated]" : L" [liste kırpıldı]") : L"");
    if (compared)
        s += Fmt(en ? L"NEW (not in the client without the output folder): %d errors, %d warnings, %d notes; %d of class \"%ls\"\r\n"
                    : L"YENİ (çıktı klasörü olmadan istemcide yok): %d hata, %d uyarı, %d bilgi; %d tanesi \"%ls\" sınıfında\r\n",
                 mineErrors, mineWarns, mineInfos, newCrash, ImpactName(Impact::Crash, en));
    s += en ? L"\r\nBY RULE\r\n" : L"\r\nKURALA GÖRE\r\n";
    for (const auto& [code, n] : byCode)
        s += Fmt(L"  %-8ls %7d  [%ls] ", code.c_str(), n, ImpactName(ImpactOf(code, Sev::Error), en)) + HealthCodeHelp(code, en) + L"\r\n";
    s += en ? L"\r\nTABLES WITH MOST PROBLEMS\r\n" : L"\r\nEN ÇOK SORUNLU TABLOLAR\r\n";
    std::vector<std::pair<int, std::wstring>> top;
    for (const auto& [t, n] : byTable) top.push_back({n, t});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 15; ++i) s += Fmt(L"  %7d  %ls\r\n", top[i].first, top[i].second.c_str());
    std::wstring cur;
    size_t shown = 0;
    for (const auto& i : issues) {
        if (i.code != cur) {
            cur = i.code;
            shown = 0;
            s += L"\r\n== " + i.code + L"  " + HealthCodeHelp(i.code, en) + L"\r\n";
        }
        if (shown++ >= perCode) {
            if (shown == perCode + 1) s += en ? L"  ... (more in the full list)\r\n" : L"  ... (devamı tam listede)\r\n";
            continue;
        }
        s += Fmt(L"  %ls[%ls] [%ls] %ls", i.isNew ? L"* " : L"", SevName(i.sev, en), ImpactName(ImpactOf(i), en), i.table.c_str());
        if (i.line >= 0) s += Fmt(L":%d", i.line + 1);
        if (!i.colName.empty()) s += L" {" + i.colName + L"}";
        if (!i.key.empty()) s += L" " + i.key;
        s += L" — " + (en ? i.msgEn : i.msgTr) + L"\r\n";
    }
    return s;
}

bool HealthReport::Write(const std::wstring& path, bool en, std::wstring& err) const {
    std::string u = "\xEF\xBB\xBF" + ToUtf8(Text(en, 1000000));
    Bytes b(u.begin(), u.end());
    return WriteFileAtomic(path, b, err);
}

}  // namespace se::ed
