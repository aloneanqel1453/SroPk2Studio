// SkillEffect Studio - render/RFormats.cpp
#include "RFormats.h"

#include "RBytes.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>

namespace se::render {

namespace {

std::wstring W949(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(949, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {   // not CP949: Latin-1
        std::wstring w;
        for (unsigned char c : s) w.push_back(c);
        return w;
    }
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(949, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::wstring RStr(RReader& r, std::uint32_t mx = 4096) { return W949(r.Str(mx)); }
V3f RV3f(RReader& r) { V3f v; v.x = r.F32(); v.y = r.F32(); v.z = r.F32(); return v; }
V3 RV3(RReader& r) { double x = r.F32(), y = r.F32(), z = r.F32(); return {x, y, z}; }
Quat RQ(RReader& r) { Quat q; q.x = r.F32(); q.y = r.F32(); q.z = r.F32(); q.w = r.F32(); return q; }
bool Sig(const std::uint8_t* d, std::size_t n, const char* s) { return n >= 12 && std::memcmp(d, s, 7) == 0; }
bool IEq(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    return true;
}
bool Fits(const RReader& r, std::uint64_t count, std::uint64_t unit) { return unit == 0 || count <= r.Left() / unit; }

}  // namespace

// ------------------------------------------------------------------------------------------------ BMS
bool ParseBms(const std::uint8_t* d, std::size_t n, BmsFile& out, std::string& err) {
    out = BmsFile();
    if (!Sig(d, n, "JMXVBMS")) { err = "not JMXVBMS"; return false; }
    RReader r(d, n);
    r.Seek(12);
    std::uint32_t h[12];
    for (auto& x : h) x = r.U32();
    const std::uint32_t vOff = h[0], skinOff = h[1], faceOff = h[2];
    r.U32();                       // subPrimCount
    out.vertexFlag = r.U32();
    r.U32();
    out.name = RStr(r);
    out.material = RStr(r);
    r.U32();
    if (r.Fail()) { err = "BMS header short"; return false; }
    const bool lm = (out.vertexFlag & 0x400) != 0, morph = (out.vertexFlag & 0x800) != 0;
    const std::size_t stride = 32 + (lm ? 8 : 0) + (morph ? 36 : 0) + 12;
    std::uint32_t vc = 0;
    if (r.SeekSection(vOff)) {
        vc = r.U32();
        if (r.Fail() || !Fits(r, vc, stride)) { err = "BMS vertex count"; return false; }
        out.verts.resize(vc);
        for (auto& v : out.verts) {
            v.pos = RV3f(r);
            v.nrm = RV3f(r);
            v.uv.x = r.F32(); v.uv.y = r.F32();
            if (lm) r.Skip(8);
            if (morph) r.Skip(36);
            r.Skip(12);
        }
    } else { err = "BMS no vertices"; return false; }
    r.ClearFail();
    if (r.SeekSection(skinOff)) {
        std::uint32_t bc = r.U32();
        if (r.Ok() && bc && Fits(r, bc, 5)) {
            for (std::uint32_t i = 0; i < bc && r.Ok(); ++i) out.boneNames.push_back(RStr(r, 1024));
            if (r.Ok() && vc && Fits(r, vc, 6)) {
                out.skin.resize(vc);
                for (auto& s : out.skin) { s.b0 = r.U8(); s.w0 = r.U16(); s.b1 = r.U8(); s.w1 = r.U16(); }
                if (r.Fail()) out.skin.clear();
            }
        }
    }
    const std::size_t afterSkin = r.Ok() ? r.Pos() : 0;
    r.ClearFail();
    auto plausible = [&](std::size_t at) {
        r.ClearFail();
        if (!r.SeekSection(at)) return false;
        std::uint32_t k = r.U32();
        bool good = r.Ok() && k > 0 && Fits(r, k, 6);
        r.ClearFail();
        return good;
    };
    std::size_t faceAt = 0;
    if (plausible(faceOff)) faceAt = faceOff;
    else if (afterSkin && plausible(afterSkin)) faceAt = afterSkin;
    else if (afterSkin)
        for (std::size_t at = afterSkin > 64 ? afterSkin - 64 : 0; at <= afterSkin + 64; ++at)
            if (plausible(at)) { faceAt = at; break; }
    if (!faceAt || !r.SeekSection(faceAt)) { err = "BMS no faces"; return false; }
    std::uint32_t fc = r.U32();
    if (r.Fail() || !Fits(r, fc, 6)) { err = "BMS face count"; return false; }
    out.idx.resize(static_cast<std::size_t>(fc) * 3);
    for (auto& i : out.idx) i = r.U16();
    for (auto& i : out.idx)
        if (i >= vc) i = 0;
    if (!out.verts.empty()) {
        out.bbMin = out.bbMax = out.verts[0].pos;
        for (const auto& v : out.verts) {
            out.bbMin.x = std::min(out.bbMin.x, v.pos.x); out.bbMin.y = std::min(out.bbMin.y, v.pos.y); out.bbMin.z = std::min(out.bbMin.z, v.pos.z);
            out.bbMax.x = std::max(out.bbMax.x, v.pos.x); out.bbMax.y = std::max(out.bbMax.y, v.pos.y); out.bbMax.z = std::max(out.bbMax.z, v.pos.z);
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ BMT
const BmtMaterial* BmtFile::Find(const std::wstring& n) const {
    for (const auto& m : mats)
        if (IEq(m.name, n)) return &m;
    return nullptr;
}

bool ParseBmt(const std::uint8_t* d, std::size_t n, BmtFile& out, std::string& err) {
    out = BmtFile();
    if (!Sig(d, n, "JMXVBMT")) { err = "not JMXVBMT"; return false; }
    RReader r0(d, n);
    r0.Seek(12);
    std::uint32_t count = r0.U32();
    if (r0.Fail() || count > 4096) { err = "BMT count"; return false; }
    const std::size_t start = r0.Pos();
    struct Attempt { std::vector<BmtMaterial> m; std::size_t used = 0; bool complete = false; };
    auto read = [&](bool bump) {
        Attempt a;
        RReader r(d, n);
        r.Seek(start);
        for (std::uint32_t i = 0; i < count; ++i) {
            BmtMaterial m;
            m.name = RStr(r);
            for (float* c : {m.diffuse, m.ambient, m.specular, m.emissive})
                for (int k = 0; k < 4; ++k) c[k] = r.F32();
            m.power = r.F32();
            m.flags = r.U32();
            m.diffuseMap = RStr(r);
            r.F32(); r.U8(); r.U8();
            m.absolute = r.U8() != 0;
            if (bump && r.Ok() && (m.flags & 0x2000)) { m.normalMap = RStr(r); r.U32(); }
            if (r.Fail()) break;
            a.m.push_back(std::move(m));
        }
        a.used = r.Pos();
        a.complete = a.m.size() == count && r.Ok();
        return a;
    };
    auto score = [&](const Attempt& a) -> long long {
        if (!a.complete) return -1;
        long long over = static_cast<long long>(n) - static_cast<long long>(a.used);
        return over >= 0 ? 1000000 - over : -1;
    };
    Attempt best = read(true);
    if (score(best) < 1000000) {
        Attempt nb = read(false);
        if (score(nb) > score(best)) best = std::move(nb);
    }
    if (!best.complete && best.m.empty()) { err = "BMT truncated"; return false; }
    out.mats = std::move(best.m);
    return true;
}

// ------------------------------------------------------------------------------------------------ BSK
int BskFile::Find(const std::wstring& n) const {
    for (std::size_t i = 0; i < bones.size(); ++i)
        if (IEq(bones[i].name, n)) return static_cast<int>(i);
    return -1;
}

bool ParseBsk(const std::uint8_t* d, std::size_t n, BskFile& out, std::string& err) {
    out = BskFile();
    if (!Sig(d, n, "JMXVBSK")) { err = "not JMXVBSK"; return false; }
    RReader r(d, n);
    r.Seek(12);
    std::uint32_t bc = r.U32();
    if (r.Fail() || bc > 4096) { err = "BSK count"; return false; }
    for (std::uint32_t i = 0; i < bc; ++i) {
        BskBone b;
        b.type = r.U8();
        b.name = RStr(r, 1024);
        b.parent = RStr(r, 1024);
        b.rotParent = RQ(r); b.transParent = RV3(r);
        b.rotOrigin = RQ(r); b.transOrigin = RV3(r);
        b.rotLocal = RQ(r); b.transLocal = RV3(r);
        std::uint32_t cc = r.U32();
        if (r.Fail() || cc > r.Left() / 4) break;
        for (std::uint32_t c = 0; c < cc && r.Ok(); ++c) RStr(r, 1024);
        if (r.Fail()) break;
        out.bones.push_back(std::move(b));
    }
    for (std::size_t i = 0; i < out.bones.size(); ++i) {
        BskBone& b = out.bones[i];
        b.parentIndex = b.parent.empty() ? -1 : out.Find(b.parent);
        if (b.parentIndex == static_cast<int>(i)) b.parentIndex = -1;
    }
    if (out.bones.empty() && bc) { err = "BSK truncated"; return false; }
    return true;
}

// ------------------------------------------------------------------------------------------------ BAN
double BanFile::LengthMs() const {
    double d = std::max<double>(durationMs, keyTimes.empty() ? 0.0 : static_cast<double>(keyTimes.back()));
    return d > 0 ? d : 1000.0;
}

bool ParseBan(const std::uint8_t* d, std::size_t n, BanFile& out, std::string& err) {
    out = BanFile();
    if (!Sig(d, n, "JMXVBAN")) { err = "not JMXVBAN"; return false; }
    int ver = 0;
    for (int i = 8; i < 12; ++i) if (d[i] >= '0' && d[i] <= '9') ver = ver * 10 + (d[i] - '0');
    RReader r(d, n);
    r.Seek(12);
    if (ver >= 102) { r.I32(); r.I32(); }
    out.name = RStr(r);
    out.durationMs = r.I32();
    out.fps = r.I32();
    out.type = r.U32();
    std::uint32_t tc = r.U32();
    if (r.Fail() || !Fits(r, tc, 4)) { err = "BAN header"; return false; }
    out.keyTimes.resize(tc);
    for (auto& t : out.keyTimes) t = r.U32();
    std::uint32_t bc = r.U32();
    if (r.Fail() || !Fits(r, bc, 8)) { err = "BAN bones"; return false; }
    for (std::uint32_t i = 0; i < bc; ++i) {
        BanBone b;
        b.name = RStr(r, 1024);
        std::uint32_t kc = r.U32();
        if (r.Fail() || !Fits(r, kc, 28)) break;
        b.keys.resize(kc);
        for (auto& k : b.keys) { k.q = RQ(r); k.t = RV3(r); }
        if (r.Fail()) break;
        out.bones.push_back(std::move(b));
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ BSR
namespace {

bool ReadModData(RReader& r, std::uint32_t tag, BsrFile& out) {
    if (!r.Skip(28)) return false;
    switch (tag) {
        case 0x00000000u: {
            r.U32();
            std::uint32_t flags = r.U32();
            r.U32();
            std::int32_t g = r.I32();
            if (g < 0 || !r.Skip(static_cast<std::size_t>(g) * 20)) return false;
            if (flags & 4u) { std::int32_t c = r.I32(); if (c < 0 || !r.Skip(static_cast<std::size_t>(c) * 8)) return false; }
            if (!r.Skip(36)) return false;
            break;
        }
        case 0x00010000u: if (!r.Skip(5 * 4 + 64)) return false; break;
        case 0x00010001u: case 0x00010002u: r.U32(); RStr(r); r.U32(); break;
        case 0x00030000u: {
            std::int32_t k = r.I32();
            if (k < 0 || k > 4096) return false;
            for (std::int32_t i = 0; i < k && r.Ok(); ++i) {
                BsrParticle p;
                p.enabled = r.I32() != 0;
                p.path = RStr(r);
                p.bone = RStr(r);
                p.offset = RV3(r);
                r.U32();
                r.U8(); r.U8(); r.U8();
                std::uint8_t last = r.U8();
                if (last == 1 && !r.Skip(12)) return false;
                if (!p.path.empty()) out.particles.push_back(p);
            }
            break;
        }
        case 0x00040000u: if (!r.Skip(16)) return false; break;
        case 0x00040001u: {
            if (!r.Skip(16 + 24)) return false;
            std::int32_t k = r.I32();
            if (k < 0 || k > 4096) return false;
            for (std::int32_t i = 0; i < k && r.Ok(); ++i)
                if (r.U8() != 0) RStr(r);
            break;
        }
        case 0x00050000u: {
            std::int32_t sets = r.I32();
            if (sets <= 0) break;
            if (!r.Skip(44) || sets > 4096) return false;
            for (std::int32_t i = 0; i < sets && r.Ok(); ++i) {
                RStr(r);
                std::int32_t t = r.I32();
                if (t < 0 || t > 8192) return false;
                for (std::int32_t k = 0; k < t && r.Ok(); ++k) {
                    if (r.U32() == 0) continue;
                    BsrSound s;
                    s.path = RStr(r);
                    s.timeMs = r.I32();
                    s.event = RStr(r);
                    if (!s.path.empty()) out.sounds.push_back(s);
                }
            }
            break;
        }
        case 0x00060000u: case 0x00060001u: case 0x00060002u: case 0x00070000u: break;
        default: return false;
    }
    return r.Ok();
}

void ReadModPalette(RReader& r, std::uint32_t off, BsrFile& out) {
    if (!r.SeekSection(off)) return;
    for (int list = 0; list < 2; ++list) {
        std::int32_t k = r.I32();
        if (r.Fail() || k < 0 || k > 4096) return;
        for (std::int32_t i = 0; i < k; ++i) {
            r.I32(); r.I32(); RStr(r);
            std::int32_t c = r.I32();
            if (r.Fail() || c < 0 || c > 4096) return;
            for (std::int32_t j = 0; j < c; ++j) {
                std::uint32_t tag = r.U32();
                if (r.Fail() || !ReadModData(r, tag, out)) return;
            }
        }
    }
}

}  // namespace

bool ParseBsr(const std::uint8_t* d, std::size_t n, BsrFile& out, std::string& err) {
    out = BsrFile();
    if (!Sig(d, n, "JMXVRES")) { err = "not JMXVRES"; return false; }
    RReader r(d, n);
    r.Seek(12);
    std::uint32_t h[13];
    for (auto& x : h) x = r.U32();
    const std::uint32_t matOff = h[0], meshOff = h[1], skelOff = h[2], aniOff = h[3], mgOff = h[4], agOff = h[5],
                        modOff = h[6], meshFlag = h[8];
    out.type = r.U16();
    out.category = r.U16();
    out.name = RStr(r);
    r.U32(); r.U32();
    if (r.Fail()) { err = "BSR header short"; return false; }
    r.ClearFail();
    if (r.SeekSection(matOff)) {
        std::uint32_t k = r.U32();
        if (r.Ok() && k <= 5)
            for (std::uint32_t i = 0; i < k && r.Ok(); ++i) { std::uint32_t id = r.U32(); std::wstring p = RStr(r); if (r.Ok()) out.materialSets.push_back({id, p}); }
        if (r.Fail() || k > 5) out.partial = true;
    }
    r.ClearFail();
    if (r.SeekSection(meshOff)) {
        std::uint32_t k = r.U32();
        if (r.Ok() && Fits(r, k, 4))
            for (std::uint32_t i = 0; i < k && r.Ok(); ++i) { std::wstring p = RStr(r); if (meshFlag & 1) r.U32(); if (r.Ok()) out.meshes.push_back(p); }
        if (r.Fail()) out.partial = true;
    }
    r.ClearFail();
    if (r.SeekSection(aniOff)) {
        r.U32(); r.U32();
        std::uint32_t k = r.U32();
        if (r.Ok() && Fits(r, k, 4))
            for (std::uint32_t i = 0; i < k && r.Ok(); ++i) { std::wstring p = RStr(r); if (r.Ok()) out.animations.push_back(p); }
        if (r.Fail()) out.partial = true;
    }
    r.ClearFail();
    if (r.SeekSection(skelOff)) {
        if (r.U32() == 1 && r.Ok()) {
            out.hasSkeleton = true;
            out.skeleton = RStr(r);
            out.attachBone = RStr(r, 1024);
        }
        if (r.Fail()) out.partial = true;
    }
    r.ClearFail();
    if (r.SeekSection(mgOff)) {
        std::uint32_t k = r.U32();
        if (r.Ok() && Fits(r, k, 8))
            for (std::uint32_t i = 0; i < k && r.Ok(); ++i) {
                std::wstring nm = RStr(r, 1024);
                std::uint32_t fc = r.U32();
                if (r.Fail() || !Fits(r, fc, 4)) break;
                std::vector<std::uint32_t> ix(fc);
                for (auto& x : ix) x = r.U32();
                out.meshGroups.push_back({nm, ix});
            }
        if (r.Fail()) out.partial = true;
    }
    r.ClearFail();
    if (r.SeekSection(agOff)) {
        std::uint32_t gc = r.U32();
        if (r.Ok() && Fits(r, gc, 8))
            for (std::uint32_t gi = 0; gi < gc && r.Ok(); ++gi) {
                BsrAniGroup g;
                g.name = RStr(r, 1024);
                std::uint32_t ac = r.U32();
                if (r.Fail() || !Fits(r, ac, 20)) break;
                for (std::uint32_t ai = 0; ai < ac && r.Ok(); ++ai) {
                    BsrAniEntry e;
                    e.type = r.U32();
                    e.fileIndex = r.U32();
                    std::uint32_t ec = r.U32();
                    if (r.Fail() || !Fits(r, ec, 16)) break;
                    for (std::uint32_t ei = 0; ei < ec; ++ei) {
                        BsrAniEvent ev;
                        ev.keyTime = r.U32(); ev.type = r.U32(); ev.index = r.U32(); ev.unk = r.U32();
                        e.events.push_back(ev);
                    }
                    std::uint32_t wc = r.U32();
                    e.walkLength = r.F32();
                    if (r.Fail() || !Fits(r, wc, 8)) break;
                    r.Skip(static_cast<std::size_t>(wc) * 8);
                    if (r.Ok()) g.entries.push_back(std::move(e));
                }
                out.aniGroups.push_back(std::move(g));
            }
        if (r.Fail()) out.partial = true;
    }
    {
        RReader mr(d, n);
        ReadModPalette(mr, modOff, out);
    }
    return true;
}

bool ParseCpd(const std::uint8_t* d, std::size_t n, CpdFile& out, std::string& err) {
    out = CpdFile();
    if (!Sig(d, n, "JMXVCPD")) { err = "not JMXVCPD"; return false; }
    RReader r(d, n);
    r.Seek(12);
    std::uint32_t colOff = r.U32(), resOff = r.U32();
    r.Skip(20);
    out.type = r.U32();
    out.name = RStr(r);
    if (r.Fail()) { err = "CPD header"; return false; }
    if (r.SeekSection(colOff)) out.collision = RStr(r);
    r.ClearFail();
    if (!r.SeekSection(resOff)) { err = "CPD no parts"; return false; }
    std::uint32_t k = r.U32();
    if (r.Fail() || k > 100000) { err = "CPD count"; return false; }
    for (std::uint32_t i = 0; i < k && r.Ok(); ++i) { std::wstring p = RStr(r); if (r.Ok()) out.parts.push_back(p); }
    return true;
}

// ------------------------------------------------------------------------------------------------ ani types
namespace {
struct AniName { const wchar_t* n; int id; };
const AniName kAni[] = {
    {L"POSE", 0x3C}, {L"STAND1", 0x00}, {L"STAND", 0x00}, {L"STAND2", 0x7A}, {L"STAND3", 0x3D}, {L"STAND4", 0x51},
    {L"ATTREADY", 0x06}, {L"TURN_L", 0x18}, {L"TURN_R", 0x19}, {L"SIT_DOWN", 0x0D}, {L"SIT", 0x0E},
    {L"STAND_UP", 0x0F}, {L"DEFENCE", 0x16}, {L"WALK", 0x01}, {L"WALK_BACK", 0x17}, {L"RUN", 0x07},
    {L"REVOLUTION", 0x27}, {L"HAMMER", 0xBB}, {L"HANDLOOF", 0xBC}, {L"TROW", 0xBD}, {L"MG_SSELF", 0x13},
    {L"MG_SOTHER", 0x14}, {L"DAMAGE1", 0x03}, {L"DAMAGE", 0x03}, {L"DAMAGE2", 0x09}, {L"HELP", 0x43},
    {L"FIND", 0x4E}, {L"STUN", 0x4F}, {L"DIE1", 0x04}, {L"DIE", 0x04}, {L"DIE1_RM", 0x24}, {L"DIE2", 0x12},
    {L"DIE2_RM", 0x25}, {L"REVIVAL", 0x79}, {L"DOWN", 0x3E}, {L"DOWN_RM", 0x3F}, {L"DOWN_DAMAGE", 0x40},
    {L"DOWN_UP", 0x41}, {L"DOWN_DIE", 0x42}, {L"PICK", 0x26}, {L"CLICK", 0x0A}, {L"CB_YEONHWAN", 0x0B},
    {L"CB_2", 0x0C}, {L"ET_BYE", 0x15},
};
const int kAttack[17] = {-1, 0x02, 0x05, 0x10, 0x11, 0xB7, 0xB8, 0xB9, 0xBA, 0xBE, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6};
int SkillId(int k) {
    if (k >= 1 && k <= 10) return 0x1A + k - 1;
    if (k >= 11 && k <= 20) return 0x44 + k - 11;
    if (k >= 21 && k <= 40) return 0x65 + k - 21;
    if (k >= 41 && k <= 100) return 0x7B + k - 41;
    return -1;
}
int TailNum(const std::wstring& s, std::size_t at) {
    if (at >= s.size()) return -1;
    int v = 0;
    for (std::size_t i = at; i < s.size(); ++i) {
        if (s[i] < L'0' || s[i] > L'9') return -1;
        v = v * 10 + (s[i] - L'0');
        if (v > 1000) return -1;
    }
    return v;
}
}  // namespace

int AniTypeId(const std::wstring& token) {
    std::wstring t;
    for (wchar_t c : token)
        if (c != L' ' && c != L'\t') t.push_back(static_cast<wchar_t>(std::towupper(c)));
    if (t.rfind(L"ANI_", 0) == 0) t = t.substr(4);
    if (t.empty() || t == L"NONE") return -1;
    for (const auto& a : kAni)
        if (t == a.n) return a.id;
    if (t.rfind(L"ATTACK", 0) == 0) { int k = TailNum(t, 6); return k >= 1 && k <= 16 ? kAttack[k] : -1; }
    if (t.rfind(L"SKILL_", 0) == 0) return SkillId(TailNum(t, 6));
    if (t.rfind(L"SKILL", 0) == 0) return SkillId(TailNum(t, 5));
    if (t.rfind(L"READY", 0) == 0) { int k = TailNum(t, 5); return k >= 1 && k <= 5 ? 0x27 + k : -1; }
    if (t.rfind(L"WAIT", 0) == 0) { int k = TailNum(t, 4); return k >= 1 && k <= 5 ? 0x5A + k : -1; }
    if (t.rfind(L"EMOTION", 0) == 0) { int k = TailNum(t, 7); return k >= 1 && k <= 8 ? 0x31 + k : -1; }
    return -1;
}

std::wstring AniTypeName(int id) {
    for (const auto& a : kAni)
        if (a.id == id && std::wcscmp(a.n, L"STAND") && std::wcscmp(a.n, L"DAMAGE") && std::wcscmp(a.n, L"DIE")) return a.n;
    for (int k = 1; k <= 16; ++k) if (kAttack[k] == id) return L"ATTACK" + std::to_wstring(k);
    for (int k = 1; k <= 100; ++k) if (SkillId(k) == id) return L"SKILL_" + std::to_wstring(k);
    if (id >= 0x28 && id <= 0x2C) return L"READY0" + std::to_wstring(id - 0x27);
    if (id >= 0x5B && id <= 0x5F) return L"WAIT0" + std::to_wstring(id - 0x5A);
    if (id >= 0x32 && id <= 0x39) return L"EMOTION0" + std::to_wstring(id - 0x31);
    return {};
}

}  // namespace se::render
