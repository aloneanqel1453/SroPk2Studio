#pragma once
// UI localization: five languages (English, Turkish, Simplified Chinese, Japanese, Vietnamese).
// The language comes from --lang, else the user's saved choice, else the Windows UI language.
//
// Every user-visible string lives in the tables under src/i18n/ - one .inc per module, each row a
// key and its five texts in the order en, tr, zh, ja, vi. L() returns the current language's text;
// Lf() formats it with std::format arguments ({} or {0}, {1} ... when a language needs another
// word order). tests/i18n_test.cpp checks that every row is complete and its placeholders match.
#include <format>
#include <span>
#include <string>
#include <string_view>

namespace pk2 {

enum class Lang { en, tr, zh, ja, vi };
inline constexpr int kLangCount = 5;
inline constexpr Lang kLangs[kLangCount] = {Lang::en, Lang::tr, Lang::zh, Lang::ja, Lang::vi};

// Windows UI language mapped to one of the five (anything else: English).
[[nodiscard]] Lang detect_ui_language();
void set_language(Lang lang);
[[nodiscard]] Lang current_language() noexcept;

[[nodiscard]] const wchar_t* language_tag(Lang lang) noexcept;    // "en", "tr", "zh", "ja", "vi"
[[nodiscard]] const wchar_t* language_name(Lang lang) noexcept;   // in the language itself: "Türkçe", "中文" ...
// Accepts the tags above and common variants ("zh-cn", "ja-JP", "tr_TR", "english" ...).
[[nodiscard]] bool parse_language(std::wstring_view tag, Lang& out) noexcept;

// Text of `key` in the current language (or `lang`). Unknown keys give an empty string.
[[nodiscard]] const std::wstring& L(std::string_view key);
[[nodiscard]] const std::wstring& L(std::string_view key, Lang lang);

// L(key) formatted with `args`; the plain text when the arguments do not fit its placeholders.
template <class... Args>
[[nodiscard]] std::wstring Lf(std::string_view key, const Args&... args) {
    const std::wstring& f = L(key);
    try {
        return std::vformat(f, std::make_wformat_args(args...));
    } catch (const std::format_error&) {
        return f;
    }
}

// The whole table (for the consistency test and tools).
struct I18nEntry {
    std::string_view key;
    std::wstring_view text[kLangCount];
};
[[nodiscard]] std::span<const I18nEntry> i18n_entries() noexcept;

} // namespace pk2
