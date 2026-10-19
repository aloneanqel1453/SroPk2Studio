// ============================================================================
//  SRO Asset Studio - formats/Bsr.h
//
//  JMXVRES 0109 - resource / composed object.  Wiki: JMXVRES.md
//
//  A .bsr is the thing the user actually wants to look at: it names the
//  material sets (.bmt), the mesh list (.bms), an optional skeleton (.bsk),
//  an animation list (.ban), mesh groups, animation groups, mod palettes and
//  the collision info.
//
//  The preview therefore has two levels:
//    ParseBsr        - structure only, no archive access. Feeds the tree view.
//    ComposeBsrModel - resolves .bms / .bmt / .ddj from INSIDE the same
//                      archive and produces a renderable model.
//
//  ComposeBsrModel takes an abstract loader callback rather than a Pk2Archive
//  so it can be unit-tested from loose files and so formats/ does not have to
//  depend on core/.
//
//  Implementation owner: Agent D (formats) + Agent E (render) for the compose
//  step.
// ============================================================================
#pragma once

#include "../Common.h"
#include "../render/Image.h"   // ComposedModel stores decoded textures as Image
#include "Bms.h"
#include "Bmt.h"

namespace sro {

constexpr char kBsrSignaturePrefix[] = "JMXVRES";

struct BsrHeader {
    u32 materialOffset      = 0;
    u32 meshOffset          = 0;
    u32 skeletonOffset      = 0;
    u32 animationOffset     = 0;
    u32 primMeshGroupOffset = 0;
    u32 primAniGroupOffset  = 0;
    u32 modPaletteOffset    = 0;
    u32 collisionOffset     = 0;
    u32 primMeshFlag        = 0;
    u32 modDataFlag         = 0;
    u32 int2 = 0, int3 = 0, int4 = 0;
};

struct BsrObjectInfo {
    u16          type     = 0;    ///< ResourceType
    u16          category = 0;    ///< ObjectGeneralCategory
    std::wstring name;
    u32          unk0 = 0, unk1 = 0;
};

struct BsrMaterialSet {
    u32          id = 0;          ///< 0..4, matches _RefObjChar.TextureType
    std::wstring path;            ///< .bmt path inside the archive
};

struct BsrMeshRef {
    std::wstring path;            ///< .bms path inside the archive
    u32          unk0 = 0;        ///< present when header.primMeshFlag & 1
};

struct BsrAnimationRef {
    std::wstring path;            ///< .ban path inside the archive
};

struct BsrMeshGroup {
    std::wstring        name;
    std::vector<u32>    meshIndices;   ///< indices into BsrFile::meshes
};

struct BsrAnimationEntry {
    u32 type      = 0;            ///< ResourceAnimationType
    u32 fileIndex = 0;            ///< index into BsrFile::animations
    struct Event { u32 keyTime = 0, type = 0, index = 0, unk = 0; };
    std::vector<Event> events;
    f32 walkLength = 0.0f;
    std::vector<Vec2> walkGraph;
};

struct BsrAnimationGroup {
    std::wstring name;
    std::vector<BsrAnimationEntry> entries;
};

struct BsrCollision {
    std::wstring meshPath;
    Aabb         box0;
    Aabb         box1;
    bool         hasMatrix = false;
    f32          matrix[16] = {};
};

// ---------------------------------------------------------------------------
// Mod palette  (JMXVRES 0109: header offset slot 7)
//
//  Two lists of ModDataSet - SystemModSets FIRST on disk, AnimationModSets
//  second. This is the ONLY place a resource names its sounds, its bone-mounted
//  particle effects and its extra texture layers.
//
//  A ModData payload has NO length prefix, so an unknown tag cannot be skipped:
//  it ends the walk. Every known type is therefore consumed byte-exactly, even
//  the ones that carry no path.
// ---------------------------------------------------------------------------
constexpr u32 kModDataMtrl         = 0x00000000u;  ///< advanced material - no path
constexpr u32 kModDataTexAni       = 0x00010000u;  ///< uv animation      - no path
constexpr u32 kModDataMultiTex     = 0x00010001u;  ///< -> .ddj
constexpr u32 kModDataMultiTexRev  = 0x00010002u;  ///< -> .ddj (own tag, same layout)
constexpr u32 kModDataParticle     = 0x00030000u;  ///< -> .efp
constexpr u32 kModDataEnvMap       = 0x00040000u;  ///< environment map   - no path
constexpr u32 kModDataBumpEnv      = 0x00040001u;  ///< -> .ddj
constexpr u32 kModDataSound        = 0x00050000u;  ///< -> .wav
constexpr u32 kModDataDyVertex     = 0x00060000u;  ///< base only
constexpr u32 kModDataDyJoint      = 0x00060001u;  ///< base only
constexpr u32 kModDataDyLattice    = 0x00060002u;  ///< base only
constexpr u32 kModDataProgEquipPow = 0x00070000u;  ///< base only

/// One sound cue. `event` is an event NAME, never a file.
struct BsrSoundTrack {
    std::wstring path;          ///< .wav inside a container
    i32          timeMs = 0;
    std::wstring event;
};

struct BsrSoundSet {
    std::wstring               name;    ///< label, not a file
    std::vector<BsrSoundTrack> tracks;
};

/// One .efp seated on a bone. `boneRelative` is a BONE NAME, never a file.
struct BsrParticleRef {
    std::wstring path;
    std::wstring boneRelative;
    bool         enabled = false;   ///< a disabled slot STILL names a real file
};

/// Everything the mod palette contributes to an export.
struct BsrModPalette {
    std::vector<std::wstring>   multiTextures;   ///< .ddj
    std::vector<BsrParticleRef> particles;       ///< .efp
    std::vector<BsrSoundSet>    soundSets;       ///< .wav
    int  setsRead      = 0;
    int  entriesRead   = 0;
    bool stoppedEarly  = false;   ///< unknown tag or truncation ended the walk
};

struct BsrFile {
    std::string   signature;
    std::wstring  version;
    BsrHeader     header;
    BsrObjectInfo objectInfo;
    BsrCollision  collision;

    std::vector<BsrMaterialSet>    materialSets;
    std::vector<BsrMeshRef>        meshes;
    std::vector<BsrAnimationRef>   animations;
    std::vector<BsrMeshGroup>      meshGroups;
    std::vector<BsrAnimationGroup> animationGroups;

    bool         hasSkeleton = false;
    std::wstring skeletonPath;      ///< .bsk
    std::wstring attachmentBone;

    u32 animationTypeVersion    = 0;
    u32 animationTypeUserDefine = 0;

    /// True when the section offsets in the header did not lead anywhere and the
    /// mesh/material paths had to be lifted out of the raw bytes instead. The
    /// paths are still the file's own - nothing is invented - but they have not
    /// been positionally verified, so the caller must confirm each one exists
    /// before trusting it. See RecoverRefsByScan in Bsr.cpp.
    bool refsRecoveredByScan = false;

    /// Sounds / particle effects / extra textures. Empty when the file has no
    /// mod palette, which is the common case for scenery.
    BsrModPalette modPalette;

    std::wstring summaryTr() const;
};

/// Structure-only parse. Never touches the archive.
Result ParseBsr(const u8* data, size_t size, BsrFile& out);

bool IsBsr(const u8* data, size_t size);

// ---------------------------------------------------------------------------
// Composition
//
// `AssetLoader` reads an arbitrary inner path out of whatever container the
// caller has open. Return false when the path does not exist - the composer
// then skips that part and records a warning instead of failing outright.
// ---------------------------------------------------------------------------
using AssetLoader = std::function<bool(const std::wstring& innerPath, std::vector<u8>& out)>;

/// A mesh from the model, already paired with its resolved material/texture.
struct ComposedPart {
    std::wstring meshPath;
    BmsFile      mesh;
    std::wstring materialName;
    bool         hasMaterial = false;
    BmtMaterial  material;
    std::wstring diffuseTexturePath;   ///< resolved .ddj path, empty if none
    int          textureIndex = -1;    ///< index into ComposedModel::textures

    /// True when this part's vertices were already MOVED out of their own
    /// bind-pose world space (an avatar wing seated on the spine). Such a part
    /// is skinned to its OWN skeleton, so animating it with the character's
    /// bind-inverse would apply the offset twice - it stays rigid instead.
    bool rigidPlaced = false;
};

/// Everything needed to render a .bsr.
struct ComposedModel {
    std::wstring              sourcePath;
    BsrFile                   resource;
    std::vector<ComposedPart> parts;
    std::vector<Image>        textures;     ///< decoded, RGBA8, deduplicated
    std::vector<std::wstring> texturePaths; ///< parallel to `textures`
    Aabb                      bounds;
    std::vector<std::wstring> warningsTr;   ///< missing meshes/textures, etc.

    /// Model-space "up" recovered from the skeleton (pelvis -> head), used to
    /// stand the model upright regardless of the axis it was authored on. Zero
    /// when there is no usable skeleton (the viewer then falls back to a
    /// bounding-box heuristic).
    Vec3 upHint{ 0, 0, 0 };
    /// Model-space "forward" (the way the face points), so the default view can
    /// turn the model to face the camera. Zero when unknown.
    Vec3 forwardHint{ 0, 0, 0 };

    u64 totalVertices() const;
    u64 totalFaces() const;
};

/// Load the meshes, materials and textures a .bsr references and build a
/// renderable model.
///   `bsrInnerPath`     path of the .bsr itself, used to resolve relative refs
///   `materialSetIndex` which of the (up to 5) material sets to use
///   `loadTextures`     false skips .ddj decoding (much faster for stats only)
Result ComposeBsrModel(const BsrFile& bsr,
                       const std::wstring& bsrInnerPath,
                       const AssetLoader& loader,
                       int materialSetIndex,
                       bool loadTextures,
                       ComposedModel& out);

/// Compose EXTRA avatar-set pieces (hat / attach / wings / transform body) on
/// top of an already-composed model, merging their meshes and textures into it
/// so one preview shows the whole dressed costume. `extraBsrRel` are inner .bsr
/// paths (relative resource paths are fine - resolved like every other ref).
/// `baseFolder` is the folder of the selected .bsr, tried as a fallback root.
/// Best-effort: a piece that will not load is noted in `inout.warningsTr` and
/// skipped. `addedOut` receives the number of pieces actually merged.
struct BskFile;   // fwd - character skeleton used to seat skinned attach pieces

/// Seat a skinned attach piece (wing/cape) at the character's attach bone using
/// a real character skeleton's bind pose - the exact placement the game uses.
void ApplyCharacterAttach(ComposedModel& m, const std::wstring& attachBone,
                          const BskFile& charSkel);

Result ComposeAvatarPieces(const std::vector<std::wstring>& extraBsrRel,
                           const std::wstring& baseFolder,
                           const AssetLoader& loader,
                           ComposedModel& inout,
                           int& addedOut,
                           const BskFile* charSkel = nullptr);

/// Recover or synthesize a BsrFile when raw parsing failed but the asset has a
/// known profile (e.g. ArcaneMax / Coiny custom pets like p_teemo_01.bsr or
/// custom avatar BSRs whose components are present in the archive).
bool RecoverCustomBsr(const std::wstring& innerPath, BsrFile& out);

/// Export a composed model to Wavefront OBJ (+ optional MTL).
Result ComposedModelToObj(const ComposedModel& model,
                          const std::wstring& mtlName,
                          std::string& outObj,
                          std::string& outMtl);

} // namespace sro
