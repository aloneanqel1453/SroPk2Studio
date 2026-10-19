// ============================================================================
//  SRO Asset Studio - fx/FxTransform.h
//
//  How an emitter's frame of reference is built from its parent's.
//
//  An effect is a TREE, and almost nothing in a child node is expressed in
//  world space. Its position, its motion vectors and its authored path are all
//  stated relative to a frame that the parent supplies, and which commands
//  route through that frame is decided by a SOURCE TYPE byte the parser used
//  to discard as "unused".
//
//  Missing this showed up as thousands of emitters whose velocity and force
//  pointed the wrong way - the magnitudes were right, the directions were not -
//  and as child emitters collapsing onto the effect origin.
//
//  The source byte splits into two independent selectors:
//      base   = sourceType % 3        which position the child hangs off
//      matrix = floor(sourceType / 3) whose rotation the vector is stated in
//
//  Matrices here are ROW-VECTOR (v*M), matching the rest of this renderer and
//  the file's own D3D layout. Composition therefore reads left-to-right in
//  application order, which is the reverse of the reference's column-vector
//  form.
// ============================================================================
#pragma once

#include "../Common.h"
#include "../formats/Efp.h"
#include "../render/Raster.h"

#include <cmath>

namespace sro {
namespace fx {

/// The frame a node hands down to its children.
struct FxFrame {
    Vec3 position{ 0, 0, 0 };
    Mat4 rotation = Mat4::Identity();
    bool hasRotation = false;
    /// The nearest authored path at or above this node, already transformed.
    std::vector<Vec3> banPositions;
    f32  banSpeed = 0.0f;
    f32  cefStartMs = 0.0f;
};

inline int SourceBase(u8 sourceType)   { return static_cast<int>(sourceType % 3u); }
inline int SourceMatrix(u8 sourceType) { return static_cast<int>(sourceType / 3u); }

/// A rotation-only transform of a direction or offset.
inline Vec3 TransformByMatrix(const Mat4& m, bool has, const Vec3& v) {
    if (!has) return v;
    return Vec3(v.x * m.m[0] + v.y * m.m[4] + v.z * m.m[8],
                v.x * m.m[1] + v.y * m.m[5] + v.z * m.m[9],
                v.x * m.m[2] + v.y * m.m[6] + v.z * m.m[10]);
}

/// Load a node's authored 4x4 as a rotation, dropping any translation.
inline Mat4 RotationOf(const f32 raw[16]) {
    Mat4 m;
    for (int i = 0; i < 16; ++i) m.m[i] = raw[i];
    m.m[3] = m.m[7] = m.m[11] = 0.0f;
    m.m[12] = m.m[13] = m.m[14] = 0.0f;
    m.m[15] = 1.0f;
    return m;
}

/// The node's own rotation, combined with the parent's as the routing byte asks.
///
/// Route 2 and 3 both mean "relative to the parent"; anything else means the
/// authored matrix stands alone. In row-vector form "local then parent" is
/// local * parent.
inline void NodeRotation(const EfpNode& n, const FxFrame& parent,
                         Mat4& out, bool& hasOut) {
    if (!n.hasRotationMatrix) { out = parent.rotation; hasOut = parent.hasRotation; return; }
    const Mat4 local = RotationOf(n.rotationMatrix);
    if ((n.transformFlags == 2 || n.transformFlags == 3) && parent.hasRotation) {
        out = local * parent.rotation;
        hasOut = true;
        return;
    }
    out = local;
    hasOut = true;
}

/// Which rotation a command's vectors are stated in.
inline void PositionMatrix(int matrixSel, const FxFrame& parent,
                           const Mat4& objectRotation, bool objectHas,
                           Mat4& out, bool& hasOut) {
    if (matrixSel == 1) { out = objectRotation; hasOut = objectHas; return; }
    if (matrixSel == 2 || matrixSel == 3) { out = parent.rotation; hasOut = parent.hasRotation; return; }
    out = Mat4::Identity();
    hasOut = false;
}

/// Which position the node hangs off.
inline Vec3 BasePosition(int baseSel, const FxFrame& parent) {
    // 1 and 2 both resolve to the parent's own position; 0 means world origin.
    return (baseSel == 1 || baseSel == 2) ? parent.position : Vec3(0, 0, 0);
}

/// Build the frame this node passes to its children, and the world position it
/// draws at. `rootScale` sizes every authored offset in the file.
inline FxFrame BuildFrame(const EfpNode& n, const FxFrame& parent,
                          f32 rootScale, bool isRoot) {
    FxFrame f;
    NodeRotation(n, parent, f.rotation, f.hasRotation);

    const Vec3 scaled(n.position.x * rootScale, n.position.y * rootScale, n.position.z * rootScale);
    Mat4 pm; bool pmHas = false;
    PositionMatrix(SourceMatrix(n.positionSourceType), parent, f.rotation, f.hasRotation, pm, pmHas);
    const Vec3 offset = TransformByMatrix(pm, pmHas, scaled);
    f.position = BasePosition(SourceBase(n.positionSourceType), parent) + offset;

    // The path is INHERITED when the node does not author one - that is why
    // thousands of emitters reported no path at all while the reference gave
    // them their parent's.
    if (!n.banPositions.empty()) {
        Mat4 bm; bool bmHas = false;
        PositionMatrix(SourceMatrix(n.banPositionSourceType), parent, f.rotation, f.hasRotation, bm, bmHas);
        f.banPositions.reserve(n.banPositions.size());
        for (const Vec3& p : n.banPositions) {
            const Vec3 s(p.x * rootScale, p.y * rootScale, p.z * rootScale);
            f.banPositions.push_back(TransformByMatrix(bm, bmHas, s));
        }
        // A "delta" path states each point relative to the one before it.
        if (((n.banPositionSourceType + 0x2Bu) % 3u) == 0u && f.banPositions.size() > 1) {
            std::vector<Vec3> d;
            d.reserve(f.banPositions.size());
            d.push_back(Vec3(0, 0, 0));
            for (size_t i = 1; i < f.banPositions.size(); ++i)
                d.push_back(f.banPositions[i] - f.banPositions[i - 1]);
            f.banPositions.swap(d);
        }
        f.banSpeed = n.banSpeed;
    } else {
        f.banPositions = parent.banPositions;
        f.banSpeed     = parent.banSpeed;
    }

    f.cefStartMs = parent.cefStartMs
        + (isRoot ? 0.0f : std::max(0.0f, static_cast<f32>(n.emit.present ? n.emit.minVal : 0)) * 50.0f);
    return f;
}

/// Motion vectors route on the source byte DIRECTLY, not on sourceType/3 the
/// way positions do - and a source of 3 does not mean "some other frame", it
/// means the vector is dropped entirely.
inline Vec3 TransformMotion(u8 sourceType, const FxFrame& self, const FxFrame& parent,
                            const Vec3& v) {
    if (sourceType == 1) return TransformByMatrix(self.rotation, self.hasRotation, v);
    if (sourceType == 2) return TransformByMatrix(parent.rotation, parent.hasRotation, v);
    if (sourceType == 3) return Vec3(0, 0, 0);
    return v;
}

/// The authored scalar size: the largest ABSOLUTE component of the resolved
/// scale curve, times the file's root scale. Taking the curve's signed maximum
/// (or the blend graph's) gives a different number.
inline f32 AuthoredScale(const EfpNode& n, f32 rootScale) {
    f32 best = 0.0f;
    bool any = false;
    for (const EfpScaleKey& k : n.scaleKeys) {
        const f32 v[3] = { std::fabs(k.v.x), std::fabs(k.v.y), std::fabs(k.v.z) };
        for (f32 x : v) if (std::isfinite(x)) { best = std::max(best, x); any = true; }
    }
    if (!any) best = 1.0f;
    // The two sources of this number differ in ONE detail, and it is the whole
    // discrepancy: the ScaleGraph CONTROLLER seeds its running maximum at 1, so
    // a curve that never exceeds 1 still reports 1, while the SetGraphScale
    // COMMAND takes the frames' plain maximum. Curves peaking at 0.5 were
    // therefore reported as half-size emitters.
    //
    // Replacing the controller's value with a flat 1 is NOT the fix - measured,
    // that moved this field from 153 disagreements to 1929, because a
    // controller curve above 1 does set the scalar.
    if (!n.scaleFromCommand) best = std::max(1.0f, best);

    const f32 s = best * rootScale;
    return (std::isfinite(s) && s > 0.0f) ? s : 1.0f;
}

} // namespace fx
} // namespace sro
