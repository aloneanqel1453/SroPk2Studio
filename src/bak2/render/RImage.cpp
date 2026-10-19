// SkillEffect Studio - render/RImage.cpp
#include "RImage.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace se::render {

namespace {

std::uint32_t RdU32(const std::uint8_t* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }

constexpr std::uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_ALPHA = 0x2, DDPF_FOURCC = 0x4, DDPF_RGB = 0x40,
                        DDPF_LUMINANCE = 0x20000;
constexpr std::uint32_t DDSD_MIPMAPCOUNT = 0x20000, DDSCAPS_MIPMAP = 0x400000;

struct MaskFmt { const char* name; std::uint32_t bits, r, g, b, a, pf; };
const MaskFmt kMask[] = {
    {"A8R8G8B8", 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000, DDPF_RGB | DDPF_ALPHAPIXELS},
    {"X8R8G8B8", 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0x00000000, DDPF_RGB},
    {"A8B8G8R8", 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000, DDPF_RGB | DDPF_ALPHAPIXELS},
    {"X8B8G8R8", 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0x00000000, DDPF_RGB},
    {"R8G8B8", 24, 0xFF0000, 0x00FF00, 0x0000FF, 0, DDPF_RGB},
    {"R5G6B5", 16, 0xF800, 0x07E0, 0x001F, 0x0000, DDPF_RGB},
    {"A1R5G5B5", 16, 0x7C00, 0x03E0, 0x001F, 0x8000, DDPF_RGB | DDPF_ALPHAPIXELS},
    {"X1R5G5B5", 16, 0x7C00, 0x03E0, 0x001F, 0x0000, DDPF_RGB},
    {"A4R4G4B4", 16, 0x0F00, 0x00F0, 0x000F, 0xF000, DDPF_RGB | DDPF_ALPHAPIXELS},
    {"X4R4G4B4", 16, 0x0F00, 0x00F0, 0x000F, 0x0000, DDPF_RGB},
    {"A8", 8, 0, 0, 0, 0xFF, DDPF_ALPHA},
    {"L8", 8, 0xFF, 0, 0, 0, DDPF_LUMINANCE},
    {"A8L8", 16, 0x00FF, 0, 0, 0xFF00, DDPF_LUMINANCE | DDPF_ALPHAPIXELS},
    {"A4L4", 8, 0x0F, 0, 0, 0xF0, DDPF_LUMINANCE | DDPF_ALPHAPIXELS},
};
const MaskFmt* MaskByName(const char* n) {
    for (const auto& m : kMask)
        if (!std::strcmp(m.name, n)) return &m;
    return nullptr;
}
const char* D3dFmtName(std::uint32_t v) {
    switch (v) {
        case 20: return "R8G8B8"; case 21: return "A8R8G8B8"; case 22: return "X8R8G8B8"; case 23: return "R5G6B5";
        case 24: return "X1R5G5B5"; case 25: return "A1R5G5B5"; case 26: return "A4R4G4B4"; case 28: return "A8";
        case 30: return "X4R4G4B4"; case 32: return "A8B8G8R8"; case 33: return "X8B8G8R8"; case 50: return "L8";
        case 51: return "A8L8"; case 52: return "A4L4"; default: return nullptr;
    }
}

struct DdsInfo {
    int w = 0, h = 0, mips = 1;
    std::string fmt;
    int blockBytes = 0, bpp = 0;
    std::uint32_t masks[4] = {}, pf = 0;
    std::size_t header = 128;
};

bool ParseDds(const std::uint8_t* d, std::size_t n, DdsInfo& info, std::string& err) {
    if (n < 128 || std::memcmp(d, "DDS ", 4) != 0) { err = "not DDS"; return false; }
    std::uint32_t flags = RdU32(d + 8), height = RdU32(d + 12), width = RdU32(d + 16), mips = RdU32(d + 28);
    std::uint32_t pfFlags = RdU32(d + 80), four = RdU32(d + 84), bits = RdU32(d + 88);
    std::uint32_t rm = RdU32(d + 92), gm = RdU32(d + 96), bm = RdU32(d + 100), am = RdU32(d + 104);
    std::uint32_t caps = RdU32(d + 108);
    info = DdsInfo();
    info.w = static_cast<int>(std::max<std::uint32_t>(1, width));
    info.h = static_cast<int>(std::max<std::uint32_t>(1, height));
    if (width > 16384 || height > 16384) { err = "bad DDS size"; return false; }
    info.pf = pfFlags;
    info.masks[0] = rm; info.masks[1] = gm; info.masks[2] = bm; info.masks[3] = am;
    if (pfFlags & DDPF_FOURCC) {
        char cc[5] = {};
        std::memcpy(cc, d + 84, 4);
        std::string s(cc);
        if (s == "DX10") {
            if (n < 148) { err = "short DX10"; return false; }
            std::uint32_t dx = RdU32(d + 128);
            info.header = 148;
            switch (dx) {
                case 71: case 72: info.fmt = "DXT1"; break;
                case 74: case 75: info.fmt = "DXT3"; break;
                case 77: case 78: info.fmt = "DXT5"; break;
                case 80: info.fmt = "BC4U"; break;
                case 83: info.fmt = "BC5U"; break;
                case 87: case 91: info.fmt = "A8R8G8B8"; break;
                case 88: info.fmt = "X8R8G8B8"; break;
                case 28: case 29: info.fmt = "A8B8G8R8"; break;
                case 65: info.fmt = "A8"; break;
                default: err = "unsupported DXGI " + std::to_string(dx); return false;
            }
        } else if (s == "DXT1" || s == "DXT2" || s == "DXT3" || s == "DXT4" || s == "DXT5" || s == "ATI1" ||
                   s == "ATI2" || s == "BC4U" || s == "BC5U") {
            info.fmt = s;
        } else if (const char* nm = D3dFmtName(four)) {
            info.fmt = nm;
        } else {
            err = "unsupported FourCC " + s;
            return false;
        }
    } else if ((pfFlags & 0x20) && bits == 8) {   // DDPF_PALETTEINDEXED8: 256 x PALETTEENTRY(r,g,b,flags) after the header
        if (n < 128 + 1024) { err = "short P8"; return false; }
        info.fmt = "P8";
        info.header = 128 + 1024;
    } else {
        for (const auto& m : kMask)
            if (m.bits == bits && m.r == rm && m.g == gm && m.b == bm &&
                m.a == ((pfFlags & (DDPF_ALPHAPIXELS | DDPF_ALPHA)) ? am : 0u)) {
                info.fmt = m.name;
                break;
            }
        if (info.fmt.empty()) {
            if (bits != 8 && bits != 16 && bits != 24 && bits != 32) { err = "unsupported bit count"; return false; }
            info.fmt = "MASK" + std::to_string(bits);
        }
    }
    const std::string& f = info.fmt;
    if (f == "DXT1" || f == "ATI1" || f == "BC4U") info.blockBytes = 8;
    else if (f == "DXT2" || f == "DXT3" || f == "DXT4" || f == "DXT5" || f == "ATI2" || f == "BC5U") info.blockBytes = 16;
    else if (f == "P8") info.bpp = 8;
    else if (const MaskFmt* m = MaskByName(f.c_str())) {
        info.bpp = static_cast<int>(m->bits);
        info.masks[0] = m->r; info.masks[1] = m->g; info.masks[2] = m->b; info.masks[3] = m->a;
    } else info.bpp = static_cast<int>(bits);
    int nm = ((flags & DDSD_MIPMAPCOUNT) || (caps & DDSCAPS_MIPMAP)) && mips > 0 ? static_cast<int>(mips) : 1;
    info.mips = std::max(1, std::min(nm, 16));
    return true;
}

std::size_t LevelSize(const DdsInfo& i, int w, int h) {
    if (i.blockBytes) return static_cast<std::size_t>(std::max(1, (w + 3) / 4)) * std::max(1, (h + 3) / 4) * i.blockBytes;
    return static_cast<std::size_t>((w * i.bpp + 7) / 8) * h;
}

void Expand565(std::uint32_t c, int out[3]) {
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = (r << 3) | (r >> 2); out[1] = (g << 2) | (g >> 4); out[2] = (b << 3) | (b >> 2);
}

void ColorBlock(const std::uint8_t* cb, bool fourAlways, std::uint8_t px[16][4]) {
    std::uint32_t c0 = cb[0] | (cb[1] << 8), c1 = cb[2] | (cb[3] << 8);
    std::uint32_t idx = RdU32(cb + 4);
    int p[4][4] = {};
    Expand565(c0, p[0]); Expand565(c1, p[1]);
    bool four = (c0 > c1) || fourAlways;
    for (int k = 0; k < 3; ++k) {
        p[2][k] = four ? (2 * p[0][k] + p[1][k] + 1) / 3 : (p[0][k] + p[1][k]) / 2;
        p[3][k] = four ? (p[0][k] + 2 * p[1][k] + 1) / 3 : 0;
    }
    p[0][3] = p[1][3] = p[2][3] = 255;
    p[3][3] = four ? 255 : 0;
    for (int i = 0; i < 16; ++i) {
        int s = (idx >> (2 * i)) & 3;
        for (int k = 0; k < 4; ++k) px[i][k] = static_cast<std::uint8_t>(p[s][k]);
    }
}

void AlphaBc3(const std::uint8_t* ab, std::uint8_t out[16]) {
    int a0 = ab[0], a1 = ab[1];
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(ab[2 + i]) << (8 * i);
    int pal[8];
    pal[0] = a0; pal[1] = a1;
    if (a0 > a1) {
        for (int k = 1; k <= 6; ++k) pal[1 + k] = ((7 - k) * a0 + k * a1 + 3) / 7;
    } else {
        for (int k = 1; k <= 4; ++k) pal[1 + k] = ((5 - k) * a0 + k * a1 + 2) / 5;
        pal[6] = 0; pal[7] = 255;
    }
    for (int i = 0; i < 16; ++i) out[i] = static_cast<std::uint8_t>(pal[(bits >> (3 * i)) & 7]);
}

void MaskShiftBits(std::uint32_t mask, int& shift, int& bits) {
    shift = bits = 0;
    if (!mask) return;
    while (!((mask >> shift) & 1)) ++shift;
    std::uint32_t m = mask >> shift;
    while (m) { bits += m & 1; m >>= 1; }
}

}  // namespace

int DdsMipCount(const std::uint8_t* data, std::size_t size) {
    if (size >= 20 && std::memcmp(data, "JMXVDDJ ", 8) == 0) { data += 20; size -= 20; }
    DdsInfo i;
    std::string e;
    return ParseDds(data, size, i, e) ? i.mips : 1;
}

bool DecodeDds(const std::uint8_t* d, std::size_t n, Image& out, std::string& err, int mip) {
    DdsInfo info;
    if (!ParseDds(d, n, info, err)) return false;
    std::size_t off = info.header;
    int w = info.w, h = info.h;
    mip = std::max(0, std::min(mip, info.mips - 1));
    for (int l = 0; l < mip; ++l) {
        off += LevelSize(info, w, h);
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }
    std::size_t sz = LevelSize(info, w, h);
    if (off + sz > n) { err = "DDS data short"; return false; }
    const std::uint8_t* raw = d + off;
    out.w = w; out.h = h;
    out.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
    const std::string& f = info.fmt;
    if (info.blockBytes) {
        int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                const std::uint8_t* b = raw + (static_cast<std::size_t>(by) * bw + bx) * info.blockBytes;
                std::uint8_t px[16][4];
                if (f == "DXT1") ColorBlock(b, false, px);
                else if (f == "DXT2" || f == "DXT3") {
                    ColorBlock(b + 8, true, px);
                    for (int i = 0; i < 16; ++i) {
                        int nib = (b[i / 2] >> ((i & 1) * 4)) & 0xF;
                        px[i][3] = static_cast<std::uint8_t>(nib * 17);
                    }
                } else if (f == "DXT4" || f == "DXT5") {
                    ColorBlock(b + 8, true, px);
                    std::uint8_t a[16];
                    AlphaBc3(b, a);
                    for (int i = 0; i < 16; ++i) px[i][3] = a[i];
                } else if (f == "ATI1" || f == "BC4U") {
                    std::uint8_t a[16];
                    AlphaBc3(b, a);
                    for (int i = 0; i < 16; ++i) { px[i][0] = px[i][1] = px[i][2] = a[i]; px[i][3] = 255; }
                } else {  // ATI2 / BC5U
                    std::uint8_t r[16], g[16];
                    AlphaBc3(b, r);
                    AlphaBc3(b + 8, g);
                    for (int i = 0; i < 16; ++i) {
                        float x = r[i] / 127.5f - 1.0f, y = g[i] / 127.5f - 1.0f;
                        float z = std::sqrt(std::clamp(1.0f - x * x - y * y, 0.0f, 1.0f));
                        px[i][0] = r[i]; px[i][1] = g[i];
                        px[i][2] = static_cast<std::uint8_t>(std::clamp((z + 1.0f) * 127.5f, 0.0f, 255.0f));
                        px[i][3] = 255;
                    }
                }
                for (int i = 0; i < 16; ++i) {
                    int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x < w && y < h) std::memcpy(out.Px(x, y), px[i], 4);
                }
            }
        if (f == "DXT2" || f == "DXT4") {  // premultiplied -> straight
            for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
                float a = out.rgba[i + 3];
                if (a > 0)
                    for (int k = 0; k < 3; ++k)
                        out.rgba[i + k] = static_cast<std::uint8_t>(std::clamp(out.rgba[i + k] * 255.0f / std::max(a, 1.0f), 0.0f, 255.0f));
            }
        }
        return true;
    }
    if (f == "P8") {   // palettised (D3DX converts these to A8R8G8B8 on load)
        const std::uint8_t* pal = d + 128;
        for (int i = 0; i < w * h; ++i) {
            const std::uint8_t* e = pal + static_cast<std::size_t>(raw[i]) * 4;
            std::uint8_t* o = &out.rgba[static_cast<std::size_t>(i) * 4];
            o[0] = e[0]; o[1] = e[1]; o[2] = e[2]; o[3] = e[3];
        }
        return true;
    }
    int bytes = info.bpp / 8;
    if (bytes < 1 || bytes > 4) { err = "unsupported bpp"; return false; }
    std::uint32_t rm = info.masks[0], gm = info.masks[1], bm = info.masks[2], am = info.masks[3], pff = info.pf;
    bool isMask = MaskByName(f.c_str()) != nullptr;
    if (const MaskFmt* m = MaskByName(f.c_str())) pff = m->pf;
    auto chan = [](std::uint32_t pix, std::uint32_t mask, int def) -> std::uint8_t {
        if (!mask) return static_cast<std::uint8_t>(def);
        int s, b;
        MaskShiftBits(mask, s, b);
        std::uint32_t v = (pix & mask) >> s;
        std::uint32_t mx = (b >= 32) ? 0xFFFFFFFFu : ((1u << b) - 1);
        return static_cast<std::uint8_t>((static_cast<std::uint64_t>(v) * 255 + mx / 2) / mx);
    };
    for (int i = 0; i < w * h; ++i) {
        std::uint32_t pix = 0;
        for (int k = 0; k < bytes; ++k) pix |= static_cast<std::uint32_t>(raw[static_cast<std::size_t>(i) * bytes + k]) << (8 * k);
        std::uint8_t* o = &out.rgba[static_cast<std::size_t>(i) * 4];
        if (pff & DDPF_LUMINANCE) {
            o[0] = o[1] = o[2] = chan(pix, rm, 0);
            o[3] = (pff & DDPF_ALPHAPIXELS) ? chan(pix, am, 255) : 255;
        } else if ((pff & DDPF_ALPHA) && !(pff & DDPF_RGB)) {
            o[0] = o[1] = o[2] = 255;
            o[3] = chan(pix, am, 255);
        } else {
            o[0] = chan(pix, rm, 0); o[1] = chan(pix, gm, 0); o[2] = chan(pix, bm, 0);
            o[3] = (am && ((pff & (DDPF_ALPHAPIXELS | DDPF_ALPHA)) || isMask)) ? chan(pix, am, 255) : 255;
        }
    }
    return true;
}

bool DecodeDdj(const std::uint8_t* data, std::size_t size, Image& out, std::string& err, int mip) {
    if (size >= 20 && std::memcmp(data, "JMXVDDJ ", 8) == 0) return DecodeDds(data + 20, size - 20, out, err, mip);
    return DecodeDds(data, size, out, err, mip);
}

// ------------------------------------------------------------------------------------------- PNG write
namespace {

std::uint32_t Crc32(const std::uint8_t* p, std::size_t n, std::uint32_t c = 0) {
    static std::uint32_t tab[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            tab[i] = v;
        }
        init = true;
    }
    c = ~c;
    for (std::size_t i = 0; i < n; ++i) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

struct BitOut {
    std::vector<std::uint8_t>& o;
    std::uint32_t acc = 0;
    int n = 0;
    explicit BitOut(std::vector<std::uint8_t>& v) : o(v) {}
    void Put(std::uint32_t bits, int cnt) {  // LSB-first
        acc |= bits << n;
        n += cnt;
        while (n >= 8) { o.push_back(static_cast<std::uint8_t>(acc)); acc >>= 8; n -= 8; }
    }
    void PutRev(std::uint32_t code, int len) {  // Huffman code MSB-first
        std::uint32_t r = 0;
        for (int i = 0; i < len; ++i) r |= ((code >> i) & 1) << (len - 1 - i);
        Put(r, len);
    }
    void Flush() { if (n > 0) { o.push_back(static_cast<std::uint8_t>(acc)); acc = 0; n = 0; } }
};

const int kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void PutLit(BitOut& b, int lit) {
    if (lit < 144) b.PutRev(0x30 + lit, 8);
    else if (lit < 256) b.PutRev(0x190 + lit - 144, 9);
    else if (lit < 280) b.PutRev(lit - 256, 7);
    else b.PutRev(0xC0 + lit - 280, 8);
}

void Deflate(const std::uint8_t* d, std::size_t n, std::vector<std::uint8_t>& out) {
    out.push_back(0x78); out.push_back(0x01);
    BitOut b(out);
    b.Put(1, 1); b.Put(1, 2);  // BFINAL, fixed Huffman
    const int HB = 15, HS = 1 << HB, WIN = 32768, CHAIN = 24;
    std::vector<int> head(HS, -1), prev(n ? n : 1, -1);
    auto hash = [&](std::size_t i) { return ((d[i] << 10) ^ (d[i + 1] << 5) ^ d[i + 2]) & (HS - 1); };
    std::size_t i = 0;
    while (i < n) {
        int bestLen = 0, bestDist = 0;
        if (i + 2 < n) {
            int h = hash(i);
            int c = head[h], chain = 0;
            while (c >= 0 && static_cast<int>(i) - c <= WIN && chain++ < CHAIN) {
                int maxl = static_cast<int>(std::min<std::size_t>(258, n - i));
                int l = 0;
                while (l < maxl && d[c + l] == d[i + l]) ++l;
                if (l > bestLen) { bestLen = l; bestDist = static_cast<int>(i) - c; if (l == maxl) break; }
                c = prev[c];
            }
        }
        if (bestLen >= 3) {
            int li = 0;
            while (li < 28 && kLenBase[li + 1] <= bestLen) ++li;
            PutLit(b, 257 + li);
            if (kLenExtra[li]) b.Put(static_cast<std::uint32_t>(bestLen - kLenBase[li]), kLenExtra[li]);
            int di = 0;
            while (di < 29 && kDistBase[di + 1] <= bestDist) ++di;
            b.PutRev(static_cast<std::uint32_t>(di), 5);
            if (kDistExtra[di]) b.Put(static_cast<std::uint32_t>(bestDist - kDistBase[di]), kDistExtra[di]);
            for (int k = 0; k < bestLen; ++k, ++i)
                if (i + 2 < n) { int h = hash(i); prev[i] = head[h]; head[h] = static_cast<int>(i); }
        } else {
            PutLit(b, d[i]);
            if (i + 2 < n) { int h = hash(i); prev[i] = head[h]; head[h] = static_cast<int>(i); }
            ++i;
        }
    }
    PutLit(b, 256);
    b.Flush();
    std::uint32_t a = 1, s2 = 0;
    for (std::size_t k = 0; k < n; ++k) { a = (a + d[k]) % 65521; s2 = (s2 + a) % 65521; }
    std::uint32_t ad = (s2 << 16) | a;
    for (int k = 3; k >= 0; --k) out.push_back(static_cast<std::uint8_t>(ad >> (8 * k)));
}

void Chunk(std::vector<std::uint8_t>& o, const char* tag, const std::vector<std::uint8_t>& data) {
    std::uint32_t n = static_cast<std::uint32_t>(data.size());
    for (int k = 3; k >= 0; --k) o.push_back(static_cast<std::uint8_t>(n >> (8 * k)));
    std::size_t st = o.size();
    o.insert(o.end(), tag, tag + 4);
    o.insert(o.end(), data.begin(), data.end());
    std::uint32_t c = Crc32(o.data() + st, o.size() - st);
    for (int k = 3; k >= 0; --k) o.push_back(static_cast<std::uint8_t>(c >> (8 * k)));
}

}  // namespace

bool EncodePng(const Image& img, std::vector<std::uint8_t>& out, bool forceRgb) {
    if (img.Empty()) return false;
    bool rgba = !forceRgb;
    if (rgba) {
        rgba = false;
        for (std::size_t i = 3; i < img.rgba.size(); i += 4)
            if (img.rgba[i] != 255) { rgba = true; break; }
    }
    int ch = rgba ? 4 : 3;
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(img.h) * (img.w * ch + 1));
    for (int y = 0; y < img.h; ++y) {
        raw.push_back(1);  // Sub filter
        for (int x = 0; x < img.w; ++x)
            for (int c = 0; c < ch; ++c) {
                int v = img.Px(x, y)[c], l = x > 0 ? img.Px(x - 1, y)[c] : 0;
                raw.push_back(static_cast<std::uint8_t>(v - l));
            }
    }
    out.clear();
    const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out.insert(out.end(), sig, sig + 8);
    std::vector<std::uint8_t> ihdr(13, 0);
    for (int k = 0; k < 4; ++k) { ihdr[3 - k] = static_cast<std::uint8_t>(img.w >> (8 * k)); ihdr[7 - k] = static_cast<std::uint8_t>(img.h >> (8 * k)); }
    ihdr[8] = 8; ihdr[9] = static_cast<std::uint8_t>(rgba ? 6 : 2);
    Chunk(out, "IHDR", ihdr);
    std::vector<std::uint8_t> z;
    Deflate(raw.data(), raw.size(), z);
    Chunk(out, "IDAT", z);
    Chunk(out, "IEND", {});
    return true;
}

bool WritePng(const std::wstring& path, const Image& img, std::string& err, bool forceRgb) {
    std::vector<std::uint8_t> data;
    if (!EncodePng(img, data, forceRgb)) { err = "empty image"; return false; }
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot create file"; return false; }
    DWORD wr = 0;
    BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wr, nullptr) && wr == data.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        err = "write failed";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------- PNG read
namespace {

struct BitIn {
    const std::uint8_t* d; std::size_t n, pos = 0; std::uint32_t acc = 0; int cnt = 0; bool bad = false;
    BitIn(const std::uint8_t* p, std::size_t k) : d(p), n(k) {}
    std::uint32_t Get(int k) {
        while (cnt < k) { if (pos >= n) { bad = true; return 0; } acc |= static_cast<std::uint32_t>(d[pos++]) << cnt; cnt += 8; }
        std::uint32_t v = acc & ((k == 32) ? 0xFFFFFFFFu : ((1u << k) - 1));
        acc >>= k; cnt -= k;
        return v;
    }
};

struct Huff {
    std::uint16_t counts[16] = {}, syms[320] = {};
    void Build(const std::uint8_t* lens, int n) {
        std::memset(counts, 0, sizeof(counts));
        for (int i = 0; i < n; ++i) counts[lens[i]]++;
        counts[0] = 0;
        std::uint16_t offs[16] = {};
        for (int i = 1; i < 16; ++i) offs[i] = static_cast<std::uint16_t>(offs[i - 1] + counts[i - 1]);
        for (int i = 0; i < n; ++i)
            if (lens[i]) syms[offs[lens[i]]++] = static_cast<std::uint16_t>(i);
    }
    int Decode(BitIn& b) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>(b.Get(1));
            int c = counts[len];
            if (code - c < first) return syms[index + (code - first)];
            index += c; first += c; first <<= 1; code <<= 1;
            if (b.bad) return -1;
        }
        return -1;
    }
};

bool Inflate(const std::uint8_t* d, std::size_t n, std::vector<std::uint8_t>& out) {
    if (n < 2) return false;
    BitIn b(d + 2, n - 2);
    bool last = false;
    while (!last) {
        last = b.Get(1) != 0;
        int type = static_cast<int>(b.Get(2));
        if (b.bad) return false;
        if (type == 0) {
            b.acc = 0; b.cnt = 0;
            if (b.pos + 4 > b.n) return false;
            std::uint16_t len = static_cast<std::uint16_t>(b.d[b.pos] | (b.d[b.pos + 1] << 8));
            b.pos += 4;
            if (b.pos + len > b.n) return false;
            out.insert(out.end(), b.d + b.pos, b.d + b.pos + len);
            b.pos += len;
            continue;
        }
        Huff lit, dist;
        if (type == 1) {
            std::uint8_t l[288];
            for (int i = 0; i < 288; ++i) l[i] = static_cast<std::uint8_t>(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
            lit.Build(l, 288);
            std::uint8_t dl[30];
            std::memset(dl, 5, 30);
            dist.Build(dl, 30);
        } else if (type == 2) {
            int hlit = static_cast<int>(b.Get(5)) + 257, hdist = static_cast<int>(b.Get(5)) + 1, hclen = static_cast<int>(b.Get(4)) + 4;
            static const int ord[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::uint8_t cl[19] = {};
            for (int i = 0; i < hclen; ++i) cl[ord[i]] = static_cast<std::uint8_t>(b.Get(3));
            Huff ch;
            ch.Build(cl, 19);
            std::uint8_t lens[320] = {};
            int k = 0;
            while (k < hlit + hdist) {
                int s = ch.Decode(b);
                if (s < 0) return false;
                if (s < 16) lens[k++] = static_cast<std::uint8_t>(s);
                else {
                    int rep = 0; std::uint8_t v = 0;
                    if (s == 16) { if (!k) return false; v = lens[k - 1]; rep = 3 + static_cast<int>(b.Get(2)); }
                    else if (s == 17) rep = 3 + static_cast<int>(b.Get(3));
                    else rep = 11 + static_cast<int>(b.Get(7));
                    while (rep-- && k < 320) lens[k++] = v;
                }
            }
            lit.Build(lens, hlit);
            dist.Build(lens + hlit, hdist);
        } else return false;
        for (;;) {
            int s = lit.Decode(b);
            if (s < 0 || b.bad) return false;
            if (s < 256) out.push_back(static_cast<std::uint8_t>(s));
            else if (s == 256) break;
            else {
                s -= 257;
                if (s >= 29) return false;
                int len = kLenBase[s] + static_cast<int>(b.Get(kLenExtra[s]));
                int ds = dist.Decode(b);
                if (ds < 0 || ds >= 30) return false;
                std::size_t dd = static_cast<std::size_t>(kDistBase[ds]) + b.Get(kDistExtra[ds]);
                if (dd > out.size()) return false;
                std::size_t st = out.size() - dd;
                for (int i = 0; i < len; ++i) out.push_back(out[st + i]);
            }
        }
    }
    return true;
}

}  // namespace

bool ReadPng(const std::wstring& path, Image& out, std::string& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot open"; return false; }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::vector<std::uint8_t> f(static_cast<std::size_t>(sz.QuadPart));
    DWORD rd = 0;
    BOOL ok = f.empty() ? FALSE : ReadFile(h, f.data(), static_cast<DWORD>(f.size()), &rd, nullptr);
    CloseHandle(h);
    if (!ok || rd != f.size() || f.size() < 33) { err = "read failed"; return false; }
    auto be = [&](std::size_t p) { return (static_cast<std::uint32_t>(f[p]) << 24) | (f[p + 1] << 16) | (f[p + 2] << 8) | f[p + 3]; };
    int w = static_cast<int>(be(16)), hh = static_cast<int>(be(20)), depth = f[24], ct = f[25], il = f[28];
    if (depth != 8 || (ct != 2 && ct != 6) || il != 0) { err = "unsupported PNG"; return false; }
    std::vector<std::uint8_t> z;
    for (std::size_t p = 8; p + 8 <= f.size();) {
        std::uint32_t n = be(p);
        if (p + 12 + n > f.size()) break;
        if (!std::memcmp(&f[p + 4], "IDAT", 4)) z.insert(z.end(), f.begin() + p + 8, f.begin() + p + 8 + n);
        p += 12 + n;
    }
    std::vector<std::uint8_t> raw;
    if (!Inflate(z.data(), z.size(), raw)) { err = "inflate failed"; return false; }
    int ch = ct == 6 ? 4 : 3;
    std::size_t stride = static_cast<std::size_t>(w) * ch;
    if (raw.size() < (stride + 1) * hh) { err = "short data"; return false; }
    std::vector<std::uint8_t> cur(stride), prv(stride, 0);
    out.w = w; out.h = hh;
    out.rgba.assign(static_cast<std::size_t>(w) * hh * 4, 255);
    for (int y = 0; y < hh; ++y) {
        const std::uint8_t* r = &raw[y * (stride + 1)];
        int ft = r[0];
        for (std::size_t i = 0; i < stride; ++i) {
            int a = i >= static_cast<std::size_t>(ch) ? cur[i - ch] : 0, bb = prv[i], c = i >= static_cast<std::size_t>(ch) ? prv[i - ch] : 0;
            int x = r[1 + i], v = x;
            switch (ft) {
                case 1: v = x + a; break;
                case 2: v = x + bb; break;
                case 3: v = x + (a + bb) / 2; break;
                case 4: { int p = a + bb - c, pa = std::abs(p - a), pb = std::abs(p - bb), pc = std::abs(p - c);
                          v = x + ((pa <= pb && pa <= pc) ? a : (pb <= pc ? bb : c)); break; }
                default: break;
            }
            cur[i] = static_cast<std::uint8_t>(v);
        }
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c) out.Px(x, y)[c] = cur[static_cast<std::size_t>(x) * ch + c];
        prv.swap(cur);
    }
    return true;
}

}  // namespace se::render
