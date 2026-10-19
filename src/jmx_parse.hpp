#pragma once
// Minimal parsers for Silkroad resource formats (SilkroadDoc wiki):
// JMXVRES (.bsr), JMXVBMS (.bms), JMXVBMT (.bmt), JMXVCPD (.cpd).
// Enough to assemble a model (meshes + materials + textures) for viewing.
#include <cstdint>
#include <string>
#include <vector>

namespace pk2 {

struct JmxBsr {
    std::vector<std::wstring> mesh_paths;      // .bms
    std::vector<std::wstring> material_paths;  // .bmt (material sets)
    std::wstring skeleton_path;                // .bsk (may be empty)
    std::vector<std::wstring> animation_paths; // .ban
    std::wstring name;
    bool ok = false;
};

struct JmxMaterial {
    std::wstring name;
    std::wstring diffuse;   // texture path (.ddj)
    bool absolute = false;  // diffuse path is absolute (from pk2 root) vs relative to .bmt dir
};
struct JmxBmt {
    std::vector<JmxMaterial> materials;
    bool ok = false;
};

struct JmxMesh {
    std::vector<float> positions; // x,y,z * vertexCount
    std::vector<float> normals;   // x,y,z * vertexCount
    std::vector<float> uvs;       // u,v   * vertexCount
    std::vector<std::uint16_t> indices;
    std::wstring material;        // material name (looked up in the .bmt set)
    std::wstring name;
    // Skinning (JMXVBMS SkinOffset): local bone list + per-vertex 2-bone weights.
    std::vector<std::wstring> skin_bones;      // mesh-local bone names (indexed by bone_idx)
    std::vector<std::uint8_t> bone_idx;        // 2 per vertex: idx1, idx2
    std::vector<float> bone_w;                 // 2 per vertex: w1, w2 (normalized 0..1)
    bool skinned = false;
    bool ok = false;
};

// JMXVBSK skeleton.
struct JmxBone {
    std::wstring name, parent;
    float rot_parent[4]{0, 0, 0, 1}, trans_parent[3]{};  // local bind (quat xyzw, vec) rel. parent
    float rot_origin[4]{0, 0, 0, 1}, trans_origin[3]{};  // bind pose rel. world origin (bone->world)
};
struct JmxSkeleton {
    std::vector<JmxBone> bones;
    bool ok = false;
};

// JMXVBAN animation (per-bone keyframe quats + translations relative to parent).
struct JmxAnimBone {
    std::wstring name;
    std::vector<float> rot;    // 4 per keyframe (x,y,z,w)
    std::vector<float> trans;  // 3 per keyframe
};
struct JmxAnim {
    std::wstring name;
    std::uint32_t duration = 0, fps = 0, type = 0; // ms, fps, 0=OneShot 1=Cyclic
    std::vector<std::uint32_t> times;              // keyframe times (ms)
    std::vector<JmxAnimBone> bones;
    bool ok = false;
};
struct JmxCpd {
    std::vector<std::wstring> resource_paths; // .bsr / .cpd
    bool ok = false;
};

[[nodiscard]] JmxBsr parse_bsr(const std::uint8_t* data, std::size_t len);
[[nodiscard]] JmxBmt parse_bmt(const std::uint8_t* data, std::size_t len);
[[nodiscard]] JmxMesh parse_bms(const std::uint8_t* data, std::size_t len);
[[nodiscard]] JmxCpd parse_cpd(const std::uint8_t* data, std::size_t len);
[[nodiscard]] JmxSkeleton parse_bsk(const std::uint8_t* data, std::size_t len);
[[nodiscard]] JmxAnim parse_ban(const std::uint8_t* data, std::size_t len);

} // namespace pk2
