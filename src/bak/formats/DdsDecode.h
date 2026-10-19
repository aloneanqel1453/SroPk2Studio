// ============================================================================
//  SRO Asset Studio - formats/DdsDecode.h
//
//  Self-contained DDS parser + decoder. We do NOT use WIC for DDS: WIC's DDS
//  support is incomplete and unavailable on some SKUs, and we need ATI1/ATI2.
//
//  Required coverage (SPEC.md §3):
//    block compressed : DXT1, DXT2, DXT3, DXT4, DXT5, ATI1/BC4, ATI2/BC5
//    uncompressed     : A8R8G8B8, X8R8G8B8, R8G8B8, A1R5G5B5, A4R4G4B4,
//                       R5G6B5, A8, L8, A8L8, V8U8
//    plus mipmap chains (select a mip) and cube maps (6 faces).
//
//  Output is ALWAYS RGBA8 in an Image.
//
//  Note on DXT2/DXT4: these are premultiplied-alpha variants that decode with
//  the same block layout as DXT3/DXT5. Decode identically and flag it in
//  DdsInfo::premultipliedAlpha so the viewer can say so.
//
//  Implementation owner: Agent D (formats).
// ============================================================================
#pragma once

#include "../Common.h"
#include "../render/Image.h"

namespace sro {

// ---------------------------------------------------------------------------
// Raw DDS structures (Direct3D 9 era, which is what Silkroad ships)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)

struct DdsPixelFormat {
    u32 size;          ///< 32
    u32 flags;         ///< DDPF_*
    u32 fourCC;
    u32 rgbBitCount;
    u32 rBitMask;
    u32 gBitMask;
    u32 bBitMask;
    u32 aBitMask;
};

struct DdsHeader {
    u32 size;          ///< 124
    u32 flags;
    u32 height;
    u32 width;
    u32 pitchOrLinearSize;
    u32 depth;
    u32 mipMapCount;
    u32 reserved1[11];
    DdsPixelFormat pf;
    u32 caps;
    u32 caps2;
    u32 caps3;
    u32 caps4;
    u32 reserved2;
};

/// Present only when pf.fourCC == 'DX10'.
struct DdsHeaderDxt10 {
    u32 dxgiFormat;
    u32 resourceDimension;
    u32 miscFlag;
    u32 arraySize;
    u32 miscFlags2;
};

#pragma pack(pop)

static_assert(sizeof(DdsPixelFormat) == 32,  "DdsPixelFormat must be 32 bytes");
static_assert(sizeof(DdsHeader)      == 124, "DdsHeader must be 124 bytes");

constexpr u32 kDdsMagic = 0x20534444; ///< "DDS "

// DDSD_ flags
constexpr u32 kDdsdCaps        = 0x00000001;
constexpr u32 kDdsdHeight      = 0x00000002;
constexpr u32 kDdsdWidth       = 0x00000004;
constexpr u32 kDdsdPitch       = 0x00000008;
constexpr u32 kDdsdPixelFormat = 0x00001000;
constexpr u32 kDdsdMipMapCount = 0x00020000;
constexpr u32 kDdsdLinearSize  = 0x00080000;
constexpr u32 kDdsdDepth       = 0x00800000;

// DDPF_ flags
constexpr u32 kDdpfAlphaPixels = 0x00000001;
constexpr u32 kDdpfAlpha       = 0x00000002;
constexpr u32 kDdpfFourCC      = 0x00000004;
// ADDITIVE (textures agent): the palettised flags. Note 0x20 is
// DDPF_PALETTEINDEXED8, NOT luminance - luminance is 0x20000.
constexpr u32 kDdpfPaletteIndexed4 = 0x00000008;
constexpr u32 kDdpfPaletteIndexed8 = 0x00000020;
constexpr u32 kDdpfRgb         = 0x00000040;
constexpr u32 kDdpfYuv         = 0x00000200;
constexpr u32 kDdpfLuminance   = 0x00020000;
constexpr u32 kDdpfBumpDudv    = 0x00080000;

// DDSCAPS2_
constexpr u32 kDdsCaps2Cubemap          = 0x00000200;
constexpr u32 kDdsCaps2CubemapPositiveX = 0x00000400;
constexpr u32 kDdsCaps2Volume           = 0x00200000;

// ---------------------------------------------------------------------------
// Decoded format identity
// ---------------------------------------------------------------------------
enum class DdsFormat : int {
    Unknown = 0,
    // block compressed
    Dxt1, Dxt2, Dxt3, Dxt4, Dxt5, Ati1, Ati2,
    // uncompressed
    A8R8G8B8, X8R8G8B8, R8G8B8, A1R5G5B5, A4R4G4B4, R5G6B5,
    A8, L8, A8L8, V8U8,
    // anything else we recognise structurally but cannot decode
    UnsupportedFourCC,
    /// ADDITIVE (textures agent): an uncompressed RGB(A) surface whose channel
    /// masks do not match one of the named D3D9 layouts (A8B8G8R8, B8G8R8,
    /// X1R5G5B5, ...). It still decodes correctly - the decoder always works
    /// from DdsInfo::rMask/gMask/bMask/aMask, never from the enum - this value
    /// only tells the readout that there is no canonical name for it.
    /// Appended at the end so every existing enumerator keeps its value.
    RgbMasked,
    /// ADDITIVE (textures agent): D3D9 palettised surfaces. A census of 61.395
    /// real .ddj files across Media/Data/Map/Particles found 144 of them
    /// (DDPF_PALETTEINDEXED8 and DDPF_PALETTEINDEXED4), e.g.
    /// icon\item\avatar\mall_avatar_m_lightingale.ddj. The palette sits
    /// immediately after the DDS header: 256 (P8) or 16 (P4) RGBA quads.
    P8,
    P4
};

/// "DXT5", "A8R8G8B8", ...
const wchar_t* DdsFormatName(DdsFormat f);
/// Bytes per 4x4 block for BC formats; 0 for uncompressed.
size_t DdsBlockBytes(DdsFormat f);
/// Bits per pixel for uncompressed formats; 0 for BC formats.
size_t DdsBitsPerPixel(DdsFormat f);
/// Size in bytes of one mip surface at the given dimensions.
size_t DdsSurfaceBytes(DdsFormat f, int width, int height);

// ---------------------------------------------------------------------------
// Parsed description of a whole DDS file
// ---------------------------------------------------------------------------
struct DdsInfo {
    DdsFormat format = DdsFormat::Unknown;
    int  width      = 0;
    int  height     = 0;
    int  depth      = 1;
    int  mipCount   = 1;
    int  faceCount  = 1;      ///< 6 for a cube map
    bool isCube     = false;
    bool isVolume   = false;
    bool hasAlphaChannel = false;
    bool premultipliedAlpha = false;   ///< DXT2 / DXT4
    u32  fourCC     = 0;
    size_t dataOffset = 0;    ///< first byte of surface data within the buffer
    std::wstring formatName;

    // --- ADDITIVE (textures agent) -----------------------------------------
    // Uncompressed surfaces are decoded from these, not from `format`, so an
    // unusual channel order can never come out with swapped channels.
    // All zero for block compressed formats.
    int  bitsPerPixel = 0;    ///< 4 / 8 / 16 / 24 / 32; 0 for DXT/ATI
    u32  rMask = 0;
    u32  gMask = 0;
    u32  bMask = 0;
    u32  aMask = 0;           ///< 0 when the surface has no alpha channel

    // Palette (P8 / P4 only). `paletteOffset` is a byte offset into the same
    // buffer that was handed to ParseDds; `dataOffset` already points past it.
    size_t paletteOffset  = 0;
    int    paletteEntries = 0;   ///< 256 for P8, 16 for P4, 0 otherwise

    /// Byte offset of one surface inside the buffer.
    /// Mip chains are stored per face: face0 mip0..N, face1 mip0..N, ...
    /// Returns SIZE_MAX when the indices are out of range.
    size_t surfaceOffset(int face, int mip) const;
    /// Dimensions of a mip level (never below 1).
    void   mipSize(int mip, int& w, int& h) const;
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/// Parse the header of a DDS buffer. Bounds-checked; never reads past `size`.
/// Returns false and fills `errorTr` (Turkish) on a malformed file.
bool ParseDds(const u8* data, size_t size, DdsInfo& out, std::wstring* errorTr);

/// Decode one surface into RGBA8.
///   `mip`  0 .. info.mipCount-1
///   `face` 0 .. info.faceCount-1
/// Returns false on unsupported formats or truncated data. Never over-reads.
bool DecodeDdsSurface(const u8* data, size_t size, const DdsInfo& info,
                      int mip, int face, Image& out, std::wstring* errorTr);

/// Convenience: parse + decode mip 0 face 0.
bool DecodeDds(const u8* data, size_t size, Image& out,
               ImageMeta* metaOut, std::wstring* errorTr);

// ---------------------------------------------------------------------------
// Block decoders, exposed so tests can hit them directly.
// Each writes a 4x4 RGBA8 tile into `out` (16 pixels, 64 bytes, row major).
// `src` must point at the compressed block; callers guarantee 8 or 16 bytes.
// ---------------------------------------------------------------------------
void DecodeBlockDxt1(const u8* src, u8* out /*[64]*/);
void DecodeBlockDxt3(const u8* src, u8* out /*[64]*/);
void DecodeBlockDxt5(const u8* src, u8* out /*[64]*/);
void DecodeBlockAti1(const u8* src, u8* out /*[64]*/);
void DecodeBlockAti2(const u8* src, u8* out /*[64]*/);

} // namespace sro
