// SkillEffect Studio - efpwrite/EfpTexture.cpp  (v6 M22)
#include "EfpTexture.h"

#include "../render/RImage.h"

#include <algorithm>
#include <cstring>

namespace se::efpw {

namespace {

void Put32(Bytes& b, std::size_t off, std::uint32_t v) { std::memcpy(b.data() + off, &v, 4); }

// 2x2 box filter (fallback when a source mip level cannot be decoded)
std::vector<std::uint8_t> Half(const std::vector<std::uint8_t>& src, int w, int h, int nw, int nh) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(nw) * nh * 4);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x)
            for (int c = 0; c < 4; ++c) {
                int sum = 0, cnt = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        int sx = std::min(w - 1, x * 2 + dx), sy = std::min(h - 1, y * 2 + dy);
                        sum += src[(static_cast<std::size_t>(sy) * w + sx) * 4 + c];
                        ++cnt;
                    }
                out[(static_cast<std::size_t>(y) * nw + x) * 4 + c] = static_cast<std::uint8_t>((sum + cnt / 2) / cnt);
            }
    return out;
}

}  // namespace

Bytes EncodeDdsArgb(const std::vector<std::vector<std::uint8_t>>& levels, int w, int h) {
    std::size_t total = 128;
    for (std::size_t i = 0; i < levels.size(); ++i) total += levels[i].size();
    Bytes b(total, 0);
    std::memcpy(b.data(), "DDS ", 4);
    std::uint32_t mips = static_cast<std::uint32_t>(levels.size());
    Put32(b, 4, 124);
    Put32(b, 8, 0x1u | 0x2u | 0x4u | 0x8u | 0x1000u | (mips > 1 ? 0x20000u : 0u));   // CAPS HEIGHT WIDTH PITCH PIXELFORMAT [MIPMAPCOUNT]
    Put32(b, 12, static_cast<std::uint32_t>(h));
    Put32(b, 16, static_cast<std::uint32_t>(w));
    Put32(b, 20, static_cast<std::uint32_t>(w) * 4);
    Put32(b, 28, mips);
    // DDPIXELFORMAT at 76
    Put32(b, 76, 32);
    Put32(b, 80, 0x40u | 0x1u);          // RGB | ALPHAPIXELS
    Put32(b, 88, 32);
    Put32(b, 92, 0x00FF0000u);
    Put32(b, 96, 0x0000FF00u);
    Put32(b, 100, 0x000000FFu);
    Put32(b, 104, 0xFF000000u);
    Put32(b, 108, 0x1000u | (mips > 1 ? 0x8u | 0x400000u : 0u));   // TEXTURE [COMPLEX MIPMAP]
    std::size_t off = 128;
    for (const auto& lv : levels) {
        for (std::size_t i = 0; i + 3 < lv.size(); i += 4) {   // RGBA -> BGRA (A8R8G8B8 little endian)
            b[off + i] = lv[i + 2];
            b[off + i + 1] = lv[i + 1];
            b[off + i + 2] = lv[i];
            b[off + i + 3] = lv[i + 3];
        }
        off += lv.size();
    }
    return b;
}

bool RecolorTexture(const Bytes& in, const ColorAdjust& adj, Bytes& out, std::string& err, std::size_t* changedPx) {
    if (changedPx) *changedPx = 0;
    bool ddj = in.size() >= 20 && std::memcmp(in.data(), "JMXVDDJ ", 8) == 0;
    const std::uint8_t* dds = ddj ? in.data() + 20 : in.data();
    std::size_t ddsSize = ddj ? in.size() - 20 : in.size();
    se::render::Image lv0;
    if (!se::render::DecodeDdj(in.data(), in.size(), lv0, err, 0)) return false;
    if (lv0.Empty()) { err = "empty texture"; return false; }
    int mips = std::max(1, se::render::DdsMipCount(dds, ddsSize));
    int maxMips = 1;
    for (int w = lv0.w, h = lv0.h; w > 1 || h > 1; w = std::max(1, w / 2), h = std::max(1, h / 2)) ++maxMips;
    mips = std::min(mips, maxMips);
    std::vector<std::vector<std::uint8_t>> levels;
    int w = lv0.w, h = lv0.h;
    for (int m = 0; m < mips; ++m) {
        se::render::Image im;
        std::string e;
        int ew = std::max(1, lv0.w >> m), eh = std::max(1, lv0.h >> m);
        if (m == 0) im = lv0;
        else if (!se::render::DecodeDdj(in.data(), in.size(), im, e, m) || im.w != ew || im.h != eh) {
            im.w = ew;
            im.h = eh;
            im.rgba = Half(levels.back(), std::max(1, lv0.w >> (m - 1)), std::max(1, lv0.h >> (m - 1)), ew, eh);
            levels.push_back(std::move(im.rgba));
            continue;   // already recoloured (derived from the recoloured level above)
        }
        std::vector<std::uint8_t> px = std::move(im.rgba);
        px.resize(static_cast<std::size_t>(ew) * eh * 4);
        for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
            std::uint8_t o[3] = {px[i], px[i + 1], px[i + 2]};
            AdjustRgba(&px[i], adj);
            if (m == 0 && changedPx && std::memcmp(o, &px[i], 3) != 0) ++*changedPx;
        }
        levels.push_back(std::move(px));
    }
    Bytes newDds = EncodeDdsArgb(levels, w, h);
    if (!ddj) {
        out = std::move(newDds);
        return true;
    }
    // JMXVDDJ header: "JMXVDDJ 1000" + u32 size field + u32; keep the source's relation of the size field
    out.assign(in.begin(), in.begin() + 20);
    std::uint32_t oldField, newField;
    std::memcpy(&oldField, in.data() + 12, 4);
    std::uint32_t oldDds = static_cast<std::uint32_t>(ddsSize), nd = static_cast<std::uint32_t>(newDds.size());
    if (oldField == oldDds + 8) newField = nd + 8;
    else if (oldField == oldDds) newField = nd;
    else if (oldField == oldDds + 20) newField = nd + 20;
    else newField = nd + 8;
    std::memcpy(out.data() + 12, &newField, 4);
    out.insert(out.end(), newDds.begin(), newDds.end());
    return true;
}

}  // namespace se::efpw
