#include "jmx_parse.hpp"

#include "text.hpp"

#include <cstring>
#include <stdexcept>

namespace pk2 {
namespace {

struct Reader {
    const std::uint8_t* d;
    std::size_t n;
    std::size_t o = 0;

    explicit Reader(const std::uint8_t* data, std::size_t len) : d(data), n(len) {}
    void need(std::size_t k) const { if (o + k > n) throw std::runtime_error("jmx: out of bounds"); }
    void seek(std::uint32_t pos) { if (pos > n) throw std::runtime_error("jmx: bad offset"); o = pos; }
    std::uint32_t u32() { need(4); std::uint32_t v; std::memcpy(&v, d + o, 4); o += 4; return v; }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    float f32() { need(4); float v; std::memcpy(&v, d + o, 4); o += 4; return v; }
    std::uint8_t u8() { need(1); return d[o++]; }
    void skip(std::size_t k) { need(k); o += k; }
    std::wstring str() {
        std::uint32_t len = u32();
        if (len > n) throw std::runtime_error("jmx: bad string len");
        need(len);
        std::string s(reinterpret_cast<const char*>(d + o), len);
        o += len;
        return acp_to_wide(s);
    }
    bool sig_is(const char* s) const { std::size_t l = std::strlen(s); return n >= l && std::memcmp(d, s, l) == 0; }
};

} // namespace

JmxBsr parse_bsr(const std::uint8_t* data, std::size_t len) {
    JmxBsr out;
    try {
        Reader r(data, len);
        if (!r.sig_is("JMXVRES")) return out;
        r.skip(12);
        std::uint32_t matOff = r.u32();
        std::uint32_t meshOff = r.u32();
        std::uint32_t skelOff = r.u32();
        std::uint32_t aniOff = r.u32();
        r.u32(); r.u32(); r.u32(); r.u32(); // group/modpalette/collision offsets
        std::uint32_t primMeshFlag = r.u32();
        r.u32(); r.u32(); r.u32(); r.u32(); // ModDataFlag + Int2..4
        // ObjectGeneralInfo (for the name)
        r.need(4); r.skip(4); // Type + Category (2+2)
        out.name = r.str();

        if (matOff) { r.seek(matOff); std::uint32_t c = r.u32(); for (std::uint32_t i = 0; i < c; ++i) { r.u32(); out.material_paths.push_back(r.str()); } }
        if (meshOff) { r.seek(meshOff); std::uint32_t c = r.u32(); for (std::uint32_t i = 0; i < c; ++i) { out.mesh_paths.push_back(r.str()); if (primMeshFlag & 1) r.u32(); } }
        if (skelOff) { r.seek(skelOff); std::uint32_t has = r.u32(); if (has == 1) { out.skeleton_path = r.str(); r.str(); } }
        if (aniOff) { r.seek(aniOff); r.u32(); r.u32(); std::uint32_t c = r.u32(); for (std::uint32_t i = 0; i < c; ++i) out.animation_paths.push_back(r.str()); }
        out.ok = true;
    } catch (...) { out.ok = !out.mesh_paths.empty() || !out.material_paths.empty(); }
    return out;
}

JmxBmt parse_bmt(const std::uint8_t* data, std::size_t len) {
    JmxBmt out;
    try {
        Reader r(data, len);
        if (!r.sig_is("JMXVBMT")) return out;
        r.skip(12);
        std::uint32_t cnt = r.u32();
        for (std::uint32_t i = 0; i < cnt; ++i) {
            JmxMaterial m;
            m.name = r.str();
            r.skip(64);        // Diffuse/Ambient/Specular/Emissive Color4
            r.f32();           // unkFloat0
            std::uint32_t flag = r.u32();
            m.diffuse = r.str();
            r.f32();           // DiffuseMap.unkFloat1
            r.skip(2);         // unkByte0, unkByte1
            m.absolute = r.u8() != 0;
            if (flag & 0x2000) { r.str(); r.u32(); } // NormalMap
            out.materials.push_back(std::move(m));
        }
        out.ok = true;
    } catch (...) { out.ok = !out.materials.empty(); }
    return out;
}

JmxMesh parse_bms(const std::uint8_t* data, std::size_t len) {
    JmxMesh out;
    try {
        Reader r(data, len);
        if (!r.sig_is("JMXVBMS")) return out;
        r.skip(12);
        std::uint32_t vertexOff = r.u32();
        std::uint32_t skinOff = r.u32();
        std::uint32_t faceOff = r.u32();
        for (int i = 0; i < 9; ++i) r.u32(); // remaining header offsets/flags up to NavFlag
        r.u32();                       // subPrimCount
        std::uint32_t vertexFlag = r.u32();
        r.u32();                       // unkUInt2
        out.name = r.str();
        out.material = r.str();
        r.u32();                       // unkUInt3

        if (vertexOff) {
            r.seek(vertexOff);
            std::uint32_t vc = r.u32();
            out.positions.reserve(vc * 3); out.normals.reserve(vc * 3); out.uvs.reserve(vc * 2);
            for (std::uint32_t i = 0; i < vc; ++i) {
                float px = r.f32(), py = r.f32(), pz = r.f32();
                float nx = r.f32(), ny = r.f32(), nz = r.f32();
                float u = r.f32(), v = r.f32();
                if (vertexFlag & 0x400) { r.f32(); r.f32(); }  // UV1
                if (vertexFlag & 0x800) r.skip(36);             // morph
                r.f32(); r.u32(); r.u32();                      // Float0, Int0, Int1
                out.positions.push_back(px); out.positions.push_back(py); out.positions.push_back(pz);
                out.normals.push_back(nx); out.normals.push_back(ny); out.normals.push_back(nz);
                out.uvs.push_back(u); out.uvs.push_back(v);
            }
        }
        std::uint32_t vc = static_cast<std::uint32_t>(out.positions.size() / 3);
        if (skinOff && vc) {
            try {
                r.seek(skinOff);
                std::uint32_t boneCount = r.u32();
                if (boneCount > 0 && boneCount < 4096) {
                    for (std::uint32_t i = 0; i < boneCount; ++i) out.skin_bones.push_back(r.str());
                    out.bone_idx.reserve(vc * 2); out.bone_w.reserve(vc * 2);
                    for (std::uint32_t i = 0; i < vc; ++i) {
                        std::uint8_t i1 = r.u8(); r.need(2); std::uint16_t w1; std::memcpy(&w1, r.d + r.o, 2); r.o += 2;
                        std::uint8_t i2 = r.u8(); r.need(2); std::uint16_t w2; std::memcpy(&w2, r.d + r.o, 2); r.o += 2;
                        float sum = static_cast<float>(w1) + static_cast<float>(w2);
                        float f1 = sum > 0 ? static_cast<float>(w1) / sum : 1.0f;
                        out.bone_idx.push_back(i1); out.bone_idx.push_back(i2);
                        out.bone_w.push_back(f1); out.bone_w.push_back(1.0f - f1);
                    }
                    out.skinned = !out.skin_bones.empty() && out.bone_idx.size() == static_cast<std::size_t>(vc) * 2;
                }
            } catch (...) { out.skinned = false; out.skin_bones.clear(); out.bone_idx.clear(); out.bone_w.clear(); }
        }
        if (faceOff) {
            r.seek(faceOff);
            std::uint32_t fc = r.u32();
            out.indices.reserve(fc * 3);
            for (std::uint32_t i = 0; i < fc; ++i) {
                r.need(6);
                std::uint16_t a, b, c;
                std::memcpy(&a, r.d + r.o, 2); std::memcpy(&b, r.d + r.o + 2, 2); std::memcpy(&c, r.d + r.o + 4, 2);
                r.o += 6;
                out.indices.push_back(a); out.indices.push_back(b); out.indices.push_back(c);
            }
        }
        out.ok = !out.positions.empty() && !out.indices.empty();
    } catch (...) { out.ok = !out.positions.empty() && !out.indices.empty(); }
    return out;
}

JmxSkeleton parse_bsk(const std::uint8_t* data, std::size_t len) {
    JmxSkeleton out;
    try {
        Reader r(data, len);
        if (!r.sig_is("JMXVBSK")) return out;
        r.skip(12);
        std::uint32_t boneCount = r.u32();
        if (boneCount > 65536) return out;
        out.bones.reserve(boneCount);
        for (std::uint32_t i = 0; i < boneCount; ++i) {
            JmxBone b;
            r.u8();                 // boneType (0 = bone, 1 = dummy)
            b.name = r.str();
            b.parent = r.str();
            for (int k = 0; k < 4; ++k) b.rot_parent[k] = r.f32();
            for (int k = 0; k < 3; ++k) b.trans_parent[k] = r.f32();
            for (int k = 0; k < 4; ++k) b.rot_origin[k] = r.f32();
            for (int k = 0; k < 3; ++k) b.trans_origin[k] = r.f32();
            r.skip(16 + 12);        // rotation/translation to local armature (unused here)
            std::uint32_t childCount = r.u32();
            for (std::uint32_t c = 0; c < childCount; ++c) r.str();
            out.bones.push_back(std::move(b));
        }
        out.ok = !out.bones.empty();
    } catch (...) { out.ok = !out.bones.empty(); }
    return out;
}

JmxAnim parse_ban(const std::uint8_t* data, std::size_t len) {
    JmxAnim out;
    try {
        Reader r(data, len);
        if (!r.sig_is("JMXVBAN")) return out;
        r.skip(12);
        r.u32(); r.u32();           // Int0, Int1 (0102)
        out.name = r.str();
        out.duration = r.u32();
        out.fps = r.u32();
        out.type = r.u32();
        std::uint32_t timeCount = r.u32();
        if (timeCount > 100000) return out;
        out.times.reserve(timeCount);
        for (std::uint32_t i = 0; i < timeCount; ++i) out.times.push_back(r.u32());
        std::uint32_t boneCount = r.u32();
        if (boneCount > 65536) return out;
        out.bones.reserve(boneCount);
        for (std::uint32_t i = 0; i < boneCount; ++i) {
            JmxAnimBone ab;
            ab.name = r.str();
            std::uint32_t kf = r.u32();
            if (kf > 1000000) return out;
            ab.rot.reserve(kf * 4); ab.trans.reserve(kf * 3);
            for (std::uint32_t k = 0; k < kf; ++k) {
                for (int q = 0; q < 4; ++q) ab.rot.push_back(r.f32());
                for (int v = 0; v < 3; ++v) ab.trans.push_back(r.f32());
            }
            out.bones.push_back(std::move(ab));
        }
        out.ok = !out.bones.empty() && !out.times.empty();
    } catch (...) { out.ok = false; }
    return out;
}

JmxCpd parse_cpd(const std::uint8_t* data, std::size_t len) {
    JmxCpd out;
    try {
        Reader r(data, len);
        if (!r.sig_is("JMXVCPD")) return out;
        r.skip(12);
        r.u32();                       // CollisionOffset
        std::uint32_t resOff = r.u32();
        for (int i = 0; i < 5; ++i) r.u32(); // unkUInt0..4
        if (resOff) {
            r.seek(resOff);
            std::uint32_t c = r.u32();
            for (std::uint32_t i = 0; i < c; ++i) out.resource_paths.push_back(r.str());
        }
        out.ok = true;
    } catch (...) { out.ok = !out.resource_paths.empty(); }
    return out;
}

} // namespace pk2
