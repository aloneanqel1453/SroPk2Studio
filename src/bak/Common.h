// ============================================================================
//  SRO Asset Studio - Common.h
//  Foundation types used by every module: fixed-width integers, math vectors,
//  the Result/Status error channel, the bounds-checked ByteReader that every
//  parser is built on, string/encoding helpers, 64-bit file helpers and a
//  scope guard.
//
//  This header is intentionally HEADER-ONLY (everything inline). It has no
//  owning .cpp, so no agent needs to implement it and it can never produce a
//  link error.
//
//  Code and comments: English.  User-facing strings: Turkish.
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Windows 10 baseline: needed for per-monitor-v2 DPI and the newer shell APIs.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include "core/I18n.h"   // every visible string follows the chosen language

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <cctype>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <utility>
#include <memory>

namespace sro {

// ---------------------------------------------------------------------------
// Source encoding sanity check.
//
// All source files in this project are UTF-8. Turkish UI strings therefore
// require cl.exe to be given /utf-8. build.bat and the .vcxproj both do that
// and additionally define SRO_CHECK_SOURCE_ENCODING so this assert fires if
// somebody ever drops the flag. Plain `cl /c` without /utf-8 simply skips the
// check (the code still compiles, only Turkish literals would be mangled).
// ---------------------------------------------------------------------------
#if defined(SRO_CHECK_SOURCE_ENCODING)
static_assert(L"ğ"[0] == 0x011F,
              "Kaynak dosyalar UTF-8. Derlerken /utf-8 bayragi gerekli.");
#endif

// ---------------------------------------------------------------------------
// Fixed-width types
// ---------------------------------------------------------------------------
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

// ---------------------------------------------------------------------------
// Small math types. Silkroad is left-handed, Y-up, clockwise winding.
// ---------------------------------------------------------------------------
struct Vec2 {
    f32 x = 0.0f, y = 0.0f;
    Vec2() = default;
    Vec2(f32 X, f32 Y) : x(X), y(Y) {}
};

struct Vec3 {
    f32 x = 0.0f, y = 0.0f, z = 0.0f;
    Vec3() = default;
    Vec3(f32 X, f32 Y, f32 Z) : x(X), y(Y), z(Z) {}

    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(f32 s)        const { return Vec3(x * s, y * s, z * s); }
    f32  dot(const Vec3& o)      const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o)    const {
        return Vec3(y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x);
    }
    f32  length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 normalized() const {
        const f32 n = length();
        return (n > 1e-20f) ? Vec3(x / n, y / n, z / n) : Vec3(0.0f, 0.0f, 0.0f);
    }
};

struct Vec4 {
    f32 x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    Vec4() = default;
    Vec4(f32 X, f32 Y, f32 Z, f32 W) : x(X), y(Y), z(Z), w(W) {}
};

/// Quaternion, stored in file order (x, y, z, w) as Silkroad writes it.
struct Quat {
    f32 x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    Quat() = default;
    Quat(f32 X, f32 Y, f32 Z, f32 W) : x(X), y(Y), z(Z), w(W) {}
};

/// Axis-aligned bounding box. `valid()` is false until at least one point is added.
struct Aabb {
    Vec3 min{ 1e30f,  1e30f,  1e30f };
    Vec3 max{ -1e30f, -1e30f, -1e30f };

    bool valid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
    void add(const Vec3& p) {
        min.x = (std::min)(min.x, p.x); min.y = (std::min)(min.y, p.y); min.z = (std::min)(min.z, p.z);
        max.x = (std::max)(max.x, p.x); max.y = (std::max)(max.y, p.y); max.z = (std::max)(max.z, p.z);
    }
    void add(const Aabb& b) { if (b.valid()) { add(b.min); add(b.max); } }
    Vec3 center() const { return Vec3((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f); }
    Vec3 extent() const { return Vec3(max.x - min.x, max.y - min.y, max.z - min.z); }
    f32  radius() const { const Vec3 e = extent(); return 0.5f * std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z); }
};

// ---------------------------------------------------------------------------
// Result / Status
//
// Every fallible operation returns a Result. `Result::Ok()` is cheap (no
// allocation). Messages are Turkish because they surface directly in the UI.
// ---------------------------------------------------------------------------
enum class Status : int {
    Ok = 0,
    Cancelled,        ///< user cancelled a background operation
    NotFound,         ///< path / entry does not exist
    InvalidArgument,
    IoError,          ///< ReadFile/WriteFile/CreateFile failed
    AccessDenied,
    BadFormat,        ///< signature or structure did not match
    Truncated,        ///< buffer ended before the structure did
    Unsupported,      ///< recognised but not implemented (e.g. exotic DDS format)
    WrongKey,         ///< blowfish key failed both oracles
    OutOfMemory,
    AlreadyExists,
    Busy,             ///< archive is locked by another operation
    NotImplemented,
    Unknown
};

/// Turkish, human readable text for a status code.
inline const wchar_t* StatusText(Status s) {
    switch (s) {
        case Status::Ok:              return L"Tamam";
        case Status::Cancelled:       return T(L"Iptal edildi");
        case Status::NotFound:        return T(L"Bulunamadi");
        case Status::InvalidArgument: return T(L"Gecersiz parametre");
        case Status::IoError:         return T(L"Dosya okuma/yazma hatasi");
        case Status::AccessDenied:    return T(L"Erisim reddedildi");
        case Status::BadFormat:       return T(L"Bozuk veya taninmayan format");
        case Status::Truncated:       return T(L"Dosya eksik (beklenenden kisa)");
        case Status::Unsupported:     return T(L"Desteklenmeyen format");
        case Status::WrongKey:        return T(L"Hatali key");
        case Status::OutOfMemory:     return T(L"Bellek yetersiz");
        case Status::AlreadyExists:   return T(L"Zaten mevcut");
        case Status::Busy:            return T(L"Mesgul");
        case Status::NotImplemented:  return T(L"Henuz uygulanmadi");
        default:                      return T(L"Bilinmeyen hata");
    }
}

struct Result {
    Status       status = Status::Ok;
    std::wstring message;   ///< Turkish detail; empty means "use StatusText(status)"

    Result() = default;
    Result(Status s) : status(s) {}
    Result(Status s, std::wstring msg) : status(s), message(std::move(msg)) {}

    bool ok() const { return status == Status::Ok; }
    explicit operator bool() const { return ok(); }

    /// Message for display; falls back to the generic status text.
    std::wstring text() const { return message.empty() ? std::wstring(StatusText(status)) : message; }

    static Result Ok() { return Result(); }
    static Result Fail(Status s) { return Result(s); }
    static Result Fail(Status s, std::wstring msg) { return Result(s, std::move(msg)); }
};

// ---------------------------------------------------------------------------
// Progress / cancel / log callbacks.
//
// Long operations (extract-all, compact, key scan) run on worker threads and
// report through these. ProgressFn returns FALSE to request cancellation; the
// operation then returns Status::Cancelled. Any of them may be empty - callees
// must check before calling.
// ---------------------------------------------------------------------------
using ProgressFn = std::function<bool(u64 done, u64 total, const wchar_t* messageTr)>;
using LogFn      = std::function<void(const std::wstring& lineTr)>;

/// Helper: invoke a progress callback safely; returns false if cancelled.
inline bool ReportProgress(const ProgressFn& fn, u64 done, u64 total, const wchar_t* msg = nullptr) {
    return fn ? fn(done, total, msg) : true;
}
inline void ReportLog(const LogFn& fn, const std::wstring& line) {
    if (fn) fn(line);
}

// ---------------------------------------------------------------------------
// ScopeGuard - runs a lambda at scope exit unless dismissed.
// ---------------------------------------------------------------------------
template <typename F>
class ScopeGuard {
public:
    explicit ScopeGuard(F&& f) : m_fn(std::move(f)), m_active(true) {}
    ScopeGuard(ScopeGuard&& o) noexcept : m_fn(std::move(o.m_fn)), m_active(o.m_active) { o.m_active = false; }
    ~ScopeGuard() { if (m_active) m_fn(); }
    void dismiss() { m_active = false; }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
private:
    F    m_fn;
    bool m_active;
};

template <typename F>
ScopeGuard<F> MakeScopeGuard(F&& f) { return ScopeGuard<F>(std::forward<F>(f)); }

#define SRO_CONCAT_INNER(a, b) a##b
#define SRO_CONCAT(a, b) SRO_CONCAT_INNER(a, b)
/// Usage:  SRO_SCOPE_EXIT { CloseHandle(h); };
#define SRO_SCOPE_EXIT \
    auto SRO_CONCAT(sroScopeGuard_, __LINE__) = ::sro::detail::ScopeGuardBuilder() + [&]()

namespace detail {
struct ScopeGuardBuilder {
    template <typename F>
    ScopeGuard<F> operator+(F&& f) const { return ScopeGuard<F>(std::forward<F>(f)); }
};
} // namespace detail

// ---------------------------------------------------------------------------
// ByteReader
//
// The single bounds-checked reader used by EVERY parser in this program.
//
// Design contract:
//  * Never reads outside [data, data + size).
//  * On any out-of-range access it LATCHES a failure flag, returns a zero
//    value and does NOT advance. Every subsequent read is also a no-op.
//    => A parser may read a whole structure without checking each field and
//       then test `r.failed()` exactly once at the end.
//  * Length-prefixed strings additionally reject lengths larger than the
//    remaining buffer, so a corrupt 4 GB length can never allocate.
//  * All multi-byte integers are LITTLE ENDIAN (x86 file formats).
// ---------------------------------------------------------------------------
class ByteReader {
public:
    /// Hard cap for a single length-prefixed string. Silkroad paths are short;
    /// anything above this is certainly corruption.
    static constexpr size_t kMaxStringLen = 1u << 20; // 1 MiB

    ByteReader() noexcept : m_data(nullptr), m_size(0), m_pos(0), m_failed(false) {}
    ByteReader(const void* data, size_t size) noexcept
        : m_data(static_cast<const u8*>(data)), m_size(data ? size : 0), m_pos(0), m_failed(false) {}
    explicit ByteReader(const std::vector<u8>& v) noexcept
        : m_data(v.empty() ? nullptr : v.data()), m_size(v.size()), m_pos(0), m_failed(false) {}

    // --- state -------------------------------------------------------------
    const u8* data()      const noexcept { return m_data; }
    size_t    size()      const noexcept { return m_size; }
    size_t    pos()       const noexcept { return m_pos; }
    size_t    remaining() const noexcept { return m_failed ? 0 : (m_size - m_pos); }
    bool      failed()    const noexcept { return m_failed; }
    bool      ok()        const noexcept { return !m_failed; }
    bool      eof()       const noexcept { return m_pos >= m_size; }

    /// Latch a failure explicitly (a parser noticing a semantic problem).
    void fail() noexcept { m_failed = true; }
    /// Clear the latch; use only when deliberately probing an optional section.
    void clearFail() noexcept { m_failed = false; }

    /// Rebind to a different buffer, resetting position and the fail latch.
    void reset(const void* data, size_t size) noexcept {
        m_data = static_cast<const u8*>(data);
        m_size = data ? size : 0;
        m_pos = 0;
        m_failed = false;
    }

    // --- navigation --------------------------------------------------------
    /// True if n more bytes are available (always false once failed).
    bool canRead(size_t n) const noexcept { return !m_failed && n <= (m_size - m_pos); }

    /// Absolute seek. Out-of-range latches failure and returns false.
    bool seek(size_t absolutePos) noexcept {
        if (absolutePos > m_size) { m_failed = true; return false; }
        m_pos = absolutePos;
        return true;
    }
    /// Relative skip. Out-of-range latches failure and returns false.
    bool skip(size_t n) noexcept {
        if (!canRead(n)) { m_failed = true; return false; }
        m_pos += n;
        return true;
    }
    /// Seek used for section offsets stored inside a file (0 = "absent").
    /// Returns false without latching if `off` is 0, so callers can write:
    ///   if (r.seekSection(h.meshOffset)) { ... }
    bool seekSection(u64 off) noexcept {
        if (off == 0 || off > m_size) return false;
        m_pos = static_cast<size_t>(off);
        return true;
    }

    // --- raw ---------------------------------------------------------------
    /// Copies n bytes to dst. Returns false (and latches) if out of range.
    bool readBytes(void* dst, size_t n) noexcept {
        if (!canRead(n)) { m_failed = true; return false; }
        if (n) std::memcpy(dst, m_data + m_pos, n);
        m_pos += n;
        return true;
    }
    /// Zero-copy view of n bytes; nullptr (and latches) if out of range.
    const u8* readPtr(size_t n) noexcept {
        if (!canRead(n)) { m_failed = true; return nullptr; }
        const u8* p = m_data + m_pos;
        m_pos += n;
        return p;
    }
    /// Peek without advancing; nullptr if out of range (does NOT latch).
    const u8* peekPtr(size_t n) const noexcept {
        return canRead(n) ? (m_data + m_pos) : nullptr;
    }

    // --- integers (little endian) -----------------------------------------
    u8  readU8()  noexcept { u8  v = 0; readRaw(&v, 1); return v; }
    i8  readI8()  noexcept { return static_cast<i8>(readU8()); }
    u16 readU16() noexcept { u16 v = 0; readRaw(&v, 2); return v; }
    i16 readI16() noexcept { return static_cast<i16>(readU16()); }
    u32 readU32() noexcept { u32 v = 0; readRaw(&v, 4); return v; }
    i32 readI32() noexcept { return static_cast<i32>(readU32()); }
    u64 readU64() noexcept { u64 v = 0; readRaw(&v, 8); return v; }
    i64 readI64() noexcept { return static_cast<i64>(readU64()); }

    f32 readF32() noexcept { f32 v = 0.0f; readRaw(&v, 4); return v; }
    f64 readF64() noexcept { f64 v = 0.0;  readRaw(&v, 8); return v; }

    /// One byte interpreted as a boolean (any non-zero is true).
    bool readBool8() noexcept { return readU8() != 0; }
    /// 4-byte int interpreted as a boolean.
    bool readBool32() noexcept { return readU32() != 0; }

    // --- vectors / quaternion ---------------------------------------------
    Vec2 readVec2() noexcept { Vec2 v; v.x = readF32(); v.y = readF32(); return v; }
    Vec3 readVec3() noexcept { Vec3 v; v.x = readF32(); v.y = readF32(); v.z = readF32(); return v; }
    Vec4 readVec4() noexcept { Vec4 v; v.x = readF32(); v.y = readF32(); v.z = readF32(); v.w = readF32(); return v; }
    /// Reads x,y,z,w in file order.
    Quat readQuat() noexcept { Quat q; q.x = readF32(); q.y = readF32(); q.z = readF32(); q.w = readF32(); return q; }
    /// 24 bytes = two Vec3 (min, max) as Silkroad stores bounding boxes.
    Aabb readAabb() noexcept {
        Aabb b; const Vec3 a = readVec3(); const Vec3 c = readVec3();
        b.min = a; b.max = c; return b;
    }

    // --- strings -----------------------------------------------------------
    /// Fixed-size field of `n` bytes; the returned string stops at the first NUL.
    std::string readFixedString(size_t n) {
        const u8* p = readPtr(n);
        if (!p) return std::string();
        size_t len = 0;
        while (len < n && p[len] != 0) ++len;
        return std::string(reinterpret_cast<const char*>(p), len);
    }

    /// Signature field, default 12 bytes ("JMXVBMS 0110" etc.).
    std::string readSignature(size_t n = 12) { return readFixedString(n); }

    /// u32 length prefix followed by that many raw bytes (no NUL).
    /// A length beyond the remaining buffer (or above maxLen) latches failure
    /// and returns an empty string - it can never over-allocate.
    std::string readLenPrefixedString(size_t maxLen = kMaxStringLen) {
        const u32 len = readU32();
        if (m_failed) return std::string();
        if (len > maxLen || !canRead(len)) { m_failed = true; return std::string(); }
        const u8* p = readPtr(len);
        if (!p) return std::string();
        return std::string(reinterpret_cast<const char*>(p), len);
    }

    /// Same as readLenPrefixedString but converted from a code page (default
    /// CP1252 which matches the western ANSI paths inside Silkroad archives).
    std::wstring readLenPrefixedStringW(UINT codePage = 1252, size_t maxLen = kMaxStringLen);

    // --- alignment helper --------------------------------------------------
    /// Advance so that pos() becomes a multiple of `align`.
    bool align(size_t alignment) noexcept {
        if (alignment <= 1) return true;
        const size_t rem = m_pos % alignment;
        return rem ? skip(alignment - rem) : true;
    }

private:
    template <typename T>
    void readRaw(T* out, size_t n) noexcept {
        if (!canRead(n)) { m_failed = true; return; }
        std::memcpy(out, m_data + m_pos, n);
        m_pos += n;
    }

    const u8* m_data;
    size_t    m_size;
    size_t    m_pos;
    bool      m_failed;
};

// ---------------------------------------------------------------------------
// String / encoding helpers
// ---------------------------------------------------------------------------

/// Convert a narrow string in `codePage` to UTF-16.
inline std::wstring MultiByteToWide(const char* s, size_t len, UINT codePage) {
    if (!s || len == 0) return std::wstring();
    const int need = ::MultiByteToWideChar(codePage, 0, s, static_cast<int>(len), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    ::MultiByteToWideChar(codePage, 0, s, static_cast<int>(len), &out[0], need);
    return out;
}
inline std::wstring MultiByteToWide(const std::string& s, UINT codePage) {
    return MultiByteToWide(s.data(), s.size(), codePage);
}

/// Convert UTF-16 to a narrow string in `codePage`.
inline std::string WideToMultiByte(const wchar_t* s, size_t len, UINT codePage) {
    if (!s || len == 0) return std::string();
    const int need = ::WideCharToMultiByte(codePage, 0, s, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(codePage, 0, s, static_cast<int>(len), &out[0], need, nullptr, nullptr);
    return out;
}
inline std::string WideToMultiByte(const std::wstring& s, UINT codePage) {
    return WideToMultiByte(s.data(), s.size(), codePage);
}

inline std::wstring Utf8ToWide(const std::string& s) { return MultiByteToWide(s, CP_UTF8); }
inline std::string  WideToUtf8(const std::wstring& s) { return WideToMultiByte(s, CP_UTF8); }
inline std::wstring AnsiToWide(const std::string& s) { return MultiByteToWide(s, CP_ACP); }
inline std::string  WideToAnsi(const std::wstring& s) { return WideToMultiByte(s, CP_ACP); }

/// Archive entry names are stored as CP1252-ish ANSI. Use this for pk2 names.
inline std::wstring Pk2NameToWide(const std::string& s) { return MultiByteToWide(s, 1252); }
inline std::string  WideToPk2Name(const std::wstring& s) { return WideToMultiByte(s, 1252); }

inline std::wstring ToLowerW(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(::towlower(c));
    return s;
}
inline std::string ToLowerA(std::string s) {
    for (char& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---------------------------------------------------------------------------
// Search filtering (shared by every archive backend so the box behaves the same
// on .pk2 / .pk3 / .lib / .vp).
// ---------------------------------------------------------------------------

/// Case-insensitive glob, anchored to the whole string.
/// '*' matches any run (including empty), '?' matches exactly one character.
/// Iterative with backtracking - no recursion, so a pathological pattern can
/// not blow the stack.
inline bool WildcardMatchI(const std::wstring& textIn, const std::wstring& patIn) {
    const std::wstring text = ToLowerW(textIn);
    const std::wstring pat  = ToLowerW(patIn);
    size_t t = 0, p = 0;
    size_t starP = std::wstring::npos, starT = 0;
    while (t < text.size()) {
        if (p < pat.size() && (pat[p] == L'?' || pat[p] == text[t])) { ++t; ++p; continue; }
        if (p < pat.size() && pat[p] == L'*') { starP = p++; starT = t; continue; }
        if (starP != std::wstring::npos) { p = starP + 1; t = ++starT; continue; }
        return false;
    }
    while (p < pat.size() && pat[p] == L'*') ++p;
    return p == pat.size();
}

inline bool HasWildcard(const std::wstring& s) {
    return s.find(L'*') != std::wstring::npos || s.find(L'?') != std::wstring::npos;
}

/// The one filter rule the search box implements:
///   * plain text        -> case-insensitive SUBSTRING on the name  ("srokim")
///   * ".ext"            -> extension match                          (".bsr")
///   * contains * or ?   -> anchored glob                            ("srokim*.bsr")
///                          matched against the FULL INNER PATH when the pattern
///                          carries a separator, otherwise against the name.
/// So "srokim*.bsr" = starts with srokim and ends in .bsr, and "*srokim*.bsr" =
/// contains srokim and ends in .bsr.
inline bool FilterMatches(const std::wstring& name, const std::wstring& fullPath,
                          const std::wstring& pattern) {
    if (pattern.empty()) return true;

    // A pattern that carries a separator is about the PATH, wildcards or not -
    // pasting a full inner path such as "res\char\x.bsr" must find that file.
    const bool pathMode = pattern.find(L'\\') != std::wstring::npos ||
                          pattern.find(L'/')  != std::wstring::npos;
    if (pathMode) {
        std::wstring pat = pattern, path = fullPath;
        for (wchar_t& c : pat)  if (c == L'/') c = L'\\';
        for (wchar_t& c : path) if (c == L'/') c = L'\\';
        while (!pat.empty() && pat.front() == L'\\') pat.erase(0, 1);
        if (HasWildcard(pat)) return WildcardMatchI(path, pat);
        return ToLowerW(path).find(ToLowerW(pat)) != std::wstring::npos;
    }

    if (HasWildcard(pattern)) return WildcardMatchI(name, pattern);

    if (pattern.size() > 1 && pattern[0] == L'.') {          // ".bsr" -> extension
        const std::wstring lname = ToLowerW(name), lext = ToLowerW(pattern);
        return lname.size() >= lext.size() &&
               lname.compare(lname.size() - lext.size(), lext.size(), lext) == 0;
    }

    return ToLowerW(name).find(ToLowerW(pattern)) != std::wstring::npos;
}

/// Case-insensitive compare of two wide strings (ordinal, ASCII folding).
inline bool IEqualsW(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (::towlower(a[i]) != ::towlower(b[i])) return false;
    return true;
}

inline bool StartsWithI(const std::wstring& s, const std::wstring& prefix) {
    if (prefix.size() > s.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (::towlower(s[i]) != ::towlower(prefix[i])) return false;
    return true;
}
inline bool EndsWithI(const std::wstring& s, const std::wstring& suffix) {
    if (suffix.size() > s.size()) return false;
    const size_t off = s.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i)
        if (::towlower(s[off + i]) != ::towlower(suffix[i])) return false;
    return true;
}
inline bool ContainsI(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    const std::wstring h = ToLowerW(haystack);
    const std::wstring n = ToLowerW(needle);
    return h.find(n) != std::wstring::npos;
}

inline std::wstring TrimW(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) --b;
    return s.substr(a, b - a);
}

/// Split on a single character. Empty fields are preserved.
inline std::vector<std::wstring> SplitW(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        const size_t p = s.find(sep, start);
        if (p == std::wstring::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

/// Replace every '/' with '\\' (Silkroad inner paths use backslashes).
inline std::wstring NormalizeInnerPath(std::wstring p) {
    for (wchar_t& c : p) if (c == L'/') c = L'\\';
    while (!p.empty() && p.front() == L'\\') p.erase(p.begin());
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    return p;
}

/// "data\\res\\a.bms" -> "a.bms"
inline std::wstring PathFileName(const std::wstring& p) {
    const size_t s1 = p.find_last_of(L'\\');
    const size_t s2 = p.find_last_of(L'/');
    size_t s = std::wstring::npos;
    if (s1 == std::wstring::npos)      s = s2;
    else if (s2 == std::wstring::npos) s = s1;
    else                               s = (std::max)(s1, s2);
    return (s == std::wstring::npos) ? p : p.substr(s + 1);
}

/// "data\\res\\a.bms" -> "data\\res"  ("" when there is no separator)
inline std::wstring PathParent(const std::wstring& p) {
    const size_t s1 = p.find_last_of(L'\\');
    const size_t s2 = p.find_last_of(L'/');
    size_t s = std::wstring::npos;
    if (s1 == std::wstring::npos)      s = s2;
    else if (s2 == std::wstring::npos) s = s1;
    else                               s = (std::max)(s1, s2);
    return (s == std::wstring::npos) ? std::wstring() : p.substr(0, s);
}

/// ".bms" (lower case, with the dot) or "" when the name has no extension.
inline std::wstring PathExtension(const std::wstring& p) {
    const std::wstring name = PathFileName(p);
    const size_t d = name.find_last_of(L'.');
    if (d == std::wstring::npos) return std::wstring();
    return ToLowerW(name.substr(d));
}

/// Join two path fragments with a single backslash.
inline std::wstring PathJoin(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const bool aSep = (a.back() == L'\\' || a.back() == L'/');
    const bool bSep = (b.front() == L'\\' || b.front() == L'/');
    if (aSep && bSep) return a + b.substr(1);
    if (!aSep && !bSep) return a + L"\\" + b;
    return a + b;
}

/// Replace characters that are illegal in a Windows file name with '_'.
inline std::wstring SanitizeFileName(std::wstring name) {
    static const wchar_t bad[] = L"<>:\"/\\|?*";
    for (wchar_t& c : name) {
        if (c < 32 || wcschr(bad, c) != nullptr) c = L'_';
    }
    return name;
}

/// Human readable size, Turkish decimal comma:  "12,4 MB"
inline std::wstring FormatBytesTr(u64 bytes) {
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    wchar_t buf[64];
    if (u == 0) swprintf(buf, 64, L"%llu %s", static_cast<unsigned long long>(bytes), units[u]);
    else        swprintf(buf, 64, L"%.1f %s", v, units[u]);
    // Turkish decimal separator (other UI languages keep the point, like the rest of the app)
    if (CurrentLang() == Lang::TR)
        for (wchar_t* p = buf; *p; ++p) if (*p == L'.') *p = L',';
    return std::wstring(buf);
}

/// Thousand-separated integer, Turkish style: 1.234.567
inline std::wstring FormatCountTr(u64 n) {
    wchar_t raw[32];
    swprintf(raw, 32, L"%llu", static_cast<unsigned long long>(n));
    std::wstring s(raw), out;
    int c = 0;
    for (size_t i = s.size(); i-- > 0; ) {
        out.push_back(s[i]);
        if (++c % 3 == 0 && i != 0) out.push_back(L'.');
    }
    std::reverse(out.begin(), out.end());
    return out;
}

/// printf-style formatting into a std::wstring.
inline std::wstring FormatW(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    wchar_t stack[512];
    const int n = _vsnwprintf_s(stack, 512, _TRUNCATE, fmt, args);
    va_end(args);
    if (n >= 0) return std::wstring(stack, static_cast<size_t>(n));
    return std::wstring(stack);
}

/// Lower-case hex of a byte buffer, no separators.
inline std::wstring BytesToHexW(const u8* p, size_t n) {
    static const wchar_t* digits = L"0123456789abcdef";
    std::wstring out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(digits[(p[i] >> 4) & 0xF]);
        out.push_back(digits[p[i] & 0xF]);
    }
    return out;
}

/// Parse a hex string (optionally with spaces) into bytes. Returns false on
/// any non-hex character or an odd digit count.
inline bool HexToBytes(const std::wstring& hex, std::vector<u8>& out) {
    out.clear();
    int hi = -1;
    for (wchar_t c : hex) {
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'-' || c == L':') continue;
        int v;
        if (c >= L'0' && c <= L'9')      v = c - L'0';
        else if (c >= L'a' && c <= L'f') v = c - L'a' + 10;
        else if (c >= L'A' && c <= L'F') v = c - L'A' + 10;
        else { out.clear(); return false; }
        if (hi < 0) hi = v;
        else { out.push_back(static_cast<u8>((hi << 4) | v)); hi = -1; }
    }
    if (hi >= 0) { out.clear(); return false; }
    return true;
}

/// Windows FILETIME (100 ns since 1601) to "dd.MM.yyyy HH:mm". Empty if zero.
inline std::wstring FormatFileTimeTr(u64 ft) {
    if (ft == 0) return std::wstring();
    FILETIME f; f.dwLowDateTime = static_cast<DWORD>(ft & 0xFFFFFFFFull);
    f.dwHighDateTime = static_cast<DWORD>(ft >> 32);
    FILETIME local{}; SYSTEMTIME st{};
    if (!::FileTimeToLocalFileTime(&f, &local)) return std::wstring();
    if (!::FileTimeToSystemTime(&local, &st))   return std::wstring();
    return FormatW(L"%02u.%02u.%04u %02u:%02u", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute);
}

/// Current time as FILETIME ticks (for new archive entries).
inline u64 NowFileTime() {
    FILETIME ft{}; ::GetSystemTimeAsFileTime(&ft);
    return (static_cast<u64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// out-of-line-ish member defined after the helpers it needs
inline std::wstring ByteReader::readLenPrefixedStringW(UINT codePage, size_t maxLen) {
    const std::string s = readLenPrefixedString(maxLen);
    return MultiByteToWide(s, codePage);
}

// ---------------------------------------------------------------------------
// 64-bit file helpers
//
// Everything here uses LARGE_INTEGER / 64-bit offsets. A test archive is
// 6.26 GB, so 32-bit offsets are never acceptable.
// ---------------------------------------------------------------------------

/// RAII wrapper around a Win32 file HANDLE with 64-bit positioning.
class File {
public:
    File() = default;
    ~File() { close(); }

    File(File&& o) noexcept : m_h(o.m_h) { o.m_h = INVALID_HANDLE_VALUE; }
    File& operator=(File&& o) noexcept {
        if (this != &o) { close(); m_h = o.m_h; o.m_h = INVALID_HANDLE_VALUE; }
        return *this;
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    bool openRead(const std::wstring& path) {
        close();
        m_h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return valid();
    }
    bool openReadWrite(const std::wstring& path) {
        close();
        m_h = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return valid();
    }
    bool createNew(const std::wstring& path, bool overwrite) {
        close();
        m_h = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, overwrite ? CREATE_ALWAYS : CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        return valid();
    }
    void close() {
        if (valid()) { ::CloseHandle(m_h); m_h = INVALID_HANDLE_VALUE; }
    }

    bool   valid()  const { return m_h != INVALID_HANDLE_VALUE && m_h != nullptr; }
    HANDLE handle() const { return m_h; }

    u64 size() const {
        LARGE_INTEGER li{};
        if (!valid() || !::GetFileSizeEx(m_h, &li)) return 0;
        return static_cast<u64>(li.QuadPart);
    }
    bool seek(u64 offset) const {
        LARGE_INTEGER li{}; li.QuadPart = static_cast<LONGLONG>(offset);
        return valid() && ::SetFilePointerEx(m_h, li, nullptr, FILE_BEGIN) != FALSE;
    }
    bool readAt(u64 offset, void* dst, size_t bytes) const {
        if (!seek(offset)) return false;
        return read(dst, bytes);
    }
    bool read(void* dst, size_t bytes) const {
        u8* p = static_cast<u8*>(dst);
        size_t left = bytes;
        while (left) {
            const DWORD chunk = static_cast<DWORD>((std::min)(left, static_cast<size_t>(1u << 24)));
            DWORD got = 0;
            if (!::ReadFile(m_h, p, chunk, &got, nullptr) || got == 0) return false;
            p += got; left -= got;
        }
        return true;
    }
    bool writeAt(u64 offset, const void* src, size_t bytes) const {
        if (!seek(offset)) return false;
        return write(src, bytes);
    }
    bool write(const void* src, size_t bytes) const {
        const u8* p = static_cast<const u8*>(src);
        size_t left = bytes;
        while (left) {
            const DWORD chunk = static_cast<DWORD>((std::min)(left, static_cast<size_t>(1u << 24)));
            DWORD put = 0;
            if (!::WriteFile(m_h, p, chunk, &put, nullptr) || put == 0) return false;
            p += put; left -= put;
        }
        return true;
    }
    bool setEndOfFile(u64 size) const {
        return seek(size) && ::SetEndOfFile(m_h) != FALSE;
    }
    bool flush() const { return valid() && ::FlushFileBuffers(m_h) != FALSE; }

private:
    HANDLE m_h = INVALID_HANDLE_VALUE;
};

inline bool FileExists(const std::wstring& path) {
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
inline bool DirectoryExists(const std::wstring& path) {
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
inline u64 GetFileSize64(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (static_cast<u64>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

/// Recursively create a directory path. True if it exists afterwards.
inline bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    if (DirectoryExists(path)) return true;
    const std::wstring parent = PathParent(path);
    if (!parent.empty() && parent != path && !DirectoryExists(parent)) {
        if (!EnsureDirectory(parent)) return false;
    }
    if (::CreateDirectoryW(path.c_str(), nullptr)) return true;
    return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

/// Read a whole file into memory. `maxBytes` guards against opening a 6 GB
/// archive by accident; 0 means "no limit".
inline Result ReadEntireFile(const std::wstring& path, std::vector<u8>& out, u64 maxBytes = 0) {
    out.clear();
    File f;
    if (!f.openRead(path))
        return Result::Fail(Status::NotFound, T(L"Dosya acilamadi: ") + path);
    const u64 sz = f.size();
    if (maxBytes && sz > maxBytes)
        return Result::Fail(Status::InvalidArgument, T(L"Dosya cok buyuk: ") + FormatBytesTr(sz));
    if (sz > (u64)SIZE_MAX)
        return Result::Fail(Status::OutOfMemory, T(L"Dosya adres alanina sigmiyor"));
    try { out.resize(static_cast<size_t>(sz)); }
    catch (...) { return Result::Fail(Status::OutOfMemory, T(L"Bellek yetersiz")); }
    if (sz && !f.read(out.data(), out.size())) {
        out.clear();
        return Result::Fail(Status::IoError, T(L"Dosya okunamadi: ") + path);
    }
    return Result::Ok();
}

/// Write a buffer to a temp file next to `path` and atomically move it over
/// the target. Never truncates the user's file on a failed write.
inline Result WriteEntireFileAtomic(const std::wstring& path, const void* data, size_t bytes) {
    const std::wstring tmp = path + L".tmp";
    {
        File f;
        if (!f.createNew(tmp, true))
            return Result::Fail(Status::IoError, T(L"Gecici dosya olusturulamadi: ") + tmp);
        if (bytes && !f.write(data, bytes)) {
            f.close(); ::DeleteFileW(tmp.c_str());
            return Result::Fail(Status::IoError, T(L"Yazma hatasi: ") + tmp);
        }
        f.flush();
    }
    if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(tmp.c_str());
        return Result::Fail(Status::IoError, T(L"Dosya degistirilemedi: ") + path);
    }
    return Result::Ok();
}

/// Plain (non-atomic) write; used for freshly created output files.
inline Result WriteEntireFile(const std::wstring& path, const void* data, size_t bytes) {
    File f;
    if (!f.createNew(path, true))
        return Result::Fail(Status::IoError, T(L"Dosya olusturulamadi: ") + path);
    if (bytes && !f.write(data, bytes))
        return Result::Fail(Status::IoError, T(L"Yazma hatasi: ") + path);
    return Result::Ok();
}

/// Directory of the running executable (no trailing separator).
inline std::wstring GetExeDirectory() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    if (n == 0) return std::wstring();
    return PathParent(std::wstring(buf, n));
}

/// "%APPDATA%\SROAssetStudio" - created on demand.
inline std::wstring GetAppDataDirectory() {
    wchar_t* env = nullptr; size_t len = 0;
    std::wstring base;
    if (_wdupenv_s(&env, &len, L"APPDATA") == 0 && env) { base = env; free(env); }
    if (base.empty()) base = GetExeDirectory();
    const std::wstring dir = PathJoin(base, L"SROAssetStudio");
    EnsureDirectory(dir);
    return dir;
}

/// A unique temp file path (not created).
inline std::wstring MakeTempFilePath(const wchar_t* prefix = L"sroas") {
    wchar_t dir[MAX_PATH + 1] = {};
    const DWORD n = ::GetTempPathW(MAX_PATH, dir);
    std::wstring d = (n > 0) ? std::wstring(dir, n) : GetAppDataDirectory();
    wchar_t name[MAX_PATH + 1] = {};
    if (::GetTempFileNameW(d.c_str(), prefix, 0, name)) return std::wstring(name);
    return PathJoin(d, FormatW(L"%s_%u.tmp", prefix, ::GetTickCount()));
}

/// Enumerate the immediate children of a directory.
/// `pattern` is a Win32 wildcard, e.g. L"*.dll".
inline std::vector<std::wstring> ListDirectory(const std::wstring& dir,
                                               const std::wstring& pattern,
                                               bool wantFiles,
                                               bool wantDirs) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(PathJoin(dir, pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if ((isDir && wantDirs) || (!isDir && wantFiles)) out.push_back(PathJoin(dir, name));
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return out;
}

} // namespace sro
