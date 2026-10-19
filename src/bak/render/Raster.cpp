// ============================================================================
//  SRO Asset Studio - render/Raster.cpp
//
//  Software rasterizer. No GPU, no D3D, no OpenGL.
//
//  Engine conventions (SilkroadDoc.wiki/Formats.md, "SR engine specifications"):
//    * LEFT-HANDED coordinate system, Y up.
//    * Vertex winding is CLOCKWISE for front faces.
//    * UV: U left->right, V top->bottom - which is exactly our top-down Image
//      layout, so V is used verbatim (no flip).
//
//  Winding, worked through concretely so the culling test is not a guess:
//    A clockwise triangle seen in NDC (x right, y up), e.g.
//        A=(0,1)  B=(1,-1)  C=(-1,-1)
//    maps to screen (x right, y DOWN) as
//        A=(50,0) B=(100,100) C=(0,100)      (for a 100x100 target)
//    and the signed area
//        (Bx-Ax)*(Cy-Ay) - (By-Ay)*(Cx-Ax) = 50*100 - 100*(-50) = +10000
//    is POSITIVE. So in screen space a FRONT face has a positive area and
//    back-face culling drops area <= 0.
//
//  Pipeline per triangle:
//    object -> clip (mvp)  ->  near-plane clip (Sutherland-Hodgman on z >= 0)
//           -> perspective divide -> screen  ->  edge functions with the
//           top-left fill rule -> perspective-correct UV/normal -> shade.
//
//  Normals: everything the *internal* raster path receives is already in VIEW
//  space (render() multiplies by model*view). The headlight is therefore just
//  the view-space -Z direction, and ShadingMode::Normals shows the view-space
//  normal as RGB. The public drawTriangle() keeps the same contract: it treats
//  RasterVertex::normal as view space, which is the identity case.
//
//  Implementation owner: Agent E (render).
// ============================================================================
#include "Raster.h"

#include <algorithm>
#include <thread>       // band-parallel rasterisation
#include <cmath>
#include <cstdio>
#include <vector>

namespace sro {

// ===========================================================================
// Mat4  -  row-major storage, row-vector convention:  v' = v * M
//          index = row * 4 + column
// ===========================================================================
Mat4 Mat4::Identity() { return Mat4(); }

Mat4 Mat4::Translation(const Vec3& t) {
    Mat4 m;
    m.m[12] = t.x; m.m[13] = t.y; m.m[14] = t.z;
    return m;
}

Mat4 Mat4::Scale(const Vec3& s) {
    Mat4 m;
    m.m[0] = s.x; m.m[5] = s.y; m.m[10] = s.z;
    return m;
}

Mat4 Mat4::RotationX(f32 radians) {
    const f32 c = std::cos(radians), s = std::sin(radians);
    Mat4 m;
    m.m[5] = c; m.m[6] = s;
    m.m[9] = -s; m.m[10] = c;
    return m;
}

Mat4 Mat4::RotationY(f32 radians) {
    const f32 c = std::cos(radians), s = std::sin(radians);
    Mat4 m;
    m.m[0] = c; m.m[2] = -s;
    m.m[8] = s; m.m[10] = c;
    return m;
}

Mat4 Mat4::RotationZ(f32 radians) {
    const f32 c = std::cos(radians), s = std::sin(radians);
    Mat4 m;
    m.m[0] = c; m.m[1] = s;
    m.m[4] = -s; m.m[5] = c;
    return m;
}

Mat4 Mat4::FromQuat(const Quat& q) {
    // Normalise defensively - Silkroad quaternions are unit, but a corrupt
    // file must not produce a NaN matrix.
    f32 n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-20f)) return Mat4();
    const f32 x = q.x / n, y = q.y / n, z = q.z / n, w = q.w / n;

    Mat4 m;
    m.m[0]  = 1.0f - 2.0f * (y * y + z * z);
    m.m[1]  =        2.0f * (x * y + z * w);
    m.m[2]  =        2.0f * (x * z - y * w);
    m.m[4]  =        2.0f * (x * y - z * w);
    m.m[5]  = 1.0f - 2.0f * (x * x + z * z);
    m.m[6]  =        2.0f * (y * z + x * w);
    m.m[8]  =        2.0f * (x * z + y * w);
    m.m[9]  =        2.0f * (y * z - x * w);
    m.m[10] = 1.0f - 2.0f * (x * x + y * y);
    return m;
}

Mat4 Mat4::LookAtLH(const Vec3& eye, const Vec3& target, const Vec3& up) {
    Vec3 zaxis = (target - eye).normalized();
    if (zaxis.length() < 0.5f) zaxis = Vec3(0.0f, 0.0f, 1.0f);

    Vec3 xaxis = up.cross(zaxis).normalized();
    if (xaxis.length() < 0.5f) {
        // up parallel to the view direction - pick any perpendicular axis.
        const Vec3 alt = (std::fabs(zaxis.y) > 0.9f) ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
        xaxis = alt.cross(zaxis).normalized();
    }
    const Vec3 yaxis = zaxis.cross(xaxis);

    Mat4 m;
    m.m[0] = xaxis.x; m.m[1] = yaxis.x; m.m[2]  = zaxis.x; m.m[3]  = 0.0f;
    m.m[4] = xaxis.y; m.m[5] = yaxis.y; m.m[6]  = zaxis.y; m.m[7]  = 0.0f;
    m.m[8] = xaxis.z; m.m[9] = yaxis.z; m.m[10] = zaxis.z; m.m[11] = 0.0f;
    m.m[12] = -xaxis.dot(eye);
    m.m[13] = -yaxis.dot(eye);
    m.m[14] = -zaxis.dot(eye);
    m.m[15] = 1.0f;
    return m;
}

Mat4 Mat4::PerspectiveFovLH(f32 fovY, f32 aspect, f32 zNear, f32 zFar) {
    Mat4 m;
    if (!(fovY > 1e-4f) || !(aspect > 1e-6f) || !(zFar > zNear) || !(zNear > 0.0f))
        return m;
    const f32 yScale = 1.0f / std::tan(fovY * 0.5f);
    const f32 xScale = yScale / aspect;

    std::memset(m.m, 0, sizeof(m.m));
    m.m[0]  = xScale;
    m.m[5]  = yScale;
    m.m[10] = zFar / (zFar - zNear);
    m.m[11] = 1.0f;
    m.m[14] = -zNear * zFar / (zFar - zNear);
    return m;
}

Mat4 Mat4::OrthoLH(f32 width, f32 height, f32 zNear, f32 zFar) {
    Mat4 m;
    if (!(width > 1e-6f) || !(height > 1e-6f) || !(zFar > zNear)) return m;
    std::memset(m.m, 0, sizeof(m.m));
    m.m[0]  = 2.0f / width;
    m.m[5]  = 2.0f / height;
    m.m[10] = 1.0f / (zFar - zNear);
    m.m[14] = -zNear / (zFar - zNear);
    m.m[15] = 1.0f;
    return m;
}

Mat4 Mat4::operator*(const Mat4& o) const {
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            f32 s = 0.0f;
            for (int k = 0; k < 4; ++k) s += m[i * 4 + k] * o.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    }
    return r;
}

Vec4 Mat4::transformVec4(const Vec4& v) const {
    return Vec4(v.x * m[0] + v.y * m[4] + v.z * m[8]  + v.w * m[12],
                v.x * m[1] + v.y * m[5] + v.z * m[9]  + v.w * m[13],
                v.x * m[2] + v.y * m[6] + v.z * m[10] + v.w * m[14],
                v.x * m[3] + v.y * m[7] + v.z * m[11] + v.w * m[15]);
}

Vec3 Mat4::transformPoint(const Vec3& p) const {
    const Vec4 r = transformVec4(Vec4(p.x, p.y, p.z, 1.0f));
    if (std::fabs(r.w) > 1e-20f && r.w != 1.0f)
        return Vec3(r.x / r.w, r.y / r.w, r.z / r.w);
    return Vec3(r.x, r.y, r.z);
}

Vec3 Mat4::transformVector(const Vec3& v) const {
    return Vec3(v.x * m[0] + v.y * m[4] + v.z * m[8],
                v.x * m[1] + v.y * m[5] + v.z * m[9],
                v.x * m[2] + v.y * m[6] + v.z * m[10]);
}

Mat4 Mat4::transposed() const {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i * 4 + j] = m[j * 4 + i];
    return r;
}

bool Mat4::invert(Mat4& out) const {
    // Straight cofactor expansion. 4x4 is small enough that clarity wins.
    const f32* a = m;
    f32 inv[16];

    inv[0]  =  a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15]
             + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    inv[4]  = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15]
             - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    inv[8]  =  a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15]
             + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14]
             - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    inv[1]  = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15]
             - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    inv[5]  =  a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15]
             + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    inv[9]  = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15]
             - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    inv[13] =  a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14]
             + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    inv[2]  =  a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15]
             + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
    inv[6]  = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15]
             - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
    inv[10] =  a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15]
             + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14]
             - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
    inv[3]  = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11]
             - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
    inv[7]  =  a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11]
             + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11]
             - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
    inv[15] =  a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10]
             + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];

    const f32 det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (std::fabs(det) < 1e-20f) { out = Mat4(); return false; }

    const f32 invDet = 1.0f / det;
    for (int i = 0; i < 16; ++i) out.m[i] = inv[i] * invDet;
    return true;
}

// ===========================================================================
// Scene
// ===========================================================================
Aabb RasterMesh::computeBounds() const {
    Aabb b;
    for (const RasterVertex& v : vertices) {
        const Vec3 p = transform.transformPoint(v.position);
        // Skip NaN/inf: one poisoned vertex would blow the AABB up to infinity,
        // which makes the auto-fit place the camera nowhere and the model
        // renders as a blank viewport.
        if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) b.add(p);
    }
    return b;
}

void Scene::computeBounds() {
    bounds = Aabb();
    for (const RasterMesh& m : meshes) bounds.add(m.computeBounds());
    for (const RasterLines& l : lines)
        for (const Vec3& p : l.points) bounds.add(l.transform.transformPoint(p));
}

void Scene::orientToUp(const Vec3& upIn, const Vec3& fwdIn) {
    Vec3 up = upIn;
    if (!(up.length() > 1e-4f)) return;
    up = up.normalized();

    // Build a rotation whose ROWS map model axes to world axes (row-vector
    // convention v*M): choose a world basis (right, worldUp=+Y, back) and set
    // the model's (right, up, forward) onto it. When forward is unknown, keep
    // the model's current horizontal facing by projecting it out of `up`.
    const Vec3 worldY(0, 1, 0);

    // Model forward: use the skeleton's if given, else derive any vector
    // perpendicular to up so the result is well defined.
    Vec3 fwd = fwdIn;
    if (!(fwd.length() > 1e-4f)) {
        fwd = (std::fabs(up.z) < 0.9f) ? Vec3(0, 0, 1) : Vec3(1, 0, 0);
    }
    // Orthonormalise: right = up x fwd, then fwd = right x up.
    Vec3 right = up.cross(fwd);
    if (!(right.length() > 1e-4f)) {
        right = up.cross((std::fabs(up.x) < 0.9f) ? Vec3(1, 0, 0) : Vec3(0, 0, 1));
    }
    right = right.normalized();
    fwd   = right.cross(up).normalized();

    // Target (row-vector v*R): model up -> +Y (stand up), model forward -> -Z
    // (the face points at the -Z camera), model right -> -X. Negating the right
    // and forward columns keeps a proper right-handed rotation (det +1), so the
    // model is never mirrored.
    Mat4 R;
    R.m[0] = -right.x; R.m[4] = -right.y; R.m[8]  = -right.z;
    R.m[1] =  up.x;    R.m[5] =  up.y;    R.m[9]  =  up.z;
    R.m[2] = -fwd.x;   R.m[6] = -fwd.y;   R.m[10] = -fwd.z;

    for (RasterMesh& m : meshes)  m.transform = R * m.transform;
    for (RasterLines& l : lines)  l.transform = R * l.transform;
    computeBounds();
}

void Scene::orientUpright() {
    // Judge orientation from a CORE bounding box (central 90% of vertices per
    // axis), not the full AABB: thin ribbons, wings and spikes that loop far out
    // would otherwise inflate an axis and hide the real body shape. Verified on
    // boss_srokimhoa (whose green banners defeated a full-AABB test).
    std::vector<f32> xs, ys, zs;
    for (const RasterMesh& m : meshes) {
        xs.reserve(xs.size() + m.vertices.size());
        for (const RasterVertex& v : m.vertices) {
            const Vec3 p = m.transform.transformPoint(v.position);
            if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) {
                xs.push_back(p.x); ys.push_back(p.y); zs.push_back(p.z);
            }
        }
    }
    if (xs.size() < 32) return;

    auto coreExtent = [](std::vector<f32>& v) -> f32 {
        std::sort(v.begin(), v.end());
        const size_t lo = v.size() * 5 / 100, hi = v.size() * 95 / 100;
        return (hi > lo) ? (v[hi] - v[lo]) : (v.back() - v.front());
    };
    const f32 cx = coreExtent(xs), cy = coreExtent(ys), cz = coreExtent(zs);

    // Fire ONLY when the viewer-up axis (Y) is the SMALLEST core extent - the
    // signature of a model lying flat. A standing character has Y tallest (or at
    // least not smallest); a standing quadruped has WIDTH (X) as its smallest
    // axis, so both are left untouched. Also require the model to be clearly
    // elongated so ambiguous near-cubic props are skipped.
    if (!(cy < cx && cy < cz)) return;
    const f32 mx = (std::max)(cx, cz), mn = (std::min)(cx, cz);
    if (!(mx >= 1.4f * mn)) return;

    // Stand the dominant horizontal (body) axis up. RotationX(+90) for a Z-up
    // import is verified against boss_srokimhoa (stands upright, faces front).
    const Mat4 R = (cz >= cx) ? Mat4::RotationX( 1.57079633f)    // +Z -> +Y
                              : Mat4::RotationZ(-1.57079633f);   // +X -> +Y
    for (RasterMesh& m : meshes)  m.transform = R * m.transform;
    for (RasterLines& l : lines)  l.transform = R * l.transform;
    computeBounds();
}

void Scene::clear() {
    meshes.clear();
    lines.clear();
    textures.clear();
    bounds = Aabb();
}

// ===========================================================================
// Camera
//
// Orbit around (target + panOffset) with a quaternion orientation (arcball), so
// the model can be turned to ANY angle with no pole or gimbal lock. setView()
// maps a yaw/pitch pair to a level default/named view; free rotation continues
// from there via orbit()/roll().
// ===========================================================================

// --- quaternion helpers (local) --------------------------------------------
namespace {
inline Quat qMul(const Quat& a, const Quat& b) {
    return Quat(a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
                a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
                a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
                a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z);
}
inline Quat qNormalize(const Quat& q) {
    const f32 n = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (!(n > 1e-20f)) return Quat();
    return Quat(q.x/n, q.y/n, q.z/n, q.w/n);
}
inline Quat qAxisAngle(const Vec3& axis, f32 angle) {
    const Vec3 a = axis.normalized();
    if (!(a.length() > 1e-6f)) return Quat();
    const f32 h = angle * 0.5f, s = std::sin(h);
    return Quat(a.x*s, a.y*s, a.z*s, std::cos(h));
}
inline Vec3 qRotate(const Quat& q, const Vec3& v) {
    const Vec3 u(q.x, q.y, q.z);
    return u * (2.0f * u.dot(v)) + v * (q.w*q.w - u.dot(u)) + u.cross(v) * (2.0f * q.w);
}
// Quaternion from an orthonormal basis whose columns are (right=X, up=Y, fwd=Z).
inline Quat qFromBasis(const Vec3& X, const Vec3& Y, const Vec3& Z) {
    const f32 t = X.x + Y.y + Z.z;
    Quat q;
    // X/Y/Z are the basis COLUMNS, so R(q) must satisfy R*ez == Z. All four
    // antisymmetric terms were NEGATED, which made `orient` the CONJUGATE - i.e.
    // R(orient) was the transpose. The damage: setView() mirrored both yaw and
    // pitch (numpad 7 "top" showed the BOTTOM), carried up to 22 degrees of roll
    // at off-axis yaw, and 'W' levelHorizon() could not level. It only looked
    // right at yaw 0/180, which is where the old default sat. The symmetric
    // off-diagonal sums were already correct; only these four terms change.
    if (t > 0.0f) {
        f32 s = std::sqrt(t + 1.0f) * 2.0f;
        q.w = 0.25f * s; q.x = (Y.z - Z.y) / s; q.y = (Z.x - X.z) / s; q.z = (X.y - Y.x) / s;
    } else if (X.x > Y.y && X.x > Z.z) {
        f32 s = std::sqrt(1.0f + X.x - Y.y - Z.z) * 2.0f;
        q.w = (Y.z - Z.y) / s; q.x = 0.25f * s; q.y = (X.y + Y.x) / s; q.z = (X.z + Z.x) / s;
    } else if (Y.y > Z.z) {
        f32 s = std::sqrt(1.0f + Y.y - X.x - Z.z) * 2.0f;
        q.w = (Z.x - X.z) / s; q.x = (X.y + Y.x) / s; q.y = 0.25f * s; q.z = (Y.z + Z.y) / s;
    } else {
        f32 s = std::sqrt(1.0f + Z.z - X.x - Y.y) * 2.0f;
        q.w = (X.y - Y.x) / s; q.x = (X.z + Z.x) / s; q.y = (Y.z + Z.y) / s; q.z = 0.25f * s;
    }
    return qNormalize(q);
}

/// The reference clamps the orbit pitch to +/-89 degrees. That is not taste: at
/// exactly +/-90 the eye direction is parallel to world up, worldUp x fwd loses
/// rank, and the level basis snaps to an arbitrary roll. 1.5533431 rad is the
/// same 89 degrees the numpad 7/9 snaps already use.
constexpr f32 kPitchLimit = 1.5533431f;
constexpr f32 kPiC        = 3.14159265f;
constexpr f32 kTwoPiC     = 6.28318531f;

/// Fold into (-pi, pi], so a long drag session cannot bleed precision.
inline f32 wrapPi(f32 a) {
    a = std::fmod(a + kPiC, kTwoPiC);
    if (a < 0.0f) a += kTwoPiC;
    return a - kPiC;
}
inline f32 clampPitch(f32 p) {
    if (p >  kPitchLimit) return  kPitchLimit;
    if (p < -kPitchLimit) return -kPitchLimit;
    return p;
}
} // namespace

// Reference main.cpp:566 opens every model at yaw 215 / pitch 16 - a 3/4 FRONT
// view from slightly above. Host forward is (-sinY*cosP, -sinP, cosY*cosP) while
// the reference's is (-cosP*sinY, -sinP, -cosP*cosY): Z is negated, so
// host yaw = reference yaw - 180 = -35 degrees.
Camera::Camera() { setView(-0.6108652f, 0.2792527f); }   // 3/4 front, 16 deg up

void Camera::setView(f32 yaw, f32 pitch) {
    // Historical turntable eye direction with a LEVEL (world-up) horizon, turned
    // into a quaternion so free rotation can continue from here.
    const f32 cp = std::cos(pitch), sp = std::sin(pitch);
    const f32 cy = std::cos(yaw),   sy = std::sin(yaw);
    Vec3 fwd = Vec3(-sy * cp, -sp, cy * cp).normalized();   // target - eye
    if (!(fwd.length() > 1e-5f)) fwd = Vec3(0, 0, 1);
    Vec3 right = Vec3(0, 1, 0).cross(fwd);
    if (!(right.length() > 1e-4f)) right = Vec3(1, 0, 0);
    right = right.normalized();
    const Vec3 up = fwd.cross(right).normalized();
    orient = qFromBasis(right, up, fwd);
}

Vec3 Camera::forwardVec() const { return qRotate(orient, Vec3(0, 0, 1)).normalized(); }
Vec3 Camera::upVector()  const { return qRotate(orient, Vec3(0, 1, 0)).normalized(); }
Vec3 Camera::rightVec()  const { return qRotate(orient, Vec3(1, 0, 0)).normalized(); }

Vec3 Camera::eyePosition() const {
    const f32 d = (distance > 1e-4f) ? distance : 1e-4f;
    return target + panOffset - forwardVec() * d;
}

Mat4 Camera::viewMatrix() const {
    return Mat4::LookAtLH(eyePosition(), target + panOffset, upVector());
}

Mat4 Camera::projMatrix(int width, int height) const {
    if (width <= 0 || height <= 0) return Mat4();
    const f32 aspect = static_cast<f32>(width) / static_cast<f32>(height);
    f32 zn = (zNear > 1e-5f) ? zNear : 1e-5f;
    f32 zf = (zFar > zn * 1.0001f) ? zFar : zn * 1000.0f;

    // Reference main.cpp:4067-4069 recomputes BOTH planes every frame from the
    // LIVE distance, holding the ratio near 20:1 at any zoom. Its own comment
    // names what a wide range costs: the depth buffer cannot separate a body from
    // tight-fitting armour and they Z-FIGHT into flickering "parca parca" shards.
    // The host's fit-time zNear was a far wider range AND went stale the moment
    // the wheel moved (dolly writes `distance` only). radius() is the half
    // DIAGONAL, so it always exceeds the view-axis half-extent and zn stays in
    // front of the nearest triangle; the floor is scaled by distance rather than
    // hard-coded so a sub-unit asset is not clipped away.
    if (sceneRadius > 1e-4f) {
        zn = (distance - sceneRadius) * 0.6f;
        const f32 znFloor = (std::max)(1e-4f, distance * 0.002f);
        if (zn < znFloor) zn = znFloor;
        zf = distance * 6.0f + sceneRadius * 8.0f + 80.0f;
        if (zf <= zn * 1.0001f) zf = zn * 1000.0f;
    }

    if (orthographic) {
        const f32 hh = 2.0f * distance * std::tan(fovY * 0.5f);
        return Mat4::OrthoLH((std::max)(1e-4f, hh * aspect), (std::max)(1e-4f, hh), zn, zf);
    }
    return Mat4::PerspectiveFovLH(fovY, aspect, zn, zf);
}

void Camera::fitToBounds(const Aabb& b, f32 marginFactor) {
    if (!b.valid()) return;
    if (!(marginFactor > 0.01f)) marginFactor = 1.08f;   // tight fit: model fills the view

    target    = b.center();
    panOffset = Vec3(0.0f, 0.0f, 0.0f);

    f32 r = b.radius();
    if (!(r > 1e-5f)) r = 1.0f;

    // Distance that makes a sphere of radius r fill the vertical FOV, plus the
    // requested margin. Vertical is the tighter axis for every viewport wider
    // than it is tall, which is all of ours.
    const f32 sinHalf = (std::max)(0.02f, std::sin((std::max)(0.05f, fovY) * 0.5f));
    distance = (r * marginFactor) / sinHalf;
    if (!(distance > 1e-4f)) distance = 1.0f;

    // Depth range tied to the model: a fixed 0.01 .. 10000 range would throw
    // away almost all z-buffer precision on a model a few hundred units wide.
    sceneRadius = r;                  // projMatrix() re-derives the planes per frame
    zNear = (std::max)(1e-3f, distance * 0.005f);
    zFar  = distance * 8.0f + r * 8.0f;
    if (zFar <= zNear * 1.0001f) zFar = zNear * 1000.0f;
}

void Camera::fitToBoundsAspect(const Aabb& b, f32 aspect, f32 marginFactor) {
    if (!b.valid()) return;
    if (!(aspect > 0.05f)) { fitToBounds(b, marginFactor); return; }
    if (!(marginFactor > 0.01f)) marginFactor = 1.06f;

    target    = b.center();
    panOffset = Vec3(0.0f, 0.0f, 0.0f);

    // View basis for the CURRENT orientation. Distance does not affect it, so it
    // can be built before the distance is known.
    const Vec3 fwd   = forwardVec();
    const Vec3 right = rightVec();
    const Vec3 up    = upVector();

    const f32 tanY = (std::max)(1e-4f, std::tan((std::max)(0.05f, fovY) * 0.5f));
    const f32 tanX = (std::max)(1e-4f, tanY * aspect);

    // A corner sitting `z` in front of the centre is only (d + z) away, so it
    // projects larger and needs |y|/tanY - z of distance. Solving each corner
    // exactly - rather than pairing the worst height with the worst depth -
    // is what stops a tall model from being framed for a box it never fills.
    const Vec3 c = b.center();
    f32 need = 0.0f, hz = 0.0f;
    for (int i = 0; i < 8; ++i) {
        const Vec3 corner((i & 1) ? b.max.x : b.min.x,
                          (i & 2) ? b.max.y : b.min.y,
                          (i & 4) ? b.max.z : b.min.z);
        const Vec3 d  = corner - c;
        const f32  cx = d.dot(right), cy = d.dot(up), cz = d.dot(fwd);
        need = (std::max)(need, std::fabs(cy) / tanY - cz);
        need = (std::max)(need, std::fabs(cx) / tanX - cz);
        hz   = (std::max)(hz, std::fabs(cz));
    }
    if (!(need > 1e-5f)) { fitToBounds(b, marginFactor); return; }

    distance = need * marginFactor;
    // Never end up inside the model.
    if (distance < hz * 1.15f) distance = hz * 1.15f;
    if (!(distance > 1e-4f)) distance = 1.0f;

    const f32 r = (std::max)(b.radius(), 1e-4f);
    sceneRadius = r;                  // projMatrix() re-derives the planes per frame
    zNear = (std::max)(1e-3f, distance * 0.005f);
    zFar  = distance * 8.0f + r * 8.0f;
    if (zFar <= zNear * 1.0001f) zFar = zNear * 1000.0f;
}

void Camera::orbit(f32 dYaw, f32 dPitch) {
    if (freeOrbit) {
        // Historical screen-space arcball, kept verbatim as the escape hatch:
        // it is the only way past the +/-89 pitch clamp to look straight down
        // the pole at a model's underside.
        const Vec3 up    = upVector();
        const Vec3 right = rightVec();
        const Quat delta = qMul(qAxisAngle(up, dYaw), qAxisAngle(right, dPitch));
        orient = qNormalize(qMul(delta, orient));
        return;
    }
    // TURNTABLE. Recover the two angles from `orient`, apply the deltas, then
    // rebuild a LEVEL basis. Nothing accumulates, so roll cannot drift. The old
    // arcball rotated about the camera's OWN up axis, which the 16 degree default
    // pitch tilts off world-Y: a 90 degree horizontal drag decayed the elevation
    // from 16 to 0 and introduced 16 degrees of roll (34.8 on a diagonal drag).
    const Vec3 fwd = forwardVec();

    // Preserve a DELIBERATE Q/E roll: measure it against the level basis for the
    // current direction and re-apply afterwards. The sign is negative - the
    // positive form mirrors the tilt instead of restoring it.
    f32  rollNow  = 0.0f;
    Vec3 lvlRight = Vec3(0.0f, 1.0f, 0.0f).cross(fwd);
    if (lvlRight.length() > 1e-4f) {
        lvlRight = lvlRight.normalized();
        const Vec3 lvlUp = fwd.cross(lvlRight).normalized();
        const Vec3 u     = upVector();
        rollNow = std::atan2(u.dot(lvlRight), u.dot(lvlUp));
    }

    f32 pitch = std::asin((std::max)(-1.0f, (std::min)(1.0f, -fwd.y)));
    f32 yaw   = std::atan2(-fwd.x, fwd.z);

    yaw   = wrapPi(yaw - dYaw);        // matches the caller's orbit(-dx*s, dy*s)
    pitch = clampPitch(pitch + dPitch);

    setView(yaw, pitch);
    if (std::fabs(rollNow) > 1e-4f) roll(-rollNow);
}

void Camera::roll(f32 dRadians) {
    orient = qNormalize(qMul(qAxisAngle(forwardVec(), dRadians), orient));
}

void Camera::levelHorizon() {
    // Keep looking the same way, but re-level the horizon (drop any roll).
    Vec3 fwd = forwardVec();
    Vec3 right = Vec3(0, 1, 0).cross(fwd);
    if (!(right.length() > 1e-4f)) return;    // looking straight up/down: nothing to level
    right = right.normalized();
    const Vec3 up = fwd.cross(right).normalized();
    orient = qFromBasis(right, up, fwd);
}

void Camera::dolly(f32 factor) {
    if (!(factor > 0.0f)) return;
    f32 d = distance * factor;
    // projMatrix() now derives zNear from (distance - sceneRadius). Once the
    // distance drops below the radius the near plane collapses to distance*0.002
    // and the lens slices the model apart instead of filling the frame, so the
    // zoom clamp has to be expressed in the same currency.
    f32 lo = 1e-4f, hi = 1.0e9f;
    if (sceneRadius > 1e-4f) { lo = sceneRadius * 0.35f; hi = sceneRadius * 60.0f; }
    if (lo < 1e-4f) lo = 1e-4f;
    // A fit may already have parked us closer than that floor - hold instead of
    // shoving the camera back out. Clamping must never move the view by itself.
    if (d < lo) d = (std::min)(lo, distance);
    if (d > hi) d = hi;
    distance = d;
}

void Camera::pan(f32 dxPixels, f32 dyPixels, int viewportW, int viewportH) {
    if (viewportW <= 0 || viewportH <= 0) return;

    // World units covered by one pixel at the orbit centre.
    const f32 worldPerPixel =
        (2.0f * distance * std::tan((std::max)(0.05f, fovY) * 0.5f)) / static_cast<f32>(viewportH);

    const Mat4 view = viewMatrix();
    // Rows 0/1 of the view matrix are the camera right / up axes in world space.
    const Vec3 right(view.m[0], view.m[4], view.m[8]);
    const Vec3 up(view.m[1], view.m[5], view.m[9]);

    // Dragging right moves the model right, i.e. the camera moves left.
    panOffset = panOffset - right * (dxPixels * worldPerPixel)
                          + up    * (dyPixels * worldPerPixel);
}

// ===========================================================================
// Rasterizer internals
// ===========================================================================
namespace {

/// A vertex in homogeneous clip space, with everything the shader needs.
struct ClipVtx {
    Vec4 clip;
    Vec3 nrm;      ///< VIEW space
    Vec2 uv;
    Rgba col;
};

inline ClipVtx LerpClip(const ClipVtx& a, const ClipVtx& b, f32 t) {
    ClipVtx r;
    r.clip = Vec4(a.clip.x + (b.clip.x - a.clip.x) * t,
                  a.clip.y + (b.clip.y - a.clip.y) * t,
                  a.clip.z + (b.clip.z - a.clip.z) * t,
                  a.clip.w + (b.clip.w - a.clip.w) * t);
    r.nrm  = Vec3(a.nrm.x + (b.nrm.x - a.nrm.x) * t,
                  a.nrm.y + (b.nrm.y - a.nrm.y) * t,
                  a.nrm.z + (b.nrm.z - a.nrm.z) * t);
    r.uv   = Vec2(a.uv.x + (b.uv.x - a.uv.x) * t,
                  a.uv.y + (b.uv.y - a.uv.y) * t);
    r.col  = Rgba(static_cast<u8>(a.col.r + (b.col.r - a.col.r) * t),
                  static_cast<u8>(a.col.g + (b.col.g - a.col.g) * t),
                  static_cast<u8>(a.col.b + (b.col.b - a.col.b) * t),
                  static_cast<u8>(a.col.a + (b.col.a - a.col.a) * t));
    return r;
}

/// Sutherland-Hodgman against the near plane, expressed as clip.z >= 0.
/// For PerspectiveFovLH that is exactly view.z >= zNear, and it also rejects
/// everything behind the eye (where w would be <= 0). `out` needs room for
/// n + 1 vertices.
int ClipPolygonNear(const ClipVtx* in, int n, ClipVtx* out) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const ClipVtx& a = in[i];
        const ClipVtx& b = in[(i + 1) % n];
        const f32 da = a.clip.z;
        const f32 db = b.clip.z;
        const bool ia = (da >= 0.0f);
        const bool ib = (db >= 0.0f);
        if (ia) out[m++] = a;
        if (ia != ib) {
            const f32 denom = da - db;
            const f32 t = (std::fabs(denom) > 1e-20f) ? (da / denom) : 0.0f;
            out[m++] = LerpClip(a, b, (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t));
        }
    }
    return m;
}

/// Clip a segment against the near plane. Returns false when fully behind.
bool ClipSegmentNear(Vec4& a, Vec4& b) {
    const f32 da = a.z, db = b.z;
    const bool ia = (da >= 0.0f), ib = (db >= 0.0f);
    if (!ia && !ib) return false;
    if (ia && ib) return true;

    const f32 denom = da - db;
    f32 t = (std::fabs(denom) > 1e-20f) ? (da / denom) : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const Vec4 p(a.x + (b.x - a.x) * t,
                 a.y + (b.y - a.y) * t,
                 a.z + (b.z - a.z) * t,
                 a.w + (b.w - a.w) * t);
    if (ia) b = p; else a = p;
    return true;
}

inline f32 Clamp01(f32 v) { return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v); }
inline u8  ToByte(f32 v)  { return static_cast<u8>(Clamp01(v) * 255.0f + 0.5f); }

/// Texture address modes. The public SampleTexture() is the wrap variant;
/// clamp is used internally for lightmap-style single-tile sampling.
enum class Address { Wrap, Clamp };

inline f32 ApplyAddress(f32 t, Address mode) {
    if (mode == Address::Clamp) return Clamp01(t);
    t = t - std::floor(t);
    return (t < 0.0f) ? (t + 1.0f) : t;
}

Rgba SampleTex(const Image& tex, f32 u, f32 v, bool bilinear, Address mode) {
    if (!tex.valid()) return Rgba(255, 255, 255, 255);
    if (!std::isfinite(u) || !std::isfinite(v)) return Rgba(255, 255, 255, 255);

    const int tw = tex.width();
    const int th = tex.height();

    if (!bilinear) {
        int x = static_cast<int>(ApplyAddress(u, mode) * static_cast<f32>(tw));
        int y = static_cast<int>(ApplyAddress(v, mode) * static_cast<f32>(th));
        if (x < 0) x = 0; if (x > tw - 1) x = tw - 1;
        if (y < 0) y = 0; if (y > th - 1) y = th - 1;
        return tex.get(x, y);
    }

    // Bilinear, sampling at texel centres.
    const f32 fx = ApplyAddress(u, mode) * static_cast<f32>(tw) - 0.5f;
    const f32 fy = ApplyAddress(v, mode) * static_cast<f32>(th) - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const f32 tx = fx - static_cast<f32>(x0);
    const f32 ty = fy - static_cast<f32>(y0);

    auto fetch = [&](int x, int y) -> Rgba {
        if (mode == Address::Clamp) {
            if (x < 0) x = 0; if (x > tw - 1) x = tw - 1;
            if (y < 0) y = 0; if (y > th - 1) y = th - 1;
        } else {
            x %= tw; if (x < 0) x += tw;
            y %= th; if (y < 0) y += th;
        }
        return tex.get(x, y);
    };

    const Rgba c00 = fetch(x0, y0),     c10 = fetch(x0 + 1, y0);
    const Rgba c01 = fetch(x0, y0 + 1), c11 = fetch(x0 + 1, y0 + 1);

    auto mix = [&](u8 a, u8 b, u8 c, u8 d) -> u8 {
        const f32 top = static_cast<f32>(a) + (static_cast<f32>(b) - static_cast<f32>(a)) * tx;
        const f32 bot = static_cast<f32>(c) + (static_cast<f32>(d) - static_cast<f32>(c)) * tx;
        const f32 val = top + (bot - top) * ty;
        return static_cast<u8>((val < 0.0f) ? 0.0f : ((val > 255.0f) ? 255.0f : val + 0.5f));
    };
    return Rgba(mix(c00.r, c10.r, c01.r, c11.r),
                mix(c00.g, c10.g, c01.g, c11.g),
                mix(c00.b, c10.b, c01.b, c11.b),
                mix(c00.a, c10.a, c01.a, c11.a));
}

/// A "nice" grid spacing (1, 2, 5 x 10^n) for a model of the given radius.
f32 NiceGridSpacing(f32 radius) {
    if (!(radius > 1e-6f)) return 1.0f;
    const f32 raw = radius / 5.0f;
    const f32 mag = std::pow(10.0f, std::floor(std::log10(raw)));
    const f32 norm = raw / mag;
    f32 step = 1.0f;
    if (norm > 5.0f)      step = 10.0f;
    else if (norm > 2.0f) step = 5.0f;
    else if (norm > 1.0f) step = 2.0f;
    return step * mag;
}

} // namespace

// ---------------------------------------------------------------------------
// Rasterizer::Impl
// ---------------------------------------------------------------------------
// D3DBLEND -> blend factor (reference renderer/legacyBlend.ts:36-84)
//
// Both odd cases are the reference's, not a simplification:
//   * 12 (BOTHSRCALPHA) and 13 (BOTHINVSRCALPHA) set BOTH factors and make
//     dstBlend irrelevant.
//   * 11 (SRCALPHASAT) is source-only; on the destination side it falls
//     through to the default.
// The equation is always ADD - the reference has no subtract/min/max path.
// ---------------------------------------------------------------------------
inline f32 D3dFactor(u32 mode, bool isSrc, f32 srcA, f32 dstA) {
    switch (mode) {
        case 1:  return 0.0f;                       // ZERO
        case 2:  return 1.0f;                       // ONE
        case 5:  return srcA;                       // SRCALPHA
        case 6:  return 1.0f - srcA;                // INVSRCALPHA
        case 7:  return dstA;                       // DESTALPHA
        case 8:  return 1.0f - dstA;                // INVDESTALPHA
        case 11: return isSrc ? (srcA < 1.0f - dstA ? srcA : 1.0f - dstA)
                              : (1.0f - srcA);      // SRCALPHASAT: source only
        default: return isSrc ? srcA : (1.0f - srcA);
    }
}

/// Resolve both factors, honouring the two "both" modes.
inline void D3dFactors(u32 sb, u32 db, f32 srcA, f32 dstA, f32& fs, f32& fd) {
    if (sb == 12) { fs = srcA;        fd = 1.0f - srcA; return; }
    if (sb == 13) { fs = 1.0f - srcA; fd = srcA;        return; }
    fs = D3dFactor(sb, true,  srcA, dstA);
    fd = D3dFactor(db, false, srcA, dstA);
}

// ---------------------------------------------------------------------------
struct Rasterizer::Impl {
    Image            color;
    std::vector<f32> depth;
    int              w = 0;
    int              h = 0;

    bool ready() const { return w > 0 && h > 0 && color.valid() &&
                                depth.size() == static_cast<size_t>(w) * h; }

    // Fill one already-near-clipped triangle.
    void fillTriangle(const ClipVtx& a, const ClipVtx& b, const ClipVtx& c,
                      const Image* tex, const RenderOptions& opt, bool twoSided);

    // Near-clip then fill (or wire-draw) a triangle.
    void triangle(const ClipVtx& a, const ClipVtx& b, const ClipVtx& c,
                  const Image* tex, const RenderOptions& opt, bool twoSided);

    // Screen-space line with depth test, from clip-space endpoints.
    void lineClip(Vec4 a, Vec4 b, Rgba color, bool depthWrite);

    // Depth-TESTED, never depth-WRITTEN, per-vertex alpha blend. Only the
    // contact shadow uses this; the lit path must not go through here (the
    // reference likewise disables lighting and texturing for its floor passes).
    struct BlendVtx { Vec4 clip; f32 a; };
    void blendTriRaw(const BlendVtx& a, const BlendVtx& b, const BlendVtx& c, Rgba col);
    void blendTri(const Vec4& ca, const Vec4& cb, const Vec4& cc,
                  f32 aa, f32 ab, f32 ac, Rgba col);

    void drawWireEdge(const ClipVtx& a, const ClipVtx& b, Rgba c) {
        lineClip(a.clip, b.clip, c, true);
    }
};

void Rasterizer::Impl::fillTriangle(const ClipVtx& v0, const ClipVtx& v1, const ClipVtx& v2,
                                    const Image* tex, const RenderOptions& opt, bool twoSided) {
    if (!ready()) return;

    struct SV {
        f32  x, y, z, invw;
        Vec3 n;
        Vec2 uv;
        Rgba c;
    };
    SV s[3];
    const ClipVtx* src[3] = { &v0, &v1, &v2 };

    const f32 fw = static_cast<f32>(w);
    const f32 fh = static_cast<f32>(h);

    for (int i = 0; i < 3; ++i) {
        const f32 cw = src[i]->clip.w;
        if (!(cw > 1e-7f)) return;                 // behind / on the eye plane
        const f32 iw = 1.0f / cw;
        s[i].invw = iw;
        s[i].x = (src[i]->clip.x * iw * 0.5f + 0.5f) * fw;
        s[i].y = (0.5f - src[i]->clip.y * iw * 0.5f) * fh;
        s[i].z = src[i]->clip.z * iw;
        s[i].n = src[i]->nrm;
        s[i].uv = src[i]->uv;
        s[i].c = src[i]->col;
        if (!std::isfinite(s[i].x) || !std::isfinite(s[i].y) || !std::isfinite(s[i].z)) return;
    }

    // Signed area in screen space. Positive == Silkroad front face (see the
    // worked example in the file header).
    f32 area = (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[1].y - s[0].y) * (s[2].x - s[0].x);
    if (area == 0.0f || !std::isfinite(area)) return;
    if (area < 0.0f) {
        if (opt.backFaceCull && !twoSided) return;
        std::swap(s[1], s[2]);
        area = -area;
    }

    // Screen bounding box, clamped before any float -> int conversion.
    f32 minXf = (std::min)(s[0].x, (std::min)(s[1].x, s[2].x));
    f32 maxXf = (std::max)(s[0].x, (std::max)(s[1].x, s[2].x));
    f32 minYf = (std::min)(s[0].y, (std::min)(s[1].y, s[2].y));
    f32 maxYf = (std::max)(s[0].y, (std::max)(s[1].y, s[2].y));
    if (maxXf < 0.0f || minXf > fw || maxYf < 0.0f || minYf > fh) return;
    if (minXf < 0.0f) minXf = 0.0f;
    if (minYf < 0.0f) minYf = 0.0f;
    if (maxXf > fw)   maxXf = fw;
    if (maxYf > fh)   maxYf = fh;

    int x0 = static_cast<int>(minXf);
    int y0 = static_cast<int>(minYf);
    int x1 = static_cast<int>(maxXf) + 1;
    int y1 = static_cast<int>(maxYf) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    // Clip to this worker's band. Threads own disjoint bands, so a triangle
    // that spans several of them is filled once per band and no pixel is ever
    // written twice.
    if (y0 < opt.bandTop) y0 = opt.bandTop;
    // The band is HALF-OPEN, [bandTop, bandBot), but this loop is inclusive of
    // y1 - so clamping to bandBot itself hands that row to two neighbouring
    // bands at once. With opaque drawing the duplicate is invisible (same pixel,
    // same colour); with ADDITIVE blending it is added twice and the boundary
    // row comes out at double brightness, drawing a bright horizontal line
    // across the effect every  rows.
    if (y1 > opt.bandBot - 1) y1 = opt.bandBot - 1;
    if (y1 < y0) return;
    if (x1 > w - 1) x1 = w - 1;
    if (y1 > h - 1) y1 = h - 1;
    if (x1 < x0 || y1 < y0) return;

    // Edge functions:  E(x,y) = ea*x + eb*y + ec , positive inside.
    struct Edge { f32 a, b, c; bool topLeft; };
    auto makeEdge = [](const SV& p, const SV& q) -> Edge {
        const f32 dx = q.x - p.x;
        const f32 dy = q.y - p.y;
        Edge e;
        e.a = -dy;
        e.b =  dx;
        e.c =  dy * p.x - dx * p.y;
        // Screen y grows downwards, front faces are CW: a left edge has dy < 0
        // and a top edge is horizontal with dx > 0.
        e.topLeft = (dy < 0.0f) || (dy == 0.0f && dx > 0.0f);
        return e;
    };
    const Edge e12 = makeEdge(s[1], s[2]);   // barycentric weight of vertex 0
    const Edge e20 = makeEdge(s[2], s[0]);   // ... of vertex 1
    const Edge e01 = makeEdge(s[0], s[1]);   // ... of vertex 2

    const f32 invArea = 1.0f / area;
    const f32 ambient = Clamp01(opt.ambient);
    const bool wantTexture = (opt.shading == ShadingMode::Textured) && tex && tex->valid();
    const bool normalsMode = (opt.shading == ShadingMode::Normals);

    u8* const  pixels = color.data();
    f32* const zbuf   = depth.data();

    for (int y = y0; y <= y1; ++y) {
        const f32 py = static_cast<f32>(y) + 0.5f;
        const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(w);

        f32 r12 = e12.a * (static_cast<f32>(x0) + 0.5f) + e12.b * py + e12.c;
        f32 r20 = e20.a * (static_cast<f32>(x0) + 0.5f) + e20.b * py + e20.c;
        f32 r01 = e01.a * (static_cast<f32>(x0) + 0.5f) + e01.b * py + e01.c;

        for (int x = x0; x <= x1; ++x, r12 += e12.a, r20 += e20.a, r01 += e01.a) {
            // Top-left fill rule: strictly inside always wins; exactly on an
            // edge only counts for top and left edges, so shared edges are
            // rasterised once.
            if (!((r12 > 0.0f || (r12 == 0.0f && e12.topLeft)) &&
                  (r20 > 0.0f || (r20 == 0.0f && e20.topLeft)) &&
                  (r01 > 0.0f || (r01 == 0.0f && e01.topLeft))))
                continue;

            const f32 b0 = r12 * invArea;
            const f32 b1 = r20 * invArea;
            const f32 b2 = r01 * invArea;

            const f32 z = b0 * s[0].z + b1 * s[1].z + b2 * s[2].z;
            const size_t idx = rowBase + static_cast<size_t>(x);
            if (!(z < zbuf[idx])) continue;

            // Perspective-correct attributes.
            const f32 iw = b0 * s[0].invw + b1 * s[1].invw + b2 * s[2].invw;
            const f32 wv = (std::fabs(iw) > 1e-20f) ? (1.0f / iw) : 0.0f;

            Vec3 n(
                (b0 * s[0].n.x * s[0].invw + b1 * s[1].n.x * s[1].invw + b2 * s[2].n.x * s[2].invw) * wv,
                (b0 * s[0].n.y * s[0].invw + b1 * s[1].n.y * s[1].invw + b2 * s[2].n.y * s[2].invw) * wv,
                (b0 * s[0].n.z * s[0].invw + b1 * s[1].n.z * s[1].invw + b2 * s[2].n.z * s[2].invw) * wv);
            n = n.normalized();

            f32 outR, outG, outB;
            u8  outA = 255;
            f32 texA = 1.0f;              // sampled alpha, for blended meshes
            f32 tR = 1.0f, tG = 1.0f, tB = 1.0f;   // raw texel, for the emissive term

            if (normalsMode) {
                outR = n.x * 0.5f + 0.5f;
                outG = n.y * 0.5f + 0.5f;
                outB = n.z * 0.5f + 0.5f;
            } else {
                // 3-POINT STUDIO LIGHTING, ported 1:1 from the reference
                // viewer's studio block. GL_LIGHT_MODEL_TWO_SIDE: a normal
                // pointing away from the eye is flipped rather than going
                // black (culling is off and SRO ships inverted normals).
                // Per channel: colour*(ambient + key*NdotL + rim*NdotL), then
                // the specular lobe is ADDED after texturing.
                f32 litR, litG, litB, spc = 0.0f;
                if (opt.unlit) {
                    // MeshBasicMaterial: a particle sprite is emissive, not lit.
                    // Running it through the studio rig adds 0.88 ambient plus a
                    // key and a rim term and washes it toward flat white - which
                    // is half of why a stack of them saturated the frame.
                    litR = litG = litB = 1.0f;
                    spc  = 0.0f;
                } else if (opt.studioLight) {
                    Vec3 nn = n;
                    if (nn.z > 0.0f) { nn.x = -nn.x; nn.y = -nn.y; nn.z = -nn.z; }
                    f32 kd = nn.x * opt.keyDirEye.x + nn.y * opt.keyDirEye.y + nn.z * opt.keyDirEye.z;
                    if (kd < 0.0f) kd = 0.0f;
                    f32 rd = nn.x * opt.rimDirEye.x + nn.y * opt.rimDirEye.y + nn.z * opt.rimDirEye.z;
                    if (rd < 0.0f) rd = 0.0f;
                    litR = opt.lightAmbient.x + opt.keyDiffuse.x * kd + opt.rimDiffuse.x * rd;
                    litG = opt.lightAmbient.y + opt.keyDiffuse.y * kd + opt.rimDiffuse.y * rd;
                    litB = opt.lightAmbient.z + opt.keyDiffuse.z * kd + opt.rimDiffuse.z * rd;
                    if (kd > 0.0f) {
                        f32 nh = nn.x * opt.keyHalfEye.x + nn.y * opt.keyHalfEye.y + nn.z * opt.keyHalfEye.z;
                        if (nh > 0.0f) spc = std::pow(nh, opt.shininess);
                    }
                } else {
                    f32 nl = -n.z;
                    if (nl < 0.0f) nl = -nl;
                    litR = litG = litB = ambient + (1.0f - ambient) * nl;
                }

                f32 cr = (b0 * static_cast<f32>(s[0].c.r) + b1 * static_cast<f32>(s[1].c.r) +
                          b2 * static_cast<f32>(s[2].c.r)) * (1.0f / 255.0f);
                f32 cg = (b0 * static_cast<f32>(s[0].c.g) + b1 * static_cast<f32>(s[1].c.g) +
                          b2 * static_cast<f32>(s[2].c.g)) * (1.0f / 255.0f);
                f32 cb = (b0 * static_cast<f32>(s[0].c.b) + b1 * static_cast<f32>(s[1].c.b) +
                          b2 * static_cast<f32>(s[2].c.b)) * (1.0f / 255.0f);

                if (wantTexture) {
                    const f32 u = (b0 * s[0].uv.x * s[0].invw + b1 * s[1].uv.x * s[1].invw +
                                   b2 * s[2].uv.x * s[2].invw) * wv;
                    const f32 v = (b0 * s[0].uv.y * s[0].invw + b1 * s[1].uv.y * s[1].invw +
                                   b2 * s[2].uv.y * s[2].invw) * wv;
                    const Rgba t = SampleTex(*tex, u, v, opt.bilinear, Address::Wrap);
                    // Alpha cut-out (SHARE ddsAlphaTest logic): discard texels below
                    // the per-mesh threshold so DXT1 punch-through and baked
                    // transparent-black areas don't render as black ("siyahlik"),
                    // while metallic-sheen alpha stays fully opaque (threshold 0).
                    if (opt.alphaTest > 0.0f &&
                        t.a < static_cast<u8>(opt.alphaTest * 255.0f + 0.5f)) continue;
                    // Optional true alpha-blend test (user toggle), independent of cut-out.
                    if (opt.honourTextureAlpha && t.a < 128) continue;
                    tR = static_cast<f32>(t.r) * (1.0f / 255.0f);
                    tG = static_cast<f32>(t.g) * (1.0f / 255.0f);
                    tB = static_cast<f32>(t.b) * (1.0f / 255.0f);
                    cr *= tR; cg *= tG; cb *= tB;
                    if (opt.honourTextureAlpha) outA = t.a;
                    // The source alpha is needed by EVERY blend path, not just
                    // the alpha one: SRCALPHA->ONE multiplies by it too. Reading
                    // it only for blendAlpha is what let additive sprites stack
                    // at full luminance and saturate the frame to white.
                    if (opt.blendAlpha || opt.additive || opt.srcBlend)
                        texA = static_cast<f32>(t.a) * (1.0f / 255.0f);
                }
                // Chromatic diffuse tint (material flag 0x40), normalised at load
                // so it shifts hue rather than darkening the model.
                cr *= opt.tint.x; cg *= opt.tint.y; cb *= opt.tint.z;
                // Specular is SEPARATE: added on top of the textured result,
                // never modulated by it (GL_SEPARATE_SPECULAR_COLOR).
                // Emissive rides the TEXTURE factor: in the D3D chain it lands in
                // the vertex colour, which the texture stage then modulates, so a
                // black texel emits nothing. Added flat it washed every dark region
                // of a self-lit material to a flat colour - worst on a black glow
                // panel, which became a solid coloured quad.
                outR = cr * litR + tR * opt.emissive.x + opt.matSpecular.x * opt.keySpecular.x * spc;
                outG = cg * litG + tG * opt.emissive.y + opt.matSpecular.y * opt.keySpecular.y * spc;
                outB = cb * litB + tB * opt.emissive.z + opt.matSpecular.z * opt.keySpecular.z * spc;

                // Fog tints a fragment toward the backdrop; an emissive sprite
                // must not be tinted at all, so unlit skips it outright.
                if (!opt.unlit && opt.fogEnabled && opt.fogEnd > opt.fogStart) {
                    // GL_LINEAR fog over view-space depth. `wv` is the
                    // interpolated clip w, which for PerspectiveFovLH (m[11]==1,
                    // m[15]==0) IS the view-space z.
                    f32 fg = (wv - opt.fogStart) / (opt.fogEnd - opt.fogStart);
                    if (fg < 0.0f) fg = 0.0f; else if (fg > 1.0f) fg = 1.0f;
                    outR += (opt.fogColor.x - outR) * fg;
                    outG += (opt.fogColor.y - outG) * fg;
                    outB += (opt.fogColor.z - outB) * fg;
                }

            }

            u8* px = pixels + idx * 4;

            // The Normals debug view writes normal-as-RGB; compositing that over
            // the backdrop corrupts the very thing the mode exists to show.
            if (normalsMode) {
                // no blend
            } else if (opt.srcBlend) {
                // Authored D3D render states (particle effects). srcA carries
                // BOTH the texture's alpha and the emitter's diffuse-curve
                // opacity, which is what makes a sprite fade instead of merely
                // getting smaller.
                f32 srcA = texA * opt.opacity;
                if (srcA < 0.0f) srcA = 0.0f;
                if (srcA > 1.0f) srcA = 1.0f;
                f32 fs = 1.0f, fd = 1.0f;
                D3dFactors(opt.srcBlend, opt.dstBlend, srcA, 1.0f, fs, fd);
                outR = outR * fs + (px[0] * (1.0f / 255.0f)) * fd;
                outG = outG * fs + (px[1] * (1.0f / 255.0f)) * fd;
                outB = outB * fs + (px[2] * (1.0f / 255.0f)) * fd;
            } else if (opt.additive) {
                // D3DTOP_ADD with its source alpha, i.e. SRCALPHA -> ONE.
                f32 a = texA * opt.opacity;
                if (a < 0.0f) a = 0.0f;
                if (a > 1.0f) a = 1.0f;
                outR = outR * a + px[0] * (1.0f / 255.0f);
                outG = outG * a + px[1] * (1.0f / 255.0f);
                outB = outB * a + px[2] * (1.0f / 255.0f);
            } else if (opt.blendAlpha) {
                f32 a = texA * opt.opacity;
                if (a <= 0.0f) continue;
                if (a > 1.0f) a = 1.0f;
                outR += (px[0] * (1.0f / 255.0f) - outR) * (1.0f - a);
                outG += (px[1] * (1.0f / 255.0f) - outG) * (1.0f - a);
                outB += (px[2] * (1.0f / 255.0f) - outB) * (1.0f - a);
            }

            // The blended pass depth-TESTS but never depth-WRITES, so two
            // translucent layers cannot occlude each other by draw order.
            if (opt.depthWrite) zbuf[idx] = z;
            px[0] = ToByte(outR);
            px[1] = ToByte(outG);
            px[2] = ToByte(outB);
            px[3] = outA;
        }
    }
}

void Rasterizer::Impl::triangle(const ClipVtx& a, const ClipVtx& b, const ClipVtx& c,
                                const Image* tex, const RenderOptions& opt, bool twoSided) {
    if (!ready()) return;

    const ClipVtx in[3] = { a, b, c };
    ClipVtx poly[8];
    const int n = ClipPolygonNear(in, 3, poly);
    if (n < 3) return;

    if (opt.shading == ShadingMode::Wireframe) {
        const Rgba wire(0xE8, 0xC8, 0x4A, 0xFF);   // Theme GOLD_LIGHT
        for (int i = 0; i < n; ++i) drawWireEdge(poly[i], poly[(i + 1) % n], wire);
        return;
    }

    for (int i = 1; i + 1 < n; ++i)
        fillTriangle(poly[0], poly[i], poly[i + 1], tex, opt, twoSided);
}

void Rasterizer::Impl::lineClip(Vec4 a, Vec4 b, Rgba col, bool depthWrite) {
    if (!ready()) return;
    if (!ClipSegmentNear(a, b)) return;
    if (!(a.w > 1e-7f) || !(b.w > 1e-7f)) return;

    const f32 fw = static_cast<f32>(w);
    const f32 fh = static_cast<f32>(h);
    const f32 iwa = 1.0f / a.w, iwb = 1.0f / b.w;

    const f32 ax = (a.x * iwa * 0.5f + 0.5f) * fw;
    const f32 ay = (0.5f - a.y * iwa * 0.5f) * fh;
    const f32 az = a.z * iwa;
    const f32 bx = (b.x * iwb * 0.5f + 0.5f) * fw;
    const f32 by = (0.5f - b.y * iwb * 0.5f) * fh;
    const f32 bz = b.z * iwb;

    if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az) ||
        !std::isfinite(bx) || !std::isfinite(by) || !std::isfinite(bz)) return;

    // Reject segments that cannot touch the viewport, so the step count below
    // stays bounded.
    if ((ax < 0.0f && bx < 0.0f) || (ax > fw && bx > fw) ||
        (ay < 0.0f && by < 0.0f) || (ay > fh && by > fh)) return;

    const f32 dx = bx - ax, dy = by - ay;
    f32 span = (std::max)(std::fabs(dx), std::fabs(dy));
    if (span > 8192.0f) span = 8192.0f;
    const int steps = (std::max)(1, static_cast<int>(span));
    const f32 invSteps = 1.0f / static_cast<f32>(steps);

    u8* const  pixels = color.data();
    f32* const zbuf   = depth.data();

    for (int i = 0; i <= steps; ++i) {
        const f32 t = static_cast<f32>(i) * invSteps;
        const int px = static_cast<int>(ax + dx * t + 0.5f);
        const int py = static_cast<int>(ay + dy * t + 0.5f);
        if (px < 0 || py < 0 || px >= w || py >= h) continue;
        // Nudge lines towards the eye so a wireframe/grid drawn over a surface
        // is not swallowed by z-fighting.
        const f32 z = (az + (bz - az) * t) - 1e-5f;
        const size_t idx = static_cast<size_t>(py) * static_cast<size_t>(w) + static_cast<size_t>(px);
        if (!(z < zbuf[idx])) continue;
        if (depthWrite) zbuf[idx] = z;
        u8* p = pixels + idx * 4;
        p[0] = col.r; p[1] = col.g; p[2] = col.b; p[3] = 255;
    }
}

void Rasterizer::Impl::blendTri(const Vec4& ca, const Vec4& cb, const Vec4& cc,
                                f32 aa, f32 ab, f32 ac, Rgba col) {
    if (!ready()) return;
    // Clip against the near plane instead of DISCARDING a triangle that crosses
    // it. Dropping them whole turned the soft contact shadow into a wedge with
    // hard straight radial edges as soon as the camera came close and level.
    // Alpha interpolates linearly along the cut edge, matching the screen-space
    // barycentric blend used in blendTriRaw.
    const BlendVtx in[3] = { { ca, aa }, { cb, ab }, { cc, ac } };
    BlendVtx poly[4];
    int m = 0;
    for (int i = 0; i < 3; ++i) {
        const BlendVtx& a = in[i];
        const BlendVtx& b = in[(i + 1) % 3];
        const bool ia = (a.clip.z >= 0.0f), ib = (b.clip.z >= 0.0f);
        if (ia) poly[m++] = a;
        if (ia != ib && m < 4) {
            const f32 den = a.clip.z - b.clip.z;
            f32 t = (std::fabs(den) > 1e-20f) ? (a.clip.z / den) : 0.0f;
            if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
            BlendVtx r;
            r.clip = Vec4(a.clip.x + (b.clip.x - a.clip.x) * t,
                          a.clip.y + (b.clip.y - a.clip.y) * t,
                          a.clip.z + (b.clip.z - a.clip.z) * t,
                          a.clip.w + (b.clip.w - a.clip.w) * t);
            r.a = a.a + (b.a - a.a) * t;
            poly[m++] = r;
        }
    }
    if (m < 3) return;
    for (int i = 1; i + 1 < m; ++i) blendTriRaw(poly[0], poly[i], poly[i + 1], col);
}

void Rasterizer::Impl::blendTriRaw(const BlendVtx& va, const BlendVtx& vb,
                                   const BlendVtx& vc, Rgba col) {
    if (!ready()) return;
    struct P { f32 x, y, z, a; } p[3];
    const Vec4 cv[3] = { va.clip, vb.clip, vc.clip };
    const f32  av[3] = { va.a, vb.a, vc.a };
    for (int i = 0; i < 3; ++i) {
        if (!(cv[i].w > 1e-7f)) return;                 // behind the eye plane
        const f32 iw = 1.0f / cv[i].w;
        p[i].x = (cv[i].x * iw * 0.5f + 0.5f) * static_cast<f32>(w);
        p[i].y = (0.5f - cv[i].y * iw * 0.5f) * static_cast<f32>(h);
        p[i].z = cv[i].z * iw;
        p[i].a = av[i];
        if (!std::isfinite(p[i].x) || !std::isfinite(p[i].y) || !std::isfinite(p[i].z)) return;
    }
    f32 area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[1].y - p[0].y) * (p[2].x - p[0].x);
    if (area == 0.0f || !std::isfinite(area)) return;
    if (area < 0.0f) { std::swap(p[1], p[2]); area = -area; }

    // Clamp in FLOAT before the int cast, exactly like fillTriangle does.
    const f32 fw = static_cast<f32>(w), fh = static_cast<f32>(h);
    f32 minXf = (std::min)(p[0].x, (std::min)(p[1].x, p[2].x));
    f32 maxXf = (std::max)(p[0].x, (std::max)(p[1].x, p[2].x));
    f32 minYf = (std::min)(p[0].y, (std::min)(p[1].y, p[2].y));
    f32 maxYf = (std::max)(p[0].y, (std::max)(p[1].y, p[2].y));
    if (maxXf < 0.0f || minXf > fw || maxYf < 0.0f || minYf > fh) return;
    if (minXf < 0.0f) minXf = 0.0f;
    if (minYf < 0.0f) minYf = 0.0f;
    if (maxXf > fw)   maxXf = fw;
    if (maxYf > fh)   maxYf = fh;
    int x0 = static_cast<int>(minXf), y0 = static_cast<int>(minYf);
    int x1 = static_cast<int>(maxXf) + 1, y1 = static_cast<int>(maxYf) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w - 1) x1 = w - 1;
    if (y1 > h - 1) y1 = h - 1;
    if (x1 < x0 || y1 < y0) return;

    const f32 inv = 1.0f / area;
    u8* const  pixels = color.data();
    f32* const zbuf   = depth.data();
    for (int y = y0; y <= y1; ++y) {
        const f32 py = static_cast<f32>(y) + 0.5f;
        for (int x = x0; x <= x1; ++x) {
            const f32 px = static_cast<f32>(x) + 0.5f;
            const f32 b0 = ((p[1].x - px) * (p[2].y - py) - (p[1].y - py) * (p[2].x - px)) * inv;
            const f32 b1 = ((p[2].x - px) * (p[0].y - py) - (p[2].y - py) * (p[0].x - px)) * inv;
            const f32 b2 = 1.0f - b0 - b1;
            if (b0 < 0.0f || b1 < 0.0f || b2 < 0.0f) continue;
            const f32 z = b0 * p[0].z + b1 * p[1].z + b2 * p[2].z;
            const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
            if (!(z < zbuf[idx])) continue;             // depth TEST, no write
            f32 a = b0 * p[0].a + b1 * p[1].a + b2 * p[2].a;
            if (a <= 0.0f) continue;
            if (a > 1.0f) a = 1.0f;
            u8* d = pixels + idx * 4;
            d[0] = static_cast<u8>(d[0] + (static_cast<f32>(col.r) - d[0]) * a);
            d[1] = static_cast<u8>(d[1] + (static_cast<f32>(col.g) - d[1]) * a);
            d[2] = static_cast<u8>(d[2] + (static_cast<f32>(col.b) - d[2]) * a);
        }
    }
}

// ---------------------------------------------------------------------------
// Rasterizer - public surface

// ---------------------------------------------------------------------------
Rasterizer::Rasterizer() : m_impl(new Impl()) {}
Rasterizer::~Rasterizer() = default;

bool Rasterizer::resize(int width, int height) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        m_impl->color.clear();
        m_impl->depth.clear();
        m_impl->w = m_impl->h = 0;
        return false;
    }
    if (!m_impl->color.resize(width, height)) {
        m_impl->depth.clear();
        m_impl->w = m_impl->h = 0;
        return false;
    }
    try {
        m_impl->depth.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 1e30f);
    } catch (...) {
        m_impl->color.clear();
        m_impl->depth.clear();
        m_impl->w = m_impl->h = 0;
        return false;
    }
    m_impl->w = width;
    m_impl->h = height;
    return true;
}

int Rasterizer::width()  const { return m_impl->w; }
int Rasterizer::height() const { return m_impl->h; }

void Rasterizer::clear(Rgba bg) {
    if (!m_impl->color.valid()) return;
    m_impl->color.fill(bg);
    std::fill(m_impl->depth.begin(), m_impl->depth.end(), 1e30f);
}

// Port of the SHARE reference viewer's ddsAlphaTest(), run on the DECODED RGBA
// texture (format-independent). Tallies opaque / low-alpha / low-alpha-black / mid
// texels, then classifies:
//   * binary alpha + a real opaque shape + some transparent gaps -> 0.5 (cut-out)
//   * mostly-black transparent recesses                          -> 0.06 (trim gaps)
//   * continuous sheen/gloss gradient, or unused alpha           -> 0.0 (opaque)
f32 ComputeTextureAlphaTest(const Image& img) {
    if (!img.valid()) return 0.0f;
    const int w = img.width(), h = img.height();
    const long long total = static_cast<long long>(w) * h;
    if (total <= 0) return 0.0f;

    long long opaque = 0, low = 0, lowBlack = 0, mid = 0;
    for (int y = 0; y < h; ++y) {
        const u8* p = img.row(y);
        if (!p) continue;
        for (int x = 0; x < w; ++x, p += 4) {
            const u8 r = p[0], g = p[1], b = p[2], a = p[3];
            if (a >= 250) ++opaque;
            if (a < 128) { ++low; if (r < 32 && g < 32 && b < 32) ++lowBlack; }
            if (a >= 24 && a <= 232) ++mid;
        }
    }

    const double dt = static_cast<double>(total);
    const bool binary = static_cast<double>(mid) / dt < 0.10;
    // Real cut-out: binary alpha, a substantial opaque shape, some transparent gaps.
    if (binary && low > total * 0.004 && opaque > total * 0.20) return 0.5f;
    // Black transparent recesses (gems/glow): trim only the fully-transparent black.
    if (low > total * 0.05 && low > 0 &&
        static_cast<double>(lowBlack) / static_cast<double>(low) > 0.35) return 0.06f;
    // Everything else (metallic sheen gradient, or unused alpha) stays opaque.
    return 0.0f;
}

bool Rasterizer::render(const Scene& scene, const Camera& camera, const RenderOptions& optIn) {
    if (!m_impl->ready()) return false;

    // The reference sets glLightfv(GL_POSITION) while the MODELVIEW holds the
    // camera, so its directional lights are fixed in WORLD space and the driver
    // rotates them into eye space once per frame. Do exactly that here.
    RenderOptions opt = optIn;
    {
        const Mat4 v = camera.viewMatrix();
        opt.keyDirEye = v.transformVector(opt.keyDirWorld).normalized();
        opt.rimDirEye = v.transformVector(opt.rimDirWorld).normalized();
        // Infinite viewer (GL default, LOCAL_VIEWER off): the eye vector is a
        // constant. This rasteriser looks down +Z, so the viewer is at -Z.
        Vec3 h(opt.keyDirEye.x, opt.keyDirEye.y, opt.keyDirEye.z - 1.0f);
        opt.keyHalfEye = h.normalized();
    }
    {
        // Reference main.cpp:4111-4112 anchors fog to the CAMERA DISTANCE rather
        // than a fixed world scale, so it behaves the same on a 20-unit item and
        // a 2000-unit building; the far plane above is sized to cover exactly
        // this range. The colour is welded to the backdrop so the floor melts
        // into it with no visible border.
        const f32 fr = scene.bounds.valid() ? scene.bounds.radius() : camera.distance * 0.25f;
        opt.fogStart = camera.distance * 1.6f;
        opt.fogEnd   = camera.distance * 6.0f + fr * 8.0f;
        const Rgba& fb = opt.gradientBackground ? opt.backgroundBottom : opt.background;
        opt.fogColor = Vec3(fb.r / 255.0f, fb.g / 255.0f, fb.b / 255.0f);
    }

    if (opt.gradientBackground) {
        // Soft vertical studio gradient, then reset depth.
        const int W = m_impl->w, H = m_impl->h;
        const Rgba& t = opt.background; const Rgba& b = opt.backgroundBottom;
        for (int y = 0; y < H; ++y) {
            const f32 f = (H > 1) ? static_cast<f32>(y) / static_cast<f32>(H - 1) : 0.0f;
            Rgba row;
            row.r = static_cast<u8>(t.r + (b.r - t.r) * f);
            row.g = static_cast<u8>(t.g + (b.g - t.g) * f);
            row.b = static_cast<u8>(t.b + (b.b - t.b) * f);
            row.a = 0xFF;
            u8* px = m_impl->color.at(0, y);
            if (!px) continue;
            for (int x = 0; x < W; ++x, px += 4) { px[0]=row.r; px[1]=row.g; px[2]=row.b; px[3]=row.a; }
        }
        std::fill(m_impl->depth.begin(), m_impl->depth.end(), 1e30f);
    } else {
        clear(opt.background);
    }

    const Mat4 view = camera.viewMatrix();
    const Mat4 proj = camera.projMatrix(m_impl->w, m_impl->h);
    const Mat4 vp   = view * proj;

    // --- studio floor, drawn UNDER the model (reference main.cpp:4144-4182) --
    f32 radius = scene.bounds.valid() ? scene.bounds.radius() : camera.distance * 0.4f;
    if (!(radius > 1e-5f)) radius = 1.0f;
    const Vec3 ctr = scene.bounds.valid() ? scene.bounds.center() : camera.target;

    if (opt.studioFloor && scene.bounds.valid()) {
        // DELIBERATE DEVIATION from the reference's floorY = center.y - radius:
        // radius is the half-DIAGONAL, so for a long quadruped (this bear is 40
        // units deep) that plane sits well BELOW the paws and the animal floats.
        // bounds.min.y puts the shadow exactly under the feet.
        const f32  floorY = scene.bounds.min.y;
        const Vec3 eye    = camera.eyePosition();
        const f32  fogA   = camera.distance * 1.6f;
        const f32  fogB   = camera.distance * 6.0f + radius * 8.0f;
        const Rgba bgT    = opt.background;
        const Rgba bgB    = opt.gradientBackground ? opt.backgroundBottom : opt.background;
        const f32  fh     = static_cast<f32>(m_impl->h);

        // The reference fades its floor with GL alpha over a fogged backdrop.
        // There is no line blending here, so fold BOTH fades into the colour and
        // resolve against the gradient row the segment actually lands on.
        auto fogLine = [&](const Vec3& a, const Vec3& b, f32 r0, f32 g0, f32 b0, f32 alpha) {
            const int kSeg = 12;
            for (int s = 0; s < kSeg; ++s) {
                const f32 t0 = static_cast<f32>(s)     / kSeg;
                const f32 t1 = static_cast<f32>(s + 1) / kSeg;
                const Vec3 p0(a.x + (b.x-a.x)*t0, a.y + (b.y-a.y)*t0, a.z + (b.z-a.z)*t0);
                const Vec3 p1(a.x + (b.x-a.x)*t1, a.y + (b.y-a.y)*t1, a.z + (b.z-a.z)*t1);
                const Vec3 mid((p0.x+p1.x)*0.5f, (p0.y+p1.y)*0.5f, (p0.z+p1.z)*0.5f);
                const Vec3 d(mid.x - eye.x, mid.y - eye.y, mid.z - eye.z);
                f32 f = (fogB > fogA) ? ((d.length() - fogA) / (fogB - fogA)) : 0.0f;
                if (f < 0.0f) f = 0.0f; else if (f > 1.0f) f = 1.0f;
                const f32 k = alpha * (1.0f - f);
                f32 sy = 0.5f;
                const Vec4 cp = vp.transformVec4(Vec4(mid.x, mid.y, mid.z, 1.0f));
                if (cp.w > 1e-7f && fh > 1.0f) {
                    sy = 0.5f - (cp.y / cp.w) * 0.5f;
                    if (sy < 0.0f) sy = 0.0f; else if (sy > 1.0f) sy = 1.0f;
                }
                const f32 br = bgT.r + (static_cast<f32>(bgB.r) - bgT.r) * sy;
                const f32 bg = bgT.g + (static_cast<f32>(bgB.g) - bgT.g) * sy;
                const f32 bb = bgT.b + (static_cast<f32>(bgB.b) - bgT.b) * sy;
                drawLine3D(p0, p1, vp,
                           Rgba(static_cast<u8>(br + (r0 * 255.0f - br) * k),
                                static_cast<u8>(bg + (g0 * 255.0f - bg) * k),
                                static_cast<u8>(bb + (b0 * 255.0f - bb) * k), 255));
            }
        };

        // The three floor layers are separated by a fraction of the SCENE, not by
        // fixed hundredths: a fixed 0.03 is meaningless both for a sub-unit item
        // and for a 2000-unit building, and lineClip nudges lines 1e-5 toward the
        // eye, which wins over too small a gap at a low camera and stripes them
        // back through the shadow.
        const f32 lift = (std::max)(0.02f, radius * 0.002f);

        // 1) floor grid: brightest under the model, dissolving at the rim
        const f32 gs = radius * 6.0f;
        const int gn = 24;
        const f32 gy = floorY + lift;
        for (int i = -gn; i <= gn; ++i) {
            const f32 t    = gs * static_cast<f32>(i) / static_cast<f32>(gn);
            const int ai   = (i < 0) ? -i : i;
            const f32 fade = 1.0f - static_cast<f32>(ai) / static_cast<f32>(gn);
            const f32 a    = 0.20f + 0.50f * fade;
            fogLine(Vec3(t + ctr.x, gy, -gs + ctr.z), Vec3(t + ctr.x, gy, gs + ctr.z),
                    0.62f, 0.66f, 0.72f, a);
            fogLine(Vec3(-gs + ctr.x, gy, t + ctr.z), Vec3(gs + ctr.x, gy, t + ctr.z),
                    0.62f, 0.66f, 0.72f, a);
        }

        // 2) gold podium rings
        const f32 ry    = floorY + lift * 2.0f;
        const f32 rr[2] = { radius * 0.55f, radius * 0.63f };
        const f32 ra[2] = { 0.70f, 0.40f };
        for (int ring = 0; ring < 2; ++ring) {
            Vec3 prev;
            bool have = false;
            for (int i = 0; i <= 48; ++i) {
                const f32 a = static_cast<f32>(i) * 6.2831853f / 48.0f;
                const Vec3 p(ctr.x + std::cos(a) * rr[ring], ry, ctr.z + std::sin(a) * rr[ring]);
                if (have) fogLine(prev, p, 0.80f, 0.62f, 0.20f, ra[ring]);
                prev = p;
                have = true;
            }
        }

        // 3) soft radial contact shadow, drawn LAST. blendTri depth-tests but
        //    never depth-WRITES, so anything drawn after it stamps straight
        //    through: with the shadow first, the grid punched a bright lattice
        //    across it and the "shadow" read as a dark disc cut into stripes.
        drawContactShadow(Vec3(ctr.x, floorY + lift * 3.0f, ctr.z), radius * 0.55f, vp, 0.28f);
    }

    // --- debug helpers, unchanged toggles ('G' / 'X' / 'B') -----------------
    if (opt.drawGrid) {
        const f32 spacing = NiceGridSpacing(radius);
        drawGrid(vp, spacing, 10, Rgba(0x4A, 0x35, 0x20, 0xFF));
    }
    if (opt.drawAxes)   drawAxes(vp, radius * 1.2f);
    if (opt.drawBounds && scene.bounds.valid())
        drawAabb(scene.bounds, vp, Rgba(0x7A, 0x5C, 0x1F, 0xFF));

    // --- meshes, in TWO passes (reference Model.h renderBody) ---------------
    // Pass 0 draws the opaque and cut-out meshes with depth WRITE on; pass 1
    // draws everything blended or additive with depth write OFF, painter style
    // over the solid body. Drawing a cape or a glow panel in one pass either
    // makes it opaque or lets it punch a hole in whatever is behind it.
    //
    // The whole thing runs once per SCANLINE BAND, and the bands run on separate
    // threads. Bands are disjoint, so two threads can never touch the same pixel
    // or the same depth slot and no locking is needed; each band still does both
    // passes in order, which is exactly the serial result for those rows.
    auto drawMeshBand = [&](int bandTop, int bandBot) {
    for (int pass = 0; pass < 2; ++pass) {
    const bool blendPass = (pass == 1);
    for (const RasterMesh& mesh : scene.meshes) {
        if (mesh.vertices.empty() || mesh.indices.size() < 3) continue;
        // A mesh carrying authored D3D blend states is transparent too. Judging
        // only by the two legacy bools put every particle plate in the OPAQUE
        // pass, where it wrote depth and z-fought its neighbours - which is what
        // tore a dense effect into horizontal dashes.
        const bool meshBlends = mesh.blendAlpha || mesh.additive || mesh.srcBlend != 0;
        if (meshBlends != blendPass) continue;
        // An additive panel is a pure GLOW CARD with no shape of its own. Drawn
        // in a mode that never samples its texture it is either blown out to a
        // white rectangle over the weapon (cr=cg=cb=1 plus the backdrop) or an
        // opaque grey quad. Skipping it is the only outcome that leaves the
        // model readable in the Flat / Wireframe / Normals debug views.
        if (blendPass && mesh.additive && opt.shading != ShadingMode::Textured) continue;

        const Mat4 mvp = mesh.transform * vp;
        const Mat4 mv  = mesh.transform * view;   // object -> view, for normals

        const Image* tex = nullptr;
        if (mesh.textureIndex >= 0 &&
            static_cast<size_t>(mesh.textureIndex) < scene.textures.size() &&
            scene.textures[static_cast<size_t>(mesh.textureIndex)].valid()) {
            tex = &scene.textures[static_cast<size_t>(mesh.textureIndex)];
        }

        const size_t vcount = mesh.vertices.size();
        const size_t icount = mesh.indices.size();

        // Per-mesh material state reaches the pixel loop through opt. This is a
        // per-thread copy, which is also how the band is carried down.
        RenderOptions mopt = opt;
        mopt.srcBlend = mesh.srcBlend;
        mopt.dstBlend = mesh.dstBlend;
        mopt.unlit    = mesh.unlit;
        mopt.bandTop    = bandTop;
        mopt.bandBot    = bandBot;
        mopt.alphaTest  = mesh.alphaTest;
        mopt.blendAlpha = mesh.blendAlpha;
        mopt.additive   = mesh.additive;
        mopt.opacity    = mesh.opacity;
        mopt.tint       = mesh.tint;
        mopt.emissive   = mesh.emissive;
        mopt.depthWrite = !blendPass;

        for (size_t i = 0; i + 2 < icount; i += 3) {
            const u32 ia = mesh.indices[i];
            const u32 ib = mesh.indices[i + 1];
            const u32 ic = mesh.indices[i + 2];
            if (ia >= vcount || ib >= vcount || ic >= vcount) continue;   // hostile data

            ClipVtx cv[3];
            const u32 idx3[3] = { ia, ib, ic };
            for (int k = 0; k < 3; ++k) {
                const RasterVertex& v = mesh.vertices[idx3[k]];
                cv[k].clip = mvp.transformVec4(Vec4(v.position.x, v.position.y, v.position.z, 1.0f));
                cv[k].nrm  = mv.transformVector(v.normal);
                cv[k].uv   = v.uv;
                cv[k].col  = v.color;
            }
            m_impl->triangle(cv[0], cv[1], cv[2], tex, mopt, mesh.twoSided);
        }
    }
    }   // pass
    };  // drawMeshBand

    // Split the frame across cores. The rasteriser is pure fill work, so this
    // scales almost linearly; measured 315 ms/frame single-threaded at
    // 1400x700 with 4x supersampling on a 32-core machine. Bands are kept a
    // few rows tall at minimum so the thread overhead cannot exceed the work.
    {
        const int hh = m_impl->h;
        unsigned cores = std::thread::hardware_concurrency();
        if (cores == 0) cores = 1;
        int bands = static_cast<int>(cores > 32u ? 32u : cores);
        if (bands > hh / 8) bands = hh / 8;
        if (bands < 1) bands = 1;

        if (bands == 1) {
            drawMeshBand(0, hh);
        } else {
            std::vector<std::thread> workers;
            workers.reserve(static_cast<size_t>(bands) - 1);
            const int step = (hh + bands - 1) / bands;
            for (int b = 1; b < bands; ++b) {
                const int y0 = b * step;
                const int y1 = (b + 1) * step < hh ? (b + 1) * step : hh;
                if (y0 >= y1) break;
                workers.emplace_back([&drawMeshBand, y0, y1]() { drawMeshBand(y0, y1); });
            }
            drawMeshBand(0, step < hh ? step : hh);   // this thread takes band 0
            for (std::thread& t : workers) t.join();
        }
    }

    // --- line lists (skeletons, helpers) -----------------------------------
    for (const RasterLines& l : scene.lines) {
        const Mat4 lmvp = l.transform * vp;
        for (size_t i = 0; i + 1 < l.points.size(); i += 2)
            drawLine3D(l.points[i], l.points[i + 1], lmvp, l.color);
    }

    return true;
}

const Image& Rasterizer::colorBuffer() const { return m_impl->color; }
Image&       Rasterizer::colorBuffer()       { return m_impl->color; }
const std::vector<f32>& Rasterizer::depthBuffer() const { return m_impl->depth; }

bool Rasterizer::present(HDC dc, const RECT& dst) const {
    return m_impl->color.blitTo(dc, dst, ChannelView::RGB);
}

void Rasterizer::drawTriangle(const RasterVertex& a, const RasterVertex& b, const RasterVertex& c,
                              const Mat4& mvp, const Image* texture, const RenderOptions& opt) {
    ClipVtx cv[3];
    const RasterVertex* src[3] = { &a, &b, &c };
    for (int k = 0; k < 3; ++k) {
        cv[k].clip = mvp.transformVec4(
            Vec4(src[k]->position.x, src[k]->position.y, src[k]->position.z, 1.0f));
        cv[k].nrm  = src[k]->normal;    // already view space, by contract
        cv[k].uv   = src[k]->uv;
        cv[k].col  = src[k]->color;
    }
    m_impl->triangle(cv[0], cv[1], cv[2], texture, opt, !opt.backFaceCull);
}

void Rasterizer::drawLine3D(const Vec3& a, const Vec3& b, const Mat4& mvp, Rgba color) {
    m_impl->lineClip(mvp.transformVec4(Vec4(a.x, a.y, a.z, 1.0f)),
                     mvp.transformVec4(Vec4(b.x, b.y, b.z, 1.0f)),
                     color, true);
}

void Rasterizer::drawGrid(const Mat4& mvp, f32 spacing, int halfCount, Rgba color) {
    if (!(spacing > 1e-6f) || halfCount <= 0) return;
    if (halfCount > 256) halfCount = 256;
    const f32 ext = spacing * static_cast<f32>(halfCount);
    for (int i = -halfCount; i <= halfCount; ++i) {
        const f32 t = spacing * static_cast<f32>(i);
        // Grid lies on the XZ plane (Y is up in Silkroad).
        drawLine3D(Vec3(t, 0.0f, -ext), Vec3(t, 0.0f, ext), mvp, color);
        drawLine3D(Vec3(-ext, 0.0f, t), Vec3(ext, 0.0f, t), mvp, color);
    }
}

void Rasterizer::drawAxes(const Mat4& mvp, f32 length) {
    if (!(length > 1e-6f)) return;
    drawLine3D(Vec3(0, 0, 0), Vec3(length, 0, 0), mvp, Rgba(0xCC, 0x33, 0x33, 0xFF));  // X red
    drawLine3D(Vec3(0, 0, 0), Vec3(0, length, 0), mvp, Rgba(0x33, 0xAA, 0x55, 0xFF));  // Y green
    drawLine3D(Vec3(0, 0, 0), Vec3(0, 0, length), mvp, Rgba(0x44, 0x88, 0xCC, 0xFF));  // Z blue
}

void Rasterizer::drawAabb(const Aabb& box, const Mat4& mvp, Rgba color) {
    if (!box.valid()) return;
    const Vec3 lo = box.min, hi = box.max;
    const Vec3 c[8] = {
        Vec3(lo.x, lo.y, lo.z), Vec3(hi.x, lo.y, lo.z),
        Vec3(hi.x, hi.y, lo.z), Vec3(lo.x, hi.y, lo.z),
        Vec3(lo.x, lo.y, hi.z), Vec3(hi.x, lo.y, hi.z),
        Vec3(hi.x, hi.y, hi.z), Vec3(lo.x, hi.y, hi.z)
    };
    static const int e[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    for (int i = 0; i < 12; ++i) drawLine3D(c[e[i][0]], c[e[i][1]], mvp, color);
}

void Rasterizer::drawContactShadow(const Vec3& c, f32 r, const Mat4& mvp, f32 alphaCentre) {
    if (!m_impl->ready() || !(r > 1e-4f)) return;
    // A 28-segment fan on the floor plane, opaque at the centre and fading to
    // nothing at the rim (reference main.cpp:4148-4152).
    const Vec4 cc = mvp.transformVec4(Vec4(c.x, c.y, c.z, 1.0f));
    Vec4 prev;
    bool have = false;
    for (int i = 0; i <= 28; ++i) {
        const f32 a = static_cast<f32>(i) * 6.2831853f / 28.0f;
        const Vec4 e = mvp.transformVec4(
            Vec4(c.x + std::cos(a) * r, c.y, c.z + std::sin(a) * r, 1.0f));
        if (have) m_impl->blendTri(cc, prev, e, alphaCentre, 0.0f, 0.0f, Rgba(0, 0, 0, 255));
        prev = e;
        have = true;
    }
}

Rgba Rasterizer::SampleTexture(const Image& tex, f32 u, f32 v, bool bilinear) {
    return SampleTex(tex, u, v, bilinear, Address::Wrap);
}

// ---------------------------------------------------------------------------
// Headless one-shot render (used by --shot)
// ---------------------------------------------------------------------------
Result RenderSceneToImage(const Scene& scene, const Camera& camera,
                          const RenderOptions& opt,
                          int width, int height, Image& out) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        out.clear();
        return Result::Fail(Status::InvalidArgument, T(L"Gecersiz goruntu boyutu."));
    }

    Rasterizer r;
    if (!r.resize(width, height)) {
        out.clear();
        return Result::Fail(Status::OutOfMemory, T(L"Render tamponu ayrilamadi."));
    }
    if (!r.render(scene, camera, opt)) {
        out.clear();
        return Result::Fail(Status::Unknown, T(L"Sahne cizilemedi."));
    }

    out = r.colorBuffer();
    if (!out.valid()) return Result::Fail(Status::OutOfMemory, T(L"Goruntu kopyalanamadi."));
    return Result::Ok();
}

} // namespace sro
