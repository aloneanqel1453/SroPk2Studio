// ============================================================================
//  SRO Asset Studio - render/Image.h
//
//  RGBA8 image. This is THE single currency of the program: every decoder
//  (DDS/DDJ/WIC/TGA) produces one, the software rasterizer renders into one,
//  every viewer displays one and PNG export consumes one.
//
//  Layout: top-down, tightly packed, stride = width * 4, order R,G,B,A.
//
//  Header-only apart from a handful of helpers - keep it cheap to include.
//  Implementation owner of the out-of-line helpers: Agent E (render).
// ============================================================================
#pragma once

#include "../Common.h"

namespace sro {

// ---------------------------------------------------------------------------
// Rgba
// ---------------------------------------------------------------------------
struct Rgba {
    u8 r = 0, g = 0, b = 0, a = 255;

    Rgba() = default;
    Rgba(u8 R, u8 G, u8 B, u8 A = 255) : r(R), g(G), b(B), a(A) {}

    /// From 0x00RRGGBB (alpha forced opaque) - matches the theme constants.
    static Rgba FromRgb(u32 rgb) {
        return Rgba(static_cast<u8>((rgb >> 16) & 0xFF),
                    static_cast<u8>((rgb >> 8) & 0xFF),
                    static_cast<u8>(rgb & 0xFF), 255);
    }
    /// From 0xAARRGGBB.
    static Rgba FromArgb(u32 argb) {
        return Rgba(static_cast<u8>((argb >> 16) & 0xFF),
                    static_cast<u8>((argb >> 8) & 0xFF),
                    static_cast<u8>(argb & 0xFF),
                    static_cast<u8>((argb >> 24) & 0xFF));
    }
    u32 toArgb() const {
        return (static_cast<u32>(a) << 24) | (static_cast<u32>(r) << 16) |
               (static_cast<u32>(g) << 8) | b;
    }
    /// Win32 COLORREF is 0x00BBGGRR.
    COLORREF toColorRef() const { return RGB(r, g, b); }
};

/// Channel selection for the image viewer's R/G/B/A toggles.
enum class ChannelView : int {
    RGB = 0,     ///< DEFAULT. Alpha ignored - SRO textures store sheen/gloss
                 ///< in alpha, so honouring it makes armour look wrongly
                 ///< transparent (SPEC.md §3).
    RGBA,        ///< explicit opt-in, composited over a checkerboard
    Red, Green, Blue, Alpha
};

// ---------------------------------------------------------------------------
// Image
// ---------------------------------------------------------------------------
class Image {
public:
    Image() = default;
    Image(int w, int h) { resize(w, h); }

    // --- geometry ----------------------------------------------------------
    int    width()  const { return m_width; }
    int    height() const { return m_height; }
    size_t stride() const { return static_cast<size_t>(m_width) * 4; }
    bool   valid()  const { return m_width > 0 && m_height > 0 &&
                                   m_pixels.size() == static_cast<size_t>(m_width) * m_height * 4; }
    bool   empty()  const { return !valid(); }

    /// Allocate w*h*4 bytes, zero filled. Fails silently (leaving the image
    /// empty) if the dimensions are non-positive or absurd.
    bool resize(int w, int h) {
        if (w <= 0 || h <= 0) { clear(); return false; }
        // 1 GB ceiling: a 16384 x 16384 RGBA texture is 1 GB, which is already
        // far beyond anything Silkroad ships.
        const u64 bytes = static_cast<u64>(w) * static_cast<u64>(h) * 4ull;
        if (bytes > (1ull << 30)) { clear(); return false; }
        try { m_pixels.assign(static_cast<size_t>(bytes), 0); }
        catch (...) { clear(); return false; }
        m_width = w; m_height = h;
        return true;
    }
    void clear() { m_pixels.clear(); m_width = 0; m_height = 0; }

    // --- data access -------------------------------------------------------
    u8*       data()       { return m_pixels.empty() ? nullptr : m_pixels.data(); }
    const u8* data() const { return m_pixels.empty() ? nullptr : m_pixels.data(); }
    size_t    sizeBytes() const { return m_pixels.size(); }
    std::vector<u8>&       storage()       { return m_pixels; }
    const std::vector<u8>& storage() const { return m_pixels; }

    u8*       row(int y)       { return data() + static_cast<size_t>(y) * stride(); }
    const u8* row(int y) const { return data() + static_cast<size_t>(y) * stride(); }

    /// Bounds-checked pixel access; returns nullptr when out of range.
    u8* at(int x, int y) {
        if (x < 0 || y < 0 || x >= m_width || y >= m_height) return nullptr;
        return data() + (static_cast<size_t>(y) * m_width + x) * 4;
    }
    const u8* at(int x, int y) const {
        if (x < 0 || y < 0 || x >= m_width || y >= m_height) return nullptr;
        return data() + (static_cast<size_t>(y) * m_width + x) * 4;
    }

    Rgba get(int x, int y) const {
        const u8* p = at(x, y);
        return p ? Rgba(p[0], p[1], p[2], p[3]) : Rgba(0, 0, 0, 0);
    }
    void set(int x, int y, Rgba c) {
        u8* p = at(x, y);
        if (p) { p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a; }
    }

    /// Fill the whole image with one colour.
    void fill(Rgba c);

    // --- derived images ----------------------------------------------------

    /// A copy with the selected channel view applied (see ChannelView).
    /// ChannelView::RGB forces alpha to 255 - this is the DEFAULT for SRO.
    Image applyChannelView(ChannelView view) const;

    /// A copy composited over a checkerboard, for previewing real transparency.
    Image compositeOverCheckerboard(int cellSize = 8,
                                    Rgba light = Rgba(0x40, 0x38, 0x2C),
                                    Rgba dark  = Rgba(0x2C, 0x26, 0x1C)) const;

    /// Nearest-neighbour resize (thumbnails).
    Image scaledNearest(int w, int h) const;
    /// Bilinear resize (preview zoom-out quality).
    Image scaledBilinear(int w, int h) const;

    // --- ADDITIVE (textures agent) -----------------------------------------

    /// Box (area average) resize. Correct choice for DOWNscaling: bilinear
    /// point-samples and aliases badly past ~2x, box averages every source
    /// pixel that falls in the destination footprint. Falls back to bilinear
    /// when either axis is being enlarged.
    Image scaledBox(int w, int h) const;

    /// The reference viewer's present chain: a vignette (main.cpp:4280-4291)
    /// followed by saturation + contrast (its kSatProg fragment program,
    /// main.cpp:5571). Runs IN PLACE on the already-downscaled buffer, which is
    /// exactly where the reference binds it - on the SSAA resolve quad, not on
    /// the supersampled target. Saturation pushes each pixel away from its own
    /// luma, so a washed-out SRO diffuse gains chroma without clipping to white.
    void applyStudioPost(f32 saturation, f32 contrast, f32 vignetteAlpha);

    /// Best-quality resize: box when shrinking, bilinear when growing.
    /// This is what the preview pane and the thumbnail cache should call.
    Image scaledSmooth(int w, int h) const;

    /// Sub-rectangle copy. The rectangle is clipped to the image; an empty
    /// intersection yields an empty Image.
    Image cropped(int x, int y, int w, int h) const;

    /// Vertical flip in place (DDS bottom-up sources).
    void flipVertical();
    /// Horizontal flip in place (ADDITIVE - cube map face orientation).
    void flipHorizontal();

    /// True when any pixel has alpha < 255.
    bool hasTransparency() const;

    // --- Win32 interop -----------------------------------------------------

    /// Fill a BITMAPINFO describing this image for StretchDIBits.
    /// The image is top-down, so biHeight is written NEGATIVE.
    void fillBitmapInfo(BITMAPINFO& bi) const;

    /// Blit into a DC, stretching to `dst`. `view` is applied on the fly.
    /// Returns false when the image is empty.
    bool blitTo(HDC dc, const RECT& dst, ChannelView view = ChannelView::RGB) const;

    /// Create a 32-bit DIB section HBITMAP holding a copy of this image
    /// (for owner-drawn tree thumbnails). Caller owns the HBITMAP.
    HBITMAP createHBitmap() const;

    // --- factories ---------------------------------------------------------
    static Image Checkerboard(int w, int h, int cellSize,
                              Rgba light, Rgba dark);
    static Image SolidColor(int w, int h, Rgba c);

private:
    int             m_width  = 0;
    int             m_height = 0;
    std::vector<u8> m_pixels;   ///< RGBA8, top-down
};

// ---------------------------------------------------------------------------
// Metadata that travels next to an image for the viewer's readout line.
// ---------------------------------------------------------------------------
struct ImageMeta {
    std::wstring formatName;    ///< "DXT5", "A8R8G8B8", "PNG", ...
    int  width       = 0;
    int  height      = 0;
    int  mipCount    = 1;
    int  faceCount   = 1;       ///< 6 for a cube map
    int  depth       = 1;       ///< > 1 for a volume texture
    u64  dataSize    = 0;       ///< encoded size in bytes
    bool hasAlpha    = false;
    bool isCube      = false;
    bool isVolume    = false;

    /// One-line Turkish summary, e.g. "DXT5  256x256  mip 9  512,0 KB".
    std::wstring summaryTr() const;
};

} // namespace sro
