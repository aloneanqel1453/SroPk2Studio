// SkillEffect Studio - render/RImage.h
// RGBA8 images: JMXVDDJ / DDS decode (DXT1-5, ATI1/2, the uncompressed masks of the shipped clients;
// identical rounding to efsanesro dds.py) and a dependency-free PNG writer (zlib fixed-Huffman LZ77).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace se::render {

struct Image {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;  // top-down rows, 4 bytes per pixel (R,G,B,A)
    bool Empty() const { return w <= 0 || h <= 0 || rgba.size() < static_cast<std::size_t>(w) * h * 4; }
    std::uint8_t* Px(int x, int y) { return &rgba[(static_cast<std::size_t>(y) * w + x) * 4]; }
    const std::uint8_t* Px(int x, int y) const { return &rgba[(static_cast<std::size_t>(y) * w + x) * 4]; }
};

// .ddj (20-byte JMXVDDJ header + DDS) or a bare .dds. mip = level (clamped). false + err on failure.
bool DecodeDdj(const std::uint8_t* data, std::size_t size, Image& out, std::string& err, int mip = 0);
bool DecodeDds(const std::uint8_t* dds, std::size_t size, Image& out, std::string& err, int mip = 0);
// number of mip levels in the DDS (1 when unknown)
int DdsMipCount(const std::uint8_t* data, std::size_t size);

// PNG (RGB when every alpha is 255 or forceRgb, else RGBA). Atomic: <path>.tmp then MoveFileEx.
bool WritePng(const std::wstring& path, const Image& img, std::string& err, bool forceRgb = true);
bool EncodePng(const Image& img, std::vector<std::uint8_t>& out, bool forceRgb = true);
// minimal PNG reader (8-bit RGB/RGBA, non-interlaced, any filter) - used by the tests to compare renders
bool ReadPng(const std::wstring& path, Image& out, std::string& err);

}  // namespace se::render
