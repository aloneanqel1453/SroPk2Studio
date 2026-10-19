// ============================================================================
//  SRO Asset Studio - formats/DataTableDeps.cpp
//
//  The other half of an export.
//
//  A .bsr names its meshes, materials, skeleton, animations and - through the
//  mod palette - its sounds and effects. It does NOT name the object's
//  inventory icon, and it does not name the ambient effect the client attaches
//  to it. Those live in the client's TEXT DATA, and an export without them
//  gives the importer a model with no icon that never lights up:
//
//     characterdata_38800  ... pet2\pet_bird_a.bsr ... pet2\item\item_..._a.ddj
//              |                                              `-- the ICON
//              `-- codename COS_PET2_ASS_BIRD_A
//                       |
//     effectsound.txt    +--> "Pet2\" + "pet_bird_a_stand1.wav"   (sounds)
//     skilleffectset_*   +--> "cos\"  + "pet2_bird_eyes.efp"      (ambient fx)
//     itemdata_*         +--> the summon scroll item row
//
//  Two shapes have to be handled: a path written whole in one column, and a
//  path SPLIT across two columns (a folder ending in '\' followed by the file
//  name). The second is what effectsound.txt and skilleffectset_* use, and
//  missing it is why those files never came along.
//
//  Nothing here guesses a file into existence: a token is only accepted when a
//  real entry in one of the archives ends with it.
// ============================================================================
#include "AssetDeps.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace sro {

namespace {

constexpr size_t kMaxRowsPerFile = 64;      ///< a codename is not in thousands of rows

bool IsAssetExt(const std::wstring& lower) {
    static const wchar_t* kExts[] = { L".ddj", L".efp", L".wav", L".bsr", L".bms",
                                      L".bmt", L".bsk", L".ban", L".dds", L".mp3",
                                      L".ogg", L".ifo", L".2dt" };
    for (const wchar_t* e : kExts) {
        const size_t n = wcslen(e);
        if (lower.size() > n && lower.compare(lower.size() - n, n, e) == 0) return true;
    }
    return false;
}

/// Split a tab/space separated row into columns, keeping empties out.
void SplitRow(const std::wstring& line, std::vector<std::wstring>& out) {
    out.clear();
    size_t i = 0;
    while (i < line.size()) {
        size_t j = line.find_first_of(L"\t", i);
        if (j == std::wstring::npos) j = line.size();
        std::wstring col = TrimW(line.substr(i, j - i));
        if (!col.empty()) out.push_back(col);
        i = j + 1;
    }
}

/// Every path-looking token in a row, including the ones SPLIT across two
/// columns as "folder\" followed by "file.ext".
void PathTokens(const std::vector<std::wstring>& cols, std::vector<std::wstring>& out) {
    for (size_t i = 0; i < cols.size(); ++i) {
        const std::wstring low = ToLowerW(cols[i]);
        if (IsAssetExt(low)) {
            // A bare file name is only usable when the previous column is the
            // folder it lives in - that is the effectsound / skilleffectset
            // shape. A whole path stands on its own.
            if (low.find(L'\\') != std::wstring::npos || low.find(L'/') != std::wstring::npos) {
                out.push_back(cols[i]);
            } else if (i > 0) {
                const std::wstring& prev = cols[i - 1];
                if (!prev.empty() && (prev.back() == L'\\' || prev.back() == L'/'))
                    out.push_back(prev + cols[i]);
                else
                    out.push_back(cols[i]);       // resolved by basename below
            } else {
                out.push_back(cols[i]);
            }
        }
    }
}

/// Object codenames in a row: SILKROAD_STYLE_UPPERCASE tokens.
void CodeNames(const std::vector<std::wstring>& cols, std::vector<std::wstring>& out) {
    for (const std::wstring& c : cols) {
        if (c.size() < 8 || c.size() > 96) continue;
        bool ok = true, hasUnderscore = false;
        for (wchar_t ch : c) {
            if (ch == L'_') { hasUnderscore = true; continue; }
            if (!((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9'))) { ok = false; break; }
        }
        if (ok && hasUnderscore) out.push_back(c);
    }
}

std::wstring ReadTextUtf(const std::vector<u8>& bytes) {
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        const size_t n = (bytes.size() - 2) / 2;
        std::wstring s(n, L'\0');
        std::memcpy(&s[0], bytes.data() + 2, n * 2);
        return s;
    }
    size_t off = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) off = 3;
    return MultiByteToWide(std::string(reinterpret_cast<const char*>(bytes.data()) + off,
                                       bytes.size() - off), CP_UTF8);
}

} // namespace

// ---------------------------------------------------------------------------
void CollectDataTableCompanions(const std::vector<std::wstring>& rootPaths,
                                const AssetEnumerator& enumerate,
                                const AssetLoader& loader,
                                AssetDeps& inout,
                                std::vector<DataTableRows>& tablesOut,
                                const ProgressFn& progress) {
    if (!enumerate || !loader || rootPaths.empty()) return;

    // 1) Index every entry by file name, so a token written relative to some
    //    root ("pet2\item\x.ddj") can be matched against the real inner path
    //    ("icon\pet2\item\x.ddj") without guessing prefixes.
    std::unordered_map<std::wstring, std::vector<std::wstring>> byName;
    std::vector<std::wstring> tables;
    u64 seen = 0;
    enumerate([&](const std::wstring& inner) -> bool {
        if ((++seen & 0x1FFF) == 0 &&
            !ReportProgress(progress, seen, 0, L"Veri tabloları taranıyor..."))
            return false;
        const std::wstring low = ToLowerW(inner);
        if (low.find(L"\\textdata\\") != std::wstring::npos &&
            low.size() > 4 && low.compare(low.size() - 4, 4, L".txt") == 0)
            tables.push_back(inner);
        byName[ToLowerW(PathFileName(inner))].push_back(inner);
        return true;
    });
    if (tables.empty()) return;

    auto resolve = [&](const std::wstring& token) -> std::wstring {
        const std::wstring norm = NormalizeInnerPath(token);
        const std::wstring name = ToLowerW(PathFileName(norm));
        auto it = byName.find(name);
        if (it == byName.end()) return std::wstring();
        // Prefer an entry whose tail equals the whole token.
        const std::wstring tail = ToLowerW(norm);
        for (const std::wstring& cand : it->second) {
            const std::wstring lc = ToLowerW(cand);
            if (lc.size() >= tail.size() &&
                lc.compare(lc.size() - tail.size(), tail.size(), tail) == 0)
                return cand;
        }
        return it->second.size() == 1 ? it->second[0] : std::wstring();
    };

    // 2) Find the object's codenames: rows that name one of our root models.
    std::vector<std::wstring> needles;
    for (const std::wstring& r : rootPaths) {
        std::wstring n = ToLowerW(PathFileName(NormalizeInnerPath(r)));
        if (!n.empty()) needles.push_back(n);
    }
    if (needles.empty()) return;

    std::unordered_set<std::wstring> codes;
    std::vector<std::wstring> cols, toks, names;

    auto tableFor = [&](const std::wstring& inner, bool utf16) -> DataTableRows& {
        for (DataTableRows& t : tablesOut) if (t.innerPath == inner) return t;
        tablesOut.push_back(DataTableRows{ inner, {}, {}, utf16 });
        return tablesOut.back();
    };

    auto scanTables = [&](bool wantCodes) {
        for (const std::wstring& t : tables) {
            std::vector<u8> bytes;
            if (!loader(t, bytes) || bytes.empty()) continue;
            const bool utf16 = bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE;
            const std::wstring text = ReadTextUtf(bytes);
            const std::wstring lowText = ToLowerW(text);

            // Cheap reject: the whole file need not be split when it cannot match.
            bool possible = false;
            if (wantCodes) {
                for (const std::wstring& n : needles)
                    if (lowText.find(n) != std::wstring::npos) { possible = true; break; }
            } else {
                for (const std::wstring& c : codes)
                    if (lowText.find(ToLowerW(c)) != std::wstring::npos) { possible = true; break; }
            }
            if (!possible) continue;

            std::vector<std::wstring> headerLines;
            bool headerWanted = true;
            size_t start = 0, rows = 0;
            while (start < text.size() && rows < kMaxRowsPerFile) {
                size_t end = text.find(L'\n', start);
                if (end == std::wstring::npos) end = text.size();
                std::wstring line = text.substr(start, end - start);
                start = end + 1;
                if (line.empty()) continue;
                if (line[0] == L'/') {
                    // The table's own header describes its columns; an importer
                    // needs it to know where a row's fields go.
                    if (headerWanted) headerLines.push_back(TrimW(line));
                    continue;
                }

                const std::wstring low = ToLowerW(line);
                bool hit = false;
                if (wantCodes) {
                    for (const std::wstring& n : needles)
                        if (low.find(n) != std::wstring::npos) { hit = true; break; }
                } else {
                    for (const std::wstring& c : codes)
                        if (low.find(ToLowerW(c)) != std::wstring::npos) { hit = true; break; }
                }
                if (!hit) continue;
                ++rows;

                SplitRow(line, cols);
                if (wantCodes) {
                    CodeNames(cols, names);
                    for (const std::wstring& c : names) codes.insert(c);
                    names.clear();
                }
                PathTokens(cols, toks);
                for (const std::wstring& tok : toks) {
                    const std::wstring full = resolve(tok);
                    if (!full.empty()) CollectAssetDependenciesInto(full, loader, inout);
                }
                toks.clear();

                DataTableRows& dst = tableFor(t, utf16);
                if (dst.header.empty() && !headerLines.empty()) dst.header = headerLines;
                const std::wstring rec = TrimW(line);
                if (std::find(dst.rows.begin(), dst.rows.end(), rec) == dst.rows.end())
                    dst.rows.push_back(rec);
            }
            headerWanted = false;
            if (!ReportProgress(progress, 0, 0, L"Veri tabloları taranıyor...")) return;
        }
    };

    scanTables(true);       // rows naming the model -> codenames + their files
    if (!codes.empty())
        scanTables(false);  // rows naming the codename -> icon, sounds, effects
}

} // namespace sro
