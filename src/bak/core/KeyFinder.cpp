// ============================================================================
//  SRO Asset Studio - core/KeyFinder.cpp
//
//  The key discovery subsystem (SPEC.md §2).
//
//  RULE 0 - nothing server-specific is hardcoded. The only key constants that
//           exist anywhere in this program are the format-level ones (the
//           default ascii key and the three published base keys). Everything
//           else is DERIVED at runtime from the archive and from the client.
//  RULE 1 - no key material is ever written to disk, not even a recovered raw
//           (P,S) state. Discovered keys live in SessionKeys (memory) and every
//           scratch buffer that held key material is wiped before release.
//
//  ---------------------------------------------------------------------------
//  ORDER OF ATTEMPTS
//
//    0.1  session memory        instant, launches nothing
//    0.2  sibling archive keys  instant, launches nothing
//    1    typed key x bases     instant, launches nothing
//    2    static binary scan    seconds, launches nothing, usually finds
//                               nothing against MaxiGuard-class protection
//    0.3  ALREADY-RUNNING clientseconds, launches nothing
//    3    launch a client       ONLY with opt.allowLaunchClient
//
//  Stage 0.3 sits after stage 2 for one reason: stage 2 is bounded and cheap
//  while scanning a live process costs seconds. Neither launches anything, so
//  the "nothing was started" guarantee of stage 0 is preserved either way, and
//  the log always names the stage that won.
//
//  ---------------------------------------------------------------------------
//  SCANNER A - key material (ascii key + base key)
//    Anchor on the locator strings, harvest NUL-terminated printable runs
//    within +-4 KB of a hit, then for each run try {STD, PARADISE, ZERO} and
//    the bytes immediately preceding it (the verified MaxiGuard layout is
//    <base key><0..8 zero pad><ascii key NUL>). Widening to the whole address
//    space is the fallback, not the plan.
//
//  SCANNER B - raw scheduled Blowfish state (KEY-DERIVATION INDEPENDENT)
//    Every 4-byte-aligned 4168-byte window is treated as { u32 P[18];
//    u32 S[4][256]; } and used to decrypt the archive's own root block. This is
//    what opens an archive whose key derivation nobody knows. It is the hot
//    loop of the whole program, so:
//      * a running zero-density filter rejects almost every window for ~8 ops
//        (a scheduled state is pseudorandom: ~0.25 zero bytes per 64, so the
//        "more than 8 zeros in the first 64" reject cannot lose a real hit),
//      * survivors decrypt exactly ONE 8-byte block and must produce 01 2E 00
//        before anything else happens,
//      * only then is the full 2560-byte block decrypted and checked.
//    Both (P,S) and the reversed (S,P) layout are tested.
//
//  Implementation owner: Agent C (key discovery).
// ============================================================================
#include "KeyFinder.h"
#include "Pk2Archive.h"
#include "Config.h"

#include <tlhelp32.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace sro {

// ===========================================================================
//  Local helpers
// ===========================================================================
namespace {

// --- little-endian unaligned load ------------------------------------------
inline u32 LdU32(const u8* p) { u32 v; std::memcpy(&v, p, sizeof(v)); return v; }

// --- Blowfish F() over a RAW S-box window (4 x 256 x u32) ------------------
inline u32 RawF(const u8* S, u32 x) {
    const u32 a = LdU32(S +           ((static_cast<u32>(x >> 24) & 0xFFu) << 2));
    const u32 b = LdU32(S + 1024u +   ((static_cast<u32>(x >> 16) & 0xFFu) << 2));
    const u32 c = LdU32(S + 2048u +   ((static_cast<u32>(x >>  8) & 0xFFu) << 2));
    const u32 d = LdU32(S + 3072u +   ((static_cast<u32>(x)       & 0xFFu) << 2));
    return ((a + b) ^ c) + d;
}

/// One 8-byte decipher straight out of a candidate (P,S) window - no Blowfish
/// object, no copying. Mirrors Blowfish_decipher of the reference C# port:
///   Xl ^= P[17]; ROUND(Xr,Xl,16); ROUND(Xl,Xr,15); ... ROUND(Xl,Xr,1);
///   Xr ^= P[0]; out = (Xr, Xl)
inline void RawDecipher(const u8* P, const u8* S, u32 inL, u32 inR, u32& outL, u32& outR) {
    u32 Xl = inL, Xr = inR;
    Xl ^= LdU32(P + 17 * 4);
    for (int n = 16; n >= 2; n -= 2) {
        Xr ^= RawF(S, Xl) ^ LdU32(P + n * 4);
        Xl ^= RawF(S, Xr) ^ LdU32(P + (n - 1) * 4);
    }
    Xr ^= LdU32(P + 0);
    outL = Xr;
    outR = Xl;
}

/// Scanner B inner test: decrypt ONLY the first 8 bytes of the root block and
/// require the plaintext to start 01 2E 00 (Folder entry named ".").
inline bool RawStateFastOracle(const u8* P, const u8* S, u32 encL, u32 encR) {
    u32 l = 0, r = 0;
    RawDecipher(P, S, encL, encR, l, r);
    (void)r;
    return (l & 0x00FFFFFFu) == 0x00002E01u;
}

/// Zero bytes in [p, p+n). Used by the Scanner B pre-filter.
inline size_t CountZeros(const u8* p, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; ++i) c += (p[i] == 0) ? 1u : 0u;
    return c;
}

constexpr size_t kZeroFilterWindow = 64;   ///< bytes examined by the pre-filter
constexpr size_t kZeroFilterMax    = 8;    ///< reject above this many zeros

// --- work caps (a scan must always terminate) ------------------------------
constexpr size_t kMaxAnchorHitsPerLocator = 4096;
constexpr size_t kMaxAnchoredCandidates   = 20000;
constexpr size_t kMaxSweepCandidates      = 100000;
/// The +-512 byte window pass is quadratic-ish; only run it when the anchored
/// candidate list is small enough for it to stay under a second or two.
constexpr size_t kNearWindowCandidateCap  = 2000;

// --- misc -------------------------------------------------------------------
inline unsigned ResolveThreadCount(unsigned requested) {
    if (requested > 0) return requested;
    const unsigned hc = std::thread::hardware_concurrency();
    return hc ? hc : 4u;
}

/// Case-sensitive byte search (locator strings are exact engine strings).
void FindAllOccurrences(const u8* buf, size_t size, const char* needle,
                        size_t maxHits, std::vector<u64>& out) {
    const size_t n = std::strlen(needle);
    if (n == 0 || size < n) return;
    const u8 first = static_cast<u8>(needle[0]);
    size_t i = 0;
    while (i + n <= size) {
        const u8* p = static_cast<const u8*>(std::memchr(buf + i, first, size - n - i + 1));
        if (!p) return;
        const size_t off = static_cast<size_t>(p - buf);
        if (std::memcmp(p, needle, n) == 0) {
            out.push_back(static_cast<u64>(off));
            if (out.size() >= maxHits) return;
        }
        i = off + 1;
    }
}

/// Merge [center-half, center+half] intervals into a sorted, non-overlapping list.
void BuildWindows(const std::vector<u64>& centers, u64 half, size_t size,
                  std::vector<std::pair<size_t, size_t>>& out) {
    out.clear();
    if (centers.empty() || size == 0) return;
    std::vector<std::pair<size_t, size_t>> raw;
    raw.reserve(centers.size());
    for (u64 c : centers) {
        const size_t lo = (c > half) ? static_cast<size_t>(c - half) : 0u;
        size_t hi = static_cast<size_t>((std::min)(static_cast<u64>(size), c + half));
        if (hi <= lo) continue;
        raw.push_back({ lo, hi });
    }
    std::sort(raw.begin(), raw.end());
    for (const auto& iv : raw) {
        if (!out.empty() && iv.first <= out.back().second) {
            if (iv.second > out.back().second) out.back().second = iv.second;
        } else {
            out.push_back(iv);
        }
    }
}

/// Wipe a buffer that may have held key material and release it.
void WipeAndFree(std::vector<u8>& v) {
    if (!v.empty()) ::SecureZeroMemory(v.data(), v.size());
    std::vector<u8>().swap(v);
}

} // namespace

// ===========================================================================
//  Locator strings - real. These mark protection/engine code, not a shard.
//  "Wrong BlowfishKey" anchored the hit on both real clients tested, so it is
//  first.
// ===========================================================================
const std::vector<KeyLocator>& KeyMaterialLocators() {
    static const std::vector<KeyLocator> locators = {
        { "Wrong BlowfishKey",  L"\"Wrong BlowfishKey\" hata metni" },
        { "MaxiGuard DLL:",     L"MaxiGuard modul metni" },
        { "GFXFileManager.dll", L"GFXFileManager referansi" },
        { ".?AVCBlowFish@@",    L"CBlowFish RTTI adi" }
    };
    return locators;
}

// ===========================================================================
//  Stage labels - load bearing: the log must always say which path ran so the
//  user knows whether a client was started.
// ===========================================================================
const wchar_t* KeyFindStageNameTr(KeyFindStage stage) {
    switch (stage) {
        case KeyFindStage::Stage0SessionMemory:  return L"Oturum belleği (istemci başlatılmadı)";
        case KeyFindStage::Stage0SiblingArchive: return L"Aynı klasördeki başka arşivin anahtarı (istemci başlatılmadı)";
        case KeyFindStage::Stage0RunningClient:  return L"Zaten çalışan istemci (yeni istemci başlatılmadı)";
        case KeyFindStage::Stage1Typed:          return L"Yazılan key x base key";
        case KeyFindStage::Stage1RawKey:         return L"Yazılan key doğrudan (raw)";
        case KeyFindStage::Stage2StaticBaseKey:  return L"Disk taraması: base key";
        case KeyFindStage::Stage2StaticAsciiKey: return L"Disk taraması: ascii key";
        case KeyFindStage::Stage3ScannerA:       return L"Bellek taraması: ascii + base key (istemci başlatıldı)";
        case KeyFindStage::Stage3ScannerB:       return L"Bellek taraması: ham Blowfish durumu (istemci başlatıldı)";
        default:                                 return L"Bulunamadı";
    }
}

bool KeyFindStageLaunchesClient(KeyFindStage stage) {
    return stage == KeyFindStage::Stage3ScannerA || stage == KeyFindStage::Stage3ScannerB;
}

// ===========================================================================
//  Client executable ranking
// ===========================================================================
bool KeyFinder::IsLauncherLikeName(const std::wstring& fileName) {
    // Demoted, NOT rejected: some shards only unpack the key in the launcher
    // or the patcher, so these stay in the list as later attempts.
    static const wchar_t* kDemoted[] = {
        L"launcher", L"patch", L"uninstall", L"setup", L"dllsaver",
        L"replacer", L"autologin", L"config", L"crash",
        L"manager", L"tool", L"pk2", L"editor"
    };
    const std::wstring lower = ToLowerW(fileName);
    for (const wchar_t* d : kDemoted)
        if (lower.find(d) != std::wstring::npos) return true;
    return false;
}

bool KeyFinder::IsSelfExecutable(const std::wstring& path) {
    wchar_t self[MAX_PATH * 2] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    if (n == 0) return false;
    const std::wstring selfName = ToLowerW(PathFileName(std::wstring(self, n)));
    const std::wstring name     = ToLowerW(PathFileName(path));
    if (name == selfName) return true;
    // Also refuse the exact same file by full path.
    return IEqualsW(std::wstring(self, n), path);
}

std::vector<ClientCandidate> KeyFinder::RankClientExecutables(const std::wstring& archiveDir) {
    std::vector<ClientCandidate> cands;

    auto scanDir = [&](const std::wstring& dir, int dirBonus) {
        if (dir.empty() || !DirectoryExists(dir)) return;
        for (const std::wstring& path : ListDirectory(dir, L"*.exe", true, false)) {
            // Never launch ourselves. Observed hazard: a PK2 manager tool sitting
            // in a game folder ranked second.
            if (IsSelfExecutable(path)) continue;

            const std::wstring name = ToLowerW(PathFileName(path));
            ClientCandidate c;
            c.path = path;
            c.size = GetFileSize64(path);
            c.isLauncherLike = IsLauncherLikeName(name);

            c.score = dirBonus;
            if (name.find(L"client") != std::wstring::npos)          c.score += 100;
            else if (name.find(L"game") != std::wstring::npos)       c.score += 60;
            else if (name.find(L"silkroad") != std::wstring::npos)   c.score += 55;
            else if (name.find(L"sro") != std::wstring::npos)        c.score += 50;

            // Demote rather than drop - a launcher is a valid later attempt.
            if (c.isLauncherLike) c.score -= 500;

            cands.push_back(c);
        }
    };

    scanDir(archiveDir, 20);
    scanDir(PathParent(archiveDir), 0);

    std::stable_sort(cands.begin(), cands.end(),
                     [](const ClientCandidate& a, const ClientCandidate& b) {
                         if (a.score != b.score) return a.score > b.score;
                         return a.size > b.size;   // largest remaining .exe wins ties
                     });
    return cands;
}

// ===========================================================================
//  ClientProcessGuard
//
//  The destructor terminates, so no exit path - normal, early or exceptional -
//  can leave a game client running.
// ===========================================================================
ClientProcessGuard::~ClientProcessGuard() { terminate(); }

namespace {

/// Copy the current environment block and force __COMPAT_LAYER=RunAsInvoker so
/// a client with a requireAdministrator manifest starts without a UAC prompt.
void BuildCompatEnvironment(std::vector<wchar_t>& out) {
    out.clear();
    static const wchar_t kVar[]   = L"__COMPAT_LAYER=";
    static const wchar_t kValue[] = L"__COMPAT_LAYER=RunAsInvoker";

    LPWCH block = ::GetEnvironmentStringsW();
    if (block) {
        for (const wchar_t* p = block; *p; ) {
            const size_t len = std::wcslen(p);
            const bool isCompat =
                (len >= (sizeof(kVar) / sizeof(wchar_t)) - 1) &&
                _wcsnicmp(p, kVar, (sizeof(kVar) / sizeof(wchar_t)) - 1) == 0;
            if (!isCompat) out.insert(out.end(), p, p + len + 1);
            p += len + 1;
        }
        ::FreeEnvironmentStringsW(block);
    }
    out.insert(out.end(), kValue, kValue + (sizeof(kValue) / sizeof(wchar_t)));
    out.push_back(L'\0');   // terminating double NUL
}

} // namespace

Result ClientProcessGuard::launchHidden(const std::wstring& exePath,
                                        const std::wstring& workingDir) {
    terminate();

    if (exePath.empty() || !FileExists(exePath))
        return Result::Fail(Status::NotFound, L"Istemci bulunamadi: " + exePath);
    if (KeyFinder::IsSelfExecutable(exePath))
        return Result::Fail(Status::InvalidArgument,
                            L"Kendi programimizi baslatmayiz: " + PathFileName(exePath));

    // A private desktop keeps the client's windows off the user's screen even
    // if it ignores SW_HIDE. Some clients refuse to initialise D3D there, so a
    // failure falls back to the default desktop.
    std::wstring deskName = FormatW(L"SROAssetStudioKeyScan_%u", ::GetCurrentProcessId());
    m_desktop = ::CreateDesktopW(deskName.c_str(), nullptr, nullptr, 0,
                                 GENERIC_ALL, nullptr);

    std::vector<wchar_t> env;
    BuildCompatEnvironment(env);

    std::wstring cmdline = L"\"" + exePath + L"\"";
    std::vector<wchar_t> cmd(cmdline.begin(), cmdline.end());
    cmd.push_back(L'\0');

    const std::wstring dir = workingDir.empty() ? PathParent(exePath) : workingDir;
    DWORD lastError = 0;

    for (int attempt = 0; attempt < 2; ++attempt) {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags     = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        if (attempt == 0 && m_desktop)
            si.lpDesktop = &deskName[0];

        PROCESS_INFORMATION pi{};
        const DWORD flags = DETACHED_PROCESS | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;

        if (::CreateProcessW(exePath.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                             flags, env.data(),
                             dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
            m_process = pi.hProcess;
            m_thread  = pi.hThread;
            m_pid     = pi.dwProcessId;
            m_owned   = true;
            return Result::Ok();
        }
        lastError = ::GetLastError();

        // Retry on the default desktop.
        if (m_desktop) { ::CloseDesktop(m_desktop); m_desktop = nullptr; }
    }

    return Result::Fail(Status::IoError,
                        FormatW(L"Istemci baslatilamadi (%s), Win32 hata %u",
                                PathFileName(exePath).c_str(), lastError));
}

Result ClientProcessGuard::attachExisting(DWORD pid) {
    terminate();
    if (pid == 0 || pid == ::GetCurrentProcessId())
        return Result::Fail(Status::InvalidArgument, L"Gecersiz surec kimligi.");

    HANDLE h = ::OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
                             FALSE, pid);
    if (!h)
        h = ::OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                          FALSE, pid);
    if (!h)
        return Result::Fail(Status::AccessDenied,
                            FormatW(L"Surece baglanilamadi (pid %u), Win32 hata %u",
                                    pid, ::GetLastError()));

    m_process = h;
    m_pid     = pid;
    m_owned   = false;   // NEVER terminate a process the user started
    return Result::Ok();
}

bool ClientProcessGuard::running() const {
    if (!m_process) return false;
    return ::WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
}

void ClientProcessGuard::terminate() {
    // Only ever kill a process WE started. Attached processes belong to the user.
    if (m_process) {
        if (m_owned) {
            ::TerminateProcess(m_process, 0);
            ::WaitForSingleObject(m_process, 3000);
        }
        ::CloseHandle(m_process);
        m_process = nullptr;
    }
    if (m_thread)  { ::CloseHandle(m_thread);  m_thread = nullptr; }
    if (m_desktop) { ::CloseDesktop(m_desktop); m_desktop = nullptr; }
    m_pid = 0;
    m_owned = false;
}

// ===========================================================================
//  Candidate harvesting  (shared by the static and the memory scanner)
// ===========================================================================
void KeyFinder::FindLocatorHits(const u8* buf, size_t size, std::vector<u64>& out) {
    out.clear();
    if (!buf || size == 0) return;
    for (const KeyLocator& loc : KeyMaterialLocators())
        FindAllOccurrences(buf, size, loc.ascii, kMaxAnchorHitsPerLocator, out);
}

void KeyFinder::HarvestAsciiCandidates(const u8* buf, size_t size,
                                       const std::vector<u64>& anchors,
                                       u64 windowBytes,
                                       std::vector<AsciiCandidate>& out) {
    out.clear();
    if (!buf || size == 0) return;

    std::vector<std::pair<size_t, size_t>> ranges;
    if (anchors.empty()) {
        ranges.push_back({ 0u, size });
    } else {
        BuildWindows(anchors, windowBytes, size, ranges);
        if (ranges.empty()) return;
    }

    for (const auto& range : ranges) {
        size_t i = range.first;
        // Do not start mid-run: back up to the beginning of the run we land in.
        while (i > 0 && IsKeyChar(buf[i - 1])) --i;

        while (i < range.second) {
            if (!IsKeyChar(buf[i])) { ++i; continue; }
            const size_t start = i;
            while (i < size && IsKeyChar(buf[i])) ++i;
            const size_t len = i - start;

            // Must be NUL terminated (the verified layout stores a C string).
            if (i >= size || buf[i] != 0) continue;

            if (len >= kMinAsciiKeyLen && len <= kMaxAsciiKeyLen) {
                AsciiCandidate c;
                c.offset = static_cast<u64>(start);
                c.text.assign(reinterpret_cast<const char*>(buf + start), len);
                out.push_back(std::move(c));
            } else if (len > kMaxAsciiKeyLen) {
                // A longer run may still END with the key; take its tail.
                const size_t tail = i - kMaxAsciiKeyLen;
                AsciiCandidate c;
                c.offset = static_cast<u64>(tail);
                c.text.assign(reinterpret_cast<const char*>(buf + tail), kMaxAsciiKeyLen);
                out.push_back(std::move(c));
            }
        }
    }
}

void KeyFinder::BaseKeyCandidatesBefore(const u8* buf, size_t size, u64 asciiOffset,
                                        std::vector<std::vector<u8>>& out) {
    out.clear();
    if (!buf || asciiOffset == 0 || asciiOffset > size) return;

    // The VERIFIED layout is <base key><0..8 zero pad><ascii key>. Walk back
    // over the padding one byte at a time; the padding is what makes the
    // position of the base key ambiguous, not a length prefix (see KeyFinder.h).
    for (size_t skip = 0; skip <= kMaxPaddingBytes; ++skip) {
        if (asciiOffset < skip) break;
        const size_t end = static_cast<size_t>(asciiOffset) - skip;   // exclusive
        for (size_t w : kBaseKeyWidths) {
            if (end < w) continue;
            const u8* p = buf + (end - w);
            bool allZero = true;
            for (size_t k = 0; k < w; ++k) if (p[k] != 0) { allZero = false; break; }
            if (allZero) continue;          // that is just the ZERO base, already tried
            out.push_back(std::vector<u8>(p, p + w));
        }
        // Stop as soon as the byte we would skip next is not padding.
        if (end == 0 || buf[end - 1] != 0) break;
    }
}

// ---------------------------------------------------------------------------
// Testing one ascii candidate against the archive.
// ---------------------------------------------------------------------------
namespace {

struct TargetArchive {
    const Pk2RawHeader* header = nullptr;
    const u8*           root   = nullptr;   ///< encrypted root block, 2560 bytes
};

/// Try one ascii key + explicit base key. Returns true when the archive opens.
inline bool TryAsciiBase(const std::string& ascii, const u8* base, size_t baseLen,
                         const TargetArchive& tgt, u64& tests) {
    Blowfish bf;
    if (!bf.initializeGameKey(ascii, base, baseLen)) return false;
    ++tests;
    if (!KeyRing::RootBlockOracleFast(bf, tgt.root)) return false;
    return KeyRing::RootBlockOracle(bf, tgt.root);
}

inline bool TryAsciiBaseId(const std::string& ascii, BaseKeyId id,
                           const TargetArchive& tgt, u64& tests) {
    Blowfish bf;
    if (!bf.initializeGameKey(ascii, id)) return false;
    ++tests;
    if (!KeyRing::RootBlockOracleFast(bf, tgt.root)) return false;
    return KeyRing::RootBlockOracle(bf, tgt.root);
}

/// Steps 2-4 of the harvesting algorithm for ONE ascii candidate.
bool TestOneAsciiCandidate(const u8* buf, size_t size,
                           const KeyFinder::AsciiCandidate& cand,
                           const TargetArchive& tgt,
                           bool doNearWindow,
                           KeyCandidate& out,
                           u64& tests) {
    // Step 2: the three published base keys (ZERO also covers "raw key").
    for (const auto& kb : KeyRing::KnownBaseKeys()) {
        if (TryAsciiBaseId(cand.text, kb.first, tgt, tests)) {
            out = KeyCandidate();
            out.asciiKey = cand.text;
            out.baseId   = kb.first;
            out.label    = L"taramada bulunan ascii key";
            return true;
        }
    }

    // Step 3: the bytes immediately preceding the candidate (verified layout).
    std::vector<std::vector<u8>> bases;
    KeyFinder::BaseKeyCandidatesBefore(buf, size, cand.offset, bases);
    for (const std::vector<u8>& b : bases) {
        if (TryAsciiBase(cand.text, b.data(), b.size(), tgt, tests)) {
            out = KeyCandidate();
            out.asciiKey   = cand.text;
            out.baseId     = BaseKeyId::Custom;
            out.customBase = b;
            out.label      = L"ascii key'in hemen oncesindeki base key";
            return true;
        }
    }

    // Step 4: every 4-byte-aligned 10-byte window within +-512 bytes.
    if (doNearWindow) {
        const size_t half = KeyFinder::kNearWindowBytes;
        const size_t lo   = (cand.offset > half) ? static_cast<size_t>(cand.offset - half) : 0u;
        size_t hi = static_cast<size_t>((std::min)(static_cast<u64>(size),
                                                   cand.offset + half));
        if (hi >= kBaseKeyLength) hi -= kBaseKeyLength; else hi = 0;
        for (size_t off = (lo + 3u) & ~static_cast<size_t>(3u); off <= hi; off += 4) {
            const u8* p = buf + off;
            bool allZero = true;
            for (size_t k = 0; k < kBaseKeyLength; ++k) if (p[k]) { allZero = false; break; }
            if (allZero) continue;
            if (TryAsciiBase(cand.text, p, kBaseKeyLength, tgt, tests)) {
                out = KeyCandidate();
                out.asciiKey   = cand.text;
                out.baseId     = BaseKeyId::Custom;
                out.customBase = std::vector<u8>(p, p + kBaseKeyLength);
                out.label      = L"ascii key yakinindaki base key penceresi";
                return true;
            }
        }
    }
    return false;
}

/// Run TestOneAsciiCandidate over a candidate list on `threads` threads.
bool TestCandidatesParallel(const u8* buf, size_t size,
                            const std::vector<KeyFinder::AsciiCandidate>& cands,
                            const TargetArchive& tgt,
                            bool doNearWindow,
                            unsigned threads,
                            KeyCandidate& out,
                            u64* hitOffset,
                            std::atomic<u64>& oracleTests) {
    if (cands.empty()) return false;
    threads = ResolveThreadCount(threads);
    if (threads > cands.size()) threads = static_cast<unsigned>(cands.size());
    if (threads == 0) threads = 1;

    std::atomic<bool> found(false);
    std::atomic<size_t> next(0);
    std::mutex publish;
    KeyCandidate winner;
    u64 winnerOffset = 0;

    auto worker = [&]() {
        u64 localTests = 0;
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= cands.size() || found.load(std::memory_order_relaxed)) break;
            KeyCandidate local;
            if (TestOneAsciiCandidate(buf, size, cands[i], tgt, doNearWindow, local, localTests)) {
                std::lock_guard<std::mutex> lock(publish);
                if (!found.exchange(true)) {
                    winner = local;
                    winnerOffset = cands[i].offset;
                }
                break;
            }
        }
        oracleTests.fetch_add(localTests);
    };

    if (threads == 1) {
        worker();
    } else {
        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
        for (std::thread& th : pool) th.join();
    }

    if (found.load()) {
        out = winner;
        if (hitOffset) *hitOffset = winnerOffset;
        return true;
    }
    return false;
}

/// Drop duplicate key strings - process memory is full of repeated literals.
void DedupeCandidates(std::vector<KeyFinder::AsciiCandidate>& cands, size_t cap) {
    std::unordered_set<std::string> seen;
    seen.reserve(cands.size() * 2 + 16);
    std::vector<KeyFinder::AsciiCandidate> kept;
    kept.reserve((std::min)(cands.size(), cap));
    for (auto& c : cands) {
        if (kept.size() >= cap) break;
        if (seen.insert(c.text).second) kept.push_back(std::move(c));
    }
    cands.swap(kept);
}

} // namespace

bool KeyFinder::HarvestAndTest(const u8* buf, size_t size,
                               const Pk2RawHeader& header,
                               const u8* encryptedRootBlock,
                               unsigned threadCount,
                               bool allowFullWindowSweep,
                               KeyCandidate& out,
                               u64* hitOffset,
                               u64* oracleTests) {
    if (hitOffset) *hitOffset = 0;
    std::atomic<u64> tests(0);
    bool hit = false;

    if (buf && size >= kMinAsciiKeyLen && encryptedRootBlock) {
        TargetArchive tgt;
        tgt.header = &header;
        tgt.root   = encryptedRootBlock;

        // --- anchored pass: the measured-fast path --------------------------
        std::vector<u64> anchors;
        FindLocatorHits(buf, size, anchors);

        std::vector<AsciiCandidate> cands;
        if (!anchors.empty()) {
            HarvestAsciiCandidates(buf, size, anchors, 4ull * 1024, cands);
            DedupeCandidates(cands, kMaxAnchoredCandidates);
            hit = TestCandidatesParallel(buf, size, cands, tgt, false,
                                         threadCount, out, hitOffset, tests);
            if (!hit && cands.size() <= kNearWindowCandidateCap) {
                hit = TestCandidatesParallel(buf, size, cands, tgt, true,
                                             threadCount, out, hitOffset, tests);
            }
        }

        // --- widened pass: the fallback, not the plan -----------------------
        if (!hit && allowFullWindowSweep) {
            std::vector<AsciiCandidate> all;
            HarvestAsciiCandidates(buf, size, {}, 0, all);
            DedupeCandidates(all, kMaxSweepCandidates);
            hit = TestCandidatesParallel(buf, size, all, tgt, false,
                                         threadCount, out, hitOffset, tests);
        }
    }

    if (oracleTests) *oracleTests = tests.load();
    return hit;
}

// ===========================================================================
//  Scanner B - raw scheduled Blowfish state
// ===========================================================================
bool KeyFinder::ScanBufferForBlowfishState(const u8* buf, size_t size,
                                           const u8* encryptedRootBlock,
                                           bool tryReversed,
                                           unsigned threadCount,
                                           BlowfishState& out,
                                           u64* hitOffset,
                                           bool* wasReversed) {
    if (hitOffset)   *hitOffset = 0;
    if (wasReversed) *wasReversed = false;
    if (!buf || !encryptedRootBlock || size < kBlowfishStateSize) return false;

    const u32 encL = LdU32(encryptedRootBlock + 0);
    const u32 encR = LdU32(encryptedRootBlock + 4);

    const size_t lastOffset = size - kBlowfishStateSize;      // inclusive
    const size_t stepCount  = lastOffset / 4 + 1;             // 4-byte aligned offsets

    unsigned threads = ResolveThreadCount(threadCount);
    if (threads == 0) threads = 1;
    if (stepCount < 4096) threads = 1;

    std::atomic<bool> found(false);
    std::mutex publish;
    size_t   winnerOffset = 0;
    bool     winnerReversed = false;

    auto worker = [&](size_t firstStep, size_t lastStep) {
        if (firstStep > lastStep) return;
        size_t off = firstStep * 4;

        // Running zero density over the first kZeroFilterWindow bytes of the
        // window. A scheduled (P,S) state is pseudorandom, so more than
        // kZeroFilterMax zeros there cannot be a real hit - but almost all of
        // a process's memory is rejected by it for a handful of instructions.
        size_t zeros = CountZeros(buf + off, kZeroFilterWindow);

        for (size_t step = firstStep; step <= lastStep; ++step, off += 4) {
            if ((step & 0xFFFFu) == 0 && found.load(std::memory_order_relaxed)) return;

            if (zeros <= kZeroFilterMax) {
                const u8* P = buf + off;
                const u8* S = P + 18 * 4;
                if (RawStateFastOracle(P, S, encL, encR)) {
                    Blowfish bf;
                    if (bf.initializeFromStateBytes(P, kBlowfishStateSize, false) &&
                        KeyRing::RootBlockOracle(bf, encryptedRootBlock)) {
                        std::lock_guard<std::mutex> lock(publish);
                        if (!found.exchange(true)) {
                            winnerOffset = off;
                            winnerReversed = false;
                            out = bf.state();
                        }
                        return;
                    }
                }
                if (tryReversed) {
                    // Reversed layout: S first (4096 bytes), then P (72 bytes).
                    const u8* Sr = buf + off;
                    const u8* Pr = Sr + 4096;
                    if (RawStateFastOracle(Pr, Sr, encL, encR)) {
                        Blowfish bf;
                        if (bf.initializeFromStateBytes(buf + off, kBlowfishStateSize, true) &&
                            KeyRing::RootBlockOracle(bf, encryptedRootBlock)) {
                            std::lock_guard<std::mutex> lock(publish);
                            if (!found.exchange(true)) {
                                winnerOffset = off;
                                winnerReversed = true;
                                out = bf.state();
                            }
                            return;
                        }
                    }
                }
            }

            // Slide the zero window by 4 bytes.
            if (step < lastStep) {
                for (size_t k = 0; k < 4; ++k) {
                    if (buf[off + k] == 0) --zeros;
                    if (buf[off + kZeroFilterWindow + k] == 0) ++zeros;
                }
            }
        }
    };

    if (threads <= 1) {
        worker(0, stepCount - 1);
    } else {
        const size_t per = (stepCount + threads - 1) / threads;
        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (unsigned t = 0; t < threads; ++t) {
            const size_t first = per * t;
            if (first >= stepCount) break;
            const size_t last = (std::min)(stepCount - 1, first + per - 1);
            pool.emplace_back(worker, first, last);
        }
        for (std::thread& th : pool) th.join();
    }

    if (!found.load()) return false;
    if (hitOffset)   *hitOffset = static_cast<u64>(winnerOffset);
    if (wasReversed) *wasReversed = winnerReversed;
    return true;
}

// ===========================================================================
//  Process memory enumeration
// ===========================================================================
namespace {

struct MemRegion {
    u64   base = 0;
    u64   size = 0;
    DWORD type = 0;
};

/// Committed, readable, not-guarded regions, MEM_IMAGE first.
void EnumerateReadableRegions(HANDLE process, u64 maxRegionBytes,
                              std::vector<MemRegion>& out) {
    out.clear();
    const u64 kMaxUserAddress = 0x00007FFFFFFF0000ull;

    u64 address = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    while (address < kMaxUserAddress) {
        const SIZE_T got = ::VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address),
                                            &mbi, sizeof(mbi));
        if (got != sizeof(mbi)) break;
        if (mbi.RegionSize == 0) break;                     // defensive, per SPEC

        const DWORD prot = mbi.Protect;
        const bool readable =
            (mbi.State == MEM_COMMIT) &&
            !(prot & PAGE_GUARD) && !(prot & PAGE_NOACCESS) &&
            ((prot & PAGE_READONLY) || (prot & PAGE_READWRITE) ||
             (prot & PAGE_WRITECOPY) || (prot & PAGE_EXECUTE_READ) ||
             (prot & PAGE_EXECUTE_READWRITE) || (prot & PAGE_EXECUTE_WRITECOPY));

        if (readable && static_cast<u64>(mbi.RegionSize) <= maxRegionBytes) {
            MemRegion r;
            r.base = reinterpret_cast<u64>(mbi.BaseAddress);
            r.size = static_cast<u64>(mbi.RegionSize);
            r.type = mbi.Type;
            out.push_back(r);
        }

        const u64 next = reinterpret_cast<u64>(mbi.BaseAddress) +
                         static_cast<u64>(mbi.RegionSize);
        if (next <= address) break;                          // no forward progress
        address = next;
    }

    // MEM_IMAGE first: the key material and the cipher object usually live in
    // or next to a mapped module. This is a preference, never a requirement.
    std::stable_sort(out.begin(), out.end(), [](const MemRegion& a, const MemRegion& b) {
        const int ra = (a.type == MEM_IMAGE) ? 0 : (a.type == MEM_PRIVATE ? 1 : 2);
        const int rb = (b.type == MEM_IMAGE) ? 0 : (b.type == MEM_PRIVATE ? 1 : 2);
        return ra < rb;
    });
}

/// Read a whole region; on a partial failure fall back to 64 KB chunks so one
/// bad page does not lose the region. Missing bytes stay zero.
bool ReadRegion(HANDLE process, const MemRegion& r, std::vector<u8>& buf) {
    if (r.size == 0 || r.size > (u64)SIZE_MAX) return false;
    try { buf.assign(static_cast<size_t>(r.size), 0); }
    catch (...) { return false; }

    SIZE_T got = 0;
    if (::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(r.base),
                            buf.data(), buf.size(), &got) && got == buf.size())
        return true;

    bool any = false;
    const size_t chunk = 64 * 1024;
    for (size_t off = 0; off < buf.size(); off += chunk) {
        const size_t n = (std::min)(chunk, buf.size() - off);
        SIZE_T g = 0;
        if (::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(r.base + off),
                                buf.data() + off, n, &g) && g > 0)
            any = true;
    }
    return any;
}

inline u64 NowTickMs() { return ::GetTickCount64(); }

} // namespace

// ---------------------------------------------------------------------------
// One scan pass over a live process.
// ---------------------------------------------------------------------------
namespace {

bool ScanProcessPass(HANDLE process,
                     const KeyFindOptions& opt,
                     const Pk2RawHeader& header,
                     const u8* encryptedRootBlock,
                     u64 deadlineMs,
                     KeyFindResult& result,
                     const ProgressFn& progress,
                     const LogFn& log) {
    std::vector<MemRegion> regions;
    EnumerateReadableRegions(process, opt.maxRegionBytes, regions);
    if (regions.empty()) return false;

    u64 totalBytes = 0;
    for (const MemRegion& r : regions) totalBytes += r.size;

    ReportLog(log, FormatW(L"Okunabilir bellek: %s (%s bolge)",
                           FormatBytesTr(totalBytes).c_str(),
                           FormatCountTr(regions.size()).c_str()));

    const unsigned threads = ResolveThreadCount(opt.threadCount);
    std::vector<u8> buf;
    u64 tests = 0;

    // --- pass 1: Scanner A anchored on the locator strings -----------------
    if (opt.useScannerA) {
        u64 done = 0;
        for (const MemRegion& r : regions) {
            if (NowTickMs() > deadlineMs) break;
            if (!ReportProgress(progress, done, totalBytes, L"Bellek taramasi (anahtar metni)"))
                return false;
            done += r.size;
            if (!ReadRegion(process, r, buf)) continue;

            KeyCandidate cand;
            u64 hitOffset = 0, passTests = 0;
            if (KeyFinder::HarvestAndTest(buf.data(), buf.size(), header, encryptedRootBlock,
                                          threads, false, cand, &hitOffset, &passTests)) {
                tests += passTests;
                result.found  = true;
                result.key    = cand;
                result.sourceOffset = r.base + hitOffset;
                result.oracleTests += tests;
                WipeAndFree(buf);
                return true;
            }
            tests += passTests;
        }
    }

    // --- pass 2: Scanner B, the key-derivation independent one -------------
    if (opt.useScannerB) {
        u64 done = 0;
        for (const MemRegion& r : regions) {
            if (NowTickMs() > deadlineMs) break;
            if (!ReportProgress(progress, done, totalBytes, L"Bellek taramasi (ham Blowfish durumu)"))
                return false;
            done += r.size;
            if (r.size < kBlowfishStateSize) continue;
            if (!ReadRegion(process, r, buf)) continue;

            BlowfishState state{};
            u64  hitOffset = 0;
            bool reversed = false;
            if (KeyFinder::ScanBufferForBlowfishState(buf.data(), buf.size(), encryptedRootBlock,
                                                      opt.scannerBTryReversedLayout, threads,
                                                      state, &hitOffset, &reversed)) {
                result.found = true;
                result.key = KeyCandidate();
                result.key.useRawState = true;
                result.key.rawState.resize(kBlowfishStateSize);
                std::memcpy(result.key.rawState.data(), &state, kBlowfishStateSize);
                result.key.label = reversed ? L"bellekte bulundu (ters (S,P) yerlesimi)"
                                            : L"bellekte bulundu";
                result.sourceOffset = r.base + hitOffset;
                result.oracleTests += tests;
                ::SecureZeroMemory(&state, sizeof(state));
                WipeAndFree(buf);
                return true;
            }
        }
    }

    // --- pass 3: Scanner A widened to everything (last resort) -------------
    if (opt.useScannerA) {
        u64 done = 0;
        for (const MemRegion& r : regions) {
            if (NowTickMs() > deadlineMs) break;
            if (!ReportProgress(progress, done, totalBytes, L"Bellek taramasi (genis ascii taramasi)"))
                return false;
            done += r.size;
            if (!ReadRegion(process, r, buf)) continue;

            KeyCandidate cand;
            u64 hitOffset = 0, passTests = 0;
            // Anchors are already known to be absent here, so this only runs the
            // widened sweep; it is capped so it always terminates.
            if (KeyFinder::HarvestAndTest(buf.data(), buf.size(), header, encryptedRootBlock,
                                          threads, true, cand, &hitOffset, &passTests)) {
                tests += passTests;
                result.found = true;
                result.key   = cand;
                result.sourceOffset = r.base + hitOffset;
                result.oracleTests += tests;
                WipeAndFree(buf);
                return true;
            }
            tests += passTests;
        }
    }

    result.oracleTests += tests;
    WipeAndFree(buf);
    return false;
}

} // namespace

bool KeyFinder::ScanProcessOnce(HANDLE process,
                                const KeyFindOptions& opt,
                                const Pk2RawHeader& header,
                                const u8* encryptedRootBlock,
                                KeyFindResult& result,
                                const ProgressFn& progress,
                                const LogFn& log) {
    if (!process || !encryptedRootBlock) return false;
    const u64 deadline = NowTickMs() +
                         static_cast<u64>((std::max)(1, opt.timeoutSeconds)) * 1000ull;
    return ScanProcessPass(process, opt, header, encryptedRootBlock,
                           deadline, result, progress, log);
}

// ===========================================================================
//  Stage 0 - free wins
// ===========================================================================
Result KeyFinder::TryStage0Keys(const KeyFindOptions& opt,
                                KeyFindResult& result,
                                const LogFn& log) {
    result = KeyFindResult();
    if (opt.archivePath.empty())
        return Result::Fail(Status::InvalidArgument, L"Arsiv yolu bos.");

    Pk2RawHeader header{};
    Result r = Pk2Archive::ProbeHeader(opt.archivePath, header);
    if (!r.ok()) return r;

    u8 root[kPk2BlockSize];
    const bool haveRoot = Pk2Archive::ReadRawRootBlock(opt.archivePath, root).ok();
    if (!haveRoot) return Result::Fail(Status::Truncated, L"Kok blok okunamadi.");

    Blowfish cipher;
    KeyCandidate found;

    // --- 0.1 session memory (this run, this exact archive content) ---------
    if (opt.useSessionKeys) {
        const ArchiveFingerprint fp = ArchiveFingerprint::fromFile(opt.archivePath);
        KeyCandidate remembered;
        if (SessionKeys::Instance().lookup(fp, remembered)) {
            std::vector<KeyCandidate> one{ remembered };
            if (KeyRing::TryCandidates(opt.archivePath, one, found, cipher, nullptr).ok()) {
                result.found = true;
                result.stage = KeyFindStage::Stage0SessionMemory;
                result.key   = found;
                result.howFoundTr = KeyFindStageNameTr(result.stage);
                ReportLog(log, L"Stage 0.1: bu arsiv icin bu oturumda kanitlanmis key kullanildi.");
                return Result::Ok();
            }
        }
    }

    // --- 0.2 sibling archives in the same folder ---------------------------
    if (opt.useSiblingKeys) {
        std::vector<KeyCandidate> siblings =
            SessionKeys::Instance().siblingCandidates(opt.archivePath);
        if (!siblings.empty()) {
            ReportLog(log, FormatW(L"Stage 0.2: ayni klasordeki %s key denenecek.",
                                   FormatCountTr(siblings.size()).c_str()));
            if (KeyRing::TryCandidates(opt.archivePath, siblings, found, cipher, nullptr).ok()) {
                result.found = true;
                result.stage = KeyFindStage::Stage0SiblingArchive;
                result.key   = found;
                result.howFoundTr = KeyFindStageNameTr(result.stage);
                ReportLog(log, L"Stage 0.2: komsu arsivin anahtari calisti - istemci baslatilmadi.");
                return Result::Ok();
            }
        }
    }

    return Result::Fail(Status::NotFound);
}

std::vector<DWORD> KeyFinder::FindRunningClientProcesses(const std::wstring& archiveDir) {
    std::vector<DWORD> out;
    if (archiveDir.empty()) return out;

    const std::wstring dirA = ToLowerW(archiveDir) + L"\\";
    const std::wstring parent = PathParent(archiveDir);
    const std::wstring dirB = parent.empty() ? std::wstring() : (ToLowerW(parent) + L"\\");

    // File names of the ranked client executables of THIS game folder, so a
    // client started from a copy of the folder still qualifies.
    //
    // MEASURED HAZARD, do not relax this: matching on every ranked exe name
    // matched twelve completely unrelated processes on a normal desktop,
    // because a Silkroad folder routinely contains "Launcher.exe",
    // "Uninstall.exe" and "DllSaver.exe" - names half the machine also uses.
    // We then opened and read hundreds of megabytes of other applications'
    // memory. So the name-only route accepts ONLY names that rank as a client
    // in their own right (client / game / silkroad / sro) and are not
    // launcher-like. Everything else must live under the game folder.
    std::vector<std::wstring> rankedNames;
    for (const ClientCandidate& c : RankClientExecutables(archiveDir)) {
        if (c.isLauncherLike) continue;
        const std::wstring n = ToLowerW(PathFileName(c.path));
        if (n.find(L"client")   == std::wstring::npos &&
            n.find(L"game")     == std::wstring::npos &&
            n.find(L"silkroad") == std::wstring::npos &&
            n.find(L"sro")      == std::wstring::npos) continue;
        rankedNames.push_back(n);
    }

    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    SRO_SCOPE_EXIT { ::CloseHandle(snap); };

    const DWORD selfPid = ::GetCurrentProcessId();

    struct Scored { DWORD pid; int score; };
    std::vector<Scored> scored;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!::Process32FirstW(snap, &pe)) return out;
    do {
        if (pe.th32ProcessID == selfPid || pe.th32ProcessID == 0) continue;

        HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2] = {};
        DWORD len = MAX_PATH * 2;
        const bool gotPath = ::QueryFullProcessImageNameW(h, 0, path, &len) != FALSE;
        ::CloseHandle(h);
        if (!gotPath) continue;

        const std::wstring full(path, len);
        if (IsSelfExecutable(full)) continue;

        const std::wstring lowerFull = ToLowerW(full);
        const std::wstring name      = ToLowerW(PathFileName(full));

        int score = -1;
        if (lowerFull.compare(0, dirA.size(), dirA) == 0) score = 200;
        else if (!dirB.empty() && lowerFull.compare(0, dirB.size(), dirB) == 0) score = 150;
        else {
            for (const std::wstring& rn : rankedNames)
                if (rn == name) { score = 100; break; }
        }
        if (score < 0) continue;

        if (name.find(L"client") != std::wstring::npos) score += 50;
        else if (name.find(L"silkroad") != std::wstring::npos) score += 30;
        else if (name.find(L"sro") != std::wstring::npos) score += 25;
        if (IsLauncherLikeName(name)) score -= 60;

        scored.push_back({ pe.th32ProcessID, score });
    } while (::Process32NextW(snap, &pe));

    std::stable_sort(scored.begin(), scored.end(),
                     [](const Scored& a, const Scored& b) { return a.score > b.score; });
    for (const Scored& s : scored) out.push_back(s.pid);
    return out;
}

Result KeyFinder::ScanRunningClient(DWORD pid,
                                    const KeyFindOptions& opt,
                                    KeyFindResult& result,
                                    const ProgressFn& progress,
                                    const LogFn& log) {
    result = KeyFindResult();

    Pk2RawHeader header{};
    Result r = Pk2Archive::ProbeHeader(opt.archivePath, header);
    if (!r.ok()) return r;
    u8 root[kPk2BlockSize];
    if (!Pk2Archive::ReadRawRootBlock(opt.archivePath, root).ok())
        return Result::Fail(Status::Truncated, L"Kok blok okunamadi.");

    // attachExisting never terminates on destruction - the process is the
    // user's, not ours.
    ClientProcessGuard guard;
    r = guard.attachExisting(pid);
    if (!r.ok()) return r;

    // Always name the process we are about to read. Reading another program's
    // memory is not something to do silently.
    std::wstring image;
    {
        wchar_t path[MAX_PATH * 2] = {};
        DWORD len = MAX_PATH * 2;
        if (::QueryFullProcessImageNameW(guard.process(), 0, path, &len))
            image = PathFileName(std::wstring(path, len));
    }
    ReportLog(log, FormatW(L"Stage 0.3: zaten calisan istemciye baglanildi: %s (pid %u).",
                           image.empty() ? L"?" : image.c_str(), pid));

    const u64 deadline = NowTickMs() +
                         static_cast<u64>((std::max)(1, opt.timeoutSeconds)) * 1000ull;
    if (ScanProcessPass(guard.process(), opt, header, root, deadline,
                        result, progress, log)) {
        // Nothing was launched here, so the stage is a Stage 0 one whichever
        // scanner produced the hit.
        result.stage          = KeyFindStage::Stage0RunningClient;
        result.howFoundTr     = KeyFindStageNameTr(result.stage);
        result.sourceFile     = image;
        result.launchedClient = false;
        return Result::Ok();
    }
    return Result::Fail(Status::NotFound);
}

// ===========================================================================
//  Stage 2 - static scan of on-disk binaries (best effort)
// ===========================================================================
Result KeyFinder::ScanStaticBinaries(const KeyFindOptions& opt,
                                     KeyFindResult& result,
                                     const ProgressFn& progress,
                                     const LogFn& log) {
    result = KeyFindResult();

    Pk2RawHeader header{};
    Result r = Pk2Archive::ProbeHeader(opt.archivePath, header);
    if (!r.ok()) return r;
    u8 root[kPk2BlockSize];
    if (!Pk2Archive::ReadRawRootBlock(opt.archivePath, root).ok())
        return Result::Fail(Status::Truncated, L"Kok blok okunamadi.");

    const std::wstring dir = PathParent(opt.archivePath);
    if (dir.empty()) return Result::Fail(Status::NotFound);

    struct Bin { std::wstring path; u64 size; int score; };
    std::vector<Bin> bins;

    // Scan the archive's folder AND its immediate subfolders. A repack routinely
    // keeps the archives at the top and the modules one level down, and a scan
    // that stops at the top reports "nothing found" on a client whose key is
    // sitting right there. Subfolders only - no unbounded recursion.
    std::vector<std::wstring> scanDirs{ dir };
    for (const std::wstring& sub : ListDirectory(dir, L"*", false, true))
        scanDirs.push_back(sub);

    auto collect = [&](const wchar_t* pattern) {
      for (const std::wstring& sdir : scanDirs) {
        for (const std::wstring& p : ListDirectory(sdir, pattern, true, false)) {
            if (IsSelfExecutable(p)) continue;
            const u64 sz = GetFileSize64(p);
            if (sz == 0 || sz > opt.maxBinaryBytes) continue;
            const std::wstring n = ToLowerW(PathFileName(p));
            int score = 0;
            if (opt.prioritiseKnownBinaries) {
                if (n.find(L"gfx")   != std::wstring::npos) score += 100;
                if (n.find(L"guard") != std::wstring::npos) score += 90;
                if (n.find(L"lib")   != std::wstring::npos) score += 60;
                if (n.find(L"bin")   != std::wstring::npos) score += 40;
                if (n.find(L"client")!= std::wstring::npos) score += 30;
            }
            bins.push_back({ p, sz, score });
        }
      }
    };
    collect(L"*.dll");
    collect(L"*.exe");

    std::stable_sort(bins.begin(), bins.end(), [](const Bin& a, const Bin& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.size < b.size;      // cheapest first among equals
    });
    if (bins.empty()) return Result::Fail(Status::NotFound);

    ReportLog(log, FormatW(L"Stage 2: %s ikili dosya taranacak (disk, yan etki yok).",
                           FormatCountTr(bins.size()).c_str()));

    const unsigned threads = ResolveThreadCount(opt.threadCount);
    u64 done = 0, total = 0;
    for (const Bin& b : bins) total += b.size;

    for (const Bin& b : bins) {
        if (!ReportProgress(progress, done, total, L"Disk taramasi"))
            return Result::Fail(Status::Cancelled);
        done += b.size;

        std::vector<u8> buf;
        if (!ReadEntireFile(b.path, buf, opt.maxBinaryBytes).ok()) continue;

        KeyCandidate cand;
        u64 hitOffset = 0, tests = 0;
        if (HarvestAndTest(buf.data(), buf.size(), header, root, threads,
                           /*allowFullWindowSweep*/ true, cand, &hitOffset, &tests)) {
            result.found = true;
            result.stage = (cand.baseId == BaseKeyId::Custom)
                             ? KeyFindStage::Stage2StaticBaseKey
                             : KeyFindStage::Stage2StaticAsciiKey;
            result.key = cand;
            result.howFoundTr = KeyFindStageNameTr(result.stage);
            result.sourceFile = b.path;
            result.sourceOffset = hitOffset;
            result.oracleTests += tests;
            WipeAndFree(buf);
            return Result::Ok();
        }
        result.oracleTests += tests;

        // A scheduled cipher state normally only exists at runtime, but it costs
        // little to check, and an unpacked module on disk would carry one.
        if (opt.useScannerB && buf.size() >= kBlowfishStateSize) {
            BlowfishState state{};
            u64 sOffset = 0; bool reversed = false;
            if (ScanBufferForBlowfishState(buf.data(), buf.size(), root,
                                           opt.scannerBTryReversedLayout, threads,
                                           state, &sOffset, &reversed)) {
                result.found = true;
                result.stage = KeyFindStage::Stage2StaticBaseKey;
                result.key = KeyCandidate();
                result.key.useRawState = true;
                result.key.rawState.resize(kBlowfishStateSize);
                std::memcpy(result.key.rawState.data(), &state, kBlowfishStateSize);
                result.key.label = reversed ? L"diskte bulunan ham Blowfish durumu (ters yerlesim)"
                                            : L"diskte bulunan ham Blowfish durumu";
                result.howFoundTr = L"Disk taraması: ham Blowfish durumu";
                result.sourceFile = b.path;
                result.sourceOffset = sOffset;
                ::SecureZeroMemory(&state, sizeof(state));
                WipeAndFree(buf);
                return Result::Ok();
            }
        }
        WipeAndFree(buf);
    }

    return Result::Fail(Status::NotFound);
}

// ===========================================================================
//  Stage 3 - runtime memory scan
// ===========================================================================
Result KeyFinder::ScanRuntime(const KeyFindOptions& opt,
                              KeyFindResult& result,
                              const ProgressFn& progress,
                              const LogFn& log) {
    result = KeyFindResult();

    if (!opt.allowLaunchClient)
        return Result::Fail(Status::AccessDenied,
                            L"Istemci baslatma onayi verilmedi (Stage 3 kapali).");

    Pk2RawHeader header{};
    Result r = Pk2Archive::ProbeHeader(opt.archivePath, header);
    if (!r.ok()) return r;
    u8 root[kPk2BlockSize];
    if (!Pk2Archive::ReadRawRootBlock(opt.archivePath, root).ok())
        return Result::Fail(Status::Truncated, L"Kok blok okunamadi.");

    const std::wstring dir = PathParent(opt.archivePath);

    std::vector<ClientCandidate> candidates;
    if (!opt.clientExe.empty()) {
        ClientCandidate c;
        c.path  = opt.clientExe;
        c.size  = GetFileSize64(opt.clientExe);
        c.score = 1000;
        candidates.push_back(c);
        if (opt.tryRankedFallbacks) {
            for (const ClientCandidate& rc : RankClientExecutables(dir))
                if (!IEqualsW(rc.path, opt.clientExe)) candidates.push_back(rc);
        }
    } else {
        candidates = RankClientExecutables(dir);
    }
    if (candidates.empty())
        return Result::Fail(Status::NotFound, L"Klasorde calistirilabilir istemci bulunamadi.");

    const int maxAttempts = (std::max)(1, opt.tryRankedFallbacks ? opt.maxClientAttempts : 1);
    const int attempts = (std::min)(maxAttempts, static_cast<int>(candidates.size()));

    for (int i = 0; i < attempts; ++i) {
        const ClientCandidate& c = candidates[static_cast<size_t>(i)];

        ReportLog(log, FormatW(L"Stage 3: istemci baslatiliyor (gizli): %s [%s]",
                               PathFileName(c.path).c_str(),
                               FormatBytesTr(c.size).c_str()));

        // RAII: this terminates the client on EVERY exit path, including an
        // exception thrown from the scanner below.
        ClientProcessGuard guard;
        Result lr = guard.launchHidden(c.path, dir);
        if (!lr.ok()) {
            ReportLog(log, L"  " + lr.text());
            continue;
        }
        result.launchedClient = true;

        const u64 start    = NowTickMs();
        const u64 deadline = start + static_cast<u64>((std::max)(1, opt.timeoutSeconds)) * 1000ull;
        const DWORD pollMs = static_cast<DWORD>((std::max)(50, opt.pollIntervalMs));

        bool diedEarly = false;
        int  pass = 0;
        while (NowTickMs() < deadline) {
            // Let the protection unpack itself before the first pass.
            ::Sleep(pollMs);

            if (!guard.running()) {
                // Some shards only unpack in the launcher/patcher; a client that
                // exits at once is a reason to try the next ranked exe, not to
                // give up. Still do one last scan attempt is pointless once the
                // process is gone, so break out.
                diedEarly = (NowTickMs() - start) < 5000;
                ReportLog(log, L"  Istemci sonlandi; sonraki adaya geciliyor.");
                break;
            }

            ++pass;
            ReportLog(log, FormatW(L"  Bellek taramasi gecis %d ...", pass));
            if (ScanProcessPass(guard.process(), opt, header, root, deadline,
                                result, progress, log)) {
                result.stage = result.key.useRawState ? KeyFindStage::Stage3ScannerB
                                                      : KeyFindStage::Stage3ScannerA;
                result.howFoundTr = KeyFindStageNameTr(result.stage);
                result.sourceFile = c.path;
                result.launchedClient = true;
                return Result::Ok();          // guard terminates the client here
            }
        }
        (void)diedEarly;
        // guard's destructor terminates the client before the next attempt.
    }

    return Result::Fail(Status::NotFound,
                        L"Bellek taramasi bir key bulamadi.");
}

// ===========================================================================
//  The whole search
// ===========================================================================
Result KeyFinder::FindKey(const KeyFindOptions& opt,
                          KeyFindResult& result,
                          const ProgressFn& progress,
                          const LogFn& log) {
    result = KeyFindResult();

    if (opt.archivePath.empty() || !FileExists(opt.archivePath))
        return Result::Fail(Status::NotFound, L"Arsiv bulunamadi: " + opt.archivePath);

    Pk2RawHeader header{};
    Result r = Pk2Archive::ProbeHeader(opt.archivePath, header);
    if (!r.ok()) return r;

    u8 root[kPk2BlockSize];
    if (!Pk2Archive::ReadRawRootBlock(opt.archivePath, root).ok())
        return Result::Fail(Status::Truncated, L"Kok blok okunamadi.");

    ReportLog(log, FormatW(L"Arsiv basligi: checksum %02x %02x %02x, Encrypted=%u",
                           header.Checksum[0], header.Checksum[1], header.Checksum[2],
                           header.Encrypted));

    // Detect a plaintext archive independently of the Encrypted flag (SPEC §1).
    if (root[0] == 0x01 && root[1] == 0x2E && root[2] == 0x00) {
        ReportLog(log, L"Kok blok zaten duz metin: bu arsiv sifrelenmemis, key gerekmiyor.");
        return Result::Fail(Status::Ok,
                            L"Arsiv sifrelenmemis - key aranmasina gerek yok.");
    }

    const ArchiveFingerprint fp = ArchiveFingerprint::fromFile(opt.archivePath);

    auto succeed = [&](KeyFindStage stage) {
        result.found = true;
        result.stage = stage;
        if (result.howFoundTr.empty()) result.howFoundTr = KeyFindStageNameTr(stage);
        // Memory only. RULE 1: nothing here ever reaches the disk.
        SessionKeys::Instance().remember(opt.archivePath, fp, result.key, result.howFoundTr);
    };

    // --- Stage 0.1 / 0.2 ---------------------------------------------------
    {
        KeyFindResult s0;
        if (TryStage0Keys(opt, s0, log).ok() && s0.found) {
            result = s0;
            succeed(s0.stage);
            return Result::Ok();
        }
    }

    // --- Stage 1 -----------------------------------------------------------
    {
        Blowfish cipher;
        KeyCandidate found;
        Result s1 = KeyRing::TryStage1(opt.archivePath, opt.typedKey, opt.rawKeyMode,
                                       nullptr, found, cipher, log);
        if (s1.ok()) {
            result.key = found;
            succeed(found.baseId == BaseKeyId::Zero ? KeyFindStage::Stage1RawKey
                                                    : KeyFindStage::Stage1Typed);
            return Result::Ok();
        }
        ReportLog(log, L"Stage 1: yazilan key hicbir base key ile calismadi.");
    }

    // --- Stage 2 (instant, no side effects, usually finds nothing) ---------
    if (opt.scanSiblingBinaries) {
        KeyFindResult s2;
        const bool hit = ScanStaticBinaries(opt, s2, progress, log).ok() && s2.found;
        result.oracleTests += s2.oracleTests;
        if (hit) {
            const u64 carried = result.oracleTests;
            result = s2;
            result.oracleTests = carried;
            succeed(s2.stage);
            return Result::Ok();
        }
        ReportLog(log, L"Stage 2: disk taramasi bir sey bulamadi (korumali istemcilerde beklenen sonuc).");
    }

    // --- Stage 0.3: an already-running client (launches NOTHING) -----------
    if (opt.scanRunningClients) {
        // Hard cap: reading another process's memory is expensive and intrusive,
        // so only the best-ranked few are ever touched.
        constexpr size_t kMaxRunningClientsScanned = 3;
        const std::wstring dir = PathParent(opt.archivePath);
        std::vector<DWORD> pids = FindRunningClientProcesses(dir);
        if (pids.empty()) {
            ReportLog(log, L"Stage 0.3: bu oyuna ait calisan istemci yok.");
        } else {
            const size_t n = (std::min)(pids.size(), kMaxRunningClientsScanned);
            for (size_t i = 0; i < n; ++i) {
                KeyFindResult s03;
                const bool hit = ScanRunningClient(pids[i], opt, s03, progress, log).ok() &&
                                 s03.found;
                result.oracleTests += s03.oracleTests;
                if (hit) {
                    const u64 carried = result.oracleTests;
                    result = s03;
                    result.oracleTests = carried;
                    succeed(KeyFindStage::Stage0RunningClient);
                    result.launchedClient = false;
                    return Result::Ok();
                }
            }
            ReportLog(log, L"Stage 0.3: calisan istemcilerde key bulunamadi.");
        }
    }

    // --- Stage 3: launch a client (explicit consent only) ------------------
    if (!opt.allowLaunchClient) {
        return Result::Fail(Status::NotFound,
                            L"Key bulunamadi. Oyun istemcisini arka planda baslatan bellek "
                            L"taramasi (Stage 3) icin acik onay gerekiyor.");
    }

    KeyFindResult s3;
    Result r3 = ScanRuntime(opt, s3, progress, log);
    const u64 carried = result.oracleTests;
    if (r3.ok() && s3.found) {
        result = s3;
        result.oracleTests += carried;
        succeed(s3.stage);
        return Result::Ok();
    }
    result.launchedClient = s3.launchedClient;
    result.oracleTests   += s3.oracleTests;
    return Result::Fail(Status::NotFound,
                        L"Hicbir asama key bulamadi. Uydurma bir key uretilmedi.");
}

// ===========================================================================
// Decrypted-asset dumping - harvest server-encrypted assets from the running
// client's memory (it decrypts them with its own code; we read the plaintext).
// ===========================================================================
namespace {

std::string ExpectedJmxSig(const std::wstring& fileName) {
    std::wstring e = PathExtension(fileName);
    for (auto& c : e) c = static_cast<wchar_t>(towlower(c));
    if (e == L".bsr") return "JMXVRES ";
    if (e == L".bms") return "JMXVBMS ";
    if (e == L".bmt") return "JMXVBMT ";
    if (e == L".bsk") return "JMXVBSK ";
    if (e == L".ban") return "JMXVBAN ";
    if (e == L".ddj") return "JMXVDDJ ";
    if (e == L".cpd") return "JMXVCPD ";
    if (e == L".efp") return "JMXVEFF ";
    if (e == L".nvm") return "JMXVNVM ";
    if (e == L".dof") return "JMXVDOF ";
    if (e == L".o" || e == L".o2") return "JMXVMAPO";
    if (e == L".m")  return "JMXVMAPM";
    if (e == L".t")  return "JMXVMAPT";
    return std::string();
}

// Sanitize an archive inner path before writing it under an output dir: drop
// empty / "." / ".." components and strip illegal chars per component, so a
// crafted archive entry name can't traverse out of the output directory.
std::wstring SanitizeRelPath(const std::wstring& full) {
    std::wstring out;
    size_t i = 0;
    while (i < full.size()) {
        const size_t j = full.find_first_of(L"\\/", i);
        const size_t end = (j == std::wstring::npos) ? full.size() : j;
        std::wstring comp = full.substr(i, end - i);
        i = (j == std::wstring::npos) ? full.size() : j + 1;
        if (comp.empty() || comp == L"." || comp == L"..") continue;
        comp = SanitizeFileName(comp);
        out = out.empty() ? comp : PathJoin(out, comp);
    }
    return out;
}

inline u32 ReadU32LE(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

// Cheap structural sanity so we don't dump the wrong-sized window under a name.
bool AssetLooksValid(const std::string& sig, const u8* buf, size_t len) {
    if (len < 16) return false;
    if (sig == "JMXVDDJ ") {                       // 20-byte header, size is exact
        const u32 texSize = ReadU32LE(buf + 12);
        return (u64)20 + texSize == len;
    }
    if (sig == "JMXVRES " || sig == "JMXVBMS " || sig == "JMXVCPD " ||
        sig == "JMXVDOF ") {
        // first header u32 after the 12-byte signature is a within-file offset.
        const u32 off0 = ReadU32LE(buf + 12);
        return off0 > 0 && off0 < len;
    }
    return true;                                    // others: accept on sig+size
}

} // namespace

Result KeyFinder::DumpDecryptedAssets(void* processV, Pk2Archive& archive,
                                      const std::wstring& outDir, int timeoutSeconds,
                                      AssetDumpResult& out,
                                      const ProgressFn& progress, const LogFn& log) {
    HANDLE process = reinterpret_cast<HANDLE>(processV);
    out = AssetDumpResult();
    if (!process) return Result::Fail(Status::InvalidArgument, L"Gecersiz surec tutamaci.");
    if (!archive.isOpen()) return Result::Fail(Status::InvalidArgument, L"Arsiv acik degil.");

    // Root everything under <outDir>\<ArchiveName>\ so a dumped asset lands at
    // e.g. OriDB\res\cos\cos_p_baby_dino.bsr - same convention as extract.
    std::wstring stem = PathFileName(archive.path());
    { const size_t d = stem.find_last_of(L'.'); if (d != std::wstring::npos) stem = stem.substr(0, d); }
    if (stem.empty()) stem = L"arsiv";
    const std::wstring baseDir = PathJoin(outDir, SanitizeFileName(stem));
    EnsureDirectory(baseDir);

    // 1) Collect the ENCRYPTED asset entries: those whose on-disk head is not JMXV.
    struct Target { std::wstring path; u64 size; std::string sig; bool done = false; };
    std::vector<Target> targets;
    ReportLog(log, L"Sifreli varliklar taraniyor (arsiv)...");
    archive.walkAll([&](Pk2Node* n) -> bool {
        if (!n || !n->isFile()) return true;
        const std::string sig = ExpectedJmxSig(n->name);
        if (sig.empty()) return true;
        std::vector<u8> head;
        if (!archive.readFilePrefix(n, 4, head).ok() || head.size() < 4) return true;
        if (head[0] == 'J' && head[1] == 'M' && head[2] == 'X' && head[3] == 'V') return true;
        const u64 sz = n->entry.size;
        if (sz >= 16 && sz < 64ull * 1024 * 1024) {
            const std::wstring safe = SanitizeRelPath(n->fullPath);
            if (!safe.empty())
                targets.push_back({ safe, sz, sig, false });
        }
        return true;
    }, progress);

    out.encryptedTargets = static_cast<int>(targets.size());
    ReportLog(log, FormatW(L"Sifreli varlik: %d — client'ta yuklendikce cozulup dokulecek.",
                           out.encryptedTargets));
    if (targets.empty()) return Result::Ok();

    // 2) Watch the client's memory; harvest decrypted matches until all done / timeout.
    const DWORD start = ::GetTickCount();
    std::vector<u8> buf, asset;
    for (;;) {
        std::vector<MemRegion> regions;
        EnumerateReadableRegions(process, 256ull * 1024 * 1024, regions);

        for (const MemRegion& r : regions) {
            if (!ReadRegion(process, r, buf)) continue;
            const size_t n = buf.size();
            for (size_t i = 0; i + 8 <= n; ) {
                if (buf[i] == 'J' && buf[i + 1] == 'M' && buf[i + 2] == 'X' && buf[i + 3] == 'V') {
                    for (Target& t : targets) {
                        if (t.done) continue;
                        if (t.sig.size() > n - i) continue;
                        if (std::memcmp(&buf[i], t.sig.data(), t.sig.size()) != 0) continue;
                        // Read the exact stored size at this address (decryption
                        // preserves length) directly from the process.
                        asset.assign(static_cast<size_t>(t.size), 0);
                        SIZE_T got = 0;
                        if (::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(r.base + i),
                                                asset.data(), static_cast<SIZE_T>(t.size), &got) &&
                            got == t.size &&
                            AssetLooksValid(t.sig, asset.data(), asset.size())) {
                            const std::wstring outPath = PathJoin(baseDir, t.path);
                            EnsureDirectory(PathParent(outPath));
                            if (WriteEntireFile(outPath, asset.data(), asset.size()).ok()) {
                                t.done = true; ++out.written;
                                ReportLog(log, L"Cozuldu -> " + t.path);
                            }
                        }
                        break;   // one target per hit
                    }
                    i += 4;
                } else {
                    ++i;
                }
            }
        }

        int remaining = 0;
        for (const Target& t : targets) if (!t.done) ++remaining;
        if (progress) progress(static_cast<u64>(out.written),
                               static_cast<u64>(out.encryptedTargets),
                               FormatW(L"Cozulen: %d / %d", out.written, out.encryptedTargets).c_str());
        if (remaining == 0) break;
        if (static_cast<int>((::GetTickCount() - start) / 1000) >= timeoutSeconds) break;
        ::Sleep(600);
    }

    ReportLog(log, FormatW(L"Bitti: %d / %d sifreli varlik cozulup dokuldu.",
                           out.written, out.encryptedTargets));
    return Result::Ok();
}

} // namespace sro
