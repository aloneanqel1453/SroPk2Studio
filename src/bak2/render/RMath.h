// SkillEffect Studio - render/RMath.h
// Double precision vector / 4x4 matrix helpers in the D3D ROW-VECTOR convention used by the client and
// by efp_sim.py:  v' = v * M  (translation in row 3).  Mul(A, B) == numpy A @ B.
#pragma once

#include <cmath>
#include <cstring>

namespace se::render {

struct V3 {
    double x = 0, y = 0, z = 0;
    V3() = default;
    constexpr V3(double X, double Y, double Z) : x(X), y(Y), z(Z) {}
    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(double s) const { return {x * s, y * s, z * s}; }
    V3 operator/(double s) const { return {x / s, y / s, z / s}; }
    V3 operator-() const { return {-x, -y, -z}; }
    V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    V3& operator-=(const V3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    double& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
    double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};
inline double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double Len(const V3& a) { return std::sqrt(Dot(a, a)); }
inline V3 Hadamard(const V3& a, const V3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline V3 Norm(const V3& a) { double l = Len(a); return l > 1e-30 ? a / l : V3{}; }

struct M4 {
    double m[4][4];
    M4() { std::memset(m, 0, sizeof(m)); m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0; }
    static M4 Zero() { M4 r; std::memset(r.m, 0, sizeof(r.m)); return r; }
    static M4 FromFloats(const float* f16) { M4 r; for (int i = 0; i < 16; ++i) r.m[i / 4][i % 4] = f16[i]; return r; }
    V3 Row(int i) const { return {m[i][0], m[i][1], m[i][2]}; }
    void SetRow(int i, const V3& v) { m[i][0] = v.x; m[i][1] = v.y; m[i][2] = v.z; }
    V3 T() const { return Row(3); }
};

inline M4 Mul(const M4& a, const M4& b) {
    M4 r = M4::Zero();
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

// sub_410C30 / sub_854E30: row vector times 4x4 INCLUDING the translation row (no divide).
inline V3 Xf(const V3& v, const M4& M) {
    return {v.x * M.m[0][0] + v.y * M.m[1][0] + v.z * M.m[2][0] + M.m[3][0],
            v.x * M.m[0][1] + v.y * M.m[1][1] + v.z * M.m[2][1] + M.m[3][1],
            v.x * M.m[0][2] + v.y * M.m[1][2] + v.z * M.m[2][2] + M.m[3][2]};
}
// rotation part only (3x3)
inline V3 Xf3(const V3& v, const M4& M) {
    return {v.x * M.m[0][0] + v.y * M.m[1][0] + v.z * M.m[2][0],
            v.x * M.m[0][1] + v.y * M.m[1][1] + v.z * M.m[2][1],
            v.x * M.m[0][2] + v.y * M.m[1][2] + v.z * M.m[2][2]};
}

// general 4x4 inverse (Gauss-Jordan, partial pivoting) - numpy.linalg.inv equivalent for the sim.
inline M4 Inverse(const M4& a) {
    double t[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) t[i][j] = j < 4 ? a.m[i][j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(t[r][c]) > std::fabs(t[p][c])) p = r;
        if (std::fabs(t[p][c]) < 1e-300) return M4();
        if (p != c)
            for (int j = 0; j < 8; ++j) { double x = t[c][j]; t[c][j] = t[p][j]; t[p][j] = x; }
        double d = t[c][c];
        for (int j = 0; j < 8; ++j) t[c][j] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c && t[r][c] != 0.0) {
                double f = t[r][c];
                for (int j = 0; j < 8; ++j) t[r][j] -= f * t[c][j];
            }
    }
    M4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = t[i][j + 4];
    return r;
}

inline M4 RotZ(double a) { double c = std::cos(a), s = std::sin(a); M4 M; M.m[0][0] = c; M.m[0][1] = s; M.m[1][0] = -s; M.m[1][1] = c; return M; }
inline M4 RotY(double a) { double c = std::cos(a), s = std::sin(a); M4 M; M.m[0][0] = c; M.m[0][2] = -s; M.m[2][0] = s; M.m[2][2] = c; return M; }
inline M4 RotX(double a) { double c = std::cos(a), s = std::sin(a); M4 M; M.m[1][1] = c; M.m[1][2] = s; M.m[2][1] = -s; M.m[2][2] = c; return M; }
inline M4 Translate(const V3& t) { M4 M; M.SetRow(3, t); return M; }
inline M4 Scale3(double s) { M4 M; M.m[0][0] = M.m[1][1] = M.m[2][2] = s; return M; }
// rotation-only copy (translation row cleared)
inline M4 RotPart(const M4& a) { M4 r = a; r.m[3][0] = r.m[3][1] = r.m[3][2] = 0; r.m[0][3] = r.m[1][3] = r.m[2][3] = 0; r.m[3][3] = 1; return r; }

// unit quaternion (x, y, z, w) -> D3DXMatrixRotationQuaternion (row-vector) with translation t
struct Quat { double x = 0, y = 0, z = 0, w = 1; };
inline M4 QuatTrans(Quat q, const V3& t) {
    double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n <= 1e-20) q = Quat{}; else { q.x /= n; q.y /= n; q.z /= n; q.w /= n; }
    double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    M4 r;
    r.m[0][0] = 1 - 2 * (yy + zz); r.m[0][1] = 2 * (xy + wz);     r.m[0][2] = 2 * (xz - wy);
    r.m[1][0] = 2 * (xy - wz);     r.m[1][1] = 1 - 2 * (xx + zz); r.m[1][2] = 2 * (yz + wx);
    r.m[2][0] = 2 * (xz + wy);     r.m[2][1] = 2 * (yz - wx);     r.m[2][2] = 1 - 2 * (xx + yy);
    r.SetRow(3, t);
    return r;
}
inline Quat Slerp(Quat a, Quat b, double t) {
    double d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; d = -d; }
    if (d > 0.9995) {
        Quat r{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z), a.w + t * (b.w - a.w)};
        double n = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
        if (n > 0) { r.x /= n; r.y /= n; r.z /= n; r.w /= n; }
        return r;
    }
    double th0 = std::acos(d), th = th0 * t;
    double s0 = std::cos(th) - d * std::sin(th) / std::sin(th0), s1 = std::sin(th) / std::sin(th0);
    return {a.x * s0 + b.x * s1, a.y * s0 + b.y * s1, a.z * s0 + b.z * s1, a.w * s0 + b.w * s1};
}

constexpr double kPi = 3.14159265358979323846;

}  // namespace se::render
