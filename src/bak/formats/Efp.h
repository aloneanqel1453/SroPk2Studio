// ============================================================================
//  SRO Asset Studio - formats/Efp.h
//
//  JMXVEFF - particle effect.  Wiki: JMXVEFF.md
//
//    8   "JMXVEFF "
//    4   version  "0011" | "0012" | "0013"
//    if version >= 0012 : f32 rootScale
//    if version == 0013 : 3 x i32
//    *   EFStoredObject rootObject   (recursive tree)
//
//  EFStoredObject is a deeply nested, controller-driven structure whose exact
//  contents depend on string-named controller types. The spec only asks for a
//  STRUCTURED SUMMARY here, so this parser deliberately extracts the tree
//  skeleton (names, controller names, child counts, referenced resources) and
//  records how many bytes it could not interpret, rather than pretending to
//  understand every controller.
//
//  Rule for the implementer: on any unrecognised controller name, stop
//  descending that branch, mark EfpNode::partial = true and keep going with
//  the siblings. Never guess a size.
//
//  Implementation owner: Agent D (formats).
// ============================================================================
#pragma once

#include "../Common.h"

namespace sro {

constexpr char kEfpSignature[] = "JMXVEFF ";

// ---------------------------------------------------------------------------
// Emitter state
//
// Retained so an effect can actually be SIMULATED. Field widths and orders are
// the reference implementation's (sro-archive-explorer), verified against its
// source rather than inferred - a wrong width here desynchronises the stream
// and loses every emitter after it.
// ---------------------------------------------------------------------------
enum class EfpLifeMode   : u8 { Never = 0, Extinct, Loop };
enum class EfpRenderMode : u8 { Unknown = 0, None, Plate, Mesh };
enum class EfpViewMode   : u8 { None = 0, Billboard, YBillboard, VBillboard };

/// EFStaticEmit - 20 bytes: four u32 then the spawn rate.
struct EfpStaticEmit {
    u32  minVal = 0, maxVal = 0, burstRate = 0, minParticles = 0;
    f32  spawnRate = 1.0f;
    bool present = false;
};

struct EfpScaleKey   { f32 time = 0.0f; Vec3 v{ 1.0f, 1.0f, 1.0f }; };
/// One texture-frame record: four raw floats whose meaning depends on whether
/// the emitter's whole list passes the atlas gate.
struct EfpTexFrame   { f32 f[4] = { 0, 0, 1, 1 }; };
/// Colour bytes sit on disk as B,G,R,A - reordered here on read.
struct EfpDiffuseKey { f32 time = 0.0f; f32 r = 1, g = 1, b = 1, a = 1; };

/// The nine ints in front of an EEResource. Defaults are D3D's own
/// (SRCALPHA / INVSRCALPHA), so a resource-less node still blends sanely.
struct EfpRenderState {
    u32 backFaceType = 0;
    u32 srcBlend = 5, dstBlend = 6;
    u32 srcArg1 = 2, srcArg2 = 0, srcOp = 4;
    u32 dstArg1 = 2, dstArg2 = 0, dstOp = 4;
    bool present = false;
    std::vector<std::wstring> meshPaths;      ///< .bms, paired with textures
    std::vector<std::wstring> texturePaths;   ///< .ddj
};

/// One node of the effect object tree.
struct EfpNode {
    std::wstring              name;
    i32                       dataOffset = 0;
    std::vector<std::wstring> controllerNames;
    std::wstring              lifeTimeSource;    ///< "NeverExtinct", "NormalTimeLoop", ...
    std::wstring              viewModeSource;    ///< "ViewBillboard", ...
    std::wstring              renderSource;      ///< "RenderPlate", "RenderMesh", ...
    std::vector<std::wstring> resourcePaths;     ///< .ddj / .bms referenced by this node
    std::vector<EfpNode>      children;

    /// True when the parser hit a controller it does not model and stopped
    /// descending. The UI shows this honestly rather than silently truncating.
    bool partial = false;

    // --- retained emitter state -------------------------------------------
    EfpLifeMode    lifeMode   = EfpLifeMode::Never;
    EfpRenderMode  renderMode = EfpRenderMode::Unknown;
    EfpViewMode    viewMode   = EfpViewMode::None;
    EfpStaticEmit  emit;
    EfpRenderState render;
    /// Particle life in 20 fps frames (EEGlobalData Int0). 0 = unset.
    u32            lifeFrames = 0;
    /// Command window, seconds, widened by every sane source triple.
    f32            startSec = 0.0f, endSec = 0.0f;
    /// The StaticEmit COMMAND's own window. When it is a real range it - not
    /// the object's union - decides when the emitter switches on and off; the
    /// object window is only the fallback.
    f32            emitStartSec = 0.0f, emitEndSec = 0.0f;
    bool           hasEmitRange = false;
    std::vector<EfpScaleKey>   scaleKeys;
    std::vector<EfpDiffuseKey> diffuseKeys;
    /// The global BlendScaleGraph / BlendDiffuseGraph curves. A resolved command
    /// frame count is RESAMPLED from these; without them the counts are right
    /// but every value collapses to 1.0 and the effect renders as a speck.
    /// The authored BAN path. A LinkPipe / LinkDPipe / LinkObj emitter only
    /// becomes a ribbon when this has more than one point; with a single point
    /// (or none) it is not a trail at all.
    std::vector<Vec3> banPositions;
    f32               banSpeed = 0.0f;

    /// Texture frames - an atlas cell schedule, or a scroll.
    std::vector<EfpTexFrame>   texFrames;

    std::vector<EfpScaleKey>   blendScale;
    std::vector<EfpDiffuseKey> blendDiffuse;

    // --- authored motion ---------------------------------------------------
    /// Units per 20 fps FRAME, not per second.
    Vec3 velocity{ 0, 0, 0 };
    /// Units per frame per frame; summed as a DISCRETE arithmetic series.
    Vec3 force{ 0, 0, 0 };
    /// The emitter's own offset.
    Vec3 position{ 0, 0, 0 };
    /// Per-axis radius of the spawn sphere (SetSpherePos).
    Vec3 spawnSphere{ 0, 0, 0 };
    bool hasSpawnSphere = false;
    /// Pulls a particle onto its own velocity axis as it ages.
    f32  attraction = 0.0f;

    /// Cone spawn / velocity / force. The triple is (min, max, angle), NOT a
    /// direction: it gives every particle its own randomized heading, which is
    /// what makes a spray fan out instead of firing along one line.
    Vec3 spawnCone{ 0, 0, 0 };
    bool hasSpawnCone = false;
    Vec3 coneVelocity{ 0, 0, 0 };
    bool hasConeVelocity = false;
    Vec3 coneForce{ 0, 0, 0 };

    /// Shape rotation: a base orientation and a PER-FRAME step that is raised
    /// to the power of the particle's frame age. Without these, rune circles,
    /// vortices and tumbling debris are all dead still.
    f32  shapeRotation[16]    = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    f32  shapeRotationVel[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    bool hasShapeRotation = false;
    bool hasShapeRotationVel = false;

    /// The TextureSlide header triple. When it describes an integer grid the
    /// authored frames are REPLACED by a generated cell schedule, and the
    /// sampler switches to the stepped (non-wrapping) form.
    f32  texSlideGrid[3] = { 0, 0, 0 };
    bool hasTexSlideGrid = false;
    /// Set when the grid produced the frames: the sheet then STEPS and holds
    /// on its last cell rather than wrapping.
    bool texFramesStepped = false;

    /// Selects which curve supplies the per-particle size jitter. 0 picks the
    /// exported BlendScaleGraph; anything else means no jitter.
    i32  randomScalePointer = -1;
    bool hasRandomScale = false;

    /// True when the SetGraphDiffuse COMMAND was present - even if it carried
    /// no frames. The command floors opacity at 0.12; a controller-supplied
    /// curve uses its own header alpha instead.
    bool diffuseFromCommand = false;

    /// True only when the size curve came from the SetGraphScale COMMAND. The
    /// reference derives the emitter's scalar size from that command alone; a
    /// curve that arrived from a ScaleGraph CONTROLLER leaves the scalar at 1.
    bool scaleFromCommand = false;

    /// The object's own rotation, and the routing byte that says how it
    /// combines with the parent's. Position, velocity, force and the BAN path
    /// are all expressed THROUGH this - without it a child emitter's motion
    /// points the wrong way, which is most of the velocity/force disagreement
    /// against the reference.
    f32  rotationMatrix[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    bool hasRotationMatrix = false;
    u8   transformFlags = 0;          ///< SetRotationMat source type
    u8   positionSourceType = 1;      ///< SetPosition source type
    u8   velocitySourceType = 0;      ///< SetVelocity source type
    u8   forceSourceType = 0;         ///< Force source type
    u8   banPositionSourceType = 1;   ///< SetBANPos source type
    /// The authored scalar size. The curve maximum is NOT the same number.
    f32  authoredScale = 1.0f;
    bool hasAuthoredScale = false;

    size_t countNodes() const;
};

struct EfpFile {
    std::string  signature;      ///< "JMXVEFF "
    std::wstring version;        ///< "0011" | "0012" | "0013"
    bool         hasRootScale = false;
    f32          rootScale    = 1.0f;
    i32          v13Int0 = 0, v13Int1 = 0, v13Int2 = 0;

    EfpNode root;
    size_t  bytesParsed      = 0;
    size_t  bytesTotal       = 0;
    bool    fullyUnderstood  = false;

    /// Every unique resource path referenced anywhere in the tree.
    std::vector<std::wstring> allResourcePaths() const;

    std::wstring summaryTr() const;
};

Result ParseEfp(const u8* data, size_t size, EfpFile& out);

bool IsEfp(const u8* data, size_t size);

} // namespace sro
