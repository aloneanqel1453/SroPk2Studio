#pragma once
// DDS / DDJ texture decoder -> BGRA8. DDJ is a DDS wrapped with a short JMXVDDJ
// header; we locate the "DDS " magic and decode BC1/BC2/BC3 and uncompressed RGBA.
#include <cstdint>
#include <optional>
#include <vector>

namespace pk2 {

struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bgra; // width*height*4, premultiplied-friendly straight BGRA
    const char* format = "";        // e.g. "DXT1", "DXT5", "BGRA8"
};

// Decode a DDS or DDJ buffer. Returns nullopt if not a recognizable DDS/DDJ.
[[nodiscard]] std::optional<Image> decode_dds(const std::uint8_t* data, std::size_t len);

// Decode a BMP (BI_RGB 24/32-bit) buffer. Returns nullopt if not a BMP.
[[nodiscard]] std::optional<Image> decode_bmp(const std::uint8_t* data, std::size_t len);

// Decode any supported image: DDS/DDJ, then BMP. Returns nullopt otherwise.
[[nodiscard]] std::optional<Image> decode_image(const std::uint8_t* data, std::size_t len);

} // namespace pk2
