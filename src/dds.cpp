#include "dds.hpp"

#include <cstring>

namespace pk2 {
namespace {

std::uint16_t rd16(const std::uint8_t* p) { std::uint16_t v; std::memcpy(&v, p, 2); return v; }
std::uint32_t rd32(const std::uint8_t* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }

struct Rgba { std::uint8_t r, g, b, a; };

void unpack565(std::uint16_t c, Rgba& o) {
    std::uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    o.r = static_cast<std::uint8_t>((r << 3) | (r >> 2));
    o.g = static_cast<std::uint8_t>((g << 2) | (g >> 1));
    o.b = static_cast<std::uint8_t>((b << 3) | (b >> 2));
    o.a = 255;
}

// Decode one BC1 colour block into 16 Rgba (alpha 255/ or 0 for 1-bit).
void bc1_colors(const std::uint8_t* blk, Rgba out[16], bool allow_alpha) {
    std::uint16_t c0 = rd16(blk), c1 = rd16(blk + 2);
    std::uint32_t bits = rd32(blk + 4);
    Rgba p[4];
    unpack565(c0, p[0]);
    unpack565(c1, p[1]);
    if (c0 > c1 || !allow_alpha) {
        for (int k = 0; k < 3; ++k) {
            p[2].r = static_cast<std::uint8_t>((2 * p[0].r + p[1].r) / 3);
            p[2].g = static_cast<std::uint8_t>((2 * p[0].g + p[1].g) / 3);
            p[2].b = static_cast<std::uint8_t>((2 * p[0].b + p[1].b) / 3);
            p[3].r = static_cast<std::uint8_t>((p[0].r + 2 * p[1].r) / 3);
            p[3].g = static_cast<std::uint8_t>((p[0].g + 2 * p[1].g) / 3);
            p[3].b = static_cast<std::uint8_t>((p[0].b + 2 * p[1].b) / 3);
        }
        p[2].a = p[3].a = 255;
    } else {
        p[2].r = static_cast<std::uint8_t>((p[0].r + p[1].r) / 2);
        p[2].g = static_cast<std::uint8_t>((p[0].g + p[1].g) / 2);
        p[2].b = static_cast<std::uint8_t>((p[0].b + p[1].b) / 2);
        p[2].a = 255;
        p[3] = {0, 0, 0, 0};
    }
    for (int i = 0; i < 16; ++i)
        out[i] = p[(bits >> (2 * i)) & 3];
}

std::optional<Image> decode_from_header(const std::uint8_t* h, std::size_t avail, bool /*unused*/) {
    // h points at the DDS header (right after the "DDS " magic). Needs >=124.
    if (avail < 124)
        return std::nullopt;
    std::uint32_t height = rd32(h + 8);
    std::uint32_t width = rd32(h + 12);
    if (width == 0 || height == 0 || width > 32768 || height > 32768)
        return std::nullopt;
    const std::uint8_t* pf = h + 72; // DDS_PIXELFORMAT
    std::uint32_t pfFlags = rd32(pf + 4);
    char fourcc[5] = {0};
    std::memcpy(fourcc, pf + 8, 4);
    std::uint32_t bitcount = rd32(pf + 12);
    std::uint32_t rmask = rd32(pf + 16), gmask = rd32(pf + 20), bmask = rd32(pf + 24), amask = rd32(pf + 28);
    const std::uint8_t* data = h + 124;
    std::size_t data_len = avail - 124;

    Image img;
    img.width = width;
    img.height = height;
    img.bgra.assign(static_cast<std::size_t>(width) * height * 4, 0);

    auto put = [&](std::uint32_t x, std::uint32_t y, const Rgba& c) {
        if (x >= width || y >= height)
            return;
        std::uint8_t* o = &img.bgra[(static_cast<std::size_t>(y) * width + x) * 4];
        o[0] = c.b; o[1] = c.g; o[2] = c.r; o[3] = c.a;
    };

    const bool fourccFlag = (pfFlags & 0x4) != 0; // DDPF_FOURCC
    const std::uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;

    if (fourccFlag && std::memcmp(fourcc, "DXT1", 4) == 0) {
        img.format = "DXT1";
        std::size_t need = static_cast<std::size_t>(bw) * bh * 8;
        if (data_len < need) return std::nullopt;
        for (std::uint32_t by = 0; by < bh; ++by)
            for (std::uint32_t bx = 0; bx < bw; ++bx) {
                Rgba t[16];
                bc1_colors(data + (static_cast<std::size_t>(by) * bw + bx) * 8, t, true);
                for (int i = 0; i < 16; ++i) put(bx * 4 + (i % 4), by * 4 + (i / 4), t[i]);
            }
    } else if (fourccFlag && std::memcmp(fourcc, "DXT3", 4) == 0) {
        img.format = "DXT3";
        std::size_t need = static_cast<std::size_t>(bw) * bh * 16;
        if (data_len < need) return std::nullopt;
        for (std::uint32_t by = 0; by < bh; ++by)
            for (std::uint32_t bx = 0; bx < bw; ++bx) {
                const std::uint8_t* blk = data + (static_cast<std::size_t>(by) * bw + bx) * 16;
                std::uint64_t ab; std::memcpy(&ab, blk, 8);
                Rgba t[16];
                bc1_colors(blk + 8, t, false);
                for (int i = 0; i < 16; ++i) {
                    std::uint8_t a4 = (ab >> (4 * i)) & 0xF;
                    t[i].a = static_cast<std::uint8_t>(a4 * 17);
                    put(bx * 4 + (i % 4), by * 4 + (i / 4), t[i]);
                }
            }
    } else if (fourccFlag && std::memcmp(fourcc, "DXT5", 4) == 0) {
        img.format = "DXT5";
        std::size_t need = static_cast<std::size_t>(bw) * bh * 16;
        if (data_len < need) return std::nullopt;
        for (std::uint32_t by = 0; by < bh; ++by)
            for (std::uint32_t bx = 0; bx < bw; ++bx) {
                const std::uint8_t* blk = data + (static_cast<std::size_t>(by) * bw + bx) * 16;
                std::uint8_t a0 = blk[0], a1 = blk[1];
                std::uint8_t ap[8];
                ap[0] = a0; ap[1] = a1;
                if (a0 > a1) {
                    for (int k = 1; k <= 6; ++k)
                        ap[k + 1] = static_cast<std::uint8_t>(((7 - k) * a0 + k * a1) / 7);
                } else {
                    for (int k = 1; k <= 4; ++k)
                        ap[k + 1] = static_cast<std::uint8_t>(((5 - k) * a0 + k * a1) / 5);
                    ap[6] = 0; ap[7] = 255;
                }
                std::uint64_t aidx = 0;
                std::memcpy(&aidx, blk + 2, 6);
                Rgba t[16];
                bc1_colors(blk + 8, t, false);
                for (int i = 0; i < 16; ++i) {
                    std::uint8_t ai = (aidx >> (3 * i)) & 0x7;
                    t[i].a = ap[ai];
                    put(bx * 4 + (i % 4), by * 4 + (i / 4), t[i]);
                }
            }
    } else if (!fourccFlag && bitcount == 32) {
        img.format = "BGRA8";
        std::size_t need = static_cast<std::size_t>(width) * height * 4;
        if (data_len < need) return std::nullopt;
        auto shift_of = [](std::uint32_t mask) { int s = 0; if (!mask) return 0; while (!(mask & 1)) { mask >>= 1; ++s; } return s; };
        int rs = shift_of(rmask), gs = shift_of(gmask), bs = shift_of(bmask), as = shift_of(amask);
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x) {
                std::uint32_t px = rd32(data + (static_cast<std::size_t>(y) * width + x) * 4);
                Rgba c;
                c.r = rmask ? static_cast<std::uint8_t>((px & rmask) >> rs) : 0;
                c.g = gmask ? static_cast<std::uint8_t>((px & gmask) >> gs) : 0;
                c.b = bmask ? static_cast<std::uint8_t>((px & bmask) >> bs) : 0;
                c.a = amask ? static_cast<std::uint8_t>((px & amask) >> as) : 255;
                put(x, y, c);
            }
    } else {
        return std::nullopt; // unsupported format
    }
    return img;
}

} // namespace

std::optional<Image> decode_dds(const std::uint8_t* data, std::size_t len) {
    if (!data || len < 128)
        return std::nullopt;
    // Find "DDS " magic within the first 256 bytes (DDJ wraps DDS with a header).
    std::size_t scan = len < 256 ? len : 256;
    for (std::size_t i = 0; i + 4 <= scan; ++i) {
        if (data[i] == 'D' && data[i + 1] == 'D' && data[i + 2] == 'S' && data[i + 3] == ' ')
            return decode_from_header(data + i + 4, len - (i + 4), false);
    }
    return std::nullopt;
}

std::optional<Image> decode_bmp(const std::uint8_t* data, std::size_t len) {
    if (!data || len < 54 || data[0] != 'B' || data[1] != 'M')
        return std::nullopt;
    std::uint32_t off = rd32(data + 10);
    std::uint32_t hdr = rd32(data + 14);
    if (hdr < 40)
        return std::nullopt;
    std::int32_t w, h;
    std::memcpy(&w, data + 18, 4);
    std::memcpy(&h, data + 22, 4);
    std::uint16_t bpp; std::memcpy(&bpp, data + 28, 2);
    std::uint32_t comp = rd32(data + 30);
    if (comp != 0 || (bpp != 24 && bpp != 32) || w <= 0 || w > 32768)
        return std::nullopt;
    bool bottom_up = h > 0;
    std::uint32_t H = static_cast<std::uint32_t>(h < 0 ? -h : h);
    std::uint32_t W = static_cast<std::uint32_t>(w);
    if (H == 0 || H > 32768)
        return std::nullopt;
    std::size_t rowSize = ((static_cast<std::size_t>(W) * bpp + 31) / 32) * 4;
    if (off + rowSize * H > len)
        return std::nullopt;
    Image img; img.width = W; img.height = H; img.format = "BMP"; img.bgra.assign(static_cast<std::size_t>(W) * H * 4, 255);
    for (std::uint32_t y = 0; y < H; ++y) {
        const std::uint8_t* row = data + off + rowSize * (bottom_up ? (H - 1 - y) : y);
        std::uint8_t* dst = &img.bgra[static_cast<std::size_t>(y) * W * 4];
        for (std::uint32_t x = 0; x < W; ++x) {
            const std::uint8_t* px = row + static_cast<std::size_t>(x) * (bpp / 8);
            dst[x * 4 + 0] = px[0]; dst[x * 4 + 1] = px[1]; dst[x * 4 + 2] = px[2];
            dst[x * 4 + 3] = (bpp == 32) ? px[3] : 255;
        }
    }
    return img;
}

std::optional<Image> decode_image(const std::uint8_t* data, std::size_t len) {
    if (auto d = decode_dds(data, len)) return d;
    if (auto b = decode_bmp(data, len)) return b;
    return std::nullopt;
}

} // namespace pk2
