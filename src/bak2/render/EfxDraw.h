// SkillEffect Studio - render/EfxDraw.h
// Effect geometry = port of efp_sim.py Renderer.draw_effect / orient / draw_link: particles in CAEFF0
// bucket order -> plate quads / .bms meshes / LinkPipe (uniform cubic B-spline, 3 samples per segment) /
// LinkDPipe ribbons facing the camera; world = sim * S().
#pragma once

#include "EfpSim.h"
#include "RFrame.h"

namespace se::render {

struct EfxDrawOptions {
    bool ztest = false;     // test against model depth (scene mode); efp_sim reference = false
};

// appends the effect's triangles to frame.batches (textures / meshes from cache, Arc::Particles)
void DrawEffect(EfpEffect& fx, AssetCache& cache, RFrame& frame, const EfxDrawOptions& opt = {});

}  // namespace se::render
