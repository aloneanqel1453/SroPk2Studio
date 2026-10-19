// ============================================================================
//  SRO Asset Studio - core/Config.h
//
//  Persistent settings in %APPDATA%\SROAssetStudio\ :
//
//    settings.ini   window geometry, splitter positions, preview flags,
//                   recent files, last used directories
//
//  ==========================================================================
//   NO KEY IS EVER WRITTEN TO DISK.  THIS IS A HARD PRODUCT REQUIREMENT.
//  ==========================================================================
//  There is deliberately no keys.ini and no "last key" setting. Two reasons,
//  both from the user:
//
//    1) CORRECTNESS. Servers re-key their archives when they patch. A shard can
//       keep the same name and folder a year later and have a completely
//       different key. A remembered key would then be silently wrong - it fails
//       the oracle at best, and confuses the user at worst. Re-deriving every
//       session is ALWAYS correct.
//    2) PRIVACY. The user does not want their server's key sitting in a file on
//       their machine.
//
//  Instead a discovered key lives in SessionKeys below: process memory only,
//  wiped on close. Discovery is engineered to be fast enough (see SPEC.md S2)
//  that persisting it buys nothing.
//
//  Implementation owner: Agent B (core).
// ============================================================================
#pragma once

#include "../Common.h"
#include "Blowfish.h"
#include "KeyRing.h"

namespace sro {

// ---------------------------------------------------------------------------
// Identity of an archive, used to match a session key to the right file.
// Content-derived, so a re-keyed/patched archive at the same path does NOT
// match a key discovered for the old one.
// ---------------------------------------------------------------------------
struct ArchiveFingerprint {
    u64 fileSize = 0;
    u8  checksum3[3] = { 0, 0, 0 };   ///< header.Checksum[0..2]
    u64 lastWriteTime = 0;            ///< FILETIME of the archive

    bool operator==(const ArchiveFingerprint& o) const;
    static ArchiveFingerprint fromFile(const std::wstring& path);
};

// ---------------------------------------------------------------------------
// SessionKeys - in-memory, process-lifetime key store. NEVER touches disk.
//
// Also the mechanism that stops us launching a game client once per archive:
// all the .pk2 files in one game folder virtually always share a key, so a key
// proven on Data.pk2 is tried first for Media.pk2, Map.pk2, Music.pk2 ...
// Every reuse is still validated against the target archive's own root block,
// so a wrong guess simply falls through to real discovery.
// ---------------------------------------------------------------------------
class SessionKeys {
public:
    static SessionKeys& Instance();

    /// Key already proven for exactly this archive content, this session.
    bool lookup(const ArchiveFingerprint& fp, KeyCandidate& out) const;

    /// Remember - IN MEMORY ONLY - a key that just opened this archive.
    void remember(const std::wstring& archivePath, const ArchiveFingerprint& fp,
                  const KeyCandidate& cand, const std::wstring& howFoundTr);

    /// Keys proven for other archives in this folder, best-first. Try these
    /// before launching anything; validate each against the target archive.
    std::vector<KeyCandidate> siblingCandidates(const std::wstring& archivePath) const;

    /// Every key proven this session (for the "Anahtar Bilgisi" dialog).
    std::vector<KeyCandidate> all() const;

    /// Wipe every stored key with SecureZeroMemory. Called from the destructor
    /// (process exit) and from the explicit "Kilitle" action in the UI.
    /// NOTE: do NOT call this on every archive close - that would throw away
    /// the sibling keys that let Media.pk2 open without another discovery run.
    void clear();

private:
    SessionKeys();
    ~SessionKeys();
    SessionKeys(const SessionKeys&) = delete;
    SessionKeys& operator=(const SessionKeys&) = delete;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ---------------------------------------------------------------------------
// Config - process-wide singleton.
// ---------------------------------------------------------------------------
class Config {
public:
    /// The one instance. First call loads from disk (failures are ignored -
    /// a missing config is normal on first run).
    static Config& Instance();

    /// %APPDATA%\SROAssetStudio (created on demand).
    std::wstring directory() const;

    Result load();
    Result save();

    // --- generic typed settings -------------------------------------------
    int          getInt(const wchar_t* name, int def) const;
    void         setInt(const wchar_t* name, int value);
    bool         getBool(const wchar_t* name, bool def) const;
    void         setBool(const wchar_t* name, bool value);
    std::wstring getString(const wchar_t* name, const wchar_t* def) const;
    void         setString(const wchar_t* name, const std::wstring& value);

    // --- recent files ------------------------------------------------------
    const std::vector<std::wstring>& recentFiles() const;
    void addRecentFile(const std::wstring& path);
    void clearRecentFiles();
    static constexpr size_t kMaxRecentFiles = 12;

    // --- keys ---------------------------------------------------------------
    // Intentionally absent. Keys live in SessionKeys (memory only).
    // Do not add a persistence API here; see the banner at the top of the file.

    // --- well-known setting names -----------------------------------------
    // Keep these in one place so UI agents do not invent divergent spellings.
    static constexpr const wchar_t* kWindowX          = L"WindowX";
    static constexpr const wchar_t* kWindowY          = L"WindowY";
    static constexpr const wchar_t* kWindowW          = L"WindowW";
    static constexpr const wchar_t* kWindowH          = L"WindowH";
    static constexpr const wchar_t* kWindowMaximized  = L"WindowMaximized";
    static constexpr const wchar_t* kSplitterMain     = L"SplitterMain";     ///< tree | preview
    static constexpr const wchar_t* kSplitterPreview  = L"SplitterPreview";  ///< preview | info
    static constexpr const wchar_t* kSplitterLog      = L"SplitterLog";      ///< info | log
    // NOTE: there is deliberately no kLastKey. The key box always starts at the
    // format default (169841); a discovered key is never written to settings.ini.
    static constexpr const wchar_t* kLastRawKeyMode   = L"LastRawKeyMode";
    static constexpr const wchar_t* kUiLanguage       = L"UiLanguage";   ///< "" = follow Windows
    static constexpr const wchar_t* kLastOpenDir      = L"LastOpenDir";
    static constexpr const wchar_t* kLastExtractDir   = L"LastExtractDir";
    static constexpr const wchar_t* kImageShowAlpha   = L"ImageShowAlpha";   ///< default OFF (SRO sheen alpha)
    static constexpr const wchar_t* kMeshShadingMode  = L"MeshShadingMode";
    static constexpr const wchar_t* kTreeThumbnails   = L"TreeThumbnails";

private:
    Config();
    ~Config();
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sro
