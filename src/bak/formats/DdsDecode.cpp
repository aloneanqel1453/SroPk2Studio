// ============================================================================
//  SRO Asset Studio - formats/DdsDecode.cpp
//
//  Self contained DDS (Direct3D 9 era, plus a small DX10 header shim) parser
//  and decoder. Output is ALWAYS RGBA8.
//
//  Coverage (SPEC.md section 3):
//    block compressed : DXT1, DXT2, DXT3, DXT4, DXT5, ATI1/BC4, ATI2/BC5
//    uncompressed     : A8R8G8B8, X8R8G8B8, R8G8B8, A1R5G5B5, A4R4G4B4,
//                       R5G6B5, A8, L8, A8L8, V8U8
//    plus mipmap chains and cube maps (6 faces), and volume textures
//    (slice 0 of the selected mip is decoded).
//
//  Implementation notes that matter for correctness:
//
//  * DXT1 has TWO modes. When colour0 > colour1 the block holds four opaque
//    colours; when colour0 <= colour1 it holds three colours plus a fully
//    transparent index 3. Getting this backwards is the classic "black holes
//    in the texture" bug. DXT2/3/4/5 always use the four-colour form for the
//    colour half of the block, regardless of the colour0/colour1 ordering.
//
//  * Uncompressed RGB surfaces are decoded from the pixel format's ACTUAL
//    channel masks rather than from the guessed format name. That is what
//    keeps unusual layouts (A8B8G8R8, B8G8R8, X1R5G5B5, ...) from coming out
//    with swapped channels; the DdsFormat enum is only used for the readout.
//
//  * Rows are assumed tightly packed (pitch = width * bpp / 8). That is what
//    every DDS Silkroad ships uses, and what DirectXTex computes when the
//    DDSD_PITCH field is missing or wrong.
//
//  Every read is bounds-checked against the buffer length; a truncated or
//  hostile file fails with a Turkish message and never over-reads.
// ============================================================================
#include "DdsDecode.h"

namespace sro {

namespace {

// ---------------------------------------------------------------------------
// Small bit helpers
// ---------------------------------------------------------------------------
inline u32 PopCount32(u32 v) {
    u32 c = 0;
    while (v) { c += (v & 1u); v >>= 1; }
    return c;
}
inline u32 TrailingZeros32(u32 v) {
    if (v == 0) return 0;
    u32 c = 0;
    while ((v & 1u) == 0) { ++c; v >>= 1; }
    return c;
}

/// One channel of an uncompressed pixel, described by its bit mask.
struct MaskChannel {
    u32 mask  = 0;
    u32 shift = 0;
    u32 maxv  = 0;

    void set(u32 m) {
        mask  = m;
        shift = TrailingZeros32(m);
        const u32 bits = PopCount32(m);
        maxv = bits ? ((bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u)) : 0u;
    }
    /// Extract and scale to 8 bits; `fallback` is used when the channel is absent.
    u8 extract(u32 pixel, u8 fallback) const {
        if (!mask || !maxv) return fallback;
        const u32 v = (pixel & mask) >> shift;
        if (maxv == 255u) return static_cast<u8>(v);
        const u64 scaled = (static_cast<u64>(v) * 255ull + (maxv / 2)) / maxv;
        return static_cast<u8>(scaled > 255ull ? 255ull : scaled);
    }
};

/// Read 1..4 little endian bytes as a u32.
inline u32 ReadPixelBytes(const u8* p, size_t bytesPerPixel) {
    switch (bytesPerPixel) {
        case 1:  return p[0];
        case 2:  return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8);
        case 3:  return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
                        (static_cast<u32>(p[2]) << 16);
        default: return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
                        (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
    }
}

inline u32 MakeFourCC(char a, char b, char c, char d) {
    return static_cast<u32>(static_cast<u8>(a)) |
           (static_cast<u32>(static_cast<u8>(b)) << 8) |
           (static_cast<u32>(static_cast<u8>(c)) << 16) |
           (static_cast<u32>(static_cast<u8>(d)) << 24);
}

const u32 kFourCC_DXT1 = MakeFourCC('D', 'X', 'T', '1');
const u32 kFourCC_DXT2 = MakeFourCC('D', 'X', 'T', '2');
const u32 kFourCC_DXT3 = MakeFourCC('D', 'X', 'T', '3');
const u32 kFourCC_DXT4 = MakeFourCC('D', 'X', 'T', '4');
const u32 kFourCC_DXT5 = MakeFourCC('D', 'X', 'T', '5');
const u32 kFourCC_ATI1 = MakeFourCC('A', 'T', 'I', '1');
const u32 kFourCC_ATI2 = MakeFourCC('A', 'T', 'I', '2');
const u32 kFourCC_BC4U = MakeFourCC('B', 'C', '4', 'U');
const u32 kFourCC_BC4S = MakeFourCC('B', 'C', '4', 'S');
const u32 kFourCC_BC5U = MakeFourCC('B', 'C', '5', 'U');
const u32 kFourCC_BC5S = MakeFourCC('B', 'C', '5', 'S');
const u32 kFourCC_DX10 = MakeFourCC('D', 'X', '1', '0');

/// Highest sensible texture edge. Silkroad never exceeds 4096.
constexpr u32 kMaxDimension = 32768;

/// Expand a 5:6:5 colour to 8 bits per channel with bit replication.
inline void Expand565(u16 c, u8& r, u8& g, u8& b) {
    const u32 r5 = (c >> 11) & 0x1Fu;
    const u32 g6 = (c >> 5)  & 0x3Fu;
    const u32 b5 = c         & 0x1Fu;
    r = static_cast<u8>((r5 << 3) | (r5 >> 2));
    g = static_cast<u8>((g6 << 2) | (g6 >> 4));
    b = static_cast<u8>((b5 << 3) | (b5 >> 2));
}

/// Colour half of a DXT block (8 bytes) into a 4x4 RGBA8 tile.
/// `allowPunchThrough` must be true only for DXT1 - DXT2..5 always use the
/// four-colour interpretation even when colour0 <= colour1.
void DecodeColourBlock(const u8* src, u8* out /*[64]*/, bool allowPunchThrough) {
    const u16 c0 = static_cast<u16>(src[0] | (src[1] << 8));
    const u16 c1 = static_cast<u16>(src[2] | (src[3] << 8));

    u8 pal[4][4];
    Expand565(c0, pal[0][0], pal[0][1], pal[0][2]); pal[0][3] = 255;
    Expand565(c1, pal[1][0], pal[1][1], pal[1][2]); pal[1][3] = 255;

    if (c0 > c1 || !allowPunchThrough) {
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = static_cast<u8>((2 * static_cast<int>(pal[0][i]) + pal[1][i]) / 3);
            pal[3][i] = static_cast<u8>((static_cast<int>(pal[0][i]) + 2 * pal[1][i]) / 3);
        }
        pal[2][3] = 255;
        pal[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = static_cast<u8>((static_cast<int>(pal[0][i]) + pal[1][i]) / 2);
            pal[3][i] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;          // 1-bit alpha: index 3 is fully transparent
    }

    const u32 bits = static_cast<u32>(src[4]) | (static_cast<u32>(src[5]) << 8) |
                     (static_cast<u32>(src[6]) << 16) | (static_cast<u32>(src[7]) << 24);
    for (int i = 0; i < 16; ++i) {
        const u32 idx = (bits >> (2 * i)) & 3u;
        out[i * 4 + 0] = pal[idx][0];
        out[i * 4 + 1] = pal[idx][1];
        out[i * 4 + 2] = pal[idx][2];
        out[i * 4 + 3] = pal[idx][3];
    }
}

/// The 8-byte "interpolated 8-value" block used by DXT5 alpha, BC4 and BC5.
/// Writes 16 single-channel samples into `values`.
void DecodeInterpolatedBlock(const u8* src, u8 values[16]) {
    const u8 a0 = src[0];
    const u8 a1 = src[1];

    u8 tbl[8];
    tbl[0] = a0;
    tbl[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i <= 6; ++i)
            tbl[1 + i] = static_cast<u8>(((7 - i) * static_cast<int>(a0) + i * static_cast<int>(a1)) / 7);
    } else {
        for (int i = 1; i <= 4; ++i)
            tbl[1 + i] = static_cast<u8>(((5 - i) * static_cast<int>(a0) + i * static_cast<int>(a1)) / 5);
        tbl[6] = 0;
        tbl[7] = 255;
    }

    u64 bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<u64>(src[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i)
        values[i] = tbl[(bits >> (3 * i)) & 7ull];
}

/// Surface size honouring a generic (mask described) uncompressed layout.
size_t SurfaceBytesOf(const DdsInfo& info, int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    const size_t block = DdsBlockBytes(info.format);
    if (block) {
        const size_t bw = static_cast<size_t>((w + 3) / 4);
        const size_t bh = static_cast<size_t>((h + 3) / 4);
        return bw * bh * block;
    }
    size_t bpp = DdsBitsPerPixel(info.format);
    if (!bpp && info.bitsPerPixel > 0) bpp = static_cast<size_t>(info.bitsPerPixel);
    if (!bpp) return 0;
    // Rows are byte aligned; this matters for the 4 bit palettised format.
    const size_t rowBytes = (static_cast<size_t>(w) * bpp + 7) / 8;
    return rowBytes * static_cast<size_t>(h);
}

/// Number of mip levels a w x h texture can actually have.
int MaxMipLevels(int w, int h) {
    int n = 1;
    while (w > 1 || h > 1) {
        if (w > 1) w >>= 1;
        if (h > 1) h >>= 1;
        ++n;
    }
    return n;
}

/// Map a DXGI_FORMAT from a DX10 header onto our enum + masks.
/// Returns false for formats we cannot decode.
bool MapDxgiFormat(u32 dxgi, DdsInfo& info) {
    switch (dxgi) {
        case 70: case 71: case 72:  info.format = DdsFormat::Dxt1; return true;   // BC1
        case 73: case 74: case 75:  info.format = DdsFormat::Dxt3; return true;   // BC2
        case 76: case 77: case 78:  info.format = DdsFormat::Dxt5; return true;   // BC3
        case 79: case 80: case 81:  info.format = DdsFormat::Ati1; return true;   // BC4
        case 82: case 83: case 84:  info.format = DdsFormat::Ati2; return true;   // BC5
        case 28: case 29:                                                          // R8G8B8A8
            info.format = DdsFormat::RgbMasked; info.bitsPerPixel = 32;
            info.rMask = 0x000000FF; info.gMask = 0x0000FF00;
            info.bMask = 0x00FF0000; info.aMask = 0xFF000000;
            info.hasAlphaChannel = true; return true;
        case 87: case 91:                                                          // B8G8R8A8
            info.format = DdsFormat::A8R8G8B8; info.bitsPerPixel = 32;
            info.rMask = 0x00FF0000; info.gMask = 0x0000FF00;
            info.bMask = 0x000000FF; info.aMask = 0xFF000000;
            info.hasAlphaChannel = true; return true;
        case 88: case 93:                                                          // B8G8R8X8
            info.format = DdsFormat::X8R8G8B8; info.bitsPerPixel = 32;
            info.rMask = 0x00FF0000; info.gMask = 0x0000FF00;
            info.bMask = 0x000000FF; info.aMask = 0;
            return true;
        case 85:                                                                   // B5G6R5
            info.format = DdsFormat::R5G6B5; info.bitsPerPixel = 16;
            info.rMask = 0xF800; info.gMask = 0x07E0; info.bMask = 0x001F; info.aMask = 0;
            return true;
        case 86:                                                                   // B5G5R5A1
            info.format = DdsFormat::A1R5G5B5; info.bitsPerPixel = 16;
            info.rMask = 0x7C00; info.gMask = 0x03E0; info.bMask = 0x001F; info.aMask = 0x8000;
            info.hasAlphaChannel = true; return true;
        case 115:                                                                  // B4G4R4A4
            info.format = DdsFormat::A4R4G4B4; info.bitsPerPixel = 16;
            info.rMask = 0x0F00; info.gMask = 0x00F0; info.bMask = 0x000F; info.aMask = 0xF000;
            info.hasAlphaChannel = true; return true;
        case 61:                                                                   // R8_UNORM
            info.format = DdsFormat::L8; info.bitsPerPixel = 8; return true;
        case 65:                                                                   // A8_UNORM
            info.format = DdsFormat::A8; info.bitsPerPixel = 8;
            info.hasAlphaChannel = true; return true;
        case 60:                                                                   // R8G8_UNORM
            info.format = DdsFormat::A8L8; info.bitsPerPixel = 16;
            info.hasAlphaChannel = true; return true;
        default:
            info.format = DdsFormat::UnsupportedFourCC;
            return false;
    }
}

/// Name a mask-described RGB layout for the readout ("A8R8G8B8", "R5G6B5", ...).
DdsFormat ClassifyMasked(u32 bpp, u32 r, u32 g, u32 b, u32 a) {
    if (bpp == 32) {
        if (r == 0x00FF0000 && g == 0x0000FF00 && b == 0x000000FF)
            return a ? DdsFormat::A8R8G8B8 : DdsFormat::X8R8G8B8;
        return DdsFormat::RgbMasked;
    }
    if (bpp == 24) {
        if (r == 0x00FF0000 && g == 0x0000FF00 && b == 0x000000FF) return DdsFormat::R8G8B8;
        return DdsFormat::RgbMasked;
    }
    if (bpp == 16) {
        if (r == 0x7C00 && g == 0x03E0 && b == 0x001F && (a == 0x8000 || a == 0))
            return DdsFormat::A1R5G5B5;
        if (r == 0x0F00 && g == 0x00F0 && b == 0x000F) return DdsFormat::A4R4G4B4;
        if (r == 0xF800 && g == 0x07E0 && b == 0x001F) return DdsFormat::R5G6B5;
        return DdsFormat::RgbMasked;
    }
    return DdsFormat::RgbMasked;
}

} // namespace

// ---------------------------------------------------------------------------
// Format tables
// ---------------------------------------------------------------------------
const wchar_t* DdsFormatName(DdsFormat f) {
    switch (f) {
        case DdsFormat::Dxt1:     return L"DXT1";
        case DdsFormat::Dxt2:     return L"DXT2";
        case DdsFormat::Dxt3:     return L"DXT3";
        case DdsFormat::Dxt4:     return L"DXT4";
        case DdsFormat::Dxt5:     return L"DXT5";
        case DdsFormat::Ati1:     return L"ATI1/BC4";
        case DdsFormat::Ati2:     return L"ATI2/BC5";
        case DdsFormat::A8R8G8B8: return L"A8R8G8B8";
        case DdsFormat::X8R8G8B8: return L"X8R8G8B8";
        case DdsFormat::R8G8B8:   return L"R8G8B8";
        case DdsFormat::A1R5G5B5: return L"A1R5G5B5";
        case DdsFormat::A4R4G4B4: return L"A4R4G4B4";
        case DdsFormat::R5G6B5:   return L"R5G6B5";
        case DdsFormat::A8:       return L"A8";
        case DdsFormat::L8:       return L"L8";
        case DdsFormat::A8L8:     return L"A8L8";
        case DdsFormat::V8U8:     return L"V8U8";
        case DdsFormat::RgbMasked:         return T(L"RGB (ozel maske)");
        case DdsFormat::P8:                return T(L"P8 (paletli, 256 renk)");
        case DdsFormat::P4:                return T(L"P4 (paletli, 16 renk)");
        case DdsFormat::UnsupportedFourCC: return T(L"Desteklenmeyen FourCC");
        default:                  return T(L"Bilinmeyen");
    }
}

size_t DdsBlockBytes(DdsFormat f) {
    switch (f) {
        case DdsFormat::Dxt1: case DdsFormat::Ati1: return 8;
        case DdsFormat::Dxt2: case DdsFormat::Dxt3:
        case DdsFormat::Dxt4: case DdsFormat::Dxt5:
        case DdsFormat::Ati2: return 16;
        default: return 0;
    }
}

size_t DdsBitsPerPixel(DdsFormat f) {
    switch (f) {
        case DdsFormat::A8R8G8B8: case DdsFormat::X8R8G8B8: return 32;
        case DdsFormat::R8G8B8:   return 24;
        case DdsFormat::A1R5G5B5: case DdsFormat::A4R4G4B4:
        case DdsFormat::R5G6B5:   case DdsFormat::A8L8:
        case DdsFormat::V8U8:     return 16;
        case DdsFormat::A8: case DdsFormat::L8: case DdsFormat::P8: return 8;
        case DdsFormat::P4: return 4;
        default: return 0;   // block compressed, or a mask-described layout
    }
}

size_t DdsSurfaceBytes(DdsFormat f, int width, int height) {
    if (width <= 0 || height <= 0) return 0;
    const size_t block = DdsBlockBytes(f);
    if (block) {
        const size_t bw = static_cast<size_t>((width + 3) / 4);
        const size_t bh = static_cast<size_t>((height + 3) / 4);
        return bw * bh * block;
    }
    const size_t bpp = DdsBitsPerPixel(f);
    if (!bpp) return 0;
    const size_t rowBytes = (static_cast<size_t>(width) * bpp + 7) / 8;
    return rowBytes * static_cast<size_t>(height);
}

// ---------------------------------------------------------------------------
// DdsInfo
// ---------------------------------------------------------------------------
void DdsInfo::mipSize(int mip, int& w, int& h) const {
    if (mip < 0) mip = 0;
    if (mip > 31) mip = 31;
    w = (std::max)(1, width  >> mip);
    h = (std::max)(1, height >> mip);
}

size_t DdsInfo::surfaceOffset(int face, int mip) const {
    if (face < 0 || face >= faceCount) return static_cast<size_t>(-1);
    if (mip  < 0 || mip  >= mipCount)  return static_cast<size_t>(-1);

    // Every face carries the whole mip chain, so one face is a fixed stride.
    size_t faceBytes = 0;
    size_t within    = 0;
    for (int m = 0; m < mipCount; ++m) {
        int w = 0, h = 0;
        mipSize(m, w, h);
        size_t s = SurfaceBytesOf(*this, w, h);
        if (isVolume) {
            const int d = (std::max)(1, depth >> m);
            s *= static_cast<size_t>(d);
        }
        if (m < mip) within += s;
        faceBytes += s;
    }
    return dataOffset + static_cast<size_t>(face) * faceBytes + within;
}

// ---------------------------------------------------------------------------
// ParseDds
// ---------------------------------------------------------------------------
bool ParseDds(const u8* data, size_t size, DdsInfo& out, std::wstring* errorTr) {
    out = DdsInfo();

    ByteReader r(data, size);
    if (!r.canRead(4 + sizeof(DdsHeader))) {
        if (errorTr) *errorTr = T(L"DDS basligi icin dosya cok kisa.");
        return false;
    }
    const u32 magic = r.readU32();
    if (magic != kDdsMagic) {
        if (errorTr) *errorTr = T(L"DDS imzasi bulunamadi (\"DDS \" bekleniyordu).");
        return false;
    }

    DdsHeader h{};
    if (!r.readBytes(&h, sizeof(h))) {
        if (errorTr) *errorTr = T(L"DDS basligi eksik.");
        return false;
    }
    if (h.size != 124) {
        if (errorTr) *errorTr = FormatW(T(L"DDS baslik boyutu beklenmedik: %u (124 olmali)."), h.size);
        return false;
    }
    if (h.width == 0 || h.height == 0 || h.width > kMaxDimension || h.height > kMaxDimension) {
        if (errorTr) *errorTr = FormatW(T(L"Gecersiz DDS boyutu: %u x %u."), h.width, h.height);
        return false;
    }

    out.width  = static_cast<int>(h.width);
    out.height = static_cast<int>(h.height);
    out.fourCC = h.pf.fourCC;
    out.dataOffset = 4 + sizeof(DdsHeader);

    // --- volume / cube -----------------------------------------------------
    out.isVolume = (h.caps2 & kDdsCaps2Volume) != 0 && h.depth > 1;
    out.depth    = out.isVolume ? static_cast<int>((std::min)(h.depth, kMaxDimension)) : 1;

    out.isCube = (h.caps2 & kDdsCaps2Cubemap) != 0;
    if (out.isCube) {
        // Count the face flags that are actually present (+X..-Z are six
        // consecutive bits starting at DDSCAPS2_CUBEMAP_POSITIVEX).
        int faces = 0;
        for (int i = 0; i < 6; ++i)
            if (h.caps2 & (kDdsCaps2CubemapPositiveX << i)) ++faces;
        out.faceCount = faces > 0 ? faces : 6;
    } else {
        out.faceCount = 1;
    }

    // --- pixel format ------------------------------------------------------
    const DdsPixelFormat& pf = h.pf;
    // A zero fourCC with the flag set happens in the wild; treat it as "no
    // fourCC" and fall through to the mask driven path.
    if ((pf.flags & kDdpfFourCC) && pf.fourCC != 0) {
        if (pf.fourCC == kFourCC_DXT1) {
            out.format = DdsFormat::Dxt1;
            out.hasAlphaChannel = true;    // 1-bit punch-through is possible
        } else if (pf.fourCC == kFourCC_DXT2) {
            out.format = DdsFormat::Dxt2; out.hasAlphaChannel = true; out.premultipliedAlpha = true;
        } else if (pf.fourCC == kFourCC_DXT3) {
            out.format = DdsFormat::Dxt3; out.hasAlphaChannel = true;
        } else if (pf.fourCC == kFourCC_DXT4) {
            out.format = DdsFormat::Dxt4; out.hasAlphaChannel = true; out.premultipliedAlpha = true;
        } else if (pf.fourCC == kFourCC_DXT5) {
            out.format = DdsFormat::Dxt5; out.hasAlphaChannel = true;
        } else if (pf.fourCC == kFourCC_ATI1 || pf.fourCC == kFourCC_BC4U || pf.fourCC == kFourCC_BC4S) {
            out.format = DdsFormat::Ati1;
        } else if (pf.fourCC == kFourCC_ATI2 || pf.fourCC == kFourCC_BC5U || pf.fourCC == kFourCC_BC5S) {
            out.format = DdsFormat::Ati2;
        } else if (pf.fourCC == kFourCC_DX10) {
            DdsHeaderDxt10 dx{};
            if (!r.readBytes(&dx, sizeof(dx))) {
                if (errorTr) *errorTr = T(L"DX10 uzatilmis basligi eksik.");
                return false;
            }
            out.dataOffset = 4 + sizeof(DdsHeader) + sizeof(DdsHeaderDxt10);
            if (dx.miscFlag & 0x4u) {           // D3D10_RESOURCE_MISC_TEXTURECUBE
                out.isCube = true;
                out.faceCount = 6;
            }
            if (dx.resourceDimension == 4 && dx.arraySize <= 1 && h.depth > 1) {
                out.isVolume = true;
                out.depth = static_cast<int>((std::min)(h.depth, kMaxDimension));
            }
            if (!MapDxgiFormat(dx.dxgiFormat, out)) {
                out.formatName = FormatW(L"DX10 DXGI %u", dx.dxgiFormat);
                if (errorTr) *errorTr = FormatW(T(L"Desteklenmeyen DX10 formati: DXGI %u."), dx.dxgiFormat);
                return false;
            }
        } else {
            out.format = DdsFormat::UnsupportedFourCC;
            if (pf.fourCC < 0x10000u) {
                // Direct3D 9 also allows a numeric D3DFORMAT value in this
                // field (that is how the float surfaces such as 113 =
                // A16B16G16R16F are written). Report the number, not gibberish.
                out.formatName = FormatW(L"D3DFMT %u", pf.fourCC);
            } else {
                char cc[5] = { static_cast<char>(pf.fourCC & 0xFF),
                               static_cast<char>((pf.fourCC >> 8) & 0xFF),
                               static_cast<char>((pf.fourCC >> 16) & 0xFF),
                               static_cast<char>((pf.fourCC >> 24) & 0xFF), 0 };
                for (int i = 0; i < 4; ++i)
                    if (cc[i] < 32 || static_cast<u8>(cc[i]) > 126) cc[i] = '?';
                out.formatName = FormatW(L"FourCC '%S'", cc);
            }
            if (errorTr) *errorTr = T(L"Desteklenmeyen DDS sikistirmasi: ") + out.formatName;
            return false;
        }
    } else if (pf.flags & (kDdpfPaletteIndexed8 | kDdpfPaletteIndexed4)) {
        // Palettised: the palette is stored immediately after the header as
        // PALETTEENTRY quads (R, G, B, flags-used-as-alpha).
        const bool p4 = (pf.flags & kDdpfPaletteIndexed4) != 0 &&
                        (pf.flags & kDdpfPaletteIndexed8) == 0;
        out.format        = p4 ? DdsFormat::P4 : DdsFormat::P8;
        out.bitsPerPixel  = p4 ? 4 : 8;
        out.paletteEntries = p4 ? 16 : 256;
        out.paletteOffset  = out.dataOffset;
        const size_t paletteBytes = static_cast<size_t>(out.paletteEntries) * 4;
        if (out.paletteOffset + paletteBytes > size) {
            if (errorTr) *errorTr = T(L"DDS renk paleti eksik.");
            return false;
        }
        out.dataOffset += paletteBytes;
        out.hasAlphaChannel = true;   // refined after the palette is inspected
    } else if (pf.flags & kDdpfLuminance) {
        out.bitsPerPixel = static_cast<int>(pf.rgbBitCount ? pf.rgbBitCount : 8);
        if (out.bitsPerPixel == 16) {
            out.format = DdsFormat::A8L8;
            out.hasAlphaChannel = true;
        } else if (out.bitsPerPixel == 8) {
            out.format = DdsFormat::L8;
        } else {
            if (errorTr) *errorTr = FormatW(T(L"Desteklenmeyen luminance derinligi: %d bit."), out.bitsPerPixel);
            return false;
        }
    } else if ((pf.flags & kDdpfAlpha) && !(pf.flags & kDdpfRgb)) {
        out.format = DdsFormat::A8;
        out.bitsPerPixel = static_cast<int>(pf.rgbBitCount ? pf.rgbBitCount : 8);
        out.hasAlphaChannel = true;
        if (out.bitsPerPixel != 8) {
            if (errorTr) *errorTr = FormatW(T(L"Desteklenmeyen alfa derinligi: %d bit."), out.bitsPerPixel);
            return false;
        }
    } else if (pf.flags & kDdpfBumpDudv) {
        out.bitsPerPixel = static_cast<int>(pf.rgbBitCount ? pf.rgbBitCount : 16);
        if (out.bitsPerPixel != 16) {
            if (errorTr) *errorTr = FormatW(T(L"Desteklenmeyen bump derinligi: %d bit."), out.bitsPerPixel);
            return false;
        }
        out.format = DdsFormat::V8U8;
    } else if (pf.flags & kDdpfRgb) {
        out.bitsPerPixel = static_cast<int>(pf.rgbBitCount);
        if (out.bitsPerPixel != 16 && out.bitsPerPixel != 24 && out.bitsPerPixel != 32) {
            if (errorTr) *errorTr = FormatW(T(L"Desteklenmeyen piksel derinligi: %d bit."), out.bitsPerPixel);
            return false;
        }
        out.rMask = pf.rBitMask;
        out.gMask = pf.gBitMask;
        out.bMask = pf.bBitMask;
        out.aMask = (pf.flags & kDdpfAlphaPixels) ? pf.aBitMask : 0;
        if (out.rMask == 0 && out.gMask == 0 && out.bMask == 0) {
            if (errorTr) *errorTr = T(L"DDS piksel maskeleri bos.");
            return false;
        }
        out.hasAlphaChannel = out.aMask != 0;
        out.format = ClassifyMasked(static_cast<u32>(out.bitsPerPixel),
                                    out.rMask, out.gMask, out.bMask, out.aMask);
    } else {
        if (errorTr) *errorTr = T(L"DDS piksel formati taninmadi.");
        return false;
    }

    // Fill in the masks for the named 16/24/32 bit formats that came in via a
    // DX10 header or a luminance flag, so the decoder can use one path.
    if (out.bitsPerPixel == 0) out.bitsPerPixel = static_cast<int>(DdsBitsPerPixel(out.format));

    // --- mip chain ---------------------------------------------------------
    int mips = 1;
    if ((h.flags & kDdsdMipMapCount) && h.mipMapCount > 0)
        mips = static_cast<int>((std::min)(h.mipMapCount, 32u));
    else if (h.mipMapCount > 1)
        mips = static_cast<int>((std::min)(h.mipMapCount, 32u));
    const int maxMips = MaxMipLevels(out.width, out.height);
    out.mipCount = (std::max)(1, (std::min)(mips, maxMips));

    if (out.formatName.empty()) out.formatName = DdsFormatName(out.format);

    // --- sanity: the declared surfaces must fit in the buffer ---------------
    const size_t last = out.surfaceOffset(out.faceCount - 1, out.mipCount - 1);
    if (last == static_cast<size_t>(-1)) {
        if (errorTr) *errorTr = T(L"DDS yuzey duzeni hesaplanamadi.");
        return false;
    }
    int lw = 0, lh = 0;
    out.mipSize(out.mipCount - 1, lw, lh);
    const size_t lastBytes = SurfaceBytesOf(out, lw, lh);
    if (lastBytes == 0) {
        if (errorTr) *errorTr = T(L"DDS yuzey boyutu hesaplanamadi.");
        return false;
    }
    if (last + lastBytes > size) {
        // Truncated file: keep only the mips/faces that are actually present
        // instead of failing outright, so a partial texture still previews.
        int usableMips = 0;
        for (int m = 0; m < out.mipCount; ++m) {
            int w = 0, hh = 0;
            out.mipSize(m, w, hh);
            const size_t off = out.surfaceOffset(0, m);
            if (off == static_cast<size_t>(-1) || off + SurfaceBytesOf(out, w, hh) > size) break;
            ++usableMips;
        }
        if (usableMips == 0) {
            if (errorTr) *errorTr = FormatW(T(L"DDS verisi eksik: %llu bayt var, en az %llu gerekiyor."),
                                            static_cast<unsigned long long>(size),
                                            static_cast<unsigned long long>(out.dataOffset +
                                                SurfaceBytesOf(out, out.width, out.height)));
            return false;
        }
        out.mipCount  = usableMips;
        out.faceCount = 1;
        out.isCube    = false;
        if (errorTr) *errorTr = T(L"DDS verisi eksik; sadece mevcut mip seviyeleri okundu.");
    }

    return true;
}

// ---------------------------------------------------------------------------
// Block decoders (public so tests can call them directly)
// ---------------------------------------------------------------------------
void DecodeBlockDxt1(const u8* src, u8* out) {
    if (!src || !out) return;
    DecodeColourBlock(src, out, /*allowPunchThrough*/ true);
}

void DecodeBlockDxt3(const u8* src, u8* out) {
    if (!src || !out) return;
    DecodeColourBlock(src + 8, out, /*allowPunchThrough*/ false);
    for (int i = 0; i < 16; ++i) {
        const u8 byte = src[i >> 1];
        const u32 nib = (i & 1) ? (byte >> 4) : (byte & 0x0F);
        out[i * 4 + 3] = static_cast<u8>(nib * 17u);   // 4 bit -> 8 bit
    }
}

void DecodeBlockDxt5(const u8* src, u8* out) {
    if (!src || !out) return;
    DecodeColourBlock(src + 8, out, /*allowPunchThrough*/ false);
    u8 alpha[16];
    DecodeInterpolatedBlock(src, alpha);
    for (int i = 0; i < 16; ++i) out[i * 4 + 3] = alpha[i];
}

void DecodeBlockAti1(const u8* src, u8* out) {
    if (!src || !out) return;
    u8 red[16];
    DecodeInterpolatedBlock(src, red);
    // A single channel texture is most useful shown as grey scale.
    for (int i = 0; i < 16; ++i) {
        out[i * 4 + 0] = red[i];
        out[i * 4 + 1] = red[i];
        out[i * 4 + 2] = red[i];
        out[i * 4 + 3] = 255;
    }
}

void DecodeBlockAti2(const u8* src, u8* out) {
    if (!src || !out) return;
    u8 red[16], green[16];
    DecodeInterpolatedBlock(src, red);
    DecodeInterpolatedBlock(src + 8, green);
    // ATI2 is a tangent space normal map: X in red, Y in green, Z rebuilt so
    // the preview looks like the normal map it is instead of a flat blue mess.
    for (int i = 0; i < 16; ++i) {
        const float nx = (red[i]   / 255.0f) * 2.0f - 1.0f;
        const float ny = (green[i] / 255.0f) * 2.0f - 1.0f;
        float zz = 1.0f - nx * nx - ny * ny;
        if (zz < 0.0f) zz = 0.0f;
        const float nz = std::sqrt(zz);
        out[i * 4 + 0] = red[i];
        out[i * 4 + 1] = green[i];
        out[i * 4 + 2] = static_cast<u8>((nz * 0.5f + 0.5f) * 255.0f + 0.5f);
        out[i * 4 + 3] = 255;
    }
}

// ---------------------------------------------------------------------------
// DecodeDdsSurface
// ---------------------------------------------------------------------------
namespace {

/// Copy a decoded 4x4 tile into the image, clipping at the right/bottom edge.
inline void BlitTile(Image& img, const u8 tile[64], int bx, int by) {
    const int w = img.width();
    const int h = img.height();
    for (int ty = 0; ty < 4; ++ty) {
        const int y = by + ty;
        if (y >= h) break;
        u8* dst = img.row(y) + static_cast<size_t>(bx) * 4;
        const int run = (std::min)(4, w - bx);
        for (int tx = 0; tx < run; ++tx) {
            const u8* s = tile + (ty * 4 + tx) * 4;
            dst[tx * 4 + 0] = s[0];
            dst[tx * 4 + 1] = s[1];
            dst[tx * 4 + 2] = s[2];
            dst[tx * 4 + 3] = s[3];
        }
    }
}

bool DecodeBlockSurface(const u8* src, size_t avail, const DdsInfo& info,
                        int w, int h, Image& img, std::wstring* errorTr) {
    const size_t blockBytes = DdsBlockBytes(info.format);
    const int bw = (w + 3) / 4;
    const int bh = (h + 3) / 4;
    const size_t need = static_cast<size_t>(bw) * static_cast<size_t>(bh) * blockBytes;
    if (need > avail) {
        if (errorTr) *errorTr = T(L"Sikistirilmis DDS yuzeyi eksik.");
        return false;
    }

    void (*decoder)(const u8*, u8*) = nullptr;
    switch (info.format) {
        case DdsFormat::Dxt1: decoder = &DecodeBlockDxt1; break;
        case DdsFormat::Dxt2:
        case DdsFormat::Dxt3: decoder = &DecodeBlockDxt3; break;
        case DdsFormat::Dxt4:
        case DdsFormat::Dxt5: decoder = &DecodeBlockDxt5; break;
        case DdsFormat::Ati1: decoder = &DecodeBlockAti1; break;
        case DdsFormat::Ati2: decoder = &DecodeBlockAti2; break;
        default:
            if (errorTr) *errorTr = T(L"Desteklenmeyen blok formati.");
            return false;
    }

    u8 tile[64];
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const u8* blk = src + (static_cast<size_t>(by) * bw + bx) * blockBytes;
            decoder(blk, tile);
            BlitTile(img, tile, bx * 4, by * 4);
        }
    }

    // Premultiplied variants: undo the multiply so the preview matches what
    // the artist authored (and so the checkerboard composite is right).
    if (info.premultipliedAlpha) {
        u8* p = img.data();
        const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
        for (size_t i = 0; i < n; ++i) {
            const u32 a = p[i * 4 + 3];
            if (a == 0 || a == 255) continue;
            for (int c = 0; c < 3; ++c) {
                const u32 v = (static_cast<u32>(p[i * 4 + c]) * 255u + a / 2) / a;
                p[i * 4 + c] = static_cast<u8>(v > 255u ? 255u : v);
            }
        }
    }
    return true;
}

/// P8 / P4. `palette` points at paletteEntries RGBA quads.
///
/// Nibble order for P4 is LOW nibble = left pixel. That was determined by
/// experiment on icon\item\etc\token_crystal.ddj: the high-nibble-first
/// reading produced a vertical comb pattern, the low-nibble-first reading a
/// clean gem. Palette byte order is R,G,B,peFlags (verified on
/// icon\item\avatar\mall_avatar_m_lightingale.ddj - a gold framed icon that
/// would come out blue framed if the order were B,G,R).
bool DecodePalettedSurface(const u8* src, size_t avail, const u8* palette,
                           const DdsInfo& info, int w, int h,
                           Image& img, std::wstring* errorTr) {
    const int bpp = info.bitsPerPixel;          // 8 or 4
    const size_t rowBytes = (static_cast<size_t>(w) * bpp + 7) / 8;
    if (rowBytes * static_cast<size_t>(h) > avail) {
        if (errorTr) *errorTr = T(L"Paletli DDS yuzeyi eksik.");
        return false;
    }

    // PALETTEENTRY::peFlags is normally 0, which would make every pixel
    // transparent. Only honour it as alpha when at least one entry is non zero.
    bool paletteHasAlpha = false;
    for (int i = 0; i < info.paletteEntries; ++i)
        if (palette[i * 4 + 3] != 0) { paletteHasAlpha = true; break; }

    const int maxIndex = info.paletteEntries - 1;
    for (int y = 0; y < h; ++y) {
        const u8* s = src + static_cast<size_t>(y) * rowBytes;
        u8* d = img.row(y);
        for (int x = 0; x < w; ++x) {
            int idx;
            if (bpp == 8) {
                idx = s[x];
            } else {
                const u8 byte = s[x >> 1];
                idx = (x & 1) ? (byte >> 4) : (byte & 0x0F);   // low nibble first
            }
            if (idx > maxIndex) idx = maxIndex;
            const u8* e = palette + static_cast<size_t>(idx) * 4;
            d[x * 4 + 0] = e[0];
            d[x * 4 + 1] = e[1];
            d[x * 4 + 2] = e[2];
            d[x * 4 + 3] = paletteHasAlpha ? e[3] : 255;
        }
    }
    return true;
}

bool DecodeUncompressedSurface(const u8* src, size_t avail, const DdsInfo& info,
                               int w, int h, Image& img, std::wstring* errorTr) {
    const int bpp = info.bitsPerPixel;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
        if (errorTr) *errorTr = T(L"Desteklenmeyen piksel derinligi.");
        return false;
    }
    const size_t bytesPerPixel = static_cast<size_t>(bpp) / 8;
    const size_t rowBytes = static_cast<size_t>(w) * bytesPerPixel;
    const size_t need = rowBytes * static_cast<size_t>(h);
    if (need > avail) {
        if (errorTr) *errorTr = T(L"Sikistirilmamis DDS yuzeyi eksik.");
        return false;
    }

    switch (info.format) {
        case DdsFormat::A8: {
            for (int y = 0; y < h; ++y) {
                const u8* s = src + static_cast<size_t>(y) * rowBytes;
                u8* d = img.row(y);
                for (int x = 0; x < w; ++x) {
                    // Grey scale in RGB as well: the viewer defaults to RGB and
                    // an alpha-only texture would otherwise look blank.
                    d[x * 4 + 0] = d[x * 4 + 1] = d[x * 4 + 2] = s[x];
                    d[x * 4 + 3] = s[x];
                }
            }
            return true;
        }
        case DdsFormat::L8: {
            for (int y = 0; y < h; ++y) {
                const u8* s = src + static_cast<size_t>(y) * rowBytes;
                u8* d = img.row(y);
                for (int x = 0; x < w; ++x) {
                    d[x * 4 + 0] = d[x * 4 + 1] = d[x * 4 + 2] = s[x];
                    d[x * 4 + 3] = 255;
                }
            }
            return true;
        }
        case DdsFormat::A8L8: {
            for (int y = 0; y < h; ++y) {
                const u8* s = src + static_cast<size_t>(y) * rowBytes;
                u8* d = img.row(y);
                for (int x = 0; x < w; ++x) {
                    const u8 l = s[x * 2 + 0];
                    const u8 a = s[x * 2 + 1];
                    d[x * 4 + 0] = d[x * 4 + 1] = d[x * 4 + 2] = l;
                    d[x * 4 + 3] = a;
                }
            }
            return true;
        }
        case DdsFormat::V8U8: {
            for (int y = 0; y < h; ++y) {
                const u8* s = src + static_cast<size_t>(y) * rowBytes;
                u8* d = img.row(y);
                for (int x = 0; x < w; ++x) {
                    const int u = static_cast<i8>(s[x * 2 + 0]);
                    const int v = static_cast<i8>(s[x * 2 + 1]);
                    const float nx = static_cast<float>(u) / 127.0f;
                    const float ny = static_cast<float>(v) / 127.0f;
                    float zz = 1.0f - nx * nx - ny * ny;
                    if (zz < 0.0f) zz = 0.0f;
                    const float nz = std::sqrt(zz);
                    d[x * 4 + 0] = static_cast<u8>((std::min)(255, (std::max)(0, u + 128)));
                    d[x * 4 + 1] = static_cast<u8>((std::min)(255, (std::max)(0, v + 128)));
                    d[x * 4 + 2] = static_cast<u8>((nz * 0.5f + 0.5f) * 255.0f + 0.5f);
                    d[x * 4 + 3] = 255;
                }
            }
            return true;
        }
        default: break;
    }

    // Generic mask driven path: covers A8R8G8B8, X8R8G8B8, R8G8B8, A1R5G5B5,
    // A4R4G4B4, R5G6B5 and any unusual channel ordering.
    MaskChannel mr, mg, mb, ma;
    mr.set(info.rMask);
    mg.set(info.gMask);
    mb.set(info.bMask);
    ma.set(info.aMask);
    if (!mr.mask && !mg.mask && !mb.mask) {
        if (errorTr) *errorTr = T(L"DDS piksel maskeleri eksik.");
        return false;
    }

    for (int y = 0; y < h; ++y) {
        const u8* s = src + static_cast<size_t>(y) * rowBytes;
        u8* d = img.row(y);
        for (int x = 0; x < w; ++x) {
            const u32 px = ReadPixelBytes(s + static_cast<size_t>(x) * bytesPerPixel, bytesPerPixel);
            d[x * 4 + 0] = mr.extract(px, 0);
            d[x * 4 + 1] = mg.extract(px, 0);
            d[x * 4 + 2] = mb.extract(px, 0);
            d[x * 4 + 3] = ma.extract(px, 255);
        }
    }
    return true;
}

} // namespace

bool DecodeDdsSurface(const u8* data, size_t size, const DdsInfo& info,
                      int mip, int face, Image& out, std::wstring* errorTr) {
    out.clear();
    if (!data || size == 0) {
        if (errorTr) *errorTr = T(L"Bos DDS arabellegi.");
        return false;
    }
    if (mip < 0 || mip >= info.mipCount || face < 0 || face >= info.faceCount) {
        if (errorTr) *errorTr = T(L"Gecersiz mip / yuz numarasi.");
        return false;
    }

    int w = 0, h = 0;
    info.mipSize(mip, w, h);
    const size_t off = info.surfaceOffset(face, mip);
    if (off == static_cast<size_t>(-1) || off >= size) {
        if (errorTr) *errorTr = T(L"DDS yuzeyi dosya sinirinin disinda.");
        return false;
    }
    const size_t avail = size - off;

    if (!out.resize(w, h)) {
        if (errorTr) *errorTr = FormatW(T(L"Goruntu ayrilamadi: %d x %d."), w, h);
        return false;
    }

    bool okDecode;
    if (DdsBlockBytes(info.format)) {
        okDecode = DecodeBlockSurface(data + off, avail, info, w, h, out, errorTr);
    } else if (info.format == DdsFormat::P8 || info.format == DdsFormat::P4) {
        const size_t paletteBytes = static_cast<size_t>(info.paletteEntries) * 4;
        if (info.paletteEntries <= 0 || info.paletteOffset + paletteBytes > size) {
            if (errorTr) *errorTr = T(L"DDS renk paleti dosya sinirinin disinda.");
            out.clear();
            return false;
        }
        okDecode = DecodePalettedSurface(data + off, avail, data + info.paletteOffset,
                                         info, w, h, out, errorTr);
    } else {
        okDecode = DecodeUncompressedSurface(data + off, avail, info, w, h, out, errorTr);
    }

    if (!okDecode) { out.clear(); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// DecodeDds - parse + mip 0 / face 0
// ---------------------------------------------------------------------------
bool DecodeDds(const u8* data, size_t size, Image& out,
               ImageMeta* metaOut, std::wstring* errorTr) {
    out.clear();
    if (metaOut) *metaOut = ImageMeta();

    DdsInfo info;
    std::wstring parseWarning;
    if (!ParseDds(data, size, info, &parseWarning)) {
        if (errorTr) *errorTr = parseWarning;
        return false;
    }
    if (!DecodeDdsSurface(data, size, info, 0, 0, out, errorTr)) return false;

    if (metaOut) {
        metaOut->formatName = info.formatName.empty()
                            ? std::wstring(DdsFormatName(info.format)) : info.formatName;
        metaOut->width     = info.width;
        metaOut->height    = info.height;
        metaOut->mipCount  = info.mipCount;
        metaOut->faceCount = info.faceCount;
        metaOut->depth     = info.depth;
        metaOut->dataSize  = static_cast<u64>(size);
        metaOut->hasAlpha  = info.hasAlphaChannel;
        metaOut->isCube    = info.isCube;
        metaOut->isVolume  = info.isVolume;
    }
    if (errorTr) *errorTr = parseWarning;   // may carry a "truncated" note
    return true;
}

} // namespace sro
