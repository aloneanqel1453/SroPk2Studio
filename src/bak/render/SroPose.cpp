// ============================================================================
//  SRO Asset Studio - render/SroPose.cpp   (see SroPose.h)
//
//  Every function here mirrors the reference viewer line for line. Where the
//  reference used its own Quat/Vec3 helpers the host equivalents are used, but
//  the ARITHMETIC and the ORDER of every multiply are unchanged.
// ============================================================================
#include "SroPose.h"

#include <cmath>

namespace sro {
namespace sropose {

namespace {

Quat QNormalize(const Quat& q) {
    const f32 n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n <= 1e-20f) return Quat(0, 0, 0, 1);
    return Quat(q.x / n, q.y / n, q.z / n, q.w / n);
}

} // namespace

// --- reference SroMath.h ----------------------------------------------------
M4 FromQuatTrans(const Quat& q0, const Vec3& t) {
    const Quat q = QNormalize(q0);
    const f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    M4 r;
    r.m[0]  = 1 - 2 * (yy + zz); r.m[1]  = 2 * (xy + wz); r.m[2]  = 2 * (xz - wy); r.m[3]  = 0;
    r.m[4]  = 2 * (xy - wz); r.m[5]  = 1 - 2 * (xx + zz); r.m[6]  = 2 * (yz + wx); r.m[7]  = 0;
    r.m[8]  = 2 * (xz + wy); r.m[9]  = 2 * (yz - wx); r.m[10] = 1 - 2 * (xx + yy); r.m[11] = 0;
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z; r.m[15] = 1;
    return r;
}

M4 Mul(const M4& a, const M4& b) {
    M4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            f32 s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

Vec3 MulPoint(const M4& m, const Vec3& v) {
    return Vec3(m.m[0] * v.x + m.m[4] * v.y + m.m[8]  * v.z + m.m[12],
                m.m[1] * v.x + m.m[5] * v.y + m.m[9]  * v.z + m.m[13],
                m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14]);
}

Vec3 MulVec(const M4& m, const Vec3& v) {
    return Vec3(m.m[0] * v.x + m.m[4] * v.y + m.m[8]  * v.z,
                m.m[1] * v.x + m.m[5] * v.y + m.m[9]  * v.z,
                m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z);
}

M4 InverseRigid(const M4& a) {
    M4 r;
    r.m[0] = a.m[0]; r.m[1] = a.m[4]; r.m[2]  = a.m[8];
    r.m[4] = a.m[1]; r.m[5] = a.m[5]; r.m[6]  = a.m[9];
    r.m[8] = a.m[2]; r.m[9] = a.m[6]; r.m[10] = a.m[10];
    r.m[3] = r.m[7] = r.m[11] = 0; r.m[15] = 1;
    const Vec3 t(a.m[12], a.m[13], a.m[14]);
    r.m[12] = -(r.m[0] * t.x + r.m[4] * t.y + r.m[8]  * t.z);
    r.m[13] = -(r.m[1] * t.x + r.m[5] * t.y + r.m[9]  * t.z);
    r.m[14] = -(r.m[2] * t.x + r.m[6] * t.y + r.m[10] * t.z);
    return r;
}

Quat Slerp(Quat a, Quat b, f32 t) {
    f32 d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; d = -d; }
    if (d > 0.9995f) {
        Quat r(a.x + t * (b.x - a.x), a.y + t * (b.y - a.y),
               a.z + t * (b.z - a.z), a.w + t * (b.w - a.w));
        return QNormalize(r);
    }
    const f32 th0 = std::acos(d), th = th0 * t;
    const f32 s0 = std::cos(th) - d * std::sin(th) / std::sin(th0);
    const f32 s1 = std::sin(th) / std::sin(th0);
    return Quat(a.x * s0 + b.x * s1, a.y * s0 + b.y * s1,
                a.z * s0 + b.z * s1, a.w * s0 + b.w * s1);
}

// --- reference Model.h ------------------------------------------------------
void Poser::computeBind(int i, std::vector<char>& done) {
    if (done[i]) return;
    const BskBone& b = skel_->bones[static_cast<size_t>(i)];
    const M4 local = FromQuatTrans(b.rotationToParent, b.translationToParent);
    const int par = b.parentIndex;
    if (par >= 0 && static_cast<size_t>(par) < skel_->bones.size() && par != i) {
        computeBind(par, done);
        bindWorld_[static_cast<size_t>(i)] = Mul(bindWorld_[static_cast<size_t>(par)], local);
    } else {
        bindWorld_[static_cast<size_t>(i)] = local;
    }
    done[static_cast<size_t>(i)] = 1;
}

void Poser::setup(const BskFile& skeleton) {
    skel_ = &skeleton;
    const size_t nb = skeleton.bones.size();
    bindWorld_.assign(nb, M4()); invBind_.assign(nb, M4());
    boneWorld_.assign(nb, M4()); boneSkin_.assign(nb, M4());
    if (!nb) return;
    std::vector<char> done(nb, 0);
    for (size_t i = 0; i < nb; ++i) computeBind(static_cast<int>(i), done);
    for (size_t i = 0; i < nb; ++i) {
        invBind_[i]   = InverseRigid(bindWorld_[i]);
        boneWorld_[i] = bindWorld_[i];
        boneSkin_[i].identity();
    }
}

void Poser::mapMesh(const BmsFile& mesh, PoseMesh& out) const {
    out.src = &mesh;
    out.boneMap.assign(mesh.boneNames.size(), -1);
    if (!skel_) return;
    for (size_t i = 0; i < mesh.boneNames.size(); ++i) {
        for (size_t j = 0; j < skel_->bones.size(); ++j)
            if (skel_->bones[j].name == mesh.boneNames[i]) { out.boneMap[i] = static_cast<int>(j); break; }
        // Attachments that carry a WHOLE second biped name their bones "Bip02 ..."
        // and would otherwise never match, leaving the piece in a raw T-pose.
        if (out.boneMap[i] < 0) {
            const std::wstring& bn = mesh.boneNames[i];
            if (bn.size() > 4 && bn.compare(0, 4, L"Bip0") == 0 && bn[4] != L'1') {
                std::wstring rn = bn; rn[4] = L'1';
                for (size_t j = 0; j < skel_->bones.size(); ++j)
                    if (skel_->bones[j].name == rn) { out.boneMap[i] = static_cast<int>(j); break; }
            }
        }
    }
    out.skinnedPos.assign(mesh.vertices.size(), Vec3());
    out.skinnedNrm.assign(mesh.vertices.size(), Vec3());
}

M4 Poser::localOf(int boneIdx, const BanFile* anim, f32 t) const {
    const BskBone& b = skel_->bones[static_cast<size_t>(boneIdx)];
    const M4 bindLocal = FromQuatTrans(b.rotationToParent, b.translationToParent);
    if (!anim) return bindLocal;

    const BanAnimatedBone* ab = anim->findBone(b.name);
    if (!ab || ab->keyframes.empty() || anim->keyframeTimesMs.empty()) return bindLocal;

    const std::vector<BanKeyframe>& keys = ab->keyframes;
    const std::vector<u32>&         kt   = anim->keyframeTimesMs;

    size_t k = 0;
    while (k + 1 < kt.size() && static_cast<f32>(kt[k + 1]) <= t) ++k;
    const size_t k1 = (k + 1 < keys.size()) ? k + 1 : k;
    const f32 t0 = static_cast<f32>(kt[k]);
    const f32 t1 = (k1 < kt.size()) ? static_cast<f32>(kt[k1]) : t0;
    f32 f = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    if (k >= keys.size()) return bindLocal;

    const Quat rot = Slerp(keys[k].rotation, keys[k1].rotation, f);
    const Vec3 tr  = keys[k].translation * (1 - f) + keys[k1].translation * f;
    return FromQuatTrans(rot, tr);
}

void Poser::computeWorld(int i, std::vector<char>& done, const BanFile* anim, f32 t) {
    if (done[static_cast<size_t>(i)]) return;
    const M4 local = localOf(i, anim, t);
    const int par = skel_->bones[static_cast<size_t>(i)].parentIndex;
    if (par >= 0 && static_cast<size_t>(par) < skel_->bones.size() && par != i) {
        computeWorld(par, done, anim, t);
        boneWorld_[static_cast<size_t>(i)] = Mul(boneWorld_[static_cast<size_t>(par)], local);
    } else {
        boneWorld_[static_cast<size_t>(i)] = local;
    }
    done[static_cast<size_t>(i)] = 1;
}

void Poser::applyPose(const BanFile* anim, f32 timeMs, std::vector<PoseMesh>& meshes) {
    if (!skel_) return;
    const size_t nb = skel_->bones.size();
    if (nb > 0) {
        std::vector<char> done(nb, 0);
        for (size_t i = 0; i < nb; ++i) computeWorld(static_cast<int>(i), done, anim, timeMs);
        for (size_t i = 0; i < nb; ++i) boneSkin_[i] = Mul(boneWorld_[i], invBind_[i]);
    }

    // reference skinMeshVec()
    for (PoseMesh& gm : meshes) {
        if (!gm.src) continue;
        const BmsFile& bms = *gm.src;
        const size_t vc = bms.vertices.size();
        if (gm.skinnedPos.size() != vc) { gm.skinnedPos.assign(vc, Vec3()); gm.skinnedNrm.assign(vc, Vec3()); }

        if (bms.isSkinned() && nb > 0 && !gm.boneMap.empty() && bms.skinWeights.size() == vc) {
            for (size_t i = 0; i < vc; ++i) {
                const BmsSkinWeight& w = bms.skinWeights[i];
                const int s1 = (w.boneIndex0 < gm.boneMap.size()) ? gm.boneMap[w.boneIndex0] : -1;
                const int s2 = (w.boneIndex1 < gm.boneMap.size()) ? gm.boneMap[w.boneIndex1] : -1;
                const Vec3 pos = bms.vertices[i].position, nrm = bms.vertices[i].normal;
                // The reference stores weights already normalised as floats; the
                // host keeps the raw u16 pair, so normalise here - by their SUM.
                const f32 tot = static_cast<f32>(w.weight0) + static_cast<f32>(w.weight1);
                const f32 w1 = (tot > 0.0f) ? static_cast<f32>(w.weight0) / tot : 1.0f;
                const f32 w2 = (tot > 0.0f) ? static_cast<f32>(w.weight1) / tot : 0.0f;
                Vec3 rp, rn;
                if (s1 >= 0 && s2 >= 0 && w2 > 0) {
                    rp = MulPoint(boneSkin_[static_cast<size_t>(s1)], pos) * w1 +
                         MulPoint(boneSkin_[static_cast<size_t>(s2)], pos) * w2;
                    rn = MulVec(boneSkin_[static_cast<size_t>(s1)], nrm) * w1 +
                         MulVec(boneSkin_[static_cast<size_t>(s2)], nrm) * w2;
                } else if (s1 >= 0) {   // only one valid bone -> FULL weight
                    rp = MulPoint(boneSkin_[static_cast<size_t>(s1)], pos);
                    rn = MulVec(boneSkin_[static_cast<size_t>(s1)], nrm);
                } else if (s2 >= 0) {
                    rp = MulPoint(boneSkin_[static_cast<size_t>(s2)], pos);
                    rn = MulVec(boneSkin_[static_cast<size_t>(s2)], nrm);
                } else {
                    rp = pos; rn = nrm;
                }
                gm.skinnedPos[i] = rp;
                gm.skinnedNrm[i] = rn.normalized();
            }
        } else {
            for (size_t i = 0; i < vc; ++i) {
                gm.skinnedPos[i] = bms.vertices[i].position;
                gm.skinnedNrm[i] = bms.vertices[i].normal;
            }
        }
    }
}

} // namespace sropose
} // namespace sro
