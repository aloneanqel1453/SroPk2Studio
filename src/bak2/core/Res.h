// SkillEffect Studio - core/Res.h
// Read-only parsers of the SRO resource formats the preview needs: JMXVRES (.bsr), JMXVBMT (.bmt), JMXVBMS (.bms),
// JMXVBSK (.bsk), JMXVBAN (.ban), JMXVDDJ (.ddj = DDS: DXT1/3/5 + uncompressed). Ported from the user's
// SroEffectViewer (Formats.h / Ddj.h) - the loaders only; the particle code there is NOT used (EFP_RUNTIME.md §13).
// Math: D3D row vectors (p' = p * M, translation row included), double precision.
#pragma once

#include "Text.h"

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace se {

struct V3 {
    double x = 0, y = 0, z = 0;
    V3() = default;
    V3(double a, double b, double c) : x(a), y(b), z(c) {}
    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(double s) const { return {x * s, y * s, z * s}; }
    V3 operator/(double s) const { return {x / s, y / s, z / s}; }
    V3& operator+=(const V3& o) {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    double Dot(const V3& o) const { return x * o.x + y * o.y + z * o.z; }
    V3 Cross(const V3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double Len() const { return std::sqrt(Dot(*this)); }
    V3 Norm() const {
        const double l = Len();
        return l > 1e-12 ? *this / l : V3{};
    }
    V3 Mul(const V3& o) const { return {x * o.x, y * o.y, z * o.z}; }
};

struct Quat {
    double x = 0, y = 0, z = 0, w = 1;
};

struct M4 {
    double m[4][4]{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    static M4 I() { return {}; }
    // row-vector composition: (a * b) applies a first, then b
    M4 operator*(const M4& b) const {
        M4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                double s = 0;
                for (int k = 0; k < 4; ++k) s += m[i][k] * b.m[k][j];
                r.m[i][j] = s;
            }
        return r;
    }
    M4 Inv() const;  // general 4x4 inverse (identity when singular)
    V3 T() const { return {m[3][0], m[3][1], m[3][2]}; }
    void SetT(const V3& t) {
        m[3][0] = t.x;
        m[3][1] = t.y;
        m[3][2] = t.z;
    }
};
inline V3 Xf(const V3& v, const M4& M) {  // sub_410C30: v * M including the translation row
    return {v.x * M.m[0][0] + v.y * M.m[1][0] + v.z * M.m[2][0] + M.m[3][0],
            v.x * M.m[0][1] + v.y * M.m[1][1] + v.z * M.m[2][1] + M.m[3][1],
            v.x * M.m[0][2] + v.y * M.m[1][2] + v.z * M.m[2][2] + M.m[3][2]};
}
inline V3 XfDir(const V3& v, const M4& M) {
    return {v.x * M.m[0][0] + v.y * M.m[1][0] + v.z * M.m[2][0], v.x * M.m[0][1] + v.y * M.m[1][1] + v.z * M.m[2][1],
            v.x * M.m[0][2] + v.y * M.m[1][2] + v.z * M.m[2][2]};
}
M4 QuatTrans(const Quat& q, const V3& t);  // D3DXMatrixRotationQuaternion + translation row
Quat Slerp(Quat a, Quat b, double t);
M4 RotY(double a);
M4 RotZ(double a);
M4 RotX(double a);

struct Img {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;  // w*h*4
    bool Valid() const { return w > 0 && h > 0 && rgba.size() == static_cast<size_t>(w) * h * 4; }
};
bool DecodeDdj(const Bytes& b, Img& out);  // .ddj (20-byte JMXVDDJ header) or plain .dds

// little-endian bounded reader; strings = u32 length + CP949 bytes
struct BinR {
    const Bytes& b;
    size_t p = 0;
    bool bad = false;
    explicit BinR(const Bytes& d) : b(d) {}
    bool Ok() const { return !bad; }
    bool Need(size_t n) {
        if (p + n > b.size()) bad = true;
        return !bad;
    }
    std::uint8_t U8() { return Need(1) ? b[p++] : 0; }
    std::uint16_t U16();
    std::uint32_t U32();
    std::int32_t I32() { return static_cast<std::int32_t>(U32()); }
    float F32();
    V3 Vec3() {
        const double x = F32(), y = F32(), z = F32();
        return {x, y, z};
    }
    Quat Q() {
        Quat q;
        q.x = F32();
        q.y = F32();
        q.z = F32();
        q.w = F32();
        return q;
    }
    std::string Str();
    void Skip(size_t n) {
        if (Need(n)) p += n;
    }
    void Seek(size_t at) {
        if (at > b.size()) bad = true;
        else p = at;
    }
};

struct BmtMat {
    std::string name, diffuse;  // diffuse map path (relative to the .bmt folder unless absolute)
    std::uint32_t flag = 0;
    bool absolute = false;
    float col[4] = {1, 1, 1, 1};
    float emissive[3] = {0, 0, 0};
};
std::vector<BmtMat> ParseBmt(const Bytes& b);

struct BmsVert {
    V3 pos, nrm;
    double u = 0, v = 0;
};
struct BmsWeight {
    int b1 = 0, b2 = 0;
    double w1 = 1, w2 = 0;
};
struct Bms {
    std::string name, material;
    std::vector<BmsVert> verts;
    std::vector<std::string> boneNames;
    std::vector<BmsWeight> weights;
    std::vector<std::uint16_t> idx;  // triangle list
    bool skinned = false;
};
bool ParseBms(const Bytes& b, Bms& out);

struct BskBone {
    std::string name, parent;
    Quat rot;  // to parent
    V3 trans;
    int parentIndex = -1;
};
std::vector<BskBone> ParseBsk(const Bytes& b);

struct BanKey {
    Quat rot;
    V3 trans;
};
struct Ban {
    std::string name;
    std::uint32_t durationMs = 0, fps = 0;
    std::vector<std::uint32_t> keyTimes;
    std::map<std::string, std::vector<BanKey>> bones;
    double DurationMs() const;
};
bool ParseBan(const Bytes& b, Ban& out);

struct BsrAniEvent {
    std::uint32_t keyTime = 0, type = 0, index = 0, val = 0;
};
struct BsrAni {
    std::uint32_t type = 0, file = 0;  // role id (ANI_*), index into anis
    std::string group;                 // lower-case ani group ("default", "sword" ...)
    std::vector<BsrAniEvent> events;
};
struct Bsr {
    std::string name;
    std::uint32_t objType = 0;
    std::vector<std::string> mtrls, meshes, anis;
    std::string skeleton, attachBone;
    std::vector<BsrAni> roles;
    bool valid = false;
    const BsrAni* Role(std::uint32_t type) const;  // "default" group first
};
bool ParseBsr(const Bytes& b, Bsr& out);
// JMXVCPD (.cpd compound): .bsr parts
std::vector<std::string> ParseCpd(const Bytes& b);

std::string LowerA(std::string s);
std::wstring W949(const std::string& s);   // CP949 -> UTF-16
std::string N949(const std::wstring& s);   // UTF-16 -> CP949
std::string NormPath(std::string s);       // lower, '/' -> '\', no leading '\'

}  // namespace se
