// ============================================================================
//  SRO Asset Studio - fx/EffectScene.h
//
//  Turns a parsed .efp into drawable geometry for one instant in time.
//
//  ==========================================================================
//   EVERY NUMBER HERE COMES OUT OF THE FILE
//  ==========================================================================
//  The effect preview this replaces invented all of them - 90 particles, a
//  golden-angle spread, a sin(pi*age) fade - and so drew the same white blob
//  for every effect in the game. This layer only arranges data the parser
//  already proved correct against the reference implementation's own output
//  for battle/deco_charge_light_a.efp:
//
//    emitter   scale     opacity   blend      texture
//    0.0.0     56.7961   172/255   5 -> 2     textures\ouras_hue.ddj
//    0.0.1     25.4369   243/255   5 -> 2     textures\circle_double.ddj
//    0.0.2     56.7961   184/255   5 -> 2     textures\eu_multi_shine.ddj
//    0.0.3      8.0000   246/255   5 -> 2     textures\eu_multi_shine.ddj
//
//  Those four numbers are the acceptance test. If a change here stops them
//  reaching the rasteriser unaltered, the change is wrong.
//
//  Two details are easy to get backwards and both are visible:
//    * The scale/diffuse curves are indexed by LIFE FRAME on a 20 fps clock,
//      not by a normalised 0..1 time - the parser already resampled them to
//      exactly lifeFrames entries for that reason.
//    * A ViewBillboard plate takes the CAMERA basis; using the emitter's own
//      orientation leaves it edge-on and invisible from most angles.
// ============================================================================
#pragma once

#include "../Common.h"
#include "../formats/Bms.h"
#include "../formats/Efp.h"
#include "../render/Raster.h"
#include "FxEmit.h"

#include <functional>
#include <vector>

namespace sro {
namespace fx {

/// One node of the effect tree that actually draws.
struct Emitter {
    std::wstring   name;
    EfpLifeMode    lifeMode   = EfpLifeMode::Never;
    EfpRenderMode  renderMode = EfpRenderMode::Plate;
    EfpViewMode    viewMode   = EfpViewMode::None;
    EfpRenderState render;

    /// Life in 20 fps frames, and the millisecond duration that follows from it.
    u32 lifeFrames = 0;
    f32 durationMs = 1000.0f;
    /// When this emitter switches on and off. Both come from the object's own
    /// command window plus the offsets accumulated down the tree - which is
    /// what makes a charge/flash/burst/smoke sequence play in order rather than
    /// all at once on frame 0. endMs <= startMs means "never switches off".
    f32 startMs    = 0.0f;
    f32 endMs      = 0.0f;

    int particleCount = 1;
    int textureIndex  = -1;
    /// Index into the Effect's mesh table, -1 when this emitter draws a plate
    /// or its .bms could not be loaded.
    int meshIndex     = -1;

    /// The emitter's own scalar size, used when no scale curve exists. Without
    /// it a curve-less emitter was skipped outright and whole layers of an
    /// effect went missing.
    f32 scale = 1.0f;

    /// Opacity is the diffuse curve's alpha AT THE CURRENT LIFE, shaped by
    /// these three. Holding the curve's peak instead - which this port used to
    /// do - makes every sprite pop in at full brightness and pop out again, so
    /// glows, shockwaves and smoke render as hard constant blobs.
    f32 opacityScale = 1.0f;
    f32 opacityMax   = 1.0f;
    f32 opacityFloor = 0.0f;
    /// The initial material colour. It is REPLACED by the diffuse curve when
    /// one exists, never multiplied with it - multiplying costs every such
    /// emitter 20% red and 15% green, which reads as the whole effect being
    /// cold and dark.
    Vec3 color{ 0.8f, 0.85f, 1.0f };
    bool hasDiffuseCurve = false;
    /// Texture-stage gain: D3DTOP_MODULATE (4) is 1x, MODULATE2X (5) is 2x and
    /// MODULATE4X (6) is 4x. The reference clamps the result only from BELOW,
    /// so a 2x stage genuinely blows out - that brightness is authored, not a
    /// bug to be clamped away.
    f32 stageGain = 1.0f;
    /// SELECTARG1 on the final stage: the texel alone reaches the frame, so the
    /// emitter's colour must NOT tint it.
    bool selectTextureOnly = false;
    /// SELECTARG2: the diffuse colour alone - the texture is not sampled at all
    /// and the emitter draws a flat colour.
    bool selectDiffuseOnly = false;

    // --- authored motion, in 20 fps frame units ----------------------------
    Vec3 velocity{ 0, 0, 0 };
    Vec3 force{ 0, 0, 0 };
    Vec3 origin{ 0, 0, 0 };
    Vec3 spawnSphere{ 0, 0, 0 };
    bool hasSpawnSphere = false;
    f32  attraction = 0.0f;
    /// (min, max, angle) triples - each particle draws its OWN heading from
    /// these, which is what turns a single line of sprites into a fan.
    Vec3 spawnCone{ 0, 0, 0 };
    bool hasSpawnCone = false;
    Vec3 coneVelocity{ 0, 0, 0 };
    bool hasConeVelocity = false;
    Vec3 coneForce{ 0, 0, 0 };
    bool hasConeForce = false;

    /// Base orientation and per-frame step. For a billboard plate these
    /// collapse to a spin in the plate's own plane.
    Mat4 shapeRotation    = Mat4::Identity();
    Mat4 shapeRotationVel = Mat4::Identity();
    bool hasShapeRotation = false;
    bool hasShapeRotationVel = false;

    /// The texture sheet steps and HOLDS on its last cell instead of wrapping.
    bool texFramesStepped = false;

    /// From the authored back-face type: 2 front-only, 3 back-only, otherwise
    /// both. A billboard plate is always both, whatever the file says.
    bool twoSided = true;
    /// Only an opaque render-mesh (DESTBLEND = ZERO) writes depth; every other
    /// effect surface is transparent and must not occlude what is behind it.
    bool depthWrite = false;

    /// Per-particle size jitter, drawn from the exported BlendScaleGraph. Its
    /// absence made every sprite in a burst exactly the same size.
    std::vector<EfpScaleKey> randomScale;
    bool hasRandomScale = false;

    /// Where this emitter sits in source order - it seeds the per-particle
    /// random stream, so two emitters sharing a spawn volume no longer produce
    /// the identical point set and line up into visible pairs.
    int sourceIndex = 0;

    /// Already resampled to exactly `lifeFrames` entries by the parser.
    /// The swept path. A pipe kind is a ribbon only when this has > 1 point.
    std::vector<Vec3>          banPositions;
    /// True for LinkPipe / LinkDPipe / LinkObj - which the reference reports as
    /// render mode "plate", so the parse comparison cannot see the difference.
    bool                       isPipe = false;
    bool                       isDoublePipe = false;
    /// LinkObj is NOT a ribbon: it is a chain of objects placed along the path.
    bool                       isLinkObj = false;

    std::vector<EfpTexFrame>   texFrames;
    std::vector<EfpScaleKey>   scaleKeys;
    std::vector<EfpDiffuseKey> diffuseKeys;
    /// Authored key times, one per frame. When they are present and monotonic
    /// the curve is positioned BY THEM; otherwise the keys are spaced evenly.
    std::vector<f32>           scaleTimes;
    std::vector<f32>           diffuseTimes;

    /// How this emitter behaves, which decides both its population and how its
    /// particles are born.
    FxBehavior behavior = FxBehavior::Static;
    LifeFacts  life;
};

/// The whole effect, flattened and ready to draw.
class Effect {
public:
    /// `resolveTexture` maps an authored path to an index in Scene::textures.
    /// `resolveTexture` maps an authored .ddj path to an index in
    /// Scene::textures; `loadMesh` reads an authored .bms and returns false when
    /// it is absent - in which case that emitter draws NOTHING, which is the
    /// reference's rule, not a plate substitute.
    void build(const EfpFile& file,
               const std::function<int(const std::wstring&)>& resolveTexture,
               const std::function<bool(const std::wstring&, BmsFile&)>& loadMesh = nullptr);

    bool   empty()        const { return m_emitters.empty(); }
    size_t emitterCount() const { return m_emitters.size(); }
    f32    durationMs()   const { return m_durationMs; }
    const std::vector<Emitter>& emitters() const { return m_emitters; }

    /// Widest reach of any emitter - the authored maximum, which a single big
    /// quad can dominate.
    f32 radius() const;

    /// The extent the effect ACTUALLY covers, measured by emitting over its
    /// whole timeline and taking the 90th percentile of particle reach. Framing
    /// on radius() alone pushes the camera far enough back that a dense effect
    /// whose particles are mostly small ends up a speck in the middle of the
    /// frame - the "looks a thousand metres away" complaint.
    f32 contentRadius() const;

    /// How far back the camera must sit to hold the whole effect in frame.
    ///
    /// The framing decision lives HERE rather than at each call site, because
    /// it depends on the field of view and every caller was using the same
    /// hand-tuned multiplier without accounting for it. At 45 degrees the
    /// visible half-height at distance d is only d*tan(22.5) = 0.414*d, so a
    /// 2.6x multiplier leaves barely any margin - and none at all for content
    /// in the corners.
    f32 cameraDistance(f32 fovY = 0.7853982f) const;

    /// Rebuild `scene.meshes` for the effect at `nowMs`. Textures are the
    /// caller's; bounds are never touched, so playback cannot make the camera
    /// breathe.
    void emitScene(f32 nowMs, const Camera& camera, Scene& scene) const;

private:
    std::vector<Emitter> m_emitters;
    std::vector<BmsFile> m_meshes;
    f32 m_durationMs = 1000.0f;
};

} // namespace fx
} // namespace sro
