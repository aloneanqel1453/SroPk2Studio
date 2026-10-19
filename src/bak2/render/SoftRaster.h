// SkillEffect Studio - render/SoftRaster.h
// CPU rasterizer. Effect triangles = literal port of efp_sim.py Renderer.tri (pixel-centre barycentrics,
// perspective UV, wrapped bilinear texel fetch, D3D stage0 colour/alpha ops, D3DBLEND factors, fb clip 0..4).
// Model triangles add a 1/w depth buffer, Gouraud colour and alpha test. Multi-threaded by row bands for
// the interactive path (identical results: every pixel is written by one thread in batch order).
#pragma once

#include "RFrame.h"
#include "RImage.h"

#include <vector>

namespace se::render {

class SoftRaster {
public:
    void Draw(const RFrame& f, Image& out, int threads = 1);
    // pieces (public for tests)
    void Begin(const RFrame& f);
    void DrawBatch(const RFrame& f, const RBatch& b, int y0, int y1);
    void End(const RFrame& f, Image& out) const;

private:
    void TriEfx(const RCamera& cam, const RVtx* v, const RTexture* tex, const RState& st, int y0, int y1);
    void TriLit(const RCamera& cam, const RVtx* v, const RTexture* tex, const RState& st, int y0, int y1);
    void Line(int x0, int y0, int x1, int y1, const std::uint8_t rgb[3]);
    int W = 0, H = 0;
    std::vector<double> m_fb;      // RGB
    std::vector<float> m_zb;       // 1/w, 0 = empty
};

// draw labels with GDI onto an image (Segoe UI 11 px)
void DrawLabels(Image& img, const std::vector<RFrame::Label>& labels);

}  // namespace se::render
