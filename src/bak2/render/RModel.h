// SkillEffect Studio - render/RModel.h
// Composed character / monster model: .bsr (or .cpd = several .bsr at one origin) -> meshes (.bms), material
// set (.bmt -> .ddj), skeleton (.bsk), animation clips (.ban) resolved through the .bsr aniGroups by
// ResourceAnimationType id with their event keys (Type 1 = skilleffect StartEvent N). CPU skinning (2 bones per
// vertex, bind = rotParent/transParent chain, Bip0N -> Bip01 remap), never re-oriented (skinned vertices are
// already in model space; sro-asset-studio-skinned-animation).
#pragma once

#include "RAssets.h"
#include "RFrame.h"

#include <memory>
#include <string>
#include <vector>

namespace se::render {

enum class PartBlend : std::uint8_t { Opaque, Cutout, Blend, Additive };

struct ModelPart {
    std::wstring meshPath;
    std::shared_ptr<const BmsFile> mesh;
    std::wstring materialName, texturePath;
    std::shared_ptr<RTexture> tex;
    BmtMaterial mat;
    bool hasMat = false;
    PartBlend blend = PartBlend::Opaque;
    float tint[3] = {1, 1, 1};
    std::vector<int> boneMap;      // mesh bone index -> skeleton bone (-1 unmapped)
    bool hidden = false;
    int bsrIndex = 0;              // which composed .bsr
    int attachBone = -1;           // unskinned mesh following the .bsr attach bone (weapon / shield)
};

struct ModelClip {
    int typeId = -1;
    std::wstring group, banPath;
    std::shared_ptr<const BanFile> ban;
    std::vector<BsrAniEvent> events;
    double lengthMs = 1000;
    std::vector<double> EventTimes(std::uint32_t type = 1) const;   // ms, Type 1 = skilleffect StartEvent keys
};

class ModelDef {
public:
    // path = .bsr or .cpd inside Data.pk2. materialSet = BSR material set index (characterdata TextureType id).
    static std::shared_ptr<ModelDef> Load(AssetCache& cache, const std::wstring& path, int materialSet,
                                          std::wstring& err);
    std::wstring path, name;
    std::vector<std::shared_ptr<const BsrFile>> bsrs;   // [0] = primary (skeleton / aniGroups)
    std::vector<std::wstring> bsrPaths;
    std::shared_ptr<const BskFile> skel;
    std::vector<ModelPart> parts;
    std::vector<M4> bindWorld, invBind;                 // per skeleton bone (row-vector)
    V3 bbMin, bbMax;                                    // bind pose bounds (model space)
    std::vector<std::wstring> warnings;
    // clip lookup: group "" = the first group that has the type (DEFAULT preferred); strict = only that group
    const ModelClip* Clip(AssetCache& cache, int typeId, const std::wstring& group = L"", bool strict = false);
    const ModelClip* ClipByIndex(AssetCache& cache, int animIndex);   // BSR animation list index
    std::vector<std::pair<std::wstring, int>> ClipList() const;       // (group, type id) of every aniGroup entry
    int Clips() const;
    double Radius() const;
    bool HasSkeleton() const { return skel && !skel->bones.empty(); }
    // add an equipment / avatar .bsr (armour, weapon, hat): skinned to this skeleton by bone name, or rigid on its
    // attach bone. Call before the instance poses (ModelInstance::Attach does it and re-poses).
    bool Append(AssetCache& cache, const std::wstring& bsrPath, int materialSet, std::wstring& err);

private:
    void ComposeBsr(AssetCache& cache, std::size_t bi, int materialSet, bool& first);
    std::vector<std::unique_ptr<ModelClip>> m_clips;
};

struct ModelLook {
    double ambient = 0.80, key = 0.34, rim = 0.16;
    V3 keyDir{-0.4, 0.85, 0.5}, rimDir{0.55, 0.35, -0.8};   // world, towards the light
    bool twoSided = true;
};

class ModelInstance {
public:
    explicit ModelInstance(std::shared_ptr<ModelDef> def);
    std::shared_ptr<ModelDef> Def() const { return m_def; }
    // placement (world = scale * RotY(yaw) * T(pos))
    V3 pos;
    double yaw = 0;       // radians, D3D LH: positive = clockwise seen from above
    double scale = 1;
    bool visible = true;
    double tint[3] = {1, 1, 1};          // colour multiplier (SCT_MAT tint by the host)
    // full placement override (props flown by the skill player: rows = rotation, row 3 = position); scale kept
    void SetWorldMatrix(const M4& m) { m_useMat = true; m_mat = m; m_posed = false; }
    void ClearWorldMatrix() { m_useMat = false; }
    M4 World() const;
    // animation
    bool Play(AssetCache& cache, int typeId, bool loop, const std::wstring& group = L"", double speed = 1.0);
    bool PlayIndex(AssetCache& cache, int animIndex, bool loop, double speed = 1.0);
    bool Attach(AssetCache& cache, const std::wstring& bsrPath, std::wstring& err, int materialSet = 0);   // equipment
    void Stop();          // bind pose
    void SetTime(double ms);   // jump inside the current clip
    void Advance(double ms);
    const ModelClip* Current() const { return m_clip; }
    double TimeMs() const { return m_t; }
    bool Finished() const;   // one-shot clip reached its end
    bool Looping() const { return m_loop; }
    // pose (call after Advance; Draw / BoneWorld call it lazily)
    void Pose();
    bool BoneWorld(const std::wstring& bone, M4& out);       // bone matrix in WORLD (row-vector), false if missing
    bool BoneWorld(int bone, M4& out);
    int FindBone(const std::wstring& bone) const;
    // draw (skinned) into a frame; pass 0 = opaque + cut-out, 1 = blended / additive, -1 = both
    void Draw(RFrame& f, const ModelLook& look, int pass = -1);
    // world bounds of the current pose
    void Bounds(V3& mn, V3& mx);

private:
    M4 Local(int bone) const;
    void MapClip();
    std::shared_ptr<ModelDef> m_def;
    std::vector<int> m_banMap;            // skeleton bone -> clip bone (-1 = bind)
    const ModelClip* m_clip = nullptr;
    double m_t = 0, m_speed = 1;
    bool m_loop = true, m_posed = false;
    bool m_useMat = false;
    M4 m_mat;
    std::vector<M4> m_boneWorld, m_skin;   // model space
    std::vector<std::vector<V3>> m_pos, m_nrm;   // skinned per part
};

}  // namespace se::render
