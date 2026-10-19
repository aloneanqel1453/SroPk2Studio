// SkillEffect Studio - render/RView.h
// Interactive 3D preview: a WS_CHILD window with an OpenGL 1.x fixed-function context (stage0 combine = the
// efp D3D colour/alpha ops, D3DBLEND -> glBlendFunc, alpha test, depth) that draws the same RFrame the CPU path
// renders; falls back to the CPU rasterizer (StretchDIBits) when no GL context can be made.
// Mouse: left drag = orbit, right/middle drag = pan, wheel = zoom, double click = frame models.
// Keys (when focused): Space = play/pause, Home = frame, G grid, B bones, H reference human, E effects, M models.
#pragma once

#include "RScene.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace se::render {

class PreviewView {
public:
    PreviewView();
    ~PreviewView();
    PreviewView(const PreviewView&) = delete;
    PreviewView& operator=(const PreviewView&) = delete;

    // creates the child window (control id) inside parent; returns false when the window cannot be created
    bool Create(HWND parent, int x, int y, int w, int h, int ctrlId = 0, bool allowGl = true);
    HWND Hwnd() const { return m_hwnd; }
    void Move(int x, int y, int w, int h);
    bool UsingGl() const { return m_gl != nullptr; }

    void SetScene(Scene* s);                         // not owned; nullptr = empty view
    Scene* GetScene() const { return m_scene; }
    void SetPlaying(bool p);
    bool Playing() const { return m_playing; }
    void SetSpeed(double s) { m_speed = s > 0 ? s : 1.0; }
    double Speed() const { return m_speed; }
    // called every frame BEFORE scene->Advance(dtMs) (drive the skill player here); dtMs already speed-scaled
    void SetOnTick(std::function<void(double dtMs)> cb) { m_onTick = std::move(cb); }
    // v5: Home / double click call this instead of FrameModels when set (the host frames a player its own way)
    void SetOnFrame(std::function<void()> cb) { m_onFrame = std::move(cb); }
    void SetOverlay(const std::vector<std::wstring>& lines);   // top-left text
    void SetEmptyText(const std::wstring& t);        // shown when no scene / nothing loaded
    void FrameModels();                              // orbit around the models, reset zoom
    void SetOrbit(double yawDeg, double pitchDeg, double dist);
    // v5: explicit orbit centre + angles + distance (player previews: PlayerDress FramePlayer -> ShotFrame)
    void SetCamera(const V3& target, double yawDeg, double pitchDeg, double dist);
    void Redraw();
    void Step(double ms);                            // advance while paused (frame stepping)
    // render the current frame off-screen-equivalent and read it back (GL back buffer or CPU image)
    bool Capture(Image& out);
    double Fps() const { return m_fps; }
    bool english = false;                            // language of the status line (bottom-left)
    // [SroPk2Studio] false hides the overlay lines (top-left) and the status / fps line (bottom-left)
    bool showOverlay = true;
    // [SroPk2Studio quality] what the GL context really uses: MSAA samples (0 = none), anisotropy (0 = none),
    // hardware mipmap generation (else gluBuild2DMipmaps)
    int GlSamples() const { return m_samples; }
    float GlAnisotropy() const { return m_aniso; }
    bool GlHwMipmaps() const { return m_hwMip; }

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Proc(UINT m, WPARAM w, LPARAM l);
    void ApplyCamera();
    void Paint();
    void BuildViewFrame(RFrame& f, int W, int H);
    void RenderGl(const RFrame& f, int W, int H);
    void RenderCpu(HDC dc, const RFrame& f, int W, int H);
    bool InitGl();
    void FreeGl();
    unsigned GlTexture(const std::shared_ptr<RTexture>& t);
    void DrawTextGl(int x, int y, const std::wstring& s, const float rgb[3], int W, int H);

    HWND m_hwnd = nullptr;
    HDC m_dc = nullptr;
    HGLRC m_gl = nullptr;
    bool m_allowGl = true;
    Scene* m_scene = nullptr;
    bool m_playing = true;
    double m_speed = 1.0;
    std::function<void(double)> m_onTick;
    std::function<void()> m_onFrame;
    std::vector<std::wstring> m_overlay;
    std::wstring m_empty;
    double m_yaw = 215, m_pitch = 16, m_dist = 60;
    V3 m_target{0, 8, 0};
    bool m_framed = false;
    POINT m_last{};
    int m_drag = 0;   // 1 orbit, 2 pan
    LARGE_INTEGER m_prev{}, m_freq{};
    double m_fps = 0;
    int m_frames = 0;
    double m_fpsAcc = 0;
    struct GlTex { std::shared_ptr<RTexture> keep; unsigned id = 0; };
    std::vector<GlTex> m_tex;
    unsigned m_white = 0;
    unsigned m_fontBase = 0;
    HFONT m_font = nullptr;
    Image m_cpu;
    // [SroPk2Studio quality]
    int m_samples = 0;
    float m_aniso = 0;
    bool m_hwMip = false, m_npot = false;
    int m_maxTex = 0;
};

}  // namespace se::render
