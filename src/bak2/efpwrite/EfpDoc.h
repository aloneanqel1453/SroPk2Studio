// SkillEffect Studio - efpwrite/EfpDoc.h  (v6 M22)
// Lossless JMXVEFF 0010-0013 (.efp) model: Parse -> edit -> Serialize. Every payload, time triple and the root
// scale are kept as the raw bytes of the file and are only re-encoded when an edit changes them, so an
// unedited document serializes byte-identical (skipLen is recomputed; a disagreeing stored value is kept as an
// override; bytes after the root object are kept raw). Unknown controller names are bounded by skipLen, unknown
// command / parameter names are recovered by re-parsing with candidate payload sizes (efsanesro efp.py rule).
// Legacy "JMXVEFF 0000" is refused (different layout, no client loader: EFP_RUNTIME.md §13-15).
// No dependency on other modules. Strings are the raw file bytes (CP949 / ASCII).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace se::efpw {

using Bytes = std::vector<std::uint8_t>;

struct Source {                    // EESourceData
    bool has = false;
    std::string command;
    std::uint8_t type = 0;         // sourceType = interpreter variant (opcode = base + type)
    std::uint8_t mode = 0;         // sourceByte = window mode bits (sub_CA0F40)
    std::array<std::uint8_t, 12> timesRaw{};   // f32 a (start), b (step), c (end) - raw
    Bytes payload;
    bool unknown = false;          // command name not in the known table (payload size recovered)

    float Time(int i) const;
    void SetTime(int i, float v);  // re-encodes one float of the triple
};

struct Named {                     // controller / global parameter
    std::string name;
    Bytes payload;
    bool unknown = false;
};

struct ResMesh {
    std::string mesh;
    std::vector<std::string> textures;
};

struct Resource {                  // EEResource
    std::array<std::uint32_t, 9> state{3, 5, 6, 2, 0, 2, 2, 0, 2};   // cull, src, dst, colour arg1/arg2/op, alpha arg1/arg2/op
    std::vector<ResMesh> meshes;
};

struct Node {                      // EFStoredObject
    std::string name;
    std::vector<Named> controllers;
    std::uint32_t lifeFrames = 0;
    std::vector<Named> globals;
    std::vector<Source> list0, emitList, list2;
    Source life;
    std::vector<Source> programList;
    std::array<std::uint8_t, 20> scalars{};    // u8 b0, u8 b1, i32 Int1, i32 Int2, i32 Int3, u8 b5, i32 Int0, u8 b7
    Source view;
    Resource res;
    Source render;
    std::vector<Source> list3, baked;          // baked = per-life program the runtime executes
    std::vector<Node> children;
    bool skipOverride = false;                 // stored skipLen disagreed with the layout -> written as stored
    std::uint32_t skipStored = 0;

    Named* Controller(const char* n);
    const Named* Controller(const char* n) const;
    Named* Global(const char* n);
    const Named* Global(const char* n) const;
    int CountNodes() const;
};

struct Doc {
    std::string version = "0011";              // "0010".."0013"
    std::array<std::uint8_t, 4> rootScaleRaw{0, 0, 0x80, 0x3F};   // f32 1.0 (0012 / 0013 only)
    std::array<std::uint8_t, 12> ints0013{};   // 3 x i32 raw (0013 only)
    Node root;
    Bytes trailing;                            // bytes after the root object (kept raw)
    std::vector<std::string> notes;            // reader notes (unknown names recovered, skip overrides, trailing)

    bool HasRootScale() const { return version == "0012" || version == "0013"; }
    float RootScale() const;                   // 1 when the version has none
    void SetRootScale(float v);                // 0012 / 0013 only
};

// data -> doc. false + err ("..." @0xOFFSET) on bad data. maxRecover = re-parse attempts for unknown names.
bool Parse(const std::uint8_t* data, std::size_t size, Doc& out, std::string& err, int maxRecover = 4000);
inline bool Parse(const Bytes& b, Doc& out, std::string& err) { return Parse(b.data(), b.size(), out, err); }
Bytes Serialize(const Doc& d);

// Node walk in depth-first pre-order (= render DescribeEffect / EfpEffect::SetNodeHidden index order).
std::vector<Node*> NodesPreOrder(Doc& d);
std::vector<const Node*> NodesPreOrder(const Doc& d);
std::vector<std::string> NodePaths(const Doc& d);      // "root/child/grandchild" names, pre-order

// Every source list of a node that holds program commands (list0, list2, programList, list3, baked) plus the
// Program controller's editor copy (decoded on demand). Fixed slots (life / view / render / emitList) excluded.
bool IsKnownCommand(const std::string& c);
bool IsProgramCommand(const std::string& c);   // a command the interpreter runs (efp_sim VARIANTS)

// --- small payload helpers (little endian) ------------------------------------------------------------
float GetF32(const Bytes& b, std::size_t off);
void PutF32(Bytes& b, std::size_t off, float v);
std::uint32_t GetU32(const Bytes& b, std::size_t off);
void PutU32(Bytes& b, std::size_t off, std::uint32_t v);

// Program controller payload <-> source list (editor copy of the program)
bool DecodeSourceList(const Bytes& payload, std::vector<Source>& out);
Bytes EncodeSourceList(const std::vector<Source>& v);
// Shape controller payload <-> (render command name, resource)
bool DecodeShape(const Bytes& payload, std::string& render, Resource& res);
Bytes EncodeShape(const std::string& render, const Resource& res);

}  // namespace se::efpw
