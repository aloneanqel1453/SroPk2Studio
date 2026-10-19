// SkillEffect Studio - render/SoftRaster.cpp
#include "SoftRaster.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace se::render {

// ------------------------------------------------------------------------------------------------ camera
void RCamera::Setup() {
    z = Norm(at - eye);
    x = Norm(Cross(V3{0, 1, 0}, z));
    if (Len(x) < 1e-9) x = {1, 0, 0};
    y = Cross(z, x);
    f = 1.0 / std::tan(fovDeg * kPi / 180.0 / 2.0);
}

void RCamera::Project(const V3& P, double& sx, double& sy, double& w) const {
    V3 v = ToView(P);
    w = v.z;
    double ws = w > 1e-3 ? w : 1e-3;
    sx = (v.x / ws * f * H / W + 1) * 0.5 * W;
    sy = (1 - v.y / ws * f) * 0.5 * H;
}

M4 RCamera::InvViewRot() const {
    M4 m;
    m.SetRow(0, x); m.SetRow(1, y); m.SetRow(2, z);
    return m;
}

bool RState::operator==(const RState& o) const {
    return cull == o.cull && src == o.src && dst == o.dst && !std::memcmp(cs, o.cs, sizeof(cs)) &&
           !std::memcmp(as, o.as, sizeof(as)) && ztest == o.ztest && zwrite == o.zwrite && alphaRef == o.alphaRef &&
           lit == o.lit;
}

RBatch& RFrame::Batch(const RState& st, const std::shared_ptr<RTexture>& tex) {
    if (batches.empty() || !(batches.back().st == st) || batches.back().tex != tex) {
        batches.emplace_back();
        batches.back().st = st;
        batches.back().tex = tex;
    }
    return batches.back();
}

// ------------------------------------------------------------------------------------------------ helpers
namespace {

inline void Arg4(std::uint32_t code, const double t[4], const double d[4], double x[4]) {
    std::uint32_t base = code & 0x0F;
    if (base == 2) std::memcpy(x, t, sizeof(double) * 4);
    else if (base <= 1) std::memcpy(x, d, sizeof(double) * 4);
    else if (base == 3) { x[0] = x[1] = x[2] = x[3] = 1.0; }   // TFACTOR inherited, assumed white
    else { x[0] = x[1] = x[2] = x[3] = 0.0; }
}
inline void ArgRgb(std::uint32_t code, const double t[4], const double d[4], double o[3]) {
    double x[4];
    Arg4(code, t, d, x);
    if (code & 0x20) x[0] = x[1] = x[2] = x[3];
    if (code & 0x10) for (double& c : x) c = 1.0 - c;
    o[0] = x[0]; o[1] = x[1]; o[2] = x[2];
}
inline double ArgA(std::uint32_t code, const double t[4], const double d[4]) {
    double x[4];
    Arg4(code, t, d, x);
    if (code & 0x10) return 1.0 - x[3];
    return x[3];
}
inline double Top(std::uint32_t op, double a, double b, double fallback) {
    switch (op) {
        case 2: return a;
        case 3: return b;
        case 4: return a * b;
        case 5: return a * b * 2;
        case 6: return a * b * 4;
        case 7: return a + b;
        case 8: return a + b - 0.5;
        case 9: return (a + b - 0.5) * 2;
        case 10: return a - b;
        case 11: return a + b - a * b;
        default: return fallback;
    }
}
inline double Clip(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline void Factor(std::uint32_t code, const double src[3], double sa, const double dst[3], double f[3]) {
    for (int i = 0; i < 3; ++i) {
        switch (code) {
            case 1: f[i] = 0; break;
            case 2: f[i] = 1; break;
            case 3: f[i] = src[i]; break;
            case 4: f[i] = 1 - src[i]; break;
            case 5: f[i] = sa; break;
            case 6: f[i] = 1 - sa; break;
            case 7: f[i] = 1; break;
            case 8: f[i] = 0; break;
            case 9: f[i] = dst[i]; break;
            case 10: f[i] = 1 - dst[i]; break;
            case 11: f[i] = 0; break;
            default: f[i] = 1; break;
        }
    }
}
inline std::int64_t PyMod(std::int64_t a, std::int64_t m) { std::int64_t r = a % m; return r < 0 ? r + m : r; }

inline void Sample(const RTexture* tex, double u, double v, double t[4]) {
    const int tw = tex->w, th = tex->h;
    double fx = u * tw - 0.5, fy = v * th - 0.5;
    double flx = std::floor(fx), fly = std::floor(fy);
    std::int64_t ix = static_cast<std::int64_t>(flx), iy = static_cast<std::int64_t>(fly);
    double ax = fx - static_cast<double>(ix), ay = fy - static_cast<double>(iy);
    std::int64_t ix0 = PyMod(ix, tw), iy0 = PyMod(iy, th), ix1 = PyMod(ix + 1, tw), iy1 = PyMod(iy + 1, th);
    const float* p00 = &tex->rgba[(static_cast<std::size_t>(iy0) * tw + ix0) * 4];
    const float* p01 = &tex->rgba[(static_cast<std::size_t>(iy0) * tw + ix1) * 4];
    const float* p10 = &tex->rgba[(static_cast<std::size_t>(iy1) * tw + ix0) * 4];
    const float* p11 = &tex->rgba[(static_cast<std::size_t>(iy1) * tw + ix1) * 4];
    for (int k = 0; k < 4; ++k)
        t[k] = static_cast<double>(p00[k]) * (1 - ax) * (1 - ay) + static_cast<double>(p01[k]) * ax * (1 - ay) +
               static_cast<double>(p10[k]) * (1 - ax) * ay + static_cast<double>(p11[k]) * ax * ay;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ frame
void SoftRaster::Begin(const RFrame& f) {
    W = f.cam.W;
    H = f.cam.H;
    m_fb.assign(static_cast<std::size_t>(W) * H * 3, 0.0);
    m_zb.assign(static_cast<std::size_t>(W) * H, 0.0f);
    for (int y = 0; y < H; ++y) {
        double c[3];
        double t = H > 1 ? static_cast<double>(y) / (H - 1) : 0.0;
        for (int k = 0; k < 3; ++k) {
            double b = f.bgGradient ? f.bg[k] * (1 - t) + f.bg2[k] * t : f.bg[k];
            c[k] = std::floor(Clip(b, 0, 1) * 255.0) / 255.0;   // efp_sim: background quantised to bytes
        }
        for (int x = 0; x < W; ++x) std::memcpy(&m_fb[(static_cast<std::size_t>(y) * W + x) * 3], c, sizeof(c));
    }
    for (const RLine& l : f.lines) {
        double ax, ay, aw, bx, by, bw;
        f.cam.Project(l.a, ax, ay, aw);
        f.cam.Project(l.b, bx, by, bw);
        if (aw > 1e-2 && bw > 1e-2)
            Line(static_cast<int>(ax), static_cast<int>(ay), static_cast<int>(bx), static_cast<int>(by), l.rgb);
    }
}

void SoftRaster::Line(int x0, int y0, int x1, int y1, const std::uint8_t rgb[3]) {   // PIL line8
    auto put = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        double* p = &m_fb[(static_cast<std::size_t>(y) * W + x) * 3];
        for (int k = 0; k < 3; ++k) p[k] = rgb[k] / 255.0;
    };
    int dx = x1 - x0, dy = y1 - y0, xs = 1, ys = 1;
    if (dx < 0) { dx = -dx; xs = -1; }
    if (dy < 0) { dy = -dy; ys = -1; }
    if (dx > 20000 || dy > 20000) return;
    if (dx == 0) { for (int i = 0; i <= dy; ++i) { put(x0, y0); y0 += ys; } }
    else if (dy == 0) { for (int i = 0; i <= dx; ++i) { put(x0, y0); x0 += xs; } }
    else if (dx > dy) {
        int n = dx; dy += dy; int e = dy - dx; dx += dx;
        for (int i = 0; i <= n; ++i) { put(x0, y0); if (e >= 0) { y0 += ys; e -= dx; } e += dy; x0 += xs; }
    } else {
        int n = dy; dx += dx; int e = dx - dy; dy += dy;
        for (int i = 0; i <= n; ++i) { put(x0, y0); if (e >= 0) { x0 += xs; e -= dy; } e += dx; y0 += ys; }
    }
}

void SoftRaster::DrawBatch(const RFrame& f, const RBatch& b, int y0, int y1) {
    const RTexture* tex = b.tex && !b.tex->rgba.empty() ? b.tex.get() : nullptr;
    for (std::size_t i = 0; i + 2 < b.v.size(); i += 3) {
        if (b.st.lit) TriLit(f.cam, &b.v[i], tex, b.st, y0, y1);
        else TriEfx(f.cam, &b.v[i], tex, b.st, y0, y1);
    }
}

void SoftRaster::End(const RFrame&, Image& out) const {
    out.w = W;
    out.h = H;
    out.rgba.assign(static_cast<std::size_t>(W) * H * 4, 255);
    for (std::size_t i = 0, n = static_cast<std::size_t>(W) * H; i < n; ++i)
        for (int k = 0; k < 3; ++k) out.rgba[i * 4 + k] = static_cast<std::uint8_t>(Clip(m_fb[i * 3 + k], 0, 1) * 255.0);
}

void SoftRaster::Draw(const RFrame& f, Image& out, int threads) {
    Begin(f);
    threads = std::max(1, std::min(threads, 16));
    if (threads == 1 || H < 64) {
        for (const RBatch& b : f.batches) DrawBatch(f, b, 0, H);
    } else {
        std::vector<std::thread> th;
        int band = (H + threads - 1) / threads;
        for (int t = 0; t < threads; ++t) {
            int y0 = t * band, y1 = std::min(H, y0 + band);
            if (y0 >= y1) break;
            th.emplace_back([this, &f, y0, y1] { for (const RBatch& b : f.batches) DrawBatch(f, b, y0, y1); });
        }
        for (auto& t : th) t.join();
    }
    End(f, out);
    DrawLabels(out, f.labels);
}

// ------------------------------------------------------------------------------------------------ triangles
void SoftRaster::TriEfx(const RCamera& cam, const RVtx* v, const RTexture* tex, const RState& st, int yb0, int yb1) {
    double S[3][2], w[3];
    for (int i = 0; i < 3; ++i) cam.Project(v[i].p, S[i][0], S[i][1], w[i]);
    const double x0 = S[0][0], y0 = S[0][1], x1 = S[1][0], y1 = S[1][1], x2 = S[2][0], y2 = S[2][1];
    const double area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (std::fabs(area) < 1e-9 || w[0] <= 1e-3 || w[1] <= 1e-3 || w[2] <= 1e-3) return;
    if (st.cull == 3 && area < 0) return;   // D3DCULL_CCW
    double mnx = std::min({x0, x1, x2}), mxx = std::max({x0, x1, x2}), mny = std::min({y0, y1, y2}), mxy = std::max({y0, y1, y2});
    if (!(mnx < 1e9 && mxx > -1e9 && mny < 1e9 && mxy > -1e9)) {
        mnx = std::max(mnx, -1e9); mxx = std::min(mxx, 1e9); mny = std::max(mny, -1e9); mxy = std::min(mxy, 1e9);
    }
    int bx0 = std::max(static_cast<int>(std::floor(mnx)), 0), bx1 = std::min(static_cast<int>(std::ceil(mxx)), W - 1);
    int by0 = std::max(static_cast<int>(std::floor(mny)), 0), by1 = std::min(static_cast<int>(std::ceil(mxy)), H - 1);
    if (bx0 > bx1 || by0 > by1) return;
    by0 = std::max(by0, yb0);
    by1 = std::min(by1, yb1 - 1);
    if (by0 > by1) return;
    const double iw0 = 1.0 / w[0], iw1 = 1.0 / w[1], iw2 = 1.0 / w[2];
    const double dif[4] = {v[0].c[0], v[0].c[1], v[0].c[2], v[0].c[3]};
    const std::uint32_t c1 = st.cs[0], c2 = st.cs[1], cop = st.cs[2], a1 = st.as[0], a2 = st.as[1], aop = st.as[2];
    std::uint32_t sb = st.src, db = st.dst;
    if (sb == 12) { sb = 5; db = 6; }
    else if (sb == 13) { sb = 6; db = 5; }
    for (int py = by0; py <= by1; ++py) {
        const double ys = py + 0.5;
        for (int px = bx0; px <= bx1; ++px) {
            const double xs = px + 0.5;
            const double l0 = ((x1 - xs) * (y2 - ys) - (x2 - xs) * (y1 - ys)) / area;
            const double l1 = ((x2 - xs) * (y0 - ys) - (x0 - xs) * (y2 - ys)) / area;
            const double l2 = 1 - l0 - l1;
            if (l0 < 0 || l1 < 0 || l2 < 0) continue;
            const double q = l0 * iw0 + l1 * iw1 + l2 * iw2;
            const std::size_t pi = static_cast<std::size_t>(py) * W + px;
            if (st.ztest && m_zb[pi] > 0 && q < m_zb[pi] * 0.99999) continue;
            double t[4] = {1, 1, 1, 1};
            if (tex) {
                const double u = (l0 * v[0].u * iw0 + l1 * v[1].u * iw1 + l2 * v[2].u * iw2) / q;
                const double vv = (l0 * v[0].v * iw0 + l1 * v[1].v * iw1 + l2 * v[2].v * iw2) / q;
                if (!std::isfinite(u) || !std::isfinite(vv)) continue;
                Sample(tex, u, vv, t);
            }
            double ra[3], rb[3], rgb[3];
            ArgRgb(c1, t, dif, ra);
            ArgRgb(c2, t, dif, rb);
            for (int k = 0; k < 3; ++k) rgb[k] = Clip(Top(cop, ra[k], rb[k], dif[k]), 0, 1);
            const double al = Clip(Top(aop, ArgA(a1, t, dif), ArgA(a2, t, dif), dif[3]), 0, 1);
            if (st.alphaRef >= 0 && al < st.alphaRef) continue;
            double* d = &m_fb[pi * 3];
            double fs[3], fd[3];
            Factor(sb, rgb, al, d, fs);
            Factor(db, rgb, al, d, fd);
            for (int k = 0; k < 3; ++k) d[k] = Clip(rgb[k] * fs[k] + d[k] * fd[k], 0, 4);
            if (st.zwrite) m_zb[pi] = static_cast<float>(q);
        }
    }
}

void SoftRaster::TriLit(const RCamera& cam, const RVtx* v, const RTexture* tex, const RState& st, int yb0, int yb1) {
    double S[3][2], w[3];
    for (int i = 0; i < 3; ++i) cam.Project(v[i].p, S[i][0], S[i][1], w[i]);
    if (w[0] <= 0.05 || w[1] <= 0.05 || w[2] <= 0.05) return;   // models: simple near reject
    const double x0 = S[0][0], y0 = S[0][1], x1 = S[1][0], y1 = S[1][1], x2 = S[2][0], y2 = S[2][1];
    const double area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (std::fabs(area) < 1e-12) return;
    if (st.cull == 3 && area < 0) return;
    if (st.cull == 2 && area > 0) return;
    int bx0 = std::max(static_cast<int>(std::floor(std::min({x0, x1, x2}))), 0);
    int bx1 = std::min(static_cast<int>(std::ceil(std::max({x0, x1, x2}))), W - 1);
    int by0 = std::max(static_cast<int>(std::floor(std::min({y0, y1, y2}))), std::max(0, yb0));
    int by1 = std::min(static_cast<int>(std::ceil(std::max({y0, y1, y2}))), std::min(H, yb1) - 1);
    if (bx0 > bx1 || by0 > by1) return;
    const double iw[3] = {1.0 / w[0], 1.0 / w[1], 1.0 / w[2]};
    std::uint32_t sb = st.src, db = st.dst;
    if (sb == 12) { sb = 5; db = 6; }
    else if (sb == 13) { sb = 6; db = 5; }
    for (int py = by0; py <= by1; ++py) {
        const double ys = py + 0.5;
        for (int px = bx0; px <= bx1; ++px) {
            const double xs = px + 0.5;
            const double l0 = ((x1 - xs) * (y2 - ys) - (x2 - xs) * (y1 - ys)) / area;
            const double l1 = ((x2 - xs) * (y0 - ys) - (x0 - xs) * (y2 - ys)) / area;
            const double l2 = 1 - l0 - l1;
            if (l0 < 0 || l1 < 0 || l2 < 0) continue;
            const double q = l0 * iw[0] + l1 * iw[1] + l2 * iw[2];
            const std::size_t pi = static_cast<std::size_t>(py) * W + px;
            if (st.ztest && m_zb[pi] > 0 && q < m_zb[pi]) continue;
            const double b0 = l0 * iw[0] / q, b1 = l1 * iw[1] / q, b2 = l2 * iw[2] / q;
            double col[4];
            for (int k = 0; k < 4; ++k) col[k] = b0 * v[0].c[k] + b1 * v[1].c[k] + b2 * v[2].c[k];
            double t[4] = {1, 1, 1, 1};
            if (tex) Sample(tex, b0 * v[0].u + b1 * v[1].u + b2 * v[2].u, b0 * v[0].v + b1 * v[1].v + b2 * v[2].v, t);
            double rgb[3], al;
            for (int k = 0; k < 3; ++k) rgb[k] = Clip(t[k] * col[k], 0, 1);
            al = Clip(t[3] * col[3], 0, 1);
            if (st.alphaRef >= 0 && al < st.alphaRef) continue;
            double* d = &m_fb[pi * 3];
            if (sb == 2 && db == 1) {
                for (int k = 0; k < 3; ++k) d[k] = rgb[k];
            } else {
                double fs[3], fd[3];
                Factor(sb, rgb, al, d, fs);
                Factor(db, rgb, al, d, fd);
                for (int k = 0; k < 3; ++k) d[k] = Clip(rgb[k] * fs[k] + d[k] * fd[k], 0, 4);
            }
            if (st.zwrite) m_zb[pi] = static_cast<float>(q);
        }
    }
}

// ------------------------------------------------------------------------------------------------ labels
void DrawLabels(Image& img, const std::vector<RFrame::Label>& labels) {
    if (labels.empty() || img.Empty()) return;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = img.w;
    bi.bmiHeader.biHeight = -img.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { DeleteDC(dc); return; }
    auto* px = static_cast<std::uint8_t*>(bits);
    for (std::size_t i = 0, n = static_cast<std::size_t>(img.w) * img.h; i < n; ++i) {
        px[i * 4 + 0] = img.rgba[i * 4 + 2]; px[i * 4 + 1] = img.rgba[i * 4 + 1]; px[i * 4 + 2] = img.rgba[i * 4 + 0]; px[i * 4 + 3] = 255;
    }
    HGDIOBJ ob = SelectObject(dc, bmp);
    HFONT font = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    for (const auto& l : labels) {
        SetTextColor(dc, RGB(l.rgb[0], l.rgb[1], l.rgb[2]));
        TextOutW(dc, l.x, l.y - 1, l.text.c_str(), static_cast<int>(l.text.size()));
    }
    GdiFlush();
    for (std::size_t i = 0, n = static_cast<std::size_t>(img.w) * img.h; i < n; ++i) {
        img.rgba[i * 4 + 0] = px[i * 4 + 2]; img.rgba[i * 4 + 1] = px[i * 4 + 1]; img.rgba[i * 4 + 2] = px[i * 4 + 0];
    }
    SelectObject(dc, of);
    SelectObject(dc, ob);
    DeleteObject(font);
    DeleteObject(bmp);
    DeleteDC(dc);
}

}  // namespace se::render
