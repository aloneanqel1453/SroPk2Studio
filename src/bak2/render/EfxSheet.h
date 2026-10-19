// SkillEffect Studio - render/EfxSheet.h
// Headless effect sheets = efp_sim.py run() + render_frame() + sheet(): simulate an .efp at fixed 20 Hz and
// render frames at given times next to an 18-unit reference human; one PNG strip. Used by the acceptance
// test (TERA\_re\efp\renders) and by the GUI for "preview this effect" thumbnails.
#pragma once

#include "EfxDraw.h"
#include "RImage.h"

#include <string>
#include <vector>

namespace se::render {

struct EfxRunSpec {
    std::wstring path;                         // inside Particles.pk2
    bool hold = false;                         // SetHold(1)
    V3 place{0, 9, 0};
    bool move = false;                         // missile: SetWorldMatrix every tick, local -Z = flight
    V3 moveDir{1, 0, 0};
    double speed = 0;                          // units / s
    V3 eye{30, 26, -48};
    bool follow = false;                       // camera follows the element
    std::uint32_t seed = 7;
    double scale = 1.0;
    std::vector<double> times{0.1, 0.3, 0.6, 1.0};
    int W = 400, H = 320;
    std::wstring title;                        // sheet title ("" = path)
    bool human = true;                         // reference stick figure + ground grid
};

struct EfxRunFrame { int tick = 0, parts = 0; bool alive = true; };

void AddHumanLines(RFrame& f, const V3& base);   // 18-unit human + 5-unit grid (efp_sim human_lines)
bool RunEffectSheet(AssetCache& cache, const EfxRunSpec& spec, Image& sheet, std::vector<EfxRunFrame>* frames,
                    std::string& err, int threads = 1);

}  // namespace se::render
