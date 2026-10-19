// SkillEffect Studio - render/Efp.h
// JMXVEFF 0010-0013 (.efp) reader - layout of efsanesro/formats/efp.py (byte-exact reader, 5 383 files).
// Legacy "JMXVEFF 0000" (7 files) is refused (no loader in the client either: EFP_RUNTIME.md §13-15).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace se::render {

struct EfpSource {             // EESourceData
    bool has = false;
    std::string command;
    std::uint8_t sourceType = 0, sourceByte = 0;
    float times[3] = {0, 0, 0};    // start, end(step), span - window triple (sub_CA0F40)
    std::vector<std::uint8_t> payload;
};

struct EfpNamed {              // controller / global parameter with raw payload
    std::string name;
    std::vector<std::uint8_t> payload;
};

struct EfpResMesh {
    std::string mesh;
    std::vector<std::string> textures;
};

struct EfpResource {           // EEResource: 9 dword render state + mesh/texture tree
    std::uint32_t cull = 3, src = 5, dst = 6;
    std::uint32_t stage1[3] = {2, 0, 2};  // colour arg1, arg2, op
    std::uint32_t stage2[3] = {2, 0, 2};  // alpha arg1, arg2, op
    std::vector<EfpResMesh> meshes;
    std::string Texture() const;   // first texture of the first mesh entry that has one
    std::string Mesh() const;      // first non-empty mesh path
};

struct EfpObject {             // EFStoredObject
    std::string name;
    std::vector<EfpNamed> controllers;
    std::uint32_t lifeFrames = 0;
    std::vector<EfpNamed> globals;
    std::vector<EfpSource> list0, emitList, list2, programList, list3, baked;
    EfpSource life, view, render;
    std::array<std::uint8_t, 20> scalars{};   // u8 b0, u8 b1, i32 Int1, i32 Int2, i32 Int3, u8 b5, i32 Int0, u8 b7
    EfpResource res;
    std::vector<std::unique_ptr<EfpObject>> children;
    const EfpNamed* Global(const char* n) const;
    int CountNodes() const;
};

struct EfpFile {
    std::string version;       // "0010".."0013"
    float rootScale = 1.0f;
    bool hasRootScale = false;   // 0012 / 0013
    std::int32_t ints0013[3] = {0, 0, 0};
    std::unique_ptr<EfpObject> root;
};

bool ParseEfp(const std::uint8_t* data, std::size_t size, EfpFile& out, std::string& err);

// payload helpers
struct EfpBlendV3Key { float t; float v[3]; };
std::vector<EfpBlendV3Key> DecodeBlendScaleGraph(const std::vector<std::uint8_t>& payload);
struct EfpStaticEmit { std::uint32_t start = 0, duration = 0, interval = 0, maxAlive = 0; float perEvent = 0; };
bool DecodeStaticEmit(const std::vector<std::uint8_t>& payload, EfpStaticEmit& e);
// BSAnimation global: .ban paths
std::vector<std::string> DecodeStrList(const std::vector<std::uint8_t>& payload);

}  // namespace se::render
