// ============================================================================
//  SRO Asset Studio - formats/MapFiles.h
//
//  The world/map family. For all of these the spec only requires a HEADER
//  SUMMARY + COUNTS in the preview pane, so each parser reads the header and
//  the top-level counts and stops - but every read is still bounds-checked.
//
//    .nvm   JMXVNVM 1000    navigation mesh          (JMXVNVM.md)
//    .dof   JMXVDOF ....    dungeon object file      (JMXVDOF.md)
//    .m     JMXVMAPM1000    terrain mesh, 6x6 blocks (JMXVMAPM.md)
//    .t     JMXVMAPT1001    terrain lightmap + DDS   (JMXVMAPT.md)
//    .o/.o2 JMXVMAPO1001    object placement         (JMXVMAPO.md)
//    .mfo   JMXVMFO 1000    map project bitmap       (JMXVMFO.md)
//    .cpd   JMXVCPD 0101    compound object          (JMXVCPD.md)
//    .ifo   text            environment / object / objectstring / objext / tile2d
//
//  Implementation owner: Agent D (formats).
// ============================================================================
#pragma once

#include "../Common.h"
#include "../render/Image.h"

namespace sro {

// ---------------------------------------------------------------------------
// Common
// ---------------------------------------------------------------------------

/// A Silkroad region id packs X in the low byte and Z in the high byte;
/// bit 15 of the high byte marks a dungeon.
struct RegionId {
    u16 raw = 0;
    int x() const { return raw & 0xFF; }
    int z() const { return (raw >> 8) & 0x7F; }
    bool isDungeon() const { return (raw & 0x8000) != 0; }
    std::wstring toStringTr() const;
};

// ---------------------------------------------------------------------------
// .nvm  - JMXVNVM 1000
// ---------------------------------------------------------------------------
struct NvmObjectInstance {
    i32  assetId = 0;          ///< index into object.ifo
    Vec3 localPosition;
    i16  type = 0;
    f32  yaw  = 0.0f;
    i16  localUid = 0;
    bool isBig = false;
    bool isStruct = false;
    u16  regionId = 0;
};

struct NvmFile {
    std::string signature;                 ///< "JMXVNVM 1000"
    std::wstring version;
    u16 objectCount   = 0;
    u32 cellQuadCount = 0;
    u32 globalEdgeCount = 0;
    u32 internalEdgeCount = 0;
    u32 tileCount     = 0;
    u32 heightCount   = 0;
    std::vector<NvmObjectInstance> objects;   ///< only when loadObjects
    std::vector<float> heightMap;             ///< 97*97 terrain heights (for 3D)
    bool partial = false;                     ///< stopped before the end

    std::wstring summaryTr() const;
};

Result ParseNvm(const u8* data, size_t size, NvmFile& out, bool loadObjects);

// ---------------------------------------------------------------------------
// .dof  - dungeon object file
// ---------------------------------------------------------------------------
/// A decoration placed inside a dungeon block: its own .bsr with a full
/// position / rotation / scale, so a dungeon is a real 3D scene, not a list.
struct DofObject {
    std::wstring name;
    std::wstring path;              ///< .bsr
    Vec3  position;
    Vec3  rotation;                 ///< radians, per axis
    Vec3  scale{ 1, 1, 1 };
    u32   flag = 0;                 ///< 0 none, 2 collision-only, 4 water
    f32   radiusSqrt = 0.0f;
    u32   waterColor = 0;           ///< only when (flag & 4)

    bool isWater()     const { return (flag & 4) != 0; }
    bool isCollision() const { return (flag & 2) != 0; }
};

struct DofLight {
    std::wstring name;
    Vec3 position;
    Vec3 diffuse, ambient, specular;
    f32  f0 = 0.0f, f1 = 0.0f, f2 = 0.0f;   ///< attenuation
};

struct DofFog {
    u32  color     = 0;
    f32  nearPlane = 0.0f, farPlane = 0.0f, intensity = 0.0f;
    bool heightFog = false;
};

/// One room/corridor piece of the dungeon: a .bsr placed at position + yaw.
struct DofBlock {
    std::wstring path;              ///< .bsr of the room shell
    std::wstring name;
    Vec3  position;
    f32   yaw = 0.0f;               ///< radians about Y
    bool  isEntrance = false;
    Aabb  collisionBox;
    DofFog fog;
    u32   roomIndex = 0, floorIndex = 0;
    std::vector<u32>       connected;   ///< walkable neighbours
    std::vector<u32>       visible;     ///< portal-culling visibility set
    std::vector<DofObject> objects;
    std::vector<DofLight>  lights;
};

struct DofGroup {
    std::wstring     name;
    u32              flag = 0;
    std::vector<u32> blockIndices;
};

struct DofFile {
    std::string  signature;
    std::wstring version;
    u32 blockOffset = 0, linkOffset = 0, gridOffset = 0;
    u32 groupOffset = 0, labelOffset = 0, boundingBoxOffset = 0;
    u32 objectType = 0;
    std::wstring objectName;
    RegionId region;
    Aabb collisionBox0, collisionBox1;
    std::vector<DofBlock>  blocks;
    std::vector<DofGroup>  groups;
    std::vector<std::wstring> roomNames;    ///< label table
    std::vector<std::wstring> floorNames;
    u32 linkCount = 0, groupCount = 0, labelCount = 0;
    bool partial = false;

    /// Total decorations across every block (what the 3D view has to place).
    size_t objectCount() const {
        size_t n = 0;
        for (const DofBlock& b : blocks) n += b.objects.size();
        return n;
    }
    size_t lightCount() const {
        size_t n = 0;
        for (const DofBlock& b : blocks) n += b.lights.size();
        return n;
    }

    std::wstring summaryTr() const;
};

Result ParseDof(const u8* data, size_t size, DofFile& out);

// ---------------------------------------------------------------------------
// .m  - JMXVMAPM1000 terrain mesh, 6x6 blocks of 17x17 vertices
// ---------------------------------------------------------------------------
constexpr int kMapBlocksPerSide     = 6;
constexpr int kMapVerticesPerSide   = 17;
constexpr int kMapTilesPerSide      = 16;

struct MapMeshBlock {
    u32  flag = 0;                 ///< 0 = None, 1 = Culled
    u16  environmentId = 0;
    i8   waterType = -1;           ///< -1 None, 0 Water, 1 Ice
    u8   waterWaveType = 0;
    f32  waterHeight = 0.0f;
    f32  heightMax = 0.0f, heightMin = 0.0f;
    f32  heights[kMapVerticesPerSide * kMapVerticesPerSide] = {};
    u16  textures[kMapVerticesPerSide * kMapVerticesPerSide] = {};
    u8   brightness[kMapVerticesPerSide * kMapVerticesPerSide] = {};
    u16  tileFlags[kMapTilesPerSide * kMapTilesPerSide] = {};
};

struct MapMeshFile {
    std::string signature;                 ///< "JMXVMAPM1000"
    std::vector<MapMeshBlock> blocks;      ///< 36 when complete
    f32 globalMin = 0.0f, globalMax = 0.0f;
    bool partial = false;

    /// Render the height field as a greyscale image (36 * 17 = 102 px side).
    Image toHeightImage() const;

    std::wstring summaryTr() const;
};

Result ParseMapMesh(const u8* data, size_t size, MapMeshFile& out);

// ---------------------------------------------------------------------------
// .t  - JMXVMAPT1001 terrain lightmap
// ---------------------------------------------------------------------------
constexpr size_t kMapTileLightMapBytes = 96 * 96;

struct MapTextureFile {
    std::string signature;              ///< "JMXVMAPT1001"
    std::vector<u8> tileLightMap;       ///< 9216 bytes, one byte per tile
    u32 lightMapBufferSize = 0;
    u32 lightMapTextureType = 0;
    size_t ddsOffset = 0;               ///< embedded DDS payload
    size_t ddsSize   = 0;

    /// The 96x96 per-tile light map as a greyscale image.
    Image toTileLightImage() const;

    std::wstring summaryTr() const;
};

Result ParseMapTexture(const u8* data, size_t size, MapTextureFile& out);

// ---------------------------------------------------------------------------
// .o / .o2 - JMXVMAPO1001 object placement
// ---------------------------------------------------------------------------
struct MapObjectInstance {
    u32  objectId = 0;          ///< index into object.ifo
    Vec3 localPosition;
    i16  isStatic = 0;          ///< 0 = no, -1 (0xFFFF) = yes
    f32  yaw = 0.0f;
    i16  uid = 0;
    i16  short0 = 0;
    bool isBig = false;
    bool isStruct = false;
    u16  regionId = 0;          ///< .o2 only
    int  lodGroup = 0;          ///< .o2 only, 0..3
    int  blockX = 0, blockZ = 0;
};

struct MapObjectFile {
    std::string signature;               ///< "JMXVMAPO1001"
    bool isO2 = false;                   ///< 4 LOD groups per block
    std::vector<MapObjectInstance> objects;
    bool partial = false;

    std::wstring summaryTr() const;
};

/// `forceO2` overrides the extension-based guess (some servers ship .o data
/// under an .o2 name); pass -1 to auto-detect by trying both and keeping the
/// one that consumes the buffer exactly.
Result ParseMapObject(const u8* data, size_t size, int forceO2, MapObjectFile& out);

// ---------------------------------------------------------------------------
// .mfo - JMXVMFO 1000 map project
// ---------------------------------------------------------------------------
constexpr size_t kMfoRegionBytes = 8192;   ///< 256*256 bits

struct MfoFile {
    std::string signature;         ///< "JMXVMFO 1000"
    i16 mapWidth = 0, mapHeight = 0;
    i16 short0 = 0, short1 = 0, short2 = 0, short3 = 0;
    std::vector<u8> regionBits;    ///< 8192 bytes

    bool regionEnabled(int x, int z) const;
    /// The region bitmap as a 256x256 black/white image.
    Image toRegionImage() const;

    std::wstring summaryTr() const;
};

Result ParseMfo(const u8* data, size_t size, MfoFile& out);

// ---------------------------------------------------------------------------
// .cpd - JMXVCPD 0101 compound
// ---------------------------------------------------------------------------
struct CpdFile {
    std::string  signature;        ///< "JMXVCPD 0101"
    u32 collisionOffset = 0, resourceOffset = 0;
    u32 unk[5] = {};
    u32 objectType = 0;
    std::wstring objectName;
    std::wstring collisionResourcePath;
    std::vector<std::wstring> resourcePaths;   ///< .bsr list

    std::wstring summaryTr() const;
};

Result ParseCpd(const u8* data, size_t size, CpdFile& out);

// ---------------------------------------------------------------------------
// .ifo - text variants (NOT binary)
// ---------------------------------------------------------------------------
enum class IfoVariant : int {
    Unknown = 0,
    Object,          ///< object.ifo        : index, flag, "path"
    ObjectString,    ///< objectstring.ifo  : uid, flag, xsec, ysec, x, y, z, yaw, "name"
    ObjectExt,       ///< objext.ifo        : uid, "", "name"
    Tile2d,          ///< tile2d.ifo        : index, type, "category", "file", grass
    Environment      ///< environment.ifo
};

struct IfoRow {
    std::vector<std::wstring> fields;
};

struct IfoFile {
    IfoVariant  variant = IfoVariant::Unknown;
    std::wstring header;              ///< first line
    u32          declaredCount = 0;   ///< second line
    std::vector<IfoRow> rows;
    std::vector<std::wstring> columnHeadersTr;  ///< for the table view

    std::wstring summaryTr() const;
};

/// Detects the variant from the file name and the shape of the first data row.
Result ParseIfo(const u8* data, size_t size, const std::wstring& fileName, IfoFile& out);

// ---------------------------------------------------------------------------
// JMXVENVI1003 - binary environment set (colour grading over the day)
// ---------------------------------------------------------------------------
struct EnviProfile {
    u16          id = 0;
    std::wstring name;
    std::wstring dayBgm, nightBgm;
    /// The 16 graphs are summarised by their key counts; the full curves are
    /// only needed if somebody later builds a gradient editor.
    u32 graphKeyCounts[16] = {};
};

struct EnviFile {
    std::string  signature;      ///< "JMXVENVI1003"
    std::wstring version;
    i16          profileCount = 0;
    std::wstring environmentSetName;
    std::vector<EnviProfile> profiles;
    bool partial = false;

    std::wstring summaryTr() const;
};

Result ParseEnvi(const u8* data, size_t size, EnviFile& out);

} // namespace sro
