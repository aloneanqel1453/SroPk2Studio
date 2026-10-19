// ============================================================================
//  SRO Asset Studio - fx/EffectScene.cpp   (see EffectScene.h)
// ============================================================================
#include "EffectScene.h"

#include "FxSampler.h"
#include "FxTransform.h"
#include "FxWindow.h"

#include <algorithm>
#include <cmath>

namespace sro {
namespace fx {

namespace {

/// The client evaluates an effect on a fixed 20 fps clock: one frame is 50 ms,
/// and every authored life is a count of those frames.
constexpr f32 kFrameMs = 50.0f;

/// The quad basis for a plate. A ViewBillboard plate faces the camera; using
/// the emitter's own orientation instead leaves it edge-on from most angles,
/// which reads as the sprite simply not being there.
void PlateBasis(EfpViewMode mode, const Camera& camera, Vec3& right, Vec3& up) {
    const Mat4 v = Mat4::FromQuat(camera.orient);
    switch (mode) {
        case EfpViewMode::Billboard:
            // Row-vector convention (v*M): the camera's world-space axes are the
            // ROWS of the view rotation. Reading columns instead transposes the
            // basis and leaves every plate edge-on - the same transpose trap that
            // once inverted qFromBasis in this renderer.
            right = Vec3(v.m[0], v.m[1], v.m[2]);
            up    = Vec3(v.m[4], v.m[5], v.m[6]);
            return;
        case EfpViewMode::YBillboard:
        case EfpViewMode::VBillboard: {
            // Yaw only: the plate stays upright, which is what a beam or a
            // pillar needs. Flattening the forward vector is the whole trick.
            Vec3 fwd(v.m[8], v.m[9], v.m[10]);
            fwd.y = 0.0f;
            if (fwd.length() < 1e-4f) fwd = Vec3(0, 0, 1);
            fwd = fwd.normalized();
            right = Vec3(0, 1, 0).cross(fwd).normalized();
            up    = Vec3(0, 1, 0);
            return;
        }
        default:
            right = Vec3(1, 0, 0);
            up    = Vec3(0, 1, 0);
            return;
    }
}


/// The reference's random table is 5000 floats, and every one of them is
/// k/32767 from MSVC's rand() - so it regenerates from the LCG rather than
/// needing to be embedded.
f32 EffectRandom(u32 index) {
    u32 s = 3749729837u;
    for (u32 i = 0; i <= (index % 5000u) + 1u; ++i) s = s * 214013u + 2531011u;
    return static_cast<f32>((s >> 16) & 0x7FFFu) / 32767.0f;
}

/// One strip of a ribbon, swept along `pts` about `up`.
///
/// Vertex order per point is LEFT then RIGHT, and the two triangles of a
/// segment are (a, a+1, a+2) and (a+1, a+3, a+2) - sharing edge {a+1, a+2},
/// traversed in opposite directions so the winding stays consistent.
void AppendRibbonStrip(const std::vector<Vec3>& pts, const Vec3& up, f32 width,
                       bool linkedUv, RasterMesh& out) {
    if (pts.size() < 2) return;

    std::vector<f32> dist(pts.size(), 0.0f);
    for (size_t i = 1; i < pts.size(); ++i) {
        const Vec3 d = pts[i] - pts[i - 1];
        dist[i] = dist[i - 1] + d.length();
    }
    const f32 total = std::max(0.0001f, dist.back());
    const u32 base = static_cast<u32>(out.vertices.size());

    for (size_t i = 0; i < pts.size(); ++i) {
        // Central difference, one-sided at the ends.
        const Vec3 a = pts[i + 1 < pts.size() ? i + 1 : pts.size() - 1];
        const Vec3 b = pts[i > 0 ? i - 1 : 0];
        Vec3 tan = a - b;
        if (tan.length() < 1e-6f) tan = Vec3(1, 0, 0);
        tan = tan.normalized();

        Vec3 side = tan.cross(up);
        if (side.length() < 1e-6f) side = tan.cross(Vec3(1, 0, 0));
        if (side.length() < 1e-6f) side = Vec3(0, 0, 1);
        side = side.normalized();

        const f32 amount = dist[i] / total;
        // No width sampler here, so the reference's taper applies.
        const f32 taper = 0.18f + (amount * amount * (3.0f - 2.0f * amount)) * 0.82f;
        const f32 half = std::max(0.01f, width) * taper;

        RasterVertex L, R;
        L.position = pts[i] - side * half;
        R.position = pts[i] + side * half;
        L.normal = R.normal = up;
        // Linked pipes swap the UV components - the easiest thing to get
        // backwards, and it shows as a texture running across the ribbon
        // instead of along it.
        L.uv = linkedUv ? Vec2(amount, 0.0f) : Vec2(0.0f, amount);
        R.uv = linkedUv ? Vec2(amount, 1.0f) : Vec2(1.0f, amount);
        out.vertices.push_back(L);
        out.vertices.push_back(R);
    }

    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const u32 a = base + static_cast<u32>(i) * 2u;
        out.indices.push_back(a);     out.indices.push_back(a + 1); out.indices.push_back(a + 2);
        out.indices.push_back(a + 1); out.indices.push_back(a + 3); out.indices.push_back(a + 2);
    }
}

/// Where a particle sits at normalised life `t`.
///
/// Closed form, no integration across frames: the reference resets the particle
/// every update and derives everything from `t`, so any instant can be drawn
/// directly. Units are 20 fps FRAMES - authored velocity is units per frame and
/// force is units per frame per frame.
/// A cone draw: (min, max, angle) -> a vector tilted off +Y by up to `angle`
/// and spun to a random azimuth. Three draws per call, in this order.
Vec3 EffectCone(u32 seed, const Vec3& v) {
    const f32 mn = std::isfinite(v.x) ? v.x : 0.0f;
    const f32 mx = std::isfinite(v.y) ? v.y : mn;
    const f32 angle = std::max(0.0f, std::isfinite(v.z) ? v.z : 0.0f);
    const f32 magnitude = EffectRandom(seed)     * (mx - mn) + mn;
    const f32 tilt      = EffectRandom(seed + 1) * angle;
    const f32 azimuth   = EffectRandom(seed + 2) * 6.2831853f;
    const f32 x = -magnitude * std::sin(tilt);
    const f32 y =  magnitude * std::cos(tilt);
    return Vec3(x * std::cos(azimuth), y, x * std::sin(azimuth));
}

/// Rejection-sampled inside the unit sphere, then scaled per axis.
Vec3 EffectSphere(u32 seed, const Vec3& radius) {
    Vec3 s(0, 0, 0);
    u32 k = seed;
    for (int attempt = 0; attempt < 64; ++attempt) {
        s = Vec3(EffectRandom(k) * 2.0f - 1.0f,
                 EffectRandom(k + 1) * 2.0f - 1.0f,
                 EffectRandom(k + 2) * 2.0f - 1.0f);
        k += 3;
        if (s.x * s.x + s.y * s.y + s.z * s.z <= 1.0f) break;
    }
    return Vec3(s.x * radius.x, s.y * radius.y, s.z * radius.z);
}

Vec3 ParticlePosition(const Emitter& e, int index, int sequence, f32 life) {
    Vec3 p = e.origin;

    // The random stream is keyed by the particle's SEQUENCE and the emitter's
    // own index. Without the emitter term two emitters sharing a spawn volume
    // drew the identical point set and lined up into visible pairs.
    const u32 seed = static_cast<u32>((sequence >= 0 ? sequence : index)
                                      + e.sourceIndex * 4099) * 8u;

    if (e.hasSpawnCone)        p = p + EffectCone(seed, e.spawnCone);
    else if (e.hasSpawnSphere) p = p + EffectSphere(seed, e.spawnSphere);

    const f32 frames = e.lifeFrames > 0 ? static_cast<f32>(e.lifeFrames)
                                        : std::max(1.0f, e.durationMs / kFrameMs);
    const f32 ageFrames = std::max(0.0f, life) * frames;   // never floored

    // A cone velocity replaces the shared direction with a per-particle one.
    Vec3 dir(0, 0, 1);                     // the reference's fallback direction
    bool moved = false;
    if (e.hasConeVelocity) {
        const Vec3 cv = EffectCone(seed + 4, e.coneVelocity);
        if (cv.x * cv.x + cv.y * cv.y + cv.z * cv.z > 1e-8f) {
            dir = cv.normalized();
            p = p + cv * ageFrames;
            moved = true;
        }
    }
    if (!moved) {
        const f32 vLenSq = e.velocity.x * e.velocity.x + e.velocity.y * e.velocity.y +
                           e.velocity.z * e.velocity.z;
        if (vLenSq > 0.0001f) {
            dir = e.velocity.normalized();
            p = p + dir * (std::sqrt(vLenSq) * ageFrames);
        }
    }

    // Force: the DISCRETE arithmetic series over whole frames, not 0.5*a*t^2.
    Vec3 force = e.force;
    if (e.hasConeForce) force = force + e.coneForce;
    const f32 fLenSq = force.x * force.x + force.y * force.y + force.z * force.z;
    if (fLenSq > 0.0001f) {
        const f32 mag = std::sqrt(fLenSq);
        p = p + force.normalized() * (0.5f * mag * ageFrames * (ageFrames + 1.0f));
    }

    // Attraction pulls the particle onto its own velocity axis as it ages. It
    // works in the emitter's LOCAL frame - measuring from the world origin
    // instead drags an offset emitter's particles off-centre.
    if (std::fabs(e.attraction) > 1e-6f) {
        f32 pull = std::fabs(e.attraction) * life * life * 0.018f;
        if (pull < 0.0f) pull = 0.0f;
        if (pull > 0.85f) pull = 0.85f;
        const Vec3 local = p - e.origin;
        const f32 d = local.x * dir.x + local.y * dir.y + local.z * dir.z;
        const Vec3 axis = dir * d;
        p = Vec3(e.origin.x + local.x + (axis.x - local.x) * pull,
                 e.origin.y + local.y + (axis.y - local.y) * pull,
                 e.origin.z + local.z + (axis.z - local.z) * pull);
    }
    return p;
}

/// The in-plane spin of a billboard plate at `ageFrames`.
///
/// The step matrix is raised to the power of the whole frame age and composed
/// with the base, then the transformed X axis gives the angle. Row-vector
/// convention means "base first, then step" is base * step.
f32 PlateSpinAngle(const Emitter& e, f32 ageFrames) {
    if (!e.hasShapeRotation && !e.hasShapeRotationVel) return 0.0f;
    Mat4 m = e.hasShapeRotation ? e.shapeRotation : Mat4::Identity();
    if (e.hasShapeRotationVel && ageFrames > 0.0f) {
        i32 n = static_cast<i32>(std::floor(ageFrames));
        // Binary exponentiation - a per-frame step over a long life would
        // otherwise be hundreds of multiplies per particle per frame.
        Mat4 acc = Mat4::Identity();
        Mat4 base = e.shapeRotationVel;
        while (n > 0) {
            if (n & 1) acc = acc * base;
            base = base * base;
            n >>= 1;
        }
        m = m * acc;
    }
    const f32 angle = std::atan2(m.m[1], m.m[0]);
    return std::isfinite(angle) ? angle : 0.0f;
}

} // namespace

// ---------------------------------------------------------------------------
void Effect::build(const EfpFile& file,
                   const std::function<int(const std::wstring&)>& resolveTexture,
                   const std::function<bool(const std::wstring&, BmsFile&)>& loadMesh) {
    m_emitters.clear();
    m_meshes.clear();
    m_durationMs = 0.0f;

    // Two things accumulate DOWN the tree and both are visible when missed:
    // the child's start offset (so stages fire in order, not all at once) and
    // the parent's position (so a nested effect does not collapse onto the
    // origin). rootScale sizes the whole thing.
    const f32 rootScale = (file.hasRootScale && std::isfinite(file.rootScale) && file.rootScale > 0.0f)
                        ? file.rootScale : 1.0f;

    std::function<void(const EfpNode&, const FxFrame&, bool)> walk =
        [&](const EfpNode& n, const FxFrame& parent, bool isRoot) {
        // The node's own frame: position, rotation and path, all resolved
        // against the parent's exactly as the reference resolves them.
        const FxFrame frame = BuildFrame(n, parent, rootScale, isRoot);
        const f32 cefStartMs = parent.cefStartMs;
        // Only a RenderNone object is a pure container. An emitter with no
        // authored life is NOT skipped: the reference falls back to a duration
        // derived from its command window, so dropping it here simply lost
        // whole layers of an effect.
        const bool draws = (n.renderMode != EfpRenderMode::None);
        if (draws) {
            Emitter e;
            e.name        = n.name;
            e.lifeMode    = n.lifeMode;
            e.renderMode  = (n.renderMode == EfpRenderMode::Mesh) ? EfpRenderMode::Mesh
                                                                  : EfpRenderMode::Plate;
            e.viewMode    = n.viewMode;
            e.render      = n.render;
            e.lifeFrames  = n.lifeFrames;
            // With no authored life the window decides, floored at 120 ms, and
            // an empty window means one second.
            e.durationMs  = n.lifeFrames > 0
                          ? static_cast<f32>(n.lifeFrames) * kFrameMs
                          : (n.endSec > n.startSec
                             ? std::max(120.0f, (n.endSec - n.startSec) * 1000.0f)
                             : 1000.0f);
            {   // The StaticEmit command's own range wins over the object's.
                const FxWindow w = EmitterWindow(n, cefStartMs);
                e.startMs = w.startMs;
                e.endMs   = w.endMs;
            }
            e.texFrames   = n.texFrames;
            // Inherited when this node authors none - a child of a moving
            // parent walks the parent's path.
            e.banPositions = frame.banPositions;
            {   // The kind is only visible in the authored Shape NAME: the
                // reported render mode collapses every pipe to "plate".
                std::wstring shape = ToLowerW(TrimW(n.renderSource));
                if (shape.rfind(L"render", 0) == 0) shape = shape.substr(6);
                e.isDoublePipe = (shape == L"linkdpipe");
                e.isLinkObj    = (shape == L"linkobj");
                e.isPipe       = e.isDoublePipe || shape == L"linkpipe";
            }
            e.scaleKeys   = n.scaleKeys;
            e.diffuseKeys = n.diffuseKeys;
            for (const EfpScaleKey& k : e.scaleKeys)     e.scaleTimes.push_back(k.time);
            for (const EfpDiffuseKey& k : e.diffuseKeys) e.diffuseTimes.push_back(k.time);
            // Motion is stated in the emitter's own frame: without the
            // rotation the magnitudes are right but every direction is wrong.
            e.velocity    = TransformMotion(n.velocitySourceType, frame, parent, n.velocity);
            e.force       = TransformMotion(n.forceSourceType,    frame, parent, n.force);
            // The emitter sits at its own offset RELATIVE TO ITS PARENT, and
            // the whole effect is sized by the file's root scale. Ignoring
            // either collapsed nested effects onto the origin and rendered
            // v0012/v0013 files at the wrong size.
            e.origin      = frame.position;
            e.spawnSphere = n.spawnSphere;
            e.hasSpawnSphere = n.hasSpawnSphere;
            e.attraction  = n.attraction;
            e.spawnCone   = n.spawnCone;
            e.hasSpawnCone = n.hasSpawnCone;
            e.coneVelocity = n.coneVelocity;
            e.hasConeVelocity = n.hasConeVelocity;
            e.coneForce   = n.coneForce;
            e.hasConeForce = Magnitude(n.coneForce) > 0.0001f;
            e.texFramesStepped = n.texFramesStepped;
            e.sourceIndex = static_cast<int>(m_emitters.size());
            {   // A billboard plate always faces the viewer, so it is drawn from
                // both sides regardless; anything else honours the file.
                const bool billboardPlate = (e.renderMode == EfpRenderMode::Plate)
                    && (n.viewMode == EfpViewMode::Billboard ||
                        n.viewMode == EfpViewMode::YBillboard ||
                        n.viewMode == EfpViewMode::VBillboard);
                const u32 bf = n.render.present ? n.render.backFaceType : 0u;
                e.twoSided = billboardPlate || (bf != 2u && bf != 3u);
                // D3DBLEND_ZERO as the destination factor is the one opaque case.
                e.depthWrite = (e.renderMode == EfpRenderMode::Mesh)
                            && n.render.present && n.render.dstBlend == 1u;
                // Pointer 0 selects the exported BlendScaleGraph as the jitter
                // source; anything else means the emitter has no jitter.
                if (n.hasRandomScale && !n.blendScale.empty()) {
                    e.randomScale = n.blendScale;
                    e.hasRandomScale = true;
                }
            }
            // Raw floats straight into the row-vector matrix: v*M then reads
            // row 0 as the transformed X axis, which is exactly what the
            // reference's column-vector M*(1,0,0) produces.
            if (n.hasShapeRotation) {
                for (int k = 0; k < 16; ++k) e.shapeRotation.m[k] = n.shapeRotation[k];
                e.shapeRotation.m[3] = e.shapeRotation.m[7] = e.shapeRotation.m[11] = 0.0f;
                e.shapeRotation.m[12] = e.shapeRotation.m[13] = e.shapeRotation.m[14] = 0.0f;
                e.shapeRotation.m[15] = 1.0f;
                e.hasShapeRotation = true;
            }
            if (n.hasShapeRotationVel) {
                for (int k = 0; k < 16; ++k) e.shapeRotationVel.m[k] = n.shapeRotationVel[k];
                e.shapeRotationVel.m[3] = e.shapeRotationVel.m[7] = e.shapeRotationVel.m[11] = 0.0f;
                e.shapeRotationVel.m[12] = e.shapeRotationVel.m[13] = e.shapeRotationVel.m[14] = 0.0f;
                e.shapeRotationVel.m[15] = 1.0f;
                e.hasShapeRotationVel = true;
            }
            // Fallback size when the emitter has no scale curve at all.
            e.scale = AuthoredScale(n, rootScale);

            // The population. spawnRate is a RATE, not a count: the live count
            // comes out of the emit schedule and is then capped by how the
            // emitter behaves. Using round(spawnRate) drew ONE sprite where the
            // file asks for dozens, which is why dense effects looked empty.
            EmitFacts facts;
            facts.lifeMode   = n.lifeMode;
            facts.renderMode = e.renderMode;
            facts.viewMode   = n.viewMode;
            facts.isTrail    = e.isPipe;
            facts.isLinkObj  = e.isLinkObj;
            facts.minParticles = n.emit.present ? static_cast<i32>(n.emit.minParticles) : 1;
            facts.particleCount = std::max(1, facts.minParticles);
            facts.burstRate  = n.emit.present ? static_cast<f32>(n.emit.burstRate) : 1.0f;
            facts.spawnRate  = n.emit.present ? n.emit.spawnRate : 1.0f;
            facts.hasSpawnVolume = n.hasSpawnSphere || n.hasSpawnCone;
            facts.velocity   = n.velocity;
            facts.force      = n.force;
            facts.coneVelocity = n.coneVelocity;
            facts.coneForce  = n.coneForce;
            facts.attraction = n.attraction;
            facts.hasShapeRotationVelocity = n.hasShapeRotationVel;
            facts.scaleFrameCount   = n.scaleKeys.size();
            facts.diffuseFrameCount = n.diffuseKeys.size();
            facts.texFrameCount     = n.texFrames.size();
            facts.banPositionCount  = frame.banPositions.size();
            facts.banPositionSpeed  = frame.banSpeed;
            if (n.emit.present) {
                facts.schedule.present    = true;
                facts.schedule.startMs    = StaticEmitFrameMs(static_cast<f32>(n.emit.minVal));
                facts.schedule.endMs      = StaticEmitFrameMs(static_cast<f32>(n.emit.minVal + n.emit.maxVal));
                facts.schedule.hasEnd     = true;
                facts.schedule.intervalMs = StaticEmitFrameMs(static_cast<f32>(n.emit.burstRate));
                facts.schedule.hasInterval = true;
                facts.schedule.amount     = static_cast<f32>(n.emit.minParticles);
                facts.schedule.rate       = n.emit.spawnRate;
            }
            facts.schedule.durationMs = e.durationMs;

            e.behavior = ClassifyEmitter(facts);
            e.particleCount = std::max(1, PreviewParticleCount(facts, e.behavior));

            // What the birth ladder needs, so every frame can ask "is particle
            // i alive right now" instead of aging it once and going dark.
            e.life.durationMs     = e.durationMs;
            e.life.lifeMode       = n.lifeMode;
            e.life.count          = e.particleCount;
            e.life.renderMode     = e.renderMode;
            e.life.useParticleBan = frame.banPositions.size() > 1;
            e.life.banPositionSpeed = frame.banSpeed;
            e.life.minParticles   = facts.minParticles;
            e.life.burstRate      = facts.burstRate;
            e.life.spawnRate      = facts.spawnRate;
            e.life.schedule       = facts.schedule;
            // The reference's viewer sets steadyState from its LOOP toggle,
            // for every emitter, and looping is on by default. Deriving it
            // from the life mode instead made extinct emitters empty once and
            // go dark rather than keep streaming.
            e.life.steadyState    = true;
            // Emitters are decorrelated so they do not all pulse in lockstep.
            e.life.phase = static_cast<f32>(m_emitters.size() * 193u % 1000u);

            // A mesh emitter needs its .bms. If it cannot be loaded the emitter
            // draws nothing at all - the reference is explicit that a missing
            // mesh is NOT replaced by a plate.
            if (n.renderMode == EfpRenderMode::Mesh && loadMesh) {
                for (const std::wstring& mp : n.render.meshPaths) {
                    if (mp.empty()) continue;
                    BmsFile bms;
                    if (!loadMesh(mp, bms) || bms.vertices.empty() || bms.faces.empty()) continue;
                    e.meshIndex = static_cast<int>(m_meshes.size());
                    m_meshes.push_back(std::move(bms));
                    break;
                }
            }

            if (resolveTexture) {
                if (!n.render.texturePaths.empty())
                    e.textureIndex = resolveTexture(n.render.texturePaths.front());
                else if (!n.resourcePaths.empty())
                    e.textureIndex = resolveTexture(n.resourcePaths.front());
            }
            // --- the D3D texture-stage combiner -------------------------------
            //
            // The reference chains two stages: stage 0 computes from
            // (current = diffuse, diffuse, texel), then stage 1 takes stage 0's
            // result as its `current`.
            //
            // It is tempting to compound the two gains, and that would be
            // WRONG here. Stage 1's arguments in this corpus are always
            // arg1 = 2 (TEXTURE) and arg2 = 0 (DIFFUSE) - measured: 34504 and
            // 34507 of 34507 drawing emitters - so stage 1 never reads
            // `current` and stage 0's result is discarded entirely. srcOp
            // therefore has NO effect, and multiplying its gain in would have
            // doubled the brightness of 871 files for no reason.
            //
            // So the gain is stage 1's alone. The three non-MODULATE ops that
            // do occur are handled explicitly rather than silently treated as
            // 1x.
            e.stageGain = 1.0f;
            switch (n.render.dstOp) {
                case 5: e.stageGain = 2.0f; break;              // MODULATE2X
                case 6: e.stageGain = 4.0f; break;              // MODULATE4X
                case 2: e.selectTextureOnly = true;  break;     // SELECTARG1 -> texel
                case 3: e.selectDiffuseOnly = true;  break;     // SELECTARG2 -> diffuse
                default: break;                                 // MODULATE, and DISABLE
            }

            // Opacity is shaped, not frozen. A trail keeps a floor so it does
            // not vanish between samples; everything else is free to reach zero.
            e.hasDiffuseCurve = !e.diffuseKeys.empty();
            if (e.behavior == FxBehavior::Trail) {
                e.opacityScale = 1.08f; e.opacityMax = 1.0f; e.opacityFloor = 0.08f;
            } else {
                e.opacityScale = 1.0f;  e.opacityMax = 1.0f; e.opacityFloor = 0.0f;
            }
            // Once a diffuse curve exists it REPLACES the constant colour.
            if (e.hasDiffuseCurve) e.color = Vec3(1.0f, 1.0f, 1.0f);
            // ...and SELECTARG1 removes the tint entirely.
            if (e.selectTextureOnly) e.color = Vec3(1.0f, 1.0f, 1.0f);
            // A diffuse COMMAND with no frames still caps the emitter at 0.12
            // - nearly transparent, not fully opaque.
            if (!e.hasDiffuseCurve && n.diffuseFromCommand) e.opacityMax = 0.12f;

            m_emitters.push_back(std::move(e));
        }
        for (const EfpNode& c : n.children) walk(c, frame, false);
    };
    walk(file.root, FxFrame(), true);

    for (const Emitter& e : m_emitters)
        m_durationMs = std::max(m_durationMs, e.startMs + e.durationMs);
    // The reference reports a whole second for this fixture even though its
    // longest emitter runs 750 ms, so the system duration is rounded up to the
    // next whole second rather than clipped to the last emitter.
    if (m_durationMs <= 0.0f) m_durationMs = 1000.0f;
    else m_durationMs = std::ceil(m_durationMs / 1000.0f) * 1000.0f;
}

// ---------------------------------------------------------------------------
f32 Effect::radius() const {
    f32 r = 1.0f;
    for (const Emitter& e : m_emitters)
        for (const EfpScaleKey& k : e.scaleKeys)
            r = std::max(r, std::max(k.v.x, std::max(k.v.y, k.v.z)));
    return r * 0.5f;      // scale is the quad's full width
}

// ---------------------------------------------------------------------------
f32 Effect::cameraDistance(f32 fovY) const {
    const f32 r = std::max(0.01f, contentRadius());
    const f32 halfPerUnit = std::tan(std::max(0.05f, fovY) * 0.5f);
    // 1.35 is the margin: the radius is a sphere, the frame is a rectangle, and
    // content near a corner needs more room than content on the vertical axis.
    return r * 1.35f / std::max(0.05f, halfPerUnit);
}

// ---------------------------------------------------------------------------
f32 Effect::contentRadius() const {
    // MEASURED, not estimated: walk the timeline, emit for real, and take the
    // reach of the geometry that actually gets drawn.
    //
    // Estimating this from the scale curves alone - as this used to - ignores
    // the fact that particles MOVE. An emitter authored with small sprites and
    // a strong velocity travels far outside its own scale, so the camera framed
    // the spawn point and the effect flew off screen. That is the "certain
    // effects are impossible to view completely" complaint.
    //
    // The percentile is taken over FRAMES, not over vertices. Per frame the
    // FULL reach is used, so nothing in that frame is ever cut; across frames a
    // high percentile then ignores the occasional instant where one sprite
    // flies far out. Percentiling over vertices instead does the opposite - it
    // trims a little off every frame and pushes the camera back for the sake of
    // outliers, which left effects complete but tiny.
    std::vector<f32> reach;
    Camera probe;
    probe.target      = Vec3(0, 0, 0);
    probe.sceneRadius = 1.0f;
    probe.distance    = 4.0f;

    Scene scene;
    constexpr int kProbeFrames = 16;
    for (int i = 0; i < kProbeFrames; ++i) {
        const f32 at = m_durationMs * static_cast<f32>(i) / static_cast<f32>(kProbeFrames);
        emitScene(at, probe, scene);
        f32 frameMax = 0.0f;
        for (const RasterMesh& m : scene.meshes)
            for (const RasterVertex& v : m.vertices)
                frameMax = std::max(frameMax, v.position.length());
        if (frameMax > 0.0f) reach.push_back(frameMax);
    }

    if (reach.empty()) {
        // Nothing drew at any sampled instant - fall back to the authored sizes
        // and the emitter offsets so the camera is at least in the right place.
        for (const Emitter& e : m_emitters) {
            const f32 off = e.origin.length();
            if (e.scaleKeys.empty()) { reach.push_back(off + e.scale * 0.5f); continue; }
            for (const EfpScaleKey& k : e.scaleKeys)
                reach.push_back(off + std::max(k.v.x, k.v.y) * 0.5f);
        }
        if (reach.empty()) return radius();
    }

    std::sort(reach.begin(), reach.end());
    // 85th percentile of the per-frame extents: the effect fits whole for the
    // large majority of its life, and a single outlying instant does not decide
    // the shot for all the others.
    const size_t idx = static_cast<size_t>(static_cast<f32>(reach.size() - 1) * 0.85f);
    const f32 p = reach[idx];
    return p > 0.01f ? p : radius();
}

// ---------------------------------------------------------------------------
void Effect::emitScene(f32 nowMs, const Camera& camera, Scene& scene) const {
    scene.meshes.clear();
    if (m_emitters.empty()) return;

    // Source order IS draw order: the rasteriser walks scene.meshes in
    // sequence, which reproduces the authored compositing without a sort.
    for (const Emitter& e : m_emitters) {
        // A steady-state emitter's only condition is that it has started.
        if (nowMs < e.startMs) continue;

        Vec3 right, up;
        PlateBasis(e.viewMode, camera, right, up);

        for (int i = 0; i < e.particleCount; ++i) {
            const f32 elapsed = nowMs - e.startMs;
            // The rolling birth ladder decides whether this particle exists at
            // all right now - not a single even wave that empties once.
            const LifeSample ls = ParticleLifeSample(e.life, i, elapsed);
            if (!ls.visible) continue;
            const f32 life = ls.life;

            // Interpolated, and positioned by the authored key times when they
            // are usable. Nearest-key sampling made size and colour stair-step.
            Vec3 sv = SampleScaleFrame(e.scaleKeys, e.scale, life, e.scaleTimes);
            if (e.hasRandomScale) {
                // Each particle reads the jitter curve at its OWN fixed phase,
                // so a burst has a spread of sizes instead of clones.
                const f32 phase = EffectRandom(static_cast<u32>(i) * 8u + 7u);
                const Vec3 j = SampleScaleFrame(e.randomScale, 1.0f, phase, std::vector<f32>());
                sv = Vec3(sv.x * j.x, sv.y * j.y, sv.z * j.z);
            }
            const EfpScaleKey sk{ 0.0f, sv };

            Vec3 rgb(1, 1, 1);
            f32  curveAlpha = 1.0f;
            // SELECTARG1 puts the TEXEL alone on screen - the diffuse colour
            // never reaches the frame, so neither the emitter tint nor the
            // curve colour may be applied.
            if (e.hasDiffuseCurve && !e.selectTextureOnly) {
                const DiffuseSample d = SampleDiffuseFrame(e.diffuseKeys, life, e.diffuseTimes);
                rgb = Vec3(d.r, d.g, d.b);
                curveAlpha = d.a;
            }
            // Alpha follows the curve AT THIS INSTANT, so sprites fade in and
            // out instead of popping.
            f32 alpha = curveAlpha * e.opacityScale;
            if (alpha < e.opacityFloor) alpha = e.opacityFloor;
            if (alpha > e.opacityMax)   alpha = e.opacityMax;
            if (alpha <= 0.0f) continue;

            // Where this particle actually is - authored spawn volume, velocity,
            // force and attraction, all closed-form in its own life.
            const Vec3 c = ParticlePosition(e, i, ls.sequence, life);

            // A mesh emitter instances its .bms. A missing .bms draws NOTHING -
            // the reference's rule, never a plate substitute.
            if (e.renderMode == EfpRenderMode::Mesh && !e.isPipe) {
                if (e.meshIndex < 0) continue;
                const BmsFile& bms = m_meshes[static_cast<size_t>(e.meshIndex)];
                RasterMesh mm;
                mm.textureIndex = e.textureIndex;
            // SELECTARG2 drops the texture; SELECTARG1 drops the tint.
            if (e.selectDiffuseOnly) mm.textureIndex = -1;
                // A mesh honours its authored back-face type: 2 front-only,
                // 3 back-only, anything else both. Forcing two-sided doubled
                // the additive brightness of cones, tubes and domes and made
                // them read solid instead of hollow.
                mm.twoSided  = e.twoSided;
                mm.alphaTest = 0.0f;
                mm.opacity   = alpha * e.stageGain;
                mm.tint      = Vec3(rgb.x * e.color.x * e.stageGain,
                                    rgb.y * e.color.y * e.stageGain,
                                    rgb.z * e.color.z * e.stageGain);
                mm.srcBlend  = e.render.present ? e.render.srcBlend : 5u;
                mm.dstBlend  = e.render.present ? e.render.dstBlend : 2u;
                mm.unlit      = true;
                // DESTBLEND = ZERO means the destination contributes nothing:
                // the surface is opaque. Routing it to the opaque pass is what
                // lets it write depth and occlude the particles behind it -
                // depth write is a property of the pass, not of the mesh.
                if (e.depthWrite) { mm.srcBlend = 0u; mm.dstBlend = 0u; }
                mm.transform  = Mat4::Identity();

                mm.vertices.resize(bms.vertices.size());
                for (size_t v = 0; v < bms.vertices.size(); ++v) {
                    const BmsVertex& src = bms.vertices[v];
                    RasterVertex& dst = mm.vertices[v];
                    dst.position = Vec3(c.x + src.position.x * sk.v.x,
                                        c.y + src.position.y * sk.v.y,
                                        c.z + src.position.z * sk.v.z);
                    dst.normal = src.normal;
                    dst.uv     = src.uv0;
                }
                mm.indices.reserve(bms.faces.size() * 3);
                for (const BmsFace& fc : bms.faces) {
                    mm.indices.push_back(fc.a);
                    mm.indices.push_back(fc.b);
                    mm.indices.push_back(fc.c);
                }
                scene.meshes.push_back(std::move(mm));
                continue;
            }

            RasterMesh m;
            m.textureIndex = e.textureIndex;
            // SELECTARG2 drops the texture; SELECTARG1 drops the tint.
            if (e.selectDiffuseOnly) m.textureIndex = -1;
            m.twoSided     = e.twoSided;
            m.alphaTest    = 0.0f;          // soft sprites are never cut out
            m.opacity      = alpha * e.stageGain;
            m.tint         = Vec3(rgb.x * e.color.x * e.stageGain,
                                  rgb.y * e.color.y * e.stageGain,
                                  rgb.z * e.color.z * e.stageGain);
            // The authored D3D states, straight through. For this fixture they
            // are 5 -> 2, i.e. SRCALPHA -> ONE.
            m.srcBlend     = e.render.present ? e.render.srcBlend : 5u;
            m.dstBlend     = e.render.present ? e.render.dstBlend : 2u;
            m.unlit        = true;          // MeshBasicMaterial: never lit, never fogged
            m.blendAlpha   = false;
            m.additive     = false;
            m.transform    = Mat4::Identity();

            // A pipe kind with a real path is a ribbon, not a plate.
            if (e.isPipe && e.banPositions.size() > 1) {
                RasterMesh rm;
                rm.textureIndex = e.textureIndex;
            // SELECTARG2 drops the texture; SELECTARG1 drops the tint.
            if (e.selectDiffuseOnly) rm.textureIndex = -1;
                rm.twoSided  = true;
                rm.alphaTest = 0.0f;
                rm.opacity   = alpha * e.stageGain;
                rm.tint      = Vec3(rgb.x * e.color.x * e.stageGain,
                                    rgb.y * e.color.y * e.stageGain,
                                    rgb.z * e.color.z * e.stageGain);
                rm.srcBlend  = e.render.present ? e.render.srcBlend : 5u;
                rm.dstBlend  = e.render.present ? e.render.dstBlend : 2u;
                rm.unlit     = true;
                rm.transform = Mat4::Identity();

                std::vector<Vec3> path;
                path.reserve(e.banPositions.size());
                for (const Vec3& q : e.banPositions)
                    path.push_back(Vec3(c.x + q.x * sk.v.x, c.y + q.y * sk.v.y, c.z + q.z * sk.v.z));

                AppendRibbonStrip(path, Vec3(0, 1, 0), sk.v.x * 0.5f, true, rm);
                if (e.isDoublePipe) {
                    // The crossed blade: a second strip about a different axis,
                    // at 55% width, and never camera-facing.
                    AppendRibbonStrip(path, Vec3(0, 0, 1), sk.v.x * 0.5f * 0.55f, true, rm);
                }
                if (!rm.vertices.empty()) scene.meshes.push_back(std::move(rm));
                continue;
            }

            // A shape rotation spins the plate in its OWN plane - the whole
            // reason rune circles and vortices are not dead still.
            Vec3 pr = right, pu = up;
            {
                const f32 frames = e.lifeFrames > 0 ? static_cast<f32>(e.lifeFrames)
                                                    : std::max(1.0f, e.durationMs / kFrameMs);
                const f32 spin = PlateSpinAngle(e, std::max(0.0f, life) * frames);
                if (std::fabs(spin) > 1e-6f) {
                    const f32 cs = std::cos(spin), sn = std::sin(spin);
                    pr = right * cs + up * sn;
                    pu = up * cs - right * sn;
                }
            }
            const Vec3 rx = pr * (sk.v.x * 0.5f);
            const Vec3 uy = pu * (sk.v.y * 0.5f);
            const Vec3 nrm = pr.cross(pu).normalized();
            // Vec3 has no unary minus in this codebase; build the corners from
            // an explicit origin instead of negating.
            const Vec3 pos[4] = { c - rx - uy, c + rx - uy,
                                  c + rx + uy, c - rx + uy };
            // Texture frames run on the WALL clock in milliseconds. Comparing a
            // 0..1 life against millisecond key times either froze the sheet on
            // its last cell or scrolled it at a wildly wrong rate.
            Vec2 uvOff(0.0f, 0.0f), uvRep(1.0f, 1.0f);
            if (!e.texFrames.empty()) {
                // A grid-generated sheet STEPS and holds on its last cell; an
                // authored list is sampled on the clock and may wrap.
                const TexSample ts = e.texFramesStepped
                    ? SampleSteppedTextureFrame(e.texFrames, e.lifeFrames, life)
                    : SampleTextureFrameMs(e.texFrames, e.durationMs, life * e.durationMs);
                if (ts.present) { uvOff = ts.offset; uvRep = ts.repeat; }
            }
            const f32  uvs[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };

            m.vertices.resize(4);
            for (int v = 0; v < 4; ++v) {
                m.vertices[static_cast<size_t>(v)].position = pos[v];
                m.vertices[static_cast<size_t>(v)].normal   = nrm;
                m.vertices[static_cast<size_t>(v)].uv =
                    Vec2(uvs[v][0] * uvRep.x + uvOff.x, uvs[v][1] * uvRep.y + uvOff.y);
            }
            m.indices = { 0, 1, 2, 0, 2, 3 };
            scene.meshes.push_back(std::move(m));
        }
    }
}

} // namespace fx
} // namespace sro
