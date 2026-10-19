// In-app file / folder picker (see file_browser.hpp). A modal card drawn with the app's own
// Direct2D resources: places sidebar, breadcrumb path bar, a natural-sorted list with a custom
// scrollbar, a custom-drawn text field, a filter chip and the dialog buttons. No Windows dialog,
// menu, control or message box is ever created; every problem is reported inline.
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include "file_browser.hpp"
#include "i18n.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#pragma comment(lib, "advapi32.lib")   // remembered folders: HKCU\Software\SroPk2Studio\Browse
#pragma comment(lib, "shlwapi.lib")    // StrCmpLogicalW

namespace pk2::ui {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kMaxRecent = 6;          // recent folders shown in the sidebar
constexpr std::size_t kMaxField = 32767;       // longest text the name field holds
constexpr std::size_t kMaxSyncChars = 4096;    // OpenFiles: longest "a" "b" list mirrored into the field
constexpr ULONGLONG kTypeAheadMs = 1000;       // type-ahead: a pause this long starts a new prefix
constexpr wchar_t kRegBrowseKey[] = L"Software\\SroPk2Studio\\Browse";
constexpr std::uint32_t kBlack = 0x000000;
constexpr std::uint32_t kWhite = 0xFFFFFF;

// Segoe Fluent Icons code points.
namespace glyph {
constexpr const wchar_t* kFolder = L"\uE8B7";
constexpr const wchar_t* kFolderOpen = L"\uE838";
constexpr const wchar_t* kFile = L"\uE7C3";
constexpr const wchar_t* kPackage = L"\uE7B8";
constexpr const wchar_t* kUp = L"\uE74A";
constexpr const wchar_t* kRefresh = L"\uE72C";
constexpr const wchar_t* kClose = L"\uE8BB";
constexpr const wchar_t* kSave = L"\uE74E";
constexpr const wchar_t* kDesktop = L"\uE7F4";
constexpr const wchar_t* kDocuments = L"\uE8A5";
constexpr const wchar_t* kDownloads = L"\uE896";
constexpr const wchar_t* kPictures = L"\uE8B9";
constexpr const wchar_t* kRecent = L"\uE81C";
constexpr const wchar_t* kDrive = L"\uEDA2";
constexpr const wchar_t* kUsb = L"\uE88E";
constexpr const wchar_t* kNetwork = L"\uE8CE";
constexpr const wchar_t* kFilter = L"\uE71C";
constexpr const wchar_t* kError = L"\uE783";
constexpr const wchar_t* kWarning = L"\uE7BA";
} // namespace glyph

constexpr D2D1_RECT_F R(float l, float t, float r, float b) noexcept { return D2D1_RECT_F{l, t, r, b}; }
constexpr bool inside(const D2D1_RECT_F& r, float x, float y) noexcept {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
constexpr bool empty_rect(const D2D1_RECT_F& r) noexcept { return r.right <= r.left || r.bottom <= r.top; }

// ---- RAII wrappers -------------------------------------------------------------------------
struct FindCloser {
    void operator()(HANDLE h) const noexcept { ::FindClose(h); }
};
using UniqueFind = std::unique_ptr<void, FindCloser>;

struct KeyCloser {
    void operator()(HKEY k) const noexcept { ::RegCloseKey(k); }
};
using UniqueKey = std::unique_ptr<std::remove_pointer_t<HKEY>, KeyCloser>;

struct CoTaskFree {
    void operator()(wchar_t* p) const noexcept { ::CoTaskMemFree(p); }
};
using CoTaskString = std::unique_ptr<wchar_t, CoTaskFree>;

// No "insert a disk" / "drive not ready" system boxes while drives and folders are probed.
class ErrorModeScope {
public:
    ErrorModeScope() noexcept
        : ok_(::SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_) != FALSE) {}
    ~ErrorModeScope() {
        if (ok_) ::SetThreadErrorMode(old_, nullptr);
    }
    ErrorModeScope(const ErrorModeScope&) = delete;
    ErrorModeScope& operator=(const ErrorModeScope&) = delete;

private:
    DWORD old_ = 0;
    bool ok_;
};

class ClipboardScope {
public:
    ClipboardScope() noexcept : ok_(::OpenClipboard(::GetActiveWindow()) != FALSE) {}
    ~ClipboardScope() {
        if (ok_) ::CloseClipboard();
    }
    ClipboardScope(const ClipboardScope&) = delete;
    ClipboardScope& operator=(const ClipboardScope&) = delete;
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool ok_;
};

template <class T>
class GlobalLockScope {
public:
    explicit GlobalLockScope(HGLOBAL h) noexcept : h_(h), p_(static_cast<T*>(::GlobalLock(h))) {}
    ~GlobalLockScope() {
        if (p_) ::GlobalUnlock(h_);
    }
    GlobalLockScope(const GlobalLockScope&) = delete;
    GlobalLockScope& operator=(const GlobalLockScope&) = delete;
    [[nodiscard]] T* get() const noexcept { return p_; }

private:
    HGLOBAL h_;
    T* p_;
};

// ---- clipboard -----------------------------------------------------------------------------
std::wstring clipboard_text() {
    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT)) return {};
    const ClipboardScope clip;
    if (!clip.ok()) return {};
    HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
    if (!data) return {};
    const GlobalLockScope<const wchar_t> lock(static_cast<HGLOBAL>(data));
    if (!lock.get()) return {};
    const std::size_t cap = ::GlobalSize(static_cast<HGLOBAL>(data)) / sizeof(wchar_t);
    return std::wstring(lock.get(), ::wcsnlen(lock.get(), cap));
}

bool set_clipboard_text(std::wstring_view s) {
    const ClipboardScope clip;
    if (!clip.ok()) return false;
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (!mem) return false;
    {
        const GlobalLockScope<wchar_t> lock(mem);
        if (!lock.get()) {
            ::GlobalFree(mem);
            return false;
        }
        std::ranges::copy(s, lock.get());
        lock.get()[s.size()] = L'\0';
    }
    if (!::EmptyClipboard() || !::SetClipboardData(CF_UNICODETEXT, mem)) {
        ::GlobalFree(mem);   // ownership passes to the system only on success
        return false;
    }
    return true;
}

// ---- strings and paths -------------------------------------------------------------------
bool is_sep(wchar_t c) noexcept { return c == L'\\' || c == L'/'; }

std::wstring trim(std::wstring_view s) {
    constexpr std::wstring_view ws = L" \t\r\n";
    const std::size_t b = s.find_first_not_of(ws);
    if (b == std::wstring_view::npos) return {};
    const std::size_t e = s.find_last_not_of(ws);
    return std::wstring(s.substr(b, e - b + 1));
}

bool equal_ci(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

bool starts_with_ci(std::wstring_view s, std::wstring_view prefix) noexcept {
    return s.size() >= prefix.size() && equal_ci(s.substr(0, prefix.size()), prefix);
}

std::wstring lower(std::wstring_view s) {
    std::wstring out(s);
    for (wchar_t& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

// Lower-case extension without the dot; "" when the name has none.
std::wstring extension_of(std::wstring_view name) {
    const std::size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0 || dot + 1 >= name.size()) return {};
    return lower(name.substr(dot + 1));
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n) != n) return {};
    return w;
}

// Length of the root of an absolute path ("C:\" = 3, "C:" = 2, "\\server\share\" ...); 0 for none.
std::size_t root_length(std::wstring_view p) noexcept {
    if (p.size() >= 2 && p[1] == L':' && std::iswalpha(p[0])) return p.size() >= 3 && is_sep(p[2]) ? 3 : 2;
    if (p.size() >= 2 && is_sep(p[0]) && is_sep(p[1])) {
        const std::size_t server_end = p.find_first_of(L"\\/", 2);
        if (server_end == std::wstring_view::npos) return p.size();
        const std::size_t share_end = p.find_first_of(L"\\/", server_end + 1);
        return share_end == std::wstring_view::npos ? p.size() : share_end + 1;
    }
    return 0;
}

bool is_root(std::wstring_view p) noexcept {
    const std::size_t r = root_length(p);
    return r > 0 && r >= p.size();
}

std::wstring join(std::wstring_view dir, std::wstring_view name) {
    std::wstring out(dir);
    if (!out.empty() && out.back() != L'\\') out += L'\\';
    out += name;
    return out;
}

// Absolute, backslashes only, no trailing separator except a drive root ("C:\").
std::wstring normalize(std::wstring_view in) {
    std::wstring p(in);
    std::ranges::replace(p, L'/', L'\\');
    if (p.size() == 2 && p[1] == L':') p += L'\\';
    if (p.empty()) return p;
    const DWORD need = ::GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (need == 0) return p;
    std::wstring full(need, L'\0');
    const DWORD got = ::GetFullPathNameW(p.c_str(), need, full.data(), nullptr);
    if (got == 0 || got >= need) return p;
    full.resize(got);
    const std::size_t root = root_length(full);
    while (full.size() > root && full.size() > 1 && full.back() == L'\\') full.pop_back();
    if (full.size() == 2 && full[1] == L':') full += L'\\';
    if (full.size() > 2 && full[0] == L'\\' && full[1] == L'\\' && full.back() == L'\\' && root == full.size())
        full.pop_back();   // UNC root: "\\server\share"
    return full;
}

std::wstring parent_of(const std::wstring& p) {
    if (p.empty() || is_root(p)) return {};
    const std::size_t pos = p.find_last_of(L'\\');
    if (pos == std::wstring::npos) return {};
    std::wstring out = p.substr(0, pos);
    if (out.size() == 2 && out[1] == L':') out += L'\\';
    return out;
}

std::wstring leaf_of(const std::wstring& p) {
    if (is_root(p)) return p;
    const std::size_t pos = p.find_last_of(L'\\');
    return pos == std::wstring::npos ? p : p.substr(pos + 1);
}

DWORD attributes_of(const std::wstring& p) noexcept {
    if (p.empty()) return INVALID_FILE_ATTRIBUTES;
    const ErrorModeScope quiet;
    return ::GetFileAttributesW(p.c_str());
}

bool is_dir(const std::wstring& p) noexcept {
    const DWORD a = attributes_of(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring expand_env(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) return s;
    const DWORD need = ::ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    if (need == 0) return s;
    std::wstring out(need, L'\0');
    const DWORD got = ::ExpandEnvironmentStringsW(s.c_str(), out.data(), need);
    if (got == 0 || got > need) return s;
    out.resize(got - 1);   // the count includes the terminating null
    return out;
}

// "a.txt" "b c.txt"  ->  a.txt, b c.txt;  text without quotes is one name.
std::vector<std::wstring> parse_names(std::wstring_view s) {
    std::vector<std::wstring> out;
    if (s.find(L'"') == std::wstring_view::npos) {
        if (std::wstring t = trim(s); !t.empty()) out.push_back(std::move(t));
        return out;
    }
    std::size_t i = 0;
    while (i < s.size()) {
        if (std::iswspace(s[i])) {
            ++i;
            continue;
        }
        if (s[i] == L'"') {
            const std::size_t close = s.find(L'"', i + 1);
            const std::size_t stop = close == std::wstring_view::npos ? s.size() : close;
            if (std::wstring t = trim(s.substr(i + 1, stop - i - 1)); !t.empty()) out.push_back(std::move(t));
            i = close == std::wstring_view::npos ? s.size() : close + 1;
        } else {
            const std::size_t stop = std::min(s.find_first_of(L" \t\"", i), s.size());
            out.emplace_back(s.substr(i, stop - i));
            i = stop;
        }
    }
    return out;
}

struct Segment {
    std::wstring label, path;
};

// Breadcrumb segments of an absolute folder path: "C:", "Users", "Name" with their full paths.
std::vector<Segment> split_segments(const std::wstring& p) {
    std::vector<Segment> out;
    if (p.empty()) return out;
    const std::size_t r = root_length(p);
    if (r == 0) {
        out.push_back({p, p});
        return out;
    }
    std::wstring root_path = p.substr(0, r);
    if (root_path.size() > 3 && root_path.back() == L'\\') root_path.pop_back();
    if (root_path.size() == 2) root_path += L'\\';
    std::wstring label = p.substr(0, r);
    while (label.size() > 2 && label.back() == L'\\') label.pop_back();
    out.push_back({std::move(label), root_path});
    std::wstring acc = root_path;
    std::size_t i = r;
    while (i < p.size()) {
        const std::size_t j = std::min(p.find(L'\\', i), p.size());
        if (j > i) {
            std::wstring part = p.substr(i, j - i);
            acc = join(acc, part);
            out.push_back({std::move(part), acc});
        }
        i = j + 1;
    }
    return out;
}

enum class NameProblem { None, Chars, Ends, Reserved };

// File-name rules of Windows (the name part only, never empty).
NameProblem check_name(std::wstring_view n) {
    constexpr std::wstring_view bad = L"<>:\"/\\|?*";
    for (const wchar_t c : n)
        if (c < 0x20 || bad.find(c) != std::wstring_view::npos) return NameProblem::Chars;
    if (n.back() == L' ' || n.back() == L'.') return NameProblem::Ends;
    std::wstring_view stem = n.substr(0, n.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.remove_suffix(1);
    static constexpr std::wstring_view kReserved[] = {
        L"CON",  L"PRN",  L"AUX",  L"NUL",  L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7",
        L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    for (const std::wstring_view r : kReserved)
        if (equal_ci(stem, r)) return NameProblem::Reserved;
    return NameProblem::None;
}

std::wstring format_size(std::uint64_t n) {
    if (n < 1024) return std::format(L"{} B", n);
    constexpr const wchar_t* kUnits[] = {L"KB", L"MB", L"GB"};
    double v = static_cast<double>(n) / 1024.0;
    std::size_t u = 0;
    while (v >= 1024.0 && u + 1 < std::size(kUnits)) {
        v /= 1024.0;
        ++u;
    }
    return std::format(L"{:.1f} {}", v, kUnits[u]);
}

std::wstring format_time(const FILETIME& ft) {
    SYSTEMTIME utc{}, local{};
    if (!::FileTimeToSystemTime(&ft, &utc) || !::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return {};
    return std::format(L"{:04}-{:02}-{:02} {:02}:{:02}", local.wYear, local.wMonth, local.wDay, local.wHour,
                       local.wMinute);
}

// ---- remembered folders --------------------------------------------------------------------
struct Memory {
    std::map<std::string, std::wstring, std::less<>> last_dir;   // per request kind
    std::vector<std::wstring> recent;                            // most recent first
};
Memory& memory() {
    static Memory m;
    return m;
}

std::wstring reg_load_dir(const std::string& kind) {
    const std::wstring name = widen(kind);
    if (name.empty()) return {};
    DWORD bytes = 0;
    if (::RegGetValueW(HKEY_CURRENT_USER, kRegBrowseKey, name.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &bytes) !=
            ERROR_SUCCESS ||
        bytes < sizeof(wchar_t))
        return {};
    std::wstring v(bytes / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(HKEY_CURRENT_USER, kRegBrowseKey, name.c_str(), RRF_RT_REG_SZ, nullptr, v.data(), &bytes) !=
        ERROR_SUCCESS)
        return {};
    v.resize(::wcsnlen(v.c_str(), v.size()));
    return v;
}

// Persisting is a convenience: when the registry refuses, the in-memory map still remembers.
void reg_save_dir(const std::string& kind, const std::wstring& dir) {
    const std::wstring name = widen(kind);
    if (name.empty()) return;
    HKEY raw = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRegBrowseKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &raw, nullptr) !=
        ERROR_SUCCESS)
        return;
    const UniqueKey key(raw);
    ::RegSetValueExW(key.get(), name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(dir.c_str()),
                     static_cast<DWORD>((dir.size() + 1) * sizeof(wchar_t)));
}

std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    const HRESULT hr = ::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
    const CoTaskString owned(raw);   // freed whether the call succeeded or not
    if (FAILED(hr) || !raw) return {};
    return std::wstring(raw);
}

// ---- drawing helpers -----------------------------------------------------------------------
// Sets alignment / paragraph alignment / no-wrap (and optionally ellipsis trimming) on a SHARED
// text format for one draw call and restores exactly what was there before.
class TextStyle {
public:
    TextStyle(IDWriteTextFormat* f, DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT para,
              IDWriteInlineObject* ellipsis) noexcept
        : f_(f), align_(f->GetTextAlignment()), para_(f->GetParagraphAlignment()), wrap_(f->GetWordWrapping()) {
        f_->SetTextAlignment(align);
        f_->SetParagraphAlignment(para);
        f_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (ellipsis) {
            IDWriteInlineObject* previous = nullptr;
            if (SUCCEEDED(f_->GetTrimming(&trim_, &previous))) {
                sign_.Attach(previous);
                const DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                trimmed_ = SUCCEEDED(f_->SetTrimming(&t, ellipsis));
            }
        }
    }
    ~TextStyle() {
        if (trimmed_) f_->SetTrimming(&trim_, sign_.Get());
        f_->SetWordWrapping(wrap_);
        f_->SetParagraphAlignment(para_);
        f_->SetTextAlignment(align_);
    }
    TextStyle(const TextStyle&) = delete;
    TextStyle& operator=(const TextStyle&) = delete;

private:
    IDWriteTextFormat* f_;
    DWRITE_TEXT_ALIGNMENT align_;
    DWRITE_PARAGRAPH_ALIGNMENT para_;
    DWRITE_WORD_WRAPPING wrap_;
    DWRITE_TRIMMING trim_{};
    ComPtr<IDWriteInlineObject> sign_;
    bool trimmed_ = false;
};

class ClipScope {
public:
    ClipScope(ID2D1RenderTarget* rt, const D2D1_RECT_F& r) noexcept : rt_(rt) {
        rt_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
    }
    ~ClipScope() { rt_->PopAxisAlignedClip(); }
    ClipScope(const ClipScope&) = delete;
    ClipScope& operator=(const ClipScope&) = delete;

private:
    ID2D1RenderTarget* rt_;
};

// One frame's drawing with the lent theme (brush colour set per call; the shared brush keeps
// its opacity untouched).
class Painter {
public:
    Painter(const Theme& t, float scale) noexcept : t_(t), scale_(scale) {}
    Painter(const Painter&) = delete;
    Painter& operator=(const Painter&) = delete;

    [[nodiscard]] float S(float px) const noexcept { return px * scale_; }

    void fill(const D2D1_RECT_F& r, std::uint32_t c, float a) {
        set(c, a);
        t_.rt->FillRectangle(r, t_.brush);
    }
    void fill_round(const D2D1_RECT_F& r, float rad, std::uint32_t c, float a) {
        set(c, a);
        t_.rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), t_.brush);
    }
    void stroke_round(const D2D1_RECT_F& r, float rad, std::uint32_t c, float a, float width) {
        set(c, a);
        t_.rt->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), t_.brush, width);
    }
    void line(D2D1_POINT_2F p0, D2D1_POINT_2F p1, std::uint32_t c, float a, float width) {
        set(c, a);
        t_.rt->DrawLine(p0, p1, t_.brush, width, t_.round_cap);
    }
    // Small ">" (or "v") drawn with strokes: crisp at any scale, unlike a 15 px icon glyph.
    void chevron(float cx, float cy, float h, bool down, std::uint32_t c, float a) {
        const float w = h * 0.55f;
        if (down) {
            line({cx - h, cy - w / 2}, {cx, cy + w / 2}, c, a, S(1.4f));
            line({cx, cy + w / 2}, {cx + h, cy - w / 2}, c, a, S(1.4f));
        } else {
            line({cx - w / 2, cy - h}, {cx + w / 2, cy}, c, a, S(1.4f));
            line({cx + w / 2, cy}, {cx - w / 2, cy + h}, c, a, S(1.4f));
        }
    }
    // Accent -> accent2 gradient (solid accent when the brush cannot be created).
    void fill_gradient(const D2D1_RECT_F& r, float rad, D2D1_POINT_2F from, D2D1_POINT_2F to, float opacity = 1.0f) {
        if (!ensure_gradient()) {
            fill_round(r, rad, t_.accent, opacity);
            return;
        }
        grad_->SetStartPoint(from);
        grad_->SetEndPoint(to);
        grad_->SetOpacity(opacity);
        t_.rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), grad_.Get());
    }
    void text(std::wstring_view s, IDWriteTextFormat* f, const D2D1_RECT_F& r, std::uint32_t c, float a = 1.0f,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool ellipsis = false) {
        if (!f || s.empty() || empty_rect(r)) return;
        const TextStyle style(f, align, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, ellipsis ? ellipsis_for(f) : nullptr);
        set(c, a);
        t_.rt->DrawTextW(s.data(), static_cast<UINT32>(s.size()), f, r, t_.brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void icon(std::wstring_view g, const D2D1_RECT_F& r, std::uint32_t c, float a = 1.0f) {
        text(g, t_.icon, r, c, a, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    void layout_text(D2D1_POINT_2F origin, IDWriteTextLayout* tl, std::uint32_t c, float a) {
        set(c, a);
        t_.rt->DrawTextLayout(origin, tl, t_.brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    [[nodiscard]] float measure(IDWriteTextFormat* f, std::wstring_view s) const {
        if (!f || !t_.dw || s.empty()) return 0.0f;
        ComPtr<IDWriteTextLayout> tl;
        if (FAILED(t_.dw->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), f, 100000.0f, 1000.0f, &tl)))
            return 0.0f;
        tl->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TEXT_METRICS m{};
        if (FAILED(tl->GetMetrics(&m))) return 0.0f;
        return m.widthIncludingTrailingWhitespace;
    }

private:
    void set(std::uint32_t c, float a) { t_.brush->SetColor(D2D1::ColorF(c, a)); }

    IDWriteInlineObject* ellipsis_for(IDWriteTextFormat* f) {
        for (const auto& [fmt, sign] : signs_)
            if (fmt == f) return sign.Get();
        ComPtr<IDWriteInlineObject> sign;
        if (!t_.dw || FAILED(t_.dw->CreateEllipsisTrimmingSign(f, &sign))) sign.Reset();
        signs_.emplace_back(f, sign);
        return sign.Get();
    }

    bool ensure_gradient() {
        if (grad_) return true;
        if (grad_failed_) return false;
        const D2D1_GRADIENT_STOP stops[2] = {{0.0f, D2D1::ColorF(t_.accent)}, {1.0f, D2D1::ColorF(t_.accent2)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        if (FAILED(t_.rt->CreateGradientStopCollection(stops, 2, &collection)) ||
            FAILED(t_.rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(1, 1)), collection.Get(),
                &grad_))) {
            grad_.Reset();
            grad_failed_ = true;
            return false;
        }
        return true;
    }

    const Theme& t_;
    float scale_;
    std::vector<std::pair<IDWriteTextFormat*, ComPtr<IDWriteInlineObject>>> signs_;
    ComPtr<ID2D1LinearGradientBrush> grad_;
    bool grad_failed_ = false;
};

// ---- picker model --------------------------------------------------------------------------
enum class Focus { List, Field };
enum class HitKind { None, Outside, Card, Close, Primary, Cancel, Chip, Field, Up, Refresh, Crumb, CrumbBar, Place,
                     List, Row, Thumb, Track };
struct Hit {
    HitKind kind = HitKind::None;
    int index = -1;
    bool operator==(const Hit&) const = default;
};

enum class PlaceKind { Known, Recent, Drive };
struct Place {
    PlaceKind kind = PlaceKind::Known;
    std::wstring path;
    const char* key = nullptr;      // i18n key of a known folder
    std::wstring label;             // recent: folder name; drive: volume label
    UINT drive_type = 0;
    const wchar_t* icon = nullptr;
};
struct SideItem {
    int place = -1;                 // index into places, -1 for a section header
    const char* header = nullptr;
    float top = 0, bottom = 0;
};

struct Entry {
    std::wstring name;
    std::wstring ext;               // files: lower case, without the dot
    std::uint64_t size = 0;
    FILETIME mtime{};
    bool folder = false;
    bool archive = false;
};

enum class ReadError { None, Access, NotReady, NotFound, Other };
enum class MsgKind { None, Error, Warning };

struct Crumb {
    D2D1_RECT_F rect{};
    std::wstring label, path, child;   // child: next segment, focused after the jump
    bool more = false;                 // the collapsed "…" crumb
};
struct Columns {
    float icon_l = 0, name_l = 0, name_r = 0, size_l = 0, size_r = 0, mod_l = 0, mod_r = 0;
    bool size = false, mod = false;
};
struct ThumbGeo {
    bool visible = false;
    float top = 0, bottom = 0;
};

ReadError classify(DWORD e) noexcept {
    switch (e) {
    case ERROR_ACCESS_DENIED:
        return ReadError::Access;
    case ERROR_NOT_READY:
    case ERROR_DEVICE_NOT_CONNECTED:
        return ReadError::NotReady;
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_DIRECTORY:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_PATHNAME:
        return ReadError::NotFound;
    default:
        return ReadError::Other;
    }
}

bool is_word_break(wchar_t c) noexcept { return std::wstring_view(L" \\/.\"-_:;,").find(c) != std::wstring_view::npos; }

} // namespace

// =============================================================================================
struct FileBrowser::Impl {
    // ---- request -------------------------------------------------------------------------
    bool open = false;
    BrowseRequest req;
    Done done;
    std::size_t filter = 0;

    // ---- current folder ------------------------------------------------------------------
    std::wstring cur;
    std::vector<Entry> all;                // folders first, then files; natural order
    std::vector<std::uint32_t> rows;       // shown entries (active filter) as indices into `all`
    std::vector<char> sel;                 // per row
    int cursor = -1, anchor = -1;
    std::size_t folder_count = 0, file_count = 0, filtered_out = 0;
    ReadError read_error = ReadError::None;
    DWORD read_code = 0;

    // ---- sidebar ---------------------------------------------------------------------------
    std::vector<Place> places;
    float side_scroll = 0;

    // ---- name field ----------------------------------------------------------------------
    Focus focus = Focus::List;
    std::wstring field;
    std::size_t caret = 0, field_anchor = 0;
    bool field_user = false;               // typed by the user (not mirrored from the list)
    bool field_drag = false;
    float field_scroll = 0, field_origin = 0;
    std::vector<std::pair<std::size_t, float>> field_stops;   // caret stops of the last drawn text
    std::wstring field_stops_text;

    // ---- inline message ------------------------------------------------------------------
    MsgKind msg = MsgKind::None;
    const char* msg_key = nullptr;
    std::wstring msg_arg;
    std::wstring overwrite_path;           // SaveFile: confirmed by a second click

    std::wstring type_buf;
    ULONGLONG type_tick = 0;

    float scroll = 0;
    bool thumb_drag = false;
    float thumb_grab = 0;

    // ---- geometry (client pixels) --------------------------------------------------------
    D2D1_RECT_F client{}, card{}, header{}, close_btn{}, sidebar{}, up_btn{}, refresh_btn{}, crumb_bar{},
        list_panel{}, list_head{}, rows_rect{}, track{}, footer{}, field_rect{}, chip{}, msg_rect{}, cancel_btn{},
        primary_btn{};
    float scale = 1.0f, row_h = 30.0f;
    std::vector<Crumb> crumbs;             // laid out while drawing (text measurement)

    Hit hover;
    float mx = -1.0e9f, my = -1.0e9f;
    bool dirty = false;

    // ---- small queries -------------------------------------------------------------------
    [[nodiscard]] float S(float px) const noexcept { return px * scale; }
    [[nodiscard]] int count() const noexcept { return static_cast<int>(rows.size()); }
    [[nodiscard]] bool valid(int r) const noexcept { return r >= 0 && r < count(); }
    [[nodiscard]] const Entry& entry(int r) const { return all[rows[static_cast<std::size_t>(r)]]; }
    [[nodiscard]] bool selected(int r) const noexcept { return valid(r) && sel[static_cast<std::size_t>(r)] != 0; }
    [[nodiscard]] bool multi() const noexcept { return req.mode == BrowseMode::OpenFiles; }
    [[nodiscard]] bool has_filters() const noexcept {
        return req.mode != BrowseMode::PickFolder && !req.filters.empty();
    }
    [[nodiscard]] bool can_cycle_filter() const noexcept { return has_filters() && req.filters.size() > 1; }
    [[nodiscard]] const BrowseFilter* active_filter() const noexcept {
        return has_filters() ? &req.filters[filter] : nullptr;
    }
    [[nodiscard]] bool can_go_up() const noexcept { return !cur.empty() && !is_root(cur); }
    [[nodiscard]] float view_h() const noexcept { return std::max(0.0f, rows_rect.bottom - rows_rect.top); }
    [[nodiscard]] float content_h() const noexcept { return static_cast<float>(rows.size()) * row_h; }

    [[nodiscard]] int single_selected() const noexcept {
        int found = -1;
        for (int i = 0; i < count(); ++i) {
            if (!selected(i)) continue;
            if (found >= 0) return -1;
            found = i;
        }
        return found;
    }
    [[nodiscard]] std::size_t selected_file_count() const {
        std::size_t n = 0;
        for (int i = 0; i < count(); ++i)
            if (selected(i) && !entry(i).folder) ++n;
        return n;
    }
    [[nodiscard]] bool primary_enabled() const noexcept {
        if (req.mode == BrowseMode::PickFolder) return !cur.empty();
        const bool typed = field.find_first_not_of(L" \t") != std::wstring::npos;
        if (req.mode == BrowseMode::SaveFile) return typed;
        return typed || std::ranges::any_of(sel, [](char c) { return c != 0; });
    }

    // ---- layout --------------------------------------------------------------------------
    void do_layout() {
        const float W = client.right - client.left, H = client.bottom - client.top;
        if (W <= 0 || H <= 0) {
            card = {};
            return;
        }
        row_h = S(30);
        const float margin = S(24), pad = S(16);
        float cw = std::min(S(920), W - 2 * margin), ch = std::min(S(600), H - 2 * margin);
        cw = std::max(cw, std::min(W, S(360)));
        ch = std::max(ch, std::min(H, S(340)));
        const float x0 = client.left + (W - cw) / 2, y0 = client.top + (H - ch) / 2;
        card = R(x0, y0, x0 + cw, y0 + ch);
        header = R(card.left, card.top, card.right, card.top + S(56));
        close_btn = R(card.right - pad - S(36), card.top + S(12), card.right - pad, card.top + S(44));
        footer = R(card.left, card.bottom - S(112), card.right, card.bottom);
        const float body_top = header.bottom + S(12);
        const float body_bottom = std::max(body_top + S(80), footer.top - S(10));

        const float side_w = cw >= S(640) ? S(206) : cw >= S(480) ? S(160) : 0.0f;
        sidebar = side_w > 0 ? R(card.left + pad, body_top, card.left + pad + side_w, body_bottom) : D2D1_RECT_F{};
        const float main_l = side_w > 0 ? sidebar.right + S(14) : card.left + pad, main_r = card.right - pad;
        up_btn = R(main_l, body_top, main_l + S(36), body_top + S(36));
        refresh_btn = R(up_btn.right + S(6), body_top, up_btn.right + S(42), body_top + S(36));
        crumb_bar = R(refresh_btn.right + S(8), body_top, main_r, body_top + S(36));
        list_panel = R(main_l, crumb_bar.bottom + S(10), main_r, body_bottom);
        list_head = R(list_panel.left, list_panel.top, list_panel.right, list_panel.top + S(30));
        track = R(list_panel.right - S(13), list_head.bottom + S(4), list_panel.right - S(5), list_panel.bottom - S(6));
        rows_rect = R(list_panel.left + S(2), list_head.bottom + S(2), track.left - S(3), list_panel.bottom - S(4));

        const float row1 = footer.top + S(14), row2 = row1 + S(38) + S(12);
        const float chip_w = has_filters() ? std::min(S(230), (cw - 2 * pad) * 0.34f) : 0.0f;
        chip = has_filters() ? R(card.right - pad - chip_w, row1, card.right - pad, row1 + S(38)) : D2D1_RECT_F{};
        field_rect = R(card.left + pad, row1, has_filters() ? chip.left - S(10) : card.right - pad, row1 + S(38));
        primary_btn = R(card.right - pad - S(136), row2, card.right - pad, row2 + S(36));
        cancel_btn = R(primary_btn.left - S(10) - S(112), row2, primary_btn.left - S(10), row2 + S(36));
        msg_rect = R(card.left + pad, row2, cancel_btn.left - S(12), row2 + S(36));
        clamp_scroll();
        clamp_side_scroll();
    }

    [[nodiscard]] Columns columns() const {
        Columns c;
        const float l = rows_rect.left, r = rows_rect.right, w = r - l;
        c.mod = w >= S(520);
        c.size = w >= S(360);
        float x = r - S(10);
        if (c.mod) {
            c.mod_r = x;
            c.mod_l = x - S(140);
            x = c.mod_l - S(14);
        }
        if (c.size) {
            c.size_r = x;
            c.size_l = x - S(84);
            x = c.size_l - S(12);
        }
        c.icon_l = l + S(10);
        c.name_l = l + S(42);
        c.name_r = x;
        return c;
    }

    [[nodiscard]] static const char* section_key(PlaceKind k) noexcept {
        switch (k) {
        case PlaceKind::Known: return "fb_places";
        case PlaceKind::Recent: return "fb_recent";
        case PlaceKind::Drive: return "fb_drives";
        }
        return "fb_places";
    }

    [[nodiscard]] std::vector<SideItem> side_items() const {
        std::vector<SideItem> out;
        out.reserve(places.size() + 3);
        float y = sidebar.top + S(6) - side_scroll;
        for (std::size_t i = 0; i < places.size(); ++i) {
            if (i == 0 || places[i].kind != places[i - 1].kind) {
                if (i != 0) y += S(6);
                out.push_back({-1, section_key(places[i].kind), y, y + S(26)});
                y += S(26);
            }
            out.push_back({static_cast<int>(i), nullptr, y, y + S(32)});
            y += S(32);
        }
        return out;
    }

    void clamp_scroll() noexcept { scroll = std::clamp(scroll, 0.0f, std::max(0.0f, content_h() - view_h())); }

    [[nodiscard]] float side_content(const std::vector<SideItem>& items) const noexcept {
        return items.empty() ? 0.0f : items.back().bottom + side_scroll - sidebar.top + S(6);
    }

    void clamp_side_scroll() {
        const float content = side_content(side_items());
        side_scroll = std::clamp(side_scroll, 0.0f, std::max(0.0f, content - (sidebar.bottom - sidebar.top)));
    }

    [[nodiscard]] ThumbGeo thumb() const noexcept {
        const float view = view_h(), content = content_h(), track_h = track.bottom - track.top;
        if (content <= view || track_h <= 0 || read_error != ReadError::None) return {};
        const float h = std::min(track_h, std::max(S(36), track_h * view / content));
        const float top = track.top + (scroll / (content - view)) * (track_h - h);
        return {true, top, top + h};
    }

    void ensure_visible(int r) noexcept {
        const float top = static_cast<float>(r) * row_h, view = view_h();
        if (top < scroll) scroll = top;
        else if (top + row_h > scroll + view) scroll = top + row_h - view;
        clamp_scroll();
    }

    // ---- hit testing ---------------------------------------------------------------------
    [[nodiscard]] Hit hit_test(float x, float y) const {
        if (!inside(card, x, y)) return {HitKind::Outside};
        if (inside(close_btn, x, y)) return {HitKind::Close};
        if (inside(primary_btn, x, y)) return {HitKind::Primary};
        if (inside(cancel_btn, x, y)) return {HitKind::Cancel};
        if (has_filters() && inside(chip, x, y)) return {HitKind::Chip};
        if (inside(field_rect, x, y)) return {HitKind::Field};
        if (inside(up_btn, x, y)) return {HitKind::Up};
        if (inside(refresh_btn, x, y)) return {HitKind::Refresh};
        for (std::size_t i = 0; i < crumbs.size(); ++i)
            if (inside(crumbs[i].rect, x, y)) return {HitKind::Crumb, static_cast<int>(i)};
        if (inside(crumb_bar, x, y)) return {HitKind::CrumbBar};
        if (!empty_rect(sidebar) && inside(sidebar, x, y)) {
            for (const SideItem& it : side_items())
                if (it.place >= 0 && y >= it.top && y < it.bottom) return {HitKind::Place, it.place};
            return {HitKind::Card};
        }
        if (inside(list_panel, x, y)) {
            if (const ThumbGeo th = thumb();
                th.visible && x >= track.left - S(3) && x < track.right + S(3) && y >= track.top && y < track.bottom)
                return {y >= th.top && y < th.bottom ? HitKind::Thumb : HitKind::Track};
            if (read_error == ReadError::None && inside(rows_rect, x, y)) {
                const int r = static_cast<int>((y - rows_rect.top + scroll) / row_h);
                if (valid(r)) return {HitKind::Row, r};
            }
            return {HitKind::List};
        }
        return {HitKind::Card};
    }

    [[nodiscard]] bool hand() const noexcept {
        switch (hover.kind) {
        case HitKind::Close:
        case HitKind::Cancel:
        case HitKind::Refresh:
        case HitKind::Crumb:
        case HitKind::Place:
            return true;
        case HitKind::Primary:
            return primary_enabled();
        case HitKind::Up:
            return can_go_up();
        case HitKind::Chip:
            return can_cycle_filter();
        default:
            return false;
        }
    }

    // ---- opening and closing -------------------------------------------------------------
    void start(BrowseRequest request, Done callback) {
        req = std::move(request);
        done = std::move(callback);
        for (BrowseFilter& f : req.filters)
            for (std::wstring& e : f.exts) {
                if (e.starts_with(L"*.")) e.erase(0, 2);
                else if (e.starts_with(L'.')) e.erase(0, 1);
                e = lower(e);
            }
        while (req.default_ext.starts_with(L'.')) req.default_ext.erase(0, 1);
        filter = 0;

        all.clear();
        rows.clear();
        sel.clear();
        cursor = anchor = -1;
        scroll = side_scroll = 0;
        field.clear();
        caret = field_anchor = 0;
        field_user = field_drag = thumb_drag = false;
        field_scroll = 0;
        field_stops.clear();
        field_stops_text.clear();
        msg = MsgKind::None;
        msg_key = nullptr;
        msg_arg.clear();
        overwrite_path.clear();
        type_buf.clear();
        crumbs.clear();
        hover = {};
        focus = req.mode == BrowseMode::SaveFile ? Focus::Field : Focus::List;
        open = true;
        dirty = true;

        do_layout();   // the filter chip depends on the mode and the filters
        load_places();
        navigate(start_folder(), {});
        if (req.mode == BrowseMode::SaveFile) set_field_stem(req.file_name);
    }

    [[nodiscard]] std::wstring start_folder() const {
        const Memory& m = memory();
        if (const auto it = m.last_dir.find(req.kind); it != m.last_dir.end() && is_dir(it->second)) return it->second;
        if (const std::wstring saved = reg_load_dir(req.kind); !saved.empty()) {
            const std::wstring n = normalize(saved);
            if (is_dir(n)) return n;
        }
        if (!req.start_dir.empty()) {
            const std::wstring s = normalize(expand_env(req.start_dir));
            if (is_dir(s)) return s;
            if (const std::wstring parent = parent_of(s); is_dir(parent)) return parent;
        }
        if (const std::wstring docs = known_folder(FOLDERID_Documents); !docs.empty()) {
            const std::wstring n = normalize(docs);
            if (is_dir(n)) return n;
        }
        wchar_t windir[MAX_PATH] = {};
        const UINT n = ::GetSystemWindowsDirectoryW(windir, MAX_PATH);
        if (n >= 3 && n < MAX_PATH) return std::wstring(windir, 3);   // the system drive root
        return L"C:\\";
    }

    void remember(const std::vector<std::wstring>& paths) {
        std::wstring folder = req.mode == BrowseMode::PickFolder ? paths.front() : parent_of(paths.front());
        if (folder.empty()) folder = cur;
        if (folder.empty()) return;
        Memory& m = memory();
        m.last_dir[req.kind] = folder;
        reg_save_dir(req.kind, folder);
        std::erase_if(m.recent, [&](const std::wstring& r) { return equal_ci(r, folder); });
        m.recent.insert(m.recent.begin(), folder);
        if (m.recent.size() > kMaxRecent) m.recent.resize(kMaxRecent);
    }

    // Closes the picker, then reports. Nothing of this object is touched after the callback
    // (it may open the picker again).
    void finish(std::vector<std::wstring> paths) {
        if (!paths.empty()) remember(paths);
        open = false;
        dirty = true;
        thumb_drag = field_drag = false;
        hover = {};
        std::vector<Entry>().swap(all);
        std::vector<std::uint32_t>().swap(rows);
        std::vector<char>().swap(sel);
        crumbs.clear();
        field_stops.clear();
        Done callback = std::move(done);
        done = nullptr;
        if (callback) callback(std::move(paths));
    }

    // ---- places --------------------------------------------------------------------------
    void load_places() {
        places.clear();
        struct Known {
            const KNOWNFOLDERID* id;
            const char* key;
            const wchar_t* icon;
        };
        const Known known[] = {{&FOLDERID_Desktop, "fb_desktop", glyph::kDesktop},
                               {&FOLDERID_Documents, "fb_documents", glyph::kDocuments},
                               {&FOLDERID_Downloads, "fb_downloads", glyph::kDownloads},
                               {&FOLDERID_Pictures, "fb_pictures", glyph::kPictures}};
        for (const Known& k : known)
            if (const std::wstring p = known_folder(*k.id); !p.empty())
                places.push_back({PlaceKind::Known, normalize(p), k.key, {}, 0, k.icon});
        for (const std::wstring& r : memory().recent)
            places.push_back({PlaceKind::Recent, r, nullptr, leaf_of(r), 0, glyph::kRecent});

        const ErrorModeScope quiet;
        const DWORD mask = ::GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if ((mask & (1u << i)) == 0) continue;
            const std::wstring root{static_cast<wchar_t>(L'A' + i), L':', L'\\'};
            const UINT type = ::GetDriveTypeW(root.c_str());
            if (type == DRIVE_UNKNOWN || type == DRIVE_NO_ROOT_DIR) continue;
            std::wstring label;
            if (type != DRIVE_REMOTE) {   // a disconnected network drive could block here for long
                wchar_t name[MAX_PATH + 1] = {};
                if (!::GetVolumeInformationW(root.c_str(), name, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0))
                    continue;             // not ready (no medium)
                label = name;
            }
            const wchar_t* icon = type == DRIVE_REMOVABLE ? glyph::kUsb : type == DRIVE_REMOTE ? glyph::kNetwork
                                                                                                 : glyph::kDrive;
            places.push_back({PlaceKind::Drive, root, nullptr, std::move(label), type, icon});
        }
        clamp_side_scroll();
    }

    [[nodiscard]] static std::wstring place_label(const Place& p) {
        switch (p.kind) {
        case PlaceKind::Known:
            return L(p.key);
        case PlaceKind::Recent:
            return p.label;
        case PlaceKind::Drive: {
            const char* type_key = p.drive_type == DRIVE_REMOVABLE ? "fb_drive_removable"
                                   : p.drive_type == DRIVE_REMOTE  ? "fb_drive_network"
                                   : p.drive_type == DRIVE_CDROM   ? "fb_drive_cd"
                                                                   : "fb_drive_local";
            return (p.label.empty() ? L(type_key) : p.label) + L" (" + p.path.substr(0, 2) + L")";
        }
        }
        return p.path;
    }

    // ---- folder contents -----------------------------------------------------------------
    void read_folder() {
        all.clear();
        read_error = ReadError::None;
        read_code = 0;
        if (cur.empty()) {
            read_error = ReadError::NotFound;
            return;
        }
        const ErrorModeScope quiet;
        const std::wstring pattern = join(cur, L"*");
        WIN32_FIND_DATAW fd{};
        HANDLE raw = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                        FIND_FIRST_EX_LARGE_FETCH);
        if (raw == INVALID_HANDLE_VALUE) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_FILE_NOT_FOUND && err != ERROR_NO_MORE_FILES) {   // those: an empty folder
                read_code = err;
                read_error = classify(err);
            }
            return;
        }
        const UniqueFind guard(raw);
        do {
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0'))) continue;
            const DWORD attr = fd.dwFileAttributes;
            const bool is_folder = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
            // Hidden entries (incl. protected system files) and system files stay out, like Explorer.
            if ((attr & FILE_ATTRIBUTE_HIDDEN) != 0 || (!is_folder && (attr & FILE_ATTRIBUTE_SYSTEM) != 0)) continue;
            Entry e;
            e.name = n;
            e.folder = is_folder;
            e.mtime = fd.ftLastWriteTime;
            if (!is_folder) {
                e.size = (std::uint64_t{fd.nFileSizeHigh} << 32) | fd.nFileSizeLow;
                e.ext = extension_of(e.name);
                e.archive = e.ext == L"pk2" || e.ext == L"pk3" || e.ext == L"lib" || e.ext == L"vp";
            }
            all.push_back(std::move(e));
        } while (::FindNextFileW(raw, &fd));
        if (const DWORD err = ::GetLastError(); err != ERROR_NO_MORE_FILES && all.empty()) {
            read_code = err;
            read_error = classify(err);
        }
        std::ranges::sort(all, [](const Entry& a, const Entry& b) {
            if (a.folder != b.folder) return a.folder;
            return ::StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
        });
    }

    void apply_filter() {
        rows.clear();
        folder_count = file_count = filtered_out = 0;
        const BrowseFilter* f = active_filter();
        const bool show_files = req.mode != BrowseMode::PickFolder;
        for (std::size_t i = 0; i < all.size(); ++i) {
            const Entry& e = all[i];
            if (e.folder) {
                rows.push_back(static_cast<std::uint32_t>(i));
                ++folder_count;
                continue;
            }
            if (!show_files) continue;
            if (f && !f->exts.empty() && !std::ranges::contains(f->exts, e.ext)) {
                ++filtered_out;
                continue;
            }
            rows.push_back(static_cast<std::uint32_t>(i));
            ++file_count;
        }
        sel.assign(rows.size(), 0);
        cursor = anchor = -1;
        clamp_scroll();
    }

    // Opens `target`; `focus_name` (the folder we came from) gets the keyboard cursor.
    void navigate(std::wstring target, std::wstring focus_name) {
        cur = normalize(target);
        read_folder();
        apply_filter();
        scroll = 0;
        if (!focus_name.empty())
            for (int i = 0; i < count(); ++i)
                if (equal_ci(entry(i).name, focus_name)) {
                    cursor = anchor = i;
                    ensure_visible(i);
                    break;
                }
        type_buf.clear();
        overwrite_path.clear();
        clear_message();
        switch (req.mode) {
        case BrowseMode::PickFolder:
            set_field(cur, false);
            break;
        case BrowseMode::OpenFile:
        case BrowseMode::OpenFiles:
            if (!field_user) set_field({}, false);
            break;
        case BrowseMode::SaveFile:
            break;
        }
        hover = hit_test(mx, my);
        dirty = true;
    }

    // After a typed path took us somewhere: the field starts over.
    void navigate_typed(std::wstring target) {
        navigate(std::move(target), {});
        switch (req.mode) {
        case BrowseMode::SaveFile:
            set_field_stem(req.file_name);
            break;
        case BrowseMode::OpenFile:
        case BrowseMode::OpenFiles:
            set_field({}, false);
            break;
        case BrowseMode::PickFolder:
            break;   // navigate() already shows the new folder
        }
    }

    void enter_folder(int r) { navigate(join(cur, entry(r).name), {}); }

    void go_up() {
        if (!can_go_up()) return;
        navigate(parent_of(cur), leaf_of(cur));
    }

    void refresh() {
        std::set<std::wstring> keep;
        for (int i = 0; i < count(); ++i)
            if (selected(i)) keep.insert(entry(i).name);
        const std::wstring focus_name = valid(cursor) ? entry(cursor).name : std::wstring();
        const float old_scroll = scroll;
        load_places();
        read_folder();
        apply_filter();
        for (int i = 0; i < count(); ++i) {
            const Entry& e = entry(i);
            if (keep.contains(e.name)) sel[static_cast<std::size_t>(i)] = 1;
            if (!focus_name.empty() && e.name == focus_name) cursor = anchor = i;
        }
        scroll = old_scroll;
        clamp_scroll();
        hover = hit_test(mx, my);
        dirty = true;
    }

    void cycle_filter() {
        if (!can_cycle_filter()) return;
        filter = (filter + 1) % req.filters.size();
        apply_filter();
        scroll = 0;
        overwrite_path.clear();
        clear_message();
        if (!field_user && req.mode != BrowseMode::SaveFile) set_field({}, false);
        dirty = true;
    }

    // ---- selection -----------------------------------------------------------------------
    void after_selection_change() {
        overwrite_path.clear();
        clear_message();
        sync_field();
        dirty = true;
    }

    void select_only(int r) {
        sel.assign(rows.size(), 0);
        if (valid(r)) sel[static_cast<std::size_t>(r)] = 1;
        cursor = anchor = r;
        after_selection_change();
    }

    void select_range(int from, int to, bool keep_others) {
        if (!keep_others) sel.assign(rows.size(), 0);
        for (int i = std::min(from, to); i <= std::max(from, to); ++i)
            if (valid(i)) sel[static_cast<std::size_t>(i)] = 1;
        cursor = to;
        after_selection_change();
    }

    void clear_selection() {
        sel.assign(rows.size(), 0);
        cursor = anchor = -1;
        after_selection_change();
    }

    void select_all_files() {
        for (int i = 0; i < count(); ++i) sel[static_cast<std::size_t>(i)] = static_cast<char>(entry(i).folder ? 0 : 1);
        after_selection_change();
    }

    void move_cursor(int to, bool extend) {
        if (rows.empty()) return;
        to = std::clamp(to, 0, count() - 1);
        if (extend && multi() && valid(anchor)) select_range(anchor, to, false);
        else select_only(to);
        ensure_visible(to);
        type_buf.clear();
    }

    // The field mirrors the list selection (unless the user is typing there).
    void sync_field() {
        switch (req.mode) {
        case BrowseMode::PickFolder: {
            const int s = single_selected();
            set_field(s >= 0 && entry(s).folder ? join(cur, entry(s).name) : cur, false);
            return;
        }
        case BrowseMode::OpenFiles: {
            std::vector<int> files;
            for (int i = 0; i < count(); ++i)
                if (selected(i) && !entry(i).folder) files.push_back(i);
            if (files.empty()) {
                if (!field_user) set_field({}, false);
                return;
            }
            if (files.size() == 1) {
                set_field(entry(files.front()).name, false);
                return;
            }
            std::wstring s;
            for (const int i : files) {
                if (s.size() > kMaxSyncChars) break;   // accepting uses the selection itself
                if (!s.empty()) s += L' ';
                s += L'"';
                s += entry(i).name;
                s += L'"';
            }
            set_field(std::move(s), false);
            return;
        }
        case BrowseMode::OpenFile:
        case BrowseMode::SaveFile:
            if (selected(cursor) && !entry(cursor).folder) set_field(entry(cursor).name, false);
            else if (req.mode == BrowseMode::OpenFile && !field_user) set_field({}, false);
            return;
        }
    }

    void click_row(int r, bool dbl, bool ctrl, bool shift) {
        focus = Focus::List;
        type_buf.clear();
        if (!valid(r)) return;
        if (dbl) {
            activate_row(r);
            return;
        }
        if (multi() && shift && valid(anchor)) {
            select_range(anchor, r, ctrl);
        } else if (multi() && ctrl) {
            char& s = sel[static_cast<std::size_t>(r)];
            s = static_cast<char>(s != 0 ? 0 : 1);
            cursor = anchor = r;
            after_selection_change();
        } else {
            select_only(r);
        }
    }

    void activate_row(int r) {
        if (entry(r).folder) {
            enter_folder(r);
            return;
        }
        select_only(r);
        accept(false);
    }

    void list_enter() {
        if (selected(cursor) && entry(cursor).folder && !(multi() && selected_file_count() > 0)) {
            enter_folder(cursor);
            return;
        }
        accept(false);
    }

    void type_ahead(wchar_t c) {
        if (rows.empty()) return;
        const ULONGLONG now = ::GetTickCount64();
        if (now - type_tick > kTypeAheadMs) type_buf.clear();
        type_tick = now;
        if (type_buf.empty() && c == L' ') return;
        type_buf += c;
        // The same letter again cycles through the names starting with it.
        const bool repeat = std::ranges::all_of(type_buf, [&](wchar_t ch) {
            return std::towlower(ch) == std::towlower(type_buf.front());
        });
        const std::wstring_view prefix = repeat ? std::wstring_view(type_buf).substr(0, 1) : std::wstring_view(type_buf);
        const int n = count();
        const int from = repeat ? cursor + 1 : std::max(cursor, 0);
        for (int k = 0; k < n; ++k) {
            const int i = ((from + k) % n + n) % n;
            if (starts_with_ci(entry(i).name, prefix)) {
                const std::wstring keep = type_buf;   // move_cursor() starts a fresh prefix
                move_cursor(i, false);
                type_buf = keep;
                return;
            }
        }
    }

    // ---- name field ----------------------------------------------------------------------
    void set_field(std::wstring text, bool user) {
        if (text.size() > kMaxField) text.resize(kMaxField);
        field = std::move(text);
        caret = field_anchor = field.size();
        field_user = user;
        dirty = true;
    }

    // SaveFile: the proposed name with its stem selected, ready to be typed over.
    void set_field_stem(const std::wstring& name) {
        set_field(name, false);
        const std::size_t dot = field.find_last_of(L'.');
        field_anchor = 0;
        caret = dot != std::wstring::npos && dot > 0 ? dot : field.size();
    }

    void field_edited() {
        field_user = true;
        overwrite_path.clear();
        clear_message();
        dirty = true;
    }

    void replace_selection(std::wstring_view with) {
        std::wstring clean;
        clean.reserve(with.size());
        for (wchar_t c : with) {
            if (c == L'\r') continue;
            if (c == L'\n' || c == L'\t') c = L' ';
            if (c < 0x20 || c == 0x7F) continue;
            clean.push_back(c);
        }
        const std::size_t a = std::min(caret, field_anchor), b = std::max(caret, field_anchor);
        const std::size_t room = kMaxField - (field.size() - (b - a));
        if (clean.size() > room) clean.resize(room);
        field.replace(a, b - a, clean);
        caret = field_anchor = a + clean.size();
        field_edited();
    }

    [[nodiscard]] std::size_t prev_pos(std::size_t p) const noexcept {
        if (p == 0) return 0;
        --p;
        if (p > 0 && IS_LOW_SURROGATE(field[p]) && IS_HIGH_SURROGATE(field[p - 1])) --p;
        return p;
    }
    [[nodiscard]] std::size_t next_pos(std::size_t p) const noexcept {
        if (p >= field.size()) return field.size();
        ++p;
        if (p < field.size() && IS_LOW_SURROGATE(field[p]) && IS_HIGH_SURROGATE(field[p - 1])) ++p;
        return p;
    }
    [[nodiscard]] std::size_t word_left(std::size_t p) const noexcept {
        while (p > 0 && is_word_break(field[p - 1])) --p;
        while (p > 0 && !is_word_break(field[p - 1])) --p;
        return p;
    }
    [[nodiscard]] std::size_t word_right(std::size_t p) const noexcept {
        const std::size_t n = field.size();
        while (p < n && !is_word_break(field[p])) ++p;
        while (p < n && is_word_break(field[p])) ++p;
        return p;
    }
    void field_move(std::size_t to, bool extend) {
        caret = std::min(to, field.size());
        if (!extend) field_anchor = caret;
        dirty = true;
    }

    bool copy_selection() {
        if (caret == field_anchor) return false;
        const std::size_t a = std::min(caret, field_anchor), b = std::max(caret, field_anchor);
        return set_clipboard_text(std::wstring_view(field).substr(a, b - a));
    }

    void paste() {
        const std::wstring s = clipboard_text();
        if (!s.empty()) replace_selection(s);
    }

    [[nodiscard]] float stop_x(std::size_t pos) const noexcept {
        float x = 0;
        for (const auto& [p, sx] : field_stops) {
            if (p > pos) break;
            x = sx;
        }
        return x;
    }

    [[nodiscard]] std::size_t caret_from_x(float x) const {
        if (field_stops.empty() || field_stops_text != field) return field.size();
        const float rel = x - field_origin;
        std::size_t best = field.size();
        float best_d = FLT_MAX;
        for (const auto& [p, sx] : field_stops) {
            const float d = std::abs(sx - rel);
            if (d < best_d) {
                best_d = d;
                best = p;
            }
        }
        return std::min(best, field.size());
    }

    void click_field(float x, bool dbl, bool shift) {
        focus = Focus::Field;
        type_buf.clear();
        if (dbl) {
            field_anchor = 0;
            caret = field.size();
            field_drag = false;
            return;
        }
        caret = caret_from_x(x);
        if (!shift) field_anchor = caret;
        field_drag = true;
    }

    // ---- messages ------------------------------------------------------------------------
    void set_message(MsgKind kind, const char* key, std::wstring arg = {}) {
        msg = kind;
        msg_key = key;
        msg_arg = std::move(arg);
        dirty = true;
    }
    void clear_message() {
        if (msg == MsgKind::None) return;
        msg = MsgKind::None;
        msg_key = nullptr;
        msg_arg.clear();
        dirty = true;
    }

    // ---- accepting -----------------------------------------------------------------------
    // A typed name or path: absolute, or relative to the current folder.
    [[nodiscard]] std::wstring resolve(std::wstring_view typed) const {
        std::wstring s = expand_env(trim(typed));
        std::ranges::replace(s, L'/', L'\\');
        if (s.empty()) return {};
        if (root_length(s) > 0) return normalize(s);
        if (s.front() == L'\\') {   // "\dir": on the current drive
            const std::size_t r = root_length(cur);
            return normalize((r >= 2 ? cur.substr(0, 2) : std::wstring()) + s);
        }
        return normalize(join(cur, s));
    }

    void accept(bool from_field) {
        switch (req.mode) {
        case BrowseMode::PickFolder:
            accept_folder(from_field);
            return;
        case BrowseMode::SaveFile:
            accept_save();
            return;
        case BrowseMode::OpenFile:
        case BrowseMode::OpenFiles:
            accept_open();
            return;
        }
    }

    void accept_open() {
        if (!field_user) {   // the field only mirrors the selection: take the selection itself
            std::vector<std::wstring> picked;
            for (int i = 0; i < count(); ++i)
                if (selected(i) && !entry(i).folder) picked.push_back(join(cur, entry(i).name));
            if (!picked.empty()) {
                finish(std::move(picked));
                return;
            }
        }
        const std::vector<std::wstring> names = parse_names(field);
        if (names.empty()) {
            if (const int s = single_selected(); s >= 0 && entry(s).folder) {
                enter_folder(s);
                return;
            }
            set_message(MsgKind::Error, "fb_err_select_file");
            return;
        }
        if (names.size() > 1 && req.mode == BrowseMode::OpenFile) {
            set_message(MsgKind::Error, "fb_err_one_file");
            return;
        }
        std::vector<std::wstring> paths;
        paths.reserve(names.size());
        for (const std::wstring& n : names) {
            std::wstring p = resolve(n);
            const DWORD a = attributes_of(p);
            if (a == INVALID_FILE_ATTRIBUTES) {
                set_message(MsgKind::Error, "fb_err_no_file", n);
                return;
            }
            if ((a & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (names.size() == 1) {
                    navigate_typed(std::move(p));
                    return;
                }
                set_message(MsgKind::Error, "fb_err_is_folder", n);
                return;
            }
            paths.push_back(std::move(p));
        }
        finish(std::move(paths));
    }

    void accept_folder(bool from_field) {
        if (field_user) {
            const std::wstring typed = trim(field);
            if (!typed.empty()) {
                std::wstring unq = typed;
                if (unq.size() >= 2 && unq.front() == L'"' && unq.back() == L'"') unq = trim(unq.substr(1, unq.size() - 2));
                std::wstring p = resolve(unq);
                if (!is_dir(p)) {
                    set_message(MsgKind::Error, "fb_err_no_folder", unq);
                    return;
                }
                if (from_field) navigate_typed(std::move(p));   // Enter in the field: go there first
                else finish({std::move(p)});
                return;
            }
        }
        const int s = single_selected();
        finish({s >= 0 && entry(s).folder ? join(cur, entry(s).name) : cur});
    }

    void accept_save() {
        std::wstring typed = trim(field);
        if (typed.size() >= 2 && typed.front() == L'"' && typed.back() == L'"') typed = trim(typed.substr(1, typed.size() - 2));
        if (typed.empty()) {
            set_message(MsgKind::Error, "fb_err_empty_name");
            return;
        }
        typed = expand_env(typed);
        std::ranges::replace(typed, L'/', L'\\');
        if (std::wstring whole = resolve(typed); is_dir(whole)) {   // a folder name or path: open it
            navigate_typed(std::move(whole));
            return;
        }
        std::wstring dir_part, name = typed;
        if (const std::size_t cut = typed.find_last_of(L'\\'); cut != std::wstring::npos) {
            dir_part = typed.substr(0, cut + 1);
            name = typed.substr(cut + 1);
        } else if (const std::size_t r = root_length(typed); r > 0) {
            dir_part = typed.substr(0, r);
            name = typed.substr(r);
        }
        if (name.empty()) {
            set_message(MsgKind::Error, "fb_err_no_folder", typed);
            return;
        }
        const std::wstring dir = dir_part.empty() ? cur : resolve(dir_part);
        if (!is_dir(dir)) {
            set_message(MsgKind::Error, "fb_err_no_folder", dir_part);
            return;
        }
        switch (check_name(name)) {
        case NameProblem::Chars:
            set_message(MsgKind::Error, "fb_err_name_chars");
            return;
        case NameProblem::Ends:
            set_message(MsgKind::Error, "fb_err_name_end");
            return;
        case NameProblem::Reserved:
            set_message(MsgKind::Error, "fb_err_name_reserved", name);
            return;
        case NameProblem::None:
            break;
        }
        if (extension_of(name).empty() && !req.default_ext.empty()) name += L"." + req.default_ext;
        std::wstring path = join(dir, name);
        const DWORD a = attributes_of(path);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            navigate_typed(std::move(path));
            return;
        }
        if (a != INVALID_FILE_ATTRIBUTES && !equal_ci(overwrite_path, path)) {
            overwrite_path = path;   // the next click (now "Overwrite") confirms
            set_message(MsgKind::Warning, "fb_warn_exists", name);
            return;
        }
        finish({std::move(path)});
    }

    // ---- input ---------------------------------------------------------------------------
    void mouse_down(float x, float y, bool dbl, bool ctrl, bool shift) {
        mx = x;
        my = y;
        const Hit h = hit_test(x, y);
        hover = h;
        dirty = true;
        switch (h.kind) {
        case HitKind::Close:
        case HitKind::Cancel:
            finish({});
            return;
        case HitKind::Primary:
            if (primary_enabled()) accept(false);
            return;
        case HitKind::Chip:
            cycle_filter();
            return;
        case HitKind::Field:
            click_field(x, dbl, shift);
            return;
        case HitKind::Up:
            go_up();
            return;
        case HitKind::Refresh:
            refresh();
            return;
        case HitKind::Crumb: {
            const Crumb c = crumbs[static_cast<std::size_t>(h.index)];
            navigate(c.path, c.child);
            return;
        }
        case HitKind::Place:
            navigate(places[static_cast<std::size_t>(h.index)].path, {});
            return;
        case HitKind::Thumb:
            focus = Focus::List;
            thumb_drag = true;
            thumb_grab = y - thumb().top;
            return;
        case HitKind::Track: {
            focus = Focus::List;
            const ThumbGeo th = thumb();
            scroll += (y < th.top ? -1.0f : 1.0f) * std::max(row_h, view_h() - row_h);
            clamp_scroll();
            hover = hit_test(x, y);
            return;
        }
        case HitKind::Row:
            click_row(h.index, dbl, ctrl, shift);
            return;
        case HitKind::List:
            focus = Focus::List;
            if (!ctrl && !shift) clear_selection();
            return;
        default:
            return;
        }
    }

    void mouse_move(float x, float y) {
        mx = x;
        my = y;
        if (thumb_drag) {
            if (const ThumbGeo th = thumb(); th.visible) {
                const float track_h = track.bottom - track.top, h = th.bottom - th.top;
                const float frac = std::clamp((y - thumb_grab - track.top) / std::max(1.0f, track_h - h), 0.0f, 1.0f);
                const float s = frac * (content_h() - view_h());
                if (s != scroll) {
                    scroll = s;
                    dirty = true;
                }
            }
        }
        if (field_drag) {
            if (const std::size_t c = caret_from_x(x); c != caret) {
                caret = c;
                dirty = true;
            }
        }
        if (const Hit h = hit_test(x, y); h != hover) {
            hover = h;
            dirty = true;
        }
    }

    void mouse_up() {
        if (thumb_drag || field_drag) dirty = true;
        thumb_drag = field_drag = false;
    }

    void wheel(float notches) {
        if (!empty_rect(sidebar) && inside(sidebar, mx, my)) {
            side_scroll -= notches * 3.0f * S(32);
            clamp_side_scroll();
        } else {
            scroll -= notches * 3.0f * row_h;
            clamp_scroll();
        }
        hover = hit_test(mx, my);
        dirty = true;
    }

    void key_down(unsigned vk, bool ctrl, bool shift) {
        switch (vk) {
        case VK_ESCAPE:
            finish({});
            return;
        case VK_TAB:
            focus = focus == Focus::Field ? Focus::List : Focus::Field;
            type_buf.clear();
            dirty = true;
            return;
        case VK_F5:
            refresh();
            return;
        default:
            break;
        }
        if (ctrl && vk == 'L') {   // jump to the field with everything selected (type a path)
            focus = Focus::Field;
            field_anchor = 0;
            caret = field.size();
            dirty = true;
            return;
        }
        if (focus == Focus::Field) field_key(vk, ctrl, shift);
        else list_key(vk, ctrl, shift);
    }

    void field_key(unsigned vk, bool ctrl, bool shift) {
        switch (vk) {
        case VK_RETURN:
            accept(true);
            return;
        case VK_LEFT:
            if (caret != field_anchor && !shift) field_move(std::min(caret, field_anchor), false);
            else field_move(ctrl ? word_left(caret) : prev_pos(caret), shift);
            return;
        case VK_RIGHT:
            if (caret != field_anchor && !shift) field_move(std::max(caret, field_anchor), false);
            else field_move(ctrl ? word_right(caret) : next_pos(caret), shift);
            return;
        case VK_HOME:
            field_move(0, shift);
            return;
        case VK_END:
            field_move(field.size(), shift);
            return;
        case VK_BACK:
            if (caret != field_anchor) replace_selection({});
            else if (field.empty()) go_up();
            else if (caret > 0) {
                field_anchor = ctrl ? word_left(caret) : prev_pos(caret);
                replace_selection({});
            }
            return;
        case VK_DELETE:
            if (caret != field_anchor) replace_selection({});
            else if (caret < field.size()) {
                field_anchor = ctrl ? word_right(caret) : next_pos(caret);
                replace_selection({});
            }
            return;
        case VK_UP:
        case VK_DOWN:
        case VK_PRIOR:
        case VK_NEXT:
            focus = Focus::List;
            list_key(vk, ctrl, shift);
            return;
        default:
            break;
        }
        if (!ctrl) return;
        switch (vk) {
        case 'A':
            field_anchor = 0;
            caret = field.size();
            dirty = true;
            return;
        case 'C':
            copy_selection();
            return;
        case 'X':
            if (copy_selection()) replace_selection({});
            return;
        case 'V':
            paste();
            return;
        default:
            return;
        }
    }

    void list_key(unsigned vk, bool ctrl, bool shift) {
        const int page = std::max(1, static_cast<int>(view_h() / row_h) - 1);
        switch (vk) {
        case VK_UP:
            move_cursor(cursor < 0 ? 0 : cursor - 1, shift);
            return;
        case VK_DOWN:
            move_cursor(cursor + 1, shift);
            return;
        case VK_PRIOR:
            move_cursor(std::max(cursor, 0) - page, shift);
            return;
        case VK_NEXT:
            move_cursor(std::max(cursor, 0) + page, shift);
            return;
        case VK_HOME:
            move_cursor(0, shift);
            return;
        case VK_END:
            move_cursor(count() - 1, shift);
            return;
        case VK_RETURN:
            list_enter();
            return;
        case VK_BACK:
            go_up();
            return;
        default:
            break;
        }
        if (ctrl && vk == 'A' && multi()) select_all_files();
    }

    void character(wchar_t c) {
        if (c < 0x20 || c == 0x7F) return;   // control keys arrive through key()
        if (focus == Focus::Field) {
            replace_selection(std::wstring_view(&c, 1));
            return;
        }
        type_ahead(c);
    }

    // ---- drawing -------------------------------------------------------------------------
    [[nodiscard]] const wchar_t* mode_glyph() const noexcept {
        switch (req.mode) {
        case BrowseMode::SaveFile: return glyph::kSave;
        case BrowseMode::PickFolder: return glyph::kFolder;
        case BrowseMode::OpenFile:
        case BrowseMode::OpenFiles: return glyph::kFolderOpen;
        }
        return glyph::kFolderOpen;
    }
    [[nodiscard]] const char* placeholder_key() const noexcept {
        switch (req.mode) {
        case BrowseMode::SaveFile: return "fb_hint_save";
        case BrowseMode::PickFolder: return "fb_hint_folder";
        case BrowseMode::OpenFile:
        case BrowseMode::OpenFiles: return "fb_hint_file";
        }
        return "fb_hint_file";
    }
    [[nodiscard]] const std::wstring& primary_label() const {
        if (!overwrite_path.empty()) return L("fb_overwrite");
        switch (req.mode) {
        case BrowseMode::OpenFile: return L("fb_open");
        case BrowseMode::OpenFiles: return L("fb_add");
        case BrowseMode::PickFolder: return L("fb_select_folder");
        case BrowseMode::SaveFile: return L("fb_save");
        }
        return L("fb_open");
    }
    [[nodiscard]] static std::wstring filter_label(const BrowseFilter& f) {
        if (const std::wstring& s = L(f.label_key); !s.empty()) return s;
        if (f.exts.empty()) return L("fb_all_files");
        std::wstring out;
        for (const std::wstring& e : f.exts) {
            if (!out.empty()) out += L"; ";
            out += L"*." + e;
        }
        return out;
    }
    [[nodiscard]] std::wstring read_error_text() const {
        switch (read_error) {
        case ReadError::Access: return L("fb_err_access");
        case ReadError::NotReady: return L("fb_err_not_ready");
        case ReadError::NotFound: return L("fb_err_not_found");
        case ReadError::Other:
        case ReadError::None: break;
        }
        return Lf("fb_err_read", read_code);
    }

    void draw(const Theme& t) {
        if (!open || !t.rt || !t.brush) return;
        if (empty_rect(card)) {   // layout() not called yet: use the whole render target
            const D2D1_SIZE_F sz = t.rt->GetSize();
            client = R(0, 0, sz.width, sz.height);
            scale = t.scale > 0 ? t.scale : 1.0f;
            do_layout();
            if (empty_rect(card)) return;
        }
        Painter p(t, scale);
        p.fill(client, kBlack, 0.55f);
        p.fill_round(R(card.left + S(4), card.top + S(8), card.right + S(4), card.bottom + S(10)), S(18), kBlack, 0.35f);
        p.fill_round(card, S(16), t.panel, 1.0f);
        p.fill_gradient(R(card.left + S(40), card.top, card.right - S(40), card.top + S(2)), S(1),
                        {card.left + S(40), card.top}, {card.right - S(40), card.top}, 0.85f);
        p.stroke_round(card, S(16), t.accent, 0.30f, 1.2f);
        draw_header(p, t);
        if (!empty_rect(sidebar)) draw_sidebar(p, t);
        draw_path_bar(p, t);
        draw_list(p, t);
        draw_footer(p, t);
    }

    void draw_header(Painter& p, const Theme& t) {
        const float cy = (header.top + header.bottom) / 2;
        const D2D1_RECT_F tile = R(card.left + S(18), cy - S(15), card.left + S(48), cy + S(15));
        p.fill_gradient(tile, S(9), {tile.left, tile.top}, {tile.right, tile.bottom});
        p.icon(mode_glyph(), tile, kWhite);
        p.text(req.title, t.title, R(tile.right + S(12), header.top, close_btn.left - S(10), header.bottom), t.text, 1.0f,
               DWRITE_TEXT_ALIGNMENT_LEADING, true);
        const bool hot = hover.kind == HitKind::Close;
        if (hot) p.fill_round(close_btn, S(8), t.red, 0.85f);
        p.icon(glyph::kClose, close_btn, hot ? kWhite : t.subtext);
        p.line({card.left + S(16), header.bottom}, {card.right - S(16), header.bottom}, kWhite, 0.06f, 1.0f);
    }

    void draw_sidebar(Painter& p, const Theme& t) {
        p.fill_round(sidebar, S(12), t.field, 0.45f);
        p.stroke_round(sidebar, S(12), kWhite, 0.05f, 1.0f);
        const ClipScope clip(t.rt, R(sidebar.left + 1, sidebar.top + 1, sidebar.right - 1, sidebar.bottom - 1));
        const std::vector<SideItem> items = side_items();
        // More places than fit (wheel scrolls them): a slim position indicator on the right edge.
        if (const float view = sidebar.bottom - sidebar.top, content = side_content(items); content > view + 0.5f) {
            const float h = std::max(S(24), view * view / content);
            const float top = sidebar.top + (side_scroll / (content - view)) * (view - h);
            p.fill_round(R(sidebar.right - S(5), top + S(3), sidebar.right - S(2), top + h - S(3)), S(1.5f), kWhite, 0.18f);
        }
        for (const SideItem& it : items) {
            if (it.bottom < sidebar.top || it.top > sidebar.bottom) continue;
            if (it.header) {
                p.text(L(it.header), t.note, R(sidebar.left + S(14), it.top + S(4), sidebar.right - S(10), it.bottom),
                       t.dim, 1.0f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
                continue;
            }
            const Place& pl = places[static_cast<std::size_t>(it.place)];
            const D2D1_RECT_F r = R(sidebar.left + S(6), it.top + S(1), sidebar.right - S(6), it.bottom - S(1));
            const bool active = equal_ci(pl.path, cur);
            const bool hot = hover.kind == HitKind::Place && hover.index == it.place;
            if (active) {
                p.fill_round(r, S(8), t.accent, 0.14f);
                p.fill_gradient(R(r.left, r.top + S(7), r.left + S(3), r.bottom - S(7)), S(1.5f), {r.left, r.top},
                                {r.left, r.bottom});
            } else if (hot) {
                p.fill_round(r, S(8), kWhite, 0.06f);
            }
            const std::uint32_t ic = active                         ? t.accent
                                     : pl.kind == PlaceKind::Known  ? t.folder
                                     : pl.kind == PlaceKind::Recent ? t.accent2
                                                                    : t.subtext;
            p.icon(pl.icon, R(r.left + S(6), r.top, r.left + S(32), r.bottom), ic);
            p.text(place_label(pl), t.ui, R(r.left + S(38), r.top, r.right - S(8), r.bottom),
                   active || hot ? t.text : t.subtext, 1.0f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        }
    }

    void layout_crumbs(Painter& p, const Theme& t) {
        crumbs.clear();
        const std::vector<Segment> segs = split_segments(cur);
        if (segs.empty()) return;
        const float x0 = crumb_bar.left + S(36), x1 = crumb_bar.right - S(8);
        const float top = crumb_bar.top + S(4), bottom = crumb_bar.bottom - S(4);
        const float sep = S(18), padx = S(16);
        const std::size_t n = segs.size();
        std::vector<float> w(n);
        for (std::size_t i = 0; i < n; ++i) w[i] = p.measure(t.ui, segs[i].label) + padx;
        const float more_w = p.measure(t.ui, L"\u2026") + padx;
        auto width_from = [&](std::size_t k) {
            float s = 0;
            for (std::size_t j = k; j < n; ++j) s += w[j] + (j + 1 < n ? sep : 0.0f);
            return s;
        };
        std::size_t first = 0;   // leading crumbs that do not fit collapse into "…"
        while (first + 1 < n && width_from(first) + (first > 0 ? more_w + sep : 0.0f) > x1 - x0) ++first;
        float x = x0;
        if (first > 0) {
            crumbs.push_back({R(x, top, x + more_w, bottom), L"\u2026", segs[first - 1].path, segs[first].label, true});
            x += more_w + sep;
        }
        for (std::size_t i = first; i < n; ++i) {
            const float right = std::min(x + w[i], x1);
            if (right - x < S(24)) break;
            crumbs.push_back({R(x, top, right, bottom), segs[i].label, segs[i].path,
                              i + 1 < n ? segs[i + 1].label : std::wstring(), false});
            x = right + sep;
        }
    }

    void draw_path_bar(Painter& p, const Theme& t) {
        auto square = [&](const D2D1_RECT_F& r, const wchar_t* g, HitKind k, bool enabled) {
            const bool hot = enabled && hover.kind == k;
            p.fill_round(r, S(9), kWhite, hot ? 0.10f : 0.04f);
            p.stroke_round(r, S(9), kWhite, 0.06f, 1.0f);
            p.icon(g, r, !enabled ? t.dim : hot ? t.text : t.subtext);
        };
        square(up_btn, glyph::kUp, HitKind::Up, can_go_up());
        square(refresh_btn, glyph::kRefresh, HitKind::Refresh, true);
        p.fill_round(crumb_bar, S(10), t.field, 0.9f);
        p.stroke_round(crumb_bar, S(10), kWhite, 0.07f, 1.0f);
        p.icon(glyph::kFolder, R(crumb_bar.left + S(8), crumb_bar.top, crumb_bar.left + S(32), crumb_bar.bottom), t.folder);
        layout_crumbs(p, t);
        for (std::size_t i = 0; i < crumbs.size(); ++i) {
            const Crumb& c = crumbs[i];
            const bool hot = hover.kind == HitKind::Crumb && hover.index == static_cast<int>(i);
            const bool last = i + 1 == crumbs.size();
            if (hot) p.fill_round(c.rect, S(6), kWhite, 0.08f);
            p.text(c.label, t.ui, R(c.rect.left + S(8), c.rect.top, c.rect.right - S(8), c.rect.bottom),
                   last || hot ? t.text : t.subtext, 1.0f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
            if (!last) p.chevron(c.rect.right + S(9), (c.rect.top + c.rect.bottom) / 2, S(4), false, t.dim, 1.0f);
        }
    }

    void draw_list_notice(Painter& p, const Theme& t, const wchar_t* g, std::uint32_t gc, std::wstring_view line1,
                          std::wstring_view line2) {
        const float cy = (rows_rect.top + rows_rect.bottom) / 2;
        p.icon(g, R(rows_rect.left, cy - S(46), rows_rect.right, cy - S(18)), gc);
        p.text(line1, t.ui, R(rows_rect.left + S(20), cy - S(14), rows_rect.right - S(20), cy + S(12)), t.text, 0.92f,
               DWRITE_TEXT_ALIGNMENT_CENTER, true);
        p.text(line2, t.note, R(rows_rect.left + S(20), cy + S(12), rows_rect.right - S(20), cy + S(34)), t.subtext, 1.0f,
               DWRITE_TEXT_ALIGNMENT_CENTER, true);
    }

    void draw_list(Painter& p, const Theme& t) {
        const bool list_focus = focus == Focus::List;
        p.fill_round(list_panel, S(12), t.field, 0.55f);
        p.stroke_round(list_panel, S(12), list_focus ? t.accent : kWhite, list_focus ? 0.22f : 0.06f, 1.0f);
        const Columns c = columns();
        p.text(L("fb_col_name"), t.note, R(c.name_l, list_head.top, c.name_r, list_head.bottom), t.dim, 1.0f,
               DWRITE_TEXT_ALIGNMENT_LEADING, true);
        if (c.size)
            p.text(L("fb_col_size"), t.note, R(c.size_l, list_head.top, c.size_r, list_head.bottom), t.dim, 1.0f,
                   DWRITE_TEXT_ALIGNMENT_TRAILING, true);
        if (c.mod)
            p.text(L("fb_col_modified"), t.note, R(c.mod_l, list_head.top, c.mod_r, list_head.bottom), t.dim, 1.0f,
                   DWRITE_TEXT_ALIGNMENT_LEADING, true);
        p.line({list_panel.left + S(10), list_head.bottom}, {list_panel.right - S(10), list_head.bottom}, kWhite, 0.06f,
               1.0f);

        if (read_error != ReadError::None) {
            draw_list_notice(p, t, glyph::kError, t.red, read_error_text(), L("fb_err_hint"));
            return;
        }
        if (rows.empty()) {
            const char* notice_key = filtered_out > 0                         ? "fb_no_match"
                                     : req.mode == BrowseMode::PickFolder ? "fb_no_folders"
                                                                            : "fb_empty";
            draw_list_notice(p, t, glyph::kFolder, t.dim, L(notice_key), {});
            return;
        }
        clamp_scroll();
        {
            const ClipScope clip(t.rt, rows_rect);
            const int first = static_cast<int>(scroll / row_h);
            const int last = std::min(count() - 1, static_cast<int>((scroll + view_h()) / row_h));
            for (int i = first; i <= last; ++i) {
                const Entry& e = entry(i);
                const float y = rows_rect.top - scroll + static_cast<float>(i) * row_h;
                const D2D1_RECT_F rr = R(rows_rect.left + S(4), y + S(1), rows_rect.right - S(2), y + row_h - S(1));
                const bool on = selected(i);
                const bool hot = hover.kind == HitKind::Row && hover.index == i;
                if (on) {
                    p.fill_round(rr, S(7), t.accent, 0.16f);
                    p.fill_gradient(R(rr.left, y + S(8), rr.left + S(3), y + row_h - S(8)), S(1.5f), {rr.left, y},
                                    {rr.left, y + row_h});
                } else if (hot) {
                    p.fill_round(rr, S(7), kWhite, 0.05f);
                }
                if (list_focus && i == cursor && (multi() || !on)) p.stroke_round(rr, S(7), t.accent, 0.38f, 1.0f);
                const std::uint32_t ic = e.folder ? t.folder : e.archive ? t.accent : t.subtext;
                p.icon(e.folder ? glyph::kFolder : e.archive ? glyph::kPackage : glyph::kFile,
                       R(c.icon_l, y, c.icon_l + S(24), y + row_h), ic, e.folder || e.archive ? 1.0f : 0.85f);
                p.text(e.name, t.ui, R(c.name_l, y, c.name_r, y + row_h), t.text, on ? 1.0f : 0.94f,
                       DWRITE_TEXT_ALIGNMENT_LEADING, true);
                if (!e.folder && c.size)
                    p.text(format_size(e.size), t.ui, R(c.size_l, y, c.size_r, y + row_h), t.subtext, 1.0f,
                           DWRITE_TEXT_ALIGNMENT_TRAILING);
                if (c.mod) p.text(format_time(e.mtime), t.ui, R(c.mod_l, y, c.mod_r, y + row_h), t.dim);
            }
        }
        if (const ThumbGeo th = thumb(); th.visible) {
            p.fill_round(track, S(4), kWhite, 0.05f);
            const bool hot = hover.kind == HitKind::Thumb;
            p.fill_round(R(track.left, th.top, track.right, th.bottom), S(4), t.accent,
                         thumb_drag ? 0.46f : hot ? 0.36f : 0.26f);
        }
    }

    void update_field_stops(IDWriteTextLayout* tl) {
        field_stops.clear();
        field_stops_text = field;
        UINT32 n = 0;
        const HRESULT probe = tl->GetClusterMetrics(nullptr, 0, &n);   // E_NOT_SUFFICIENT_BUFFER + the count
        std::vector<DWRITE_CLUSTER_METRICS> cm(n);
        field_stops.emplace_back(0, 0.0f);
        if (n == 0 || (FAILED(probe) && probe != E_NOT_SUFFICIENT_BUFFER) ||
            FAILED(tl->GetClusterMetrics(cm.data(), n, &n)))
            return;
        field_stops.reserve(cm.size() + 1);
        std::size_t pos = 0;
        float x = 0;
        for (const DWRITE_CLUSTER_METRICS& m : cm) {
            pos += m.length;
            x += m.width;
            field_stops.emplace_back(pos, x);
        }
    }

    void draw_field(Painter& p, const Theme& t) {
        const bool focused = focus == Focus::Field;
        const bool hot = hover.kind == HitKind::Field;
        p.fill_round(field_rect, S(10), t.field, 0.9f);
        p.stroke_round(field_rect, S(10), focused ? t.accent : kWhite, focused ? 0.55f : hot ? 0.14f : 0.07f,
                       focused ? 1.2f : 1.0f);
        p.icon(req.mode == BrowseMode::SaveFile     ? glyph::kSave
               : req.mode == BrowseMode::PickFolder ? glyph::kFolder
                                                    : glyph::kFile,
               R(field_rect.left + S(8), field_rect.top, field_rect.left + S(34), field_rect.bottom),
               focused ? t.accent : t.subtext);
        const float tx0 = field_rect.left + S(40), tx1 = field_rect.right - S(12);
        if (tx1 <= tx0) return;
        field_origin = tx0;
        const float caret_top = field_rect.top + S(9), caret_bottom = field_rect.bottom - S(9);
        if (field.empty()) {
            field_stops.clear();
            field_stops_text.clear();
            field_scroll = 0;
            p.text(L(placeholder_key()), t.ui, R(tx0, field_rect.top, tx1, field_rect.bottom), t.dim, 1.0f,
                   DWRITE_TEXT_ALIGNMENT_LEADING, true);
            if (focused) p.fill(R(tx0, caret_top, tx0 + S(1.5f), caret_bottom), t.accent, 1.0f);
            return;
        }
        ComPtr<IDWriteTextLayout> tl;
        if (!t.dw || !t.ui ||
            FAILED(t.dw->CreateTextLayout(field.c_str(), static_cast<UINT32>(field.size()), t.ui, 100000.0f,
                                          field_rect.bottom - field_rect.top, &tl))) {
            field_stops.clear();
            p.text(field, t.ui, R(tx0, field_rect.top, tx1, field_rect.bottom), t.text);
            return;
        }
        tl->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        tl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        tl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        update_field_stops(tl.Get());
        const float caret_x = stop_x(caret), anchor_x = stop_x(field_anchor);
        const float width = field_stops.back().second, view = tx1 - tx0;
        if (width <= view) {
            field_scroll = 0;
        } else {   // keep the caret in view
            if (caret_x - field_scroll > view - S(2)) field_scroll = caret_x - view + S(2);
            if (caret_x - field_scroll < 0) field_scroll = caret_x;
            field_scroll = std::clamp(field_scroll, 0.0f, width - view + S(2));
        }
        field_origin = tx0 - field_scroll;
        const ClipScope clip(t.rt, R(tx0 - S(2), field_rect.top + S(2), tx1 + S(2), field_rect.bottom - S(2)));
        if (caret != field_anchor) {
            const float a = std::min(caret_x, anchor_x), b = std::max(caret_x, anchor_x);
            p.fill(R(field_origin + a, caret_top - S(1), field_origin + b, caret_bottom + S(1)), t.accent,
                   focused ? 0.32f : 0.16f);
        }
        p.layout_text({field_origin, field_rect.top}, tl.Get(), t.text, 1.0f);
        if (focused)
            p.fill(R(field_origin + caret_x - S(0.75f), caret_top, field_origin + caret_x + S(0.75f), caret_bottom),
                   t.accent, 1.0f);
    }

    void draw_footer(Painter& p, const Theme& t) {
        p.line({card.left + S(16), footer.top}, {card.right - S(16), footer.top}, kWhite, 0.06f, 1.0f);
        draw_field(p, t);
        if (const BrowseFilter* f = active_filter()) {
            const bool cyc = can_cycle_filter(), hot = cyc && hover.kind == HitKind::Chip;
            p.fill_round(chip, S(10), kWhite, hot ? 0.09f : 0.04f);
            p.stroke_round(chip, S(10), hot ? t.accent2 : kWhite, hot ? 0.45f : 0.08f, 1.0f);
            p.icon(glyph::kFilter, R(chip.left + S(8), chip.top, chip.left + S(32), chip.bottom), t.accent2);
            p.text(filter_label(*f), t.semibold, R(chip.left + S(36), chip.top, chip.right - (cyc ? S(30) : S(10)), chip.bottom),
                   t.text, 1.0f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
            if (cyc) p.chevron(chip.right - S(17), (chip.top + chip.bottom) / 2, S(4), true, t.subtext, 1.0f);
        }

        // Message line: an error / overwrite warning in red, else counts of the folder.
        if (msg != MsgKind::None && msg_key) {
            p.icon(msg == MsgKind::Error ? glyph::kError : glyph::kWarning,
                   R(msg_rect.left, msg_rect.top, msg_rect.left + S(22), msg_rect.bottom), t.red);
            p.text(Lf(msg_key, msg_arg), t.ui, R(msg_rect.left + S(28), msg_rect.top, msg_rect.right, msg_rect.bottom),
                   t.red, 1.0f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        } else if (read_error == ReadError::None) {
            const std::size_t picked = multi() ? selected_file_count() : 0;
            std::wstring info;
            if (picked > 1) info = Lf("fb_selected", picked);
            else {
                info = req.mode == BrowseMode::PickFolder ? Lf("fb_count_folders", folder_count)
                                                          : Lf("fb_count", folder_count, file_count);
                if (filtered_out > 0) info += L"  \u00B7  " + Lf("fb_hidden", filtered_out);
            }
            p.text(info, t.note, R(msg_rect.left + S(2), msg_rect.top, msg_rect.right, msg_rect.bottom),
                   picked > 1 ? t.accent : t.dim, 1.0f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        }

        {
            const bool hot = hover.kind == HitKind::Cancel;
            p.fill_round(cancel_btn, S(9), kWhite, hot ? 0.11f : 0.07f);
            p.text(L("fb_cancel"), t.semibold, cancel_btn, hot ? t.text : t.subtext, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER,
                   true);
        }
        {
            const bool enabled = primary_enabled(), hot = enabled && hover.kind == HitKind::Primary;
            const std::uint32_t c = overwrite_path.empty() ? t.accent : t.red;
            if (enabled) {
                p.fill_round(primary_btn, S(9), c, hot ? 0.30f : 0.22f);
                p.stroke_round(primary_btn, S(9), c, hot ? 0.75f : 0.5f, 1.0f);
            } else {
                p.fill_round(primary_btn, S(9), kWhite, 0.04f);
                p.stroke_round(primary_btn, S(9), kWhite, 0.06f, 1.0f);
            }
            p.text(primary_label(), t.semibold, primary_btn, enabled ? t.text : t.dim, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER,
                   true);
        }
    }
};

// =============================================================================================
FileBrowser::FileBrowser() : impl_(std::make_unique<Impl>()) {}

// A picker still open when its owner goes away is dropped without calling back: the owner (and
// whatever the callback captured) is being destroyed.
FileBrowser::~FileBrowser() = default;

void FileBrowser::open(BrowseRequest request, Done done) {
    if (impl_->open) {
        impl_->finish({});       // the earlier request is reported as cancelled
        if (impl_->open) {       // ...and its callback opened a picker of its own: that one stays
            if (done) done({});
            return;
        }
    }
    impl_->start(std::move(request), std::move(done));
}

void FileBrowser::cancel() {
    if (impl_->open) impl_->finish({});
}

bool FileBrowser::is_open() const noexcept { return impl_->open; }

void FileBrowser::layout(D2D1_RECT_F client, float scale) {
    const float s = scale > 0 ? scale : 1.0f;
    const D2D1_RECT_F& c = impl_->client;
    if (c.left != client.left || c.top != client.top || c.right != client.right || c.bottom != client.bottom ||
        impl_->scale != s)
        impl_->dirty = impl_->dirty || impl_->open;
    impl_->client = client;
    impl_->scale = s;
    impl_->do_layout();
}

void FileBrowser::draw(const Theme& theme) { impl_->draw(theme); }

bool FileBrowser::on_mouse_move(int x, int y) {
    if (!impl_->open) return false;
    impl_->mouse_move(static_cast<float>(x), static_cast<float>(y));
    return true;
}

bool FileBrowser::on_mouse_down(int x, int y, bool double_click, bool ctrl, bool shift) {
    if (!impl_->open) return false;
    impl_->mouse_down(static_cast<float>(x), static_cast<float>(y), double_click, ctrl, shift);
    return true;
}

bool FileBrowser::on_mouse_up(int, int) {
    if (!impl_->open) return false;
    impl_->mouse_up();
    return true;
}

bool FileBrowser::on_wheel(float notches) {
    if (!impl_->open) return false;
    impl_->wheel(notches);
    return true;
}

bool FileBrowser::on_key(unsigned vk, bool ctrl, bool shift) {
    if (!impl_->open) return false;
    impl_->key_down(vk, ctrl, shift);
    return true;
}

bool FileBrowser::on_char(wchar_t c) {
    if (!impl_->open) return false;
    impl_->character(c);
    return true;
}

bool FileBrowser::needs_redraw() noexcept { return std::exchange(impl_->dirty, false); }

bool FileBrowser::hand_cursor() const noexcept { return impl_->open && impl_->hand(); }

} // namespace pk2::ui
