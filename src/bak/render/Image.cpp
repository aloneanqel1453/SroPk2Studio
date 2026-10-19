// ============================================================================
//  SRO Asset Studio - render/Image.cpp
//
//  RGBA8 image operations: channel views, checkerboard composite, resampling
//  (nearest / bilinear / box), crop, flip, and the Win32 GDI interop that the
//  themed window and the CLI both use.
//
//  Resampling notes:
//    * scaledBilinear is a straight (non premultiplied) bilinear filter. That
//      is deliberate: Silkroad textures keep sheen in the alpha channel rather
//      than transparency, so premultiplying would darken armour edges.
//    * scaledBox is the one to use when SHRINKING. Bilinear only reads 2x2
//      source pixels, so shrinking a 512x512 texture into a 64 px thumbnail
//      with it throws away 98% of the image and aliases hard.
//    * scaledSmooth picks between them per call.
// ============================================================================
#include "Image.h"

#include <thread>
#include <vector>

namespace sro {

namespace {

/// Run `body(yBegin, yEnd)` over disjoint row ranges, one per core.
///
/// Every user of this writes only to the rows it was given, so the split needs
/// no locking. Small images stay single-threaded: below the cutoff the thread
/// hand-off costs more than the work itself.
template <typename Fn>
void ParallelRows(int rows, Fn body) {
    if (rows <= 0) return;
    unsigned cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 1;
    int parts = static_cast<int>(cores > 32u ? 32u : cores);
    if (rows < 64) parts = 1;
    if (parts > rows / 16) parts = rows / 16;
    if (parts < 1) parts = 1;

    if (parts == 1) { body(0, rows); return; }

    const int step = (rows + parts - 1) / parts;
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(parts) - 1);
    for (int p = 1; p < parts; ++p) {
        const int y0 = p * step;
        const int y1 = (p + 1) * step < rows ? (p + 1) * step : rows;
        if (y0 >= y1) break;
        workers.emplace_back([&body, y0, y1]() { body(y0, y1); });
    }
    body(0, step < rows ? step : rows);
    for (std::thread& t : workers) t.join();
}

} // namespace

void Image::fill(Rgba c) {
    if (!valid()) return;
    u8* p = data();
    const size_t n = static_cast<size_t>(m_width) * m_height;
    for (size_t i = 0; i < n; ++i) {
        p[i * 4 + 0] = c.r; p[i * 4 + 1] = c.g; p[i * 4 + 2] = c.b; p[i * 4 + 3] = c.a;
    }
}

Image Image::applyChannelView(ChannelView view) const {
    Image out;
    if (!valid()) return out;
    if (!out.resize(m_width, m_height)) return out;

    const u8* src = data();
    u8* dst = out.data();
    const size_t n = static_cast<size_t>(m_width) * m_height;
    for (size_t i = 0; i < n; ++i) {
        const u8 r = src[i * 4 + 0], g = src[i * 4 + 1], b = src[i * 4 + 2], a = src[i * 4 + 3];
        u8 or_ = r, og = g, ob = b, oa = 255;
        switch (view) {
            case ChannelView::RGB:   oa = 255; break;         // SRO default: ignore alpha
            case ChannelView::RGBA:  oa = a;   break;
            case ChannelView::Red:   or_ = og = ob = r; break;
            case ChannelView::Green: or_ = og = ob = g; break;
            case ChannelView::Blue:  or_ = og = ob = b; break;
            case ChannelView::Alpha: or_ = og = ob = a; break;
        }
        dst[i * 4 + 0] = or_; dst[i * 4 + 1] = og; dst[i * 4 + 2] = ob; dst[i * 4 + 3] = oa;
    }
    return out;
}

Image Image::compositeOverCheckerboard(int cellSize, Rgba light, Rgba dark) const {
    Image out;
    if (!valid()) return out;
    if (!out.resize(m_width, m_height)) return out;
    if (cellSize < 1) cellSize = 8;

    for (int y = 0; y < m_height; ++y) {
        const u8* s = row(y);
        u8* d = out.row(y);
        for (int x = 0; x < m_width; ++x) {
            const bool even = (((x / cellSize) + (y / cellSize)) & 1) == 0;
            const Rgba bg = even ? light : dark;
            const int a = s[x * 4 + 3];
            d[x * 4 + 0] = static_cast<u8>((s[x * 4 + 0] * a + bg.r * (255 - a)) / 255);
            d[x * 4 + 1] = static_cast<u8>((s[x * 4 + 1] * a + bg.g * (255 - a)) / 255);
            d[x * 4 + 2] = static_cast<u8>((s[x * 4 + 2] * a + bg.b * (255 - a)) / 255);
            d[x * 4 + 3] = 255;
        }
    }
    return out;
}

Image Image::scaledNearest(int w, int h) const {
    Image out;
    if (!valid() || w <= 0 || h <= 0) return out;
    if (!out.resize(w, h)) return out;
    for (int y = 0; y < h; ++y) {
        const int sy = (std::min)(m_height - 1, y * m_height / h);
        const u8* s = row(sy);
        u8* d = out.row(y);
        for (int x = 0; x < w; ++x) {
            const int sx = (std::min)(m_width - 1, x * m_width / w);
            std::memcpy(d + x * 4, s + sx * 4, 4);
        }
    }
    return out;
}

Image Image::scaledBilinear(int w, int h) const {
    Image out;
    if (!valid() || w <= 0 || h <= 0) return out;
    if (w == m_width && h == m_height) { out = *this; return out; }
    if (!out.resize(w, h)) return out;

    // Map destination pixel centres back into source space.
    const double sx = static_cast<double>(m_width)  / static_cast<double>(w);
    const double sy = static_cast<double>(m_height) / static_cast<double>(h);

    for (int y = 0; y < h; ++y) {
        double fy = (y + 0.5) * sy - 0.5;
        if (fy < 0.0) fy = 0.0;
        int y0 = static_cast<int>(fy);
        if (y0 > m_height - 1) y0 = m_height - 1;
        int y1 = (y0 + 1 < m_height) ? (y0 + 1) : y0;
        const double wy = fy - y0;

        const u8* r0 = row(y0);
        const u8* r1 = row(y1);
        u8* d = out.row(y);

        for (int x = 0; x < w; ++x) {
            double fx = (x + 0.5) * sx - 0.5;
            if (fx < 0.0) fx = 0.0;
            int x0 = static_cast<int>(fx);
            if (x0 > m_width - 1) x0 = m_width - 1;
            int x1 = (x0 + 1 < m_width) ? (x0 + 1) : x0;
            const double wx = fx - x0;

            for (int c = 0; c < 4; ++c) {
                const double a = r0[x0 * 4 + c] * (1.0 - wx) + r0[x1 * 4 + c] * wx;
                const double b = r1[x0 * 4 + c] * (1.0 - wx) + r1[x1 * 4 + c] * wx;
                const double v = a * (1.0 - wy) + b * wy;
                d[x * 4 + c] = static_cast<u8>(v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v + 0.5));
            }
        }
    }
    return out;
}

Image Image::scaledBox(int w, int h) const {
    Image out;
    if (!valid() || w <= 0 || h <= 0) return out;
    if (w == m_width && h == m_height) { out = *this; return out; }
    // Box only makes sense when shrinking on both axes.
    if (w > m_width || h > m_height) return scaledBilinear(w, h);
    if (!out.resize(w, h)) return out;

    // Row-parallel: every output row reads its own slab of the source and
    // writes only its own row, so the split needs no synchronisation. This runs
    // on the supersampled buffer - at 4x on a 1400x700 pane that is 15.7 M
    // source pixels every frame, which was one of the two costs left after the
    // rasteriser itself was parallelised.
    ParallelRows(h, [&](int yBegin, int yEnd) {
    for (int y = yBegin; y < yEnd; ++y) {
        int y0 = static_cast<int>(static_cast<i64>(y)     * m_height / h);
        int y1 = static_cast<int>(static_cast<i64>(y + 1) * m_height / h);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > m_height) y1 = m_height;

        u8* d = out.row(y);
        for (int x = 0; x < w; ++x) {
            int x0 = static_cast<int>(static_cast<i64>(x)     * m_width / w);
            int x1 = static_cast<int>(static_cast<i64>(x + 1) * m_width / w);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > m_width) x1 = m_width;

            u32 acc[4] = { 0, 0, 0, 0 };
            u32 n = 0;
            for (int yy = y0; yy < y1; ++yy) {
                const u8* s = row(yy);
                for (int xx = x0; xx < x1; ++xx) {
                    acc[0] += s[xx * 4 + 0];
                    acc[1] += s[xx * 4 + 1];
                    acc[2] += s[xx * 4 + 2];
                    acc[3] += s[xx * 4 + 3];
                    ++n;
                }
            }
            if (!n) n = 1;
            for (int c = 0; c < 4; ++c)
                d[x * 4 + c] = static_cast<u8>((acc[c] + n / 2) / n);
        }
    }
    });
    return out;
}

Image Image::scaledSmooth(int w, int h) const {
    if (!valid() || w <= 0 || h <= 0) return Image();
    if (w <= m_width && h <= m_height) return scaledBox(w, h);
    return scaledBilinear(w, h);
}

Image Image::cropped(int x, int y, int w, int h) const {
    Image out;
    if (!valid() || w <= 0 || h <= 0) return out;

    // Clip to the source.
    int x0 = (std::max)(0, x);
    int y0 = (std::max)(0, y);
    int x1 = (std::min)(m_width,  x + w);
    int y1 = (std::min)(m_height, y + h);
    if (x1 <= x0 || y1 <= y0) return out;

    if (!out.resize(x1 - x0, y1 - y0)) return out;
    const size_t rowBytes = static_cast<size_t>(x1 - x0) * 4;
    for (int yy = y0; yy < y1; ++yy)
        std::memcpy(out.row(yy - y0), row(yy) + static_cast<size_t>(x0) * 4, rowBytes);
    return out;
}

void Image::applyStudioPost(f32 sat, f32 con, f32 vig) {
    if (!valid()) return;
    const f32 cx  = m_width * 0.5f, cy = m_height * 0.5f;
    const f32 inv = 1.0f / std::sqrt(cx * cx + cy * cy);   // 1.0 at the corner
    const f32 off = 0.5f * (1.0f - con);                   // -0.03 at contrast 1.06
    // One sqrt and a luma per pixel over the whole frame - worth splitting.
    ParallelRows(m_height, [&](int yBegin, int yEnd) {
    for (int y = yBegin; y < yEnd; ++y) {
        u8* p = row(y);
        if (!p) continue;
        const f32 dy = (static_cast<f32>(y) + 0.5f - cy) * inv;
        for (int x = 0; x < m_width; ++x, p += 4) {
            f32 r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;
            const f32 dx = (static_cast<f32>(x) + 0.5f - cx) * inv;
            f32 d = std::sqrt(dx * dx + dy * dy) / 1.55f;  // reference fan radius
            if (d > 1.0f) d = 1.0f;
            const f32 a = vig * d;
            r += (0.00f - r) * a; g += (0.00f - g) * a; b += (0.02f - b) * a;
            const f32 l = 0.299f * r + 0.587f * g + 0.114f * b;
            r = l + (r - l) * sat; g = l + (g - l) * sat; b = l + (b - l) * sat;
            r = r * con + off;     g = g * con + off;     b = b * con + off;
            p[0] = static_cast<u8>(r <= 0.0f ? 0.0f : (r >= 1.0f ? 255.0f : r * 255.0f + 0.5f));
            p[1] = static_cast<u8>(g <= 0.0f ? 0.0f : (g >= 1.0f ? 255.0f : g * 255.0f + 0.5f));
            p[2] = static_cast<u8>(b <= 0.0f ? 0.0f : (b >= 1.0f ? 255.0f : b * 255.0f + 0.5f));
        }
    }
    });
}

void Image::flipVertical() {
    if (!valid()) return;
    const size_t rowBytes = stride();
    std::vector<u8> tmp(rowBytes);
    for (int y = 0; y < m_height / 2; ++y) {
        u8* a = row(y);
        u8* b = row(m_height - 1 - y);
        std::memcpy(tmp.data(), a, rowBytes);
        std::memcpy(a, b, rowBytes);
        std::memcpy(b, tmp.data(), rowBytes);
    }
}

void Image::flipHorizontal() {
    if (!valid()) return;
    for (int y = 0; y < m_height; ++y) {
        u8* r = row(y);
        for (int x = 0; x < m_width / 2; ++x) {
            u8* a = r + static_cast<size_t>(x) * 4;
            u8* b = r + static_cast<size_t>(m_width - 1 - x) * 4;
            for (int c = 0; c < 4; ++c) std::swap(a[c], b[c]);
        }
    }
}

bool Image::hasTransparency() const {
    if (!valid()) return false;
    const u8* p = data();
    const size_t n = static_cast<size_t>(m_width) * m_height;
    for (size_t i = 0; i < n; ++i) if (p[i * 4 + 3] != 255) return true;
    return false;
}

void Image::fillBitmapInfo(BITMAPINFO& bi) const {
    std::memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = m_width;
    bi.bmiHeader.biHeight      = -m_height;   // negative = top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
}

bool Image::blitTo(HDC dc, const RECT& dst, ChannelView view) const {
    if (!valid() || !dc) return false;

    // GDI wants BGRA; our storage is RGBA, so a channel-swapped copy is needed.
    Image work = applyChannelView(view);
    if (!work.valid()) return false;
    u8* p = work.data();
    const size_t n = static_cast<size_t>(work.width()) * work.height();
    for (size_t i = 0; i < n; ++i) std::swap(p[i * 4 + 0], p[i * 4 + 2]);

    BITMAPINFO bi;
    work.fillBitmapInfo(bi);

    const int oldMode = ::SetStretchBltMode(dc, HALFTONE);
    ::SetBrushOrgEx(dc, 0, 0, nullptr);
    const int rc = ::StretchDIBits(dc,
        dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top,
        0, 0, work.width(), work.height(),
        work.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    ::SetStretchBltMode(dc, oldMode);
    return rc != GDI_ERROR;
}

HBITMAP Image::createHBitmap() const {
    if (!valid()) return nullptr;
    BITMAPINFO bi;
    fillBitmapInfo(bi);
    void* bits = nullptr;
    HBITMAP bmp = ::CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { if (bmp) ::DeleteObject(bmp); return nullptr; }

    u8* dst = static_cast<u8*>(bits);
    const u8* src = data();
    const size_t n = static_cast<size_t>(m_width) * m_height;
    for (size_t i = 0; i < n; ++i) {
        dst[i * 4 + 0] = src[i * 4 + 2];   // B
        dst[i * 4 + 1] = src[i * 4 + 1];   // G
        dst[i * 4 + 2] = src[i * 4 + 0];   // R
        dst[i * 4 + 3] = src[i * 4 + 3];   // A
    }
    return bmp;
}

Image Image::Checkerboard(int w, int h, int cellSize, Rgba light, Rgba dark) {
    Image out;
    if (!out.resize(w, h)) return out;
    if (cellSize < 1) cellSize = 8;
    for (int y = 0; y < h; ++y) {
        u8* d = out.row(y);
        for (int x = 0; x < w; ++x) {
            const bool even = (((x / cellSize) + (y / cellSize)) & 1) == 0;
            const Rgba c = even ? light : dark;
            d[x * 4 + 0] = c.r; d[x * 4 + 1] = c.g; d[x * 4 + 2] = c.b; d[x * 4 + 3] = 255;
        }
    }
    return out;
}

Image Image::SolidColor(int w, int h, Rgba c) {
    Image out;
    if (out.resize(w, h)) out.fill(c);
    return out;
}

// ---------------------------------------------------------------------------
// The one-line readout under the image preview, e.g.
//   "DXT1  512x512  mip 10  alfa yok  174,7 KB"
//   "A8R8G8B8  1024x768  alfa var  3,0 MB"
//   "DXT5  128x128  kup harita (6 yuz)  mip 8  alfa var  128,0 KB"
// ---------------------------------------------------------------------------
std::wstring ImageMeta::summaryTr() const {
    std::wstring s = formatName.empty() ? std::wstring(L"?") : formatName;
    s += FormatW(L"  %d×%d", width, height);          // 512x512 with a real multiplication sign
    if (isVolume && depth > 1) s += FormatW(L"×%d", depth);

    if (isCube)        s += FormatW(T(L"  kup harita (%d yuz)"), faceCount);
    else if (isVolume) s += T(L"  hacim dokusu");
    else if (faceCount > 1) s += FormatW(T(L"  %d yuz"), faceCount);

    if (mipCount > 1)  s += FormatW(T(L"  mip %d"), mipCount);
    s += hasAlpha ? T(L"  alfa var") : T(L"  alfa yok");
    if (dataSize)      s += L"  " + FormatBytesTr(dataSize);
    return s;
}

} // namespace sro
