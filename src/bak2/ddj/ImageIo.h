// SkillEffect Studio v6 - ddj/ImageIo.h  (M33 / M38)
// Picture input for the .ddj writer and image helpers: own PNG (zlib inflate, every colour type / bit depth,
// Adam7, tRNS), BMP (1-32 bit, BI_RGB / BITFIELDS / RLE4 / RLE8, top-down), TGA (types 1/2/3/9/10/11, both
// origins) readers, DDS / DDJ through the render decoder, Windows Imaging Component fallback (JPG / GIF / TIFF ...).
// Pixels: se::render::Image (RGBA, top-down, straight alpha). Resampling with premultiplied alpha. Read-only.
#pragma once

#include "../render/RImage.h"

#include <cstdint>
#include <string>
#include <vector>

namespace se::ddj {

using Image = se::render::Image;
using Bytes = std::vector<std::uint8_t>;

enum class PicKind : int { Unknown = 0, Png, Bmp, Tga, Dds, Ddj, Other };

// sniff by content (TGA has no magic: header plausibility + file extension hint)
PicKind SniffPicture(const std::uint8_t* p, std::size_t n, const std::wstring& nameHint = L"");
const wchar_t* PicKindName(PicKind k);

// Any supported picture -> RGBA. whyTr = plain Turkish reason on failure. useWic: try WIC for other formats.
bool DecodePicture(const std::uint8_t* p, std::size_t n, Image& out, std::wstring* whyTr = nullptr,
                   const std::wstring& nameHint = L"", bool useWic = true, PicKind* kind = nullptr);
bool LoadPicture(const std::wstring& path, Image& out, std::wstring* whyTr = nullptr, PicKind* kind = nullptr);

bool DecodePng(const std::uint8_t* p, std::size_t n, Image& out, std::wstring* whyTr = nullptr);
bool DecodeBmp(const std::uint8_t* p, std::size_t n, Image& out, std::wstring* whyTr = nullptr);
bool DecodeTga(const std::uint8_t* p, std::size_t n, Image& out, std::wstring* whyTr = nullptr);
bool DecodeWic(const std::uint8_t* p, std::size_t n, Image& out, std::wstring* whyTr = nullptr);
// zlib stream (RFC 1950) -> raw bytes; adler32 checked
bool Inflate(const std::uint8_t* p, std::size_t n, Bytes& out, std::wstring* whyTr = nullptr, std::size_t expect = 0);

// PNG out (RGBA when any alpha < 255) and atomic file writes (temp + MoveFileEx; optional .bak of an old file)
bool EncodePngRgba(const Image& img, Bytes& out);
bool WriteFileAtomic(const std::wstring& path, const Bytes& data, std::wstring* whyTr = nullptr, bool backup = false);
bool ReadFileBytes(const std::wstring& path, Bytes& out, std::wstring* whyTr = nullptr, std::size_t maxBytes = 512u << 20);

// ------------------------------------------------------------------------------------------- image helpers
enum class Filter : int { Box = 0, Triangle, Mitchell, Lanczos3 };
Image MakeImage(int w, int h, std::uint32_t rgba = 0);   // rgba = 0xAABBGGRR (R in the low byte)
// separable resample in premultiplied alpha (box = exact area average when shrinking)
Image Resample(const Image& src, int w, int h, Filter f = Filter::Mitchell);
enum class Fit : int { Contain = 0, Cover, Stretch };
// resize into w x h keeping the aspect (Contain: centred, transparent pad; Cover: centre crop)
Image FitInto(const Image& src, int w, int h, Fit fit = Fit::Contain, Filter f = Filter::Mitchell);
// bounding box of alpha > thr (false when empty)
bool AlphaBounds(const Image& img, int& x0, int& y0, int& x1, int& y1, int thr = 0);
Image Crop(const Image& img, int x, int y, int w, int h);   // out-of-range = transparent
void BlitOver(Image& dst, const Image& src, int x, int y);   // straight-alpha "over"
void FillRect(Image& img, int x, int y, int w, int h, std::uint32_t rgba);
bool HasAlpha(const Image& img, bool* binary = nullptr);     // any alpha < 255; binary = only 0 / 255

// PSNR in dB (99 = identical). channels: bit 0 R, 1 G, 2 B, 3 A. mse optional.
double Psnr(const Image& a, const Image& b, unsigned channels = 0xF, double* mse = nullptr);

}  // namespace se::ddj
