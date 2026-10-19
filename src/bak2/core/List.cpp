// SkillEffect Studio - core/List.cpp
#include "List.h"

#include "Enums.h"

#include <cwctype>

namespace se {

Sec GuessSection(const std::vector<std::wstring>& tok) {
    const size_t n = tok.size();
    if (n >= 4 && enums::AniTypeIndex(tok[2]) >= 0) return Sec::SE;
    if (n >= 5 && (tok[0] == L"0" || tok[0] == L"1") && (tok[4] == L"FALSE" || tok[4] == L"TRUE")) return Sec::SA;
    double f = 0;
    if (n >= 9 && enums::In(enums::kRide, tok[3]) && ParseFloat(tok[2], f)) return Sec::CI;
    return Sec::None;
}

static Issue Mk(Sev s, const wchar_t* rule, int line, const std::wstring& key, std::wstring tr, std::wstring en) {
    Issue i;
    i.sev = s;
    i.rule = rule;
    i.listLine = line;
    i.key = key;
    i.tr = std::move(tr);
    i.en = std::move(en);
    return i;
}

static std::vector<std::wstring> WsSplit(std::wstring_view s) {
    std::vector<std::wstring> v;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i;
        size_t j = i;
        while (j < s.size() && s[j] != L' ' && s[j] != L'\t') ++j;
        if (j > i) v.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return v;
}

ParsedList ParseListText(const std::wstring& text, const std::wstring& desc) {
    ParsedList pl;
    pl.desc = desc;
    Sec cur = Sec::None;
    Dir dir = Dir::Auto;
    std::wstring dirKey;
    std::vector<std::wstring> pending;
    bool inBlock = false;
    int blockStart = 0, blockLines = 0;
    std::wstring table;  // inside "#table <id>": rows go verbatim to that client table
    std::wstring_view t(text);
    size_t a = 0;
    int lineNo = 0;
    while (a < t.size()) {
        size_t b = t.find(L'\n', a);
        if (b == std::wstring_view::npos) b = t.size();
        std::wstring_view l = t.substr(a, b - a);
        a = b + 1;
        ++lineNo;
        while (!l.empty() && l.back() == L'\r') l.remove_suffix(1);
        if (inBlock) {  // mimic the client: a /* block hides lines until the first '*' followed by '/'
            ++blockLines;
            const size_t j = l.find(L'*');
            if (j != std::wstring_view::npos && j + 1 < l.size() && l[j + 1] == L'/') {
                inBlock = false;
                pl.issues.push_back(Mk(Sev::Warn, L"L2", blockStart, L"",
                                       Fmt(L"Listede /* */ yorum bloğu: %d satır yok sayıldı (istemci de okumaz).", blockLines),
                                       Fmt(L"/* */ comment block in the list: %d lines ignored (the client skips them too).",
                                           blockLines)));
                l = l.substr(j + 2);
                if (IsBlankClient(l)) continue;
            } else {
                continue;
            }
        }
        if (IsBlankClient(l)) {
            if (table.empty()) pending.emplace_back(l);
            continue;
        }
        {  // "#replace KEY" / "#remove KEY" are global: valid anywhere, also inside a #table block
            size_t ld = 0;
            while (ld < l.size() && IsClientWs(l[ld])) ++ld;
            if (ld < l.size() && l[ld] == L'#') {
                auto tok = WsSplit(l.substr(ld));
                std::wstring cmd = tok[0];
                for (auto& c : cmd) c = static_cast<wchar_t>(towlower(c));
                const bool rep = cmd == L"#replace" || cmd == L"#degistir" || cmd == L"#değiştir";
                const bool rem = cmd == L"#remove" || cmd == L"#kaldir" || cmd == L"#kaldır";
                if (rep || rem) {
                    if (tok.size() < 2)
                        pl.issues.push_back(Mk(Sev::Warn, L"L4", lineNo, L"", cmd + L" için anahtar yok.", cmd + L" needs a key."));
                    for (size_t k = 1; k < tok.size(); ++k) (rep ? pl.replaceKeys : pl.removeKeys).insert(tok[k]);
                    continue;
                }
            }
        }
        if (!table.empty()) {
            size_t ld = 0;
            while (ld < l.size() && (l[ld] == L' ' || l[ld] == 0x3000)) ++ld;
            if (ld < l.size() && l[ld] == L'#') {
                std::wstring_view h = l.substr(ld);
                if (StartsWith(h, L"#section") || StartsWith(h, L"#table") || StartsWith(h, L"#end-table")) {
                    table.clear();  // handled below like any directive
                } else {
                    continue;
                }
            } else {
                if (StartsWith(l.substr(ld), L"//")) continue;
                TableRow tr;
                tr.table = table;
                tr.text = std::wstring(l);
                tr.listLine = lineNo;
                pl.tables.push_back(std::move(tr));
                continue;
            }
        }
        size_t lead = 0;
        while (lead < l.size() && IsClientWs(l[lead])) ++lead;
        if (l[lead] == L'#') {
            auto tok = WsSplit(l.substr(lead));
            std::wstring cmd = tok[0];
            for (auto& c : cmd) c = static_cast<wchar_t>(towlower(c));
            const std::wstring arg = tok.size() > 1 ? tok[1] : L"";
            if (cmd == L"#table" || cmd == L"#tablo") {
                table = arg;
                for (auto& c : table) c = static_cast<wchar_t>(towlower(c));
                if (table.empty()) {
                    pl.issues.push_back(Mk(Sev::Warn, L"L4", lineNo, L"", L"#table için tablo adı yok.", L"#table needs a table name."));
                }
                cur = Sec::None;
                pending.clear();
                continue;
            } else if (cmd == L"#end-table") {
                pending.clear();
                continue;
            } else if (cmd == L"#section") {
                const Sec s = SecFromName(arg);
                if (s == Sec::None || s == Sec::Light || s == Sec::Env)
                    pl.issues.push_back(Mk(Sev::Warn, L"L3", lineNo, arg, L"Bilinmeyen/desteklenmeyen bölüm adı: " + arg,
                                           L"Unknown/unsupported section name: " + arg));
                cur = (s == Sec::CI || s == Sec::SA || s == Sec::SE) ? s : Sec::None;
                dir = Dir::Auto;
                dirKey.clear();
            } else if (cmd == L"#after" || cmd == L"#sonra") {
                dir = Dir::After;
                dirKey = arg;
            } else if (cmd == L"#before" || cmd == L"#once" || cmd == L"#önce") {
                dir = Dir::Before;
                dirKey = arg;
            } else if (cmd == L"#end" || cmd == L"#son" || cmd == L"#sonuna") {
                dir = Dir::End;
                dirKey.clear();
            } else if (cmd == L"#auto" || cmd == L"#otomatik") {
                dir = Dir::Auto;
                dirKey.clear();
            } else {
                pl.issues.push_back(Mk(Sev::Warn, L"L4", lineNo, cmd, L"Bilinmeyen komut satırı, yok sayıldı: " + cmd,
                                       L"Unknown directive line ignored: " + cmd));
            }
            if ((dir == Dir::After || dir == Dir::Before) && dirKey.empty()) {
                pl.issues.push_back(Mk(Sev::Warn, L"L4", lineNo, L"", L"#after/#before için anahtar yok.",
                                       L"#after/#before needs a key."));
                dir = Dir::Auto;
            }
            pending.clear();
            continue;
        }
        // client comment rules (R3): only the first '/' of the line counts
        std::wstring_view eff = l;
        const size_t j = l.find(L'/');
        if (j != std::wstring_view::npos && j + 1 < l.size() && (l[j + 1] == L'/' || l[j + 1] == L'*')) {
            const bool block = l[j + 1] == L'*';
            eff = l.substr(0, j);
            if (block) {
                inBlock = true;
                blockStart = lineNo;
                blockLines = 1;
            }
            if (IsBlankClient(eff)) {
                if (!block) pending.emplace_back(l);  // a safe '//' comment line
                continue;
            }
            pl.issues.push_back(Mk(Sev::Error, L"V3", lineNo, L"",
                                   L"Satırda '//' veya '/*' var: istemci satırı orada keser (yolda '/' yerine '\\' kullanın).",
                                   L"Row contains '//' or '/*': the client cuts the row there (use '\\' in paths)."));
        }
        auto tok = ClientTokens(eff);
        if (tok.empty()) {
            pending.emplace_back(l);
            continue;
        }
        ListRow r;
        r.text = std::wstring(l);
        r.listLine = lineNo;
        r.dir = dir;
        r.dirKey = dirKey;
        const Sec g = GuessSection(tok);
        if (cur != Sec::None) {
            r.sec = cur;
            if (g != Sec::None && g != cur)
                pl.issues.push_back(Mk(Sev::Warn, L"L5", lineNo, L"",
                                       std::wstring(L"Satır #section ") + SecName(cur) + L" altında ama " + SecName(g) +
                                           L" satırına benziyor.",
                                       std::wstring(L"Row is under #section ") + SecName(cur) + L" but looks like a " +
                                           SecName(g) + L" row."));
        } else if (g != Sec::None) {
            r.sec = g;
            r.guessed = true;
        } else {
            pl.issues.push_back(Mk(Sev::Error, L"L1", lineNo, tok[0],
                                   L"Satır tanınmadı (characterInfo/skillaniset2/skilleffectset değil); '#section' ekleyin.",
                                   L"Row not recognised (not characterInfo/skillaniset2/skilleffectset); add '#section'."));
            pending.clear();
            continue;
        }
        const size_t kc = static_cast<size_t>(KeyColumn(r.sec));
        r.key = kc < tok.size() ? tok[kc] : L"";
        r.cells = std::move(tok);
        r.pre = std::move(pending);
        pending.clear();
        pl.rows.push_back(std::move(r));
    }
    if (inBlock)
        pl.issues.push_back(Mk(Sev::Error, L"L2", blockStart, L"",
                               L"Listede kapanmayan /* bloğu: sonraki tüm satırlar yok sayıldı.",
                               L"Unclosed /* block in the list: all following lines were ignored."));
    return pl;
}

static bool ReadText(const std::wstring& p, std::wstring& out, std::wstring& err) {
    Bytes b;
    if (!ReadFileBytes(p, b, err)) return false;
    out = DecodeText(b);
    return true;
}

bool LoadListPath(const std::wstring& path, std::wstring& listText, std::wstring& desc, std::wstring& err) {
    std::wstring dir;
    if (DirExists(path)) {
        if (FileExists(JoinPath(JoinPath(path, L"_merge"), L"skilleffect.txt.skillaniset2.frag")) ||
            FileExists(JoinPath(JoinPath(path, L"_merge"), L"skilleffect.txt.characterInfo.frag")))
            dir = JoinPath(path, L"_merge");
        else
            dir = path;
    } else if (EndsWithI(path, L"merge_plan.json") || EndsWithI(path, L".frag")) {
        dir = DirOf(path);
    }
    if (!dir.empty()) {
        const wchar_t* secs[] = {L"characterInfo", L"skillaniset2", L"skilleffectset"};
        std::wstring out = L"// TeraStudio _merge: " + dir + L"\r\n";
        int found = 0;
        for (const auto* s : secs) {
            const std::wstring f = JoinPath(dir, std::wstring(L"skilleffect.txt.") + s + L".frag");
            if (!FileExists(f)) continue;
            std::wstring t;
            if (!ReadText(f, t, err)) return false;
            ++found;
            out += std::wstring(L"#section ") + s + L"\r\n";
            out += t;
            if (!t.empty() && t.back() != L'\n') out += L"\r\n";
        }
        if (!found) {
            err = L"Klasörde skilleffect.txt.*.frag yok / no skilleffect.txt.*.frag in: " + dir;
            return false;
        }
        if (FileExists(JoinPath(dir, L"effectsound.txt.append.frag")))
            out += L"// not: effectsound.txt.append.frag ayrı dosyadır (effectsound.txt sonuna eklenebilir, V18)\r\n";
        listText = out;
        desc = Fmt(L"TeraStudio _merge (%d frag)", found);
        return true;
    }
    if (!FileExists(path)) {
        err = L"Bulunamadı / not found: " + path;
        return false;
    }
    Bytes b;
    if (!ReadFileBytes(path, b, err)) return false;
    TextEnc enc{};
    listText = DecodeText(b, &enc);
    desc = FileNameOf(path) + L" (" + EncName(enc) + L")";
    return true;
}

}  // namespace se
