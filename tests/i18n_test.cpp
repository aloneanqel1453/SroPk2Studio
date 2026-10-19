// Translation table check: every key unique, every row filled in all five languages, the same
// placeholders in every language, and (given the source folder) every L("key") / Lf("key") the
// sources use present in the table.
//
//   i18n_test [<src folder>]
#include "i18n.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;
using namespace pk2;

namespace {

std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The replacement fields of a std::format string, normalised to "index:spec" (automatic numbering
// resolved), or nullopt-like "!" when the braces do not form a valid format string.
std::multiset<std::wstring> fields(std::wstring_view f, bool& ok) {
    std::multiset<std::wstring> out;
    int automatic = 0;
    bool manual = false, autos = false;
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i] == L'}') {
            if (i + 1 < f.size() && f[i + 1] == L'}') { ++i; continue; }
            ok = false;
            return out;
        }
        if (f[i] != L'{') continue;
        if (i + 1 < f.size() && f[i + 1] == L'{') { ++i; continue; }
        const std::size_t end = f.find(L'}', i);
        if (end == std::wstring_view::npos) { ok = false; return out; }
        std::wstring body(f.substr(i + 1, end - i - 1));
        const std::size_t colon = body.find(L':');
        std::wstring id = body.substr(0, colon);
        const std::wstring spec = colon == std::wstring::npos ? std::wstring() : body.substr(colon);
        if (id.empty()) { autos = true; id = std::to_wstring(automatic++); }
        else manual = true;
        out.insert(id + spec);
        i = end;
    }
    if (manual && autos) ok = false;   // std::format rejects mixed numbering
    return out;
}

} // namespace

int main(int argc, char** argv) {
    int failures = 0;
    const auto entries = i18n_entries();
    std::unordered_set<std::string_view> keys;
    static const char* const kNames[kLangCount] = {"en", "tr", "zh", "ja", "vi"};

    for (const I18nEntry& e : entries) {
        if (!keys.insert(e.key).second) {
            std::printf("DUPLICATE  %.*s\n", static_cast<int>(e.key.size()), e.key.data());
            ++failures;
        }
        bool ok_en = true;
        const auto ref = fields(e.text[0], ok_en);
        if (!ok_en) {
            std::printf("BAD FORMAT %.*s [en]\n", static_cast<int>(e.key.size()), e.key.data());
            ++failures;
        }
        for (int l = 0; l < kLangCount; ++l) {
            if (e.text[l].empty()) {
                std::printf("MISSING    %.*s [%s]\n", static_cast<int>(e.key.size()), e.key.data(), kNames[l]);
                ++failures;
                continue;
            }
            if (l == 0) continue;
            bool ok = true;
            const auto got = fields(e.text[l], ok);
            if (!ok || got != ref) {
                std::printf("PLACEHOLDER %.*s [%s]: %s\n", static_cast<int>(e.key.size()), e.key.data(), kNames[l],
                            narrow(e.text[l]).c_str());
                ++failures;
            }
        }
    }

    std::size_t used = 0;
    if (argc > 1) {
        // Keys the sources ask for. Vendored trees (bak, bak2, thirdparty) have their own texts.
        const std::regex call(R"(\bLf?\(\s*"([A-Za-z0-9_.]+)\")");
        std::set<std::string> missing;
        for (const auto& it : fs::recursive_directory_iterator(argv[1])) {
            if (!it.is_regular_file()) continue;
            const fs::path& p = it.path();
            const std::string rel = fs::relative(p, argv[1]).generic_string();
            if (rel.starts_with("bak/") || rel.starts_with("bak2/") || rel.starts_with("thirdparty/")) continue;
            const std::string ext = p.extension().string();
            if (ext != ".cpp" && ext != ".hpp") continue;
            std::ifstream in(p, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            for (std::sregex_iterator m(text.begin(), text.end(), call), end; m != end; ++m) {
                ++used;
                const std::string key = (*m)[1].str();
                if (!keys.contains(key)) missing.insert(rel + ": " + key);
            }
        }
        for (const std::string& m : missing) {
            std::printf("UNKNOWN KEY %s\n", m.c_str());
            ++failures;
        }
    }

    std::printf("%zu keys x %d languages, %zu lookups in the sources: %s\n", entries.size(), kLangCount, used,
                failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}
