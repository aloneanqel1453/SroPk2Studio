#include "i18n.hpp"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <unordered_map>

namespace pk2 {
namespace {

// Rows: key, then en, tr, zh, ja, vi. One file per module keeps the tables reviewable.
constexpr I18nEntry kEntries[] = {
#include "i18n/core.inc"
#include "i18n/app.inc"
#include "i18n/app2.inc"
#include "i18n/browser.inc"
#include "i18n/archive.inc"
#include "i18n/binary_info.inc"
#include "i18n/bak_bridge.inc"
#include "i18n/bak2.inc"
};

Lang g_lang = Lang::en;

int index_of(Lang l) noexcept {
    switch (l) {
    case Lang::en: return 0;
    case Lang::tr: return 1;
    case Lang::zh: return 2;
    case Lang::ja: return 3;
    case Lang::vi: return 4;
    }
    return 0;
}

// Built once; the strings themselves stay in kEntries.
struct Table {
    std::unordered_map<std::string_view, const I18nEntry*> by_key;
    std::wstring text[std::size(kEntries)][kLangCount];
    Table() {
        by_key.reserve(std::size(kEntries));
        for (std::size_t i = 0; i < std::size(kEntries); ++i) {
            by_key.emplace(kEntries[i].key, &kEntries[i]);
            for (int l = 0; l < kLangCount; ++l) text[i][l] = std::wstring(kEntries[i].text[l]);
        }
    }
};
const Table& table() {
    static const Table t;
    return t;
}

} // namespace

Lang detect_ui_language() {
    switch (PRIMARYLANGID(::GetUserDefaultUILanguage())) {
    case LANG_TURKISH:    return Lang::tr;
    case LANG_CHINESE:    return Lang::zh;
    case LANG_JAPANESE:   return Lang::ja;
    case LANG_VIETNAMESE: return Lang::vi;
    default:              return Lang::en;
    }
}

void set_language(Lang lang) { g_lang = lang; }
Lang current_language() noexcept { return g_lang; }

const wchar_t* language_tag(Lang lang) noexcept {
    static constexpr const wchar_t* kTags[kLangCount] = {L"en", L"tr", L"zh", L"ja", L"vi"};
    return kTags[index_of(lang)];
}

const wchar_t* language_name(Lang lang) noexcept {
    static constexpr const wchar_t* kNames[kLangCount] = {L"English", L"Türkçe", L"中文", L"日本語", L"Tiếng Việt"};
    return kNames[index_of(lang)];
}

bool parse_language(std::wstring_view tag, Lang& out) noexcept {
    std::wstring t(tag);
    std::transform(t.begin(), t.end(), t.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    const std::size_t sep = t.find_first_of(L"-_");
    const std::wstring head = t.substr(0, sep);
    struct Alias { const wchar_t* name; Lang lang; };
    static constexpr Alias kAliases[] = {
        {L"en", Lang::en}, {L"english", Lang::en},
        {L"tr", Lang::tr}, {L"turkish", Lang::tr}, {L"türkçe", Lang::tr}, {L"turkce", Lang::tr},
        {L"zh", Lang::zh}, {L"chinese", Lang::zh}, {L"cn", Lang::zh},
        {L"ja", Lang::ja}, {L"japanese", Lang::ja}, {L"jp", Lang::ja},
        {L"vi", Lang::vi}, {L"vietnamese", Lang::vi}, {L"vn", Lang::vi},
    };
    for (const Alias& a : kAliases)
        if (head == a.name || t == a.name) { out = a.lang; return true; }
    return false;
}

const std::wstring& L(std::string_view key, Lang lang) {
    static const std::wstring empty;
    const Table& t = table();
    const auto it = t.by_key.find(key);
    if (it == t.by_key.end()) return empty;
    const std::size_t row = static_cast<std::size_t>(it->second - kEntries);
    const std::wstring& s = t.text[row][index_of(lang)];
    return s.empty() ? t.text[row][0] : s;   // English when a translation is missing (the test forbids it)
}

const std::wstring& L(std::string_view key) { return L(key, g_lang); }

std::span<const I18nEntry> i18n_entries() noexcept { return kEntries; }

} // namespace pk2
