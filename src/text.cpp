#include "text.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace pk2 {
namespace {

std::wstring mb_to_wide(std::string_view s, UINT cp, DWORD flags) {
    if (s.empty())
        return {};
    const int n = ::MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wide_to_mb(std::wstring_view s, UINT cp) {
    if (s.empty())
        return {};
    const int n = ::WideCharToMultiByte(cp, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(cp, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

} // namespace

std::wstring utf8_to_wide(std::string_view s) { return mb_to_wide(s, CP_UTF8, 0); }
std::string  wide_to_utf8(std::wstring_view s) { return wide_to_mb(s, CP_UTF8); }
std::wstring acp_to_wide(std::string_view s)  { return mb_to_wide(s, CP_ACP, 0); }
std::string  wide_to_acp(std::wstring_view s)  { return wide_to_mb(s, CP_ACP); }

std::wstring decode_pk2_name(const std::array<std::uint8_t, kNameBytes>& raw) {
    // Length up to first NUL.
    std::size_t len = 0;
    while (len < kNameBytes && raw[len] != 0)
        ++len;
    const std::string_view sv(reinterpret_cast<const char*>(raw.data()), len);
    if (sv.empty())
        return {};
    // Prefer UTF-8 when the bytes form valid UTF-8; otherwise fall back to CP_ACP.
    const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, sv.data(),
                                        static_cast<int>(sv.size()), nullptr, 0);
    if (n > 0)
        return utf8_to_wide(sv);
    return acp_to_wide(sv);
}

std::array<std::uint8_t, kNameBytes> encode_pk2_name(std::wstring_view name) {
    std::array<std::uint8_t, kNameBytes> raw{};
    const std::string bytes = wide_to_acp(name);
    const std::size_t n = std::min(bytes.size(), kNameBytes - 1); // keep room for NUL
    std::memcpy(raw.data(), bytes.data(), n);
    return raw;
}

std::wstring to_lower(std::wstring_view s) {
    std::wstring out(s);
    if (!out.empty())
        ::CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    return out;
}

std::wstring decode_text(const std::vector<std::uint8_t>& b, int& enc) {
    if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
        enc = kEncUtf16;
        return std::wstring(reinterpret_cast<const wchar_t*>(b.data() + 2), (b.size() - 2) / 2);
    }
    if (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
        enc = kEncUtf8Bom;
        return utf8_to_wide(std::string_view(reinterpret_cast<const char*>(b.data() + 3), b.size() - 3));
    }
    // UTF-16LE without BOM (common in SRO data tables): many zero high bytes.
    if (b.size() >= 4 && b.size() % 2 == 0) {
        std::size_t zeros = 0, checks = 0;
        const std::size_t lim = std::min<std::size_t>(b.size(), 1024);
        for (std::size_t i = 1; i < lim; i += 2) { ++checks; if (b[i] == 0) ++zeros; }
        if (checks > 0 && zeros * 100 / checks > 55) {
            enc = kEncUtf16;
            return std::wstring(reinterpret_cast<const wchar_t*>(b.data()), b.size() / 2);
        }
    }
    const std::string_view s(reinterpret_cast<const char*>(b.data()), b.size());
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0) > 0) {
        enc = kEncUtf8;
        return utf8_to_wide(s);
    }
    // Korean (CP949): SRO data tables are frequently EUC-KR / CP949 encoded.
    if (std::wstring w = mb_to_wide(s, 949, MB_ERR_INVALID_CHARS); !w.empty()) {
        enc = kEncCp949;
        return w;
    }
    enc = kEncAnsi;
    return acp_to_wide(s);
}

bool is_text_bytes(const std::vector<std::uint8_t>& b) {
    const std::size_t n = b.size();
    if (n == 0) return true;
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) return true;
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) return true;
    const std::size_t lim = std::min<std::size_t>(n, 2048);
    // UTF-16LE without BOM: high byte 0, low byte a printable / whitespace ASCII char.
    if (n >= 4 && n % 2 == 0) {
        std::size_t pairs = 0, good = 0;
        for (std::size_t i = 0; i + 1 < lim; i += 2) {
            ++pairs;
            const std::uint8_t lo = b[i], hi = b[i + 1];
            if (hi == 0 && (lo == 0x09 || lo == 0x0A || lo == 0x0D || (lo >= 0x20 && lo <= 0x7E))) ++good;
        }
        if (pairs > 0 && good * 100 / pairs > 60) return true;
    }
    // Single-byte encodings: no NUL and under 2% control bytes in the sampled head.
    std::size_t ctrl = 0, nul = 0;
    for (std::size_t i = 0; i < lim; ++i) {
        const std::uint8_t c = b[i];
        if (c == 0) ++nul;
        else if (c < 0x09 || (c > 0x0D && c < 0x20)) ++ctrl;
    }
    if (nul != 0 || ctrl * 100 / lim >= 2) return false;
    const int cap = static_cast<int>(std::min<std::size_t>(n, 65536));
    const char* p = reinterpret_cast<const char*>(b.data());
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, cap, nullptr, 0) > 0 ||
           ::MultiByteToWideChar(949, MB_ERR_INVALID_CHARS, p, cap, nullptr, 0) > 0;
}

bool readable_text(const std::wstring& s) {
    if (s.empty()) return false;
    const std::size_t n = std::min<std::size_t>(s.size(), 20000);
    std::size_t bad = 0, cjk = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const wchar_t c = s[i];
        if (c == L'\t' || c == L'\r' || c == L'\n') continue;
        if (c < 0x20 || c == 0x7F || c == 0xFFFD || (c >= 0xE000 && c <= 0xF8FF)) { ++bad; continue; }
        if (c >= 0xD800 && c <= 0xDBFF) {   // a high surrogate must be followed by a low one
            if (i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) { ++i; continue; }
            ++bad; continue;
        }
        if (c >= 0xDC00 && c <= 0xDFFF) { ++bad; continue; }
        if ((c >= 0x3400 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF)) ++cjk;
        else if (c >= 0x0250 && c <= 0x1DFF) {
            // Scripts of the client's languages are fine: Greek / Cyrillic, Hebrew / Arabic (+ supplement),
            // Thai, Hangul Jamo. The other blocks of this range (IPA, Armenian, Syriac, Indic, Tibetan,
            // Georgian, Ethiopic, Khmer, Mongolian ...) are typical of a wrong-encoding decode.
            const bool real = (c >= 0x0370 && c <= 0x052F) || (c >= 0x0590 && c <= 0x06FF) ||
                              (c >= 0x0750 && c <= 0x077F) || (c >= 0x0E00 && c <= 0x0E7F) ||
                              (c >= 0x1100 && c <= 0x11FF);
            if (!real) ++bad;
        } else if ((c >= 0x2C00 && c <= 0x2FFF) || (c >= 0xA000 && c <= 0xABFF)) ++bad;   // rare blocks: typical of garbage
    }
    if (bad * 100 > n * 3) return false;
    // Wrong-encoding UTF-16 of binary data reads as dense random Han characters with no line structure.
    if (cjk * 100 > n * 60 && s.find(L'\n') == std::wstring::npos && n > 64) return false;
    return true;
}

} // namespace pk2
