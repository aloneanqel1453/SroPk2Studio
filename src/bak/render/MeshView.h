// ============================================================================
//  SRO Asset Studio - render/MeshView.h
//
//  Interactive 3D viewport: a child HWND that owns a Rasterizer, a Camera and
//  the mouse interaction (orbit / pan / dolly). Used for .bms, .bsr, .bsk and
//  .ban playback.
//
//  Interaction (SPEC.md §3):
//    left drag           orbit
//    middle / right drag pan
//    wheel               dolly
//    F / double click    fit to the model AABB
//    1..4                shading mode
//
//  Rendering is lazy: a repaint is only issued when something changed or an
//  animation is playing, so an idle window costs nothing.
//
//  Implementation owner: Agent E (render).
// ============================================================================
#pragma once

#include "../Common.h"
#include "Raster.h"
#include "../formats/Bms.h"
#include "../formats/Bsr.h"
#include "../formats/Bsk.h"
#include "../formats/Ban.h"

#include <functional>

namespace sro {

/// What is currently loaded into the viewport.
enum class MeshViewContent : int {
    Empty = 0,
    Mesh,        ///< a single .bms
    Model,       ///< a composed .bsr
    Skeleton,    ///< a .bsk
    Animation    ///< a .bsk driven by a .ban
};

class MeshView {
public:
    MeshView();
    ~MeshView();

    MeshView(const MeshView&) = delete;
    MeshView& operator=(const MeshView&) = delete;

    /// Create the child window. `id` is the control id used for WM_COMMAND.
    bool create(HWND parent, const RECT& rc, int id);
    void destroy();
    HWND hwnd() const;
    void setBounds(const RECT& rc);
    void show(bool visible);

    // --- content -----------------------------------------------------------
    void clear();

    /// Single mesh, no textures.
    Result setMesh(const BmsFile& mesh, const std::wstring& titleTr);

    /// Composed .bsr model, with its textures.
    Result setModel(const ComposedModel& model);

    /// Skeleton only; drawn as bone lines with joint markers.
    Result setSkeleton(const BskFile& skeleton);

    /// Skeleton + animation. Playback controls become active.
    Result setAnimation(const BskFile& skeleton, const BanFile& animation);

    /// SKINNED playback: the model itself is deformed by the animation, so a mob
    /// moves instead of standing there as a bind-pose statue. Falls back to
    /// static geometry when the meshes carry no skin weights.
    Result setModelAnimation(const ComposedModel& model,
                             const BskFile& skeleton,
                             const BanFile& animation);

    MeshViewContent content() const;

    // --- view state --------------------------------------------------------
    void        setShadingMode(ShadingMode mode);
    ShadingMode shadingMode() const;
    void        setRenderOptions(const RenderOptions& opt);
    const RenderOptions& renderOptions() const;

    Camera&       camera();
    const Camera& camera() const;

    /// Reframe on the current content's AABB (keeps the current orbit angle).
    void fitToContent();

    /// Return to the exact initial view: default orbit angle + fit. This is the
    /// "Sıfırla" (reset) button and double-click / R key.
    void resetView();

    /// Force a redraw on the next message pump turn.
    void invalidate();

    // --- animation playback -------------------------------------------------
    void play();
    void pause();
    bool isPlaying() const;
    void setTimeMs(f32 ms);
    f32  timeMs() const;
    f32  durationMs() const;

    /// Called a few times a second while a clip plays, so the owner can refresh
    /// a status readout. Without it the "t / duration ms" line is computed once
    /// when the model is picked and then sits at "0 / N ms" forever, which reads
    /// as a frozen animation even when the model is moving perfectly.
    void setOnAnimTick(std::function<void()> fn);

    // --- headless ----------------------------------------------------------
    /// Render the current content at an explicit size without a window.
    /// This is what `--shot` uses for .bms / .bsr / .bsk.
    /// `ssOverride` > 0 forces a supersample factor instead of the still-image
    /// ladder - 1 is what the interactive view uses while a clip plays, so a
    /// benchmark can measure the frame cost the animation actually pays.
    Result renderToImage(int width, int height, Image& out, int ssOverride = 0);

    /// The scene as currently built (for OBJ export / diagnostics).
    const Scene& scene() const;

    /// Turkish one-line status for the info pane
    /// (e.g. "12.480 vertex, 8.220 face, 3 doku").
    std::wstring statusTr() const;

    /// Rich multi-line breakdown for the "Dosya Bilgisi" panel: totals,
    /// bounding box, per-part meshes + materials + resolved textures, bones,
    /// warnings. Aimed at people building new models. Empty when nothing loaded.
    std::wstring detailsTr() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sro
