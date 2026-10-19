// SkillEffect Studio - core/Validate.cpp
// Validation rules V1..V18 of SKILLEFFECT_LOGIC.md section 8 that can be checked on text
// (V13/V14/V17 need the .bsr/.bsk files and are reported as "not checked").
#include "Engine.h"

#include "Enums.h"

#include <cwctype>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace se {

namespace {

struct Ctx {
    std::vector<Issue>* out;
    const ListRow* row;
    void Add(Sev s, const wchar_t* rule, std::wstring tr, std::wstring en) const {
        Issue i;
        i.sev = s;
        i.rule = rule;
        i.listLine = row ? row->listLine : -1;
        i.key = row ? row->key : L"";
        i.tr = std::move(tr);
        i.en = std::move(en);
        out->push_back(std::move(i));
    }
    void Col(Sev s, const wchar_t* rule, int col, const std::wstring& v, const wchar_t* tr, const wchar_t* en) const {
        Add(s, rule, Fmt(L"Sütun %d ('%s'): %s", col, v.c_str(), tr), Fmt(L"Column %d ('%s'): %s", col, v.c_str(), en));
    }
};

bool IsNum(std::wstring_view s) {
    double d = 0;
    return ParseFloat(s, d);
}
bool IsIntS(std::wstring_view s) {
    long long v = 0;
    return ParseInt(s, v);
}
bool VecOf(std::wstring_view s, size_t n, bool ints) {
    auto p = SplitView(s, L',');
    if (p.size() != n) return false;
    for (auto& x : p)
        if (ints ? !IsIntS(x) : !IsNum(x)) return false;
    return true;
}
bool IsNone(std::wstring_view s) { return s == L"none"; }
bool ExtIs(std::wstring_view s, std::initializer_list<const wchar_t*> exts) {
    for (auto* e : exts)
        if (EndsWithI(s, e)) return true;
    return false;
}
// a cell that names a file / folder (v5 M3 ENC2): has a path separator or a client file extension
bool LooksLikePath(std::wstring_view s) {
    if (s.find(L'\\') != std::wstring_view::npos) return true;
    for (const wchar_t* e : {L".bsr", L".efp", L".wav", L".ddj", L".bsk", L".bms", L".bmt", L".ban", L".ogg", L".dds", L".txt"})
        if (EndsWithI(s, e)) return true;
    return false;
}
bool BoolWord(std::wstring_view s) { return s == L"true" || s == L"false" || s == L"TRUE" || s == L"FALSE" || s == L"0" || s == L"1"; }

// stem = code without a trailing "_<digits>" (MOB_X_3 / MOB_X_6 -> MOB_X)
std::wstring CodeStem(const std::wstring& c) {
    size_t e = c.size();
    while (e > 0 && iswdigit(c[e - 1])) --e;
    return (e < c.size() && e > 1 && c[e - 1] == L'_') ? c.substr(0, e - 1) : c;
}

// a client characterdata code that differs from `code` only in its "_N" suffix (empty = none)
std::wstring SuffixTwin(const std::wstring& code, const RefData& ref) {
    const std::wstring st = CodeStem(code);
    std::wstring best;
    for (const auto& kv : ref.chars)
        if (kv.first != code && CodeStem(kv.first) == st && (best.empty() || kv.first < best)) best = kv.first;
    return best;
}

void CheckCI(const Ctx& c, const std::vector<std::wstring>& t, const RefData& ref) {
    const auto& k = t[0];
    if (k.find(L'~') != std::wstring::npos) {
        const size_t n = k.size();
        bool ok = n >= 7 && k[n - 4] == L'~';
        long long a = 0, b = 0;
        if (ok) ok = ParseInt(std::wstring_view(k).substr(n - 7, 3), a) && ParseInt(std::wstring_view(k).substr(n - 3), b) && a <= b;
        if (!ok)
            c.Add(Sev::Error, L"V5", L"Aralık anahtarı 'ÖNEK###~###' biçiminde olmalı (3+3 hane, ilk ≤ son).",
                  L"Range key must be 'PREFIX###~###' (3+3 digits, first <= last).");
        else if (ref.HasChars()) {
            const std::wstring pre = k.substr(0, n - 7);
            int miss = 0;
            for (long long i = a; i <= b; ++i)
                if (!ref.Char(pre + Fmt(L"%03lld", i))) ++miss;
            if (miss)
                c.Add(Sev::Warn, L"V5", Fmt(L"Aralıktaki %d kod characterdata'da yok (o seviyeler için satır etkisiz).", miss),
                      Fmt(L"%d codes of the range are not in characterdata (no effect for those).", miss));
        }
    } else if (ref.HasChars()) {
        const CharRef* r = ref.Char(k);
        if (!r) {
            const std::wstring twin = SuffixTwin(k, ref);
            if (!twin.empty())
                c.Add(Sev::Error, L"V5",
                      L"Bu canavar istemcide tanımlı değil (characterdata'sında yok): istemci satırı sessizce atar. İstemcide yalnız son eki farklı " +
                          twin + L" var: dışa aktarmayı bu kodla yapın (skill / effectsound / textdata kodları da aynı son ekle).",
                      L"This creature is not defined in the client (not in its characterdata): the client silently drops the row. The client has " +
                          twin + L" (only the suffix differs): export with that code (skill / effectsound / textdata codes with the same suffix).");
            else
                c.Add(Sev::Error, L"V5", L"Bu canavar istemcide tanımlı değil (characterdata'sında yok): istemci satırı sessizce atar.",
                      L"This creature is not defined in the client (not in its characterdata): the client silently drops the row.");
        }
        else if (!r->service)
            c.Add(Sev::Warn, L"V5", L"characterdata satırı Service 0.", L"characterdata row has Service 0.");
    }
    if (!IsNum(t[2])) c.Col(Sev::Error, L"V10", 2, t[2], L"Size sayı olmalı.", L"Size must be a number.");
    if (!enums::In(enums::kRide, t[3])) c.Col(Sev::Error, L"V10", 3, t[3], L"Ride Type: none / RT_FIXED / RT_DUMMY.", L"Ride Type: none / RT_FIXED / RT_DUMMY.");
    for (int col : {4, 5})
        if (!IsNone(t[static_cast<size_t>(col)]) && !EndsWithI(t[static_cast<size_t>(col)], L".bsr"))
            c.Col(Sev::Warn, L"V10", col, t[static_cast<size_t>(col)], L".bsr yolu ya da none bekleniyor.", L"expected a .bsr path or none.");
    if (!VecOf(t[8], 3, false)) c.Col(Sev::Error, L"V12", 8, t[8], L"DamagePos 'x,y,z' olmalı.", L"DamagePos must be 'x,y,z'.");
    if (!IsNone(t[9]) && (t[9].find(L'\\') != std::wstring::npos || EndsWithI(t[9], L".efp")))
        c.Col(Sev::Warn, L"V10", 9, t[9], L"BloodType yalnız ad (istemci hiteffect\\<ad>.efp kurar).", L"BloodType is a bare name (client builds hiteffect\\<name>.efp).");
    if (!enums::In(enums::kDeadEffect, t[10])) c.Col(Sev::Error, L"V10", 10, t[10], L"Dead Effect: none / DEFFECT_FLASH.", L"Dead Effect: none / DEFFECT_FLASH.");
    {
        auto p = SplitView(t[11], L',');
        long long v = 0;
        if (!ParseInt(p[0], v) || p.size() > 2)
            c.Col(Sev::Error, L"V12", 11, t[11], L"env: sayı ya da 'bayrak,dosya.ogg'.", L"env: number or 'flags,file.ogg'.");
        else if (p.size() == 2 && !(v & 2))
            c.Col(Sev::Warn, L"V12", 11, t[11], L"'.ogg' yalnız bit 1 (2) ile okunur.", L"'.ogg' is read only with bit 1 (2).");
        else if (p.size() == 1 && (v & 2))
            c.Col(Sev::Error, L"V12", 11, t[11], L"bit 1 ayarlı ama ',dosya.ogg' yok.", L"bit 1 set but ',file.ogg' missing.");
    }
    if (!IsIntS(t[12])) c.Col(Sev::Error, L"V10", 12, t[12], L"tam sayı olmalı.", L"must be an integer.");
}

void CheckSA(const Ctx& c, const std::vector<std::wstring>& t, const RefData& ref) {
    if (t[0] == L"0")
        c.Add(Sev::Warn, L"V7", L"Service 0: anahtar devre dışı, skilleffectset satırları düşer.", L"Service 0: key disabled, its skilleffectset rows are dropped.");
    else if (t[0] != L"1")
        c.Col(Sev::Error, L"V7", 0, t[0], L"1 olmalı.", L"must be 1.");
    const auto& k = t[2];
    const bool fixedName = enums::In(enums::kSystemKeys, k);
    const bool effectOnly = StartsWith(k, L"EFFECT_") || StartsWith(k, L"PARAM_");
    if (ref.HasSkills() && !fixedName && !effectOnly) {
        if (ref.skillGroups.count(k)) {
        } else if (ref.skillCodes.count(k)) {
            auto g = ref.groupOfCode.find(k);
            if (g != ref.groupOfCode.end() && g->second != L"xxx" && !g->second.empty() && g->second != k)
                c.Add(Sev::Warn, L"V8", L"Bu skill bir skill grubunun üyesi (" + g->second + L"): anahtar olarak grup adı yazılmalı.",
                      L"This skill is a member of a skill group (" + g->second + L"): the group name must be the key.");
        } else {
            c.Add(Sev::Error, L"V8", L"Bu skill istemcide tanımlı değil (istemcinin skill tablosunda yok): satır hiçbir skile bağlanmaz, efekt oyunda görünmez.",
                  L"This skill is not defined in the client (not in its skill table): the row reaches no skill, the effect never shows in game.");
        }
    }
    if (!IsIntS(t[3])) c.Col(Sev::Error, L"V10", 3, t[3], L"Priority tam sayı.", L"Priority must be an integer.");
    if (t[4] != L"FALSE" && t[4] != L"TRUE") c.Col(Sev::Error, L"V10", 4, t[4], L"AniTRUE: FALSE/TRUE.", L"AniTRUE: FALSE/TRUE.");
    if (!IsIntS(t[5])) c.Col(Sev::Error, L"V10", 5, t[5], L"Hide Weapon tam sayı.", L"Hide Weapon must be an integer.");
    for (int col : {7, 8})
        if (!enums::IsAniName(t[static_cast<size_t>(col)]))
            c.Col(Sev::Error, L"V10", col, t[static_cast<size_t>(col)], L"ANI_* adı ya da none olmalı.", L"must be an ANI_* name or none.");
    for (auto& a : SplitView(t[9], L','))
        if (!enums::IsAniName(a)) {
            c.Col(Sev::Error, L"V10", 9, t[9], L"AniShot: ANI_* adı/listesi ya da none.", L"AniShot: ANI_* name/list or none.");
            break;
        }
    if (!enums::In(enums::kTiming, t[10])) c.Col(Sev::Error, L"V10", 10, t[10], L"before / after / none.", L"before / after / none.");
    for (int col : {13, 14})
        if (!IsNone(t[static_cast<size_t>(col)]) && !EndsWithI(t[static_cast<size_t>(col)], L".efp"))
            c.Col(Sev::Warn, L"V10", col, t[static_cast<size_t>(col)], L".efp yolu ya da none bekleniyor.", L"expected an .efp path or none.");
    if (!IsIntS(t[15])) c.Col(Sev::Error, L"V10", 15, t[15], L"Length tam sayı (boş hücre hatası L2687 gibi?).", L"Length must be an integer (empty-cell shift like L2687?).");
    if (!VecOf(t[16], 4, true)) c.Col(Sev::Error, L"V12", 16, t[16], L"Color 'a,r,g,b' olmalı.", L"Color must be 'a,r,g,b'.");
    if (!enums::In(enums::kOp, t[17])) c.Col(Sev::Error, L"V10", 17, t[17], L"Op: ONE / INVSRCALPHA.", L"Op: ONE / INVSRCALPHA.");
    if (!IsNone(t[22]))
        c.Col(Sev::Warn, L"V10", 22, t[22], L"LightEffect etkisiz (textdata'da light bölümü yok), none yazın.", L"LightEffect is inert (no light section in textdata); write none.");
    if (!enums::In(enums::kWaist, t[24])) c.Col(Sev::Error, L"V10", 24, t[24], L"WaistTwist: none/Roll/Yaw/Pitch/RollR/YawR/PitchR.", L"WaistTwist: none/Roll/Yaw/Pitch/RollR/YawR/PitchR.");
    if (!IsIntS(t[25])) c.Col(Sev::Error, L"V10", 25, t[25], L"IsAttackSkill 0/1.", L"IsAttackSkill 0/1.");
    if (!IsIntS(t[26])) c.Col(Sev::Error, L"V10", 26, t[26], L"Blood 0/1.", L"Blood 0/1.");
    if (StartsWith(k, L"EFFECT_AM_") && !IsNone(t[9]))
        c.Add(Sev::Warn, L"V16", L"EFFECT_AM_ anahtarında AniShot none olmalı.", L"An EFFECT_AM_ key should have AniShot none.");
}

void CheckSE(const Ctx& c, const std::vector<std::wstring>& t) {
    const int at = enums::AniTypeIndex(t[2]);
    if (at < 0) c.Col(Sev::Error, L"V10", 2, t[2], L"AniType: READY/WAIT/SHOT/ACT_OS/ACT_OL/ACT_OE/ACT_S/ACT_L/DEACT/S_RETURN.", L"AniType: READY/WAIT/SHOT/ACT_OS/ACT_OL/ACT_OE/ACT_S/ACT_L/DEACT/S_RETURN.");
    long long ev = 0;
    if (!ParseInt(t[3], ev) || ev < 0) c.Col(Sev::Error, L"V10", 3, t[3], L"StartEvent ≥ 0 tam sayı.", L"StartEvent integer >= 0.");
    const bool dmg = t[4] == L"TRUE" || t[4] == L"true";
    if (t[4] == L"FLASE" || t[4] == L"flase")
        c.Col(Sev::Error, L"V10", 4, t[4], L"yazım hatası (FALSE).", L"typo (FALSE).");
    else if (!enums::In(enums::kBool, t[4]))
        c.Col(Sev::Error, L"V10", 4, t[4], L"DMG Event: TRUE/FALSE.", L"DMG Event: TRUE/FALSE.");
    if (dmg && at >= 0 && !(at < 3 || at == 9))
        c.Add(Sev::Error, L"V11", L"DMG Event TRUE yalnız READY/WAIT/SHOT/S_RETURN'de (assert 'i < 3 || i == 9').",
              L"DMG Event TRUE only on READY/WAIT/SHOT/S_RETURN (assert 'i < 3 || i == 9').");
    else if (dmg && at >= 0 && at != 2)
        c.Add(Sev::Warn, L"V11", L"DMG Event TRUE SHOT dışı bir fazda (sevkiyatta yalnız SHOT).", L"DMG Event TRUE outside SHOT (shipped data: SHOT only).");
    for (auto& f : SplitView(t[5], L'|'))
        if (!enums::In(enums::kDmgType, f)) {
            if (enums::InCI(enums::kDmgType, f))  // 'None' (shipped Kerberos rows): -1 = every flag, harmless
                c.Col(Sev::Warn, L"V10", 5, t[5], L"DamageType büyük/küçük harf farklı: istemci -1 okur (tüm bayraklar). 'none' / NOR|CRI|HWAN yazın.",
                      L"DamageType differs in case: the client reads -1 (every flag). Write 'none' / NOR|CRI|HWAN.");
            else
                c.Col(Sev::Error, L"V10", 5, t[5], L"DamageType: NOR|CRI|HWAN ya da none.", L"DamageType: NOR|CRI|HWAN or none.");
            break;
        }
    if (!enums::In(enums::kScale, t[6])) {
        if (enums::InCI(enums::kScale, t[6]))
            c.Col(Sev::Warn, L"V10", 6, t[6], L"Scale büyük/küçük harf farklı: istemci -1 okur. none / CHAR_BASE / MOB_BASE yazın.",
                  L"Scale differs in case: the client reads -1. Write none / CHAR_BASE / MOB_BASE.");
        else
            c.Col(Sev::Error, L"V10", 6, t[6], L"Scale: none / CHAR_BASE / MOB_BASE.", L"Scale: none / CHAR_BASE / MOB_BASE.");
    }
    for (int col = 7; col <= 11; ++col)
        if (!IsIntS(t[static_cast<size_t>(col)])) c.Col(Sev::Error, L"V10", col, t[static_cast<size_t>(col)], L"tam sayı olmalı.", L"must be an integer.");
    if (!VecOf(t[12], 2, true)) c.Col(Sev::Error, L"V12", 12, t[12], L"Fade 'giriş,çıkış' olmalı.", L"Fade must be 'in,out'.");
    if (!enums::In(enums::kActType, t[13])) c.Col(Sev::Error, L"V10", 13, t[13], L"ActType AT_* olmalı.", L"ActType must be AT_*.");
    {
        auto p = SplitView(t[14], L',');
        bool ok = p.size() == 4 && enums::In(enums::kMov, p[0]);
        for (size_t i = 1; ok && i < 4; ++i) ok = IsIntS(p[i]);
        if (!ok) c.Col(Sev::Error, L"V12", 14, t[14], L"'MOV_x,a,b,c' olmalı.", L"must be 'MOV_x,a,b,c'.");
    }
    if (!VecOf(t[15], 3, false)) c.Col(Sev::Error, L"V12", 15, t[15], L"Param 'a,b,c' olmalı.", L"Param must be 'a,b,c'.");
    {
        auto p = SplitView(t[16], L',');
        bool ok = p.size() == 6 && BoolWord(p[0]) && BoolWord(p[5]);
        for (size_t i = 1; ok && i < 5; ++i) ok = IsNum(p[i]);
        if (!ok) c.Col(Sev::Error, L"V12", 16, t[16], L"ActOption 'bool,a,b,c,d,bool' olmalı.", L"ActOption must be 'bool,a,b,c,d,bool'.");
    }
    if (!IsNone(t[17]) && t[17].back() != L'\\')
        c.Col(Sev::Error, L"V15", 17, t[17], L"ObjPath none ya da '\\' ile biten klasör olmalı.", L"ObjPath must be none or a folder ending in '\\'.");
    if (!IsNone(t[18]) && !ExtIs(t[18], {L".bsr", L".efp", L".wav", L".ddj"}))
        c.Col(Sev::Warn, L"V10", 18, t[18], L"ObjName uzantısı bsr/efp/wav/ddj değil.", L"ObjName extension is not bsr/efp/wav/ddj.");
    if (!IsNone(t[17]) && t[17].size() + t[18].size() + 9 >= 512)
        c.Add(Sev::Error, L"V15", L"Kurulan yol ≥ 512 karakter (wcscat_s).", L"Built path >= 512 characters (wcscat_s).");
    if (!VecOf(t[20], 3, false)) c.Col(Sev::Error, L"V12", 20, t[20], L"StartOffset 'x,y,z' olmalı.", L"StartOffset must be 'x,y,z'.");
    if (!VecOf(t[22], 3, false)) c.Col(Sev::Error, L"V12", 22, t[22], L"TargetOffset 'x,y,z' olmalı.", L"TargetOffset must be 'x,y,z'.");
    if (!IsIntS(t[24])) c.Col(Sev::Error, L"V10", 24, t[24], L"Rotate tam sayı.", L"Rotate must be an integer.");
    {
        auto p = SplitView(t[25], L',');
        bool ok = true;
        for (auto& f : SplitView(p[0], L'|')) ok = ok && enums::In(enums::kScript, f);
        for (size_t i = 1; ok && i < p.size(); ++i) ok = IsNum(p[i]);  // v6 P7/P8: the client's own rows use 'SCT_EFFECT_SCALE,2.0'
        if (!ok) c.Col(Sev::Error, L"V10", 25, t[25], L"Script: none ya da SCT_* (+',sayılar').", L"Script: none or SCT_* (+',numbers').");
    }
    for (int col : {26, 27}) {
        const auto& s = t[static_cast<size_t>(col)];
        if (!IsNone(s) && !EndsWithI(s, L".wav"))
            c.Col(Sev::Warn, L"V10", col, s, L"ses .wav yolu (prim\\snd\\ göreli) ya da none.", L"sound .wav path (relative to prim\\snd\\) or none.");
        if (!IsNone(s) && s.size() + 9 >= 512) c.Add(Sev::Error, L"V15", L"Ses yolu ≥ 512 karakter.", L"Sound path >= 512 characters.");
    }
}

}  // namespace

std::vector<Issue> Engine::Validate(const SeDoc& doc, const ParsedList& list, const RefData& ref, const Plan& plan) const {
    std::vector<Issue> out = list.issues;
    for (const auto& i : plan.issues) out.push_back(i);
    // keys of the file
    std::unordered_set<std::wstring> fileSa, fileCi;
    std::unordered_map<std::wstring, std::vector<int>> fileSeAct;
    for (const auto& r : doc.sim.recs) {
        if (r.header) continue;
        if (r.sec == Sec::SA) fileSa.insert(r.Key());
        if (r.sec == Sec::CI && !r.cells.empty()) fileCi.insert(r.cells[0]);
        if (r.sec == Sec::SE && r.cells.size() > 2) fileSeAct[r.Key()].push_back(enums::AniTypeIndex(r.cells[2]));
    }
    std::unordered_map<std::wstring, int> listSa, listCi;
    std::unordered_map<std::wstring, std::vector<const ListRow*>> listSe;
    for (const auto& r : list.rows) {
        if (r.sec == Sec::SA) listSa[r.key]++;
        if (r.sec == Sec::CI) listCi[r.key]++;
        if (r.sec == Sec::SE) listSe[r.key].push_back(&r);
    }
    for (const auto& r : list.rows) {
        Ctx c{&out, &r};
        // V3 / V4 on the raw text (up to a '//' the client would cut)
        std::wstring_view eff(r.text);
        const size_t sl = eff.find(L'/');
        if (sl != std::wstring_view::npos && sl + 1 < eff.size() && (eff[sl + 1] == L'/' || eff[sl + 1] == L'*')) eff = eff.substr(0, sl);
        else if (sl != std::wstring_view::npos) {
            // the first '/' is not a comment start, so the client reads the whole row; a risk only when the text also
            // holds a '//' or '/*' further on (it becomes the comment start if the first '/' is ever edited away)
            const bool later = eff.find(L"//", sl + 1) != std::wstring_view::npos || eff.find(L"/*", sl + 1) != std::wstring_view::npos;
            if (later)
                c.Add(Sev::Warn, L"V3", L"Hücrede '/' var ve satırda sonra '//' ya da '/*' geçiyor: yollarda '\\' kullanın.",
                      L"A cell contains '/' and the row has a '//' or '/*' later on: use '\\' in paths.");
            else
                c.Add(Sev::Info, L"V3", L"Hücrede '/' var (ör. etiket metninde): istemci satırı tam okur, zararsız. Yollarda '\\' tercih edin.",
                      L"A cell contains '/' (e.g. in a label): the client reads the whole row, harmless. Prefer '\\' in paths.");
        }
        auto raw = SplitView(eff, L'\t');
        while (!raw.empty() && IsBlankClient(raw.back())) raw.pop_back();
        size_t lead = 0;
        while (lead < raw.size() && raw[lead].empty()) ++lead;
        bool emptyCell = false;
        for (size_t i = lead; i < raw.size(); ++i) {
            const auto& cell = raw[i];
            if (IsBlankClient(cell)) {
                if (!emptyCell)
                    c.Add(Sev::Error, L"V4", Fmt(L"Boş hücre (ham sütun %zu): sonraki tüm sütunlar SOLA KAYAR. 'none'/'0' yazın.", i),
                          Fmt(L"Empty cell (raw column %zu): every later column SHIFTS LEFT. Write 'none'/'0'.", i));
                emptyCell = true;
                continue;
            }
            for (wchar_t ch : cell)
                if ((ch >= 1 && ch <= 0x10 && ch != L'\t') || ch == 0) {
                    c.Add(Sev::Error, L"V4", Fmt(L"Hücrede kontrol karakteri U+%04X.", static_cast<unsigned>(ch)),
                          Fmt(L"Control character U+%04X in a cell.", static_cast<unsigned>(ch)));
                    break;
                }
            if (cell.front() == L' ' || cell.back() == L' ')
                c.Add(Sev::Warn, L"V4", Fmt(L"Hücrede baş/son boşluk (ham sütun %zu): sondaki boşluk değere dahil olur.", i),
                      Fmt(L"Leading/trailing space in a cell (raw column %zu): trailing spaces become part of the value.", i));
        }
        if (lead > 0)
            c.Add(Sev::Warn, L"V4", L"Satır TAB ile başlıyor (ilk boş hücre yutulur).", L"Row starts with a TAB (the empty first cell vanishes).");
        for (size_t i = lead; i < raw.size(); ++i) {  // v5 M3: pk2 names are CP949, a path cell must convert exactly
            const auto cell = raw[i];
            size_t bad = 0;
            if (!LooksLikePath(cell) || IsExactIn(949, cell, &bad)) continue;
            c.Add(Sev::Error, L"ENC2",
                  Fmt(L"Ham sütun %zu: ", i) + EncLossText(cell, bad, false) + L". pk2 / istemci dosya adları CP949: bu dosya istemcide bulunamaz.",
                  Fmt(L"Raw column %zu: ", i) + EncLossText(cell, bad, true) + L". pk2 / client file names are CP949: the client cannot find this file.");
        }
        const int want = ExpectedCells(r.sec);
        const auto& t = r.cells;
        const bool countOk = (r.sec == Sec::CI) ? (t.size() == 13 || t.size() == 14) : (static_cast<int>(t.size()) == want);
        if (!countOk) {
            c.Add(Sev::Error, L"V4", Fmt(L"%s satırı %d hücre olmalı, %zu var.", SecName(r.sec), want, t.size()),
                  Fmt(L"A %s row needs %d cells, it has %zu.", SecName(r.sec), want, t.size()));
            continue;
        }
        if (r.sec == Sec::CI) CheckCI(c, t, ref);
        if (r.sec == Sec::SA) CheckSA(c, t, ref);
        if (r.sec == Sec::SE) CheckSE(c, t);
    }
    // list-level rules
    for (const auto& [k, n] : listSa)
        if (n > 1) {
            Issue i;
            i.sev = Sev::Warn;
            i.rule = L"V6";
            i.key = k;
            i.tr = Fmt(L"skillaniset2 anahtarı listede %d kez: son satır kazanır.", n);
            i.en = Fmt(L"skillaniset2 key %d times in the list: the last row wins.", n);
            out.push_back(i);
        }
    for (const auto& [k, n] : listCi)
        if (n > 1) {
            Issue i;
            i.sev = Sev::Warn;
            i.rule = L"V6";
            i.key = k;
            i.tr = Fmt(L"characterInfo anahtarı listede %d kez: son satır kazanır.", n);
            i.en = Fmt(L"characterInfo key %d times in the list: the last row wins.", n);
            out.push_back(i);
        }
    for (const auto& [k, rows] : listSe) {
        Ctx c{&out, rows.front()};
        const bool effectOnly = StartsWith(k, L"EFFECT_") || StartsWith(k, L"PARAM_") || enums::In(enums::kSystemKeys, k);
        if (!listSa.count(k) && !fileSa.count(k) && !effectOnly)
            c.Add(Sev::Warn, L"V9", L"Bu efektin animasyon satırı (skillaniset2) yok, ne listede ne istemcide: efekt satırları skile bağlanmaz.",
                  L"This effect has no animation row (skillaniset2), neither in the list nor in the client: the effect rows do not reach the skill.");
        int lastPh = -1;
        long long lastEv = -1;
        for (const auto* r : rows) {
            if (r->cells.size() < 4) continue;
            const int ph = enums::AniTypeIndex(r->cells[2]);
            long long ev = 0;
            ParseInt(r->cells[3], ev);
            if (ph >= 0 && ph < lastPh) {
                Ctx(c.out, r).Add(Sev::Info, L"P2", L"Faz sırası READY<WAIT<SHOT<ACT_* değil (gelenek, istemci dosya sırasıyla okur).",
                                   L"Phase order is not READY<WAIT<SHOT<ACT_* (convention; the client keeps file order).");
                break;
            }
            if (ph == 2 && lastPh == 2 && ev < lastEv)
                Ctx(c.out, r).Add(Sev::Info, L"P2", L"SHOT StartEvent azalıyor (gelenek: artan).", L"SHOT StartEvent decreases (convention: ascending).");
            lastPh = ph;
            lastEv = ev;
        }
        if (StartsWith(k, L"EFFECT_AM_")) {
            const std::wstring ci = k.substr(10);
            if (!listCi.count(ci) && !fileCi.count(ci))
                c.Add(Sev::Warn, L"V16", L"EFFECT_AM_ anahtarı bir characterInfo sütun 0 metniyle eşleşmiyor: aura bağlanmaz.",
                      L"EFFECT_AM_ key does not match a characterInfo column 0 text: the aura is not linked.");
        }
    }
    for (const auto& [k, n] : listSa) {
        (void)n;
        if (!StartsWith(k, L"EFFECT_AM_")) continue;
        bool actL = false;
        if (auto it = listSe.find(k); it != listSe.end())
            for (const auto* r : it->second) actL = actL || (r->cells.size() > 2 && r->cells[2] == L"ACT_L");
        if (auto it = fileSeAct.find(k); it != fileSeAct.end())
            for (int ph : it->second) actL = actL || ph == 7;
        if (!actL) {
            Issue i;
            i.sev = Sev::Warn;
            i.rule = L"V16";
            i.key = k;
            i.tr = L"EFFECT_AM_ anahtarının ACT_L satırı yok: aura görünmez.";
            i.en = L"EFFECT_AM_ key has no ACT_L row: no visible aura.";
            out.push_back(i);
        }
    }
    {
        Issue i;
        i.sev = Sev::Info;
        i.rule = L"-";
        if (!ref.HasChars() || !ref.HasSkills()) {
            i.tr = std::wstring(L"Referans eksik: ") + (ref.HasChars() ? L"" : L"characterdata (V5 + seviye sırası) ") +
                   (ref.HasSkills() ? L"" : L"skilldata (V8) ") + L"- Media klasörü/pk2 açarak yükleyin.";
            i.en = std::wstring(L"Reference missing: ") + (ref.HasChars() ? L"" : L"characterdata (V5 + level order) ") +
                   (ref.HasSkills() ? L"" : L"skilldata (V8) ") + L"- open a Media folder/pk2 to load it.";
            out.push_back(i);
        }
        i.tr = L"Model dosyalarının içi (.bsr olayları, .bsk kemikleri, animasyon başına en çok 15 ses) metinden denetlenemez: oyunda deneyin.";
        i.en = L"The inside of the model files (.bsr events, .bsk bones, at most 15 sounds per animation) cannot be checked from text: test in game.";
        out.push_back(i);
    }
    return out;
}

std::vector<Issue> Engine::FileHealth(const SeDoc& doc) const {
    std::vector<Issue> v;
    auto add = [&](Sev s, const wchar_t* rule, int line, std::wstring tr, std::wstring en) {
        Issue i;
        i.sev = s;
        i.rule = rule;
        i.fileLine = line;
        i.tr = std::move(tr);
        i.en = std::move(en);
        v.push_back(std::move(i));
    };
    if (!doc.hasBom)
        add(Sev::Error, L"V1", 1, L"BOM (FF FE) yok: istemci ilk 2 baytı körlemesine atlar, ilk karakter kaybolur.",
            L"No BOM (FF FE): the client skips the first 2 bytes blindly, the first character is lost.");
    if (doc.countLf || doc.countCr)
        add(Sev::Warn, L"V1", -1, Fmt(L"Karışık satır sonu: CRLF %d, LF %d, CR %d.", doc.countCrlf, doc.countLf, doc.countCr),
            Fmt(L"Mixed line ends: CRLF %d, LF %d, CR %d.", doc.countCrlf, doc.countLf, doc.countCr));
    if (!doc.finalTerm) add(Sev::Info, L"V1", static_cast<int>(doc.lines.size()), L"Son satırda satır sonu yok.", L"Last line has no line end.");
    if (doc.sim.eofInBlock) {
        int open = -1;
        for (size_t i = doc.lines.size(); i-- > 0;)
            if (doc.sim.blockAfter[i] && (i == 0 || !doc.sim.blockAfter[i - 1])) {
                open = static_cast<int>(i);
                break;
            }
        add(Sev::Info, L"V2", open + 1,
            Fmt(L"İstemcinin dosyası kapanmamış bir /* yorumuyla bitiyor (satır %d): dosyanın SONUNA eklenen her şey okunmaz. Araç yeni satırları okunan kısma koyar.", open + 1),
            Fmt(L"The client's file ends inside an unclosed /* comment (line %d): anything appended at the END is not read. The tool puts new rows in the read part.", open + 1));
    }
    {  // data-looking rows hidden by /* */ (e.g. rows appended after an unclosed block)
        int hidden = 0, firstHidden = -1;
        for (size_t i = 0; i < doc.lines.size(); ++i) {
            if (!doc.sim.dropped[i]) continue;
            const auto l = doc.Line(i);
            size_t a = 0;
            while (a < l.size() && IsClientWs(l[a])) ++a;
            if (StartsWith(l.substr(a), L"//")) continue;
            if (ClientTokens(l).size() >= 10) {
                if (firstHidden < 0) firstHidden = static_cast<int>(i) + 1;
                ++hidden;
            }
        }
        if (hidden)
            add(Sev::Info, L"V2", firstHidden,
                Fmt(L"İstemcinin dosyasında %d veri satırı yorum içinde kalmış, oyun bunları okumaz (ilki satır %d). Oyunla ya da önceki bir düzenlemeyle gelmiş.", hidden, firstHidden),
                Fmt(L"%d data rows of the client's file are inside comments, the game does not read them (first: line %d). Shipped or from an earlier edit.", hidden, firstHidden));
    }
    {  // v5 M5: S1 section header traps, S2 data before the first header, S3 lone CR / NUL
        const auto& lk = doc.sim.lineKind;
        auto kindAt = [&](size_t i) { return i < lk.size() ? static_cast<LineKind>(lk[i]) : LineKind::Blank; };
        int shown = 0, total = 0;
        for (size_t i = 0; i < lk.size(); ++i) {
            if (kindAt(i) != LineKind::BadHeader) continue;
            ++total;
            if (shown >= 20) continue;
            ++shown;
            int rows = 0;
            for (size_t j = i + 1; j < lk.size(); ++j) {
                const auto k = kindAt(j);
                if (k == LineKind::Header || k == LineKind::BadHeader) break;
                if (k == LineKind::Misrouted || k == LineKind::OutOfSection) ++rows;
            }
            Sec kept = Sec::None;  // the section the client stays in
            for (size_t j = i; j-- > 0;)
                if (kindAt(j) == LineKind::Header) {
                    const auto th = ClientTokens(doc.Line(j));
                    if (th.size() > 1) kept = SecFromName(th[1]);
                    break;
                }
            const auto t = ClientTokens(doc.Line(i));
            const std::wstring tok0 = t.empty() ? std::wstring() : t[0];
            std::wstring name = tok0 == L"#section" ? (t.size() > 1 ? t[1] : std::wstring()) : Trim(std::wstring_view(tok0).substr(std::min<size_t>(8, tok0.size())));
            std::wstring fix;  // the exact header the user most likely meant
            {
                std::wstring want = Trim(name), lw;
                for (wchar_t ch : want) lw += static_cast<wchar_t>(towlower(ch));
                for (const wchar_t* k : {L"characterInfo", L"skillaniset2", L"skilleffectset", L"skillaniset", L"light", L"env"}) {
                    std::wstring kl;
                    for (const wchar_t* q = k; *q; ++q) kl += static_cast<wchar_t>(towlower(*q));
                    if (kl == lw) {
                        fix = std::wstring(L"#section<TAB>") + k;
                        break;
                    }
                }
            }
            const std::wstring keptTr = kept == Sec::None ? std::wstring(L"yok: önceki geçerli başlık yok, satırlar hiç okunmaz")
                                                          : std::wstring(SecName(kept)) + L": satırlar o bölümün işleyicisine gider";
            const std::wstring keptEn = kept == Sec::None ? std::wstring(L"none: no valid header before, the rows are not read at all")
                                                          : std::wstring(SecName(kept)) + L": the rows go to that section's handler";
            std::wstring tr, en;
            if (tok0 == L"#section") {
                tr = Fmt(L"'#section %s': istemcinin tanıdığı bölüm adı değil (tam yazım, büyük/küçük harf duyarlı: characterInfo / skillaniset2 / "
                         L"skilleffectset). İstemci önceki bölümde kalır (%s); sonraki %d satır amaçlandığı gibi okunmaz.",
                         name.empty() ? L"(ad yok)" : name.c_str(), keptTr.c_str(), rows);
                en = Fmt(L"'#section %s': not a section name the client knows (exact, case-sensitive: characterInfo / skillaniset2 / "
                         L"skilleffectset). The client stays in the previous section (%s); the next %d rows are not read as meant.",
                         name.empty() ? L"(no name)" : name.c_str(), keptEn.c_str(), rows);
            } else {
                tr = Fmt(L"'%s': istemci için başlık değil (ilk hücre tam '#section' olmalı, küçük harf, ad TAB ile ayrılır): satır veri sayılır, "
                         L"bölüm değişmez (%s); sonraki %d satır amaçlandığı gibi okunmaz.",
                         tok0.c_str(), keptTr.c_str(), rows);
                en = Fmt(L"'%s': not a header for the client (the first cell must be exactly '#section', lower case, the name after a TAB): the "
                         L"line is a data row, the section does not change (%s); the next %d rows are not read as meant.",
                         tok0.c_str(), keptEn.c_str(), rows);
            }
            if (!fix.empty()) {
                tr += L" Doğrusu: '" + fix + L"'.";
                en += L" Correct: '" + fix + L"'.";
            }
            add(Sev::Error, L"S1", static_cast<int>(i) + 1, std::move(tr), std::move(en));
        }
        if (total > shown)
            add(Sev::Error, L"S1", -1, Fmt(L"%d hatalı bölüm başlığı daha.", total - shown), Fmt(L"%d more bad section headers.", total - shown));
        if (doc.sim.outOfSection > 0) {
            int first = -1;
            for (size_t i = 0; i < lk.size() && first < 0; ++i)
                if (kindAt(i) == LineKind::OutOfSection) first = static_cast<int>(i) + 1;
            add(Sev::Warn, L"S2", first,
                Fmt(L"İlk geçerli '#section' başlığından önce %d veri satırı var (ilki satır %d): istemci bunları okumaz (R9).", doc.sim.outOfSection, first),
                Fmt(L"%d data rows come before the first valid '#section' header (first: line %d): the client does not read them (R9).",
                    doc.sim.outOfSection, first));
        }
        if (!doc.sim.nulLines.empty()) {
            int split = 0, firstSplit = -1;
            for (int ln : doc.sim.nulLines) {
                const auto l = doc.Line(static_cast<size_t>(ln));
                const size_t z = l.find(L'\0');
                if (z != std::wstring_view::npos && !IsBlankClient(l.substr(z + 1).substr(0, l.substr(z + 1).find(L'\0')))) {
                    ++split;
                    if (firstSplit < 0) firstSplit = ln + 1;
                }
            }
            const int first = doc.sim.nulLines.front() + 1;
            add(split ? Sev::Error : Sev::Warn, L"S3", split ? firstSplit : first,
                Fmt(L"%zu satırda NUL (U+0000) var (ilki satır %d): istemci satırı orada bitirir. %d satırda NUL'dan sonra metin var: düzenleyicide "
                    L"TEK satır görünen yer istemci için İKİ satırdır (birleşik satır), sütunlar kayar. NUL'u silin.",
                    doc.sim.nulLines.size(), first, split),
                Fmt(L"%zu lines hold a NUL (U+0000) (first: line %d): the client ends the line there. %d of them have text after the NUL: "
                    L"what an editor shows as ONE line is TWO lines for the client (joined lines), the columns shift. Delete the NUL.",
                    doc.sim.nulLines.size(), first, split));
        }
        if (doc.countCr > 0) {
            int joined = 0, first = -1;
            for (size_t i = 0; i + 1 < doc.lines.size(); ++i)
                if (doc.lines[i].term == 3 && !IsBlankClient(doc.Line(i)) && !IsBlankClient(doc.Line(i + 1))) {
                    ++joined;
                    if (first < 0) first = static_cast<int>(i) + 1;
                }
            if (joined)
                add(Sev::Warn, L"S3", first,
                    Fmt(L"%d yerde tek CR (LF'siz) satır sonu var (ilki satır %d): istemci satırı orada böler, ama yalnız CRLF tanıyan düzenleyiciler "
                        L"iki satırı TEK satır (birleşik) gösterir; düzenlerken satırlar karışabilir. CRLF yazın.", joined, first),
                    Fmt(L"%d lone CR line ends (no LF) (first: line %d): the client splits the line there, but editors that only know CRLF show "
                        L"the two lines as ONE (joined); edits can mix them up. Write CRLF.", joined, first));
        }
    }
    int bad = 0, firstBad = -1;
    for (const auto& r : doc.sim.recs) {
        if (r.header) continue;
        const int want = ExpectedCells(r.sec);
        const int n = static_cast<int>(r.cells.size());
        const bool ok = r.sec == Sec::CI ? (n == 13 || n == 14) : (want == 0 || n == want);
        if (!ok) {
            if (firstBad < 0) firstBad = r.line + 1;
            ++bad;
        }
    }
    if (bad)
        add(Sev::Info, L"V4", firstBad, Fmt(L"İstemcinin dosyasında sütun sayısı yanlış %d satır var (ilki satır %d); oyunla gelmiş, dokunulmadı.", bad, firstBad),
            Fmt(L"%d rows of the client's file have a wrong column count (first: line %d); shipped, left untouched.", bad, firstBad));
    add(Sev::Info, L"-", -1,
        Fmt(L"İstemcinin skilleffect.txt dosyası: characterInfo %d, skillaniset2 %d, skilleffectset %d okunan satır; toplam %zu satır.", doc.sim.counts[1], doc.sim.counts[3],
            doc.sim.counts[4], doc.lines.size()),
        Fmt(L"Active records: characterInfo %d, skillaniset2 %d, skilleffectset %d; %zu physical lines.", doc.sim.counts[1],
            doc.sim.counts[3], doc.sim.counts[4], doc.lines.size()));
    return v;
}

// Doctor: rows the client cannot see. (1) keys whose rows are ALL inside /* */ (no active row of the key in that
// section): the client never reads them (Warn in the block left open at the end of the file - the "appended at the
// end" mistake, Info in closed blocks - usually switched off on purpose). (2) active characterInfo rows whose code is
// not in the client's characterdata: the reader looks the code up and silently drops the row (0xA9AE42).
std::vector<Issue> Engine::Visibility(const SeDoc& doc, const RefData& ref, Unseen* counts) const {
    std::vector<Issue> v;
    Unseen u;
    std::unordered_set<std::wstring> act[kSecCount];
    for (const auto& r : doc.sim.recs)
        if (!r.header && r.sec != Sec::None) act[static_cast<int>(r.sec)].insert(r.Key());
    const size_t n = doc.lines.size();
    size_t eofStart = n;
    if (doc.sim.eofInBlock)
        for (size_t i = n; i > 0; --i) {
            if (!doc.sim.blockAfter[i - 1]) break;
            eofStart = i - 1;
        }
    struct Hit {
        std::wstring key;
        int line = 0;
        Sec sec = Sec::None;
    };
    std::vector<Hit> eofKeys, blockKeys;
    std::unordered_set<std::wstring> seen;
    Sec cur = Sec::None;
    const auto& lk = doc.sim.lineKind;
    for (size_t i = 0; i < n; ++i) {
        if (i < lk.size() && static_cast<LineKind>(lk[i]) == LineKind::Header) {
            const auto t = ClientTokens(doc.Line(i));
            cur = t.size() > 1 ? SecFromName(t[1]) : Sec::None;
            continue;
        }
        if (!doc.sim.dropped[i] || (cur != Sec::CI && cur != Sec::SA && cur != Sec::SE)) continue;
        const auto tok = ClientTokens(doc.Line(i));
        if (tok.size() < 10 || StartsWith(tok[0], L"#") || StartsWith(tok[0], L"/")) continue;
        const size_t kc = static_cast<size_t>(KeyColumn(cur));
        if (kc >= tok.size() || tok[kc].empty() || act[static_cast<int>(cur)].count(tok[kc])) continue;
        if (!seen.insert(std::wstring(SecName(cur)) + L"|" + tok[kc]).second) continue;
        (i >= eofStart ? eofKeys : blockKeys).push_back(Hit{tok[kc], static_cast<int>(i) + 1, cur});
    }
    auto names = [](const std::vector<Hit>& h) {
        std::wstring s;
        for (size_t i = 0; i < h.size() && i < 8; ++i) s += (i ? L", " : L"") + h[i].key;
        if (h.size() > 8) s += Fmt(L" (+%zu)", h.size() - 8);
        return s;
    };
    auto add = [&](Sev sev, const wchar_t* rule, int line, const std::wstring& key, std::wstring tr, std::wstring en) {
        Issue i;
        i.sev = sev;
        i.rule = rule;
        i.fileLine = line;
        i.key = key;
        i.tr = std::move(tr);
        i.en = std::move(en);
        v.push_back(std::move(i));
    };
    if (!eofKeys.empty())
        add(Sev::Warn, L"V2", eofKeys.front().line, eofKeys.front().key,
            Fmt(L"%zu anahtarın satırları yalnız dosya sonundaki kapanmamış /* bloğunda (satır %d'den beri): istemci bu anahtarları hiç görmez. "
                L"Satırları birleştirme ile okunan kısma taşıyın. Anahtarlar: %s",
                eofKeys.size(), static_cast<int>(eofStart) + 1, names(eofKeys).c_str()),
            Fmt(L"The rows of %zu keys are only in the /* block left open at the end of the file (since line %d): the client never sees "
                L"these keys. Move the rows into the read part with a merge. Keys: %s",
                eofKeys.size(), static_cast<int>(eofStart) + 1, names(eofKeys).c_str()));
    if (!blockKeys.empty())
        add(Sev::Info, L"V2", blockKeys.front().line, blockKeys.front().key,
            Fmt(L"%zu anahtarın satırları yalnız /* */ yorum bloklarında: istemci bu anahtarları görmez (çoğu bilerek kapatılmıştır). Anahtarlar: %s",
                blockKeys.size(), names(blockKeys).c_str()),
            Fmt(L"The rows of %zu keys are only inside /* */ comment blocks: the client does not see these keys (mostly switched off on "
                L"purpose). Keys: %s",
                blockKeys.size(), names(blockKeys).c_str()));
    if (ref.HasChars()) {
        auto stemOf = [](const std::wstring& c) { return CodeStem(c); };
        std::unordered_map<std::wstring, std::wstring> byStem;  // stem -> one client code
        for (const auto& [code, cr] : ref.chars) {
            (void)cr;
            auto& slot = byStem[stemOf(code)];
            if (slot.empty() || code < slot) slot = code;
        }
        std::vector<Hit> drop, near;
        std::vector<std::wstring> nearTo;
        for (const auto& r : doc.sim.recs) {
            if (r.header || r.sec != Sec::CI) continue;
            const auto& k = r.Key();
            if (k.empty() || k.find(L'~') != std::wstring::npos || ref.Char(k)) continue;
            drop.push_back(Hit{k, r.line + 1, Sec::CI});
            const auto st = byStem.find(stemOf(k));
            if (st != byStem.end() && st->second != k) {
                near.push_back(drop.back());
                nearTo.push_back(st->second);
            }
        }
        u.ciDropped = static_cast<int>(drop.size());
        // a code that differs from a client code only in its "_N" suffix (an export whose code drifted, e.g. _6 vs the
        // DB's _3; the shipped file has such leftovers too: COS_C_DRACO_5, STRUCTURE_*_05) - one row each, with the client's code
        for (size_t i = 0; i < near.size() && i < 50; ++i)
            add(Sev::Info, L"V5", near[i].line, near[i].key,
                Fmt(L"Bu characterInfo satırının kodu istemcinin characterdata'sında yok, istemci satırı sessizce atar; characterdata'da "
                    L"yalnız son eki farklı %s var. Kodu (ve skill / effectsound / textdata kodlarını) istemcideki kodla aynı yazın.",
                    nearTo[i].c_str()),
                Fmt(L"This characterInfo row's code is not in the client's characterdata, the client silently drops the row; characterdata "
                    L"has %s (only the suffix differs). Use the client's code (in the skill / effectsound / textdata rows too).",
                    nearTo[i].c_str()));
        if (drop.size() > near.size()) {
            std::vector<Hit> rest;
            for (const auto& h : drop) {
                bool isNear = false;
                for (const auto& x : near) isNear = isNear || x.line == h.line;
                if (!isNear) rest.push_back(h);
            }
            add(Sev::Info, L"V5", rest.front().line, rest.front().key,
                Fmt(L"%zu characterInfo satırının kodu istemcinin characterdata'sında yok: istemci bu satırları sessizce atar (çoğu oyunla "
                    L"gelmiş eski satırlardır). Kodlar: %s",
                    rest.size(), names(rest).c_str()),
                Fmt(L"%zu characterInfo rows have a code that is not in the client's characterdata: the client silently drops them (mostly "
                    L"old shipped rows). Codes: %s",
                    rest.size(), names(rest).c_str()));
        }
        u.ciNear = static_cast<int>(near.size());
    }
    u.hiddenKeysEof = static_cast<int>(eofKeys.size());
    u.hiddenKeys = static_cast<int>(eofKeys.size() + blockKeys.size());
    if (counts) *counts = u;
    return v;
}

}  // namespace se
