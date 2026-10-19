// SkillEffect Studio - efpwrite/EfpEdit.cpp  (v6 M22) - edits on the lossless .efp model
#include "EfpEdit.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace se::efpw {

namespace {

constexpr std::uint32_t kInfiniteTicks = 100000;   // StaticEmit duration at / above this = "whole parent life"

bool Sel(const NodeList& nl, int i) { return nl.empty() || std::find(nl.begin(), nl.end(), i) != nl.end(); }
bool Finite(float v) { return std::isfinite(v); }

bool Drawn(const Node& n) { return n.render.has && n.render.command != "RenderNone"; }
bool HasBaked(const Node& n, const char* cmd) {
    for (const auto& s : n.baked)
        if (s.has && s.command == cmd) return true;
    return false;
}

// every runtime program list + (optionally) the Program controller's editor copy
void ForEachProgramSource(Node& n, const std::function<void(Source&)>& f, bool withController = true) {
    for (auto* l : {&n.list0, &n.list2, &n.programList, &n.list3, &n.baked})
        for (auto& s : *l)
            if (s.has) f(s);
    if (!withController) return;
    if (Named* c = n.Controller("Program")) {
        std::vector<Source> v;
        if (!DecodeSourceList(c->payload, v)) return;
        Bytes before = c->payload;
        for (auto& s : v)
            if (s.has) f(s);
        Bytes after = EncodeSourceList(v);
        if (after != before) c->payload = std::move(after);
    }
}

Source MakeSource(const char* cmd, std::uint8_t mode, float a, float b, float c, Bytes payload) {
    Source s;
    s.has = true;
    s.command = cmd;
    s.type = 0;
    s.mode = mode;
    s.SetTime(0, a);
    s.SetTime(1, b);
    s.SetTime(2, c);
    s.payload = std::move(payload);
    return s;
}

// blend = f32 begin, f32 end, u32 n, n x (f32 t, value[valueBytes]); returns byte size or 0 when out of range
std::size_t BlendSize(const Bytes& p, std::size_t off, std::size_t valueBytes) {
    if (off + 12 > p.size()) return 0;
    std::uint64_t n = GetU32(p, off + 8);
    std::uint64_t sz = 12 + n * (4 + valueBytes);
    if (off + sz > p.size()) return 0;
    return static_cast<std::size_t>(sz);
}

int AdjustBytes(std::uint8_t* bgra, const ColorAdjust& a) {
    std::uint8_t old[4];
    std::memcpy(old, bgra, 4);
    AdjustBgra(bgra, a);
    return std::memcmp(old, bgra, 4) != 0 ? 1 : 0;
}
int AdjustBlendBgra(Bytes& p, std::size_t off, const ColorAdjust& a) {
    if (!BlendSize(p, off, 4)) return 0;
    std::uint32_t n = GetU32(p, off + 8);
    int c = 0;
    for (std::uint32_t i = 0; i < n; ++i) c += AdjustBytes(&p[off + 12 + i * 8 + 4], a);
    return c;
}
int AdjustDiffusePayload(Bytes& p, const ColorAdjust& a) {
    if (p.size() < 4) return 0;
    std::uint64_t n = GetU32(p, 0);
    if (4 + n * 4 > p.size()) return 0;
    int c = 0;
    for (std::uint64_t i = 0; i < n; ++i) c += AdjustBytes(&p[4 + static_cast<std::size_t>(i) * 4], a);
    return c;
}

// --- rotation matrices (D3DX row-vector, payload 16 floats) -----------------------------------------------
bool ScaleRotationAngle(Bytes& p, std::size_t off, double k) {
    if (off + 64 > p.size()) return false;
    double R[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) R[r][c] = GetF32(p, off + static_cast<std::size_t>(r * 4 + c) * 4);
    // orthonormal + det 1 (else it is not a pure rotation: leave it)
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double d = R[i][0] * R[j][0] + R[i][1] * R[j][1] + R[i][2] * R[j][2];
            if (std::fabs(d - (i == j ? 1.0 : 0.0)) > 2e-3) return false;
        }
    double det = R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) - R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0]) +
                 R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]);
    if (std::fabs(det - 1.0) > 5e-3) return false;
    double c = std::clamp((R[0][0] + R[1][1] + R[2][2] - 1.0) * 0.5, -1.0, 1.0);
    double sx = (R[1][2] - R[2][1]) * 0.5, sy = (R[2][0] - R[0][2]) * 0.5, sz = (R[0][1] - R[1][0]) * 0.5;
    double sn = std::sqrt(sx * sx + sy * sy + sz * sz);
    double theta = std::atan2(sn, c);
    double x, y, z;
    if (sn > 1e-7) {
        x = sx / sn; y = sy / sn; z = sz / sn;
    } else if (c > 0) {
        return true;   // identity: nothing to scale
    } else {           // ~180 degrees: axis from the diagonal
        x = std::sqrt(std::max(0.0, (R[0][0] + 1) * 0.5));
        y = std::sqrt(std::max(0.0, (R[1][1] + 1) * 0.5));
        z = std::sqrt(std::max(0.0, (R[2][2] + 1) * 0.5));
        if (x >= y && x >= z) { if (R[0][1] + R[1][0] < 0) y = -y; if (R[0][2] + R[2][0] < 0) z = -z; }
        else if (y >= z) { if (R[0][1] + R[1][0] < 0) x = -x; if (R[1][2] + R[2][1] < 0) z = -z; }
        else { if (R[0][2] + R[2][0] < 0) x = -x; if (R[1][2] + R[2][1] < 0) y = -y; }
        double l = std::sqrt(x * x + y * y + z * z);
        if (l < 1e-9) return false;
        x /= l; y /= l; z /= l;
    }
    double t = theta * k, cs = std::cos(t), s = std::sin(t), ic = 1 - cs;
    double M[3][3] = {{ic * x * x + cs, ic * x * y + s * z, ic * x * z - s * y},
                      {ic * x * y - s * z, ic * y * y + cs, ic * y * z + s * x},
                      {ic * x * z + s * y, ic * y * z - s * x, ic * z * z + cs}};
    for (int r = 0; r < 3; ++r)
        for (int cc = 0; cc < 3; ++cc) PutF32(p, off + static_cast<std::size_t>(r * 4 + cc) * 4, static_cast<float>(M[r][cc]));
    return true;
}

void MulF32(Bytes& p, std::size_t off, double k) { PutF32(p, off, static_cast<float>(GetF32(p, off) * k)); }

// --- per-firing arrays ---------------------------------------------------------------------------------
bool ArrayLayout(const std::string& cmd, std::size_t& pre, std::size_t& unit) {
    if (cmd == "TextureSlide") { pre = 12; unit = 16; return true; }
    if (cmd == "SetGraphScale") { pre = 0; unit = 12; return true; }
    if (cmd == "SetGraphDiffuse") { pre = 0; unit = 4; return true; }
    if (cmd == "SetBANRot") { pre = 4; unit = 64; return true; }
    if (cmd == "SetBANPos") { pre = 4; unit = 12; return true; }
    return false;
}
int ArrayLen(const Source& s) {
    std::size_t pre, unit;
    if (!ArrayLayout(s.command, pre, unit) || s.payload.size() < pre + 4) return -1;
    return static_cast<int>(GetU32(s.payload, pre));
}
bool ResampleArray(Source& s, int n1) {
    std::size_t pre, unit;
    if (!ArrayLayout(s.command, pre, unit) || s.payload.size() < pre + 4 || n1 <= 0) return false;
    std::uint32_t n0 = GetU32(s.payload, pre);
    if (n0 == 0 || static_cast<int>(n0) == n1 || s.payload.size() < pre + 4 + n0 * unit) return false;
    Bytes out(s.payload.begin(), s.payload.begin() + static_cast<std::ptrdiff_t>(pre));
    out.resize(pre + 4 + static_cast<std::size_t>(n1) * unit);
    PutU32(out, pre, static_cast<std::uint32_t>(n1));
    const std::uint8_t* src = s.payload.data() + pre + 4;
    for (int i = 0; i < n1; ++i) {
        double u = n1 > 1 ? static_cast<double>(i) * (n0 - 1) / (n1 - 1) : 0.0;
        std::uint32_t i0 = static_cast<std::uint32_t>(std::floor(u));
        if (i0 > n0 - 1) i0 = n0 - 1;
        std::uint32_t i1 = std::min(i0 + 1, n0 - 1);
        double fr = u - i0;
        std::uint8_t* dst = out.data() + pre + 4 + static_cast<std::size_t>(i) * unit;
        const std::uint8_t* a = src + static_cast<std::size_t>(i0) * unit;
        const std::uint8_t* b = src + static_cast<std::size_t>(i1) * unit;
        if (s.command == "SetGraphScale" || s.command == "SetBANPos") {
            for (int c = 0; c < 3; ++c) {
                float fa, fb;
                std::memcpy(&fa, a + c * 4, 4);
                std::memcpy(&fb, b + c * 4, 4);
                float v = fr == 0 ? fa : static_cast<float>(fa + (fb - fa) * fr);
                std::memcpy(dst + c * 4, &v, 4);
            }
        } else if (s.command == "SetGraphDiffuse") {
            for (int c = 0; c < 4; ++c)
                dst[c] = static_cast<std::uint8_t>(std::clamp(std::lround(a[c] + (b[c] - a[c]) * fr), 0L, 255L));
        } else {
            std::memcpy(dst, fr < 0.5 ? a : b, unit);   // TextureSlide frames / BAN matrices: nearest
        }
    }
    s.payload = std::move(out);
    return true;
}

// --- StaticEmit ------------------------------------------------------------------------------------------
void EmitPayloads(Node& n, std::vector<Bytes*>& out) {
    for (auto& s : n.emitList)
        if (s.has && s.command == "StaticEmit" && s.payload.size() >= 20) out.push_back(&s.payload);
    if (Named* c = n.Controller("StaticEmit"))
        if (c->payload.size() >= 20) out.push_back(&c->payload);
}
std::uint32_t Events(std::uint32_t duration, std::uint32_t interval) {
    if (interval == 0) return 0;
    return (duration + interval - 1) / interval;
}
std::uint32_t RoundU(double v, std::uint32_t lo) {
    if (!(v > 0)) return lo;
    double r = std::floor(v + 0.5);
    if (r > 4.0e9) r = 4.0e9;
    return std::max(lo, static_cast<std::uint32_t>(r));
}

// time stretch of one command record (factor f = new / old time); values of rate commands x k (= 1 / f)
bool StretchTimes(Source& s, double f) {
    float a = s.Time(0), b = s.Time(1), c = s.Time(2);
    if (!Finite(a) || !Finite(b) || !Finite(c)) return false;
    bool ch = false;
    if (!(s.mode & 1)) {   // start floored: a command never starts later than its scaled time
        float na = static_cast<float>(std::floor(std::trunc(a) * f + 1e-9));
        if (na != a) { s.SetTime(0, na); ch = true; }
    }
    if (!((s.mode >> 1) & 1)) {
        float nb = static_cast<float>(b * f);
        if (nb != b) { s.SetTime(1, nb); ch = true; }
    }
    int em = (s.mode >> 2) % 5;
    if (em == 0 || em == 2) {
        float nc = static_cast<float>(std::trunc(c) * f);
        if (em == 0 && !(s.mode & 1)) {
            // keep the end relative to the (rounded) start: end' = start' + (end - start) * f
            double st = std::trunc(a), en = std::trunc(c);
            nc = static_cast<float>(std::floor(st * f + 1e-9) + (en - st) * f);
        }
        if (nc != c) { s.SetTime(2, nc); ch = true; }
    }
    return ch;
}
int ScaleRateValues(Source& s, double k, EditReport* rep) {
    Bytes& p = s.payload;
    const std::string& c = s.command;
    if ((c == "Force" || c == "SetVelocity") && p.size() >= 12) {
        for (int i = 0; i < 3; ++i) MulF32(p, static_cast<std::size_t>(i) * 4, k);
        return 1;
    }
    if ((c == "ConeForce" || c == "SetConeVel") && p.size() >= 24) {
        for (std::size_t o : {0u, 4u, 12u, 16u}) MulF32(p, o, k);
        return 1;
    }
    if (c == "Attraction" && p.size() >= 4) { MulF32(p, 0, k); return 1; }
    std::size_t mat = SIZE_MAX;
    if (c == "SetRVelocity" && p.size() >= 76) {
        for (int i = 0; i < 3; ++i) MulF32(p, static_cast<std::size_t>(i) * 4, k);
        mat = 12;
    } else if ((c == "SetRVelocityAxis" || c == "SetShapeRotVel") && p.size() >= 80) {
        MulF32(p, 12, k);
        mat = 16;
    } else if (c == "SetRVelocityMat" && p.size() >= 64) {
        mat = 0;
    }
    if (mat != SIZE_MAX) {
        if (!ScaleRotationAngle(p, mat, k) && rep) rep->notes.push_back(c + ": matrix is not a pure rotation, kept");
        return 1;
    }
    return 0;
}

std::string LowerA(const std::string& s) {
    std::string r = s;
    for (char& ch : r)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    for (char& ch : r)
        if (ch == '/') ch = '\\';
    return r;
}

double NiceUpper(double v) {
    static const double k[] = {1, 2, 4, 5, 10, 15, 20, 25, 30, 40, 50, 60, 70, 80, 100, 120, 150, 200, 250, 300, 400,
                               500, 600, 700, 800, 1000, 1500, 2000, 3000, 5000, 10000};
    for (double x : k)
        if (v <= x) return x;
    return std::ceil(v);
}

std::uint8_t ToByte(double v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v * 255.0), 0L, 255L)); }

struct Rgb { double r, g, b; };
void RgbToHsv(double r, double g, double b, double& h, double& s, double& v) {
    double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0;
    if (d <= 0) { h = 0; return; }
    if (mx == r) h = 60.0 * std::fmod((g - b) / d, 6.0);
    else if (mx == g) h = 60.0 * ((b - r) / d + 2.0);
    else h = 60.0 * ((r - g) / d + 4.0);
    if (h < 0) h += 360.0;
}
Rgb HsvToRgb(double h, double s, double v) {
    double c = v * s, hp = std::fmod(h, 360.0) / 60.0;
    if (hp < 0) hp += 6.0;
    double x = c * (1 - std::fabs(std::fmod(hp, 2.0) - 1)), m = v - c;
    double r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else { r = c; b = x; }
    return {r + m, g + m, b + m};
}

}  // namespace

// ================================================================================================ colour
void AdjustBgra(std::uint8_t* p, const ColorAdjust& a) {
    if (a.IsIdentity()) return;
    double h, s, v;
    RgbToHsv(p[2] / 255.0, p[1] / 255.0, p[0] / 255.0, h, s, v);
    if (a.colorize) {
        h = std::fmod(a.hueShift, 360.0);
        if (h < 0) h += 360.0;
        s = std::clamp(a.saturation, 0.0, 1.0);
    } else {
        h = std::fmod(h + a.hueShift, 360.0);
        if (h < 0) h += 360.0;
        s = std::clamp(s * a.saturation, 0.0, 1.0);
    }
    v = std::clamp(v * a.brightness, 0.0, 1.0);
    Rgb c = HsvToRgb(h, s, v);
    p[2] = ToByte(c.r);
    p[1] = ToByte(c.g);
    p[0] = ToByte(c.b);
}
void AdjustRgba(std::uint8_t* p, const ColorAdjust& a) {
    std::uint8_t q[4] = {p[2], p[1], p[0], p[3]};
    AdjustBgra(q, a);
    p[0] = q[2];
    p[1] = q[1];
    p[2] = q[0];
}

int Recolor(Doc& d, const ColorAdjust& a, const NodeList& nodes, EditReport* rep, bool addWhereMissing) {
    if (a.IsIdentity()) return 0;
    auto all = NodesPreOrder(d);
    int total = 0;
    for (int i = 0; i < static_cast<int>(all.size()); ++i) {
        if (!Sel(nodes, i)) continue;
        Node& n = *all[static_cast<std::size_t>(i)];
        int c = 0;
        if (Named* g = n.Global("BlendDiffuseGraph")) c += AdjustBlendBgra(g->payload, 0, a);
        if (Named* ctl = n.Controller("DiffuseGraph")) {
            std::size_t off = BlendSize(ctl->payload, 0, 1);
            if (off) c += AdjustBlendBgra(ctl->payload, off, a);
        }
        ForEachProgramSource(n, [&](Source& s) {
            if (s.command == "SetGraphDiffuse") c += AdjustDiffusePayload(s.payload, a);
        });
        if (addWhereMissing && Drawn(n) && !HasBaked(n, "SetGraphDiffuse")) {
            std::uint8_t w[4] = {255, 255, 255, 255};
            AdjustBgra(w, a);
            if (w[0] != 255 || w[1] != 255 || w[2] != 255) {
                Bytes pl(8);
                PutU32(pl, 0, 1);
                std::memcpy(pl.data() + 4, w, 4);
                n.baked.push_back(MakeSource("SetGraphDiffuse", 0, 0.0f, 1.0f, 0.0f, std::move(pl)));
                ++c;
                if (rep) rep->notes.push_back("node '" + n.name + "': birth SetGraphDiffuse added");
            }
        }
        if (c && rep) ++rep->nodes;
        total += c;
    }
    if (rep) rep->values += total;
    return total;
}

std::vector<ColorKey> GetColorKeys(const Node& n) {
    std::vector<ColorKey> keys;
    if (const Named* g = n.Global("BlendDiffuseGraph")) {
        const Bytes& p = g->payload;
        if (BlendSize(p, 0, 4)) {
            std::uint32_t c = GetU32(p, 8);
            for (std::uint32_t i = 0; i < c; ++i) {
                const std::uint8_t* q = &p[12 + i * 8 + 4];
                keys.push_back({GetF32(p, 12 + i * 8), q[2], q[1], q[0], q[3]});
            }
            if (!keys.empty()) return keys;
        }
    }
    if (const Named* ctl = n.Controller("DiffuseGraph")) {
        const Bytes& p = ctl->payload;
        std::size_t off = BlendSize(p, 0, 1);
        if (off && BlendSize(p, off, 4)) {
            std::uint32_t c = GetU32(p, off + 8);
            for (std::uint32_t i = 0; i < c; ++i) {
                const std::uint8_t* q = &p[off + 12 + i * 8 + 4];
                keys.push_back({GetF32(p, off + 12 + i * 8), q[2], q[1], q[0], q[3]});
            }
            if (!keys.empty()) return keys;
        }
    }
    for (const auto& s : n.baked)
        if (s.has && s.command == "SetGraphDiffuse" && s.payload.size() >= 4) {
            std::uint32_t c = GetU32(s.payload, 0);
            if (4 + static_cast<std::uint64_t>(c) * 4 > s.payload.size()) break;
            for (std::uint32_t i = 0; i < c; ++i) {
                const std::uint8_t* q = &s.payload[4 + i * 4];
                keys.push_back({c > 1 ? static_cast<float>(i) / static_cast<float>(c - 1) : 0.0f, q[2], q[1], q[0], q[3]});
            }
            break;
        }
    return keys;
}

namespace {
void SampleKeys(const std::vector<ColorKey>& k, double t, double out[4]) {
    if (k.empty()) { out[0] = out[1] = out[2] = out[3] = 255; return; }
    auto put = [&](const ColorKey& c) { out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = c.a; };
    if (k.size() == 1 || t <= k.front().t) { put(k.front()); return; }
    if (t >= k.back().t) { put(k.back()); return; }
    for (std::size_t i = 1; i < k.size(); ++i)
        if (t <= k[i].t) {
            double t0 = k[i - 1].t, t1 = k[i].t, u = t1 - t0 > 1e-9 ? (t - t0) / (t1 - t0) : 0.0;
            const ColorKey &a = k[i - 1], &b = k[i];
            out[0] = a.r + (b.r - a.r) * u;
            out[1] = a.g + (b.g - a.g) * u;
            out[2] = a.b + (b.b - a.b) * u;
            out[3] = a.a + (b.a - a.a) * u;
            return;
        }
    put(k.back());
}
std::uint8_t B8(double v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)); }
Bytes EncodeBlendBgra(const std::vector<ColorKey>& keys, const std::vector<ColorKey>* alphaFrom) {
    Bytes p(12 + keys.size() * 8);
    PutF32(p, 0, keys.empty() ? 0.0f : keys.front().t);
    PutF32(p, 4, keys.empty() ? 0.0f : keys.back().t);
    PutU32(p, 8, static_cast<std::uint32_t>(keys.size()));
    for (std::size_t i = 0; i < keys.size(); ++i) {
        PutF32(p, 12 + i * 8, keys[i].t);
        std::uint8_t* q = &p[12 + i * 8 + 4];
        q[0] = keys[i].b; q[1] = keys[i].g; q[2] = keys[i].r;
        double s[4];
        if (alphaFrom) SampleKeys(*alphaFrom, keys[i].t, s);
        q[3] = alphaFrom ? B8(s[3]) : keys[i].a;
    }
    return p;
}
}  // namespace

bool SetColorKeys(Node& n, const std::vector<ColorKey>& keysIn, bool keepAlpha, EditReport* rep) {
    if (keysIn.empty()) return false;
    std::vector<ColorKey> keys = keysIn;
    std::stable_sort(keys.begin(), keys.end(), [](const ColorKey& a, const ColorKey& b) { return a.t < b.t; });
    std::vector<ColorKey> old = GetColorKeys(n);
    const std::vector<ColorKey>* alphaFrom = keepAlpha ? &old : nullptr;
    if (keepAlpha && old.empty()) {
        static const std::vector<ColorKey> white{ColorKey{}};
        alphaFrom = &white;
    }
    int changed = 0;
    if (Named* g = n.Global("BlendDiffuseGraph")) {
        Bytes p = EncodeBlendBgra(keys, alphaFrom);
        if (p != g->payload) { g->payload = std::move(p); ++changed; }
    }
    if (Named* ctl = n.Controller("DiffuseGraph")) {
        std::size_t off = BlendSize(ctl->payload, 0, 1);
        if (off && BlendSize(ctl->payload, off, 4)) {
            Bytes alpha(ctl->payload.begin(), ctl->payload.begin() + static_cast<std::ptrdiff_t>(off));
            if (!keepAlpha) {
                alpha.assign(12 + keys.size() * 5, 0);
                PutF32(alpha, 0, keys.front().t);
                PutF32(alpha, 4, keys.back().t);
                PutU32(alpha, 8, static_cast<std::uint32_t>(keys.size()));
                for (std::size_t i = 0; i < keys.size(); ++i) {
                    PutF32(alpha, 12 + i * 5, keys[i].t);
                    alpha[12 + i * 5 + 4] = keys[i].a;
                }
            }
            Bytes col = EncodeBlendBgra(keys, alphaFrom);
            Bytes p = alpha;
            p.insert(p.end(), col.begin(), col.end());
            p.insert(p.end(), ctl->payload.begin() + static_cast<std::ptrdiff_t>(off + BlendSize(ctl->payload, off, 4)),
                     ctl->payload.end());
            if (p != ctl->payload) { ctl->payload = std::move(p); ++changed; }
        }
    }
    bool any = false;
    ForEachProgramSource(n, [&](Source& s) {
        if (s.command != "SetGraphDiffuse" || s.payload.size() < 4) return;
        std::uint32_t c = GetU32(s.payload, 0);
        if (4 + static_cast<std::uint64_t>(c) * 4 > s.payload.size()) return;
        any = true;
        for (std::uint32_t i = 0; i < c; ++i) {
            double v[4];
            SampleKeys(keys, c > 1 ? static_cast<double>(i) / (c - 1) : 0.0, v);
            std::uint8_t* q = &s.payload[4 + i * 4];
            std::uint8_t nq[4] = {B8(v[2]), B8(v[1]), B8(v[0]), keepAlpha ? q[3] : B8(v[3])};
            if (std::memcmp(nq, q, 4) != 0) { std::memcpy(q, nq, 4); ++changed; }
        }
    }, false);
    if (!any && Drawn(n)) {   // no bake yet: one entry per life tick (window 0,1,100 mode 5 = every tick, k = age)
        std::uint32_t L = std::max<std::uint32_t>(1, std::min<std::uint32_t>(n.lifeFrames, 4096));
        Bytes pl(4 + L * 4);
        PutU32(pl, 0, L);
        for (std::uint32_t i = 0; i < L; ++i) {
            double v[4];
            SampleKeys(keys, L > 1 ? static_cast<double>(i) / (L - 1) : 0.0, v);
            std::uint8_t* q = &pl[4 + i * 4];
            q[0] = B8(v[2]); q[1] = B8(v[1]); q[2] = B8(v[0]); q[3] = keepAlpha ? 255 : B8(v[3]);
        }
        n.baked.push_back(MakeSource("SetGraphDiffuse", 5, 0.0f, 1.0f, 100.0f, std::move(pl)));
        ++changed;
        if (rep) rep->notes.push_back("node '" + n.name + "': per-tick SetGraphDiffuse added");
    }
    if (rep) {
        rep->values += changed;
        if (changed) ++rep->nodes;
    }
    return changed > 0;
}

// ================================================================================================ size / time
bool ScaleEffect(Doc& d, double k, std::string& err, bool upgradeVersion, EditReport* rep) {
    if (!(k > 0) || !std::isfinite(k) || k > 1000) { err = "scale must be > 0"; return false; }
    if (k == 1.0) return true;
    if (!d.HasRootScale()) {
        if (!upgradeVersion) { err = "JMXVEFF " + d.version + " has no root scale (allow the 0012 header)"; return false; }
        if (rep) rep->notes.push_back("header " + d.version + " -> 0012 (root scale added)");
        d.version = "0012";
        d.SetRootScale(static_cast<float>(k));
    } else {
        float s = d.RootScale();
        if (!std::isfinite(s) || s <= 0) s = 1.0f;
        d.SetRootScale(static_cast<float>(s * k));
    }
    if (rep) ++rep->values;
    return true;
}

int ScaleParticles(Doc& d, double k, const NodeList& nodes, EditReport* rep, bool addWhereMissing) {
    if (!(k > 0) || !std::isfinite(k) || k == 1.0) return 0;
    auto all = NodesPreOrder(d);
    int total = 0;
    for (int i = 0; i < static_cast<int>(all.size()); ++i) {
        if (!Sel(nodes, i)) continue;
        Node& n = *all[static_cast<std::size_t>(i)];
        int c = 0;
        ForEachProgramSource(n, [&](Source& s) {
            if (s.command != "SetGraphScale" || s.payload.size() < 4) return;
            std::uint64_t cnt = GetU32(s.payload, 0);
            if (4 + cnt * 12 > s.payload.size()) return;
            for (std::uint64_t j = 0; j < cnt * 3; ++j) MulF32(s.payload, 4 + static_cast<std::size_t>(j) * 4, k);
            ++c;
        });
        if (Named* g = n.Global("BlendScaleGraph"); g && BlendSize(g->payload, 0, 12)) {
            std::uint32_t cnt = GetU32(g->payload, 8);
            for (std::uint32_t j = 0; j < cnt; ++j)
                for (int a = 0; a < 3; ++a) MulF32(g->payload, 12 + j * 16 + 4 + static_cast<std::size_t>(a) * 4, k);
            ++c;
        }
        if (Named* ctl = n.Controller("ScaleGraph")) {
            Bytes& p = ctl->payload;
            std::size_t off = 0;
            double top = 0;
            bool ok = true;
            for (int ax = 0; ax < 3 && ok; ++ax) {
                std::size_t sz = BlendSize(p, off, 4);
                if (!sz) { ok = false; break; }
                std::uint32_t cnt = GetU32(p, off + 8);
                for (std::uint32_t j = 0; j < cnt; ++j) {
                    MulF32(p, off + 12 + j * 8 + 4, k);
                    top = std::max(top, static_cast<double>(GetF32(p, off + 12 + j * 8 + 4)));
                }
                off += sz;
            }
            if (ok && off + 8 <= p.size() && top > GetF32(p, off + 4)) PutF32(p, off + 4, static_cast<float>(NiceUpper(top)));
            if (ok) ++c;
        }
        if (addWhereMissing && Drawn(n) && !HasBaked(n, "SetGraphScale") && !HasBaked(n, "SetGraphRandomScale")) {
            Bytes pl(16);
            PutU32(pl, 0, 1);
            for (int a = 0; a < 3; ++a) PutF32(pl, 4 + static_cast<std::size_t>(a) * 4, static_cast<float>(k));
            n.baked.push_back(MakeSource("SetGraphScale", 0, 0.0f, 1.0f, 0.0f, std::move(pl)));
            ++c;
            if (rep) rep->notes.push_back("node '" + n.name + "': birth SetGraphScale added");
        }
        if (c && rep) ++rep->nodes;
        total += c;
    }
    if (rep) rep->values += total;
    return total;
}

int ChangeSpeed(Doc& d, double k, const NodeList& nodes, EditReport* rep) {
    if (!(k > 0) || !std::isfinite(k) || k == 1.0) return 0;
    k = std::clamp(k, 0.05, 20.0);
    double f = 1.0 / k;
    auto all = NodesPreOrder(d);
    int total = 0;
    for (int i = 0; i < static_cast<int>(all.size()); ++i) {
        if (!Sel(nodes, i)) continue;
        Node& n = *all[static_cast<std::size_t>(i)];
        int c = 0;
        std::uint32_t oldL = n.lifeFrames, newL = oldL;
        if (oldL > 1) newL = std::min<std::uint32_t>(RoundU(oldL * f, 1), 1000000u);
        if (newL != oldL) { n.lifeFrames = newL; ++c; }
        std::vector<Bytes*> emits;
        EmitPayloads(n, emits);
        for (Bytes* p : emits) {
            std::uint32_t start = GetU32(*p, 0), dur = GetU32(*p, 4), itv = GetU32(*p, 8);
            float per = GetF32(*p, 16);
            std::uint32_t nStart = RoundU(start * f, 0), nDur = dur, nItv = itv;
            if (dur > 0 && dur < kInfiniteTicks) nDur = RoundU(dur * f, 1);
            float nPer = per;
            if (itv > 0) {
                nItv = RoundU(itv * f, 1);
                if (dur > 0 && dur < kInfiniteTicks) {
                    std::uint32_t e0 = Events(dur, itv), e1 = Events(nDur, nItv);
                    if (e1 > 0 && e0 != e1) nPer = static_cast<float>(per * static_cast<double>(e0) / e1);
                } else {
                    double rate = nItv / (itv * f);
                    if (rate != 1.0) nPer = static_cast<float>(per * rate);
                }
            }
            if (nStart != start) { PutU32(*p, 0, nStart); ++c; }
            if (nDur != dur) { PutU32(*p, 4, nDur); ++c; }
            if (nItv != itv) { PutU32(*p, 8, nItv); ++c; }
            if (nPer != per && std::isfinite(per)) { PutF32(*p, 16, nPer); ++c; }
        }
        ForEachProgramSource(n, [&](Source& s) {
            if (!IsProgramCommand(s.command)) return;
            Source before = s;
            if (StretchTimes(s, f)) ++c;
            c += ScaleRateValues(s, k, rep);
            int len = ArrayLen(s);
            if (len > 0) {
                int n0 = FiringCount(before, oldL), n1 = FiringCount(s, newL);
                if (n0 == len && n1 > 0 && n1 != n0 && ResampleArray(s, n1)) ++c;
            }
        });
        if (c && rep) ++rep->nodes;
        total += c;
    }
    if (rep) rep->values += total;
    return total;
}

int ChangeDuration(Doc& d, double k, const NodeList& nodes, EditReport* rep) {
    if (!(k > 0) || !std::isfinite(k) || k == 1.0) return 0;
    k = std::clamp(k, 0.05, 20.0);
    auto all = NodesPreOrder(d);
    int total = 0;
    for (int i = 0; i < static_cast<int>(all.size()); ++i) {
        if (!Sel(nodes, i)) continue;
        Node& n = *all[static_cast<std::size_t>(i)];
        int c = 0;
        std::uint32_t oldL = n.lifeFrames;
        if (!n.children.empty() && oldL > 1) {
            std::uint32_t newL = std::min<std::uint32_t>(RoundU(oldL * k, 1), 1000000u);
            if (newL != oldL) {
                double delta = static_cast<double>(newL) - static_cast<double>(oldL);
                ForEachProgramSource(n, [&](Source& s) {
                    if (!IsProgramCommand(s.command)) return;
                    Source before = s;
                    float a = s.Time(0), cc = s.Time(2);
                    if (!Finite(a) || !Finite(cc)) return;
                    int em = (s.mode >> 2) % 5;
                    double start = (s.mode & 1) ? oldL * std::trunc(a / 100.0) : std::trunc(a);
                    double end = em == 0 ? std::trunc(cc) : em == 2 ? start + std::trunc(cc) : -1;
                    if ((em == 0 || em == 2) && end >= oldL - 1.0) {
                        s.SetTime(2, static_cast<float>(std::trunc(cc) + delta));
                        ++c;
                    }
                    int len = ArrayLen(s);
                    if (len > 0) {
                        int n0 = FiringCount(before, oldL), n1 = FiringCount(s, newL);
                        if (n0 == len && n1 > 0 && n1 != n0 && ResampleArray(s, n1)) ++c;
                    }
                }, true);
                n.lifeFrames = newL;
                ++c;
            }
        }
        std::vector<Bytes*> emits;
        EmitPayloads(n, emits);
        for (Bytes* p : emits) {
            std::uint32_t dur = GetU32(*p, 4), itv = GetU32(*p, 8);
            if (itv > 0 && dur > itv && dur < kInfiniteTicks) {
                std::uint32_t nd = RoundU(dur * k, 1);
                if (nd != dur) { PutU32(*p, 4, nd); ++c; }
            }
        }
        if (c && rep) ++rep->nodes;
        total += c;
    }
    if (rep) rep->values += total;
    return total;
}

// ================================================================================================ resources
std::vector<ResUse> ListResources(const Doc& d) {
    std::vector<ResUse> out;
    auto add = [&](const std::string& p, bool mesh, int node) {
        if (p.empty()) return;
        for (auto& u : out)
            if (u.mesh == mesh && LowerA(u.path) == LowerA(p)) {
                if (std::find(u.nodes.begin(), u.nodes.end(), node) == u.nodes.end()) u.nodes.push_back(node);
                return;
            }
        out.push_back({p, mesh, {node}});
    };
    auto all = NodesPreOrder(d);
    for (int i = 0; i < static_cast<int>(all.size()); ++i)
        for (const auto& m : all[static_cast<std::size_t>(i)]->res.meshes) {
            add(m.mesh, true, i);
            for (const auto& t : m.textures) add(t, false, i);
        }
    return out;
}

namespace {
int SwapIn(Resource& r, const std::string& from, const std::string& to, bool mesh) {
    int c = 0;
    std::string lf = LowerA(from);
    for (auto& m : r.meshes) {
        if (mesh) {
            if (!m.mesh.empty() && (from.empty() || LowerA(m.mesh) == lf) && m.mesh != to) { m.mesh = to; ++c; }
        } else {
            for (auto& t : m.textures)
                if (!t.empty() && (from.empty() || LowerA(t) == lf) && t != to) { t = to; ++c; }
        }
    }
    return c;
}
int SwapRes(Doc& d, const std::string& from, const std::string& to, const NodeList& nodes, EditReport* rep, bool mesh) {
    if (to.empty()) return 0;
    auto all = NodesPreOrder(d);
    int total = 0;
    for (int i = 0; i < static_cast<int>(all.size()); ++i) {
        if (!Sel(nodes, i)) continue;
        Node& n = *all[static_cast<std::size_t>(i)];
        int c = SwapIn(n.res, from, to, mesh);
        if (Named* sh = n.Controller("Shape")) {
            std::string render;
            Resource r;
            if (DecodeShape(sh->payload, render, r)) {
                int cs = SwapIn(r, from, to, mesh);
                if (cs) { sh->payload = EncodeShape(render, r); c += cs; }
            }
        }
        if (c && rep) ++rep->nodes;
        total += c;
    }
    if (rep) rep->values += total;
    return total;
}
}  // namespace

int SwapTexture(Doc& d, const std::string& from, const std::string& to, const NodeList& nodes, EditReport* rep) {
    return SwapRes(d, from, to, nodes, rep, false);
}
int SwapMesh(Doc& d, const std::string& from, const std::string& to, const NodeList& nodes, EditReport* rep) {
    return SwapRes(d, from, to, nodes, rep, true);
}

// ================================================================================================ inspection
bool GetStaticEmit(const Node& n, StaticEmit& e) {
    for (const auto& s : n.emitList)
        if (s.has && s.command == "StaticEmit" && s.payload.size() >= 20) {
            e.start = GetU32(s.payload, 0);
            e.duration = GetU32(s.payload, 4);
            e.interval = GetU32(s.payload, 8);
            e.maxAlive = GetU32(s.payload, 12);
            e.perEvent = GetF32(s.payload, 16);
            return true;
        }
    return false;
}

std::vector<NodeInfo> DescribeNodes(const Doc& d) {
    std::vector<NodeInfo> out;
    std::function<void(const Node&, int, int, const std::string&)> walk = [&](const Node& n, int depth, int parent,
                                                                             const std::string& prefix) {
        NodeInfo ni;
        ni.index = static_cast<int>(out.size());
        ni.depth = depth;
        ni.parent = parent;
        ni.name = n.name;
        ni.path = prefix.empty() ? n.name : prefix + "/" + n.name;
        ni.lifeFrames = n.lifeFrames;
        ni.life = n.life.has ? n.life.command : "NormalTimeExtinct";
        ni.view = n.view.has ? n.view.command : "ViewNone";
        ni.render = n.render.has ? n.render.command : "RenderNone";
        for (const auto& m : n.res.meshes) {
            if (ni.mesh.empty() && !m.mesh.empty()) ni.mesh = m.mesh;
            if (ni.texture.empty() && !m.textures.empty()) ni.texture = m.textures[0];
        }
        ni.hasEmit = GetStaticEmit(n, ni.emit);
        ni.colourKeys = static_cast<int>(GetColorKeys(n).size());
        ni.diffuse = HasBaked(n, "SetGraphDiffuse");
        ni.scaleBake = HasBaked(n, "SetGraphScale") || HasBaked(n, "SetGraphRandomScale");
        for (const auto& s : n.baked)
            if (s.has && IsProgramCommand(s.command)) ++ni.commands;
        int me = ni.index;
        std::string path = ni.path;
        out.push_back(std::move(ni));
        for (const auto& c : n.children) walk(c, depth + 1, me, path);
    };
    walk(d.root, 0, -1, "");
    return out;
}

bool FiringWindow(std::uint8_t mode, float a, float b, float c, std::uint32_t L, double& start, double& step, double& end) {
    if (L < 1 || !Finite(a) || !Finite(b) || !Finite(c)) return false;
    start = (mode & 1) ? L * std::trunc(a / 100.0) : std::trunc(static_cast<double>(a));
    step = ((mode >> 1) & 1) ? L * static_cast<double>(b) / 100.0 : static_cast<double>(b);
    int em = (mode >> 2) % 5;
    if (em == 0) end = std::trunc(static_cast<double>(c));
    else if (em == 1) end = L * std::trunc(c / 100.0);
    else if (em == 2) end = start + std::trunc(static_cast<double>(c));
    else if (em == 3) end = start - L * std::trunc(c / -100.0);
    else {
        if (c < 1.0f) return false;
        end = start + std::trunc(c * step);
    }
    if (end >= L) end = L - 1.0;
    if (end > L || start > end || step < 1e-6) return false;
    return true;
}

int FiringCount(const Source& s, std::uint32_t L) {
    double start, step, end;
    if (!s.has || !FiringWindow(s.mode, s.Time(0), s.Time(1), s.Time(2), L, start, step, end)) return 0;
    float t = static_cast<float>(start), st = static_cast<float>(step);
    int k = 0;
    while (static_cast<double>(t) <= end && k < 4096) {
        t = t + st;
        ++k;
    }
    return k;
}

bool Validate(const Doc& d, std::vector<std::string>& problems) {
    Bytes a = Serialize(d);
    Doc back;
    std::string err;
    if (!Parse(a, back, err)) {
        problems.push_back("serialized document does not parse: " + err);
        return false;
    }
    if (Serialize(back) != a) problems.push_back("serialize -> parse -> serialize is not stable");
    auto all = NodesPreOrder(d);
    for (const Node* n : all) {
        for (const auto& s : n->baked)
            if (s.has && s.unknown) problems.push_back("node '" + n->name + "': unknown command '" + s.command + "'");
    }
    return problems.empty();
}

}  // namespace se::efpw
