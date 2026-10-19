// ============================================================================
//  SRO Asset Studio - formats/Efp.cpp
//
//  JMXVEFF particle effect.  Wiki: JMXVEFF.md
//
//  This format is string-driven: the size of almost every block depends on a
//  command name that was just read. That makes it the one format here where a
//  wrong guess silently desynchronises the stream instead of failing loudly.
//
//  The rule this parser follows (from Efp.h):
//    on an UNRECOGNISED controller / command / parameter name, stop, mark the
//    node partial, and unwind. NEVER invent a size.
//
//  When the structured walk stops early we still owe the user something, so a
//  second pass scavenges length-prefixed strings that look like asset paths
//  (.ddj / .bms / .ban) out of the remaining bytes. Those are attributed to the
//  root node and the summary says plainly that they came from a scan.
//
//  Verified against real files: see the harness output in the agent report.
// ============================================================================
#include "Efp.h"
#include "../core/I18n.h"   // UI strings follow the chosen language

namespace sro {

namespace {

constexpr UINT kPathCodePage = 1252;

/// Silkroad command/resource strings are short. Anything longer is a
/// desynchronised stream, not a real string.
constexpr u32 kMaxEfpString = 512;

/// Effect trees are shallow; this only exists so a corrupt child count cannot
/// blow the C++ stack.
constexpr int kMaxDepth = 32;

/// Sanity ceiling for any count field inside the effect tree.
constexpr u32 kMaxCount = 65536;

// ---------------------------------------------------------------------------
// A reader that also latches "I did not understand this", separately from
// "I ran out of bytes". Both stop the walk, but they mean different things in
// the report.
// ---------------------------------------------------------------------------
struct EfpReader {
    ByteReader r;
    bool       unknownTag = false;
    std::wstring unknownName;
    /// The node currently being filled. Readers that used to skip now write
    /// through this; null is always tolerated so nothing depends on it.
    EfpNode*   node = nullptr;

    /// The source record currently being read. The frame-count formula needs
    /// its control byte and its start/end/span triple, so these can no longer
    /// be skipped over.
    u8  srcByte  = 0;
    u8  srcType  = 0;
    f32 srcStart = 0.0f, srcEnd = 0.0f, srcSpan = 0.0f;
    /// True only while a command read through EESourceData is being handled.
    /// EEGlobalData shares the same parameter readers but carries NO time
    /// triple, and these fields are never cleared between commands - so a
    /// globally-declared command would otherwise inherit the PREVIOUS
    /// command's window and switch its emitter off at the wrong moment.
    bool inSourceCommand = false;

    explicit EfpReader(const u8* d, size_t n) : r(d, n) {}

    bool bad() const { return r.failed() || unknownTag; }

    void markUnknown(const std::wstring& what) {
        if (!unknownTag) { unknownTag = true; unknownName = what; }
    }

    /// u32 length prefix + raw chars, with a hard length cap.
    std::string str() {
        const u32 len = r.readU32();
        if (r.failed()) return std::string();
        if (len > kMaxEfpString || !r.canRead(len)) { r.fail(); return std::string(); }
        const u8* p = r.readPtr(len);
        if (!p) return std::string();
        return std::string(reinterpret_cast<const char*>(p), len);
    }
    std::wstring wstr() { return MultiByteToWide(str(), kPathCodePage); }

    /// Count field with a ceiling and a "does it fit" check.
    bool count(u32& out, size_t unitBytes) {
        out = r.readU32();
        if (r.failed()) return false;
        if (out > kMaxCount) { r.fail(); return false; }
        if (unitBytes && out > r.remaining() / unitBytes) { r.fail(); return false; }
        return true;
    }
};

// ---------------------------------------------------------------------------
// EEParameter - size depends on the command / parameter name.
// Table: JMXVEFF.md "ParameterByCommandName".
// Returns false (and marks unknown) for a name we do not model.
/// EFStaticEmit: four u32 then the spawn rate. 20 bytes.
/// The authored render/view mode names, mapped onto the enums. The reference
/// stores these as length-prefixed CP949 STRINGS - there is no numeric enum in
/// the file - so the match is on the trimmed lowercase name, and both the bare
/// and the "Render"/"View" prefixed spellings are accepted.
/// The render mode a node REPORTS is not its Shape name alone - the reference
/// reconciles the name against the node's first usable resource. Verified by
/// running its own parser over 2086 effects / 25860 objects: this reproduces
/// its emitter modes with zero mismatches.
///
/// The asymmetry is deliberate and load-bearing:
///   * a MESH with no mesh path but a texture is DOWNGRADED to plate,
///   * a plate is never upgraded to mesh,
///   * a none is never touched, even when the node carries a texture.
/// In this corpus the downgrade fires on all 28 RenderLinkObj nodes - which is
/// the whole plate/mesh disagreement. Do NOT hard-code RenderLinkObj to plate:
/// the rule is resource-driven, and a client shipping a real .bms for one would
/// legitimately render it as a mesh.
EfpRenderMode DisplayMode(EfpRenderMode named,
                          const std::vector<std::wstring>& meshPaths,
                          const std::vector<std::wstring>& texPaths) {
    // "First usable resource": the first entry with EITHER a mesh or a texture.
    // An empty string counts as absent, exactly as JS truthiness does.
    std::wstring mesh, tex;
    const size_t n = (std::max)(meshPaths.size(), texPaths.size());
    for (size_t i = 0; i < n; ++i) {
        const std::wstring m = i < meshPaths.size() ? meshPaths[i] : std::wstring();
        const std::wstring t = i < texPaths.size()  ? texPaths[i]  : std::wstring();
        if (!m.empty() || !t.empty()) { mesh = m; tex = t; break; }
    }

    if (named == EfpRenderMode::Mesh && mesh.empty() && !tex.empty())
        return EfpRenderMode::Plate;
    if (named != EfpRenderMode::Unknown) return named;
    if (!mesh.empty()) return EfpRenderMode::Mesh;
    if (!tex.empty())  return EfpRenderMode::Plate;
    return named;
}

EfpRenderMode RenderModeFromName(const std::wstring& raw) {
    std::wstring n = ToLowerW(TrimW(raw));
    if (n.rfind(L"render", 0) == 0) n = n.substr(6);
    if (n == L"none")     return EfpRenderMode::None;
    if (n == L"plate" || n == L"linkpipe" || n == L"linkdpipe") return EfpRenderMode::Plate;
    if (n == L"mesh"  || n == L"linkobj")                       return EfpRenderMode::Mesh;
    return raw.empty() ? EfpRenderMode::None : EfpRenderMode::Unknown;
}

EfpViewMode ViewModeFromName(const std::wstring& raw) {
    std::wstring n = ToLowerW(TrimW(raw));
    if (n.rfind(L"view", 0) == 0) n = n.substr(4);
    if (n == L"billboard")  return EfpViewMode::Billboard;
    if (n == L"ybillboard") return EfpViewMode::YBillboard;
    if (n == L"vbillboard") return EfpViewMode::VBillboard;
    return EfpViewMode::None;
}

/// NormalTimeLife maps to EXTINCT, not to a looping life - an easy and very
/// visible thing to get backwards.
EfpLifeMode LifeModeFromName(const std::wstring& raw) {
    const std::wstring n = ToLowerW(TrimW(raw));
    if (n == L"normaltimelife" || n == L"normaltimeextinct") return EfpLifeMode::Extinct;
    if (n == L"normaltimelooplife" || n == L"normaltimeloop") return EfpLifeMode::Loop;
    return EfpLifeMode::Never;
}

/// The reference's command frame-count formula (its effectCommandFrames.ts).
/// `fc` is EEGlobalData Int0 - the particle life in 20 fps frames.
///
/// Returns 0 when the count does not resolve, and only then does the on-disk
/// list length survive. Note how many branches read `trunc` rather than round:
/// JS truncation toward zero, not nearest.
/// Piecewise-linear sample of a scale curve, clamped at both ends.
Vec3 SampleScaleAt(const std::vector<EfpScaleKey>& k, f32 t) {
    if (k.empty()) return Vec3(1.0f, 1.0f, 1.0f);
    if (k.size() == 1 || t <= k.front().time) return k.front().v;
    if (t >= k.back().time) return k.back().v;
    for (size_t i = 1; i < k.size(); ++i) {
        if (t <= k[i].time) {
            const f32 span = k[i].time - k[i - 1].time;
            const f32 u = span > 1e-6f ? (t - k[i - 1].time) / span : 0.0f;
            return Vec3(k[i-1].v.x + (k[i].v.x - k[i-1].v.x) * u,
                        k[i-1].v.y + (k[i].v.y - k[i-1].v.y) * u,
                        k[i-1].v.z + (k[i].v.z - k[i-1].v.z) * u);
        }
    }
    return k.back().v;
}

EfpDiffuseKey SampleDiffuseAt(const std::vector<EfpDiffuseKey>& k, f32 t) {
    if (k.empty()) return EfpDiffuseKey();
    if (k.size() == 1 || t <= k.front().time) return k.front();
    if (t >= k.back().time) return k.back();
    for (size_t i = 1; i < k.size(); ++i) {
        if (t <= k[i].time) {
            const f32 span = k[i].time - k[i - 1].time;
            const f32 u = span > 1e-6f ? (t - k[i - 1].time) / span : 0.0f;
            EfpDiffuseKey o;
            o.time = t;
            o.r = k[i-1].r + (k[i].r - k[i-1].r) * u;
            o.g = k[i-1].g + (k[i].g - k[i-1].g) * u;
            o.b = k[i-1].b + (k[i].b - k[i-1].b) * u;
            o.a = k[i-1].a + (k[i].a - k[i-1].a) * u;
            return o;
        }
    }
    return k.back();
}

/// A Vector payload: three floats in file order x, y, z.
bool ReadVec3Into(EfpReader& e, Vec3* out) {
    const f32 x = e.r.readF32(), y = e.r.readF32(), z = e.r.readF32();
    if (e.r.failed()) return false;
    if (out) *out = Vec3(x, y, z);
    return true;
}

i32 CommandFrameCount(u8 byte, f32 startF, f32 endF, f32 stepF, u32 fc) {
    if (fc < 1) return 0;
    const f32 fcf = static_cast<f32>(fc);

    const f32 start = (byte & 1) ? fcf * std::trunc(startF / 100.0f) : std::trunc(startF);
    const int mode  = byte >> 1;
    const f32 step  = (mode & 1) ? fcf * stepF / 100.0f : stepF;

    f32 end = 0.0f;
    switch ((mode >> 1) % 5) {
        case 0: end = std::trunc(endF); break;
        case 1: end = fcf * std::trunc(endF / 100.0f); break;
        case 2: end = start + std::trunc(endF); break;
        case 3: end = start - fcf * std::trunc(endF / -100.0f); break;
        case 4:
            if (endF < 1.0f) return 0;
            end = start + std::trunc(endF * step);
            break;
        default: return 0;
    }
    if (end >= fcf) end = fcf - 1.0f;
    if (end > fcf || start > end || step < 0.000001f) return 0;

    const f32 n = std::trunc((end - start) / step) + 1.0f;
    if (n < 1.0f || n > 4096.0f) return 0;
    return static_cast<i32>(n);
}

/// FrameScale: u32 count, then count x (x,y,z) float32.
///
/// The on-disk list is always READ - the stream has to advance - but it is
/// DISCARDED whenever the count formula resolves, in which case the curve has
/// exactly that many entries. Only an unresolved formula lets the disk length
/// survive. Assignment, never append: the last writer in the file wins.
bool ReadFrameScale(EfpReader& e) {
    u32 n = 0;
    if (!e.count(n, 12)) return false;
    std::vector<EfpScaleKey> onDisk;
    onDisk.reserve(n);
    for (u32 i = 0; i < n; ++i) {
        EfpScaleKey k;
        k.v = Vec3(e.r.readF32(), e.r.readF32(), e.r.readF32());
        if (e.r.failed()) return false;
        onDisk.push_back(k);
    }
    if (!e.node) return true;

    // Argument order matters and is NOT the on-disk order: the formula's `end`
    // is the triple's THIRD float and its `step` is the second. Feeding them in
    // disk order makes the count collapse to 1 - measured 4 frames on a fixture
    // whose reference answer is 45.
    const i32 resolved = CommandFrameCount(e.srcByte, e.srcStart, e.srcSpan,
                                           e.srcEnd, e.node->lifeFrames);
    std::vector<EfpScaleKey> out;
    if (resolved > 0) {
        // Resampled from the global BlendScaleGraph at index/(count-1) over
        // [0,1]. Only a genuinely absent graph falls back to unit scale, which
        // is what the reference does too.
        const std::vector<EfpScaleKey>& g = e.node->blendScale;
        out.resize(static_cast<size_t>(resolved));
        for (size_t i = 0; i < out.size(); ++i) {
            const f32 t = out.size() > 1 ? static_cast<f32>(i) / static_cast<f32>(out.size() - 1) : 0.0f;
            out[i].time = t;
            out[i].v = g.empty() ? Vec3(1.0f, 1.0f, 1.0f) : SampleScaleAt(g, t);
        }
    } else {
        out = onDisk;
        for (size_t i = 0; i < out.size(); ++i)
            out[i].time = out.size() > 1 ? static_cast<f32>(i) / static_cast<f32>(out.size() - 1) : 0.0f;
    }
    e.node->scaleKeys = out;
    e.node->scaleFromCommand = true;
    return true;
}

bool ReadFrameDiffuse(EfpReader& e) {
    u32 n = 0;
    if (!e.count(n, 4)) return false;
    std::vector<EfpDiffuseKey> onDisk;
    onDisk.reserve(n);
    for (u32 i = 0; i < n; ++i) {
        const u8 b = e.r.readU8(), g = e.r.readU8(), r = e.r.readU8(), a = e.r.readU8();
        if (e.r.failed()) return false;
        EfpDiffuseKey k;
        k.b = static_cast<f32>(b) / 255.0f;
        k.g = static_cast<f32>(g) / 255.0f;
        k.r = static_cast<f32>(r) / 255.0f;
        k.a = static_cast<f32>(a) / 255.0f;
        onDisk.push_back(k);
    }
    if (!e.node) return true;

    // Argument order matters and is NOT the on-disk order: the formula's `end`
    // is the triple's THIRD float and its `step` is the second. Feeding them in
    // disk order makes the count collapse to 1 - measured 4 frames on a fixture
    // whose reference answer is 45.
    const i32 resolved = CommandFrameCount(e.srcByte, e.srcStart, e.srcSpan,
                                           e.srcEnd, e.node->lifeFrames);
    std::vector<EfpDiffuseKey> out;
    if (resolved > 0) {
        const std::vector<EfpDiffuseKey>& g = e.node->blendDiffuse;
        out.resize(static_cast<size_t>(resolved));
        for (size_t i = 0; i < out.size(); ++i) {
            const f32 t = out.size() > 1 ? static_cast<f32>(i) / static_cast<f32>(out.size() - 1) : 0.0f;
            out[i].time = t;
            if (!g.empty()) {
                const EfpDiffuseKey k = SampleDiffuseAt(g, t);
                out[i].r = k.r; out[i].g = k.g; out[i].b = k.b; out[i].a = k.a;
            }
        }
    } else {
        out = onDisk;
        for (size_t i = 0; i < out.size(); ++i)
            out[i].time = out.size() > 1 ? static_cast<f32>(i) / static_cast<f32>(out.size() - 1) : 0.0f;
    }
    e.node->diffuseKeys = out;
    return true;
}

/// Resample a key list onto exactly `frames` evenly spaced samples over 0..1.
/// The client runs an effect on a fixed 20 fps frame clock, so a curve is
/// evaluated once per frame of the particle's life rather than at its authored
/// key times - which is why the reference reports one frame per life frame.
template <class KeyT, class LerpFn>
void ResampleKeys(std::vector<KeyT>& keys, u32 frames, LerpFn lerp) {
    if (frames == 0 || keys.empty()) return;
    std::vector<KeyT> out;
    out.reserve(frames);
    for (u32 i = 0; i < frames; ++i) {
        const f32 t = (frames > 1) ? static_cast<f32>(i) / static_cast<f32>(frames - 1) : 0.0f;
        out.push_back(lerp(keys, t));
        out.back().time = t;
    }
    keys.swap(out);
}

void ResampleToFrames(std::vector<EfpScaleKey>& keys, u32 frames) {
    ResampleKeys(keys, frames, [](const std::vector<EfpScaleKey>& k, f32 t) {
        EfpScaleKey r;
        if (k.size() == 1 || t <= k.front().time) { r = k.front(); return r; }
        if (t >= k.back().time) { r = k.back(); return r; }
        for (size_t i = 1; i < k.size(); ++i) {
            if (t <= k[i].time) {
                const f32 span = k[i].time - k[i - 1].time;
                const f32 u = span > 1e-6f ? (t - k[i - 1].time) / span : 0.0f;
                r.v = Vec3(k[i-1].v.x + (k[i].v.x - k[i-1].v.x) * u,
                           k[i-1].v.y + (k[i].v.y - k[i-1].v.y) * u,
                           k[i-1].v.z + (k[i].v.z - k[i-1].v.z) * u);
                return r;
            }
        }
        r = k.back();
        return r;
    });
}

void ResampleToFrames(std::vector<EfpDiffuseKey>& keys, u32 frames) {
    ResampleKeys(keys, frames, [](const std::vector<EfpDiffuseKey>& k, f32 t) {
        EfpDiffuseKey r;
        if (k.size() == 1 || t <= k.front().time) { r = k.front(); return r; }
        if (t >= k.back().time) { r = k.back(); return r; }
        for (size_t i = 1; i < k.size(); ++i) {
            if (t <= k[i].time) {
                const f32 span = k[i].time - k[i - 1].time;
                const f32 u = span > 1e-6f ? (t - k[i - 1].time) / span : 0.0f;
                r.r = k[i-1].r + (k[i].r - k[i-1].r) * u;
                r.g = k[i-1].g + (k[i].g - k[i-1].g) * u;
                r.b = k[i-1].b + (k[i].b - k[i-1].b) * u;
                r.a = k[i-1].a + (k[i].a - k[i-1].a) * u;
                return r;
            }
        }
        r = k.back();
        return r;
    });
}


bool ReadStaticEmit(EfpReader& e) {
    EfpStaticEmit se;
    se.minVal       = e.r.readU32();
    se.maxVal       = e.r.readU32();
    se.burstRate    = e.r.readU32();
    se.minParticles = e.r.readU32();
    se.spawnRate    = e.r.readF32();
    if (e.r.failed()) return false;
    se.present = true;
    if (e.node) e.node->emit = se;
    return true;
}

/// One EEBlend: begin/end floats, then count x (time float + value). `comp` is
/// how many components the value has; `asByte` reads them as bytes /255.
/// Values land in `out` as (time, v0..v3).
struct BlendKey { f32 t = 0; f32 v[4] = { 0, 0, 0, 0 }; };

bool ReadBlendInto(EfpReader& e, int comp, bool asByte, std::vector<BlendKey>& out) {
    e.r.skip(8);                                  // begin, end
    u32 n = 0;
    const size_t unit = 4 + static_cast<size_t>(comp) * (asByte ? 1u : 4u);
    if (!e.count(n, unit)) return false;
    out.clear();
    out.reserve(n);
    for (u32 i = 0; i < n; ++i) {
        BlendKey k;
        k.t = e.r.readF32();
        for (int c = 0; c < comp; ++c)
            k.v[c] = asByte ? (static_cast<f32>(e.r.readU8()) / 255.0f) : e.r.readF32();
        if (e.r.failed()) return false;
        out.push_back(k);
    }
    return true;
}

/// Sample a blend curve at `t`, piecewise linear, clamped at both ends.
f32 SampleBlend(const std::vector<BlendKey>& keys, int comp, f32 t) {
    if (keys.empty()) return 1.0f;
    if (t <= keys.front().t) return keys.front().v[comp];
    if (t >= keys.back().t)  return keys.back().v[comp];
    for (size_t i = 1; i < keys.size(); ++i) {
        if (t <= keys[i].t) {
            const f32 span = keys[i].t - keys[i - 1].t;
            const f32 u = span > 1e-6f ? (t - keys[i - 1].t) / span : 0.0f;
            return keys[i - 1].v[comp] + (keys[i].v[comp] - keys[i - 1].v[comp]) * u;
        }
    }
    return keys.back().v[comp];
}

/// The union of every curve's key times, so the three scale axes (or the
/// colour and the alpha curve) can be resampled onto one shared timeline.
void UnionTimes(const std::vector<std::vector<BlendKey>>& curves, std::vector<f32>& out) {
    out.clear();
    for (const std::vector<BlendKey>& c : curves)
        for (const BlendKey& k : c) out.push_back(k.t);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end(),
                          [](f32 a, f32 b) { return std::fabs(a - b) < 1e-6f; }),
              out.end());
    // An empty union stays EMPTY. Synthesising a key at t=0 gives a curve of
    // one entry where the reference reports none, which is exactly the +1 the
    // batch comparison found on 31 of 2852 files.
}

// ---------------------------------------------------------------------------
bool ReadParameter(EfpReader& e, const std::string& name);

/// EEBlend<T>: float begin, float end, int blendCount, then count * (float time + T)
bool ReadBlend(EfpReader& e, size_t valueBytes) {
    e.r.skip(8);                       // begin, end
    u32 n = 0;
    if (!e.count(n, 4 + valueBytes)) return false;
    e.r.skip(static_cast<size_t>(n) * (4 + valueBytes));
    return !e.r.failed();
}

/// A plain "int count, then count * unitBytes" list.
bool ReadCountedBlob(EfpReader& e, size_t unitBytes) {
    u32 n = 0;
    if (!e.count(n, unitBytes)) return false;
    e.r.skip(static_cast<size_t>(n) * unitBytes);
    return !e.r.failed();
}

/// BSAnimation: int count, then that many .ban paths.
bool ReadBsAnimation(EfpReader& e, std::vector<std::wstring>* sink) {
    u32 n = 0;
    if (!e.count(n, 4)) return false;
    for (u32 i = 0; i < n; ++i) {
        const std::wstring p = e.wstr();
        if (e.r.failed()) return false;
        if (sink && !p.empty()) sink->push_back(p);
    }
    return true;
}

bool ReadParameter(EfpReader& e, const std::string& name) {
    // --- no parameter at all ------------------------------------------------
    if (name == "NeverExtinct" || name == "NormalTimeExtinct" ||
        name == "NormalTimeLoop" || name == "ProgramUpdate" ||
        name == "ViewNone" || name == "ViewBillboard" ||
        name == "ViewYBillboard" || name == "ViewVBillboard" ||
        name == "RenderNone" || name == "RenderPlate" || name == "RenderMesh" ||
        name == "RenderLinkDPipe" || name == "RenderLinkPipe" || name == "RenderLinkObj" ||
        name == "NormalTimeLife" || name == "NormalTimeLoopLife")
        return true;

    // --- fixed size ---------------------------------------------------------
    if (name == "StaticEmit") {
        if (e.node) {
            auto sane = [](f32 t) {
                return std::isfinite(t) && t >= 0.0f && t <= 600.0f &&
                       (t == 0.0f || t >= 1e-4f);
            };
            if (e.inSourceCommand && (e.srcEnd - e.srcStart) >= 1e-4f
                && sane(e.srcStart) && sane(e.srcEnd)) {
                e.node->emitStartSec = e.srcStart;
                e.node->emitEndSec   = e.srcEnd;
                e.node->hasEmitRange = true;
            }
        }
        return ReadStaticEmit(e);
    }
    if (name == "Attraction") {
        const f32 a = e.r.readF32();
        if (e.r.failed()) return false;
        if (e.node) e.node->attraction = a;
        return true;
    }
    // A pointer that SELECTS a curve: 0 means "the exported BlendScaleGraph",
    // which then multiplies each particle's size by a value drawn from its own
    // random phase. Discarding it made every sprite in a burst the same size.
    if (name == "SetGraphRandomScale") {
        const i32 ptr = static_cast<i32>(e.r.readU32());
        if (e.r.failed()) return false;
        if (e.node) { e.node->randomScalePointer = ptr; e.node->hasRandomScale = (ptr == 0); }
        return true;
    }
    if (name == "SetRotationMat" || name == "SetRVelocityMat") {
        f32 mat[16];
        for (int i = 0; i < 16; ++i) mat[i] = e.r.readF32();
        if (e.r.failed()) return false;
        if (e.node && name == "SetRotationMat") {
            for (int i = 0; i < 16; ++i) e.node->rotationMatrix[i] = mat[i];
            e.node->hasRotationMatrix = true;
            e.node->transformFlags = e.srcType;
        }
        return true;
    }
    // Motion payloads are all a plain Vector; they used to be skipped, which is
    // why every particle sat at the emitter origin.
    if (name == "SetVelocity") {
        if (e.node) e.node->velocitySourceType = e.srcType;
        return ReadVec3Into(e, e.node ? &e.node->velocity : nullptr);
    }
    if (name == "Force") {
        if (e.node) e.node->forceSourceType = e.srcType;
        return ReadVec3Into(e, e.node ? &e.node->force : nullptr);
    }
    if (name == "SetPosition") {
        if (e.node) e.node->positionSourceType = e.srcType;
        return ReadVec3Into(e, e.node ? &e.node->position : nullptr);
    }
    if (name == "SetSpherePos") {
        Vec3 v;
        if (!ReadVec3Into(e, &v)) return false;
        if (e.node) { e.node->spawnSphere = v; e.node->hasSpawnSphere = true; }
        return true;
    }
    // AngleVector1 - two Vector3s: the authored SOURCE triple and a converted
    // copy. Only the source is used; it is (min, max, angle), not a direction,
    // and it is what gives every particle its OWN randomized heading. Skipping
    // it made every cone emitter - sparks, explosions, dust - fire along a
    // single line instead of fanning out.
    if (name == "SetConePos" || name == "SetConeVel" || name == "ConeForce") {
        const f32 a = e.r.readF32(), b = e.r.readF32(), c = e.r.readF32();
        if (!e.r.skip(12)) return false;                              // converted copy
        if (e.r.failed()) return false;
        if (e.node) {
            const Vec3 src(a, b, c);
            if (name == "SetConePos") {
                e.node->spawnCone = src;
                e.node->hasSpawnCone = true;
            } else if (name == "SetConeVel") {
                e.node->coneVelocity = src;
                e.node->hasConeVelocity = true;
                // A cone velocity also supplies the scalar velocity when none
                // was authored: the mean of (min, max), on +Y, clamped to +-2.
                const f32 len = std::sqrt(e.node->velocity.x * e.node->velocity.x +
                                          e.node->velocity.y * e.node->velocity.y +
                                          e.node->velocity.z * e.node->velocity.z);
                if (len <= 0.0001f) {
                    const f32 speed = (a + b) * 0.5f;
                    if (std::fabs(speed) > 0.0001f) {
                        e.node->velocity = Vec3(0.0f, std::max(-2.0f, std::min(2.0f, speed)), 0.0f);
                        // A velocity that came FROM the cone is routed by the
                        // CONE's source byte, not by SetVelocity's.
                        e.node->velocitySourceType = e.srcType;
                    }
                }
            } else {
                const f32 speed = (a + b) * 0.5f;
                e.node->coneForce = std::fabs(speed) > 0.0001f
                                  ? Vec3(0.0f, std::max(-2.0f, std::min(2.0f, speed)), 0.0f)
                                  : Vec3(0.0f, 0.0f, 0.0f);
            }
        }
        return true;
    }
    // RotVector - a Vector3 then a 4x4. EVERY rotation-payload command feeds
    // the object's rotation, not just the Matrix form; missing these left
    // thousands of emitters with the right speed in the wrong direction.
    if (name == "SetRotation" || name == "SetRVelocity") {
        if (!e.r.skip(12)) return false;
        f32 mat[16];
        for (int i = 0; i < 16; ++i) mat[i] = e.r.readF32();
        if (e.r.failed()) return false;
        if (e.node && name == "SetRotation") {
            for (int i = 0; i < 16; ++i) e.node->rotationMatrix[i] = mat[i];
            e.node->hasRotationMatrix = true;
            e.node->transformFlags = e.srcType;
        }
        return true;
    }
    // AxisVector4 - a Vector4 (axis + angle) followed by a 4x4 matrix. The
    // matrix is the useful half: SetShapeRot is the base orientation and
    // SetShapeRotVel the PER-FRAME step.
    if (name == "SetRotationAxis" || name == "SetRVelocityAxis" ||
        name == "SetShapeRot"     || name == "SetShapeRotVel") {
        if (!e.r.skip(16)) return false;                              // axis + angle
        f32 mat[16];
        for (int i = 0; i < 16; ++i) mat[i] = e.r.readF32();
        if (e.r.failed()) return false;
        if (e.node) {
            if (name == "SetShapeRot") {
                for (int i = 0; i < 16; ++i) e.node->shapeRotation[i] = mat[i];
                e.node->hasShapeRotation = true;
            } else if (name == "SetShapeRotVel") {
                for (int i = 0; i < 16; ++i) e.node->shapeRotationVel[i] = mat[i];
                e.node->hasShapeRotationVel = true;
            } else if (name == "SetRotationAxis") {
                for (int i = 0; i < 16; ++i) e.node->rotationMatrix[i] = mat[i];
                e.node->hasRotationMatrix = true;
                e.node->transformFlags = e.srcType;
            }
        }
        return true;
    }

    // --- counted lists ------------------------------------------------------
    // Both BAN frame lists are preceded by a float32 SPEED, before the count.
    if (name == "SetBANRot")       return e.r.skip(4) && ReadCountedBlob(e, 64);
    if (name == "SetBANPos") {
        const f32 speed = e.r.readF32();     // precedes the count
        u32 n = 0;
        if (!e.count(n, 12)) return false;
        std::vector<Vec3> pts;
        pts.reserve(n);
        for (u32 i = 0; i < n; ++i) {
            const f32 x = e.r.readF32(), y = e.r.readF32(), z = e.r.readF32();
            if (e.r.failed()) return false;
            pts.push_back(Vec3(x, y, z));
        }
        if (e.node) {
            e.node->banPositions = pts;
            e.node->banSpeed = speed;
            e.node->banPositionSourceType = e.srcType;
        }
        return true;
    }
    if (name == "SetGraphScale")   return ReadFrameScale(e);          // FrameScale
    if (name == "SetGraphDiffuse") {
        if (e.node) e.node->diffuseFromCommand = true;
        return ReadFrameDiffuse(e);                                   // FrameDiffuse
    }
    if (name == "TextureSlide") {                                     // Vector3 + Vector4 list
        // The three floats in front are a GRID descriptor, not padding:
        // columns, rows and a step. Discarding them made every flipbook sprite
        // play the wrong cells at the wrong rate.
        const f32 g0 = e.r.readF32(), g1 = e.r.readF32(), g2 = e.r.readF32();
        if (e.r.failed()) return false;
        if (e.node) {
            e.node->texSlideGrid[0] = g0;
            e.node->texSlideGrid[1] = g1;
            e.node->texSlideGrid[2] = g2;
            e.node->hasTexSlideGrid = true;
        }
        u32 n = 0;
        if (!e.count(n, 16)) return false;
        std::vector<EfpTexFrame> frames;
        frames.reserve(n);
        for (u32 i = 0; i < n; ++i) {
            EfpTexFrame tf;
            for (int k = 0; k < 4; ++k) tf.f[k] = e.r.readF32();
            if (e.r.failed()) return false;
            frames.push_back(tf);
        }
        if (e.node) {
            // An integer grid REPLACES the authored frames with a generated
            // cell schedule; anything else keeps them as authored.
            const i32 columns = static_cast<i32>(g0);
            const i32 rows    = static_cast<i32>(g1);
            const bool grid = (static_cast<f32>(columns) == g0 && static_cast<f32>(rows) == g1)
                           && columns > 0 && columns <= 1024 && rows > 0 && rows <= 1024;
            if (grid && !frames.empty()) {
                const f32 step = std::isfinite(g2) ? g2 : 1.0f;
                const i32 cells = columns * rows;
                const f32 w = 1.0f / static_cast<f32>(columns);
                const f32 h = 1.0f / static_cast<f32>(rows);
                std::vector<EfpTexFrame> cellFrames;
                cellFrames.reserve(frames.size());
                for (size_t i = 0; i < frames.size(); ++i) {
                    i32 fi = static_cast<i32>(std::trunc(static_cast<f32>(i) * step)) % cells;
                    if (fi < 0) fi += cells;
                    EfpTexFrame tf;
                    tf.f[0] = static_cast<f32>(fi % columns) / static_cast<f32>(columns);
                    tf.f[1] = static_cast<f32>((fi / columns) % rows) / static_cast<f32>(rows);
                    tf.f[2] = w;
                    tf.f[3] = h;
                    cellFrames.push_back(tf);
                }
                e.node->texFrames = cellFrames;
                e.node->texFramesStepped = true;   // holds on the last cell
            } else {
                e.node->texFrames = frames;
            }
        }
        return true;
    }

    // --- graphs used by EEGlobalData ---------------------------------------
    if (name == "BlendScaleGraph") {
        std::vector<BlendKey> g;
        if (!ReadBlendInto(e, 3, false, g)) return false;             // Vector3
        if (e.node) {
            e.node->blendScale.clear();
            for (const BlendKey& k : g) {
                EfpScaleKey sk;
                sk.time = k.t;
                sk.v = Vec3(k.v[0], k.v[1], k.v[2]);
                e.node->blendScale.push_back(sk);
            }
        }
        return true;
    }
    if (name == "BlendDiffuseGraph") {
        std::vector<BlendKey> g;
        if (!ReadBlendInto(e, 4, true, g)) return false;              // ARGB32 bytes
        if (e.node) {
            e.node->blendDiffuse.clear();
            for (const BlendKey& k : g) {
                EfpDiffuseKey dk;
                dk.time = k.t;
                // Stored B,G,R,A on disk, like every other colour in this format.
                dk.b = k.v[0]; dk.g = k.v[1]; dk.r = k.v[2]; dk.a = k.v[3];
                e.node->blendDiffuse.push_back(dk);
            }
            // The reference seeds diffuseFrames from this graph, but ONLY when
            // they are still empty - there is no equivalent seed for scale.
            if (e.node->diffuseKeys.empty())
                e.node->diffuseKeys = e.node->blendDiffuse;
        }
        return true;
    }
    if (name == "BSAnimation")       return ReadBsAnimation(e, nullptr);

    e.markUnknown(MultiByteToWide(name, kPathCodePage));
    return false;
}

// ---------------------------------------------------------------------------
// EESourceData / EESource / EESourceList
// ---------------------------------------------------------------------------
bool ReadSourceData(EfpReader& e, std::wstring* outCommand) {
    const bool hasData = e.r.readBool8();
    if (e.r.failed()) return false;
    if (!hasData) { if (outCommand) outCommand->clear(); return true; }

    const std::string cmd = e.str();
    if (e.r.failed()) return false;
    if (outCommand) *outCommand = MultiByteToWide(cmd, kPathCodePage);

    // byte0 is NOT unused: it is the command's SOURCE TYPE, which decides
    // whether a position / path / rotation is expressed in the object's own
    // frame, the parent's, or the link's.
    e.srcType  = e.r.readU8();
    e.srcByte  = e.r.readU8();       // byte1 - selects the count formula
    e.srcStart = e.r.readF32();
    e.srcEnd   = e.r.readF32();
    e.srcSpan  = e.r.readF32();
    if (e.r.failed()) return false;

    // The object's own time window is the UNION of every command triple that
    // reads as a sane timeline range. Leaving it at zero made every stage of a
    // choreographed effect - charge, flash, burst, smoke - fire at once on
    // frame 0 and never stop.
    e.inSourceCommand = true;
    if (e.node) {
        // A real authored time is seconds with millisecond resolution. These
        // fields also carry DENORMALS (values around 1e-39) when the bytes are
        // not a time at all, and those pass every finite/range test while
        // being meaningless - one such pair set an object window of 1.7e-39 to
        // 2.7e-39 seconds and collapsed the emitter's end time to zero.
        auto sane = [](f32 t) {
            return std::isfinite(t) && t >= 0.0f && t <= 600.0f &&
                   (t == 0.0f || t >= 1e-4f);
        };
        if (sane(e.srcStart) && sane(e.srcEnd) && (e.srcEnd - e.srcStart) >= 1e-4f) {
            if (e.node->endSec > e.node->startSec)
                e.node->startSec = std::min(e.node->startSec, e.srcStart);
            else
                e.node->startSec = e.srcStart;
            e.node->endSec = std::max(e.node->endSec, e.srcEnd);
        }
    }

    return ReadParameter(e, cmd);
}

bool ReadSourceList(EfpReader& e) {
    u32 n = 0;
    if (!e.count(n, 1)) return false;
    for (u32 i = 0; i < n; ++i)
        if (!ReadSourceData(e, nullptr)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// EEGlobalData
// ---------------------------------------------------------------------------
bool ReadGlobalData(EfpReader& e) {
    // EEGlobalData reuses the same parameter readers but has no time triple.
    e.inSourceCommand = false;
    const u32 int0 = e.r.readU32();    // particle life, in 20 fps frames
    if (e.node) e.node->lifeFrames = int0;
    u32 n = 0;
    if (!e.count(n, 4)) return false;
    for (u32 i = 0; i < n; ++i) {
        const std::string pname = e.str();
        if (e.r.failed()) return false;
        if (!ReadParameter(e, pname)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// EEResource - 9 render state ints, then the mesh / texture reference tree.
// This is where a node's .bms and .ddj paths come from.
// ---------------------------------------------------------------------------
bool ReadResource(EfpReader& e, std::vector<std::wstring>& sink) {
    EfpRenderState rs;
    rs.backFaceType = e.r.readU32();
    rs.srcBlend     = e.r.readU32();
    rs.dstBlend     = e.r.readU32();
    rs.srcArg1      = e.r.readU32();
    rs.srcArg2      = e.r.readU32();
    rs.srcOp        = e.r.readU32();
    rs.dstArg1      = e.r.readU32();
    rs.dstArg2      = e.r.readU32();
    rs.dstOp        = e.r.readU32();
    if (e.r.failed()) return false;
    rs.present = true;
    if (e.node) {
        const EfpRenderState keepPaths = e.node->render;
        e.node->render = rs;
        e.node->render.meshPaths    = keepPaths.meshPaths;
        e.node->render.texturePaths = keepPaths.texturePaths;
    }

    u32 meshCount = 0;
    if (!e.count(meshCount, 4)) return false;

    for (u32 i = 0; i < meshCount; ++i) {
        const std::wstring meshPath = e.wstr();
        if (e.r.failed()) return false;
        if (!meshPath.empty()) sink.push_back(meshPath);
        // Kept separately as well: the render mode a node REPORTS depends on
        // whether it actually has a mesh, not just on its Shape name.
        if (e.node) e.node->render.meshPaths.push_back(meshPath);

        u32 texCount = 0;
        if (!e.count(texCount, 4)) return false;
        for (u32 t = 0; t < texCount; ++t) {
            const std::wstring texPath = e.wstr();
            if (e.r.failed()) return false;
            if (!texPath.empty()) sink.push_back(texPath);
            if (e.node && !texPath.empty()) e.node->render.texturePaths.push_back(texPath);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// EFController - dispatched on the controller name.
// ---------------------------------------------------------------------------
bool ReadController(EfpReader& e, const std::string& name, EfpNode& node) {
    if (name == "NormalTimeLife" || name == "NormalTimeLoopLife")
        return true;                                     // empty

    if (name == "StaticEmit")  return ReadStaticEmit(e);
    if (name == "LinkMode")    return e.r.skip(16);      // 4 ints
    if (name == "BAN")         return ReadBsAnimation(e, &node.resourcePaths);

    if (name == "ViewMode") {
        node.viewModeSource = e.wstr();
        if (e.r.failed()) return false;
        node.viewMode = ViewModeFromName(node.viewModeSource);
        return true;
    }
    if (name == "Shape") {
        node.renderSource = e.wstr();
        if (e.r.failed()) return false;
        node.renderMode = RenderModeFromName(node.renderSource);
        return ReadResource(e, node.resourcePaths);
    }
    if (name == "ScaleGraph") {
        std::vector<BlendKey> gx, gy, gz;
        if (!ReadBlendInto(e, 1, false, gx)) return false;
        if (!ReadBlendInto(e, 1, false, gy)) return false;
        if (!ReadBlendInto(e, 1, false, gz)) return false;
        if (!e.r.skip(8)) return false;                  // ScaleGraph alone has a trailing pair
        if (e.node) {
            std::vector<f32> times;
            UnionTimes({ gx, gy, gz }, times);
            e.node->scaleKeys.clear();
            for (f32 t : times) {
                EfpScaleKey k;
                k.time = t;
                k.v = Vec3(SampleBlend(gx, 0, t), SampleBlend(gy, 0, t), SampleBlend(gz, 0, t));
                e.node->scaleKeys.push_back(k);
            }
        }
        return true;
    }
    if (name == "DiffuseGraph") {
        std::vector<BlendKey> alpha, colour;
        if (!ReadBlendInto(e, 1, true,  alpha))  return false;   // EEBlend<byte>
        if (!ReadBlendInto(e, 4, true,  colour)) return false;   // EEBlend<Color>, B,G,R,A
        if (e.node) {
            std::vector<f32> times;
            UnionTimes({ alpha, colour }, times);
            e.node->diffuseKeys.clear();
            for (f32 t : times) {
                EfpDiffuseKey k;
                k.time = t;
                // On disk the colour components are B,G,R,A - reorder here, or
                // every effect comes out with red and blue swapped.
                k.b = SampleBlend(colour, 0, t);
                k.g = SampleBlend(colour, 1, t);
                k.r = SampleBlend(colour, 2, t);
                k.a = alpha.empty() ? SampleBlend(colour, 3, t) : SampleBlend(alpha, 0, t);
                e.node->diffuseKeys.push_back(k);
            }
        }
        return !e.r.failed();   // NO trailing pair - only ScaleGraph carries one
    }

    // "Program" (EEStaticProgram) is serialised as an EESourceList - this is
    // how the reference (sro-archive-explorer effectStructure) reads it. Modeling
    // it here stops the tree from bailing at the very common Program node, so
    // ParseEfp completes and every emitter's resources are recovered.
    if (name == "Program") return ReadSourceList(e);

    // Genuinely unknown controller: stop this branch honestly rather than guess.
    e.markUnknown(MultiByteToWide(name, kPathCodePage));
    return false;
}

// ---------------------------------------------------------------------------
// EFStoredObject - the recursive node.
// ---------------------------------------------------------------------------
bool ReadObject(EfpReader& e, EfpNode& node, int depth) {
    if (depth > kMaxDepth) { e.markUnknown(T(L"(çok derin ağaç)")); return false; }

    // Readers that retain state write through e.node. Saved and restored so a
    // child never leaves the pointer aimed at itself when the parent resumes.
    EfpNode* const savedNode = e.node;
    e.node = &node;
    struct NodeGuard {
        EfpReader& e; EfpNode* prev;
        ~NodeGuard() { e.node = prev; }
    } guard{ e, savedNode };

    node.dataOffset = e.r.readI32();
    node.name       = e.wstr();
    if (e.r.failed()) return false;

    // --- controllers --------------------------------------------------------
    u32 controllerCount = 0;
    if (!e.count(controllerCount, 4)) return false;
    for (u32 i = 0; i < controllerCount; ++i) {
        const std::string cname = e.str();
        if (e.r.failed()) return false;
        node.controllerNames.push_back(MultiByteToWide(cname, kPathCodePage));
        if (!ReadController(e, cname, node)) { node.partial = true; return false; }
    }

    // --- global data and the five source blocks -----------------------------
    if (!ReadGlobalData(e))  { node.partial = true; return false; }
    if (!ReadSourceList(e))  { node.partial = true; return false; }   // emptySourceList0
    if (!ReadSourceList(e))  { node.partial = true; return false; }   // emitterSourceList
    if (!ReadSourceList(e))  { node.partial = true; return false; }   // emptySourceList2
    if (!ReadSourceData(e, &node.lifeTimeSource)) { node.partial = true; return false; }
    node.lifeMode = LifeModeFromName(node.lifeTimeSource);


    // No Shape controller at all means the object draws nothing. That is the
    // authored "none" render mode, not an unrecognised one.
    if (node.renderMode == EfpRenderMode::Unknown && node.renderSource.empty())
        node.renderMode = EfpRenderMode::None;
    if (!ReadSourceList(e))  { node.partial = true; return false; }   // programSourceList

    e.r.skip(2);        // byte0, byte1
    e.r.skip(12);       // int0, int1, int2
    e.r.skip(1);        // byte2
    e.r.skip(4);        // int3
    e.r.skip(1);        // byte3
    if (e.r.failed()) { node.partial = true; return false; }

    if (!ReadSourceData(e, &node.viewModeSource))  { node.partial = true; return false; }
    if (!ReadResource(e, node.resourcePaths))      { node.partial = true; return false; }
    if (!ReadSourceData(e, &node.renderSource))    { node.partial = true; return false; }
    if (!ReadSourceList(e))                        { node.partial = true; return false; }
    if (!ReadSourceList(e))                        { node.partial = true; return false; }

    // Reconcile the authored name against what the node actually carries.
    node.renderMode = DisplayMode(node.renderMode, node.render.meshPaths,
                                  node.render.texturePaths);

    // NO resampling and NO clearing here. The reference does neither: it lets
    // the last writer's array stand as-is. Resampling inflated a 2-key
    // controller curve to lifeFrames (2193 emitters keep controller times), and
    // clearing non-drawing nodes deleted frames that 170 "none" emitters
    // legitimately carry.

    // --- children -----------------------------------------------------------
    u32 childCount = 0;
    if (!e.count(childCount, 4)) { node.partial = true; return false; }
    node.children.resize(childCount);
    for (u32 i = 0; i < childCount; ++i) {
        if (!ReadObject(e, node.children[i], depth + 1)) {
            node.partial = true;
            node.children.resize(i + 1);   // keep the partial child for the report
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Fallback scan: harvest plausible length-prefixed asset paths from a byte
// range. Only used when the structured walk stopped early, and only ever
// reported as "scan found this".
// ---------------------------------------------------------------------------
bool LooksLikeAssetPath(const std::string& s) {
    if (s.size() < 5 || s.size() > 260) return false;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) return false;
    }
    const std::string lower = ToLowerA(s);
    return EndsWithI(MultiByteToWide(lower, kPathCodePage), L".ddj") ||
           EndsWithI(MultiByteToWide(lower, kPathCodePage), L".bms") ||
           EndsWithI(MultiByteToWide(lower, kPathCodePage), L".ban") ||
           EndsWithI(MultiByteToWide(lower, kPathCodePage), L".efp");
}

void ScavengePaths(const u8* data, size_t size, size_t from,
                   std::vector<std::wstring>& out) {
    if (!data || from >= size) return;
    for (size_t i = from; i + 4 < size; ++i) {
        u32 len = 0;
        std::memcpy(&len, data + i, 4);
        if (len < 5 || len > 260) continue;
        if (i + 4 + len > size) continue;
        const std::string s(reinterpret_cast<const char*>(data + i + 4), len);
        if (!LooksLikeAssetPath(s)) continue;

        const std::wstring w = MultiByteToWide(s, kPathCodePage);
        bool seen = false;
        for (const std::wstring& e : out) if (IEqualsW(e, w)) { seen = true; break; }
        if (!seen) out.push_back(w);
        i += 4 + len - 1;
    }
}

void CollectPaths(const EfpNode& n, std::vector<std::wstring>& out) {
    for (const std::wstring& p : n.resourcePaths) {
        if (p.empty()) continue;
        bool seen = false;
        for (const std::wstring& e : out) if (IEqualsW(e, p)) { seen = true; break; }
        if (!seen) out.push_back(p);
    }
    for (const EfpNode& c : n.children) CollectPaths(c, out);
}

bool AnyPartial(const EfpNode& n) {
    if (n.partial) return true;
    for (const EfpNode& c : n.children) if (AnyPartial(c)) return true;
    return false;
}

void AppendNodeTree(const EfpNode& n, int indent, std::wstring& s, int& budget) {
    if (budget <= 0) return;
    --budget;

    std::wstring pad(static_cast<size_t>(indent) * 2, L' ');
    s += L"  " + pad + (n.name.empty() ? std::wstring(T(L"(isimsiz)")) : n.name);
    if (n.partial) s += T(L"  [kısmi]");
    s += L"\n";

    if (!n.controllerNames.empty()) {
        std::wstring c;
        for (size_t i = 0; i < n.controllerNames.size(); ++i) {
            if (i) c += L", ";
            c += n.controllerNames[i];
        }
        s += L"  " + pad + T(L"  denetleyici: ") + c + L"\n";
    }
    if (!n.lifeTimeSource.empty())
        s += L"  " + pad + T(L"  ömür       : ") + n.lifeTimeSource + L"\n";
    if (!n.renderSource.empty())
        s += L"  " + pad + L"  render     : " + n.renderSource + L"\n";
    for (const std::wstring& p : n.resourcePaths)
        s += L"  " + pad + T(L"  kaynak     : ") + p + L"\n";

    for (const EfpNode& c : n.children) AppendNodeTree(c, indent + 1, s, budget);
}

} // namespace

// ---------------------------------------------------------------------------
// EfpNode / EfpFile helpers
// ---------------------------------------------------------------------------
size_t EfpNode::countNodes() const {
    size_t n = 1;
    for (const EfpNode& c : children) n += c.countNodes();
    return n;
}

std::vector<std::wstring> EfpFile::allResourcePaths() const {
    std::vector<std::wstring> out;
    CollectPaths(root, out);
    return out;
}

// ---------------------------------------------------------------------------
// IsEfp
// ---------------------------------------------------------------------------
bool IsEfp(const u8* data, size_t size) {
    return data && size >= sizeof(kEfpSignature) - 1 &&
           std::memcmp(data, kEfpSignature, sizeof(kEfpSignature) - 1) == 0;
}

// ---------------------------------------------------------------------------
// ParseEfp
// ---------------------------------------------------------------------------
Result ParseEfp(const u8* data, size_t size, EfpFile& out) {
    out = EfpFile();

    if (!data || size < 12)
        return Result::Fail(Status::Truncated, T(L"EFP: dosya 12 bayttan kısa."));
    if (!IsEfp(data, size))
        return Result::Fail(Status::BadFormat, T(L"EFP: imza \"JMXVEFF \" değil."));

    out.bytesTotal = size;

    EfpReader e(data, size);
    out.signature = e.r.readFixedString(8);
    const std::string ver = e.r.readFixedString(4);
    out.version = MultiByteToWide(ver, kPathCodePage);

    if (ver == "0012" || ver == "0013") {
        out.hasRootScale = true;
        out.rootScale    = e.r.readF32();
    }
    if (ver == "0013") {
        out.v13Int0 = e.r.readI32();
        out.v13Int1 = e.r.readI32();
        out.v13Int2 = e.r.readI32();
    }
    if (e.r.failed())
        return Result::Fail(Status::Truncated, T(L"EFP: başlık eksik."));

    const bool complete = ReadObject(e, out.root, 0);
    out.bytesParsed = e.r.pos();

    // "Fully understood" means: the tree parsed AND we consumed the file. A few
    // bytes of trailing padding are tolerated.
    out.fullyUnderstood = complete && !AnyPartial(out.root) &&
                          (out.bytesParsed + 8 >= size);

    if (!complete) {
        // Give the user the asset references even though the walk stopped.
        ScavengePaths(data, size, out.bytesParsed, out.root.resourcePaths);
        out.root.partial = true;

        std::wstring why;
        if (e.unknownTag)
            why = T(L"EFP: modellenmemiş denetleyici/komut \"") + e.unknownName +
                  T(L"\" - ağaç bu noktada durduruldu.");
        else
            why = T(L"EFP: dosya beklenenden önce bitti.");
        return Result::Fail(Status::Truncated, why);
    }

    return Result::Ok();
}

// ---------------------------------------------------------------------------
// summaryTr
// ---------------------------------------------------------------------------
std::wstring EfpFile::summaryTr() const {
    std::wstring s;
    s += T(L"İmza          : ") + MultiByteToWide(signature, 1252) + version + L"\n";
    if (hasRootScale)
        s += FormatW(T(L"Kök ölçek     : %.4f\n"), rootScale);
    if (version == L"0013")
        s += FormatW(T(L"0013 alanları : %d, %d, %d\n"), v13Int0, v13Int1, v13Int2);

    s += T(L"Nesne sayısı  : ") + FormatCountTr(root.countNodes()) + L"\n";
    s += T(L"Çözümlenen    : ") + FormatBytesTr(bytesParsed) + L" / " +
         FormatBytesTr(bytesTotal) + L"\n";
    s += T(L"Durum         : ") + std::wstring(fullyUnderstood
             ? T(L"tam çözümlendi")
             : T(L"kısmi - aşağıdaki kaynak listesi kısmen tarama ile bulundu")) + L"\n";

    const std::vector<std::wstring> res = allResourcePaths();
    s += T(L"Kaynak sayısı : ") + FormatCountTr(res.size()) + L"\n";
    {
        const size_t shown = (res.size() < 16) ? res.size() : 16;
        for (size_t i = 0; i < shown; ++i) s += L"    " + res[i] + L"\n";
        if (res.size() > shown)
            s += T(L"    ... ve ") + FormatCountTr(res.size() - shown) + T(L" kaynak daha\n");
    }

    s += T(L"\nNesne ağacı:\n");
    int budget = 40;
    AppendNodeTree(root, 0, s, budget);
    if (budget <= 0) s += L"  ...\n";

    return s;
}

} // namespace sro
