// SkillEffect Studio v6 - ddj/Portrait.h  (M38)
// Portrait gallery engine: a monster / pet / NPC / player model rendered headless (render module CPU rasteriser)
// into a PNG with a real alpha channel (two renders over black and white, difference matte; anti-aliased edges
// survive), auto-cropped and centred. Batch over many objects on worker threads with a shared AssetCache, model
// de-duplication, a static index.html gallery and a report of objects without a model. Read-only on the client.
#pragma once

#include "ImageIo.h"
#include "../render/RAssets.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace se::ddj {

struct PortraitOptions {
    int size = 256;              // output square
    double yawDeg = 215;         // camera orbit (Scene::FrameModels default = front three-quarter)
    double pitchDeg = 12;
    double distMul = 2.6;
    double poseMs = 600;         // idle clip time of the pose
    int ssaa = 2;                // supersampling (1..3)
    bool transparent = true;     // false: flat background colour below
    std::uint32_t background = 0xFF2A2420;   // RGBA (R low byte) when !transparent
    bool shadow = false;         // soft contact shadow (comes out semi-transparent)
    bool palette = false;        // .bsr mod-palette particles (glows) - off by default
    float margin = 0.06f;        // free border after cropping (share of size)
    int rasterThreads = 4;       // CPU raster threads of one render
};

struct PortraitJob {
    std::wstring code;           // object code (file name stem)
    std::wstring name;           // display name (gallery)
    std::wstring kindTr;         // "Unique", "Canavar", "Pet", "Binek", "NPC", "Paralı asker", "Oyuncu"
    std::wstring model;          // Data inner path (.bsr / .cpd); "" = no model -> reported
    int materialSet = 0;
    double scale = 1.0;
    int level = 0;
    std::wstring skipTr;         // non-empty: not rendered, reported with this reason (e.g. model file missing)
};

struct PortraitResult {
    std::wstring code, name, kindTr, model;
    std::wstring file;           // PNG name inside outDir ("" on failure)
    std::wstring gif;            // optional idle GIF
    bool ok = false;
    bool reused = false;         // same model + material as an earlier job: PNG copied
    std::wstring whyTr;          // failure reason (plain Turkish)
    double ms = 0;
    float coverage = 0;          // share of opaque pixels (0..1)
};

class PortraitRenderer {
public:
    explicit PortraitRenderer(se::render::AssetCache& cache) : m_cache(cache) {}
    // RGBA portrait (size x size). false + whyTr when the model does not load / shows nothing.
    bool Render(const PortraitJob& job, const PortraitOptions& o, Image& out, std::wstring* whyTr = nullptr);
    // animated idle GIF (studio background, GIF has no soft alpha). ms of animation at fps.
    bool RenderGif(const PortraitJob& job, const PortraitOptions& o, const std::wstring& path, int ms, int fps, std::wstring* whyTr = nullptr);

private:
    se::render::AssetCache& m_cache;
};

struct BatchOptions {
    PortraitOptions portrait;
    std::wstring outDir;         // refused when inside a client folder (pk2 / sro_client.exe)
    int workers = 0;             // 0 = auto (<= 4); each worker renders one portrait at a time
    bool html = true;            // index.html + report.txt
    bool skipExisting = false;   // keep PNGs already in outDir
    int gifMs = 0;               // > 0: also an idle GIF per object (slow)
    bool english = false;        // gallery / report language
    std::wstring title;          // gallery title ("" = default)
    std::function<void(int done, int total, const std::wstring& code)> progress;   // any thread
    const std::atomic<bool>* cancel = nullptr;
};

struct BatchReport {
    int total = 0, ok = 0, failed = 0, noModel = 0, reused = 0, skipped = 0;
    double seconds = 0;
    std::vector<PortraitResult> items;   // job order
    std::wstring indexHtml, reportTxt;   // full paths ("" when html = false)
};

bool RunPortraitBatch(se::render::AssetCache& cache, const std::vector<PortraitJob>& jobs, const BatchOptions& o,
                      BatchReport& rep, std::wstring* whyTr = nullptr);

// static gallery (relative image links, filter box, missing-model list). Atomic write.
bool WriteGalleryHtml(const std::wstring& path, const std::vector<PortraitResult>& items, const std::wstring& title,
                      bool english, std::wstring* whyTr = nullptr);

std::wstring SafeFileStem(const std::wstring& code);   // ASCII, [a-z0-9_-], lower case

}  // namespace se::ddj
