// ============================================================================
//  SRO Asset Studio - fx/FxSampler.h
//
//  Curve sampling, ported from the reference's particleFrameSampler.
//
//  The port previously took the nearest lower key and returned it unchanged.
//  That is visibly wrong in two ways at once: size and colour JUMP between
//  keys instead of easing, and curves whose keys sit at non-uniform authored
//  times peak at the wrong moment. Both are obvious on the 2-4 key curves that
//  most effects use.
//
//  Two blending rules, and which applies depends on the key times:
//    * authored times present and monotonic -> LINEAR between the bracketing
//      keys, positioned by those times;
//    * no usable times -> uniform spacing with a SMOOTHSTEP blend, and the
//      index WRAPS.
//  Diffuse additionally quantises through 8-bit (`lerpByte`), which is not
//  cosmetic: it is what keeps a colour ramp stepping the same way it does in
//  the reference.
// ============================================================================
#pragma once

#include "../Common.h"
#include "../formats/Efp.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sro {
namespace fx {

/// One frame is 50 ms - every authored life is a count of these.
constexpr f32 kEffectFrameMs = 50.0f;

inline f32 SmoothStep01(f32 v) { return v * v * (3.0f - 2.0f * v); }
inline f32 Lerp1(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

/// Colour interpolation goes through 8-bit, exactly as the reference does.
inline f32 LerpByte(f32 a, f32 b, f32 t) {
    return std::trunc(Lerp1(a * 255.0f, b * 255.0f, t)) / 255.0f;
}

inline f32 Modulo(f32 v, f32 d) {
    if (d <= 0.0f) return 0.0f;
    f32 r = std::fmod(v, d);
    if (r < 0.0f) r += d;
    return r;
}

struct FrameSample {
    size_t index = 0;
    size_t next  = 0;
    f32    amount = 0.0f;
};

/// Uniform spacing, smoothstep blend, WRAPPING index.
inline FrameSample UniformFrameSample(size_t count, f32 life) {
    FrameSample s;
    if (count == 0) return s;
    const f32 wrapped = life - std::floor(life);
    const f32 position = wrapped * static_cast<f32>(count);
    const size_t i = static_cast<size_t>(std::floor(position)) % count;
    s.index  = i;
    s.next   = (i + 1) % count;
    s.amount = SmoothStep01(position - std::floor(position));
    return s;
}

/// Key times count only when there is one per frame and they never decrease -
/// a partial or unsorted list falls back to uniform spacing rather than
/// producing a plausible-looking wrong answer.
inline bool ValidFrameTimes(const std::vector<f32>& times, size_t count) {
    if (times.size() != count || count == 0) return false;
    f32 prev = -1e30f;
    for (f32 t : times) {
        if (!std::isfinite(t) || t < prev) return false;
        prev = t;
    }
    return true;
}

inline FrameSample FrameSampleAt(size_t count, f32 life, const std::vector<f32>& times) {
    if (!ValidFrameTimes(times, count)) return UniformFrameSample(count, life);
    FrameSample s;
    const f32 first = times.front();
    const f32 last  = times[count - 1];
    if (life <= first) { s.index = 0; s.next = 0; s.amount = 0.0f; return s; }
    if (life >= last)  { s.index = count - 1; s.next = count - 1; s.amount = 0.0f; return s; }
    for (size_t i = 1; i < count; ++i) {
        const f32 right = times[i];
        if (life > right) continue;
        const f32 left = times[i - 1];
        s.index  = i - 1;
        s.next   = i;
        s.amount = right <= left ? 0.0f : (life - left) / (right - left);
        return s;
    }
    s.index = count - 1; s.next = count - 1; s.amount = 0.0f;
    return s;
}

/// Negative or non-finite scales are clamped to 0, not passed through - a
/// single bad key would otherwise mirror or explode the quad.
inline Vec3 NormalizeScaleFrame(const Vec3& v) {
    auto n = [](f32 x) { return std::isfinite(x) ? std::max(0.0f, x) : 1.0f; };
    return Vec3(n(v.x), n(v.y), n(v.z));
}

/// Size at `life`. With no curve at all the emitter's scalar scale applies -
/// the port used to skip such emitters entirely, which simply lost them.
inline Vec3 SampleScaleFrame(const std::vector<EfpScaleKey>& frames, f32 fallback,
                             f32 life, const std::vector<f32>& times) {
    if (frames.empty()) return Vec3(fallback, fallback, fallback);
    if (frames.size() == 1) return NormalizeScaleFrame(frames[0].v);
    const FrameSample s = FrameSampleAt(frames.size(), life, times);
    const Vec3 l = NormalizeScaleFrame(frames[s.index].v);
    const Vec3 r = NormalizeScaleFrame(frames[s.next].v);
    return Vec3(Lerp1(l.x, r.x, s.amount), Lerp1(l.y, r.y, s.amount), Lerp1(l.z, r.z, s.amount));
}

inline f32 AverageScaleFrame(const std::vector<EfpScaleKey>& frames, f32 fallback,
                             f32 life, const std::vector<f32>& times) {
    const Vec3 f = SampleScaleFrame(frames, fallback, life, times);
    return std::max(0.08f, (f.x + f.y + f.z) / 3.0f);
}

struct DiffuseSample { f32 r = 1, g = 1, b = 1, a = 1; bool present = false; };

inline DiffuseSample SampleDiffuseFrame(const std::vector<EfpDiffuseKey>& frames,
                                        f32 life, const std::vector<f32>& times) {
    DiffuseSample d;
    if (frames.empty()) return d;
    d.present = true;
    if (frames.size() == 1) {
        d.r = frames[0].r; d.g = frames[0].g; d.b = frames[0].b; d.a = frames[0].a;
        return d;
    }
    const FrameSample s = FrameSampleAt(frames.size(), life, times);
    const EfpDiffuseKey& l = frames[s.index];
    const EfpDiffuseKey& r = frames[s.next];
    d.r = LerpByte(l.r, r.r, s.amount);
    d.g = LerpByte(l.g, r.g, s.amount);
    d.b = LerpByte(l.b, r.b, s.amount);
    d.a = LerpByte(l.a, r.a, s.amount);
    return d;
}

// ----------------------------------------------------------------------------
//  Texture frames
// ----------------------------------------------------------------------------

/// All-or-nothing: one frame that does not look like an atlas cell and the
/// whole list is treated as a scroll instead.
inline bool HasAtlasTextureFrame(const EfpTexFrame& f) {
    for (int k = 0; k < 4; ++k) if (!std::isfinite(f.f[k])) return false;
    if (std::fabs(f.f[0]) > 1024.0f || std::fabs(f.f[1]) > 1024.0f) return false;
    if (!(f.f[2] > 0.0001f && f.f[3] > 0.0001f)) return false;
    return f.f[2] <= 1.001f && f.f[3] <= 1.001f;   // 1.001 is a slop tolerance
}

inline bool HasAtlasTextureFrames(const std::vector<EfpTexFrame>& fr) {
    if (fr.empty()) return false;
    for (const EfpTexFrame& f : fr) if (!HasAtlasTextureFrame(f)) return false;
    return true;
}

struct TexSample { Vec2 offset{ 0, 0 }; Vec2 repeat{ 1, 1 }; bool present = false; };

inline TexSample TextureSampleOf(const EfpTexFrame& f) {
    TexSample s;
    s.present = true;
    if (HasAtlasTextureFrame(f)) {
        // The V term subtracts the HEIGHT and nests two moduli: it converts a
        // top-left-origin row into a bottom-left-origin UV offset.
        s.offset = Vec2(Modulo(f.f[0], 1.0f), Modulo(1.0f - Modulo(f.f[1], 1.0f) - f.f[3], 1.0f));
        s.repeat = Vec2(f.f[2], f.f[3]);
    } else {
        s.offset = Vec2(f.f[1], f.f[2]);           // f[0] is a TIME here
        s.repeat = Vec2(std::max(0.0001f, f.f[3]), 1.0f);
    }
    return s;
}

/// Key times are in MILLISECONDS. A raw value under 600 is read as seconds -
/// the same heuristic the reference uses, and the reason the port's old
/// life-fraction comparison scrolled at a wildly wrong rate.
inline f32 TextureFrameTimeMs(const EfpTexFrame& f, f32 durationMs) {
    const f32 raw = f.f[0];
    if (!std::isfinite(raw) || raw < 0.0f) return 0.0f;
    const f32 ms = raw <= 600.0f ? raw * 1000.0f : raw;
    return std::max(0.0f, std::min(ms, std::max(1.0f, durationMs)));
}

inline bool HasMonotonicTextureTimes(const std::vector<EfpTexFrame>& fr, f32 durationMs) {
    f32 prev = -1e30f;
    for (const EfpTexFrame& f : fr) {
        const f32 t = TextureFrameTimeMs(f, durationMs);
        if (t < prev) return false;
        prev = t;
    }
    return true;
}

/// Sampled on the WALL clock in milliseconds, not on a life fraction.
inline TexSample SampleTextureFrameMs(const std::vector<EfpTexFrame>& fr,
                                      f32 durationMs, f32 elapsedMs) {
    TexSample s;
    if (fr.empty()) return s;
    if (fr.size() == 1) return TextureSampleOf(fr[0]);

    const f32 duration = std::max(1.0f, durationMs);
    if (HasAtlasTextureFrames(fr))
        return TextureSampleOf(fr[UniformFrameSample(fr.size(), elapsedMs / duration).index]);
    if (!HasMonotonicTextureTimes(fr, duration))
        return TextureSampleOf(fr[UniformFrameSample(fr.size(), elapsedMs / duration).index]);

    const f32 firstT = TextureFrameTimeMs(fr.front(), duration);
    const f32 lastT  = TextureFrameTimeMs(fr.back(),  duration);
    const f32 span   = std::max(1.0f, lastT - firstT);
    const f32 time   = firstT + Modulo(elapsedMs - firstT, span);
    for (size_t i = 1; i < fr.size(); ++i) {
        const f32 rt = TextureFrameTimeMs(fr[i], duration);
        if (time > rt) continue;
        const f32 lt = TextureFrameTimeMs(fr[i - 1], duration);
        const f32 amount = rt <= lt ? 0.0f : SmoothStep01((time - lt) / (rt - lt));
        s.present = true;
        s.offset = Vec2(Lerp1(fr[i - 1].f[1], fr[i].f[1], amount),
                        Lerp1(fr[i - 1].f[2], fr[i].f[2], amount));
        s.repeat = Vec2(std::max(0.0001f, Lerp1(fr[i - 1].f[3], fr[i].f[3], amount)), 1.0f);
        return s;
    }
    return TextureSampleOf(fr.back());
}

/// The atlas-grid schedule: a discrete cell that HOLDS on the last frame
/// rather than wrapping.
inline TexSample SampleSteppedTextureFrame(const std::vector<EfpTexFrame>& fr,
                                           u32 lifeFrames, f32 life) {
    TexSample s;
    if (fr.empty()) return s;
    const f32 maxAge = static_cast<f32>(std::max<u32>(1, lifeFrames ? lifeFrames
                                                      : static_cast<u32>(fr.size())));
    const f32 clamped = std::max(0.0f, std::min(1.0f, life));
    size_t idx = static_cast<size_t>(std::floor(clamped * maxAge));
    if (idx >= fr.size()) idx = fr.size() - 1;
    return TextureSampleOf(fr[idx]);
}

} // namespace fx
} // namespace sro
