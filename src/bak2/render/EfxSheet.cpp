// SkillEffect Studio - render/EfxSheet.cpp
#include "EfxSheet.h"

#include "SoftRaster.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace se::render {

void AddHumanLines(RFrame& f, const V3& base) {
    const double x = base.x, y = base.y, z = base.z;
    auto add = [&](V3 a, V3 b, double g) {
        RLine l;
        l.a = a; l.b = b;
        std::uint8_t c = static_cast<std::uint8_t>(static_cast<int>(255 * g));
        l.rgb[0] = l.rgb[1] = l.rgb[2] = c;
        f.lines.push_back(l);
    };
    for (int i = -4; i <= 4; ++i) {
        add({x - 20, 0, z + i * 5}, {x + 20, 0, z + i * 5}, 0.10);
        add({x + i * 5, 0, z - 20}, {x + i * 5, 0, z + 20}, 0.10);
    }
    struct J { const char* n; double a, b; };
    static const J js[] = {{"head", 0, 16.6}, {"neck", 0, 15.2}, {"hip", 0, 9.4}, {"lk", -1.2, 4.8}, {"rk", 1.2, 4.8},
                           {"lf", -1.6, 0}, {"rf", 1.6, 0}, {"ls", -2.4, 14.6}, {"rs", 2.4, 14.6}, {"le", -3.4, 11.6},
                           {"re", 3.4, 11.6}, {"lh", -3.8, 8.8}, {"rh", 3.8, 8.8}};
    auto jp = [&](const char* n) {
        for (const auto& j : js)
            if (!std::strcmp(j.n, n)) return V3{x + j.a, y + j.b, z};
        return V3{x, y, z};
    };
    static const char* bones[][2] = {{"neck", "hip"}, {"hip", "lk"}, {"lk", "lf"}, {"hip", "rk"}, {"rk", "rf"},
                                     {"ls", "rs"},    {"ls", "le"},  {"le", "lh"}, {"rs", "re"},  {"re", "rh"}};
    for (const auto& b : bones) add(jp(b[0]), jp(b[1]), 0.35);
    for (int i = 0; i < 12; ++i) {
        double a0 = i * kPi / 6, a1 = (i + 1) * kPi / 6;
        add({x + 1.3 * std::cos(a0), y + 16.6 + 1.3 * std::sin(a0), z}, {x + 1.3 * std::cos(a1), y + 16.6 + 1.3 * std::sin(a1), z}, 0.35);
    }
}

bool RunEffectSheet(AssetCache& cache, const EfxRunSpec& sp, Image& sheet, std::vector<EfxRunFrame>* frames,
                    std::string& err, int threads) {
    auto file = cache.Efp(sp.path, &err);
    if (!file) return false;
    EfpEffect fx(file, sp.scale, 255, sp.seed);
    fx.SetHold(sp.hold);
    M4 rot;
    bool hasRot = false;
    if (sp.move) {   // missiles are authored along local -Z
        V3 d = Norm(sp.moveDir);
        V3 zf = -d;
        V3 xr = Norm(Cross(V3{0, 1, 0}, zf));
        rot.SetRow(0, xr);
        rot.SetRow(1, Cross(zf, xr));
        rot.SetRow(2, zf);
        hasRot = true;
    }
    const int W = sp.W, H = sp.H, nf = static_cast<int>(sp.times.size());
    sheet.w = W * nf;
    sheet.h = H + 16;
    sheet.rgba.assign(static_cast<std::size_t>(sheet.w) * sheet.h * 4, 255);
    for (std::size_t i = 0; i < sheet.rgba.size(); i += 4) { sheet.rgba[i] = 20; sheet.rgba[i + 1] = 20; sheet.rgba[i + 2] = 24; }
    int step = 0;
    SoftRaster sr;
    for (int fi = 0; fi < nf; ++fi) {
        double t = sp.times[fi];
        int n = std::max(1, static_cast<int>(std::nearbyint(t * 20)));
        while (step < n) {
            V3 pos = sp.place;
            if (sp.move) pos = sp.place + sp.moveDir * sp.speed * step / 20.0;
            fx.SetWorld(pos, hasRot ? &rot : nullptr);
            if (fx.Alive()) fx.Update();
            ++step;
        }
        V3 at = sp.follow ? fx.Element().pos * fx.S() : V3{0, 8, 0};
        RFrame f;
        f.cam.W = W; f.cam.H = H;
        f.cam.eye = sp.follow ? at + sp.eye : sp.eye;
        f.cam.at = at;
        f.cam.Setup();
        if (sp.human) AddHumanLines(f, sp.follow ? V3{at.x, 0, at.z + 12} : V3{0, 0, 0});
        if (fx.Alive()) DrawEffect(fx, cache, f);
        char lab[160];
        sprintf_s(lab, "t=%.1fs tick=%d parts=%d%s", t, step, fx.Count(), fx.Alive() ? "" : " (released)");
        RFrame::Label L;
        L.text = std::wstring(lab, lab + std::strlen(lab));
        f.labels.push_back(L);
        Image img;
        sr.Draw(f, img, threads);
        for (int y = 0; y < H; ++y)
            std::memcpy(&sheet.rgba[(static_cast<std::size_t>(y + 16) * sheet.w + static_cast<std::size_t>(fi) * W) * 4],
                        &img.rgba[static_cast<std::size_t>(y) * W * 4], static_cast<std::size_t>(W) * 4);
        if (frames) frames->push_back({step, fx.Count(), fx.Alive()});
    }
    RFrame::Label T;
    T.x = 4; T.y = 2;
    T.text = sp.title.empty() ? sp.path : sp.title;
    T.rgb[0] = T.rgb[1] = T.rgb[2] = 255;
    DrawLabels(sheet, {T});
    return true;
}

}  // namespace se::render
