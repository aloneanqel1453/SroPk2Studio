// SkillEffect Studio - core/Source.cpp
#include "Source.h"

#include "Pk2.h"
#include "SkillCrypt.h"

#include <cwctype>
#include <memory>

namespace se {

void RefData::Clear() {
    chars.clear();
    skillCodes.clear();
    skillGroups.clear();
    groupOfCode.clear();
    skills.clear();
    skillById.clear();
    charById.clear();
    ownerOfSkill.clear();
    desc.clear();
}

const CharRef* RefData::Char(const std::wstring& code) const {
    auto it = chars.find(code);
    return it == chars.end() ? nullptr : &it->second;
}

static void ForEachLine(const std::wstring& text, const std::function<void(std::wstring_view)>& fn) {
    std::wstring_view t(text);
    size_t a = 0;
    while (a < t.size()) {
        size_t b = t.find(L'\n', a);
        if (b == std::wstring_view::npos) b = t.size();
        std::wstring_view l = t.substr(a, b - a);
        if (!l.empty() && l.back() == L'\r') l.remove_suffix(1);
        fn(l);
        a = b + 1;
    }
}

void RefData::AddCharacterData(const std::wstring& text) {
    ForEachLine(text, [&](std::wstring_view l) {
        if (l.empty() || l[0] == L'/') return;
        auto c = SplitView(l, L'\t');
        if (c.size() < 58) return;
        long long id = 0, lv = -1, rar = 0;
        if (!ParseInt(c[1], id) || c[2].empty()) return;
        ParseInt(c[57], lv);
        ParseInt(c[15], rar);
        CharRef r;
        r.id = static_cast<int>(id);
        r.level = static_cast<int>(lv);
        r.rarity = static_cast<int>(rar);
        r.service = c[0] == L"1";
        if (c.size() > 52) r.bsr = std::wstring(c[52]);
        r.nameStrId = std::wstring(c[5]);
        chars[std::wstring(c[2])] = r;
        charById[r.id] = std::wstring(c[2]);
        for (size_t k = 83; k <= 92 && k < c.size(); ++k) {
            long long sid = 0;
            if (ParseInt(c[k], sid) && sid > 0) ownerOfSkill.emplace(static_cast<int>(sid), std::wstring(c[2]));
        }
    });
}

void RefData::AddSkillData(const std::wstring& text) {
    ForEachLine(text, [&](std::wstring_view l) {
        if (l.empty() || l[0] == L'/') return;
        auto c = SplitView(l, L'\t');
        if (c.size() < 6) return;
        long long id = 0;
        if (!ParseInt(c[1], id)) return;
        if (!c[3].empty()) {
            skillCodes.insert(std::wstring(c[3]));
            groupOfCode[std::wstring(c[3])] = std::wstring(c[5]);
            SkillRef sr;
            sr.id = static_cast<int>(id);
            long long org = 0;
            if (c.size() > 6 && ParseInt(c[6], org)) sr.original = static_cast<int>(org);
            sr.group = std::wstring(c[5]);
            sr.name = std::wstring(c[4]);
            skills[std::wstring(c[3])] = std::move(sr);
            skillById[static_cast<int>(id)] = std::wstring(c[3]);
        }
        if (!c[5].empty() && c[5] != L"xxx") skillGroups.insert(std::wstring(c[5]));
    });
}

static std::vector<std::wstring> ListFile(const TextdataGetter& get, const wchar_t* name) {
    std::vector<std::wstring> v;
    Bytes b;
    if (!get(name, b)) return v;
    ForEachLine(DecodeText(b), [&](std::wstring_view l) {
        auto t = Trim(l);
        if (!t.empty() && t[0] != L'/') v.push_back(t);
    });
    return v;
}

static bool LooksPlainTable(const std::wstring& t) {
    // plain textdata starts with a service digit, a comment or a BOM-less row
    size_t i = 0;
    while (i < t.size() && (t[i] == L'\r' || t[i] == L'\n' || t[i] == L' ')) ++i;
    return i < t.size() && ((t[i] >= L'0' && t[i] <= L'9') || t[i] == L'/');
}

std::wstring LoadRefData(RefData& rd, const TextdataGetter& get) {
    std::wstring log;
    int nChar = 0, nSkill = 0;
    for (const auto& f : ListFile(get, L"characterdata.txt")) {
        Bytes b;
        if (get(f, b)) {
            rd.AddCharacterData(DecodeText(b));
            ++nChar;
        }
    }
    // the client loads skilldataenc.txt -> SkillData_*ENC.txt (the plain skilldata_*.txt copies are older and
    // miss rows), so the encrypted files are the reference; plain files only when no ENC index exists
    bool enc = true;
    auto skillList = ListFile(get, L"skilldataenc.txt");
    if (skillList.empty()) {
        enc = false;
        skillList = ListFile(get, L"skilldata.txt");
    }
    for (const auto& f : skillList) {
        Bytes b;
        if (!get(f, b)) continue;
        const std::wstring t = SkillDataText(b);
        if (!LooksPlainTable(t)) continue;
        rd.AddSkillData(t);
        ++nSkill;
    }
    (void)enc;
    log = Fmt(L"characterdata: %d dosya / %zu kod; skilldata: %d dosya / %zu kod", nChar, rd.chars.size(), nSkill,
              rd.skillCodes.size());
    return log;
}

static std::wstring FindTextdataDir(const std::wstring& p) {
    const wchar_t* cands[] = {L"server_dep\\silkroad\\textdata", L"Media\\server_dep\\silkroad\\textdata",
                              L"0Media\\server_dep\\silkroad\\textdata", L"textdata", L"silkroad\\textdata", L""};
    for (const auto* c : cands) {
        const std::wstring d = *c ? JoinPath(p, c) : p;
        if (FileExists(JoinPath(d, L"skilleffect.txt"))) return d;
    }
    return {};
}

static bool IsPk2Path(const std::wstring& p) { return EndsWithI(p, L".pk2"); }

bool OpenSource(const std::wstring& path, const std::string& pk2Key, Bytes& data, SourceInfo& info, RefData& ref,
                std::wstring& err) {
    info = SourceInfo{};
    info.path = path;
    ref.Clear();
    if (IsPk2Path(path) && FileExists(path)) {
        auto pk = std::make_shared<Pk2Reader>();
        if (!pk->Open(path, pk2Key, err)) return false;
        info.kind = SourceInfo::Kind::Pk2;
        info.filePath = path;
        info.inner = std::wstring(kTextdataInner) + L"skilleffect.txt";
        if (!pk->Read(info.inner, data, err)) return false;
        info.refLog = LoadRefData(ref, [pk](const std::wstring& n, Bytes& b) {
            std::wstring e;
            return pk->Read(std::wstring(kTextdataInner) + n, b, e);
        });
        ref.desc = path;
        return true;
    }
    std::wstring file;
    if (DirExists(path)) {
        info.kind = SourceInfo::Kind::Folder;
        info.textdataDir = FindTextdataDir(path);
        if (info.textdataDir.empty()) {
            err = L"Klasörde skilleffect.txt bulunamadı (server_dep\\silkroad\\textdata) / skilleffect.txt not found: " +
                  path;
            return false;
        }
        file = JoinPath(info.textdataDir, L"skilleffect.txt");
    } else if (FileExists(path)) {
        info.kind = SourceInfo::Kind::File;
        file = path;
        info.textdataDir = DirOf(path);
    } else {
        err = L"Bulunamadı / not found: " + path;
        return false;
    }
    info.filePath = file;
    if (!ReadFileBytes(file, data, err)) return false;
    if (FileExists(JoinPath(info.textdataDir, L"characterdata.txt"))) {
        const std::wstring dir = info.textdataDir;
        info.refLog = LoadRefData(ref, [dir](const std::wstring& n, Bytes& b) {
            std::wstring e;
            return FileExists(JoinPath(dir, n)) && ReadFileBytes(JoinPath(dir, n), b, e);
        });
        ref.desc = dir;
    }
    return true;
}

bool OpenRefOnly(const std::wstring& path, const std::string& pk2Key, RefData& ref, std::wstring& err) {
    if (FileExists(path) && !IsPk2Path(path)) {  // a single characterdata_*.txt / skilldata_*.txt (plain)
        std::wstring low = FileNameOf(path);
        for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
        Bytes b;
        if (!ReadFileBytes(path, b, err)) return false;
        const std::wstring t = low.find(L"skilldata") != std::wstring::npos ? SkillDataText(b) : DecodeText(b);
        if (low.find(L"characterdata") != std::wstring::npos) ref.AddCharacterData(t);
        else if (low.find(L"skilldata") != std::wstring::npos && LooksPlainTable(t)) ref.AddSkillData(t);
        else {
            err = L"characterdata/skilldata (düz metin) değil / not a plain characterdata/skilldata file: " + path;
            return false;
        }
        ref.desc += (ref.desc.empty() ? L"" : L" + ") + FileNameOf(path);
        return true;
    }
    if (IsPk2Path(path) && FileExists(path)) {
        auto pk = std::make_shared<Pk2Reader>();
        if (!pk->Open(path, pk2Key, err)) return false;
        LoadRefData(ref, [pk](const std::wstring& n, Bytes& b) {
            std::wstring e;
            return pk->Read(std::wstring(kTextdataInner) + n, b, e);
        });
        ref.desc += (ref.desc.empty() ? L"" : L" + ") + path;
        return true;
    }
    std::wstring dir = DirExists(path) ? path : DirOf(path);
    const wchar_t* cands[] = {L"", L"server_dep\\silkroad\\textdata", L"Media\\server_dep\\silkroad\\textdata",
                              L"textdata"};
    for (const auto* c : cands) {
        const std::wstring d = *c ? JoinPath(dir, c) : dir;
        if (FileExists(JoinPath(d, L"characterdata.txt"))) {
            LoadRefData(ref, [d](const std::wstring& n, Bytes& b) {
                std::wstring e;
                return FileExists(JoinPath(d, n)) && ReadFileBytes(JoinPath(d, n), b, e);
            });
            ref.desc += (ref.desc.empty() ? L"" : L" + ") + d;
            return true;
        }
    }
    err = L"characterdata.txt bulunamadı / not found: " + path;
    return false;
}

}  // namespace se
