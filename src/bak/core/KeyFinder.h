// ============================================================================
//  SRO Asset Studio - core/KeyFinder.h
//
//  The key discovery subsystem (SPEC.md §2). This is the headline feature.
//
//  ==========================================================================
//   RULE 0 - NOTHING SERVER-SPECIFIC IS HARDCODED
//  ==========================================================================
//  No per-server key table, no special case for Origin / Golden / anything.
//  The finder DERIVES keys generically and must work on a shard nobody has
//  ever seen. The concrete keys quoted in SPEC.md are TEST EXPECTATIONS used
//  to check the finder - if grepping src\ finds them outside a comment or a
//  test, the finder is cheating.
//
//  ==========================================================================
//   RULE 1 - NO KEY IS EVER WRITTEN TO DISK
//  ==========================================================================
//  Not to settings.ini, not to a keys file, not to a log, not to a crash dump,
//  not to an exported report. This includes a recovered raw Blowfish (P,S)
//  state, which IS key material. Discovered keys live in SessionKeys
//  (core/Config.h) for the process lifetime and are wiped with
//  SecureZeroMemory. A "save log" feature must redact key material.
//
//  ---------------------------------------------------------------------------
//  STAGE 0 - free wins, tried before anything else. Launches nothing.
//    1. Session memory: a key already proven this run for this exact archive
//       content (fingerprint = size + header checksum bytes + last write time,
//       so a patched/re-keyed archive never matches an old key).
//    2. Sibling archives in the same folder. One derived key normally opens
//       every archive of a game (measured on Golden Online: one key opens
//       Data/Map/Media/Music/Particles, identical header checksum). This is
//       what stops the tool launching a client once per archive.
//    3. An ALREADY-RUNNING client. Enumerate processes for a client belonging
//       to this game folder and scan it. Instant, and nothing is launched.
//       This is the nicest path - try it before considering a launch.
//
//  STAGE 1 - instant, no side effects. See KeyRing.h.
//
//  STAGE 2 - opportunistic static scan of on-disk binaries. BEST EFFORT ONLY.
//    Protected clients keep the key encrypted on disk, so for MaxiGuard-class
//    protection this stage NORMALLY FINDS NOTHING and that is expected. It
//    runs only because it is instant and side-effect free. Never assume a
//    decrypted module exists on disk.
//
//  STAGE 3 - runtime memory scan. THIS IS WHAT "KEY BUL" ACTUALLY IS.
//    Launch the ranked client hidden, let the protection unpack itself, then
//    read the key out of the running process. Nothing here is server-specific:
//    the same code path handles sro_client.exe, GoldenClient.exe and
//    OriginClient.exe.
//
//  ---------------------------------------------------------------------------
//  THE VERIFIED KEY-MATERIAL LAYOUT (MaxiGuard-class), observed live:
//
//     "(Wrong BlowfishKey)\n"      <- locator string
//     00 00 00                     <- alignment padding
//     <10 non-zero bytes>          <- the BASE KEY
//     00 00                        <- padding
//     <ascii key, NUL terminated>  <- the ASCII KEY
//
//  CORRECTION worth remembering: the 0A 00 00 00 before the base key is NOT a
//  length prefix - the 0A is the newline ending the previous string and the
//  zeros are alignment. Do NOT key the search off a length field. The reliable
//  signal is purely POSITIONAL: a ~10-byte non-zero run immediately preceding
//  an ascii-key candidate.
//
//  Search order that is measured to work: anchor on the locator strings first
//  and harvest ascii candidates only within +-4 KB of a locator hit. On a real
//  client that derived a completely unknown key in ~6,700 oracle tests.
//  Widening to the whole address space is the fallback, not the plan.
//
//  Implementation owner: Agent C (key discovery).
// ============================================================================
#pragma once

#include "../Common.h"
#include "Blowfish.h"
#include "KeyRing.h"
#include "Pk2Types.h"

namespace sro {

class Pk2Archive;   // forward decl - DumpDecryptedAssets takes an open archive

// ---------------------------------------------------------------------------
// Locator strings. These are HINTS that mark a memory region as worth scanning
// first. They are never requirements, and they are not server-specific - they
// are strings of the protection/engine code itself.
// ---------------------------------------------------------------------------
struct KeyLocator {
    const char*  ascii;      ///< byte pattern to search for
    const wchar_t* labelTr;  ///< for the log
};

/// Ordered best-first. "Wrong BlowfishKey" anchored the hit on BOTH real
/// clients tested, so it is tried first.
const std::vector<KeyLocator>& KeyMaterialLocators();

/// The stock Blowfish P-array first word (0x243F6A88). Finding it marks a
/// region as containing a cipher object - useful for Scanner B.
constexpr u32 kStockBlowfishP0 = 0x243F6A88u;

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct KeyFindOptions {
    std::wstring archivePath;                 ///< the archive we are trying to open
    std::string  typedKey = "169841";         ///< the ASCII key from the UI field
    bool         rawKeyMode = false;          ///< "Raw Key" toggle

    // --- stage 0 -----------------------------------------------------------
    bool useSessionKeys      = true;   ///< keys proven this run for this archive
    bool useSiblingKeys      = true;   ///< keys proven for archives in the same folder
    bool scanRunningClients  = true;   ///< attach to an already-running client

    // --- stage 2 -----------------------------------------------------------
    bool scanSiblingBinaries = true;          ///< *.dll / *.exe next to the archive
    /// Order the on-disk scan by likelihood: names containing gfx, guard, lib, bin.
    bool prioritiseKnownBinaries = true;
    u64  maxBinaryBytes = 256ull * 1024 * 1024;

    // --- stage 3 -----------------------------------------------------------
    /// MUST be true only after the user explicitly confirmed - it starts the game.
    bool         allowLaunchClient = false;
    /// Explicit override; empty means "use the ranked list".
    std::wstring clientExe;
    /// Try the next ranked exe when one dies immediately or yields nothing.
    /// Some shards only unpack the key in the launcher or patcher, so
    /// Launcher.exe / *Patcher.exe are valid LATER attempts, not rejects.
    bool         tryRankedFallbacks = true;
    int          maxClientAttempts  = 3;
    int          timeoutSeconds     = 20;
    int          pollIntervalMs     = 750;
    bool         useScannerA = true;          ///< key material (ascii + base)
    bool         useScannerB = true;          ///< raw Blowfish (P,S) state
    bool         scannerBTryReversedLayout = true;
    /// Skip single regions larger than this. Measured: a real client has about
    /// 895 MB of committed readable memory and walks in ~2 s even naively.
    u64          maxRegionBytes = 256ull * 1024 * 1024;
    /// Harvest window around a locator hit before widening to everything.
    u64          locatorWindowBytes = 4ull * 1024;

    // --- general -----------------------------------------------------------
    unsigned threadCount = 0;                 ///< 0 = hardware_concurrency()
};

// ---------------------------------------------------------------------------
// Outcome
// ---------------------------------------------------------------------------
enum class KeyFindStage : int {
    None = 0,
    Stage0SessionMemory,     ///< already proven this run for this archive
    Stage0SiblingArchive,    ///< proven for another archive in the same folder
    Stage0RunningClient,     ///< read from a client that was already running
    Stage1Typed,             ///< the typed key x a base key
    Stage1RawKey,            ///< the typed key used verbatim
    Stage2StaticBaseKey,
    Stage2StaticAsciiKey,
    Stage3ScannerA,          ///< ascii + base key recovered from RAM
    Stage3ScannerB           ///< raw Blowfish state recovered from RAM
};

/// Turkish label, used in the log so the user always knows which path ran and
/// in particular whether a client was launched.
const wchar_t* KeyFindStageNameTr(KeyFindStage stage);

/// True for the stages that start a process. The UI must have obtained
/// explicit consent before any of these can run.
bool KeyFindStageLaunchesClient(KeyFindStage stage);

struct KeyFindResult {
    bool          found = false;
    KeyFindStage  stage = KeyFindStage::None;
    KeyCandidate  key;                 ///< usable directly with Pk2Archive::open
    std::wstring  howFoundTr;          ///< Turkish, for the log and the dialog
    std::wstring  sourceFile;          ///< binary / exe the hit came from
    u64           sourceOffset = 0;    ///< offset within that source, when known
    bool          launchedClient = false;
    u64           oracleTests = 0;     ///< work done, for the log
};

// ---------------------------------------------------------------------------
// Client executable ranking
// ---------------------------------------------------------------------------
struct ClientCandidate {
    std::wstring path;
    int          score = 0;      ///< higher is more likely to be the client
    u64          size  = 0;
    bool         isLauncherLike = false;  ///< demoted, but still tried later
};

// ---------------------------------------------------------------------------
// ClientProcessGuard
//
// RAII: terminates and closes the launched client no matter how the scope is
// left - normal return, early return or exception. Never leave the game
// running.
// ---------------------------------------------------------------------------
class ClientProcessGuard {
public:
    ClientProcessGuard() = default;
    ~ClientProcessGuard();

    ClientProcessGuard(const ClientProcessGuard&) = delete;
    ClientProcessGuard& operator=(const ClientProcessGuard&) = delete;

    /// Launch `exePath` hidden:
    ///   DETACHED_PROCESS | CREATE_NO_WINDOW, SW_HIDE,
    ///   STARTUPINFO.lpDesktop set to a private desktop name,
    ///   env __COMPAT_LAYER=RunAsInvoker to dodge an admin manifest,
    ///   working directory = `workingDir` (the archive's directory).
    Result launchHidden(const std::wstring& exePath, const std::wstring& workingDir);

    /// Adopt an already-running process. Nothing is launched and NOTHING is
    /// terminated on destruction - we must never kill a process the user
    /// started themselves.
    Result attachExisting(DWORD pid);

    bool   running() const;
    bool   ownsProcess() const { return m_owned; }
    HANDLE process() const { return m_process; }
    DWORD  pid() const { return m_pid; }

    /// Terminate now if we own it (also called by the destructor).
    void terminate();

private:
    HANDLE m_process = nullptr;
    HANDLE m_thread  = nullptr;
    DWORD  m_pid     = 0;
    HDESK  m_desktop = nullptr;
    bool   m_owned   = false;   ///< false when attached to an existing process
};

// ---------------------------------------------------------------------------
// KeyFinder
// ---------------------------------------------------------------------------
class KeyFinder {
public:
    // =======================================================================
    // Whole search: Stage 0 -> 1 -> 2 -> (only with consent) 3
    // =======================================================================

    /// Returns Status::NotFound when every stage failed. The caller must then
    /// tell the user honestly that no key was found - never invent one.
    static Result FindKey(const KeyFindOptions& opt,
                          KeyFindResult& result,
                          const ProgressFn& progress,
                          const LogFn& log);

    // =======================================================================
    // Stage 0
    // =======================================================================

    /// Session memory + sibling archives. Launches nothing, touches no disk
    /// beyond reading the target archive's header and root block.
    static Result TryStage0Keys(const KeyFindOptions& opt,
                                KeyFindResult& result,
                                const LogFn& log);

    /// Processes that look like a client for this game folder, best first.
    /// A process qualifies when its main module path sits under the archive's
    /// directory (or its parent), or when its name ranks as a client.
    static std::vector<DWORD> FindRunningClientProcesses(const std::wstring& archiveDir);

    /// Scan an already-running process. Nothing is launched and the process is
    /// never terminated.
    static Result ScanRunningClient(DWORD pid,
                                    const KeyFindOptions& opt,
                                    KeyFindResult& result,
                                    const ProgressFn& progress,
                                    const LogFn& log);

    // =======================================================================
    // Client executable ranking
    // =======================================================================

    /// Rank the .exe files in `archiveDir` and its parent, best first:
    ///   name contains "client"  >  "game" / "silkroad" / "sro"
    ///   >  largest remaining .exe
    /// Demoted (but NOT removed - some shards only unpack the key in the
    /// launcher or patcher): launcher, patch, uninstall, setup, dllsaver,
    /// replacer, autologin, config, crash, manager, tool, pk2, editor.
    /// ALWAYS excluded: our own module name. Never launch ourselves - a real
    /// hazard, since a PK2 manager tool was observed sitting in a game folder
    /// and ranking second.
    static std::vector<ClientCandidate> RankClientExecutables(const std::wstring& archiveDir);

    /// True when the name looks like a launcher/patcher/tool rather than the
    /// client. Such names are demoted, not rejected.
    static bool IsLauncherLikeName(const std::wstring& fileName);

    /// True when the file must never be launched (currently: this program).
    static bool IsSelfExecutable(const std::wstring& path);

    // =======================================================================
    // Stage 2 - static binary scan (best effort, usually finds nothing)
    // =======================================================================
    static Result ScanStaticBinaries(const KeyFindOptions& opt,
                                     KeyFindResult& result,
                                     const ProgressFn& progress,
                                     const LogFn& log);

    // =======================================================================
    // Decrypted-asset dumping (for server-side per-file encrypted assets)
    //
    // Some protected servers (MaxiGuard) encrypt individual asset files inside
    // the archive with a custom cipher embedded in the client - it is NOT the
    // archive blowfish key and cannot be cracked from the bytes. But the running
    // client DECRYPTS each file it loads into its own memory. So: attach to the
    // client, and whenever it has decrypted an asset whose bytes match one of the
    // archive's encrypted entries (same expected JMXV signature + exact size),
    // read it back out and write the plaintext file to disk. The client does the
    // decryption with its own code; we just harvest the result. Generic - works
    // for any client, no per-server cipher knowledge needed.
    //
    // `process` must be an OPEN handle (PROCESS_VM_READ | QUERY_INFORMATION).
    // Runs passes for up to `timeoutSeconds`, so the user can load more assets
    // (e.g. summon a COS) while it watches. Returns the number written.
    // =======================================================================
    struct AssetDumpResult {
        int encryptedTargets = 0;   ///< encrypted files found in the archive
        int written          = 0;   ///< decrypted files harvested from RAM
        int learnedPairs     = 0;   ///< samples that taught the XOR keystream
        bool cipherFound     = false; ///< the asset cipher itself was recovered
        std::wstring cipherWhere;     ///< module it was found in
    };
    static Result DumpDecryptedAssets(void* process, Pk2Archive& archive,
                                      const std::wstring& outDir, int timeoutSeconds,
                                      AssetDumpResult& out,
                                      const ProgressFn& progress, const LogFn& log);

    // =======================================================================
    // Stage 3 - runtime memory scan
    // =======================================================================

    /// Launch the ranked client hidden (honouring opt.allowLaunchClient),
    /// poll its memory until a hit or opt.timeoutSeconds, then ALWAYS
    /// terminate it. Falls back to the next ranked exe when one dies at once.
    static Result ScanRuntime(const KeyFindOptions& opt,
                              KeyFindResult& result,
                              const ProgressFn& progress,
                              const LogFn& log);

    /// One pass over a live process's committed readable memory.
    /// Enumerate with VirtualQueryEx up to 0x7FFFFFFF0000, stop if RegionSize
    /// comes back 0, skip non-readable protections and regions larger than
    /// opt.maxRegionBytes. Prefer MEM_IMAGE regions and regions containing a
    /// locator string, but do not require them.
    static bool ScanProcessOnce(HANDLE process,
                                const KeyFindOptions& opt,
                                const Pk2RawHeader& header,
                                const u8* encryptedRootBlock /*[2560]*/,
                                KeyFindResult& result,
                                const ProgressFn& progress,
                                const LogFn& log);

    // =======================================================================
    // Candidate harvesting - ONE implementation shared by the static and the
    // memory scanner. Order matters; it is the measured-fast order.
    // =======================================================================

    /// An ascii-key candidate found in a buffer.
    struct AsciiCandidate {
        u64         offset = 0;    ///< position within the scanned buffer
        std::string text;          ///< 6..56 printable bytes, NUL terminated
    };

    /// Step 1: printable-ASCII runs of length 6..56 that are NUL terminated.
    /// When `anchors` is non-empty, only runs within `windowBytes` of an anchor
    /// are harvested - that is the measured-fast path.
    static void HarvestAsciiCandidates(const u8* buf, size_t size,
                                       const std::vector<u64>& anchors,
                                       u64 windowBytes,
                                       std::vector<AsciiCandidate>& out);

    /// Offsets of every locator string present in the buffer, best locator
    /// first. Feed these to HarvestAsciiCandidates as anchors.
    static void FindLocatorHits(const u8* buf, size_t size, std::vector<u64>& out);

    /// Step 3: walk BACKWARDS from `asciiOffset`, skip up to 8 zero bytes, then
    /// take the preceding 8/10/12/16 bytes as base-key candidates. This is the
    /// verified layout and normally hits immediately.
    /// Emits candidates best-first (10 bytes first).
    static void BaseKeyCandidatesBefore(const u8* buf, size_t size, u64 asciiOffset,
                                        std::vector<std::vector<u8>>& out);

    /// Steps 2-5 in order for one buffer:
    ///   known base keys -> backwards walk -> +-512 byte windows -> all windows.
    /// Validates with the fast root-block oracle. Returns true on a hit.
    /// `oracleTests` accumulates the work done, for the log.
    static bool HarvestAndTest(const u8* buf, size_t size,
                               const Pk2RawHeader& header,
                               const u8* encryptedRootBlock /*[2560]*/,
                               unsigned threadCount,
                               bool allowFullWindowSweep,
                               KeyCandidate& out,
                               u64* hitOffset,
                               u64* oracleTests);

    /// Scanner B core, exposed for tests: treat every 4-byte-aligned window as
    /// a BlowfishState and try the root-block oracle directly. Key-derivation
    /// independent - it recovers a working cipher even when the ascii key and
    /// its transform are unknown. `tryReversed` also tests the (S,P) layout.
    static bool ScanBufferForBlowfishState(const u8* buf, size_t size,
                                           const u8* encryptedRootBlock /*[2560]*/,
                                           bool tryReversed,
                                           unsigned threadCount,
                                           BlowfishState& out,
                                           u64* hitOffset,
                                           bool* wasReversed);

    // =======================================================================
    // Shared constants
    // =======================================================================
    static constexpr size_t kMinAsciiKeyLen = 6;
    static constexpr size_t kMaxAsciiKeyLen = 56;
    /// Base-key widths to try, best first.
    static constexpr size_t kBaseKeyWidths[] = { 10, 8, 12, 16 };
    /// Zero bytes tolerated between the base key and the ascii key.
    static constexpr size_t kMaxPaddingBytes = 8;
    /// Near-window half-width for step 4.
    static constexpr size_t kNearWindowBytes = 512;

    /// Bytes we accept inside an ASCII key run (printable, no space).
    static bool IsKeyChar(u8 c) { return c >= 0x21 && c <= 0x7E; }
};

} // namespace sro
