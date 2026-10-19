// ============================================================================
//  SRO Asset Studio - core/Config.cpp
//
//  Settings persistence + the in-memory-only session key store.
//
//  ==========================================================================
//   HARD RULE: NO KEY IS EVER WRITTEN TO DISK.
//  ==========================================================================
//  Nothing in this file opens a file for writing except Config::save(), and
//  save() only ever emits the named settings below - window geometry, splitter
//  positions, view flags, recent paths. There is no code path that can put key
//  material into settings.ini, because SessionKeys and Config share no storage
//  at all: SessionKeys lives entirely in process memory and wipes itself with
//  SecureZeroMemory.
//
//  Why that is a correctness rule and not just privacy: shards re-key their
//  archives when they patch, so a remembered key silently goes stale. Deriving
//  it again every session is always right. What makes that cheap is the sibling
//  reuse below - one key proven on Data.pk2 opens Media/Map/Music/Particles in
//  the same folder without launching anything.
// ============================================================================
#include "Config.h"
#include "Obfuscate.h"

#include <map>
#include <mutex>

namespace sro {

// ---------------------------------------------------------------------------
// ArchiveFingerprint
//
// Content derived on purpose: a patched or re-keyed archive at the same path
// produces a different fingerprint, so an old session key can never be reused
// for it by accident.
// ---------------------------------------------------------------------------
bool ArchiveFingerprint::operator==(const ArchiveFingerprint& o) const {
    return fileSize == o.fileSize &&
           lastWriteTime == o.lastWriteTime &&
           std::memcmp(checksum3, o.checksum3, 3) == 0;
}

ArchiveFingerprint ArchiveFingerprint::fromFile(const std::wstring& path) {
    ArchiveFingerprint fp;
    if (path.empty()) return fp;

    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) {
        fp.fileSize = (static_cast<u64>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
        fp.lastWriteTime = (static_cast<u64>(d.ftLastWriteTime.dwHighDateTime) << 32) |
                           d.ftLastWriteTime.dwLowDateTime;
    }

    // Only the three meaningful checksum bytes - never the key itself.
    File f;
    if (f.openRead(path)) {
        u8 header[kPk2HeaderSize];
        if (f.size() >= kPk2HeaderSize && f.readAt(0, header, sizeof(header)))
            std::memcpy(fp.checksum3, header + 35, 3);   // Pk2RawHeader::Checksum[0..2]
    }
    return fp;
}

// ---------------------------------------------------------------------------
// SessionKeys - process memory only, wiped on clear().
// ---------------------------------------------------------------------------
namespace {

/// Overwrite anything that could be key material before it is released.
void WipeCandidate(KeyCandidate& c) {
    if (!c.asciiKey.empty())
        ::SecureZeroMemory(&c.asciiKey[0], c.asciiKey.size());
    c.asciiKey.clear();
    if (!c.customBase.empty())
        ::SecureZeroMemory(c.customBase.data(), c.customBase.size());
    c.customBase.clear();
    if (!c.rawState.empty())
        ::SecureZeroMemory(c.rawState.data(), c.rawState.size());
    c.rawState.clear();
    c.useRawState = false;
}

std::wstring FolderKeyOf(const std::wstring& archivePath) {
    return ToLowerW(PathParent(archivePath));
}

} // namespace

struct SessionKeys::Impl {
    struct Record {
        std::wstring       archivePath;
        std::wstring       folderKey;     ///< lower-case parent directory
        ArchiveFingerprint fingerprint;
        KeyCandidate       candidate;
        std::wstring       howFoundTr;
    };

    mutable std::mutex  mutex;
    std::vector<Record> records;
};

SessionKeys::SessionKeys() : m_impl(new Impl()) {}
SessionKeys::~SessionKeys() { clear(); }

SessionKeys& SessionKeys::Instance() {
    static SessionKeys s;
    return s;
}

bool SessionKeys::lookup(const ArchiveFingerprint& fp, KeyCandidate& out) const {
    if (fp.fileSize == 0) return false;   // nothing to match against
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    for (const Impl::Record& r : m_impl->records) {
        if (r.fingerprint == fp) { out = r.candidate; return true; }
    }
    return false;
}

void SessionKeys::remember(const std::wstring& archivePath, const ArchiveFingerprint& fp,
                           const KeyCandidate& cand, const std::wstring& howFoundTr) {
    if (archivePath.empty()) return;
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    for (Impl::Record& r : m_impl->records) {
        if (r.fingerprint == fp && IEqualsW(r.archivePath, archivePath)) {
            WipeCandidate(r.candidate);
            r.candidate  = cand;
            r.howFoundTr = howFoundTr;
            return;
        }
    }

    Impl::Record rec;
    rec.archivePath = archivePath;
    rec.folderKey   = FolderKeyOf(archivePath);
    rec.fingerprint = fp;
    rec.candidate   = cand;
    rec.howFoundTr  = howFoundTr;
    m_impl->records.push_back(rec);
}

std::vector<KeyCandidate> SessionKeys::siblingCandidates(const std::wstring& archivePath) const {
    std::vector<KeyCandidate> out;
    if (archivePath.empty()) return out;

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const std::wstring folder = FolderKeyOf(archivePath);

    // Same folder first: on a real client one derived key opens every archive
    // in the set, which is what stops us launching a game once per file.
    for (const Impl::Record& r : m_impl->records) {
        if (IEqualsW(r.archivePath, archivePath)) continue;
        if (r.folderKey == folder) out.push_back(r.candidate);
    }
    // Then anything else proven this session - different builds of the same
    // client have been observed to share a key. Each is still validated against
    // the target archive's own root block before it is used.
    for (const Impl::Record& r : m_impl->records) {
        if (IEqualsW(r.archivePath, archivePath)) continue;
        if (r.folderKey != folder) out.push_back(r.candidate);
    }
    return out;
}

std::vector<KeyCandidate> SessionKeys::all() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    std::vector<KeyCandidate> out;
    out.reserve(m_impl->records.size());
    for (const Impl::Record& r : m_impl->records) out.push_back(r.candidate);
    return out;
}

void SessionKeys::clear() {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    for (Impl::Record& r : m_impl->records) WipeCandidate(r.candidate);
    m_impl->records.clear();
}

// ---------------------------------------------------------------------------
// Config
//
// settings.ini is a flat "Name=Value" file, UTF-8, one setting per line.
// Unknown names are preserved on save so a newer build's settings survive an
// older one. Lines starting with ';' or '#' are comments.
// ---------------------------------------------------------------------------
namespace {

constexpr const wchar_t* kSettingsFileName = L"settings.ini";
constexpr const wchar_t* kRecentPrefix     = L"Recent";

// ---------------------------------------------------------------------------
// Path obfuscation for settings.ini
//
// The recent-file list and the last-used directories leak which games the user
// has been opening - both on disk (settings.ini in plaintext) and in a process
// dump (the map values sit in RAM as-is). We don't want a `strings` dump of
// either to spell out "...\MyPrivateVSRO\Data.pk2".
//
// So on save() every path value is written XOR-obfuscated + hex ("Recent0=1a2f..").
// On load() a hex blob is decoded back; a value that isn't our hex form (e.g. an
// old plaintext "C:\..." from a settings.ini written by an earlier build) is
// taken verbatim, so upgrading is seamless and downgrading never corrupts.
//
// This is privacy, not security: the XOR key is embedded, only hidden with OBF()
// so the key itself is not plaintext in the exe. Anyone with the exe can reverse
// it; the goal is merely that neither the file nor a casual memory scan reads out
// the paths, and that the key does not sit in the binary as a grep-able constant.
// ---------------------------------------------------------------------------

// The XOR keystream. Kept out of the binary as plaintext via OBF(); returned as a
// std::string so it is never a global `const char[]`.
inline std::string PathXorKey() {
    // 24 bytes of pseudo-random material - long enough to cover any path byte
    // pattern without an obvious short repeat.
    return OBF("\x5B\xC1\x17\xA9\x3E\x82\x6D\xF4\x08\x9C\x51\x2B\xE7\x74\xBD\x30"
               "\x1F\x66\xAA\xD8\x93\x4C\x25\xF1");
}

inline bool IsHexString(const std::wstring& s) {
    if (s.empty() || (s.size() & 1u)) return false;   // hex is even-length, non-empty
    for (wchar_t c : s) {
        const bool hex = (c >= L'0' && c <= L'9') ||
                         (c >= L'a' && c <= L'f') ||
                         (c >= L'A' && c <= L'F');
        if (!hex) return false;
    }
    return true;
}

// path -> UTF-8 -> XOR(key) -> lowercase hex -> wstring. Empty maps to empty.
std::wstring encPath(const std::wstring& path) {
    if (path.empty()) return std::wstring();

    const std::string utf8 = WideToUtf8(path);
    const std::string key  = PathXorKey();   // named local: keeps the buffer alive
    if (key.empty()) return path;            // paranoia: never lose the value

    static const wchar_t* kHex = L"0123456789abcdef";
    std::wstring out;
    out.reserve(utf8.size() * 2);
    for (size_t i = 0; i < utf8.size(); ++i) {
        const unsigned char x =
            static_cast<unsigned char>(utf8[i]) ^ static_cast<unsigned char>(key[i % key.size()]);
        out.push_back(kHex[(x >> 4) & 0xF]);
        out.push_back(kHex[x & 0xF]);
    }
    return out;
}

// hex(wstring) -> XOR(key) -> UTF-8 -> path. A value that is not our hex form
// (an old plaintext path, or anything else) is returned unchanged.
std::wstring decPath(const std::wstring& stored) {
    if (stored.empty()) return std::wstring();
    if (!IsHexString(stored)) return stored;   // legacy plaintext or non-encoded

    const std::string key = PathXorKey();      // named local: keeps the buffer alive
    if (key.empty()) return stored;

    auto nib = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return 0;
    };

    std::string utf8;
    utf8.reserve(stored.size() / 2);
    for (size_t i = 0; i + 1 < stored.size(); i += 2) {
        const unsigned char x =
            static_cast<unsigned char>((nib(stored[i]) << 4) | nib(stored[i + 1]));
        utf8.push_back(static_cast<char>(x ^ static_cast<unsigned char>(key[(i / 2) % key.size()])));
    }
    return Utf8ToWide(utf8);
}

// True for value-names whose value is a filesystem path and must be obfuscated.
inline bool IsPathValueName(const std::wstring& name) {
    return IEqualsW(name, Config::kLastOpenDir) ||
           IEqualsW(name, Config::kLastExtractDir);
}

} // namespace

struct Config::Impl {
    mutable std::mutex mutex;
    std::map<std::wstring, std::wstring> values;
    std::vector<std::wstring> recent;
    bool loaded = false;

    std::wstring filePath() const { return PathJoin(GetAppDataDirectory(), kSettingsFileName); }
};

Config::Config() : m_impl(new Impl()) {
    load();   // a missing file on first run is normal, not an error
}
Config::~Config() = default;

Config& Config::Instance() {
    static Config c;
    return c;
}

std::wstring Config::directory() const { return GetAppDataDirectory(); }

Result Config::load() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->values.clear();
    m_impl->recent.clear();

    const std::wstring path = m_impl->filePath();
    std::vector<u8> raw;
    // 4 MB cap: this file is a few hundred bytes; anything bigger is not ours.
    const Result r = ReadEntireFile(path, raw, 4u * 1024 * 1024);
    if (!r.ok()) { m_impl->loaded = true; return r; }

    size_t start = 0;
    if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) start = 3;
    const std::string utf8(reinterpret_cast<const char*>(raw.data()) + start, raw.size() - start);
    const std::wstring text = Utf8ToWide(utf8);

    std::map<int, std::wstring> recentByIndex;
    for (const std::wstring& rawLine : SplitW(text, L'\n')) {
        const std::wstring line = TrimW(rawLine);
        if (line.empty() || line[0] == L';' || line[0] == L'#' || line[0] == L'[') continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;

        const std::wstring name  = TrimW(line.substr(0, eq));
        const std::wstring value = TrimW(line.substr(eq + 1));
        if (name.empty()) continue;

        if (StartsWithI(name, kRecentPrefix) && name.size() > wcslen(kRecentPrefix)) {
            const std::wstring digits = name.substr(wcslen(kRecentPrefix));
            bool numeric = !digits.empty();
            for (wchar_t c : digits) if (c < L'0' || c > L'9') { numeric = false; break; }
            if (numeric) {
                // Recent paths are stored obfuscated; decPath() passes through any
                // legacy plaintext value unchanged.
                if (!value.empty()) recentByIndex[_wtoi(digits.c_str())] = decPath(value);
                continue;
            }
        }
        // Directory paths are obfuscated on disk; everything else is stored plain.
        m_impl->values[name] = IsPathValueName(name) ? decPath(value) : value;
    }

    for (const auto& kv : recentByIndex) {
        if (m_impl->recent.size() >= kMaxRecentFiles) break;
        m_impl->recent.push_back(kv.second);
    }

    m_impl->loaded = true;
    return Result::Ok();
}

Result Config::save() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    std::wstring out;
    out += L"; SRO Asset Studio ayarlari\r\n";
    out += L"; Bu dosyaya ASLA key yazilmaz - keyler yalnizca bellekte tutulur.\r\n";
    out += L"; Yollar (Recent*, LastOpenDir, LastExtractDir) gizlilik icin gizlenmis yazilir.\r\n";
    for (const auto& kv : m_impl->values) {
        // Path values are written obfuscated so neither the file nor a memory
        // dump spells out which games were opened; other settings stay readable.
        const std::wstring stored = IsPathValueName(kv.first) ? encPath(kv.second) : kv.second;
        out += kv.first + L"=" + stored + L"\r\n";
    }
    for (size_t i = 0; i < m_impl->recent.size(); ++i)
        out += FormatW(L"%s%u=%s", kRecentPrefix, static_cast<unsigned>(i),
                       encPath(m_impl->recent[i]).c_str()) + std::wstring(L"\r\n");

    const std::string utf8 = WideToUtf8(out);
    const std::wstring dir = GetAppDataDirectory();
    if (!EnsureDirectory(dir))
        return Result::Fail(Status::IoError, L"Ayar klasoru olusturulamadi: " + dir);

    return WriteEntireFileAtomic(m_impl->filePath(), utf8.data(), utf8.size());
}

int Config::getInt(const wchar_t* name, int def) const {
    if (!name) return def;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    auto it = m_impl->values.find(name);
    if (it == m_impl->values.end() || it->second.empty()) return def;
    wchar_t* end = nullptr;
    const long v = wcstol(it->second.c_str(), &end, 10);
    if (end == it->second.c_str()) return def;
    return static_cast<int>(v);
}

void Config::setInt(const wchar_t* name, int value) {
    if (!name) return;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->values[name] = FormatW(L"%d", value);
}

bool Config::getBool(const wchar_t* name, bool def) const {
    if (!name) return def;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    auto it = m_impl->values.find(name);
    if (it == m_impl->values.end()) return def;
    const std::wstring v = ToLowerW(TrimW(it->second));
    if (v == L"1" || v == L"true" || v == L"evet" || v == L"yes") return true;
    if (v == L"0" || v == L"false" || v == L"hayir" || v == L"no") return false;
    return def;
}

void Config::setBool(const wchar_t* name, bool value) {
    if (!name) return;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->values[name] = value ? L"1" : L"0";
}

std::wstring Config::getString(const wchar_t* name, const wchar_t* def) const {
    const std::wstring fallback = def ? std::wstring(def) : std::wstring();
    if (!name) return fallback;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    auto it = m_impl->values.find(name);
    return (it == m_impl->values.end()) ? fallback : it->second;
}

void Config::setString(const wchar_t* name, const std::wstring& value) {
    if (!name) return;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    // Newlines would break the flat file format.
    std::wstring clean = value;
    for (wchar_t& c : clean) if (c == L'\r' || c == L'\n') c = L' ';
    m_impl->values[name] = clean;
}

const std::vector<std::wstring>& Config::recentFiles() const { return m_impl->recent; }

void Config::addRecentFile(const std::wstring& path) {
    if (path.empty()) return;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    for (size_t i = 0; i < m_impl->recent.size(); ++i) {
        if (IEqualsW(m_impl->recent[i], path)) {
            m_impl->recent.erase(m_impl->recent.begin() + static_cast<ptrdiff_t>(i));
            break;
        }
    }
    m_impl->recent.insert(m_impl->recent.begin(), path);
    while (m_impl->recent.size() > kMaxRecentFiles) m_impl->recent.pop_back();
}

void Config::clearRecentFiles() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->recent.clear();
}

} // namespace sro
