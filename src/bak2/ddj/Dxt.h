// SkillEffect Studio v6 - ddj/Dxt.h  (M33)
// Block compressors for the DDS layouts the Silkroad client reads: BC1 (DXT1, optional 1-bit alpha), BC2 (DXT2/3)
// and BC3 (DXT4/5). Colour endpoints: PCA + quantisation-aware cluster fit (4 and 3 colour), then a hill climb on
// the 5:6:5 grid with the decoder's exact rounding (render/RImage.cpp ColorBlock: (2*c0 + c1 + 1) / 3).
// Alpha (BC3): both 8- and 6-value modes, endpoint window search. No third-party code.
#pragma once

#include <cstdint>

namespace se::ddj {

enum class DxtQuality : int { Fast = 0, Normal = 1, Best = 2 };

struct DxtParams {
    DxtQuality quality = DxtQuality::Normal;
    bool perceptual = false;     // weight R,G,B 0.299 / 0.587 / 0.114 (default uniform = best PSNR)
    bool weightByAlpha = false;  // colour error weighted by alpha (soft-alpha DXT3/5 textures)
    int alphaRef = 128;          // BC1 with alpha: alpha < ref -> transparent texel (3-colour block, index 3)
    // BC1 without alpha: near-black texels (max channel <= blackLevel) may use index 3 of a 3-colour block, which
    // decodes as black with alpha 0. Only for materials without alpha test / blending (else they vanish). Off.
    bool threeColorBlack = false;
    int blackLevel = 6;
};

// px = 16 texels RGBA, row-major 4x4 (64 bytes). Output little-endian block bytes.
// allowTransparent = BC1 with 1-bit alpha (DXT1a); false = alpha ignored, every texel opaque.
void EncodeBc1(const std::uint8_t px[64], std::uint8_t out[8], const DxtParams& p, bool allowTransparent);
// colour part of BC2 / BC3 (always 4-colour, c0 > c1 kept so old 3-colour decoders show the same)
void EncodeColorBc23(const std::uint8_t px[64], std::uint8_t out[8], const DxtParams& p);
void EncodeBc2(const std::uint8_t px[64], std::uint8_t out[16], const DxtParams& p);   // DXT3 (explicit 4-bit alpha)
void EncodeBc3(const std::uint8_t px[64], std::uint8_t out[16], const DxtParams& p);   // DXT5 (interpolated alpha)
void EncodeAlphaBc3(const std::uint8_t a[16], std::uint8_t out[8], DxtQuality q);      // 8-byte alpha block

// decoders with the render module's rounding (tests / error evaluation). fourAlways = BC2/BC3 colour block.
void DecodeColorBlock(const std::uint8_t in[8], std::uint8_t px[64], bool fourAlways);
void DecodeAlphaBc3(const std::uint8_t in[8], std::uint8_t a[16]);

}  // namespace se::ddj
