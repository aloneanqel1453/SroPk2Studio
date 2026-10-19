// SroPk2Studio glue (not BAK-2 code): BAK-2's own texts in the app's five languages. Private to the
// src/bak2_*.cpp bridges.
//
// BAK-2 writes its texts in Turkish and English, some only in one of them. Where it offers the choice
// the bridges pass Bak2English(). A BAK-2 text that reaches the app's UI is read back against the
// BAK-2 wording it was made from (TextMap::from, "{}" where BAK-2 inserts a value) and shown from its
// row in src/i18n/bak2.inc in the current language, the inserted values in place.
#pragma once

#include "../../i18n.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bak2::glue {

// BAK-2's language for a text it has in Turkish and English: English for every UI language but Turkish.
inline bool Bak2English() { return pk2::current_language() != pk2::Lang::tr; }

struct TextMap {
    std::wstring_view from;   // BAK-2's wording; "{}" = any text (a value BAK-2 inserted)
    std::string_view key;     // bak2.inc row; its {} / {0} {1} ... take the values in order
};

namespace detail {

// text against pat; each "{}" takes the shortest run that lets the rest match. vals = the runs.
inline bool Match(std::wstring_view text, std::wstring_view pat, std::vector<std::wstring>& vals) {
    const std::size_t ph = pat.find(L"{}");
    if (ph == std::wstring_view::npos) return text == pat;
    if (text.substr(0, ph) != pat.substr(0, ph)) return false;
    text.remove_prefix(ph);
    pat.remove_prefix(ph + 2);
    const std::size_t next = pat.find(L"{}");
    const std::wstring_view lit = pat.substr(0, next);
    if (next == std::wstring_view::npos) {   // last value: the rest of the pattern ends the text
        if (text.size() < lit.size() || text.substr(text.size() - lit.size()) != lit) return false;
        vals.emplace_back(text.substr(0, text.size() - lit.size()));
        return true;
    }
    for (std::size_t at = text.find(lit); at != std::wstring_view::npos; at = text.find(lit, at + 1)) {
        vals.emplace_back(text.substr(0, at));
        if (Match(text.substr(at), pat, vals)) return true;
        vals.pop_back();
    }
    return false;
}

// f with "{}" (in order) and "{n}" replaced by vals, "{{" / "}}" as braces.
inline std::wstring Fill(std::wstring_view f, const std::vector<std::wstring>& vals) {
    std::wstring out;
    std::size_t auto_index = 0;
    for (std::size_t i = 0; i < f.size(); ++i) {
        const wchar_t c = f[i];
        if ((c == L'{' || c == L'}') && i + 1 < f.size() && f[i + 1] == c) {
            out += c;
            ++i;
            continue;
        }
        if (c == L'{') {
            const std::size_t end = f.find(L'}', i);
            if (end != std::wstring_view::npos) {
                const std::wstring_view id = f.substr(i + 1, end - i - 1);
                std::size_t k = id.empty() ? auto_index++ : 0;
                bool digits = true;
                for (wchar_t d : id) {
                    if (d < L'0' || d > L'9') digits = false;
                    else k = k * 10 + static_cast<std::size_t>(d - L'0');
                }
                if (digits) {
                    if (k < vals.size()) out += vals[k];
                    i = end;
                    continue;
                }
            }
        }
        out += c;
    }
    return out;
}

}  // namespace detail

// text in the current language when it is one of map's BAK-2 texts; else false (out untouched).
inline bool Localize(std::wstring_view text, std::span<const TextMap> map, std::wstring& out) {
    std::vector<std::wstring> vals;
    for (const TextMap& m : map) {
        vals.clear();
        if (detail::Match(text, m.from, vals)) {
            out = detail::Fill(pk2::L(m.key), vals);
            return true;
        }
    }
    return false;
}

// a BAK-2 text it writes in one language only: in the current language when map knows it, else as written.
inline std::wstring Localize(const std::wstring& text, std::span<const TextMap> map) {
    std::wstring out;
    return Localize(text, map, out) ? out : text;
}

// a BAK-2 text it writes in Turkish and English: the English form read back through map, else the form
// of the UI language.
inline std::wstring Localize(const std::wstring& tr, const std::wstring& en, std::span<const TextMap> map) {
    std::wstring out;
    if (Localize(en, map, out)) return out;
    return Bak2English() ? en : tr;
}

// se::render texts (asset cache warnings, model load errors) - BAK-2 writes them in Turkish only.
inline constexpr TextMap kRenderTexts[] = {
    {L"doku okunamadi: {} ({})", "b2_w_tex_unreadable"},
    {L"doku yok: {}", "b2_w_tex_missing"},
    {L"okunamadi: {} ({})", "b2_w_unreadable"},
    {L"dosya yok: {}", "b2_w_file_missing"},
    {L"bsr yok: {}", "b2_w_bsr_missing"},
    {L"iskelet yok: {}", "b2_w_skel_missing"},
    {L"mesh yok: {}", "b2_w_mesh_missing"},
    {L"cpd okunamadi: {}", "b2_w_cpd_unreadable"},
    {L"model okunamadi: {}", "b2_w_model_unreadable"},
    {L"modelin mesh'i yok: {}", "b2_w_model_no_mesh"},
    {L"parca mesh'i yok: {}", "b2_w_part_no_mesh"},
};

}  // namespace bak2::glue
