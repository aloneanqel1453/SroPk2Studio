// SkillEffect Studio v3 - client/Catalog.cpp
#include "Catalog.h"

#include "../core/SeModel.h"
#include "../core/SkillCrypt.h"
#include "../core/Table.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>

namespace se::client {

namespace {

std::wstring Upper(std::wstring_view s) {
    std::wstring r(s);
    if (!r.empty()) CharUpperBuffW(r.data(), static_cast<DWORD>(r.size()));
    return r;
}
std::wstring LowerW(std::wstring_view s) {
    std::wstring r(s);
    if (!r.empty()) CharLowerBuffW(r.data(), static_cast<DWORD>(r.size()));
    return r;
}

bool IsXxx(const std::wstring& s) { return s.empty() || s == L"xxx" || s == L"XXX" || s == L"0"; }

// a name cell that carries no text: "0", "xxx", only question marks (text lost to a wrong code page)
std::wstring NameCell(const std::vector<std::wstring>& c, int i) {
    if (i < 0 || static_cast<size_t>(i) >= c.size()) return {};
    std::wstring v = Trim(c[static_cast<size_t>(i)]);
    if (IsXxx(v)) return {};
    bool onlyQ = true;
    for (wchar_t ch : v)
        if (ch != L'?' && ch != L' ') onlyQ = false;
    return onlyQ ? std::wstring() : v;
}

int I(const std::vector<std::wstring>& c, int i) {
    if (i < 0 || static_cast<size_t>(i) >= c.size()) return 0;
    long long v = 0;
    if (!ParseInt(Trim(c[static_cast<size_t>(i)]), v)) {
        double d = 0;
        if (ParseFloat(Trim(c[static_cast<size_t>(i)]), d)) return static_cast<int>(d);
        return 0;
    }
    return static_cast<int>(v);
}
std::wstring S(const std::vector<std::wstring>& c, int i) {
    if (i < 0 || static_cast<size_t>(i) >= c.size()) return {};
    return Trim(c[static_cast<size_t>(i)]);
}
float F(const std::vector<std::wstring>& c, int i, float def) {
    double d = 0;
    if (i < 0 || static_cast<size_t>(i) >= c.size() || !ParseFloat(Trim(c[static_cast<size_t>(i)]), d)) return def;
    return static_cast<float>(d);
}

bool IsCodeLike(const std::wstring& s) {
    if (s.size() < 3 || s.find(L'_') == std::wstring::npos) return false;
    for (wchar_t ch : s)
        if (!((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || ch == L'_' || (ch >= L'a' && ch <= L'z'))) return false;
    return true;
}

// column index of the code (canonical canon) in the first data rows -> offset
int DetectOffset(const TableDoc& d, int canon, bool skill) {
    std::map<int, int> votes;
    int seen = 0;
    for (size_t i = 0; i < d.Count() && seen < 200; ++i) {
        if (!d.IsData(i)) continue;
        const auto c = d.Cells(i);
        if (c.size() < 4) continue;
        ++seen;
        for (int k = 0; k < static_cast<int>(c.size()) && k < canon + 6; ++k) {
            const std::wstring v = Trim(c[static_cast<size_t>(k)]);
            const bool ok = skill ? (v.find(L"SKILL_") != std::wstring::npos && IsCodeLike(v)) : IsCodeLike(v);
            if (ok) {
                long long n = 0;
                if (k > 0 && ParseInt(Trim(c[static_cast<size_t>(k - 1)]), n)) {
                    ++votes[k];
                    break;
                }
            }
        }
    }
    int best = canon, bv = -1;
    for (const auto& kv : votes)
        if (kv.second > bv) {
            bv = kv.second;
            best = kv.first;
        }
    return best - canon;
}

bool LoadDoc(const Vfs& vfs, const std::wstring& name, bool skillEnc, TableDoc& d, std::wstring& why) {
    Bytes b;
    if (!vfs.ReadTextdata(name, b)) {
        why = L"okunamadı: " + name;
        return false;
    }
    if (skillEnc && IsSkillEnc(b)) {
        Bytes plain;
        if (!SkillDecrypt(b, plain)) {
            why = L"şifre çözülemedi: " + name;
            return false;
        }
        b.swap(plain);
    }
    std::wstring e;
    if (!d.Load(b, e)) {
        why = name + L": " + e;
        return false;
    }
    return true;
}

ObjKind KindOf(const int t[4], const std::wstring& code) {
    if (t[0] == 3) return ObjKind::Item;
    if (t[0] == 1) {
        if (t[1] == 1) return ObjKind::Player;
        if (t[1] == 2) {
            if (t[2] == 1) return ObjKind::Monster;
            if (t[2] == 2) return ObjKind::Npc;
            if (t[2] == 3) return ObjKind::Cos;
            if (t[2] == 4 || t[2] == 5) return ObjKind::Structure;
        }
    }
    if (StartsWith(code, L"MOB_")) return ObjKind::Monster;
    if (StartsWith(code, L"NPC_")) return ObjKind::Npc;
    if (StartsWith(code, L"COS_")) return ObjKind::Cos;
    if (StartsWith(code, L"CHAR_")) return ObjKind::Player;
    if (StartsWith(code, L"STRUCTURE_")) return ObjKind::Structure;
    if (StartsWith(code, L"ITEM_")) return ObjKind::Item;
    return ObjKind::OtherChar;
}

bool IsTurkishLetter(wchar_t c) {
    return c == L'ğ' || c == L'Ğ' || c == L'ş' || c == L'Ş' || c == L'ı' || c == L'İ';
}
bool IsHangul(wchar_t c) { return c >= 0xAC00 && c <= 0xD7A3; }

// run fn(i) for i in [0, n) on up to 4 threads
void ParallelFor(size_t n, const std::function<void(size_t)>& fn) {
    const size_t threads = std::min<size_t>(n, 4);
    if (threads <= 1) {
        for (size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (size_t t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (size_t i = next++; i < n; i = next++) fn(i);
        });
    for (auto& th : pool) th.join();
}

}  // namespace

const wchar_t* ObjKindTr(ObjKind k) {
    switch (k) {
        case ObjKind::Player: return L"Karakter";
        case ObjKind::Monster: return L"Canavar";
        case ObjKind::Npc: return L"NPC";
        case ObjKind::Cos: return L"Pet / binek";
        case ObjKind::Structure: return L"Yapı";
        case ObjKind::OtherChar: return L"Diğer";
        case ObjKind::Item: return L"Eşya";
    }
    return L"?";
}

const wchar_t* ObjKindEn(ObjKind k) {
    switch (k) {
        case ObjKind::Player: return L"Character";
        case ObjKind::Monster: return L"Monster";
        case ObjKind::Npc: return L"NPC";
        case ObjKind::Cos: return L"Pet / mount";
        case ObjKind::Structure: return L"Structure";
        case ObjKind::OtherChar: return L"Other";
        case ObjKind::Item: return L"Item";
    }
    return L"?";
}

const std::wstring& Names::Best(bool english) const {
    if (english) {
        if (!en.empty()) return en;
        if (!tr.empty()) return tr;
    } else {
        if (!tr.empty()) return tr;
        if (!en.empty()) return en;
    }
    return kr;
}

void Catalog::Clear() {
    m_objs.clear();
    m_skills.clear();
    m_objById.clear();
    m_skillById.clear();
    m_objByCode.clear();
    m_skillByCode.clear();
    m_names.clear();
    m_users.clear();
    m_aniKeys.clear();
    m_ciKeys.clear();
    m_effectRows.clear();
    m_nameCols.clear();
    m_info = ClientInfo{};
}

bool Catalog::Load(const Vfs& vfs, const CatalogOptions& opt, std::vector<Problem>* problems) {
    Clear();
    std::mutex pmx;
    auto prob = [&](Problem::Level lv, const wchar_t* code, std::wstring tr, std::wstring en) {
        if (!problems) return;
        std::lock_guard<std::mutex> lk(pmx);
        problems->push_back({lv, code, std::move(tr), std::move(en)});
    };
    auto cancelled = [&] { return opt.cancel && opt.cancel->load(); };
    auto progress = [&](int p, const std::wstring& w) {
        if (opt.progress) opt.progress(p, w);
    };
    m_info = ReadClientInfo(vfs);
    const int gameOff = opt.gameNameOffset == -2 ? m_info.gameNameOffset : opt.gameNameOffset;

    // ---------------------------------------------------------------- objects (characterdata + itemdata)
    struct Job {
        std::wstring file;
        bool item;
    };
    std::vector<Job> jobs;
    std::vector<Problem> tmp;
    for (const auto& f : vfs.TextdataParts(L"characterdata", &tmp)) jobs.push_back({f, false});
    const size_t charFiles = jobs.size();
    if (opt.items)
        for (const auto& f : vfs.TextdataParts(L"itemdata", &tmp)) jobs.push_back({f, true});
    if (problems)
        for (auto& p : tmp) problems->push_back(std::move(p));
    if (charFiles == 0)
        prob(Problem::Error, L"NO_CHARACTERDATA",
             L"characterdata bulunamadı: canavar / karakter / pet listesi boş kalır (Media.pk2 textdata klasöründe "
             L"characterdata.txt ya da CharacterData_*.txt yok).",
             L"No characterdata: the creature list stays empty.");
    progress(2, L"Nesne tabloları okunuyor");
    std::vector<std::vector<CatObject>> parts(jobs.size());
    std::atomic<int> doneJobs{0};
    ParallelFor(jobs.size(), [&](size_t ji) {
        if (cancelled()) return;
        TableDoc d;
        std::wstring why;
        if (!LoadDoc(vfs, jobs[ji].file, false, d, why)) {
            prob(Problem::Warn, L"TABLE_READ", L"Tablo okunamadı: " + why, L"Table not readable: " + jobs[ji].file);
            return;
        }
        const int off = DetectOffset(d, 2, false);
        auto& out = parts[ji];
        for (size_t i = 0; i < d.Count(); ++i) {
            if (!d.IsData(i)) continue;
            const auto c = d.Cells(i);
            if (c.size() < static_cast<size_t>(57 + off)) continue;
            CatObject o;
            o.id = I(c, 1 + off);
            o.code = S(c, 2 + off);
            if (o.id <= 0 || o.code.empty()) continue;
            o.service = I(c, 0 + off) != 0;
            const std::wstring org = S(c, 4 + off);
            o.orgCode = IsXxx(org) ? L"" : org;
            const std::wstring nk = S(c, 5 + off), dk = S(c, 6 + off);
            o.nameKey = IsXxx(nk) ? L"" : nk;
            o.descKey = IsXxx(dk) ? L"" : dk;
            for (int t = 0; t < 4; ++t) o.type[t] = I(c, 9 + off + t);
            o.country = I(c, 14 + off);
            o.rarity = I(c, 15 + off);
            o.scale = F(c, 48 + off, 100.f);
            const std::wstring m = S(c, 52 + off), dm = S(c, 53 + off), ic = S(c, 54 + off);
            if (!IsXxx(m)) o.model = Vfs::Norm(L"res\\" + m);
            if (!IsXxx(dm)) o.dropModel = Vfs::Norm(L"res\\" + dm);
            if (!IsXxx(ic)) o.icon = Vfs::Norm(L"icon\\" + ic);
            o.kind = KindOf(o.type, o.code);
            if (jobs[ji].item) {
                o.kind = ObjKind::Item;
                o.level = I(c, 33 + off);
            } else {
                o.level = I(c, 57 + off);
                for (int k = 0; k < 10; ++k) {
                    const int sid = I(c, 83 + off + k);
                    if (sid > 0) o.skills.push_back(sid);
                }
            }
            o.file = jobs[ji].file;
            out.push_back(std::move(o));
        }
        const int dj = ++doneJobs;
        progress(2 + dj * 40 / static_cast<int>(std::max<size_t>(1, jobs.size())), L"Okundu: " + jobs[ji].file);
    });
    if (cancelled()) return false;
    int dupObj = 0;
    std::wstring firstDup;
    for (auto& p : parts)
        for (auto& o : p) {
            const std::wstring uc = Upper(o.code);
            if (m_objById.count(o.id) || m_objByCode.count(uc)) {  // first definition wins (like the client's map insert)
                if (!dupObj++) firstDup = o.code + Fmt(L" (%d)", o.id);
                continue;
            }
            m_objById[o.id] = m_objs.size();
            m_objByCode[uc] = m_objs.size();
            m_objs.push_back(std::move(o));
        }
    // clones (xxx look) take model / icon / scale from OrgObjCodeName
    for (auto& o : m_objs) {
        if (o.orgCode.empty() || (!o.model.empty() && !o.icon.empty())) continue;
        const CatObject* src = ObjectByCode(o.orgCode);
        for (int guard = 0; src && guard < 8 && src->model.empty() && !src->orgCode.empty(); ++guard)
            src = ObjectByCode(src->orgCode);
        if (!src) continue;
        if (o.model.empty()) o.model = src->model;
        if (o.icon.empty()) o.icon = src->icon;
    }
    for (const auto& o : m_objs)
        for (int s : o.skills) m_users[s].push_back(o.id);

    // ---------------------------------------------------------------- skills (ENC first: the files the client loads)
    progress(45, L"Skill tabloları okunuyor");
    std::vector<Problem> tmp2;
    std::vector<std::wstring> sfiles = vfs.TextdataParts(L"skilldataenc", &tmp2);
    bool enc = true;
    if (sfiles.empty()) {
        sfiles = vfs.TextdataParts(L"skilldata", &tmp2);
        enc = false;
        if (!sfiles.empty())
            prob(Problem::Info, L"SKILL_PLAIN",
                 L"skilldataenc.txt yok: düz skilldata dosyaları okunuyor (bu istemci şifresiz skill tablosu kullanıyor).",
                 L"No skilldataenc.txt: reading the plain skilldata files.");
    }
    if (problems)
        for (auto& p : tmp2) problems->push_back(std::move(p));
    if (sfiles.empty())
        prob(Problem::Error, L"NO_SKILLDATA", L"skilldata bulunamadı: skill listesi boş kalır (skilldataenc.txt / skilldata.txt yok).",
             L"No skilldata: the skill list stays empty.");
    std::vector<std::vector<CatSkill>> sparts(sfiles.size());
    ParallelFor(sfiles.size(), [&](size_t fi) {
        if (cancelled()) return;
        TableDoc d;
        std::wstring why;
        if (!LoadDoc(vfs, sfiles[fi], true, d, why)) {
            prob(Problem::Warn, L"TABLE_READ", L"Skill tablosu okunamadı: " + why, L"Skill table not readable: " + sfiles[fi]);
            return;
        }
        const int off = DetectOffset(d, 3, true);
        auto& out = sparts[fi];
        for (size_t i = 0; i < d.Count(); ++i) {
            if (!d.IsData(i)) continue;
            const auto c = d.Cells(i);
            if (c.size() < static_cast<size_t>(68 + off)) continue;
            CatSkill s;
            s.id = I(c, 1 + off);
            s.code = S(c, 3 + off);
            if (s.id <= 0 || s.code.empty()) continue;
            s.service = I(c, 0 + off) != 0;
            s.groupId = I(c, 2 + off);
            s.group = S(c, 5 + off);
            s.original = I(c, 6 + off);
            s.level = I(c, 7 + off);
            s.activity = I(c, 8 + off);
            s.chain = I(c, 9 + off);
            s.prepare = I(c, 11 + off);
            s.cast = I(c, 12 + off);
            s.duration = I(c, 13 + off);
            s.reuse = I(c, 14 + off);
            s.cool = I(c, 15 + off);
            s.flying = I(c, 16 + off);
            s.range = I(c, 21 + off);
            s.mastery = I(c, 34 + off);
            s.race = I(c, 47 + off);
            s.reqWeapon[0] = I(c, 50 + off);
            s.reqWeapon[1] = I(c, 51 + off);
            const std::wstring ic = S(c, 61 + off), nk = S(c, 62 + off), dk = S(c, 64 + off);
            if (!IsXxx(ic)) s.icon = Vfs::Norm(L"icon\\" + ic);
            s.nameKey = IsXxx(nk) ? L"" : nk;
            s.descKey = IsXxx(dk) ? L"" : dk;
            s.attackChance = I(c, 66 + off);
            for (int p = 0; p < 50; ++p) s.params.push_back(I(c, 68 + off + p));
            s.file = sfiles[fi];
            out.push_back(std::move(s));
        }
    });
    if (cancelled()) return false;
    for (auto& p : sparts)
        for (auto& s : p) {
            if (m_skillById.count(s.id)) continue;
            const std::wstring uc = Upper(s.code);
            if (m_skillByCode.count(uc)) continue;
            m_skillById[s.id] = m_skills.size();
            m_skillByCode[uc] = m_skills.size();
            m_skills.push_back(std::move(s));
        }
    if (dupObj)
        prob(Problem::Info, L"DUP_OBJECTS",
             Fmt(L"%d yinelenen nesne satırı (aynı ID ya da kod) atlandı; ilk tanım geçerli. İlki: %s", dupObj, firstDup.c_str()),
             Fmt(L"%d duplicate object rows skipped (first definition wins). First: %s", dupObj, firstDup.c_str()));
    {
        int broken = 0;
        std::wstring first;
        for (const auto& o : m_objs)
            for (int sid : o.skills)
                if (!m_skillById.count(sid)) {
                    if (!broken++) first = o.code + Fmt(L" -> %d", sid);
                }
        if (broken)
            prob(Problem::Info, L"SKILL_REF_MISSING",
                 Fmt(L"%d DefaultSkill bağlantısı skill tablosunda yok (oyunda o skill kullanılmaz). İlki: %s", broken, first.c_str()),
                 Fmt(L"%d DefaultSkill references point to no skill. First: %s", broken, first.c_str()));
    }
    if (!enc && m_skills.empty() && !sfiles.empty())
        prob(Problem::Warn, L"SKILL_EMPTY", L"Skill tablosunda okunabilir satır yok.", L"No readable skill rows.");

    // ---------------------------------------------------------------- names (textdata, language columns per file)
    progress(65, L"Adlar okunuyor");
    std::vector<std::wstring> nfiles;
    {
        std::vector<Problem> tmp3;
        std::vector<std::wstring> bases;
        const auto listed = vfs.TextdataParts(L"textdataname", &tmp3);
        const bool viaIndex = !listed.empty() && !(listed.size() == 1 && LowerW(listed[0]) == L"textdataname.txt");
        if (viaIndex) {
            for (const auto& l : listed) bases.push_back(l.substr(0, l.size() - 4));
        } else {
            bases = {L"textdata_object", L"textdata_equip&skill"};
        }
        if (opt.uiTexts) bases.push_back(L"textuisystem");
        for (const auto& b : bases)
            for (const auto& f : vfs.TextdataParts(b, &tmp3))
                if (std::find_if(nfiles.begin(), nfiles.end(), [&](const std::wstring& x) { return LowerW(x) == LowerW(f); }) ==
                    nfiles.end())
                    nfiles.push_back(f);
        if (problems)
            for (auto& p : tmp3) problems->push_back(std::move(p));
    }
    if (nfiles.empty())
        prob(Problem::Warn, L"NO_NAMES", L"Ad tablosu (textdata_object / textdata_equip&skill) bulunamadı: listede yalnız kodlar görünür.",
             L"No name tables: only codes are shown.");
    struct NamePart {
        NameColumns cols;
        std::vector<std::pair<std::wstring, Names>> rows;
        bool ui = false;
    };
    std::vector<NamePart> nparts(nfiles.size());
    ParallelFor(nfiles.size(), [&](size_t fi) {
        if (cancelled()) return;
        TableDoc d;
        std::wstring why;
        if (!LoadDoc(vfs, nfiles[fi], false, d, why)) {
            prob(Problem::Warn, L"TABLE_READ", L"Ad tablosu okunamadı: " + why, L"Name table not readable: " + nfiles[fi]);
            return;
        }
        NamePart& np = nparts[fi];
        np.ui = LowerW(nfiles[fi]).find(L"textuisystem") != std::wstring::npos;
        np.cols.file = nfiles[fi];
        // key column: most "SN_" / code-like cells among the first rows
        std::map<int, int> kv;
        int sampled = 0;
        std::vector<std::vector<std::wstring>> rows;
        rows.reserve(d.Count());
        for (size_t i = 0; i < d.Count(); ++i) {
            if (!d.IsData(i)) continue;
            rows.push_back(d.Cells(i));
        }
        for (const auto& c : rows) {
            if (sampled++ > 400) break;
            for (int k = 0; k < static_cast<int>(c.size()) && k < 5; ++k) {
                const std::wstring v = Trim(c[static_cast<size_t>(k)]);
                if (StartsWith(v, L"SN_") || (np.ui && IsCodeLike(v) && k > 0)) {
                    ++kv[k];
                    break;
                }
            }
        }
        int keyCol = 1, best = -1;
        for (const auto& x : kv)
            if (x.second > best) {
                best = x.second;
                keyCol = x.first;
            }
        np.cols.keyCol = keyCol;
        // language columns
        std::map<int, int> hangul, ascii, turk, filled;
        for (const auto& c : rows)
            for (int j = keyCol + 1; j < static_cast<int>(c.size()); ++j) {
                const std::wstring v = NameCell(c, j);
                if (v.empty()) continue;
                ++filled[j];
                bool allAscii = true, letter = false, hg = false, tk = false;
                for (wchar_t ch : v) {
                    if (ch > 0x7E) allAscii = false;
                    if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z')) letter = true;
                    if (IsHangul(ch)) hg = true;
                    if (IsTurkishLetter(ch)) tk = true;
                }
                if (hg) ++hangul[j];
                if (allAscii && letter) ++ascii[j];
                if (tk) ++turk[j];
            }
        auto argmax = [](const std::map<int, int>& m, int exclude, int minCount) {
            int b = -1, bv = minCount - 1;
            for (const auto& x : m)
                if (x.first != exclude && x.second > bv) {
                    bv = x.second;
                    b = x.first;
                }
            return b;
        };
        np.cols.rows = static_cast<int>(rows.size());
        // Korean: the column that is mostly Hangul (an English column with a few Korean leftovers must not win)
        {
            std::map<int, int> krCand;
            for (const auto& x : hangul)
                if (x.second * 10 >= filled[x.first] * 3) krCand[x.first] = x.second;
            np.cols.kr = argmax(krCand, -1, 1);
        }
        np.cols.en = argmax(ascii, np.cols.kr, 1);
        {
            // Turkish: a column mostly in Turkish, or one with enough Turkish rows (servers often translate a part only)
            std::map<int, int> trCand;
            const int need = std::max(3, static_cast<int>(rows.size()) / 300);
            for (const auto& x : turk)
                if (x.second >= need || x.second * 2 >= filled[x.first]) trCand[x.first] = x.second;
            np.cols.tr = argmax(trCand, np.cols.kr, 1);
        }
        if (np.cols.tr >= 0) {
            np.cols.trRows = turk[np.cols.tr];
            if (np.cols.tr == np.cols.en) np.cols.turkishInEn = true;
        }
        if (gameOff >= 0) np.cols.game = keyCol + gameOff;
        np.rows.reserve(rows.size());
        for (const auto& c : rows) {
            const std::wstring key = S(c, keyCol);
            if (key.empty()) continue;
            Names n;
            n.kr = NameCell(c, np.cols.kr);
            n.en = NameCell(c, np.cols.en);
            n.tr = NameCell(c, np.cols.tr);
            n.game = NameCell(c, np.cols.game);
            if (np.cols.turkishInEn) {  // the server wrote Turkish into the English column
                n.tr = n.en;
            }
            if (n.Empty()) continue;
            np.rows.push_back({Upper(key), std::move(n)});
        }
    });
    if (cancelled()) return false;
    for (auto& np : nparts) {
        if (np.cols.file.empty()) continue;
        m_nameCols.push_back(np.cols);
        for (auto& r : np.rows) {
            auto it = m_names.find(r.first);
            if (it == m_names.end()) {
                m_names.emplace(std::move(r.first), std::move(r.second));
                continue;
            }
            Names& n = it->second;  // first file wins per language; later files only fill gaps
            if (n.kr.empty()) n.kr = r.second.kr;
            if (n.en.empty()) n.en = r.second.en;
            if (n.tr.empty()) n.tr = r.second.tr;
            if (n.game.empty()) n.game = r.second.game;
        }
        if (np.cols.en < 0 && np.cols.tr < 0 && !np.ui)
            prob(Problem::Warn, L"NAME_COLUMNS", L"Ad sütunu bulunamadı: " + np.cols.file + L" (yalnız kodlar görünür).",
                 L"No name column found in " + np.cols.file);
    }
    for (auto& o : m_objs)
        if (!o.nameKey.empty())
            if (const Names* n = NamesOf(o.nameKey)) o.names = *n;
    for (auto& s : m_skills)
        if (!s.nameKey.empty())
            if (const Names* n = NamesOf(s.nameKey)) s.names = *n;

    // ---------------------------------------------------------------- presentation (skilleffect.txt)
    if (opt.presentation) {
        progress(85, L"skilleffect okunuyor");
        std::vector<Problem> tmp4;
        const auto efiles = vfs.TextdataParts(L"skilleffect", &tmp4);
        if (efiles.empty())
            prob(Problem::Warn, L"NO_SKILLEFFECT", L"skilleffect.txt yok: skill animasyon / efekt bilgisi gösterilemez.",
                 L"No skilleffect.txt: no animation / effect info.");
        for (const auto& f : efiles) {
            Bytes b;
            if (!vfs.ReadTextdata(f, b)) continue;
            SeDoc doc;
            std::wstring e;
            if (!doc.Load(b, e)) {
                prob(Problem::Warn, L"SKILLEFFECT_READ", L"skilleffect okunamadı: " + f + L" " + e, L"skilleffect unreadable: " + f);
                continue;
            }
            for (const auto& r : doc.sim.recs) {
                if (r.header || r.cells.empty()) continue;
                std::wstring key = r.Key();
                if (r.sec == Sec::SA || r.sec == Sec::SE) {
                    if (key.find(L"SKILL") == std::wstring::npos)  // shifted columns (some servers insert leading cells)
                        for (const auto& c : r.cells)
                            if (c.find(L"SKILL_") != std::wstring::npos && IsCodeLike(c)) {
                                key = c;
                                break;
                            }
                }
                if (key.empty()) continue;
                const std::wstring uk = Upper(key);
                if (r.sec == Sec::CI) m_ciKeys.insert(uk);
                else if (r.sec == Sec::SA) m_aniKeys.insert(uk);
                else if (r.sec == Sec::SE) ++m_effectRows[uk];
            }
        }
        if (problems)
            for (auto& p : tmp4) problems->push_back(std::move(p));
        for (auto& o : m_objs) o.charInfo = m_ciKeys.count(Upper(o.code)) > 0;
        for (auto& s : m_skills) {
            s.presKey = PresentationKey(s);
            const std::wstring uk = Upper(s.presKey);
            s.hasAni = m_aniKeys.count(uk) > 0;
            auto it = m_effectRows.find(uk);
            s.effectRows = it == m_effectRows.end() ? 0 : it->second;
        }
    } else {
        for (auto& s : m_skills) s.presKey = PresentationKey(s);
    }
    progress(100, L"Katalog hazır");
    return !m_objs.empty() || !m_skills.empty();
}

int Catalog::CheckFiles(const Vfs& vfs, std::vector<Problem>* problems, const std::atomic<bool>* cancel) {
    int missModel = 0, missIcon = 0, models = 0;
    std::wstring firstMissing;
    for (auto& o : m_objs) {
        if (cancel && cancel->load()) break;
        if (!o.model.empty()) {
            ++models;
            o.modelOk = vfs.Exists(ArcKind::Data, o.model) ? 1 : 0;
            if (!o.modelOk) {
                ++missModel;
                if (firstMissing.empty()) firstMissing = o.code + L" (" + o.model + L")";
            }
        }
        if (!o.icon.empty()) {
            o.iconOk = vfs.Exists(ArcKind::Media, o.icon) ? 1 : 0;
            missIcon += !o.iconOk;
        }
    }
    for (auto& s : m_skills) {
        if (cancel && cancel->load()) break;
        if (!s.icon.empty()) {
            s.iconOk = vfs.Exists(ArcKind::Media, s.icon) ? 1 : 0;
            missIcon += !s.iconOk;
        }
    }
    if (problems && missModel)
        problems->push_back({Problem::Info, L"MODELS_MISSING",
                             Fmt(L"%d / %d nesnenin modeli Data içinde yok (tabloda var, dosyası yok; önizlenemez). İlki: %s",
                                 missModel, models, firstMissing.c_str()),
                             Fmt(L"%d of %d models are missing from Data. First: %s", missModel, models, firstMissing.c_str())});
    if (problems && missIcon)
        problems->push_back({Problem::Info, L"ICONS_MISSING", Fmt(L"%d ikon Media içinde yok.", missIcon),
                             Fmt(L"%d icons are missing from Media.", missIcon)});
    return missModel;
}

// ------------------------------------------------------------------------------------------------ lookups
const CatObject* Catalog::ObjectById(int id) const {
    auto it = m_objById.find(id);
    return it == m_objById.end() ? nullptr : &m_objs[it->second];
}
const CatObject* Catalog::ObjectByCode(std::wstring_view code) const {
    auto it = m_objByCode.find(Upper(Trim(code)));
    return it == m_objByCode.end() ? nullptr : &m_objs[it->second];
}
const CatSkill* Catalog::SkillById(int id) const {
    auto it = m_skillById.find(id);
    return it == m_skillById.end() ? nullptr : &m_skills[it->second];
}
const CatSkill* Catalog::SkillByCode(std::wstring_view code) const {
    auto it = m_skillByCode.find(Upper(Trim(code)));
    return it == m_skillByCode.end() ? nullptr : &m_skills[it->second];
}
const Names* Catalog::NamesOf(std::wstring_view key) const {
    auto it = m_names.find(Upper(Trim(key)));
    return it == m_names.end() ? nullptr : &it->second;
}
std::wstring Catalog::DisplayName(const CatObject& o, bool english) const {
    const std::wstring& n = o.names.Best(english);
    if (!n.empty()) return n;
    if (o.kind == ObjKind::Player) {  // player characters have no name in textdata
        std::wstring who = PlayerName(o.code, english);
        if (!who.empty()) return who;
    }
    return o.code;
}
std::wstring Catalog::PlayerName(std::wstring_view code, bool english) {
    const std::wstring u = Upper(code);
    if (u.rfind(L"CHAR_", 0) != 0) return L"";
    const bool eu = u.rfind(L"CHAR_EU_", 0) == 0, woman = u.find(L"_WOMAN") != std::wstring::npos;
    std::wstring who = english ? std::wstring(eu ? L"European " : L"Chinese ") + (woman ? L"woman" : L"man")
                               : std::wstring(eu ? L"Avrupalı " : L"Çinli ") + (woman ? L"kadın" : L"erkek");
    const size_t at = u.rfind(L'_');
    const std::wstring_view last = std::wstring_view(u).substr(at == std::wstring::npos ? u.size() : at + 1);
    if (!last.empty() && last != L"MAN" && last != L"WOMAN") {  // CHAR_CH_MAN has no job tail (NECROMANCER does)
        std::wstring tail(code.substr(at + 1));
        for (size_t i = 0; i < tail.size(); ++i) tail[i] = i ? static_cast<wchar_t>(towlower(tail[i])) : static_cast<wchar_t>(towupper(tail[i]));
        who += L" – " + tail;
    }
    return who;
}
std::wstring Catalog::DisplayName(const CatSkill& s, bool english) const {
    const std::wstring& n = s.names.Best(english);
    return n.empty() ? s.code : n;
}

bool Catalog::HasAniKey(std::wstring_view key) const { return m_aniKeys.count(Upper(key)) > 0; }
int Catalog::EffectRows(std::wstring_view key) const {
    auto it = m_effectRows.find(Upper(key));
    return it == m_effectRows.end() ? 0 : it->second;
}

std::wstring Catalog::PresentationKey(const CatSkill& s) const {
    const CatSkill* cur = &s;
    for (int depth = 0; depth < 16; ++depth) {
        const std::wstring key0 = IsXxx(cur->group) ? cur->code : cur->group;
        if (cur->original == 0 || HasAniKey(key0)) return key0;
        const CatSkill* o = SkillById(cur->original);
        if (!o || o == cur) return key0;
        cur = o;
    }
    return IsXxx(cur->group) ? cur->code : cur->group;
}

std::vector<int> Catalog::ChainOf(int skillId) const {
    std::vector<int> out;
    int cur = skillId;
    for (int guard = 0; guard < 16 && cur > 0; ++guard) {
        if (std::find(out.begin(), out.end(), cur) != out.end()) break;
        const CatSkill* s = SkillById(cur);
        if (!s) break;
        out.push_back(cur);
        cur = s->chain;
    }
    return out;
}

std::vector<int> Catalog::UsersOf(int skillId) const {
    auto it = m_users.find(skillId);
    return it == m_users.end() ? std::vector<int>{} : it->second;
}

std::vector<int> Catalog::SkillsOf(const CatObject& o) const {
    std::vector<int> out;
    auto add = [&](int id) {
        for (int c : ChainOf(id))
            if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    };
    const CatObject* src = &o;
    if (o.skills.empty() && !o.orgCode.empty())
        if (const CatObject* org = ObjectByCode(o.orgCode)) src = org;
    for (int s : src->skills) add(s);
    if (src->kind == ObjKind::Monster && StartsWith(src->code, L"MOB_")) {
        const std::wstring stem = Upper(src->code.substr(4));
        const std::wstring pre = L"MSKILL_" + stem + L"_";
        std::vector<std::pair<std::wstring, int>> fam;
        for (const auto& kv : m_skillByCode) {
            const std::wstring& uc = kv.first;
            if (uc.compare(0, pre.size(), pre) != 0) continue;
            const CatSkill& s = m_skills[kv.second];
            if (!s.service) continue;
            const std::wstring rest = uc.substr(pre.size());
            const std::wstring tok = rest.substr(0, rest.find(L'_'));
            if (rest.find(L'_') != std::wstring::npos && m_objByCode.count(L"MOB_" + stem + L"_" + tok)) continue;  // another mob
            fam.push_back({uc, s.id});
        }
        std::sort(fam.begin(), fam.end());
        for (const auto& f : fam) add(f.second);
    }
    return out;
}

std::vector<const CatObject*> Catalog::Search(std::wstring_view text, const SearchFilter& f) const {
    const std::wstring q = LowerW(Trim(text));
    long long qid = -1;
    if (!q.empty()) ParseInt(q, qid);
    std::vector<std::pair<int, const CatObject*>> hits;
    for (const auto& o : m_objs) {
        if (!(f.kinds & (1u << static_cast<unsigned>(o.kind)))) continue;
        if (f.onlyService && !o.service) continue;
        if (f.onlyWithModel && o.model.empty()) continue;
        if (f.onlyWithIcon && o.icon.empty()) continue;
        if (f.onlyWithSkills && o.skills.empty()) continue;
        if (o.level < f.minLevel || o.level > f.maxLevel) continue;
        int score = 9;
        if (q.empty()) {
            score = 5;
        } else {
            const std::wstring lc = LowerW(o.code);
            if (qid >= 0 && o.id == qid) score = 0;
            else if (lc == q) score = 0;
            else if (lc.compare(0, q.size(), q) == 0) score = 1;
            else {
                for (const std::wstring* n : {&o.names.tr, &o.names.en, &o.names.kr}) {
                    if (n->empty()) continue;
                    const std::wstring ln = LowerW(*n);
                    if (ln == q) score = std::min(score, 1);
                    else if (ln.compare(0, q.size(), q) == 0) score = std::min(score, 2);
                    else if (ln.find(q) != std::wstring::npos) score = std::min(score, 3);
                }
                if (score == 9 && lc.find(q) != std::wstring::npos) score = 4;
            }
            if (score == 9) continue;
        }
        hits.push_back({score, &o});
    }
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        if (a.second->kind != b.second->kind) return a.second->kind < b.second->kind;
        if (a.second->level != b.second->level) return a.second->level < b.second->level;
        return a.second->code < b.second->code;
    });
    std::vector<const CatObject*> out;
    for (size_t i = 0; i < hits.size() && out.size() < f.maxResults; ++i) out.push_back(hits[i].second);
    return out;
}

std::vector<const CatSkill*> Catalog::SearchSkills(std::wstring_view text, size_t maxResults) const {
    const std::wstring q = LowerW(Trim(text));
    long long qid = -1;
    if (!q.empty()) ParseInt(q, qid);
    std::vector<std::pair<int, const CatSkill*>> hits;
    for (const auto& s : m_skills) {
        int score = 9;
        const std::wstring lc = LowerW(s.code);
        if (q.empty()) score = 5;
        else if ((qid >= 0 && s.id == qid) || lc == q) score = 0;
        else if (lc.compare(0, q.size(), q) == 0) score = 1;
        else {
            for (const std::wstring* n : {&s.names.tr, &s.names.en, &s.names.kr})
                if (!n->empty() && LowerW(*n).find(q) != std::wstring::npos) score = std::min(score, 2);
            if (score == 9 && lc.find(q) != std::wstring::npos) score = 3;
        }
        if (score == 9) continue;
        hits.push_back({score, &s});
    }
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second->code < b.second->code;
    });
    std::vector<const CatSkill*> out;
    for (size_t i = 0; i < hits.size() && out.size() < maxResults; ++i) out.push_back(hits[i].second);
    return out;
}

int Catalog::CountKind(ObjKind k) const {
    int n = 0;
    for (const auto& o : m_objs) n += o.kind == k;
    return n;
}

std::wstring Catalog::Summary(bool english) const {
    std::wstring s;
    for (int k = 0; k < kObjKinds; ++k) {
        const int n = CountKind(static_cast<ObjKind>(k));
        if (!n) continue;
        if (!s.empty()) s += L", ";
        s += std::to_wstring(n) + L" " + (english ? ObjKindEn(static_cast<ObjKind>(k)) : ObjKindTr(static_cast<ObjKind>(k)));
    }
    s += Fmt(english ? L", %zu skills, %zu names" : L", %zu skill, %zu ad", m_skills.size(), m_names.size());
    return s;
}

}  // namespace se::client
