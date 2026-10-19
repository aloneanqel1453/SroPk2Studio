// SkillEffect Studio - core/Text.cpp
#include "Text.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <cwctype>
#include <mutex>

namespace se {

static std::wstring LastErr(const wchar_t* what, const std::wstring& path) {
    const DWORD e = GetLastError();
    wchar_t buf[512] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, buf,
                   static_cast<DWORD>(std::size(buf)), nullptr);
    std::wstring m = buf;
    while (!m.empty() && (m.back() == L'\n' || m.back() == L'\r' || m.back() == L' ')) m.pop_back();
    return std::wstring(what) + L" " + path + L" (" + std::to_wstring(e) + L": " + m + L")";
}

bool ReadFileBytes(const std::wstring& path, Bytes& out, std::wstring& err) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = LastErr(L"open", path); return false; }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > (1LL << 31)) {
        err = L"size " + path;
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 24)), got = 0;
        if (!ReadFile(h, out.data() + done, chunk, &got, nullptr) || got == 0) {
            err = LastErr(L"read", path);
            CloseHandle(h);
            return false;
        }
        done += got;
    }
    CloseHandle(h);
    return true;
}

bool WriteFileAtomic(const std::wstring& path, const Bytes& data, std::wstring& err) {
    if (PendingEncLosses() && FindEncLossIn(data, &err) >= 0) {  // v5 M3: a lossy CP949 row is never written
        err += L"  (" + path + L")";
        return false;
    }
    const std::wstring tmp = path + L".tmp~" + std::to_wstring(GetCurrentProcessId());
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = LastErr(L"create", tmp); return false; }
    size_t done = 0;
    bool ok = true;
    while (done < data.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 24)), put = 0;
        if (!WriteFile(h, data.data() + done, chunk, &put, nullptr) || put != chunk) { ok = false; break; }
        done += put;
    }
    if (ok) ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    if (!ok) {
        err = LastErr(L"write", tmp);
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = LastErr(L"move", path);
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool FileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool DirExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool ValidUtf8(const Bytes& b, size_t start) {
    size_t i = start;
    while (i < b.size()) {
        const unsigned c = b[i];
        size_t n = 0;
        if (c < 0x80) { ++i; continue; }
        if ((c & 0xE0) == 0xC0) n = 1;
        else if ((c & 0xF0) == 0xE0) n = 2;
        else if ((c & 0xF8) == 0xF0) n = 3;
        else return false;
        for (size_t k = 1; k <= n; ++k) {
            if (i + k >= b.size() || (b[i + k] & 0xC0) != 0x80) return false;
        }
        i += n + 1;
    }
    return true;
}

std::wstring FromAnsi(std::string_view s, unsigned codePage) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(codePage, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(codePage, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring DecodeText(const Bytes& b, TextEnc* encOut, bool* hadBom) {
    auto setEnc = [&](TextEnc e, bool bom) {
        if (encOut) *encOut = e;
        if (hadBom) *hadBom = bom;
    };
    if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
        setEnc(TextEnc::Utf16LE, true);
        std::wstring w((b.size() - 2) / 2, L'\0');
        if (!w.empty()) memcpy(w.data(), b.data() + 2, w.size() * 2);
        return w;
    }
    if (b.size() >= 2 && b[0] == 0xFE && b[1] == 0xFF) {
        setEnc(TextEnc::Utf16BE, true);
        std::wstring w((b.size() - 2) / 2, L'\0');
        for (size_t i = 0; i < w.size(); ++i) w[i] = static_cast<wchar_t>((b[2 + 2 * i] << 8) | b[3 + 2 * i]);
        return w;
    }
    if (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
        setEnc(TextEnc::Utf8, true);
        return FromUtf8(std::string_view(reinterpret_cast<const char*>(b.data()) + 3, b.size() - 3));
    }
    // UTF-16 without BOM: many zero bytes at odd (LE) or even (BE) positions
    if (b.size() >= 4 && b.size() % 2 == 0) {
        size_t zOdd = 0, zEven = 0, n = std::min<size_t>(b.size(), 8192);
        for (size_t i = 0; i < n; ++i) {
            if (b[i] == 0) (i & 1 ? zOdd : zEven)++;
        }
        if (zOdd * 4 > n / 2 && zEven * 10 < zOdd) {
            setEnc(TextEnc::Utf16LE, false);
            std::wstring w(b.size() / 2, L'\0');
            memcpy(w.data(), b.data(), w.size() * 2);
            return w;
        }
        if (zEven * 4 > n / 2 && zOdd * 10 < zEven) {
            setEnc(TextEnc::Utf16BE, false);
            std::wstring w(b.size() / 2, L'\0');
            for (size_t i = 0; i < w.size(); ++i) w[i] = static_cast<wchar_t>((b[2 * i] << 8) | b[2 * i + 1]);
            return w;
        }
    }
    const std::string_view sv(reinterpret_cast<const char*>(b.data()), b.size());
    if (ValidUtf8(b, 0)) {
        setEnc(TextEnc::Utf8, false);
        return FromUtf8(sv);
    }
    setEnc(TextEnc::Cp949, false);
    return FromAnsi(sv, 949);
}

const wchar_t* EncName(TextEnc e) {
    switch (e) {
        case TextEnc::Utf16LE: return L"UTF-16LE";
        case TextEnc::Utf16BE: return L"UTF-16BE";
        case TextEnc::Utf8: return L"UTF-8";
        case TextEnc::Cp949: return L"CP949";
    }
    return L"?";
}

std::string ToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(std::string_view s) { return FromAnsi(s, CP_UTF8); }

// ------------------------------------------------------------------------------------------------ v5 M3 NarrowExact
namespace {

bool AsciiSafeCp(unsigned cp) {
    return cp == 949 || cp == 932 || cp == 936 || cp == 950 || cp == 1361 || (cp >= 1250 && cp <= 1258) || cp == CP_UTF8 ||
           cp == 20127 || cp == 437 || cp == 850 || cp == 857;
}

// one conversion attempt, no position search
bool NarrowTry(unsigned cp, std::wstring_view w, std::string& out) {
    out.clear();
    if (w.empty()) return true;
    if (w.size() > 0x3FFFFFFF) return false;
    const bool utf8 = cp == CP_UTF8;
    const DWORD flags = utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL used = FALSE;
    LPBOOL pUsed = utf8 ? nullptr : &used;
    const int wn = static_cast<int>(w.size());
    const int n = WideCharToMultiByte(cp, flags, w.data(), wn, nullptr, 0, nullptr, pUsed);
    if (n <= 0 || used) return false;
    out.resize(static_cast<size_t>(n));
    used = FALSE;
    if (WideCharToMultiByte(cp, flags, w.data(), wn, out.data(), n, nullptr, pUsed) != n || used) {
        out.clear();
        return false;
    }
    const int m = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, out.data(), n, nullptr, 0);
    if (m != wn) {
        out.clear();
        return false;
    }
    std::wstring back(static_cast<size_t>(m), L'\0');
    if (MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, out.data(), n, back.data(), m) != m || std::wstring_view(back) != w) {
        out.clear();
        return false;
    }
    return true;
}

std::mutex g_lossMx;
std::vector<EncLoss> g_losses;

}  // namespace

bool NarrowExact(unsigned cp, std::wstring_view w, std::string& out, size_t* badPos) {
    if (badPos) *badPos = std::wstring_view::npos;
    bool ascii = AsciiSafeCp(cp);
    for (size_t i = 0; ascii && i < w.size(); ++i) ascii = w[i] < 0x80;
    if (ascii) {  // ASCII protection: byte for byte, no API in the way
        out.resize(w.size());
        for (size_t i = 0; i < w.size(); ++i) out[i] = static_cast<char>(w[i]);
        return true;
    }
    if (NarrowTry(cp, w, out)) return true;
    out.clear();
    if (badPos) {
        std::string tmp;
        size_t pos = 0;
        for (size_t i = 0; i < w.size();) {
            size_t len = 1;
            if (w[i] >= 0xD800 && w[i] <= 0xDBFF && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) len = 2;
            if (w[i] >= 0x80 && !NarrowTry(cp, w.substr(i, len), tmp)) {
                pos = i;
                break;
            }
            i += len;
        }
        *badPos = pos;
    }
    return false;
}

bool IsExactIn(unsigned cp, std::wstring_view text, size_t* badPos) {
    std::string tmp;
    return NarrowExact(cp, text, tmp, badPos);
}

std::wstring EncLossText(std::wstring_view text, size_t pos, bool english, unsigned cp) {
    std::wstring ch;
    unsigned code = 0;
    if (pos < text.size()) {
        ch.assign(1, text[pos]);
        code = text[pos];
        if (code >= 0xD800 && code <= 0xDBFF && pos + 1 < text.size()) {
            ch.push_back(text[pos + 1]);
            code = 0x10000u + ((code - 0xD800u) << 10) + (static_cast<unsigned>(text[pos + 1]) - 0xDC00u);
        }
    }
    const std::wstring cpName = cp == 949 ? std::wstring(L"CP949") : cp == CP_UTF8 ? std::wstring(L"UTF-8") : L"CP" + std::to_wstring(cp);
    std::wstring t(text.substr(0, std::min<size_t>(text.size(), 200)));
    for (auto& c : t)
        if (c == L'\t') c = L' ';
    if (text.size() > 200) t += L"...";
    return english ? Fmt(L"'%s' (U+%04X, character %zu) is not in %s, it cannot be written without loss: %s", ch.c_str(), code, pos + 1,
                         cpName.c_str(), t.c_str())
                   : Fmt(L"'%s' (U+%04X, %zu. karakter) %s'da yok, kayıpsız yazılamaz: %s", ch.c_str(), code, pos + 1, cpName.c_str(),
                         t.c_str());
}

void NoteEncLoss(EncLoss e) {
    std::lock_guard<std::mutex> lk(g_lossMx);
    if (g_losses.size() >= 256) g_losses.erase(g_losses.begin());
    g_losses.push_back(std::move(e));
}

std::vector<EncLoss> TakeEncLosses() {
    std::lock_guard<std::mutex> lk(g_lossMx);
    std::vector<EncLoss> v;
    v.swap(g_losses);
    return v;
}

size_t PendingEncLosses() {
    std::lock_guard<std::mutex> lk(g_lossMx);
    return g_losses.size();
}

int FindEncLossIn(const Bytes& data, std::wstring* msgTr, std::wstring* msgEn) {
    std::lock_guard<std::mutex> lk(g_lossMx);
    for (size_t i = 0; i < g_losses.size(); ++i) {
        const auto& l = g_losses[i];
        if (l.lossy.size() < 8 || l.lossy.size() > data.size()) continue;
        if (std::search(data.begin(), data.end(), l.lossy.begin(), l.lossy.end()) == data.end()) continue;
        const std::wstring cell = l.cell >= 0 ? Fmt(L" [hücre %d]", l.cell) : std::wstring();
        const std::wstring cellEn = l.cell >= 0 ? Fmt(L" [cell %d]", l.cell) : std::wstring();
        if (msgTr) *msgTr = L"ENC2 " + l.where + cell + L": " + EncLossText(l.text, l.pos, false, l.cp);
        if (msgEn) *msgEn = L"ENC2 " + l.where + cellEn + L": " + EncLossText(l.text, l.pos, true, l.cp);
        return static_cast<int>(i);
    }
    return -1;
}

bool IsBlankClient(std::wstring_view s) {
    for (wchar_t c : s)
        if (!IsClientWs(c)) return false;
    return true;
}

std::vector<std::wstring> ClientTokens(std::wstring_view line) {
    std::vector<std::wstring> t;
    size_t i = 0;
    const size_t n = line.size();
    for (;;) {
        while (i < n && IsClientWs(line[i])) ++i;
        if (i >= n) return t;
        size_t j = i;
        while (j < n && line[j] != L'\t') ++j;
        t.emplace_back(line.substr(i, j - i));
        i = j;
    }
}

std::vector<std::wstring_view> SplitView(std::wstring_view s, wchar_t sep) {
    std::vector<std::wstring_view> v;
    size_t a = 0;
    for (;;) {
        const size_t b = s.find(sep, a);
        if (b == std::wstring_view::npos) {
            v.push_back(s.substr(a));
            return v;
        }
        v.push_back(s.substr(a, b - a));
        a = b + 1;
    }
}

bool ParseInt(std::wstring_view s, long long& v) {
    if (s.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == L'-' || s[0] == L'+') { neg = s[0] == L'-'; i = 1; }
    if (i >= s.size()) return false;
    long long r = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < L'0' || s[i] > L'9') return false;
        r = r * 10 + (s[i] - L'0');
        if (r > (1LL << 40)) return false;
    }
    v = neg ? -r : r;
    return true;
}

bool ParseFloat(std::wstring_view s, double& v) {
    if (s.empty() || s.size() > 40) return false;
    std::wstring t(s);
    wchar_t* end = nullptr;
    v = wcstod(t.c_str(), &end);
    return end && *end == L'\0' && end != t.c_str();
}

std::wstring Trim(std::wstring_view s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return std::wstring(s.substr(a, b - a));
}

bool StartsWith(std::wstring_view s, std::wstring_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }

bool EndsWithI(std::wstring_view s, std::wstring_view p) {
    if (s.size() < p.size()) return false;
    return _wcsnicmp(s.data() + s.size() - p.size(), p.data(), p.size()) == 0;
}

std::wstring ToUpper(std::wstring_view s) {
    std::wstring r(s);
    for (auto& c : r) c = static_cast<wchar_t>(towupper(c));
    return r;
}

std::wstring Fmt(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    wchar_t buf[2048];
    const int n = _vsnwprintf_s(buf, std::size(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return n < 0 ? std::wstring(buf) : std::wstring(buf, static_cast<size_t>(n));
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring DirOf(const std::wstring& p) {
    const size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring() : p.substr(0, k);
}

std::wstring FileNameOf(const std::wstring& p) {
    const size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(k + 1);
}

}  // namespace se
