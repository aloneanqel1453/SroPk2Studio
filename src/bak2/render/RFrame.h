// SkillEffect Studio - render/RFrame.h
// One frame = camera + background + reference lines + ordered triangle batches with D3D-style state.
// Built by the scene (models, effects) and executed by either backend: SoftRaster (CPU, exact port of
// efp_sim.py's rasterizer; headless PNG) or GlView (OpenGL 1.x fixed function in a child window).
#pragma once

#include "RAssets.h"
#include "RMath.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace se::render {

// LookAtLH camera of efp_sim.py (vertical fov, w clamp 1e-3, no near-plane clipping in the CPU path)
struct RCamera {
    V3 eye{30, 26, -48}, at{0, 8, 0};
    int W = 400, H = 320;
    double fovDeg = 50.0;
    V3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};   // right, up, forward (rows of inv_view_rot)
    double f = 1.0;
    void Setup();
    V3 ToView(const V3& P) const { V3 d = P - eye; return {Dot(d, x), Dot(d, y), Dot(d, z)}; }
    void Project(const V3& P, double& sx, double& sy, double& w) const;
    M4 InvViewRot() const;   // rows = right, up, forward
};

struct RState {
    std::uint32_t cull = 1;                    // 1 none, 3 CCW (front = clockwise on screen, y down)
    std::uint32_t src = 5, dst = 6;            // D3DBLEND
    std::uint32_t cs[3] = {2, 0, 2};           // stage0 colour arg1, arg2, op
    std::uint32_t as[3] = {2, 0, 2};           // stage0 alpha arg1, arg2, op
    bool ztest = false, zwrite = false;
    float alphaRef = -1.0f;                    // alpha test: discard when alpha < ref (model cut-outs)
    bool lit = false;                          // model triangle: per-vertex colour, opaque path allowed
    bool operator==(const RState& o) const;
};

struct RVtx { V3 p; double u = 0, v = 0; double c[4] = {1, 1, 1, 1}; };   // c = r, g, b, a (0..1)

struct RBatch {
    RState st;
    std::shared_ptr<RTexture> tex;             // nullptr = white
    std::vector<RVtx> v;                       // triangle list
};

struct RLine { V3 a, b; std::uint8_t rgb[3] = {89, 89, 89}; };

struct RFrame {
    RCamera cam;
    double bg[3] = {0.07, 0.08, 0.10};
    bool bgGradient = false;                   // studio gradient (top -> bottom) instead of flat
    double bg2[3] = {0.02, 0.02, 0.03};
    std::vector<RLine> lines;                  // drawn first, 1 px (efp_sim: PIL on the byte image)
    std::vector<RBatch> batches;               // drawn in order
    struct Label { int x = 4, y = 3; std::wstring text; std::uint8_t rgb[3] = {230, 230, 120}; };
    std::vector<Label> labels;                 // drawn last (GDI)
    void Clear() { lines.clear(); batches.clear(); labels.clear(); }
    RBatch& Batch(const RState& st, const std::shared_ptr<RTexture>& tex);   // reuse the last one when equal
};

}  // namespace se::render
