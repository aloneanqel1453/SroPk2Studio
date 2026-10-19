// SkillEffect Studio v6 - ddj/DdjCodec.h  (M33)
// .ddj / .dds writer with the exact header layout of the shipped Silkroad files (measured on 17 520 files of
// CLEAN-CLIENT Media / Data / Particles):
//   "JMXVDDJ 1000" + u32 (file size - 12) + u32 3 + DDS
//   DDS: size 124, flags 0x1007 (+0x20000 with mips), pitch 0, depth 0, mip count 0 or N (full chain to 1x1),
//   pixel format FOURCC (4) or RGB (0x40) [+ALPHAPIXELS 1], caps 0x1000 [+ALPHA 2 for alpha RGB]
//   [+COMPLEX|MIPMAP 0x400008 with mips], data tightly packed, no trailing bytes.
// Formats: DXT1 / DXT1a / DXT2 / DXT3 / DXT4 / DXT5, A8R8G8B8, X8R8G8B8, A1R5G5B5, X1R5G5B5, R5G6B5, A4R4G4B4.
// Nothing here touches a client; WriteDdjFile writes atomically (temp + MoveFileEx, optional .bak).
#pragma once

#include "Dxt.h"
#include "ImageIo.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace se::ddj {

enum class DdjFormat : int {
    Auto = 0,     // no alpha -> DXT1, alpha -> DXT5
    Dxt1, Dxt1a, Dxt2, Dxt3, Dxt4, Dxt5,
    Argb8888, Xrgb8888, Argb1555, Xrgb1555, Rgb565, Argb4444,
    Unknown = 99  // ReadDdjInfo only (a layout this writer cannot reproduce, e.g. P8, DX10, ATI2)
};

const wchar_t* FormatName(DdjFormat f);                 // "dxt5", "argb8888" ...
std::wstring LayoutName(DdjFormat f);                    // D3D name: "DXT5", "A8R8G8B8", "A1R5G5B5" ...
bool ParseFormat(std::wstring_view s, DdjFormat& f);     // also "a8r8g8b8", "bc3", "rgb32" ...
bool IsBlockFormat(DdjFormat f);
bool FormatHasAlpha(DdjFormat f);

enum class MipFilter : int { Box = 0, Mitchell };

struct DdjOptions {
    DdjFormat format = DdjFormat::Auto;
    int mipLevels = 1;              // 1 = none (icons, interface), 0 = full chain to 1x1 (model textures), N = N levels
    MipFilter mipFilter = MipFilter::Box;
    bool mipKeepCoverage = false;   // alpha-test textures: keep the share of texels >= coverageRef in every level
    int coverageRef = 128;
    bool jmxv = true;               // false = plain .dds
    DxtParams dxt;                  // quality / perceptual / alpha weighting / DXT1a threshold
    bool dither16 = false;          // Floyd-Steinberg error diffusion for the 16-bit formats
    std::uint8_t xFill = 0x00;      // unused bits of X8R8G8B8 / X1R5G5B5 (shipped files: 0)
    int threads = 0;                // block rows in parallel; 0 = auto (<= 6)
    const std::atomic<bool>* cancel = nullptr;
    // Replacing an existing file: its header bytes (20 + 128, or 128) and file size. The new header then keeps
    // that file's field style (pitch / linear size, depth, reserved words, caps, pixel-format quirks, an
    // inconsistent JMXV size field) wherever the layout allows; sizes / mips / format are updated when they change.
    // Empty = the shipped Joymax layout (see top).
    Bytes headerLike;
    std::size_t headerLikeSize = 0;
};

// Information of an existing .ddj / .dds (header only; never decodes pixels).
struct DdjInfo {
    bool jmxv = false;              // "JMXVDDJ 1000" wrapper present
    std::uint32_t jmxvSize = 0, jmxvType = 0;   // u32 at 12 (file size - 12) and 16 (3)
    int w = 0, h = 0, mips = 1;     // mips = levels present (>= 1)
    std::uint32_t mipField = 0;     // raw DDS dwMipMapCount
    std::uint32_t flags = 0, pfFlags = 0, fourcc = 0, bits = 0, masks[4]{}, caps1 = 0, caps2 = 0, pitch = 0;
    DdjFormat format = DdjFormat::Unknown;
    std::string layout;             // "DXT3", "A8R8G8B8", "P8", "DX10" ...
    std::size_t headerBytes = 0;    // 20 + 128 (or 128)
    std::size_t expectedSize = 0;   // header + every level
    std::size_t fileSize = 0;
    bool sizeOk = false;            // expectedSize == fileSize
    bool headerLikeShipped = false; // every header field as the shipped writer sets it (see top)
    std::wstring noteTr;            // plain Turkish: what differs from the shipped files
};

bool ReadDdjInfo(const std::uint8_t* p, std::size_t n, DdjInfo& out, std::wstring* whyTr = nullptr);
// options that reproduce an existing file's layout (format + mip levels + wrapper); with the file's bytes also
// its header style (headerLike), so a replacement is header-compatible with the file it replaces
DdjOptions OptionsLike(const DdjInfo& info, const std::uint8_t* original = nullptr, std::size_t originalSize = 0);

// mip chain from level 0 (levels: 0 = full chain; returns at least the base)
std::vector<Image> BuildMips(const Image& base, int levels, MipFilter f = MipFilter::Box, bool keepCoverage = false, int coverageRef = 128);
int FullMipCount(int w, int h);

// encode (base image -> mips per options)
bool EncodeDdj(const Image& img, const DdjOptions& o, Bytes& out, std::wstring* whyTr = nullptr);
// encode given levels as they are (exact rewrite of a decoded file); levels[i] must be max(1, w >> i) x max(1, h >> i)
bool EncodeDdjLevels(const std::vector<Image>& levels, const DdjOptions& o, Bytes& out, std::wstring* whyTr = nullptr);
// decode every mip level (render decoder; DXT2/4 come back straight alpha)
bool DecodeDdjLevels(const std::uint8_t* p, std::size_t n, std::vector<Image>& levels, std::wstring* whyTr = nullptr);
// the format Auto picks for an image
DdjFormat ResolveFormat(DdjFormat f, const Image& img);

// Safe write: refuses a folder inside a game client (pk2 / sro_client.exe beside it or in a parent), atomic,
// .bak of an existing file. whyTr says why it refused.
bool IsSafeOutputPath(const std::wstring& path, std::wstring* whyTr = nullptr);
bool WriteDdjFile(const std::wstring& path, const Bytes& data, std::wstring* whyTr = nullptr, bool backup = true);

}  // namespace se::ddj
