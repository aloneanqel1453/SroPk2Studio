// SkillEffect Studio - skillplay/FxDb.cpp
// math, client enum tables, row parsers (client tokens), FxDb (client rows + pending rows), effectsound, skilldata.
#include "SkillPlay.h"

#include "../core/List.h"
#include "../core/SeModel.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace se::sp {

// ------------------------------------------------------------------------------------------------------- math
double Vec3::Len() const { return std::sqrt(Dot(*this)); }
Vec3 Vec3::Norm() const {
    const double l = Len();
    return l > 1e-12 ? Vec3{x / l, y / l, z / l} : Vec3{};
}
Mat4 Mat4::operator*(const Mat4& b) const {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}
Mat4 Mat4::RotOnly() const {
    Mat4 r = *this;
    r.SetT({});
    return r;
}
Mat4 Mat4::Translation(const Vec3& t) {
    Mat4 r;
    r.SetT(t);
    return r;
}
Mat4 Mat4::RotY(double a) {  // D3DXMatrixRotationY
    Mat4 r;
    const double c = std::cos(a), s = std::sin(a);
    r.m[0][0] = c;
    r.m[0][2] = -s;
    r.m[2][0] = s;
    r.m[2][2] = c;
    return r;
}
Mat4 Mat4::Scale(double s) {
    Mat4 r;
    r.m[0][0] = r.m[1][1] = r.m[2][2] = s;
    return r;
}
Mat4 Mat4::LookAlongNegZ(const Vec3& dir) {
    Mat4 r;
    const Vec3 z = (dir * -1.0).Norm();
    if (z.Zero()) return r;
    Vec3 up{0, 1, 0};
    if (std::fabs(z.Dot(up)) > 0.999) up = {1, 0, 0};
    const Vec3 x = up.Cross(z).Norm();
    const Vec3 y = z.Cross(x);
    const Vec3 rows[3] = {x, y, z};
    for (int i = 0; i < 3; ++i) {
        r.m[i][0] = rows[i].x;
        r.m[i][1] = rows[i].y;
        r.m[i][2] = rows[i].z;
    }
    return r;
}
Vec3 XfPoint(const Vec3& v, const Mat4& M) {
    return {v.x * M.m[0][0] + v.y * M.m[1][0] + v.z * M.m[2][0] + M.m[3][0],
            v.x * M.m[0][1] + v.y * M.m[1][1] + v.z * M.m[2][1] + M.m[3][1],
            v.x * M.m[0][2] + v.y * M.m[1][2] + v.z * M.m[2][2] + M.m[3][2]};
}
Vec3 XfDir(const Vec3& v, const Mat4& M) {
    return {v.x * M.m[0][0] + v.y * M.m[1][0] + v.z * M.m[2][0], v.x * M.m[0][1] + v.y * M.m[1][1] + v.z * M.m[2][1],
            v.x * M.m[0][2] + v.y * M.m[1][2] + v.z * M.m[2][2]};
}

// ------------------------------------------------------------------------------------------------- enums
namespace {

struct EnumV {
    const wchar_t* name;
    int v;
};
const EnumV kPhase[] = {{L"READY", 0}, {L"WAIT", 1},  {L"SHOT", 2},  {L"ACT_OS", 3}, {L"ACT_OL", 4},
                        {L"ACT_OE", 5}, {L"ACT_S", 6}, {L"ACT_L", 7}, {L"DEACT", 8},  {L"S_RETURN", 9}};
const EnumV kAct[] = {{L"AT_STOP", 0},       {L"AT_ONE_FOLLOW", 1}, {L"AT_LOOP", 2},        {L"AT_TARGET", 3},
                      {L"AT_TARGET_F", 4},   {L"AT_SOURCE", 5},     {L"AT_DMG_POS", 6},     {L"AT_MOV_1TAR", 7},
                      {L"AT_MOV_OPTION", 8}, {L"AT_MOV_APIECE", 9}, {L"AT_MOV_SPLASH", 10}, {L"AT_MOV_PIERCE", 11}};
const EnumV kMov[] = {{L"MOV_NONE", 0}, {L"MOV_STRAIGHT", 1}, {L"MOV_UP", 2},
                      {L"MOV_UPR", 3},  {L"MOV_ROUND", 4},    {L"MOV_HWAN", 5}};
const EnumV kSct[] = {{L"none", 0},
                      {L"SCT_ARROW", 1},
                      {L"SCT_RUT", 2},
                      {L"SCT_MOVER", 4},
                      {L"SCT_SHAKECAM0", 8},
                      {L"SCT_SHAKECAM1", 16},
                      {L"SCT_SHAKECAM_MOV0", 32},
                      {L"SCT_SHAKECAM_MOV1", 64},
                      {L"SCT_SHAKECAM2", 128},
                      {L"SCT_SHAKECAM3", 256},
                      {L"SCT_SHAKECAM_MOV2", 512},
                      {L"SCT_SHAKECAM_MOV3", 1024},
                      {L"SCT_MAT", 2048},
                      {L"SCT_CHAR_SCALE", 4096},
                      {L"SCT_EFFECT_SCALE", 8192}};

// client lookup 0xA94F50: exact name (the tables carry both cases where the client accepts both)
template <size_t N>
int Lookup(const EnumV (&t)[N], std::wstring_view s, int bad = -1) {
    for (const auto& e : t)
        if (s == e.name) return e.v;
    return bad;
}
template <size_t N>
const wchar_t* NameOf(const EnumV (&t)[N], int v) {
    for (const auto& e : t)
        if (e.v == v) return e.name;
    return L"?";
}

const std::wstring& Cell(const std::vector<std::wstring>& c, size_t i) {
    static const std::wstring empty;
    return i < c.size() ? c[i] : empty;
}
bool IsNone(std::wstring_view s) { return s.empty() || s == L"none" || s == L"NONE" || s == L"None"; }
std::wstring NoneEmpty(const std::wstring& s) { return IsNone(s) ? std::wstring() : s; }
int Int(const std::wstring& s, int def = 0) {
    long long v = 0;
    return ParseInt(Trim(s), v) ? static_cast<int>(v) : def;
}
double Dbl(std::wstring_view s, double def = 0) {
    double v = 0;
    return ParseFloat(Trim(s), v) ? v : def;
}
std::vector<std::wstring> SplitW(std::wstring_view s, wchar_t sep) {
    std::vector<std::wstring> out;
    for (auto v : SplitView(s, sep)) out.emplace_back(v);
    return out;
}
Vec3 VecOf(const std::wstring& s) {
    const auto p = SplitW(s, L',');
    Vec3 r;
    if (p.size() >= 1) r.x = Dbl(p[0]);
    if (p.size() >= 2) r.y = Dbl(p[1]);
    if (p.size() >= 3) r.z = Dbl(p[2]);
    return r;
}
std::wstring Up(std::wstring_view s) { return ToUpper(s); }
std::wstring Slash(std::wstring s) {
    for (auto& c : s)
        if (c == L'/') c = L'\\';
    while (!s.empty() && s.front() == L'\\') s.erase(s.begin());
    return s;
}
std::wstring Folder(std::wstring s) {
    s = Slash(std::move(s));
    if (!s.empty() && s.back() != L'\\') s += L'\\';
    return s;
}

}  // namespace

const wchar_t* PhaseName(Phase p) { return NameOf(kPhase, static_cast<int>(p)); }
const wchar_t* ActName(Act a) { return NameOf(kAct, static_cast<int>(a)); }
const wchar_t* MovName(Mov m) { return NameOf(kMov, static_cast<int>(m)); }
Phase PhaseFrom(std::wstring_view s) { return static_cast<Phase>(Lookup(kPhase, s)); }
Act ActFrom(std::wstring_view s) { return static_cast<Act>(Lookup(kAct, s)); }
Mov MovFrom(std::wstring_view s) { return static_cast<Mov>(Lookup(kMov, s)); }

int AniRole(std::wstring_view t) {
    // table 0xEC7138
    struct R {
        const wchar_t* n;
        int v;
    };
    static const R fixed[] = {{L"ANI_ATTACK1", 2},   {L"ANI_ATTACK2", 5},   {L"ANI_ATTACK3", 16},  {L"ANI_ATTACK4", 17},
                              {L"ANI_ATTACK5", 183}, {L"ANI_ATTACK6", 184}, {L"ANI_ATTACK7", 185}, {L"ANI_ATTACK8", 186},
                              {L"ANI_ATTACK9", 190}, {L"ANI_HAMMER", 187},  {L"ANI_HANDLOOF", 188}, {L"ANI_TROW", 189},
                              {L"ANI_SHOT", 191}};
    for (const auto& r : fixed)
        if (t == r.n) return r.v;
    auto num = [&](std::wstring_view pre, int lo, int hi) -> int {
        if (t.size() <= pre.size() || t.substr(0, pre.size()) != pre) return -1;
        long long n = 0;
        if (!ParseInt(t.substr(pre.size()), n) || n < lo || n > hi) return -1;
        return static_cast<int>(n);
    };
    int n = num(L"ANI_ATTACK", 10, 16);
    if (n >= 0) return 182 + n;
    n = num(L"ANI_READY0", 1, 4);
    if (n >= 0) return 39 + n;
    n = num(L"ANI_WAIT0", 1, 4);
    if (n >= 0) return 90 + n;
    n = num(L"ANI_SKILL_", 1, 100);
    if (n >= 0) {
        if (n <= 10) return 25 + n;
        if (n <= 20) return 57 + n;
        if (n <= 40) return 80 + n;
        return 82 + n;
    }
    return -1;
}

std::wstring AniTokenOf(int role) {
    static const std::pair<int, const wchar_t*> fixed[] = {
        {2, L"ANI_ATTACK1"},   {5, L"ANI_ATTACK2"},   {16, L"ANI_ATTACK3"},  {17, L"ANI_ATTACK4"},  {183, L"ANI_ATTACK5"},
        {184, L"ANI_ATTACK6"}, {185, L"ANI_ATTACK7"}, {186, L"ANI_ATTACK8"}, {190, L"ANI_ATTACK9"}, {187, L"ANI_HAMMER"},
        {188, L"ANI_HANDLOOF"}, {189, L"ANI_TROW"},   {191, L"ANI_SHOT"}};
    for (const auto& f : fixed)
        if (f.first == role) return f.second;
    if (role >= 192 && role <= 198) return L"ANI_ATTACK" + std::to_wstring(role - 182);
    if (role >= 40 && role <= 43) return L"ANI_READY0" + std::to_wstring(role - 39);
    if (role >= 91 && role <= 94) return L"ANI_WAIT0" + std::to_wstring(role - 90);
    if (role >= 26 && role <= 35) return L"ANI_SKILL_" + std::to_wstring(role - 25);
    if (role >= 68 && role <= 77) return L"ANI_SKILL_" + std::to_wstring(role - 57);
    if (role >= 101 && role <= 120) return L"ANI_SKILL_" + std::to_wstring(role - 80);
    if (role >= 123 && role <= 182) return L"ANI_SKILL_" + std::to_wstring(role - 82);
    return {};
}

// --------------------------------------------------------------------------------------------------- rows
CharInfoRow ParseCharInfo(const std::vector<std::wstring>& c) {
    CharInfoRow r;
    r.cells = c;
    r.key = Cell(c, 0);
    r.soundObject = Cell(c, 1);
    r.size = Dbl(Cell(c, 2), 2) * 0.5;  // 0xA9AFBE: * 0.5
    r.rideType = Cell(c, 3) == L"RT_FIXED" ? 1 : Cell(c, 3) == L"RT_DUMMY" ? 2 : 0;
    r.ride = NoneEmpty(Cell(c, 4));
    r.dieBsr = NoneEmpty(Cell(c, 5));
    r.dieEffect = NoneEmpty(Cell(c, 6));
    r.damageBone = NoneEmpty(Cell(c, 7));
    r.damagePos = VecOf(Cell(c, 8));
    r.bloodType = NoneEmpty(Cell(c, 9));
    r.deadEffect = Cell(c, 10) == L"DEFFECT_FLASH" ? 1 : 0;
    const auto env = SplitW(Cell(c, 11), L',');
    r.env = env.empty() ? 0 : Int(env[0]);
    if ((r.env & 2) && env.size() > 1) r.bgm = env[1];
    r.rangedOffset = Int(Cell(c, 12));
    return r;
}

AniSetRow ParseAniSet(const std::vector<std::wstring>& c) {
    AniSetRow r;
    r.cells = c;
    r.service = Int(Cell(c, 0), 1) != 0;
    r.label = Cell(c, 1);
    r.key = Cell(c, 2);
    r.priority = Int(Cell(c, 3));
    r.hideWeapon = Int(Cell(c, 5));
    r.aniGroup = Cell(c, 6);
    r.readyTok = Cell(c, 7);
    r.waitTok = Cell(c, 8);
    r.shotTok = Cell(c, 9);
    r.ready = AniRole(r.readyTok);
    r.wait = AniRole(r.waitTok);
    for (const auto& t : SplitW(r.shotTok, L',')) {
        const int role = AniRole(Trim(t));
        if (role >= 0) r.shot.push_back(role);
    }
    r.playTiming = NoneEmpty(Cell(c, 10));
    r.actW = NoneEmpty(Cell(c, 11));
    r.actS = NoneEmpty(Cell(c, 12));
    r.defenseEfp = Slash(NoneEmpty(Cell(c, 13)));
    r.damageEfp = Slash(NoneEmpty(Cell(c, 14)));
    r.trailLen = Int(Cell(c, 15));
    const auto col = SplitW(Cell(c, 16), L',');
    for (size_t i = 0; i < 4 && i < col.size(); ++i) r.trailColor[i] = Int(col[i]);
    r.trailOp = Cell(c, 17) == L"ONE" ? 2 : 6;
    r.trailTex = NoneEmpty(Cell(c, 18));
    r.startTiming = NoneEmpty(Cell(c, 19));
    r.arrowTail = Slash(NoneEmpty(Cell(c, 20)));
    r.arrowForce = Slash(NoneEmpty(Cell(c, 21)));
    r.light = NoneEmpty(Cell(c, 22));
    r.skillObject = NoneEmpty(Cell(c, 23));
    static const EnumV tw[] = {{L"none", 0}, {L"Roll", 1}, {L"Yaw", 2}, {L"Pitch", 3}, {L"RollR", 4}, {L"YawR", 5}, {L"PitchR", 6}};
    r.waistTwist = Lookup(tw, Cell(c, 24), 0);
    r.isAttack = Int(Cell(c, 25));
    r.blood = Int(Cell(c, 26), 1);
    return r;
}

EffectRow ParseEffect(const std::vector<std::wstring>& c) {
    EffectRow r;
    r.cells = c;
    if (c.size() < 28) r.parseNotes.push_back(Fmt(L"%zu hücre (28 gerekir) / %zu cells (28 needed)", c.size(), c.size()));
    r.label = Cell(c, 0);
    r.key = Cell(c, 1);
    r.phase = PhaseFrom(Cell(c, 2));
    if (r.phase == Phase::Bad) r.parseNotes.push_back(L"AniType: " + Cell(c, 2));
    r.startEvent = Int(Cell(c, 3));
    const std::wstring& d = Cell(c, 4);
    r.dmg = (d == L"TRUE" || d == L"true") ? 1 : (d == L"FALSE" || d == L"false") ? 0 : -1;
    if (r.dmg < 0) r.parseNotes.push_back(L"DMG Event: " + d);
    for (const auto& p : SplitW(Cell(c, 5), L'|')) {
        if (p == L"NOR") r.damageTypes |= kNor;
        else if (p == L"CRI") r.damageTypes |= kCri;
        else if (p == L"HWAN") r.damageTypes |= kHwan;
    }
    const std::wstring& sc = Cell(c, 6);
    r.scale = sc == L"MOB_BASE" ? ScaleBase::Mob : sc == L"CHAR_BASE" ? ScaleBase::Char : IsNone(sc) ? ScaleBase::None : ScaleBase::Bad;
    r.id = Int(Cell(c, 7));
    r.attach = Int(Cell(c, 8));
    r.trade = Int(Cell(c, 9));
    r.kill = Int(Cell(c, 10));
    r.createCnt = Int(Cell(c, 11), 1);
    const auto fade = SplitW(Cell(c, 12), L',');
    if (!fade.empty()) r.fadeIn = Dbl(fade[0]);
    if (fade.size() > 1) r.fadeOut = Dbl(fade[1]);
    r.act = ActFrom(Cell(c, 13));
    if (r.act == Act::Bad) r.parseNotes.push_back(L"ActType: " + Cell(c, 13));
    const auto mv = SplitW(Cell(c, 14), L',');
    if (!mv.empty()) r.mov = MovFrom(mv[0]);
    if (r.mov == Mov::Bad) r.parseNotes.push_back(L"MovType: " + (mv.empty() ? std::wstring() : mv[0]));
    if (mv.size() > 1) r.movDelay = Dbl(mv[1]);
    if (mv.size() > 2) r.speedMin = Dbl(mv[2]);
    r.speedMax = mv.size() > 3 ? Dbl(mv[3]) : r.speedMin;
    if (r.speedMax < r.speedMin) r.speedMax = r.speedMin;
    const auto pa = SplitW(Cell(c, 15), L',');
    for (size_t i = 0; i < 3 && i < pa.size(); ++i) r.param[i] = Dbl(pa[i]);
    const auto ao = SplitW(Cell(c, 16), L',');
    if (!ao.empty()) r.opt.on = ao[0] == L"true" || ao[0] == L"TRUE";
    if (ao.size() > 1) r.opt.angle = Dbl(ao[1]);
    if (ao.size() > 2) r.opt.b = Dbl(ao[2]);
    if (ao.size() > 3) r.opt.c = Dbl(ao[3]);
    if (ao.size() > 4) r.opt.ms = Dbl(ao[4]);
    if (ao.size() > 5) r.opt.flag = ao[5] == L"true" || ao[5] == L"TRUE";
    r.objPath = NoneEmpty(Cell(c, 17));
    r.objName = NoneEmpty(Cell(c, 18));
    if (!r.objName.empty()) {
        const std::wstring u = Up(r.objName);
        r.kind = EndsWithI(u, L".BSR")   ? ObjKind::Bsr
                 : EndsWithI(u, L".EFP") ? ObjKind::Efp
                 : EndsWithI(u, L".WAV") ? ObjKind::Wav
                 : EndsWithI(u, L".DDJ") ? ObjKind::Ddj
                                         : ObjKind::None;
        if (r.kind == ObjKind::None) r.parseNotes.push_back(L"ObjName: " + r.objName);
    }
    r.startBone = NoneEmpty(Cell(c, 19));
    r.startOff = VecOf(Cell(c, 20));
    r.targetBone = NoneEmpty(Cell(c, 21));
    r.targetOff = VecOf(Cell(c, 22));
    r.objName2 = NoneEmpty(Cell(c, 23));
    r.rotate = Int(Cell(c, 24));
    r.scriptText = NoneEmpty(Cell(c, 25));
    if (!r.scriptText.empty()) {
        const auto parts = SplitW(r.scriptText, L',');
        for (const auto& f : SplitW(parts[0], L'|')) {
            const int v = Lookup(kSct, Trim(f), -1);
            if (v < 0) r.parseNotes.push_back(L"Script: " + f);
            else r.script |= v;
        }
        for (size_t i = 1; i < parts.size(); ++i) r.scriptArgs.push_back(Dbl(parts[i]));
    }
    r.sndBegin = Slash(NoneEmpty(Cell(c, 26)));
    r.sndEnd = Slash(NoneEmpty(Cell(c, 27)));
    return r;
}

std::wstring EffectRow::ResPath() const {
    if (objName.empty()) return {};
    const std::wstring name = Slash(objName);
    std::wstring dir = objPath.empty() ? std::wstring() : Folder(objPath);
    if (kind == ObjKind::Wav) return L"prim\\snd\\" + dir + name;  // 0xA9DBB6: "prim\snd\" + ObjPath + ObjName
    return dir + name;
}

std::wstring EffectRow::Res2Path() const {
    if (objName2.empty()) return {};
    const std::wstring n = Slash(objName2);
    if (n.find(L'\\') != std::wstring::npos) return n;
    return (objPath.empty() || kind != ObjKind::Efp ? std::wstring() : Folder(objPath)) + n;
}

bool EffectRow::Projectile() const {
    if (mov == Mov::None || mov == Mov::Bad) return false;
    return speedMax > 0;
}

std::wstring SoundRow::DataPath() const { return L"prim\\snd\\" + Folder(folder) + Slash(file); }

SkillInfo ParseSkillRow(const std::vector<std::wstring>& c) {
    SkillInfo s;
    if (c.size() < 17) return s;
    s.id = Int(Cell(c, 1));
    s.code = Cell(c, 3);
    s.group = Cell(c, 5);
    s.original = Int(Cell(c, 6));
    s.chain = Cell(c, 9);
    s.prepMs = Dbl(Cell(c, 11));
    s.castMs = Dbl(Cell(c, 12));
    s.durMs = Dbl(Cell(c, 13));
    s.flySpeed = Dbl(Cell(c, 16));
    s.valid = s.id != 0 && !s.code.empty();
    if (c.size() >= 118) {
        std::vector<long long> p(50, 0);
        for (size_t k = 0; k < 50; ++k) {
            long long v = 0;
            if (ParseInt(Trim(c[68 + k]), v)) p[k] = v;
        }
        WalkSkillParams(p, s);
    }
    return s;
}

namespace {
struct TagDef {
    const char* name;
    long long value;
    int dwords;
};
const TagDef kTags[] = {
#include "ParamTags.inc"
};
}  // namespace

void WalkSkillParams(const std::vector<long long>& p, SkillInfo& s) {
    s.params.clear();
    s.param1 = p.empty() ? 0 : static_cast<int>(p[0]);
    size_t i = 1;
    while (i < p.size() && i < 50) {
        const long long v = p[i];
        if (v == 0) {
            ++i;
            continue;
        }
        const TagDef* t = nullptr;
        for (const auto& d : kTags)
            if (d.value == v) {
                t = &d;
                break;
            }
        if (!t) {
            s.paramError = Fmt(L"Param%zu = %lld: etiket değil / not a tag", i + 1, v);
            break;
        }
        std::vector<long long> args;
        const size_t n = t->dwords < 0 ? p.size() - i : static_cast<size_t>(t->dwords);
        for (size_t k = 1; k < n && i + k < p.size(); ++k) args.push_back(p[i + k]);
        s.params[t->name] = std::move(args);
        if (t->dwords < 0) break;  // ssou / sstt: the rest of the row
        i += n;
    }
    // duration family [durationMs, probability, grade ...] (SRO_SKILLS §3.4) + petrify stns
    static const char* durFam[] = {"stns", "st", "se", "rt", "sl", "fe", "my", "dn", "ds", "ca", "bl"};
    for (const char* tg : durFam) {
        auto it = s.params.find(tg);
        if (it != s.params.end() && !it->second.empty() && it->second[0] > 0) {
            s.statusMs = static_cast<double>(it->second[0]);
            break;
        }
    }
    if (s.statusMs <= 0)
        for (const auto& [tg, a] : s.params)
            if (tg.size() >= 3 && tg[0] == 'c' && tg[1] == 's' && !a.empty() && a[0] > 0) {
                s.statusMs = static_cast<double>(a[0]);
                break;
            }
    if (auto it = s.params.find("dura"); it != s.params.end() && !it->second.empty()) s.duraMs = static_cast<double>(it->second[0]);
    if (auto it = s.params.find("mc"); it != s.params.end() && it->second.size() >= 2) s.hits = static_cast<int>(it->second[1]);
    s.buffMs = s.statusMs > 0 ? s.statusMs : s.duraMs;
}

// ----------------------------------------------------------------------------------------------------- FxDb
bool FxDb::LoadSkillEffect(const Bytes& raw, std::wstring& err) {
    SeDoc doc;
    if (!doc.Load(raw, err)) return false;
    m_cli = Set{};
    m_seCount = 0;
    for (const Rec& rec : doc.sim.recs) {
        if (rec.header) continue;
        const int line = rec.line + 1;
        if (rec.sec == Sec::CI) {
            CharInfoRow r = ParseCharInfo(rec.cells);
            r.line = line;
            if (!r.key.empty()) m_cli.ci[r.key] = std::move(r);  // last row wins
        } else if (rec.sec == Sec::SA) {
            AniSetRow r = ParseAniSet(rec.cells);
            r.line = line;
            if (!r.key.empty()) m_cli.sa[r.key] = std::move(r);  // the later row overwrites the object's fields
        } else if (rec.sec == Sec::SE) {
            EffectRow r = ParseEffect(rec.cells);
            r.line = line;
            if (!r.key.empty()) {
                m_cli.se[r.key].push_back(std::move(r));  // rows accumulate per key in file order
                ++m_seCount;
            }
        }
    }
    return true;
}

void FxDb::LoadEffectSound(const Bytes& raw) {
    m_snd.clear();
    m_sndByObj.clear();
    const std::wstring t = DecodeText(raw);
    int ln = 0;
    size_t a = 0;
    while (a <= t.size()) {
        size_t b = t.find(L'\n', a);
        if (b == std::wstring::npos) b = t.size();
        std::wstring_view line(t.data() + a, b - a);
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        ++ln;
        a = b + 1;
        if (line.size() < 2 || line.substr(0, 2) == L"//") continue;
        const auto c = SplitView(line, L'\t');
        if (c.size() < 10) continue;
        SoundRow r;
        r.object = Trim(c[1]);
        r.handle = Trim(c[2]);
        r.skill = Trim(c[3]);
        r.event1 = Trim(c[4]);
        r.folder = Trim(c[8]);
        r.file = Trim(c[9]);
        r.volume = c.size() > 10 ? Int(std::wstring(c[10]), 100) : 100;
        r.line = ln;
        if (r.object.empty() || r.file.empty()) continue;
        m_sndByObj.emplace(Up(r.object), m_snd.size());
        m_snd.push_back(std::move(r));
    }
}

int FxDb::SetPending(const std::wstring& listText, std::vector<std::wstring>* notes) {
    ClearPending();
    const ParsedList pl = ParseListText(listText, L"pending");
    int taken = 0;
    for (const ListRow& lr : pl.rows) {
        if (lr.key.empty()) {
            if (notes) notes->push_back(Fmt(L"satır %d: anahtar yok / line %d: no key", lr.listLine, lr.listLine));
            continue;
        }
        if (lr.sec == Sec::CI) {
            CharInfoRow r = ParseCharInfo(lr.cells);
            r.origin = Origin::Pending;
            r.line = lr.listLine;
            m_pend.ci[r.key] = std::move(r);
        } else if (lr.sec == Sec::SA) {
            AniSetRow r = ParseAniSet(lr.cells);
            r.origin = Origin::Pending;
            r.line = lr.listLine;
            m_pend.sa[r.key] = std::move(r);
        } else if (lr.sec == Sec::SE) {
            EffectRow r = ParseEffect(lr.cells);
            r.origin = Origin::Pending;
            r.line = lr.listLine;
            m_pend.se[r.key].push_back(std::move(r));
        } else {
            if (notes) notes->push_back(Fmt(L"satır %d: bölüm bilinmiyor / line %d: unknown section", lr.listLine, lr.listLine));
            continue;
        }
        ++taken;
    }
    m_replace = pl.replaceKeys;
    m_remove = pl.removeKeys;
    return taken;
}

bool FxDb::HasPending() const {
    return !m_pend.ci.empty() || !m_pend.sa.empty() || !m_pend.se.empty() || !m_replace.empty() || !m_remove.empty();
}

void FxDb::ClearPending() {
    m_pend = Set{};
    m_replace.clear();
    m_remove.clear();
}

const AniSetRow* FxDb::AniSet(const std::wstring& key) const {
    if (auto it = m_pend.sa.find(key); it != m_pend.sa.end()) return &it->second;
    if (m_remove.count(key)) return nullptr;
    auto it = m_cli.sa.find(key);
    return it == m_cli.sa.end() ? nullptr : &it->second;
}

bool FxDb::Disabled(const std::wstring& key) const {
    const AniSetRow* a = AniSet(key);
    return a && !a->service;
}

namespace {
bool SameCells(const EffectRow& a, const EffectRow& b) { return a.cells == b.cells; }
// is `sub` an ordered subsequence of `all`?
bool Subseq(const std::vector<EffectRow>& sub, const std::vector<EffectRow>& all) {
    size_t j = 0;
    for (size_t i = 0; i < all.size() && j < sub.size(); ++i)
        if (SameCells(all[i], sub[j])) ++j;
    return j == sub.size();
}
}  // namespace

std::vector<const EffectRow*> FxDb::Effects(const std::wstring& key) const {
    std::vector<const EffectRow*> out;
    const auto ci = m_cli.se.find(key);
    const auto pi = m_pend.se.find(key);
    const bool dropClient = m_replace.count(key) || m_remove.count(key);
    if (pi == m_pend.se.end()) {
        if (!dropClient && ci != m_cli.se.end())
            for (const auto& r : ci->second) out.push_back(&r);
        return out;
    }
    // the merge engine's per-key group rule (V6): client subset of the list -> the list's group; list subset of
    // the client -> the client's group; otherwise a conflict that only "#replace KEY" resolves -> preview the list
    if (!dropClient && ci != m_cli.se.end() && Subseq(pi->second, ci->second) && !Subseq(ci->second, pi->second)) {
        for (const auto& r : ci->second) out.push_back(&r);
        return out;
    }
    for (const auto& r : pi->second) out.push_back(&r);
    return out;
}

const CharInfoRow* FxDb::CharInfo(const std::wstring& code) const {
    for (const Set* s : {&m_pend, &m_cli}) {
        if (s == &m_cli && m_remove.count(code)) return nullptr;
        if (auto it = s->ci.find(code); it != s->ci.end()) return &it->second;
    }
    // range rows PREFIX###~### (client registers PREFIX + %03d for every n)
    if (code.size() < 4) return nullptr;
    long long n = 0;
    if (!ParseInt(std::wstring_view(code).substr(code.size() - 3), n)) return nullptr;
    const std::wstring prefix = code.substr(0, code.size() - 3);
    for (const Set* s : {&m_pend, &m_cli})
        for (auto it = s->ci.lower_bound(prefix); it != s->ci.end() && StartsWith(it->first, prefix); ++it) {
            const std::wstring& k = it->first;
            if (k.size() != prefix.size() + 7 || k[k.size() - 4] != L'~') continue;
            long long lo = 0, hi = 0;
            if (ParseInt(std::wstring_view(k).substr(prefix.size(), 3), lo) && ParseInt(std::wstring_view(k).substr(k.size() - 3), hi) &&
                n >= lo && n <= hi)
                return &it->second;
        }
    return nullptr;
}

std::vector<const SoundRow*> FxDb::Sounds(const std::wstring& object, const std::wstring& handle,
                                          const std::wstring& skillKey) const {
    std::vector<const SoundRow*> out;
    const std::wstring uo = Up(object), uh = Up(handle);
    const std::wstring sk = skillKey.empty() ? std::wstring(L"-") : skillKey;
    auto [a, b] = m_sndByObj.equal_range(uo);
    for (auto it = a; it != b; ++it) {
        const SoundRow& r = m_snd[it->second];
        if (Up(r.handle) == uh && r.skill == sk) out.push_back(&r);
    }
    return out;
}

std::vector<const SoundRow*> FxDb::SoundsOf(const std::wstring& object, const std::wstring& skillKey) const {
    std::vector<const SoundRow*> out;
    auto [a, b] = m_sndByObj.equal_range(Up(object));
    for (auto it = a; it != b; ++it)
        if (m_snd[it->second].skill == skillKey) out.push_back(&m_snd[it->second]);
    std::sort(out.begin(), out.end(), [](const SoundRow* x, const SoundRow* y) { return x->line < y->line; });
    return out;
}

std::vector<std::wstring> FxDb::Keys() const {
    std::set<std::wstring> k;
    for (const Set* s : {&m_cli, &m_pend}) {
        for (const auto& [key, v] : s->sa) k.insert(key);
        for (const auto& [key, v] : s->se) k.insert(key);
    }
    return {k.begin(), k.end()};
}

std::vector<std::wstring> FxDb::PendingKeys() const {
    std::set<std::wstring> k(m_replace.begin(), m_replace.end());
    k.insert(m_remove.begin(), m_remove.end());
    for (const auto& [key, v] : m_pend.sa) k.insert(key);
    for (const auto& [key, v] : m_pend.se) k.insert(key);
    return {k.begin(), k.end()};
}

bool FxDb::IsPendingKey(const std::wstring& key) const {
    return m_pend.sa.count(key) || m_pend.se.count(key) || m_replace.count(key) || m_remove.count(key);
}

std::wstring FxDb::PresentationKey(const SkillInfo& s0, const std::function<const SkillInfo*(int)>& byId) const {
    const SkillInfo* s = &s0;
    for (int depth = 0; s && depth < 8; ++depth) {
        const std::wstring key0 = (!s->group.empty() && s->group != L"xxx") ? s->group : s->code;
        if (AniSet(key0) || s->original == 0 || !byId) return key0;
        s = byId(s->original);
    }
    return (!s0.group.empty() && s0.group != L"xxx") ? s0.group : s0.code;
}

}  // namespace se::sp
