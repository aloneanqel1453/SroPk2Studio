// ============================================================================
//  SRO Asset Studio - formats/Bms.h
//
//  JMXVBMS 0110 - mesh.  Wiki: JMXVBMS.md
//
//  Layout recap (all offsets are absolute file offsets stored in the header):
//    12  signature "JMXVBMS 0110"
//    13 x u32 section offsets + flags (see BmsHeader)
//    u32 subPrimCount (asserted 1), u32 vertexFlag, u32 unk
//    len-prefixed Name, len-prefixed Material, u32 unk
//
//    VertexOffset  : u32 count, then per vertex
//                      Vec3 position, Vec3 normal, Vec2 uv0
//                      [+ Vec2 uv1        if vertexFlag & 0x400]
//                      [+ 36 bytes morph  if vertexFlag & 0x800]
//                      f32 f0, u32 i0, u32 i1
//                    [+ len-prefixed lightmap path if vertexFlag & 0x400]
//                    [+ extra vertex data          if vertexFlag & 0x1000]
//    SkinOffset    : u32 boneCount, bone names, then per vertex
//                      u8 idx1, u16 w1, u8 idx2, u16 w2
//    FaceOffset    : u32 count, then u16 a,b,c
//    BoundingBox   : 24 bytes = Vec3 min, Vec3 max
//
//  SRO winding is CLOCKWISE (left-handed), Y-up. UV: U left->right,
//  V top->bottom. The rasterizer relies on that.
//
//  We parse what the preview needs and record the rest as counts. Everything
//  is bounds-checked through ByteReader; a truncated file yields a partial but
//  consistent BmsFile plus Status::Truncated.
//
//  Implementation owner: Agent D (formats).
// ============================================================================
#pragma once

#include "../Common.h"

namespace sro {

constexpr char kBmsSignaturePrefix[] = "JMXVBMS";

/// VertexFlag bits.
constexpr u32 kBmsVertexLightMapUV   = 0x400;
constexpr u32 kBmsVertexMorphingData = 0x800;
constexpr u32 kBmsVertexExtraData    = 0x1000;   ///< seen in newer ISRO-R models

/// NavFlag bits (header.navFlag).
constexpr u32 kBmsNavEdge  = 1;
constexpr u32 kBmsNavCell  = 2;
constexpr u32 kBmsNavEvent = 4;

struct BmsHeader {
    u32 vertexOffset          = 0;
    u32 skinOffset            = 0;
    u32 faceOffset            = 0;
    u32 clothVertexOffset     = 0;
    u32 clothEdgeOffset       = 0;
    u32 boundingBoxOffset     = 0;
    u32 occlusionPortalOffset = 0;
    u32 navMeshOffset         = 0;
    u32 skinnedNavMeshOffset  = 0;
    u32 unknown9Offset        = 0;
    u32 unkUInt0              = 0;
    u32 navFlag               = 0;
};

/// One vertex, already widened to what the rasterizer wants.
struct BmsVertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv0;
    Vec2 uv1;        ///< lightmap UV, valid only when hasLightmapUV
};

/// Two-bone skinning weights, as stored.
struct BmsSkinWeight {
    u8  boneIndex0 = 0;
    u16 weight0    = 0;
    u8  boneIndex1 = 0;
    u16 weight1    = 0;
};

struct BmsFace {
    u16 a = 0, b = 0, c = 0;
};

struct BmsFile {
    std::string  signature;         ///< "JMXVBMS 0110"
    std::wstring version;           ///< "0110"
    BmsHeader    header;

    u32          subPrimCount = 0;
    u32          vertexFlag   = 0;
    std::wstring name;              ///< mesh name
    std::wstring material;          ///< material name inside the active .bmt set
    std::wstring lightmapPath;      ///< .ddj, only when vertexFlag & 0x400

    bool hasLightmapUV = false;
    bool hasMorphData  = false;

    std::vector<BmsVertex>     vertices;
    std::vector<BmsFace>       faces;
    std::vector<std::wstring>  boneNames;      ///< skinning bone list
    std::vector<BmsSkinWeight> skinWeights;    ///< one per vertex when skinned

    /// Counts as DECLARED in the file. Always filled, even when
    /// ParseBms was called with loadGeometry = false and the arrays above are
    /// therefore empty. The info pane reads these.
    u32 headerVertexCount = 0;
    u32 headerFaceCount   = 0;

    Aabb boundingBox;

    // --- counts we surface in the info pane without fully parsing ----------
    u32 clothVertexCount   = 0;
    u32 clothEdgeCount     = 0;
    u32 navVertexCount     = 0;
    u32 navCellCount       = 0;
    u32 navOutlineCount    = 0;
    u32 navInlineCount     = 0;
    bool hasOcclusionPortal = false;
    std::wstring occlusionPortalName;

    bool isSkinned() const { return !boneNames.empty(); }

    /// Turkish multi-line summary for the info pane.
    std::wstring summaryTr() const;
};

/// Parse a .bms buffer.
/// `loadGeometry = false` skips vertex/face arrays (fast header-only pass used
/// by the tree/info pane); counts are still filled in.
Result ParseBms(const u8* data, size_t size, BmsFile& out, bool loadGeometry);

/// True when the buffer begins with "JMXVBMS".
bool IsBms(const u8* data, size_t size);

/// Export to Wavefront OBJ text (UTF-8). `mtlName` may be empty.
/// Used by "extract with conversion".
Result BmsToObj(const BmsFile& mesh, const std::wstring& mtlName, std::string& outObj);

} // namespace sro
