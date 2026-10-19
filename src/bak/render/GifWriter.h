// ============================================================================
//  SRO Asset Studio - render/GifWriter.h
//
//  Animated GIF89a writer, header-only, no dependencies.
//
//  This exists for one reason: a still frame cannot tell you whether an effect
//  animates correctly. Two renderers can agree perfectly on a single instant
//  and still disagree about every motion in the file - the spawn cadence, the
//  fade curve, the direction a ribbon sweeps. Comparing effects honestly means
//  comparing them MOVING, and a GIF is the one format that can be looked at
//  side by side without a player.
//
//  Colour: GIF allows 256 palette entries per frame. Effects are smooth
//  additive gradients over a dark ground, which is the worst case for naive
//  quantisation - a uniform cube posterises a glow into visible rings. So the
//  palette is built by median cut over the WHOLE animation (one global table,
//  which also keeps the file small) and mapped with a 4x4 ordered dither, which
//  breaks the rings up at no cost in size.
// ============================================================================
#pragma once

#include "Image.h"
#include "../Common.h"

#include <vector>
#include <algorithm>
#include <string>

namespace sro {
namespace gif {

// ----------------------------------------------------------------------------
//  Median-cut palette
// ----------------------------------------------------------------------------
namespace detail {

struct Box {
    int lo[3] = { 0, 0, 0 };
    int hi[3] = { 31, 31, 31 };   ///< inclusive, in 5-bit space
    u64 count = 0;
};

/// Longest axis of a box, weighted so no channel dominates purely by range.
inline int LongestAxis(const Box& b) {
    const int d0 = b.hi[0] - b.lo[0], d1 = b.hi[1] - b.lo[1], d2 = b.hi[2] - b.lo[2];
    if (d0 >= d1 && d0 >= d2) return 0;
    return d1 >= d2 ? 1 : 2;
}

inline u64 BoxCount(const std::vector<u32>& hist, const Box& b) {
    u64 t = 0;
    for (int r = b.lo[0]; r <= b.hi[0]; ++r)
        for (int g = b.lo[1]; g <= b.hi[1]; ++g)
            for (int bl = b.lo[2]; bl <= b.hi[2]; ++bl)
                t += hist[static_cast<size_t>((r << 10) | (g << 5) | bl)];
    return t;
}

/// Population-weighted mean colour of a box, in 8-bit space.
inline void BoxMean(const std::vector<u32>& hist, const Box& b, u8 out[3]) {
    u64 n = 0, s[3] = { 0, 0, 0 };
    for (int r = b.lo[0]; r <= b.hi[0]; ++r)
        for (int g = b.lo[1]; g <= b.hi[1]; ++g)
            for (int bl = b.lo[2]; bl <= b.hi[2]; ++bl) {
                const u32 c = hist[static_cast<size_t>((r << 10) | (g << 5) | bl)];
                if (!c) continue;
                n += c; s[0] += static_cast<u64>(r) * c;
                s[1] += static_cast<u64>(g) * c; s[2] += static_cast<u64>(bl) * c;
            }
    if (!n) {
        for (int k = 0; k < 3; ++k)
            out[k] = static_cast<u8>(((b.lo[k] + b.hi[k]) / 2) * 255 / 31);
        return;
    }
    for (int k = 0; k < 3; ++k) {
        const int v5 = static_cast<int>(s[k] / n);
        out[k] = static_cast<u8>(std::min(255, v5 * 255 / 31));
    }
}

}  // namespace detail

/// Build a global palette of at most `want` colours from every frame.
inline void BuildPalette(const std::vector<Image>& frames, int want,
                         std::vector<u8>& palette) {
    if (want < 2)   want = 2;
    if (want > 256) want = 256;

    // 5:5:5 histogram - 32768 bins, which is small enough to walk repeatedly
    // and fine enough that a gradient does not collapse before the cut runs.
    std::vector<u32> hist(32768, 0);
    for (const Image& f : frames) {
        if (!f.valid()) continue;
        for (int y = 0; y < f.height(); ++y) {
            const u8* r = f.row(y);
            for (int x = 0; x < f.width(); ++x) {
                const u32 idx = (static_cast<u32>(r[x * 4] >> 3) << 10) |
                                (static_cast<u32>(r[x * 4 + 1] >> 3) << 5) |
                                 static_cast<u32>(r[x * 4 + 2] >> 3);
                ++hist[idx];
            }
        }
    }

    std::vector<detail::Box> boxes;
    detail::Box root;
    root.count = detail::BoxCount(hist, root);
    boxes.push_back(root);

    while (static_cast<int>(boxes.size()) < want) {
        // Split the most populous box that still HAS an axis to split. A box
        // one cell wide on every axis cannot be divided; skipping it is what
        // stops the loop spinning when the image has few distinct colours.
        int pick = -1;
        u64 best = 0;
        for (size_t i = 0; i < boxes.size(); ++i) {
            const detail::Box& b = boxes[i];
            const bool splittable = (b.hi[0] > b.lo[0]) || (b.hi[1] > b.lo[1]) || (b.hi[2] > b.lo[2]);
            if (!splittable || b.count <= 1) continue;
            if (b.count > best) { best = b.count; pick = static_cast<int>(i); }
        }
        if (pick < 0) break;

        detail::Box b = boxes[static_cast<size_t>(pick)];
        const int ax = detail::LongestAxis(b);
        // Median by population, not by midpoint: a glow puts most of its pixels
        // near black, and a midpoint split would spend the palette on colours
        // that barely occur.
        u64 half = b.count / 2, run = 0;
        int cut = b.lo[ax];
        for (int v = b.lo[ax]; v <= b.hi[ax]; ++v) {
            detail::Box slice = b;
            slice.lo[ax] = slice.hi[ax] = v;
            run += detail::BoxCount(hist, slice);
            cut = v;
            if (run >= half) break;
        }
        if (cut >= b.hi[ax]) cut = b.hi[ax] - 1;

        detail::Box lo = b, hi = b;
        lo.hi[ax] = cut;
        hi.lo[ax] = cut + 1;
        lo.count = detail::BoxCount(hist, lo);
        hi.count = detail::BoxCount(hist, hi);
        boxes[static_cast<size_t>(pick)] = lo;
        boxes.push_back(hi);
    }

    palette.assign(static_cast<size_t>(want) * 3, 0);
    for (size_t i = 0; i < boxes.size() && i < static_cast<size_t>(want); ++i) {
        u8 rgb[3];
        detail::BoxMean(hist, boxes[i], rgb);
        palette[i * 3 + 0] = rgb[0];
        palette[i * 3 + 1] = rgb[1];
        palette[i * 3 + 2] = rgb[2];
    }
}

/// Nearest palette entry, plain squared distance in RGB.
inline u8 NearestIndex(const std::vector<u8>& pal, int r, int g, int b) {
    const size_t n = pal.size() / 3;
    int bestD = 1 << 30;
    u8 best = 0;
    for (size_t i = 0; i < n; ++i) {
        const int dr = r - pal[i * 3], dg = g - pal[i * 3 + 1], db = b - pal[i * 3 + 2];
        const int d = dr * dr + dg * dg + db * db;
        if (d < bestD) { bestD = d; best = static_cast<u8>(i); if (!d) break; }
    }
    return best;
}

// ----------------------------------------------------------------------------
//  LZW, as GIF specifies it (variable code width, clear + end codes)
// ----------------------------------------------------------------------------
namespace detail {

class BitPacker {
public:
    void push(u32 code, int width) {
        for (int i = 0; i < width; ++i) {
            if (code & (1u << i)) m_cur |= static_cast<u8>(1u << m_bits);
            if (++m_bits == 8) { m_out.push_back(m_cur); m_cur = 0; m_bits = 0; }
        }
    }
    void flush() { if (m_bits) { m_out.push_back(m_cur); m_cur = 0; m_bits = 0; } }
    const std::vector<u8>& bytes() const { return m_out; }

private:
    std::vector<u8> m_out;
    u8  m_cur  = 0;
    int m_bits = 0;
};

inline void LzwEncode(const std::vector<u8>& px, int minCode, std::vector<u8>& out) {
    const u32 clearCode = 1u << minCode;
    const u32 endCode   = clearCode + 1;

    // Dictionary as a flat table: 4096 entries x 256 next-symbols. 4 MB of
    // int32, allocated once per frame - trivial next to decoding the textures,
    // and far simpler than a hash map to get right.
    std::vector<i32> next(4096 * 256, -1);
    BitPacker bp;
    int  width = minCode + 1;
    u32  nextFree = endCode + 1;

    bp.push(clearCode, width);
    if (px.empty()) { bp.push(endCode, width); bp.flush(); out = bp.bytes(); return; }

    i32 cur = px[0];
    for (size_t i = 1; i < px.size(); ++i) {
        const u8 k = px[i];
        const i32 probe = next[static_cast<size_t>(cur) * 256 + k];
        if (probe >= 0) { cur = probe; continue; }

        bp.push(static_cast<u32>(cur), width);
        if (nextFree < 4096) {
            next[static_cast<size_t>(cur) * 256 + k] = static_cast<i32>(nextFree);
            // The width grows AFTER the code that fills the range is emitted,
            // which is the classic off-by-one in every hand-written LZW.
            if (nextFree == (1u << width) && width < 12) ++width;
            ++nextFree;
        } else {
            bp.push(clearCode, width);
            std::fill(next.begin(), next.end(), -1);
            width = minCode + 1;
            nextFree = endCode + 1;
        }
        cur = k;
    }
    bp.push(static_cast<u32>(cur), width);
    bp.push(endCode, width);
    bp.flush();
    out = bp.bytes();
}

inline void PutU16(std::vector<u8>& v, u16 x) {
    v.push_back(static_cast<u8>(x & 0xFF));
    v.push_back(static_cast<u8>(x >> 8));
}

}  // namespace detail

// ----------------------------------------------------------------------------
//  The writer
// ----------------------------------------------------------------------------

/// Write `frames` as one looping animation. `delayCs` is per frame, in
/// hundredths of a second - GIF's own unit, so 8 is 12.5 fps.
inline bool WriteAnimated(const std::vector<Image>& frames, int delayCs,
                          const std::wstring& path, int colours = 255) {
    if (frames.empty() || !frames[0].valid()) return false;
    const int W = frames[0].width(), H = frames[0].height();

    std::vector<u8> pal;
    BuildPalette(frames, colours, pal);

    // Palette size must be a power of two, and the code width follows from it.
    int bits = 1;
    while ((1 << bits) < static_cast<int>(pal.size() / 3)) ++bits;
    if (bits > 8) bits = 8;
    const size_t palSlots = static_cast<size_t>(1) << bits;
    pal.resize(palSlots * 3, 0);

    // Ordered dither: cheap, deterministic, and it keeps a smooth glow from
    // banding into rings once it is squeezed into 255 colours.
    static const int kBayer[4][4] = { {  0,  8,  2, 10 }, { 12,  4, 14,  6 },
                                      {  3, 11,  1,  9 }, { 15,  7, 13,  5 } };

    std::vector<u8> out;
    out.insert(out.end(), { 'G', 'I', 'F', '8', '9', 'a' });
    detail::PutU16(out, static_cast<u16>(W));
    detail::PutU16(out, static_cast<u16>(H));
    out.push_back(static_cast<u8>(0x80 | ((bits - 1) & 7)));   // global table present
    out.push_back(0);                                          // background index
    out.push_back(0);                                          // pixel aspect
    out.insert(out.end(), pal.begin(), pal.end());

    // NETSCAPE2.0 - the loop-forever extension.
    out.insert(out.end(), { 0x21, 0xFF, 0x0B });
    const char* app = "NETSCAPE2.0";
    out.insert(out.end(), app, app + 11);
    out.insert(out.end(), { 0x03, 0x01, 0x00, 0x00, 0x00 });

    std::vector<u8> idx(static_cast<size_t>(W) * H);
    for (const Image& f : frames) {
        if (!f.valid() || f.width() != W || f.height() != H) continue;

        for (int y = 0; y < H; ++y) {
            const u8* r = f.row(y);
            for (int x = 0; x < W; ++x) {
                const int d = kBayer[y & 3][x & 3] - 8;      // -8 .. +7
                const int rr = std::min(255, std::max(0, r[x * 4]     + d / 2));
                const int gg = std::min(255, std::max(0, r[x * 4 + 1] + d / 2));
                const int bb = std::min(255, std::max(0, r[x * 4 + 2] + d / 2));
                idx[static_cast<size_t>(y) * W + x] = NearestIndex(pal, rr, gg, bb);
            }
        }

        out.insert(out.end(), { 0x21, 0xF9, 0x04, 0x00 });    // graphic control
        detail::PutU16(out, static_cast<u16>(delayCs));
        out.push_back(0);                                     // no transparency
        out.push_back(0);                                     // block terminator

        out.push_back(0x2C);                                  // image descriptor
        detail::PutU16(out, 0); detail::PutU16(out, 0);
        detail::PutU16(out, static_cast<u16>(W));
        detail::PutU16(out, static_cast<u16>(H));
        out.push_back(0);                                     // no local table

        const int minCode = bits < 2 ? 2 : bits;
        out.push_back(static_cast<u8>(minCode));
        std::vector<u8> lzw;
        detail::LzwEncode(idx, minCode, lzw);
        for (size_t p = 0; p < lzw.size(); p += 255) {
            const size_t n = std::min<size_t>(255, lzw.size() - p);
            out.push_back(static_cast<u8>(n));
            out.insert(out.end(), lzw.begin() + p, lzw.begin() + p + n);
        }
        out.push_back(0);                                     // end of blocks
    }
    out.push_back(0x3B);                                      // trailer

    File fo;
    if (!fo.createNew(path, true)) return false;
    return fo.write(out.data(), out.size());
}

}  // namespace gif
}  // namespace sro
