// SkillEffect Studio v6 - ddj/DdjCodec.cpp  (M33) - .ddj / .dds writer + header reader. See DdjCodec.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "DdjCodec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <thread>

namespace se::ddj {
namespace {

void Why(std::wstring* w, const std::wstring& s) {
    if (w) *w = s;
}
inline std::uint32_t Le32(const std::uint8_t* p) {
    return p[0] | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline void Put32(Bytes& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
constexpr std::uint32_t Cc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24);
}

struct Desc {
    DdjFormat f;
    const wchar_t* name;
    const char* layout;
    std::uint32_t fourcc;
    int bits;               // uncompressed bit count
    std::uint32_t m[4];     // r, g, b, a masks
    int blockBytes;         // 0 = uncompressed
};

const Desc kDesc[] = {
    {DdjFormat::Dxt1, L"dxt1", "DXT1", Cc('D', 'X', 'T', '1'), 0, {0, 0, 0, 0}, 8},
    {DdjFormat::Dxt1a, L"dxt1a", "DXT1", Cc('D', 'X', 'T', '1'), 0, {0, 0, 0, 0}, 8},
    {DdjFormat::Dxt2, L"dxt2", "DXT2", Cc('D', 'X', 'T', '2'), 0, {0, 0, 0, 0}, 16},
    {DdjFormat::Dxt3, L"dxt3", "DXT3", Cc('D', 'X', 'T', '3'), 0, {0, 0, 0, 0}, 16},
    {DdjFormat::Dxt4, L"dxt4", "DXT4", Cc('D', 'X', 'T', '4'), 0, {0, 0, 0, 0}, 16},
    {DdjFormat::Dxt5, L"dxt5", "DXT5", Cc('D', 'X', 'T', '5'), 0, {0, 0, 0, 0}, 16},
    {DdjFormat::Argb8888, L"argb8888", "A8R8G8B8", 0, 32, {0xFF0000, 0xFF00, 0xFF, 0xFF000000u}, 0},
    {DdjFormat::Xrgb8888, L"xrgb8888", "X8R8G8B8", 0, 32, {0xFF0000, 0xFF00, 0xFF, 0}, 0},
    {DdjFormat::Argb1555, L"argb1555", "A1R5G5B5", 0, 16, {0x7C00, 0x3E0, 0x1F, 0x8000}, 0},
    {DdjFormat::Xrgb1555, L"xrgb1555", "X1R5G5B5", 0, 16, {0x7C00, 0x3E0, 0x1F, 0}, 0},
    {DdjFormat::Rgb565, L"rgb565", "R5G6B5", 0, 16, {0xF800, 0x7E0, 0x1F, 0}, 0},
    {DdjFormat::Argb4444, L"argb4444", "A4R4G4B4", 0, 16, {0xF00, 0xF0, 0xF, 0xF000}, 0},
};

const Desc* DescOf(DdjFormat f) {
    for (const auto& d : kDesc)
        if (d.f == f) return &d;
    return nullptr;
}

std::size_t LevelBytes(const Desc& d, int w, int h) {
    if (d.blockBytes) return static_cast<std::size_t>(std::max(1, (w + 3) / 4)) * std::max(1, (h + 3) / 4) * d.blockBytes;
    return static_cast<std::size_t>(w) * h * (d.bits / 8);
}

std::uint32_t ExpectedFlags(int mips) { return 0x1007u | (mips > 1 ? 0x20000u : 0u); }
std::uint32_t ExpectedCaps(const Desc& d, int mips) {
    return 0x1000u | ((!d.blockBytes && d.m[3]) ? 2u : 0u) | (mips > 1 ? 0x400008u : 0u);
}
std::uint32_t ExpectedPf(const Desc& d) { return d.blockBytes ? 4u : (0x40u | (d.m[3] ? 1u : 0u)); }

void MaskShift(std::uint32_t m, int& sh, int& nb) {
    sh = nb = 0;
    if (!m) return;
    while (!((m >> sh) & 1)) ++sh;
    std::uint32_t v = m >> sh;
    while (v & 1) { ++nb; v >>= 1; }
}

int Threads(int want, std::size_t work) {
    if (work < 64) return 1;
    int t = want > 0 ? want : static_cast<int>(std::thread::hardware_concurrency());
    return std::clamp(t, 1, 6);
}

template <class Fn>
void ParallelRows(int rows, int threads, Fn&& fn) {
    if (threads <= 1 || rows < 2) {
        for (int r = 0; r < rows; ++r) fn(r);
        return;
    }
    std::vector<std::thread> pool;
    std::atomic<int> next{0};
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (int r = next++; r < rows; r = next++) fn(r);
        });
    for (auto& th : pool) th.join();
}

void EncodeBlocks(const Image& img, const Desc& d, const DdjOptions& o, std::uint8_t* dst) {
    const int bw = std::max(1, (img.w + 3) / 4), bh = std::max(1, (img.h + 3) / 4);
    const bool premul = d.f == DdjFormat::Dxt2 || d.f == DdjFormat::Dxt4;
    ParallelRows(bh, Threads(o.threads, static_cast<std::size_t>(bw) * bh), [&](int by) {
        if (o.cancel && o.cancel->load()) return;
        for (int bx = 0; bx < bw; ++bx) {
            std::uint8_t px[64];
            for (int i = 0; i < 16; ++i) {
                const int x = std::min(img.w - 1, bx * 4 + (i & 3)), y = std::min(img.h - 1, by * 4 + (i >> 2));
                std::memcpy(px + i * 4, img.Px(x, y), 4);
                if (premul)
                    for (int k = 0; k < 3; ++k) px[i * 4 + k] = static_cast<std::uint8_t>((px[i * 4 + k] * px[i * 4 + 3] + 127) / 255);
            }
            std::uint8_t* out = dst + (static_cast<std::size_t>(by) * bw + bx) * d.blockBytes;
            switch (d.f) {
                case DdjFormat::Dxt1: EncodeBc1(px, out, o.dxt, false); break;
                case DdjFormat::Dxt1a: EncodeBc1(px, out, o.dxt, true); break;
                case DdjFormat::Dxt2:
                case DdjFormat::Dxt3: EncodeBc2(px, out, o.dxt); break;
                default: EncodeBc3(px, out, o.dxt); break;
            }
        }
    });
}

void EncodePlain(const Image& img, const Desc& d, const DdjOptions& o, std::uint8_t* dst) {
    int sh[4], nb[4];
    for (int k = 0; k < 4; ++k) MaskShift(d.m[k], sh[k], nb[k]);
    const int bytes = d.bits / 8;
    // X bits (no alpha mask): every bit of the pixel no colour mask covers
    const std::uint32_t used = d.m[0] | d.m[1] | d.m[2] | d.m[3];
    const std::uint32_t all = d.bits == 32 ? 0xFFFFFFFFu : ((1u << d.bits) - 1);
    const std::uint32_t xbits = (all & ~used);
    std::uint32_t xval = 0;
    if (xbits) {
        int xs, xn;
        MaskShift(xbits, xs, xn);
        const std::uint32_t xm = xn >= 32 ? 0xFFFFFFFFu : ((1u << xn) - 1);
        xval = ((static_cast<std::uint32_t>(o.xFill) * xm + 127) / 255) << xs;
    }
    const bool dith = o.dither16 && d.bits == 16;
    std::vector<float> err0, err1;
    if (dith) { err0.assign(static_cast<std::size_t>(img.w + 2) * 4, 0.0f); err1 = err0; }
    for (int y = 0; y < img.h; ++y) {
        if (dith) { std::swap(err0, err1); std::fill(err1.begin(), err1.end(), 0.0f); }
        for (int x = 0; x < img.w; ++x) {
            const std::uint8_t* s = img.Px(x, y);
            std::uint32_t v = xval;
            for (int k = 0; k < 4; ++k) {
                if (!d.m[k]) continue;
                const std::uint32_t mx = nb[k] >= 32 ? 0xFFFFFFFFu : ((1u << nb[k]) - 1);
                std::uint32_t q;
                if (dith) {
                    const float want = s[k] + err0[static_cast<std::size_t>(x + 1) * 4 + k];
                    const int qi = std::clamp(static_cast<int>(std::lround(want * mx / 255.0f)), 0, static_cast<int>(mx));
                    q = static_cast<std::uint32_t>(qi);
                    const float got = static_cast<float>((q * 255 + mx / 2) / mx);
                    const float e = want - got;
                    err0[static_cast<std::size_t>(x + 2) * 4 + k] += e * 7 / 16;
                    err1[static_cast<std::size_t>(x) * 4 + k] += e * 3 / 16;
                    err1[static_cast<std::size_t>(x + 1) * 4 + k] += e * 5 / 16;
                    err1[static_cast<std::size_t>(x + 2) * 4 + k] += e * 1 / 16;
                } else {
                    q = (static_cast<std::uint32_t>(s[k]) * mx + 127) / 255;
                }
                v |= (q << sh[k]) & d.m[k];
            }
            std::uint8_t* o8 = dst + (static_cast<std::size_t>(y) * img.w + x) * bytes;
            for (int b = 0; b < bytes; ++b) o8[b] = static_cast<std::uint8_t>(v >> (8 * b));
        }
    }
}

float Coverage(const Image& img, int ref, float scale) {
    std::size_t n = 0, c = 0;
    for (std::size_t i = 3; i < img.rgba.size(); i += 4, ++n)
        if (std::min(255.0f, img.rgba[i] * scale) >= ref) ++c;
    return n ? static_cast<float>(c) / n : 0.0f;
}

}  // namespace

const wchar_t* FormatName(DdjFormat f) {
    if (f == DdjFormat::Auto) return L"auto";
    if (const Desc* d = DescOf(f)) return d->name;
    return L"?";
}

std::wstring LayoutName(DdjFormat f) {
    if (f == DdjFormat::Auto) return L"AUTO";
    if (f == DdjFormat::Dxt1a) return L"DXT1 (1 bit alfa)";
    const Desc* d = DescOf(f);
    if (!d) return L"?";
    const std::string s = d->layout;
    return std::wstring(s.begin(), s.end());
}

bool ParseFormat(std::wstring_view s, DdjFormat& f) {
    std::wstring t(s);
    for (auto& c : t) c = static_cast<wchar_t>(std::towlower(c));
    struct A { const wchar_t* n; DdjFormat f; };
    static const A alias[] = {{L"auto", DdjFormat::Auto}, {L"bc1", DdjFormat::Dxt1}, {L"bc1a", DdjFormat::Dxt1a},
                              {L"bc2", DdjFormat::Dxt3}, {L"bc3", DdjFormat::Dxt5}, {L"a8r8g8b8", DdjFormat::Argb8888},
                              {L"rgb32", DdjFormat::Argb8888}, {L"argb", DdjFormat::Argb8888}, {L"x8r8g8b8", DdjFormat::Xrgb8888},
                              {L"a1r5g5b5", DdjFormat::Argb1555}, {L"rgb16", DdjFormat::Argb1555}, {L"x1r5g5b5", DdjFormat::Xrgb1555},
                              {L"r5g6b5", DdjFormat::Rgb565}, {L"a4r4g4b4", DdjFormat::Argb4444}};
    for (const auto& a : alias)
        if (t == a.n) { f = a.f; return true; }
    for (const auto& d : kDesc)
        if (t == d.name) { f = d.f; return true; }
    return false;
}

bool IsBlockFormat(DdjFormat f) {
    const Desc* d = DescOf(f);
    return d && d->blockBytes;
}

bool FormatHasAlpha(DdjFormat f) {
    if (f == DdjFormat::Dxt1a || f == DdjFormat::Dxt2 || f == DdjFormat::Dxt3 || f == DdjFormat::Dxt4 || f == DdjFormat::Dxt5) return true;
    const Desc* d = DescOf(f);
    return d && !d->blockBytes && d->m[3];
}

int FullMipCount(int w, int h) {
    int n = 1, m = std::max(w, h);
    while (m > 1) { m >>= 1; ++n; }
    return n;
}

DdjFormat ResolveFormat(DdjFormat f, const Image& img) {
    if (f != DdjFormat::Auto) return f;
    return HasAlpha(img) ? DdjFormat::Dxt5 : DdjFormat::Dxt1;
}

bool ReadDdjInfo(const std::uint8_t* p, std::size_t n, DdjInfo& o, std::wstring* why) {
    o = DdjInfo{};
    o.fileSize = n;
    const std::uint8_t* d = p;
    std::size_t dn = n;
    if (n >= 20 && std::memcmp(p, "JMXVDDJ ", 8) == 0) {
        o.jmxv = true;
        o.jmxvSize = Le32(p + 12);
        o.jmxvType = Le32(p + 16);
        d = p + 20;
        dn = n - 20;
    }
    if (dn < 128 || std::memcmp(d, "DDS ", 4) != 0) { Why(why, L"DDS başlığı yok ya da kısa"); return false; }
    o.headerBytes = (o.jmxv ? 20 : 0) + 128;
    const std::uint32_t hsize = Le32(d + 4);
    o.flags = Le32(d + 8);
    o.h = static_cast<int>(Le32(d + 12));
    o.w = static_cast<int>(Le32(d + 16));
    o.pitch = Le32(d + 20);
    const std::uint32_t depth = Le32(d + 24);
    o.mipField = Le32(d + 28);
    const std::uint32_t pfSize = Le32(d + 76);
    o.pfFlags = Le32(d + 80);
    o.fourcc = Le32(d + 84);
    o.bits = Le32(d + 88);
    for (int k = 0; k < 4; ++k) o.masks[k] = Le32(d + 92 + 4 * k);
    o.caps1 = Le32(d + 108);
    o.caps2 = Le32(d + 112);
    if (o.w <= 0 || o.h <= 0 || o.w > 16384 || o.h > 16384) { Why(why, L"DDS boyutu geçersiz"); return false; }
    o.mips = ((o.flags & 0x20000) || (o.caps1 & 0x400000)) && o.mipField > 0 ? static_cast<int>(std::min<std::uint32_t>(o.mipField, 16)) : 1;
    const Desc* desc = nullptr;
    if (o.pfFlags & 4) {
        char cc[5] = {static_cast<char>(o.fourcc & 0xFF), static_cast<char>((o.fourcc >> 8) & 0xFF), static_cast<char>((o.fourcc >> 16) & 0xFF), static_cast<char>(o.fourcc >> 24), 0};
        o.layout = cc;
        for (const auto& x : kDesc)
            if (x.fourcc && x.fourcc == o.fourcc && x.f != DdjFormat::Dxt1a) { desc = &x; break; }
        if (o.fourcc == Cc('D', 'X', '1', '0')) o.headerBytes += 20;
    } else if (o.pfFlags & 0x20) {
        o.layout = "P8";
    } else {
        const std::uint32_t am = (o.pfFlags & 1) ? o.masks[3] : 0;
        for (const auto& x : kDesc)
            if (!x.fourcc && static_cast<std::uint32_t>(x.bits) == o.bits && x.m[0] == o.masks[0] && x.m[1] == o.masks[1] && x.m[2] == o.masks[2] && x.m[3] == am) { desc = &x; break; }
        o.layout = desc ? desc->layout : ("MASK" + std::to_string(o.bits));
    }
    if (desc) {
        o.format = desc->f;
        std::size_t total = o.headerBytes;
        int w = o.w, h = o.h;
        for (int i = 0; i < o.mips; ++i) {
            total += LevelBytes(*desc, w, h);
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
        }
        o.expectedSize = total;
        o.sizeOk = total == n;
    }
    std::wstring note;
    auto add = [&](const wchar_t* s) { if (!note.empty()) note += L"; "; note += s; };
    if (!o.jmxv) add(L"JMXVDDJ başlığı yok (düz DDS)");
    else {
        if (o.jmxvSize != n - 12) add(L"JMXVDDJ boy alanı dosya boyu - 12 değil");
        if (o.jmxvType != 3) add(L"JMXVDDJ tür alanı 3 değil");
    }
    if (!desc) add(L"bu yazıcının üretmediği bir DDS düzeni");
    else {
        if (hsize != 124 || pfSize != 32) add(L"DDS başlık boyları standart değil");
        if (o.flags != ExpectedFlags(o.mips)) add(L"DDS bayrakları sevkiyattakinden farklı");
        if (o.pfFlags != ExpectedPf(*desc)) add(L"piksel biçimi bayrakları farklı");
        if (o.caps1 != ExpectedCaps(*desc, o.mips) || o.caps2 != 0) add(L"DDS yetenek (caps) alanı farklı");
        if (o.pitch != 0 || depth != 0) add(L"pitch / depth alanı 0 değil");
        if (o.mipField != static_cast<std::uint32_t>(o.mips > 1 ? o.mips : 0)) add(L"mip sayısı alanı farklı");
        bool res = true;
        for (int i = 32; i < 76; ++i) res = res && d[i] == 0;
        for (int i = 116; i < 128; ++i) res = res && d[i] == 0;
        if (!res) add(L"ayrılmış alanlar sıfır değil");
        if (!o.sizeOk) add(L"dosya boyu beklenenden farklı");
    }
    o.headerLikeShipped = note.empty();
    o.noteTr = note;
    return true;
}

DdjOptions OptionsLike(const DdjInfo& info, const std::uint8_t* original, std::size_t n) {
    DdjOptions o;
    o.format = info.format == DdjFormat::Unknown ? DdjFormat::Auto : info.format;
    o.mipLevels = info.mips;
    o.jmxv = info.jmxv;
    if (original && n >= info.headerBytes && info.headerBytes >= 128 && info.format != DdjFormat::Unknown) {
        o.headerLike.assign(original, original + (info.jmxv ? 148 : 128));
        o.headerLikeSize = n;
        // unused X bits: as the original's first texel had them
        if (info.format == DdjFormat::Xrgb8888 && n >= info.headerBytes + 4) o.xFill = original[info.headerBytes + 3];
        if (info.format == DdjFormat::Xrgb1555 && n >= info.headerBytes + 2) o.xFill = (original[info.headerBytes + 1] & 0x80) ? 0xFF : 0x00;
    }
    return o;
}

namespace {
// Keep the header style of the file being replaced (DdjOptions::headerLike). out already holds the canonical header.
void ApplyHeaderLike(Bytes& out, const DdjOptions& o, const Desc& d, int W, int H, int mips, std::size_t level0) {
    DdjInfo t;
    if (o.headerLike.empty() || !ReadDdjInfo(o.headerLike.data(), o.headerLike.size(), t) || t.jmxv != o.jmxv || t.format == DdjFormat::Unknown) return;
    const std::uint8_t* tp = o.headerLike.data();
    const std::size_t at = o.jmxv ? 20 : 0;
    if (o.jmxv) {
        std::memcpy(out.data(), tp, 12);
        std::memcpy(out.data() + 16, tp + 16, 4);
        if (o.headerLikeSize && t.jmxvSize != o.headerLikeSize - 12) std::memcpy(out.data() + 12, tp + 12, 4);   // keep a quirky size field
    }
    const std::uint8_t* td = tp + at;
    std::uint8_t* od = out.data() + at;
    const Bytes canon(od, od + 128);
    std::memcpy(od, td, 128);
    Put32(out, at + 12, static_cast<std::uint32_t>(H));
    Put32(out, at + 16, static_cast<std::uint32_t>(W));
    const DdjFormat tf = t.format == DdjFormat::Dxt1 && d.f == DdjFormat::Dxt1a ? DdjFormat::Dxt1a : t.format;
    if (tf != d.f) {   // another layout: canonical pixel format + caps alpha bit
        std::memcpy(od + 76, canon.data() + 76, 32);
        const std::uint32_t c = (Le32(td + 108) & ~2u) | (Le32(canon.data() + 108) & 2u);
        Put32(out, at + 108, c);
    }
    if (t.mips != mips) {
        const std::uint32_t fl = (Le32(td + 8) & ~0x20000u) | (mips > 1 ? 0x20000u : 0u);
        Put32(out, at + 8, fl);
        Put32(out, at + 28, mips > 1 ? static_cast<std::uint32_t>(mips) : (t.mipField == 1 ? 1u : 0u));
        const std::uint32_t c = (Le32(od + 108) & ~0x400008u) | (mips > 1 ? 0x400008u : 0u);
        Put32(out, at + 108, c);
    }
    // pitch / linear size: recompute when the template's value matched its own picture, else keep it as it was
    const std::uint32_t fl = Le32(od + 8);
    if (fl & 0x80008u) {
        const Desc* td2 = DescOf(t.format);
        std::uint32_t tplWant = 0, now = 0;
        if (td2) tplWant = static_cast<std::uint32_t>((fl & 0x80000u) ? LevelBytes(*td2, t.w, t.h) : static_cast<std::size_t>(t.w) * (td2->bits / 8));
        now = static_cast<std::uint32_t>((fl & 0x80000u) ? level0 : static_cast<std::size_t>(W) * (d.bits / 8));
        if (t.pitch == tplWant) Put32(out, at + 20, now);
    }
}
}  // namespace

std::vector<Image> BuildMips(const Image& base, int levels, MipFilter f, bool keepCoverage, int ref) {
    std::vector<Image> out;
    if (base.Empty()) return out;
    const int full = FullMipCount(base.w, base.h);
    const int n = levels <= 0 ? full : std::min(levels, full);
    out.push_back(base);
    const float cov0 = keepCoverage ? Coverage(base, ref, 1.0f) : 0.0f;
    for (int i = 1; i < n; ++i) {
        const int w = std::max(1, base.w >> i), h = std::max(1, base.h >> i);
        Image m = f == MipFilter::Box ? Resample(out.back(), w, h, Filter::Box) : Resample(base, w, h, Filter::Mitchell);
        if (keepCoverage && cov0 > 0) {
            float lo = 0.0f, hi = 4.0f;
            for (int it = 0; it < 14; ++it) {
                const float mid = (lo + hi) * 0.5f;
                if (Coverage(m, ref, mid) < cov0) lo = mid; else hi = mid;
            }
            // the closer of the two bracketing scales (coverage is a step function)
            const float s = std::fabs(Coverage(m, ref, lo) - cov0) < std::fabs(Coverage(m, ref, hi) - cov0) ? lo : hi;
            for (std::size_t k = 3; k < m.rgba.size(); k += 4) m.rgba[k] = static_cast<std::uint8_t>(std::min(255.0f, m.rgba[k] * s + 0.5f));
        }
        out.push_back(std::move(m));
    }
    return out;
}

bool EncodeDdjLevels(const std::vector<Image>& levels, const DdjOptions& o, Bytes& out, std::wstring* why) {
    out.clear();
    if (levels.empty() || levels[0].Empty()) { Why(why, L"resim boş"); return false; }
    const int W = levels[0].w, H = levels[0].h;
    if (W > 16384 || H > 16384) { Why(why, L"resim çok büyük (en çok 16384 x 16384)"); return false; }
    const DdjFormat fmt = ResolveFormat(o.format, levels[0]);
    const Desc* d = DescOf(fmt);
    if (!d) { Why(why, L"bilinmeyen doku biçimi"); return false; }
    const int mips = static_cast<int>(levels.size());
    if (mips > FullMipCount(W, H)) { Why(why, L"mip seviyesi sayısı resim boyutunu aşıyor"); return false; }
    std::size_t total = (o.jmxv ? 20 : 0) + 128;
    for (int i = 0; i < mips; ++i) {
        const int w = std::max(1, W >> i), h = std::max(1, H >> i);
        if (levels[i].w != w || levels[i].h != h) { Why(why, L"mip seviyesi boyutu yanlış (seviye " + std::to_wstring(i) + L")"); return false; }
        total += LevelBytes(*d, w, h);
    }
    out.assign(total, 0);
    std::size_t at = 0;
    if (o.jmxv) {
        std::memcpy(out.data(), "JMXVDDJ 1000", 12);
        Put32(out, 12, static_cast<std::uint32_t>(total - 12));
        Put32(out, 16, 3);
        at = 20;
    }
    std::memcpy(out.data() + at, "DDS ", 4);
    Put32(out, at + 4, 124);
    Put32(out, at + 8, ExpectedFlags(mips));
    Put32(out, at + 12, static_cast<std::uint32_t>(H));
    Put32(out, at + 16, static_cast<std::uint32_t>(W));
    Put32(out, at + 28, mips > 1 ? static_cast<std::uint32_t>(mips) : 0u);
    Put32(out, at + 76, 32);
    Put32(out, at + 80, ExpectedPf(*d));
    Put32(out, at + 84, d->fourcc);
    Put32(out, at + 88, d->blockBytes ? 0u : static_cast<std::uint32_t>(d->bits));
    for (int k = 0; k < 4; ++k) Put32(out, at + 92 + 4 * k, d->blockBytes ? 0u : d->m[k]);
    Put32(out, at + 108, ExpectedCaps(*d, mips));
    ApplyHeaderLike(out, o, *d, W, H, mips, LevelBytes(*d, W, H));
    at += 128;
    for (int i = 0; i < mips; ++i) {
        if (o.cancel && o.cancel->load()) { Why(why, L"iptal edildi"); out.clear(); return false; }
        if (d->blockBytes) EncodeBlocks(levels[i], *d, o, out.data() + at);
        else EncodePlain(levels[i], *d, o, out.data() + at);
        at += LevelBytes(*d, levels[i].w, levels[i].h);
    }
    if (o.cancel && o.cancel->load()) { Why(why, L"iptal edildi"); out.clear(); return false; }
    return true;
}

bool EncodeDdj(const Image& img, const DdjOptions& o, Bytes& out, std::wstring* why) {
    if (img.Empty()) { Why(why, L"resim boş"); return false; }
    DdjOptions oo = o;
    oo.format = ResolveFormat(o.format, img);
    const auto levels = BuildMips(img, o.mipLevels, o.mipFilter, o.mipKeepCoverage, o.coverageRef);
    return EncodeDdjLevels(levels, oo, out, why);
}

bool DecodeDdjLevels(const std::uint8_t* p, std::size_t n, std::vector<Image>& levels, std::wstring* why) {
    levels.clear();
    DdjInfo info;
    if (!ReadDdjInfo(p, n, info, why)) return false;
    for (int i = 0; i < info.mips; ++i) {
        Image im;
        std::string e;
        if (!se::render::DecodeDdj(p, n, im, e, i)) { Why(why, L"seviye " + std::to_wstring(i) + L" çözülemedi: " + std::wstring(e.begin(), e.end())); return false; }
        levels.push_back(std::move(im));
    }
    return true;
}

bool IsSafeOutputPath(const std::wstring& path, std::wstring* why) {
    wchar_t full[MAX_PATH * 4];
    const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
    if (n == 0 || n >= std::size(full)) { Why(why, L"yol çözülemedi: " + path); return false; }
    std::wstring dir(full);
    const auto s = dir.find_last_of(L"\\/");
    if (s == std::wstring::npos) return true;
    dir.resize(s);
    while (dir.size() > 2) {
        for (const wchar_t* pat : {L"\\*.pk2", L"\\sro_client.exe", L"\\silkroad.exe"}) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + pat).c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                FindClose(h);
                Why(why, L"Bu klasör bir oyun istemcisinin içinde (" + dir + L"); istemci klasörüne yazılmaz. Başka bir çıktı klasörü seçin.");
                return false;
            }
        }
        const auto t = dir.find_last_of(L"\\/");
        if (t == std::wstring::npos) break;
        dir.resize(t);
    }
    return true;
}

bool WriteDdjFile(const std::wstring& path, const Bytes& data, std::wstring* why, bool backup) {
    if (!IsSafeOutputPath(path, why)) return false;
    return WriteFileAtomic(path, data, why, backup);
}

}  // namespace se::ddj
