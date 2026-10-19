// SkillEffect Studio - editor/EdSchema.cpp
#include "EdSchema.h"

#include "../core/Enums.h"
#include "../core/Text.h"

#include <array>
#include <cwchar>
#include <map>
#include <mutex>

namespace se::ed {

#include "EdColumnsGen.inc"

namespace {

using enums::kActType;
using enums::kAniType;
using enums::kBool;
using enums::kDeadEffect;
using enums::kDmgType;
using enums::kOp;
using enums::kRide;
using enums::kScale;
using enums::kTiming;
using enums::kWaist;

inline constexpr std::wstring_view kTF[] = {L"FALSE", L"TRUE"};
// Int column + enumv = placeholder words the client reads as "no value" (not an error): effectsound Blank / Volume
// hold "-" in shipped rows (CLEAN 23, STANDART 23, DRAMA 23 cells; the effectsound loader 0xA75080 reads them as 0)
inline constexpr std::wstring_view kDashPlaceholder[] = {L"-"};
inline constexpr std::wstring_view kAniGroup[] = {
    L"DEFAULT",       L"SWORD",       L"SPEAR",         L"BOW",           L"ONEHAND_SWORD",   L"TWOHAND_SWORD",
    L"DUAL_AXE",      L"DAGGER",      L"ONEHAND_STAFF", L"TWOHAND_STAFF", L"HARF",            L"AVATAR_NASRUN1",
    L"CROSSBOW",      L"STAFF",       L"BLADE",         L"AXE",           L"HARP",            L"CLERIC_STAFF"};

template <size_t N>
void SetEnum(ColDef& c, const std::wstring_view (&t)[N]) {
    c.type = CT::Enum;
    c.enumv = t;
    c.enumN = N;
}

ColDef Mk(const wchar_t* name, CT t, const wchar_t* tipTr = L"", const wchar_t* tipEn = L"") {
    ColDef c;
    c.name = name;
    c.type = t;
    c.tipTr = tipTr;
    c.tipEn = tipEn;
    c.lo = 1;
    c.hi = 0;  // no range
    return c;
}
ColDef IntC(const wchar_t* name, long long lo, long long hi, const wchar_t* tipTr = L"", const wchar_t* tipEn = L"") {
    ColDef c = Mk(name, CT::Int, tipTr, tipEn);
    c.lo = lo;
    c.hi = hi;
    c.def = lo > 0 ? std::to_wstring(lo) : L"0";
    return c;
}
ColDef PathC(const wchar_t* name, Arc4 arc, const wchar_t* prefix, const wchar_t* ext, const wchar_t* tipTr = L"",
             const wchar_t* tipEn = L"") {
    ColDef c = Mk(name, CT::Path, tipTr, tipEn);
    c.arc = arc;
    c.prefix = prefix;
    c.ext = ext;
    c.def = L"none";
    return c;
}
ColDef CommaC(const wchar_t* name, int n, const wchar_t* def) {
    ColDef c = Mk(name, CT::Comma);
    c.commaN = n;
    c.def = def;
    return c;
}
template <size_t N>
ColDef EnumC(const wchar_t* name, const std::wstring_view (&t)[N], const wchar_t* def, const wchar_t* tipTr = L"",
             const wchar_t* tipEn = L"") {
    ColDef c = Mk(name, CT::Enum, tipTr, tipEn);
    SetEnum(c, t);
    c.def = def;
    return c;
}

void Finish(Schema& s) {
    for (size_t i = 0; i < s.cols.size(); ++i) {
        const auto f = s.cols[i].flags;
        if ((f & CF_Key) && s.keyCol < 0) s.keyCol = static_cast<int>(i);
        if ((f & CF_Id) && s.idCol < 0) s.idCol = static_cast<int>(i);
        if ((f & CF_Label) && s.labelCol < 0) s.labelCol = static_cast<int>(i);
        if (s.cols[i].def.empty()) {
            switch (s.cols[i].type) {
                case CT::Int: case CT::Real: s.cols[i].def = L"0"; break;
                case CT::Str: case CT::Code: s.cols[i].def = L"xxx"; break;
                case CT::Enum: s.cols[i].def = s.cols[i].enumN ? std::wstring(s.cols[i].enumv[0]) : L"none"; break;
                case CT::Empty: break;
                default: s.cols[i].def = L"none"; break;
            }
        }
    }
}

template <size_t N>
void FromGen(Schema& s, const ColGen (&g)[N]) {
    for (const auto& c : g) {
        ColDef d;
        d.name = c.name;
        switch (c.t) {
            case SqlT::I32: d = IntC(c.name, -2147483647LL - 1, 2147483647LL); break;
            case SqlT::U8: d = IntC(c.name, 0, 255); break;
            case SqlT::I16: d = IntC(c.name, -32768, 32767); break;
            case SqlT::I64: d = IntC(c.name, -9223372036854775807LL, 9223372036854775807LL); break;
            case SqlT::Real: d = Mk(c.name, CT::Real); d.def = L"0.0"; break;
            case SqlT::Str: d = Mk(c.name, CT::Str); d.maxLen = c.maxLen; break;
        }
        d.def = d.type == CT::Str ? L"xxx" : d.def;
        s.cols.push_back(std::move(d));
    }
    // semantic overrides by name (shared _RefObjCommon part + skill columns)
    for (auto& d : s.cols) {
        const std::wstring& n = d.name;
        if (n == L"Service") {
            d.lo = 0;
            d.hi = 1;
            d.def = L"1";
            d.tipTr = L"1 = canlı satır, 0 = ölü (istemci/sunucu okumaz)";
            d.tipEn = L"1 = live row, 0 = dead";
        } else if (n == L"ID") {
            d.flags |= CF_Id | CF_Required;
            d.lo = 1;
            d.tipTr = L"birincil anahtar; dosya N için N-5000..N-1 aralığı";
            d.tipEn = L"primary key; file N holds N-5000..N-1";
        } else if (n == L"CodeName128" || n == L"Basic_Code") {
            d.type = CT::Code;
            d.flags |= CF_Key | CF_Required;
            d.tipTr = L"benzersiz kod adı";
            d.tipEn = L"unique codename";
        } else if (n == L"ObjName128" || n == L"Basic_Name") {
            d.type = CT::Label;
            d.flags |= CF_Label;
        } else if (n == L"NameStrID128" || n == L"DescStrID128") {
            d.type = CT::Code;
            if (n == L"NameStrID128") d.flags |= CF_NameStr;
            d.tipTr = L"textdata_object / equip&skill satırının StrID'si";
            d.tipEn = L"StrID of a textdata name row";
        } else if (n == L"OrgObjCodeName128" || n == L"Basic_Group" || n == L"UI_SkillName" ||
                   n == L"UI_SkillToolTip" || n == L"UI_SkillToolTip_Desc" || n == L"UI_SkillStudy_Desc") {
            d.type = CT::Code;
            if (n == L"Basic_Group") {
                d.tipTr = L"xxx değilse skilleffect sunum anahtarı";
                d.tipEn = L"presentation key when not xxx";
            }
        } else if (n == L"AssocFileObj128" || n == L"AssocFileDrop128") {
            d.type = CT::Path;
            d.arc = Arc4::Data;
            d.prefix = L"res\\";
            d.ext = L"bsr|cpd";
            d.tipTr = L"Data.pk2\\res\\ altında model (.bsr); çift silah _l/_r; set 'a,b'";
            d.tipEn = L"model under Data.pk2\\res\\";
        } else if (n == L"AssocFileIcon128" || n == L"UI_IconFile") {
            d.type = CT::Path;
            d.arc = Arc4::Media;
            d.prefix = L"icon\\";
            d.ext = L"ddj";
            d.tipTr = L"Media.pk2\\icon\\ altında simge";
            d.tipEn = L"icon under Media.pk2\\icon\\";
        } else if (n.rfind(L"DefaultSkill_", 0) == 0 || n == L"Basic_Original" || n == L"Basic_ChainCode" ||
                   (n.rfind(L"ReqLearn_Skill", 0) == 0 && n.find(L"Level") == std::wstring::npos)) {
            d.flags |= CF_SkillIdRef;
            d.lo = 0;
            d.tipTr = L"skill ID (0 = yok)";
            d.tipEn = L"skill ID (0 = none)";
        } else if (n == L"Lvl") {
            d.tipTr = L"seviye";
            d.tipEn = L"level";
        }
    }
}

Schema BuildCharInfo() {
    Schema s;
    s.kind = TKind::SeCharInfo;
    s.nameTr = L"skilleffect › characterInfo";
    s.nameEn = L"skilleffect › characterInfo";
    s.clientCells = 13;
    s.trailingTab = true;
    ColDef k = Mk(L"ResourceFileID", CT::SeKey, L"canavar kodu ya da ÖNEK###~###", L"creature code or PREFIX###~###");
    k.flags = CF_Key | CF_Required | CF_CharCode;
    s.cols.push_back(k);
    ColDef t = Mk(L"ResourceTypeName", CT::Code, L"effectsound nesnesi (sesler); NPC'ler PLAYER", L"effectsound object");
    t.flags = CF_SoundObj | CF_Required;
    s.cols.push_back(t);
    ColDef sz = Mk(L"Size", CT::Real, L"boyut (2.8 Kaplan Kız)", L"size");
    sz.def = L"1";
    s.cols.push_back(sz);
    s.cols.push_back(EnumC(L"RideType", kRide, L"none"));
    s.cols.push_back(PathC(L"Ride", Arc4::Data, L"", L"bsr", L"binek .bsr (res\\ ile tam yol) ya da none", L"ride .bsr or none"));
    s.cols.push_back(PathC(L"DieBsr", Arc4::Data, L"", L"bsr", L"ölüm .bsr (res\\mob\\common\\..._die.bsr)", L"death .bsr"));
    s.cols.push_back(Mk(L"DieEffect", CT::Any));
    s.cols.push_back(Mk(L"DamageBone", CT::Bone));
    s.cols.push_back(CommaC(L"DamagePos", 3, L"0,0,0"));
    s.cols.push_back(PathC(L"BloodType", Arc4::Particles, L"hiteffect\\", L"efp", L"çıplak ad: hiteffect\\<ad>.efp", L"bare name: hiteffect\\<name>.efp"));
    s.cols.push_back(EnumC(L"DeadEffect", kDeadEffect, L"none"));
    ColDef env = Mk(L"Env", CT::Env, L"sayı ya da 'bayrak,dosya.ogg' (bit 2)", L"number or 'flags,file.ogg'");
    env.def = L"0";
    s.cols.push_back(env);
    ColDef env2 = Mk(L"Env2", CT::Env, L"sayı ya da 'bayrak,dosya.ogg' (MOB_RM_ROC: 2,roc_battle.ogg)", L"number or 'flags,file.ogg'");
    env2.def = L"0";
    s.cols.push_back(env2);
    for (int i = 13; i < 16; ++i) {
        ColDef n = Mk(L"Not", CT::Label, L"serbest not (istemci okumaz)", L"free note");
        n.def = L"";
        s.cols.push_back(n);
    }
    Finish(s);
    return s;
}

Schema BuildAniSet() {
    Schema s;
    s.kind = TKind::SeAniSet;
    s.nameTr = L"skilleffect › skillaniset2";
    s.nameEn = s.nameTr;
    s.clientCells = 27;
    s.trailingTab = true;
    s.cols.push_back(IntC(L"Service", 0, 1, L"1 olmalı (0 = anahtar kapalı)", L"must be 1"));
    s.cols.back().def = L"1";
    ColDef lab = Mk(L"SkillName", CT::Label, L"etiket (serbest)", L"label");
    lab.flags = CF_Label;
    s.cols.push_back(lab);
    ColDef k = Mk(L"SkillID", CT::Code, L"sunum anahtarı = Basic_Group (xxx değilse) ya da Basic_Code", L"presentation key");
    k.flags = CF_Key | CF_Required | CF_SkillKey;
    s.cols.push_back(k);
    s.cols.push_back(IntC(L"Priority", -2147483647LL - 1, 2147483647LL));
    s.cols.push_back(EnumC(L"AniTRUE", kTF, L"FALSE"));
    s.cols.push_back(IntC(L"HideWeapon", -2147483647LL - 1, 2147483647LL));
    ColDef g = EnumC(L"AniGroup", kAniGroup, L"DEFAULT");
    s.cols.push_back(g);
    s.cols.push_back(Mk(L"AniReady", CT::AniName));
    s.cols.push_back(Mk(L"AniWait", CT::AniName));
    ColDef shot = Mk(L"AniShot", CT::AniList, L"ANI_* ya da virgüllü liste (rastgele)", L"ANI_* or comma list (random)");
    s.cols.push_back(shot);
    s.cols.push_back(EnumC(L"AniPlayTiming", kTiming, L"none"));
    s.cols.push_back(Mk(L"ActW", CT::Any));
    s.cols.push_back(Mk(L"ActS", CT::Any));
    s.cols.push_back(PathC(L"DefenseEfp", Arc4::Particles, L"", L"efp"));
    s.cols.push_back(PathC(L"DamageEfp", Arc4::Particles, L"", L"efp", L"isabet efekti (Particles.pk2)", L"hit effect"));
    s.cols.push_back(IntC(L"Length", -2147483647LL - 1, 2147483647LL));
    s.cols.push_back(CommaC(L"Color", 4, L"0,0,0,0"));
    s.cols.push_back(EnumC(L"Op", kOp, L"INVSRCALPHA"));
    s.cols.push_back(PathC(L"Texture", Arc4::Particles, L"textures\\", L"ddj"));
    s.cols.push_back(Mk(L"StartTiming", CT::Any));
    s.cols.push_back(PathC(L"ArrowTail", Arc4::Particles, L"", L"efp"));
    s.cols.push_back(PathC(L"ArrowForce", Arc4::Particles, L"", L"efp"));
    ColDef le = Mk(L"LightEffect", CT::Any, L"etkisiz: none yazın", L"inert: write none");
    s.cols.push_back(le);
    s.cols.push_back(PathC(L"SkillObject", Arc4::Particles, L"", L"efp|bsr", L".efp (Particles) ya da res\\...bsr (Data)", L".efp or res\\...bsr"));
    s.cols.push_back(EnumC(L"WaistTwist", kWaist, L"none"));
    s.cols.push_back(IntC(L"IsAttackSkill", 0, 1));
    s.cols.back().def = L"1";
    s.cols.push_back(IntC(L"Blood", 0, 1));
    s.cols.push_back(Mk(L"(boş)", CT::Empty));
    Finish(s);
    return s;
}

Schema BuildEffectSet() {
    Schema s;
    s.kind = TKind::SeEffectSet;
    s.nameTr = L"skilleffect › skilleffectset";
    s.nameEn = s.nameTr;
    s.clientCells = 28;
    s.trailingTab = true;
    s.keyUnique = false;
    ColDef lab = Mk(L"SkillName", CT::Label, L"etiket ('-' devam satırı)", L"label ('-' = continuation)");
    lab.flags = CF_Label;
    lab.def = L"-";
    s.cols.push_back(lab);
    ColDef k = Mk(L"SkillEffectID", CT::Code, L"sunum anahtarı (skillaniset2 satırından SONRA)", L"presentation key");
    k.flags = CF_Key | CF_Required | CF_SkillKey;
    s.cols.push_back(k);
    s.cols.push_back(EnumC(L"AniType", kAniType, L"SHOT", L"faz: READY/WAIT/SHOT/...", L"phase"));
    s.cols.push_back(IntC(L"StartEvent", 0, 1000, L"0 = klip başı, k = klibin k. vuruş olayı", L"0 = clip start, k = k-th hit event"));
    s.cols.push_back(EnumC(L"DMGEvent", kBool, L"FALSE", L"TRUE = hasar burada (yalnız SHOT)", L"TRUE = damage here"));
    ColDef dt = Mk(L"DamageType", CT::EnumPipe);
    dt.enumv = kDmgType;
    dt.enumN = std::size(kDmgType);
    dt.def = L"none";
    s.cols.push_back(dt);
    s.cols.push_back(EnumC(L"Scale", kScale, L"none"));
    for (const wchar_t* n : {L"ID", L"Attach", L"Trade", L"Kill"}) s.cols.push_back(IntC(n, -2147483647LL - 1, 2147483647LL));
    s.cols.push_back(IntC(L"CreateCnt", -2147483647LL - 1, 2147483647LL));
    s.cols.back().def = L"1";
    s.cols.push_back(CommaC(L"Fade", 2, L"0,0"));
    s.cols.push_back(EnumC(L"ActType", kActType, L"AT_ONE_FOLLOW"));
    ColDef mv = Mk(L"MovTypeSpeed", CT::MovSpeed);
    mv.def = L"MOV_NONE,0,0,0";
    s.cols.push_back(mv);
    s.cols.push_back(CommaC(L"Param", 3, L"0,0,0"));
    ColDef ao = Mk(L"ActOption", CT::ActOption);
    ao.def = L"false,0,0,0,0,false";
    s.cols.push_back(ao);
    ColDef dir = Mk(L"ObjPath", CT::Folder, L"Particles klasörü '\\' ile biter; res\\ = Data (uçan .bsr)", L"Particles folder ending in '\\'");
    dir.arc = Arc4::Particles;
    s.cols.push_back(dir);
    ColDef on = PathC(L"ObjName", Arc4::Particles, L"", L"efp|bsr|wav|ddj", L"ObjPath + ObjName", L"ObjPath + ObjName");
    on.dirCol = 17;
    s.cols.push_back(on);
    s.cols.push_back(Mk(L"StartBone", CT::Bone));
    s.cols.push_back(CommaC(L"StartOffset", 3, L"0,0,0"));
    s.cols.push_back(Mk(L"TargetBone", CT::Bone));
    s.cols.push_back(CommaC(L"TargetOffset", 3, L"0,0,0"));
    s.cols.push_back(Mk(L"ObjName2", CT::Any));
    s.cols.push_back(IntC(L"Rotate", -2147483647LL - 1, 2147483647LL));
    s.cols.push_back(Mk(L"Script", CT::Script));
    s.cols.push_back(PathC(L"SndBegin", Arc4::Data, L"prim\\snd\\", L"wav", L"Data.pk2\\prim\\snd\\ göreli", L"relative to prim\\snd\\"));
    s.cols.push_back(PathC(L"SndEnd", Arc4::Data, L"prim\\snd\\", L"wav"));
    s.cols.push_back(Mk(L"(boş)", CT::Empty));
    Finish(s);
    return s;
}

Schema BuildNameTable() {
    Schema s;
    s.kind = TKind::NameTable;
    s.nameTr = L"İsim tablosu (textdata_*)";
    s.nameEn = L"Name table (textdata_*)";
    s.trailingTab = true;
    s.cols.push_back(IntC(L"Service", 0, 1));
    s.cols.back().def = L"1";
    ColDef k = Mk(L"StrID", CT::Code, L"SN_<kod> / SN_<kod>_TT_DESC ...", L"string id");
    k.flags = CF_Key | CF_Required;
    s.cols.push_back(k);
    const wchar_t* names[] = {L"KOR", L"Lang3", L"Lang4", L"Lang5", L"Lang6", L"Lang7", L"ENG", L"VNM",
                              L"Lang10", L"Lang11", L"TUR", L"Lang13", L"ARA", L"Lang15"};
    for (int i = 0; i < 14; ++i) {
        ColDef c = Mk(names[i], CT::Label);
        c.def = (i == 0 || i == 6 || i == 7) ? L"" : L"0";
        if (i == 0) c.flags |= CF_Label;
        s.cols.push_back(c);
    }
    s.cols.push_back(Mk(L"(boş)", CT::Empty));
    Finish(s);
    return s;
}

Schema BuildEffectSound() {
    Schema s;
    s.kind = TKind::EffectSound;
    s.nameTr = L"effectsound.txt (CP949)";
    s.nameEn = s.nameTr;
    s.trailingTab = true;
    s.cp949 = true;
    s.keyUnique = false;
    s.cols.push_back(Mk(L"(boş)", CT::Empty));
    ColDef o = Mk(L"Object", CT::Code, L"characterInfo ResourceTypeName (ya da ITEM)", L"characterInfo ResourceTypeName");
    o.flags = CF_Key | CF_Required | CF_SoundObj;
    s.cols.push_back(o);
    s.cols.push_back(Mk(L"Handle", CT::Code, L"SND_SWING1 / SND_DMG / VOC_MOAN ...", L"sound handle"));
    ColDef sk = Mk(L"SkillID", CT::Code, L"'-' ya da sunum anahtarı", L"'-' or presentation key");
    sk.def = L"-";
    s.cols.push_back(sk);
    for (const wchar_t* n : {L"Event1", L"Event2", L"Event3"}) {
        ColDef e = Mk(n, CT::Any);
        e.def = L"-";
        s.cols.push_back(e);
    }
    ColDef bl = IntC(L"Blank", 1, 0, L"0 / 2 / 3 ya da '-' (anlamı belirsiz)", L"0 / 2 / 3 or '-' (meaning unknown)");
    bl.enumv = kDashPlaceholder;  // '-' = placeholder (shipped rows), see CheckCell CT::Int
    bl.enumN = std::size(kDashPlaceholder);
    s.cols.push_back(bl);
    ColDef f = Mk(L"Folder", CT::Folder, L"Data.pk2\\prim\\snd\\ altında klasör", L"folder under prim\\snd\\");
    f.arc = Arc4::Data;
    f.prefix = L"prim\\snd\\";
    f.def = L"monster\\";
    s.cols.push_back(f);
    ColDef fn = PathC(L"FileName", Arc4::Data, L"prim\\snd\\", L"wav", L"Folder + FileName", L"Folder + FileName");
    fn.dirCol = 8;
    s.cols.push_back(fn);
    ColDef vol = IntC(L"Volume", 0, 100, L"0..100 ya da '-' (varsayılan ses düzeyi)", L"0..100 or '-' (default volume)");
    vol.def = L"80";
    vol.enumv = kDashPlaceholder;
    vol.enumN = std::size(kDashPlaceholder);
    s.cols.push_back(vol);
    ColDef d = Mk(L"Description", CT::Label);
    d.def = L"...";
    s.cols.push_back(d);
    s.cols.push_back(Mk(L"(boş)", CT::Empty));
    Finish(s);
    return s;
}

Schema BuildIndex() {
    Schema s;
    s.kind = TKind::IndexFile;
    s.nameTr = L"Dizin dosyası";
    s.nameEn = L"Index file";
    ColDef c = Mk(L"FileName", CT::Str, L"aynı klasördeki tablo dosyası", L"table file in the same folder");
    c.flags = CF_Key | CF_Required;
    c.ext = L"txt";
    s.cols.push_back(c);
    Finish(s);
    return s;
}

Schema BuildNpcPos() {
    Schema s;
    s.kind = TKind::NpcPos;
    s.nameTr = L"npcpos.txt";
    s.nameEn = s.nameTr;
    s.keyUnique = false;
    ColDef id = IntC(L"RefObjID", 1, 2147483647LL, L"characterdata ID", L"characterdata ID");
    id.flags = CF_Key | CF_Required;
    s.cols.push_back(id);
    s.cols.push_back(IntC(L"RegionID", -32768, 32767));
    for (const wchar_t* n : {L"X", L"Y", L"Z"}) {
        ColDef c = Mk(n, CT::Real);
        c.def = L"0.0";
        s.cols.push_back(c);
    }
    Finish(s);
    return s;
}

Schema BuildGen(TKind k, const wchar_t* tr, const wchar_t* en) {
    Schema s;
    s.kind = k;
    s.nameTr = tr;
    s.nameEn = en;
    if (k == TKind::CharacterData) FromGen(s, kGenCharacterData);
    else if (k == TKind::ItemData) FromGen(s, kGenItemData);
    else FromGen(s, kGenSkillData);
    Finish(s);
    return s;
}

}  // namespace

std::vector<std::wstring> ColDef::Choices() const {
    std::vector<std::wstring> v;
    if (type == CT::Enum || type == CT::EnumPipe)
        for (size_t i = 0; i < enumN; ++i) v.emplace_back(enumv[i]);
    else if (type == CT::BoolTF) v = {L"FALSE", L"TRUE"};
    else if (type == CT::AniName || type == CT::AniList) {
        v = {L"none"};
        for (int i = 1; i <= 9; ++i) v.push_back(L"ANI_ATTACK" + std::to_wstring(i));
        for (int i = 1; i <= 20; ++i) v.push_back(L"ANI_SKILL_" + std::to_wstring(i));
        for (int i = 1; i <= 4; ++i) v.push_back(L"ANI_READY0" + std::to_wstring(i));
        for (int i = 1; i <= 4; ++i) v.push_back(L"ANI_WAIT0" + std::to_wstring(i));
    } else if (type == CT::Script) {
        for (const auto& x : enums::kScript) v.emplace_back(x);
    } else if (type == CT::MovSpeed) {
        for (const auto& x : enums::kMov) v.push_back(std::wstring(x) + L",0,0,0");
    }
    return v;
}

int Schema::Find(std::wstring_view name) const {
    for (size_t i = 0; i < cols.size(); ++i)
        if (cols[i].name.size() == name.size() && _wcsnicmp(cols[i].name.c_str(), name.data(), name.size()) == 0)
            return static_cast<int>(i);
    return -1;
}

const Schema& SchemaFor(TKind k) {
    static std::once_flag once;
    static std::array<Schema, static_cast<size_t>(TKind::Count)> all;
    std::call_once(once, [] {
        auto at = [](TKind t) -> Schema& { return all[static_cast<size_t>(t)]; };
        at(TKind::SeCharInfo) = BuildCharInfo();
        at(TKind::SeAniSet) = BuildAniSet();
        at(TKind::SeEffectSet) = BuildEffectSet();
        at(TKind::CharacterData) = BuildGen(TKind::CharacterData, L"characterdata (canavar/NPC/COS)", L"characterdata");
        at(TKind::ItemData) = BuildGen(TKind::ItemData, L"itemdata (eşya)", L"itemdata");
        at(TKind::SkillData) = BuildGen(TKind::SkillData, L"skilldata (ENC = istemcinin okuduğu)", L"skilldata (ENC = client)");
        at(TKind::NameTable) = BuildNameTable();
        at(TKind::EffectSound) = BuildEffectSound();
        at(TKind::IndexFile) = BuildIndex();
        at(TKind::NpcPos) = BuildNpcPos();
        Schema se;
        se.kind = TKind::SeFile;
        se.nameTr = L"skilleffect.txt";
        se.nameEn = L"skilleffect.txt";
        at(TKind::SeFile) = se;
        Schema g;
        g.kind = TKind::Generic;
        g.nameTr = L"Genel tablo";
        g.nameEn = L"Generic table";
        at(TKind::Generic) = g;
        at(TKind::Unknown) = g;
    });
    return all[static_cast<size_t>(k)];
}

TKind KindFromName(std::wstring_view fn) {
    const size_t sl = fn.find_last_of(L"\\/");
    std::wstring n = ToUpper(sl == std::wstring_view::npos ? fn : fn.substr(sl + 1));
    auto starts = [&](const wchar_t* p) { return n.rfind(p, 0) == 0; };
    if (n == L"SKILLEFFECT.TXT") return TKind::SeFile;
    if (n == L"CHARACTERDATA.TXT" || n == L"ITEMDATA.TXT" || n == L"SKILLDATAENC.TXT" || n == L"SKILLDATA.TXT" ||
        n == L"TEXTDATANAME.TXT" || n == L"TEXTQUEST.TXT")
        return TKind::IndexFile;
    if (starts(L"CHARACTERDATA_")) return TKind::CharacterData;
    if (starts(L"ITEMDATA_")) return TKind::ItemData;
    if (starts(L"SKILLDATA_") && n != L"SKILLDATA_VIRTUAL.TXT") return TKind::SkillData;
    if (starts(L"TEXTDATA_") || starts(L"TEXTQUEST_") || n == L"TEXTUISYSTEM.TXT" || n == L"TEXTZONENAME.TXT" ||
        n == L"TEXTHELP.TXT" || n == L"TEXTEVENT.TXT")
        return TKind::NameTable;
    if (n == L"EFFECTSOUND.TXT") return TKind::EffectSound;
    if (n == L"NPCPOS.TXT") return TKind::NpcPos;
    if (n.size() > 4 && n.substr(n.size() - 4) == L".TXT") return TKind::Generic;
    return TKind::Unknown;
}

const wchar_t* KindName(TKind k, bool en) {
    const Schema& s = SchemaFor(k);
    return en ? s.nameEn.c_str() : s.nameTr.c_str();
}

TKind SectionKind(std::wstring_view t) {
    if (t.size() < 9 || t.substr(0, 8) != L"#section") return TKind::Unknown;
    std::wstring_view r = t.substr(8);
    while (!r.empty() && (r.front() == L'\t' || r.front() == L' ')) r.remove_prefix(1);
    while (!r.empty() && (r.back() == L'\t' || r.back() == L' ' || r.back() == L'\r')) r.remove_suffix(1);
    if (r == L"characterInfo") return TKind::SeCharInfo;
    if (r == L"skillaniset2") return TKind::SeAniSet;
    if (r == L"skilleffectset") return TKind::SeEffectSet;
    return TKind::Unknown;
}

Schema InferSchema(const std::vector<std::vector<std::wstring>>& rows, const std::wstring& name) {
    Schema s;
    s.kind = TKind::Generic;
    s.nameTr = name;
    s.nameEn = name;
    size_t n = 0;
    for (const auto& r : rows) n = std::max(n, r.size());
    for (size_t c = 0; c < n; ++c) {
        int ints = 0, reals = 0, other = 0, empty = 0;
        for (const auto& r : rows) {
            if (c >= r.size() || r[c].empty()) {
                ++empty;
                continue;
            }
            long long v = 0;
            double d = 0;
            if (ParseInt(r[c], v)) ++ints;
            else if (ParseFloat(r[c], d)) ++reals;
            else ++other;
        }
        ColDef d;
        d.name = L"C" + std::to_wstring(c);
        d.lo = 1;
        d.hi = 0;
        if (other == 0 && reals == 0 && ints > 0) d.type = CT::Int, d.def = L"0";
        else if (other == 0 && reals + ints > 0) d.type = CT::Real, d.def = L"0";
        else if (other + ints + reals == 0) d.type = CT::Empty;
        else d.type = CT::Any, d.def = L"xxx";
        s.cols.push_back(std::move(d));
    }
    if (!s.cols.empty() && s.cols.back().type == CT::Empty) s.trailingTab = true;
    return s;
}

namespace {
struct RoleTok {
    const wchar_t* tok;
    int id;
};
const RoleTok kRoles[] = {
    {L"ANI_STAND1", 0},    {L"ANI_WALK", 1},       {L"ANI_ATTACK1", 2},   {L"ANI_DAMAGE1", 3},   {L"ANI_DIE1", 4},
    {L"ANI_ATTACK2", 5},   {L"ANI_ATTREADY", 6},   {L"ANI_RUN", 7},       {L"ANI_DAMAGE2", 9},   {L"ANI_SIT_DOWN", 13},
    {L"ANI_SIT", 14},      {L"ANI_STAND_UP", 15},  {L"ANI_ATTACK3", 16},  {L"ANI_ATTACK4", 17},  {L"ANI_DEFENCE", 22},
    {L"ANI_WALK_BACK", 23}, {L"ANI_TURN_L", 24},   {L"ANI_TURN_R", 25},   {L"ANI_DIE1_RM", 36},  {L"ANI_PICK", 38},
    {L"ANI_POSE", 60},     {L"ANI_STAND3", 61},    {L"ANI_DOWN", 62},     {L"ANI_DOWN_RM", 63},  {L"ANI_DOWN_DAMAGE", 64},
    {L"ANI_DOWN_UP", 65},  {L"ANI_DOWN_DIE", 66},  {L"ANI_HELP", 67},     {L"ANI_FIND", 78},     {L"ANI_STUN", 79},
    {L"ANI_STAND4", 81},   {L"ANI_REVIVAL", 121},  {L"ANI_STAND2", 122},  {L"ANI_ATTACK5", 183}, {L"ANI_ATTACK6", 184},
    {L"ANI_ATTACK7", 185}, {L"ANI_ATTACK8", 186},  {L"ANI_HAMMER", 187},  {L"ANI_HANDLOOF", 188}, {L"ANI_TROW", 189},
    {L"ANI_ATTACK9", 190}, {L"ANI_SHOT", 191},
};
}  // namespace

int AniRoleId(std::wstring_view t) {
    for (const auto& r : kRoles)
        if (t == r.tok) return r.id;
    auto num = [&](std::wstring_view p, int& v) {
        if (t.substr(0, p.size()) != p) return false;
        long long x = 0;
        if (!ParseInt(t.substr(p.size()), x)) return false;
        v = static_cast<int>(x);
        return true;
    };
    int v = 0;
    if (num(L"ANI_SKILL_", v)) {
        if (v >= 1 && v <= 10) return 25 + v;
        if (v >= 11 && v <= 20) return 57 + v;
        if (v >= 21 && v <= 40) return 80 + v;
        if (v >= 41 && v <= 100) return 82 + v;
    }
    if (num(L"ANI_ATTACK", v) && v >= 10 && v <= 16) return 182 + v;
    if (num(L"ANI_READY0", v) && v >= 1 && v <= 5) return 39 + v;
    if (num(L"ANI_WAIT0", v) && v >= 1 && v <= 5) return 90 + v;
    if (num(L"ANI_EMOTION", v) && v >= 1 && v <= 10) return 49 + v;
    return -1;
}

std::wstring AniRoleName(int id) {
    for (const auto& r : kRoles)
        if (r.id == id) return r.tok;
    if (id >= 26 && id <= 35) return L"ANI_SKILL_" + std::to_wstring(id - 25);
    if (id >= 68 && id <= 77) return L"ANI_SKILL_" + std::to_wstring(id - 57);
    if (id >= 101 && id <= 120) return L"ANI_SKILL_" + std::to_wstring(id - 80);
    if (id >= 123 && id <= 182) return L"ANI_SKILL_" + std::to_wstring(id - 82);
    if (id >= 192 && id <= 198) return L"ANI_ATTACK" + std::to_wstring(id - 182);
    if (id >= 40 && id <= 44) return L"ANI_READY0" + std::to_wstring(id - 39);
    if (id >= 91 && id <= 95) return L"ANI_WAIT0" + std::to_wstring(id - 90);
    if (id >= 50 && id <= 59) return L"ANI_EMOTION" + std::wstring(id - 49 < 10 ? L"0" : L"") + std::to_wstring(id - 49);
    return L"role " + std::to_wstring(id);
}

}  // namespace se::ed
