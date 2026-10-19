// ============================================================================
//  SRO Asset Studio - fx/FxEmit.h
//
//  How many particles an emitter has, and when each one is alive.
//
//  Ported from the reference's staticEmitSchedule / particleEmitCount /
//  particleBehaviorClassify / particleLifetime. This is the single biggest
//  reason effects looked wrong: the port used round(spawnRate) as a
//  POPULATION, but spawnRate is a RATE. A fountain authored as forty live
//  sprites drew one, so nearly every dense effect rendered as a lonely quad.
//
//  The population comes from the emit schedule:
//      live  = ceil(rate * duration / interval)
//      total = trunc(min(amount, events * rate))
//      count = trunc(min(total, max(1, live)))
//  and is then capped by how the emitter behaves - a trail is a single object,
//  a spray tops out at 32, a volume at 64, a loop at 128.
//
//  Birth is a rolling ladder, not one even wave. The port aged each particle
//  once and went dark; the reference recomputes the event index against the
//  CURRENT time, so a continuous emitter keeps streaming for the whole
//  timeline.
// ============================================================================
#pragma once

#include "../Common.h"
#include "FxSampler.h"

#include <algorithm>
#include <cmath>

namespace sro {
namespace fx {

// ----------------------------------------------------------------------------
//  Emit schedule
// ----------------------------------------------------------------------------

/// Authored counts are in 20 fps frames; this is the only conversion.
inline f32 StaticEmitFrameMs(f32 v) { return std::max(0.0f, v) * kEffectFrameMs; }

struct EmitSchedule {
    bool present    = false;    ///< any of start/end/interval authored
    f32  startMs    = 0.0f;
    f32  endMs      = 0.0f;
    bool hasEnd     = false;
    f32  intervalMs = 0.0f;
    bool hasInterval = false;
    f32  amount     = 1.0f;     ///< minParticles
    f32  rate       = 1.0f;     ///< spawnRate
    f32  durationMs = 1000.0f;
};

struct NormalizedSchedule {
    f32 duration = 1.0f, start = 0.0f, interval = 1.0f;
    i32 amount = 1;
    f32 rate = 1.0f;
    i32 events = 0;
};

inline NormalizedSchedule NormalizeSchedule(const EmitSchedule& s) {
    NormalizedSchedule n;
    const f32 fallbackDur = std::max(s.startMs, s.hasEnd ? s.endMs : 1.0f);
    n.duration = std::max(1.0f, std::isfinite(s.durationMs) && s.durationMs > 0.0f ? s.durationMs : fallbackDur);
    n.start    = std::max(0.0f, std::min(s.startMs, n.duration));
    const f32 end = std::max(n.start, std::min(s.hasEnd ? s.endMs : n.duration, n.duration));
    const f32 window = std::max(0.0f, end - n.start);
    n.interval = std::max(1.0f, s.hasInterval && std::isfinite(s.intervalMs)
                                ? s.intervalMs : (window > 0.0f ? window : n.duration));
    n.amount = static_cast<i32>(std::max(0.0f, std::floor(std::isfinite(s.amount) ? s.amount : 1.0f)));
    n.rate   = std::max(0.0f, std::isfinite(s.rate) ? s.rate : 1.0f);
    // The 1e-6 slack stops an exactly-divisible window from claiming one extra
    // event through floating-point drift.
    n.events = window > 0.0f
             ? std::max(0, static_cast<i32>(std::ceil(window / n.interval - 0.000001f)))
             : 0;
    return n;
}

inline i32 StaticEmitTotalCount(const NormalizedSchedule& n) {
    if (n.amount <= 0 || n.rate <= 0.0f || n.events <= 0) return 0;
    return static_cast<i32>(std::trunc(std::min(static_cast<f32>(n.amount),
                                                static_cast<f32>(n.events) * n.rate)));
}

inline i32 StaticEmitLiveCount(const EmitSchedule& s) {
    const NormalizedSchedule n = NormalizeSchedule(s);
    const i32 total = StaticEmitTotalCount(n);
    if (total <= 0) return 0;
    const f32 live = std::ceil(n.rate * n.duration / n.interval);
    return static_cast<i32>(std::trunc(std::min(static_cast<f32>(total), std::max(1.0f, live))));
}

/// Which emit EVENT produced particle `index`, or -1 when the schedule never
/// gets that far. With rate > 1 the events fire in bursts; with rate < 1 the
/// fractional accumulator decides which event each particle belongs to.
inline i32 StaticEmitEventIndex(const NormalizedSchedule& n, i32 targetIndex) {
    const i32 spawned = StaticEmitTotalCount(n);
    if (targetIndex >= n.amount || targetIndex >= spawned) return -1;
    if (n.rate > 1.0f) {
        const i32 ev = static_cast<i32>(std::floor(targetIndex / std::ceil(n.rate)));
        return ev < n.events ? ev : -1;
    }
    f32 accumulator = 0.0f;
    i32 emitted = 0;
    for (i32 ev = 0; ev < n.events; ++ev) {
        const f32 next = std::min(static_cast<f32>(n.amount), accumulator + n.rate);
        const i32 count = static_cast<i32>(std::trunc(next)) - static_cast<i32>(std::trunc(accumulator));
        if (targetIndex < emitted + count) return ev;
        emitted += count;
        accumulator = next;
        if (emitted >= n.amount) break;
    }
    return -1;
}

/// Birth time of particle `index`, or +inf when it is never emitted.
inline f32 StaticEmitBirthMs(const EmitSchedule& s, i32 index) {
    const NormalizedSchedule n = NormalizeSchedule(s);
    if (index < 0 || n.amount <= 0 || n.rate <= 0.0f) return 1e30f;
    const i32 ev = StaticEmitEventIndex(n, index);
    if (ev < 0) return 1e30f;
    return n.start + static_cast<f32>(ev) * n.interval;
}

// ----------------------------------------------------------------------------
//  Behaviour
// ----------------------------------------------------------------------------

enum class FxBehavior { LinkedShape, Mesh, Static, Spray, Trail, Volume };

/// Everything the count/lifetime rules need, pulled off an emitter so this
/// header stays independent of the scene types.
struct EmitFacts {
    EfpLifeMode   lifeMode   = EfpLifeMode::Never;
    EfpRenderMode renderMode = EfpRenderMode::Plate;
    EfpViewMode   viewMode   = EfpViewMode::None;
    bool isTrail = false;          ///< LinkPipe / LinkDPipe
    bool isLinkObj = false;        ///< LinkObj - a chain of objects, not a ribbon
    i32  particleCount = 1;        ///< authored pool
    i32  minParticles  = 1;
    f32  burstRate = 1.0f;
    f32  spawnRate = 1.0f;
    bool hasSpawnVolume = false;
    Vec3 velocity{ 0, 0, 0 }, force{ 0, 0, 0 };
    Vec3 coneVelocity{ 0, 0, 0 }, coneForce{ 0, 0, 0 };
    f32  attraction = 0.0f;
    bool hasShapeRotationVelocity = false;
    size_t scaleFrameCount = 0, diffuseFrameCount = 0, texFrameCount = 0, randomScaleFrameCount = 0;
    size_t banPositionCount = 0;
    f32  banPositionSpeed = 0.0f;
    EmitSchedule schedule;
};

inline f32 Magnitude(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
inline bool IsExtinct(EfpLifeMode m) { return m == EfpLifeMode::Extinct; }

inline i32 ClampCount(f32 v, i32 lo, i32 hi) {
    return std::max(lo, std::min(hi, static_cast<i32>(std::ceil(v))));
}

inline i32 AuthoredPoolCount(const EmitFacts& e) {
    return std::max(1, std::max(e.particleCount, e.minParticles));
}

inline bool HasAuthoredMotion(const EmitFacts& e) {
    return Magnitude(e.velocity) > 0.01f || Magnitude(e.coneVelocity) > 0.01f
        || Magnitude(e.force) > 0.01f    || Magnitude(e.coneForce) > 0.01f
        || std::fabs(e.attraction) > 0.0001f;
}

inline bool HasMeshFrameAnimation(const EmitFacts& e) {
    return e.scaleFrameCount > 1 || e.diffuseFrameCount > 1
        || e.texFrameCount > 1 || e.randomScaleFrameCount > 1;
}

inline bool UsesStaticEmitPool(const EmitFacts& e) {
    if (e.hasSpawnVolume) return true;
    if (Magnitude(e.coneVelocity) > 0.01f || Magnitude(e.coneForce) > 0.01f) return true;
    if (e.renderMode != EfpRenderMode::Mesh) return HasAuthoredMotion(e);
    if (HasAuthoredMotion(e)) return true;
    if (e.hasShapeRotationVelocity) return AuthoredPoolCount(e) > 1;
    return HasMeshFrameAnimation(e) && AuthoredPoolCount(e) > 1;
}

inline i32 ActiveStaticEmitCount(const EmitFacts& e) {
    if (e.schedule.present) {
        const i32 n = StaticEmitLiveCount(e.schedule);
        return std::max(1, n);
    }
    const i32 pool = AuthoredPoolCount(e);
    if (UsesStaticEmitPool(e)) return pool;
    if (e.spawnRate > 0.0f)
        return std::min(pool, std::max(1, static_cast<i32>(std::ceil(e.spawnRate))));
    const i32 minimum = std::max(1, e.minParticles);
    const i32 burst   = std::max(1, static_cast<i32>(e.burstRate));
    return std::min(pool, std::min(minimum, burst));
}

inline i32 MeshPoolLimit(const EmitFacts& e) {
    // A parked BAN path (speed 0, more than one point) means the mesh is being
    // placed ALONG the path, so only a handful of copies exist.
    if (e.banPositionSpeed == 0.0f && e.banPositionCount > 1) return 3;
    return e.viewMode == EfpViewMode::VBillboard ? 12 : 24;
}

inline bool IsStaticVolumePool(const EmitFacts& e, i32 activeCount) {
    return e.renderMode == EfpRenderMode::Plate
        && IsExtinct(e.lifeMode)
        && activeCount > 1
        && e.hasSpawnVolume
        && e.banPositionCount <= 1
        && Magnitude(e.coneForce) <= 0.01f
        && std::fabs(e.attraction) <= 0.0001f;
}

inline FxBehavior ClassifyEmitter(const EmitFacts& e) {
    // A LINKED kind with a real path is a TRAIL - LinkObj included. Only a
    // LinkObj with no usable path stays an object chain. Checking isLinkObj
    // first made every pathed LinkObj a linkedShape and gave it 20 particles
    // where the reference draws one.
    const bool anyLinked = e.isTrail || e.isLinkObj;
    if (anyLinked && e.banPositionCount > 1) return FxBehavior::Trail;
    if (e.isLinkObj) return FxBehavior::LinkedShape;
    if (e.isTrail)   return FxBehavior::Trail;
    if (e.renderMode == EfpRenderMode::Mesh) return FxBehavior::Mesh;
    const i32 activeCount = IsExtinct(e.lifeMode)
                          ? ActiveStaticEmitCount(e)
                          : std::max(1, std::max(e.particleCount, e.minParticles));
    if (IsStaticVolumePool(e, activeCount)) return FxBehavior::Volume;
    const bool hasMotion = HasAuthoredMotion(e);
    return (hasMotion || (activeCount > 1 && e.hasSpawnVolume))
         ? FxBehavior::Spray : FxBehavior::Static;
}

inline i32 NormalExtinctVisibleCount(const EmitFacts& e, FxBehavior b) {
    if (b == FxBehavior::Mesh && !UsesStaticEmitPool(e)) return 1;
    const i32 active = ActiveStaticEmitCount(e);
    switch (b) {
        case FxBehavior::LinkedShape: return ClampCount(static_cast<f32>(active), 1, 48);
        case FxBehavior::Volume:      return ClampCount(static_cast<f32>(active), 1, 64);
        case FxBehavior::Mesh:        return ClampCount(static_cast<f32>(active), 1, MeshPoolLimit(e));
        case FxBehavior::Spray:       return ClampCount(static_cast<f32>(active), 1, 32);
        default:                      return ClampCount(static_cast<f32>(active), 1, 24);
    }
}

inline i32 PreviewParticleCount(const EmitFacts& e, FxBehavior b) {
    if (e.renderMode == EfpRenderMode::None) return 0;
    if (b == FxBehavior::Trail) return 1;
    if (IsExtinct(e.lifeMode)) return NormalExtinctVisibleCount(e, b);
    const i32 authored = AuthoredPoolCount(e);
    const i32 visible  = e.minParticles > 0 ? e.minParticles : authored;
    if (b == FxBehavior::Mesh)   return ClampCount(static_cast<f32>(visible), 1, MeshPoolLimit(e));
    if (b == FxBehavior::Static) return ClampCount(static_cast<f32>(authored), 1, 90);
    return ClampCount(static_cast<f32>(visible), 1, 128);
}

// ----------------------------------------------------------------------------
//  Lifetime
// ----------------------------------------------------------------------------

struct LifeSample { f32 life = 0.0f; i32 sequence = 0; bool visible = false; };

/// Everything the birth ladder needs.
struct LifeFacts {
    f32  durationMs = 1000.0f;
    EfpLifeMode lifeMode = EfpLifeMode::Never;
    f32  phase = 0.0f;
    i32  count = 1;
    EfpRenderMode renderMode = EfpRenderMode::Plate;
    bool useParticleBan = false;
    f32  banPositionSpeed = 0.0f;
    f32  banPositionStartMs = 0.0f, banPositionEndMs = 0.0f;
    bool hasBanWindow = false;
    f32  pathTrailWindow = 0.16f;
    bool pathFlare = false;
    i32  minParticles = 1;
    f32  burstRate = 1.0f, spawnRate = 1.0f;
    EmitSchedule schedule;
    bool steadyState = false;
};

inline f32 ClampF(f32 v, f32 lo, f32 hi) { return std::max(lo, std::min(hi, v)); }

inline bool StaticBanExtinct(const LifeFacts& r) {
    return r.useParticleBan && r.banPositionSpeed == 0.0f && r.count > 1;
}
inline bool StaticMeshExtinct(const LifeFacts& r) {
    return r.renderMode == EfpRenderMode::Mesh && r.count > 1 && !r.useParticleBan;
}

inline f32 EvenBirth(i32 index, f32 duration, i32 count) {
    return count > 1 ? static_cast<f32>(index) * duration / static_cast<f32>(count) : 0.0f;
}

inline f32 StaticScheduledBirth(const LifeFacts& r, i32 index, f32 duration, i32 count) {
    if (!r.schedule.present) return EvenBirth(index, duration, count);
    EmitSchedule s = r.schedule;
    s.amount     = static_cast<f32>(r.minParticles > 0 ? r.minParticles : count);
    s.durationMs = duration;
    return StaticEmitBirthMs(s, index);
}

/// A path-parked emitter lives only as long as its trail window, not the whole
/// duration - that short life IS the trail.
inline f32 ExtinctParticleDuration(const LifeFacts& r, f32 duration) {
    if (!StaticBanExtinct(r)) return duration;
    const f32 window = r.pathTrailWindow > 0.0f ? r.pathTrailWindow : 0.16f;
    const f32 pathDuration = (r.hasBanWindow && r.banPositionEndMs > r.banPositionStartMs)
                           ? (r.banPositionEndMs - r.banPositionStartMs) : duration;
    return std::max(1.0f, pathDuration * ClampF(window, 0.06f, r.pathFlare ? 0.22f : 0.32f));
}

inline f32 StaticEmitBurstSize(const LifeFacts& r) {
    return r.spawnRate > 1.0f ? std::max(1.0f, std::ceil(r.spawnRate)) : 1.0f;
}

inline f32 StaticEmitInterval(const LifeFacts& r, f32 duration, i32 count) {
    if (r.schedule.hasInterval && std::isfinite(r.schedule.intervalMs) && r.schedule.intervalMs > 0.0f)
        return ClampF(r.schedule.intervalMs, 1.0f, duration);
    const f32 authored = std::max(1.0f, r.burstRate);
    const f32 previewFloor = duration / std::max(1.0f, static_cast<f32>(count) * 4.0f);
    return ClampF(authored, 1.0f, std::max(1.0f, previewFloor));
}

inline f32 StaticEmitStart(const LifeFacts& r, f32 duration) {
    return ClampF(r.schedule.startMs, 0.0f, duration);
}
inline f32 StaticEmitEnd(const LifeFacts& r, f32 duration, f32 start) {
    return ClampF(r.schedule.hasEnd ? r.schedule.endMs : duration, start, duration);
}

/// The rolling ladder: which event is particle `index` on RIGHT NOW. Deriving
/// it from the current time is what keeps a continuous emitter streaming
/// instead of emptying once and going dark.
inline bool StaticEmitParticle(const LifeFacts& r, i32 index, f32 now, f32 duration,
                               i32 count, i32& outEvent, i32& outSequence) {
    const f32 interval = StaticEmitInterval(r, duration, count);
    const f32 start = StaticEmitStart(r, duration);
    const f32 end   = StaticEmitEnd(r, duration, start);
    const f32 activeNow = ClampF(now, start, end);
    const i32 latest = static_cast<i32>(std::floor((activeNow - start) / interval));
    const i32 burst  = static_cast<i32>(StaticEmitBurstSize(r));
    const i32 event  = latest - static_cast<i32>(std::floor(static_cast<f32>(index) / static_cast<f32>(burst)));
    if (event < 0 && !r.steadyState) return false;
    outEvent = event;
    outSequence = event * burst + index % burst;
    return true;
}

inline LifeSample ExtinctLife(const LifeFacts& r, i32 index, f32 now) {
    const f32 duration = std::max(1.0f, r.durationMs);
    const i32 count = std::max(1, r.count);
    const f32 sampleNow = r.steadyState ? Modulo(now, duration) : now;

    f32 birth;
    i32 sequence = index;
    if (StaticMeshExtinct(r)) {
        birth = StaticScheduledBirth(r, index, duration, count);
    } else if (StaticBanExtinct(r)) {
        birth = StaticScheduledBirth(r, index, duration, count);
    } else if (count > 1) {
        i32 ev = 0, seq = 0;
        if (!StaticEmitParticle(r, index, sampleNow, duration, count, ev, seq))
            return LifeSample{ 0.0f, 0, false };
        birth = StaticEmitStart(r, duration) + static_cast<f32>(ev) * StaticEmitInterval(r, duration, count);
        sequence = seq;
    } else {
        birth = 0.0f;
    }

    const f32 lifeDuration = ExtinctParticleDuration(r, duration);
    if (sampleNow < birth || sampleNow > birth + lifeDuration || sampleNow > duration)
        return LifeSample{ 0.0f, 0, false };
    return LifeSample{ ClampF((sampleNow - birth) / lifeDuration, 0.0f, 1.0f), sequence, true };
}

inline LifeSample ParticleLifeSample(const LifeFacts& r, i32 index, f32 now) {
    const f32 duration = std::max(1.0f, r.durationMs);
    const i32 count = std::max(1, r.count);
    if (r.lifeMode == EfpLifeMode::Extinct) return ExtinctLife(r, index, now);
    const f32 offset = count > 1 ? static_cast<f32>(index) * duration / static_cast<f32>(count) : 0.0f;
    if (r.lifeMode == EfpLifeMode::Loop)
        return LifeSample{ Modulo(now + r.phase + offset, duration) / duration, index, true };
    return LifeSample{ ClampF((now + r.phase + offset) / duration, 0.0f, 1.0f), index, true };
}

} // namespace fx
} // namespace sro
