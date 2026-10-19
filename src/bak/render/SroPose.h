// ============================================================================
//  SRO Asset Studio - render/SroPose.h
//
//  Skeletal posing + mesh skinning, ported 1:1 from the reference viewer
//  (SroEffectViewer: SroMath.h + Model.h setupSkeleton / computeBind /
//  localOf / computeWorld / applyPose / skinMeshVec).
//
//  IMPORTANT - why this module carries its OWN matrix type:
//  the reference is COLUMN-vector / column-major (OpenGL layout, v' = M*v,
//  matMul(a,b) applies b first), while the rest of this program is ROW-vector
//  (v' = v*M). Translating the pipeline between the two conventions is exactly
//  where a hand-written port goes wrong - silently, as a sheared or collapsed
//  model rather than an obvious error. So the maths here stays in the
//  reference's convention verbatim, and the ONLY conversion happens at the end,
//  when the finished world-space positions are handed back as plain Vec3.
// ============================================================================
#pragma once

#include "../Common.h"
#include "../formats/Ban.h"
#include "../formats/Bms.h"
#include "../formats/Bsk.h"

namespace sro {
namespace sropose {

/// Column-major 4x4, OpenGL layout: m[col*4 + row]. (reference SroMath.h)
struct M4 {
    f32 m[16];
    M4() { identity(); }
    void identity() {
        std::memset(m, 0, sizeof(m));
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }
};

M4   FromQuatTrans(const Quat& q, const Vec3& t);
M4   Mul(const M4& a, const M4& b);          ///< a*b, b applied first
Vec3 MulPoint(const M4& m, const Vec3& v);
Vec3 MulVec(const M4& m, const Vec3& v);
M4   InverseRigid(const M4& a);
Quat Slerp(Quat a, Quat b, f32 t);

/// One skinned mesh: the BIND source it never mutates, its slot->bone map, and
/// the posed output the renderer consumes (reference GLMesh::skinnedPos/Nrm).
struct PoseMesh {
    const BmsFile*    src = nullptr;
    std::vector<int>  boneMap;
    std::vector<Vec3> skinnedPos;
    std::vector<Vec3> skinnedNrm;
};

/// The reference's Model animation state, isolated.
class Poser {
public:
    /// bindWorld from the toParent chain, invBind = its rigid inverse,
    /// boneWorld = bindWorld, boneSkin = identity. (Model.h setupSkeleton)
    void setup(const BskFile& skeleton);

    /// mesh bone slot -> skeleton bone, EXACT name match, with the reference's
    /// "Bip0N (N>1) -> Bip01" remap for attachments carrying a second biped.
    void mapMesh(const BmsFile& mesh, PoseMesh& out) const;

    /// Pose every mesh for `timeMs`. `anim` may be null (bind pose).
    void applyPose(const BanFile* anim, f32 timeMs, std::vector<PoseMesh>& meshes);

    bool   ready()     const { return skel_ != nullptr && !bindWorld_.empty(); }
    size_t boneCount() const { return bindWorld_.size(); }

private:
    const BskFile*   skel_ = nullptr;
    std::vector<M4>  bindWorld_, invBind_, boneWorld_, boneSkin_;

    void computeBind(int i, std::vector<char>& done);
    void computeWorld(int i, std::vector<char>& done, const BanFile* anim, f32 t);
    M4   localOf(int boneIdx, const BanFile* anim, f32 t) const;
};

} // namespace sropose
} // namespace sro
