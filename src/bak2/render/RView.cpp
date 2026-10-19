// SkillEffect Studio - render/RView.cpp
#include "RView.h"

#include "SoftRaster.h"

#include <windowsx.h>
#include <gl/GL.h>
#include <gl/GLU.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#ifndef GL_COMBINE
#define GL_COMBINE 0x8570
#define GL_COMBINE_RGB 0x8571
#define GL_COMBINE_ALPHA 0x8572
#define GL_RGB_SCALE 0x8573
#define GL_ADD_SIGNED 0x8574
#define GL_INTERPOLATE 0x8575
#define GL_CONSTANT 0x8576
#define GL_PRIMARY_COLOR 0x8577
#define GL_PREVIOUS 0x8578
#define GL_SOURCE0_RGB 0x8580
#define GL_SOURCE1_RGB 0x8581
#define GL_SOURCE2_RGB 0x8582
#define GL_SOURCE0_ALPHA 0x8588
#define GL_SOURCE1_ALPHA 0x8589
#define GL_SOURCE2_ALPHA 0x858A
#define GL_OPERAND0_RGB 0x8590
#define GL_OPERAND1_RGB 0x8591
#define GL_OPERAND2_RGB 0x8592
#define GL_OPERAND0_ALPHA 0x8598
#define GL_OPERAND1_ALPHA 0x8599
#define GL_OPERAND2_ALPHA 0x859A
#define GL_SUBTRACT 0x84E7
#endif

// [SroPk2Studio quality] tokens of WGL_ARB_pixel_format, WGL_ARB_multisample, GL_ARB_multisample,
// GL_EXT_texture_filter_anisotropic and GL 1.4 GL_GENERATE_MIPMAP (not in the SDK's GL 1.1 headers)
#define SE_WGL_DRAW_TO_WINDOW_ARB 0x2001
#define SE_WGL_ACCELERATION_ARB 0x2003
#define SE_WGL_SUPPORT_OPENGL_ARB 0x2010
#define SE_WGL_DOUBLE_BUFFER_ARB 0x2011
#define SE_WGL_PIXEL_TYPE_ARB 0x2013
#define SE_WGL_COLOR_BITS_ARB 0x2014
#define SE_WGL_ALPHA_BITS_ARB 0x201B
#define SE_WGL_DEPTH_BITS_ARB 0x2022
#define SE_WGL_STENCIL_BITS_ARB 0x2023
#define SE_WGL_FULL_ACCELERATION_ARB 0x2027
#define SE_WGL_TYPE_RGBA_ARB 0x202B
#define SE_WGL_SAMPLE_BUFFERS_ARB 0x2041
#define SE_WGL_SAMPLES_ARB 0x2042
#define SE_GL_MULTISAMPLE_ARB 0x809D
#define SE_GL_SAMPLE_BUFFERS_ARB 0x80A8
#define SE_GL_SAMPLES_ARB 0x80A9
#define SE_GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define SE_GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#define SE_GL_GENERATE_MIPMAP 0x8191

namespace se::render {

namespace {

const wchar_t* kClass = L"SeRenderView";

// [SroPk2Studio quality] CPU fallback: the scene is rendered at kCpuSsaa x size (Scene::Render's SSAA path)
constexpr int kCpuSsaa = 2;
constexpr int kMaxSamples = 16;

// [SroPk2Studio quality] WGL_ARB_pixel_format + WGL_ARB_multisample: a window's pixel format can be set only once,
// so wglChoosePixelFormatARB is fetched through a throw-away window + legacy context. Returns the accelerated
// RGBA8 / D24S8 double-buffered format with the most samples <= kMaxSamples for `target`, 0 when the driver has
// none (the caller then keeps ChoosePixelFormat's plain format).
int ChooseMultisampleFormat(HDC target) {
    using ChooseFn = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
    using AttribFn = BOOL(WINAPI*)(HDC, int, int, UINT, const int*, int*);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"SeRenderViewGlProbe";
    RegisterClassW(&wc);   // fails harmlessly when already registered
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!w) return 0;
    HDC dc = GetDC(w);
    int best = 0, bestSamples = 0;
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    const int pf = dc ? ChoosePixelFormat(dc, &pfd) : 0;
    if (pf && SetPixelFormat(dc, pf, &pfd)) {
        if (HGLRC rc = wglCreateContext(dc)) {
            if (wglMakeCurrent(dc, rc)) {
                const auto choose = reinterpret_cast<ChooseFn>(reinterpret_cast<void*>(wglGetProcAddress("wglChoosePixelFormatARB")));
                const auto attrib = reinterpret_cast<AttribFn>(reinterpret_cast<void*>(wglGetProcAddress("wglGetPixelFormatAttribivARB")));
                if (choose && attrib) {
                    const int want[] = {SE_WGL_DRAW_TO_WINDOW_ARB, 1, SE_WGL_SUPPORT_OPENGL_ARB, 1, SE_WGL_DOUBLE_BUFFER_ARB, 1,
                                        SE_WGL_ACCELERATION_ARB, SE_WGL_FULL_ACCELERATION_ARB, SE_WGL_PIXEL_TYPE_ARB, SE_WGL_TYPE_RGBA_ARB,
                                        SE_WGL_COLOR_BITS_ARB, 32, SE_WGL_ALPHA_BITS_ARB, 8, SE_WGL_DEPTH_BITS_ARB, 24,
                                        SE_WGL_STENCIL_BITS_ARB, 8, SE_WGL_SAMPLE_BUFFERS_ARB, 1, SE_WGL_SAMPLES_ARB, 2, 0};
                    int formats[128] = {};
                    UINT n = 0;
                    if (choose(target, want, nullptr, 128, formats, &n)) {
                        const int q = SE_WGL_SAMPLES_ARB;
                        for (UINT i = 0; i < std::min<UINT>(n, 128); ++i) {   // best-first order: keep the first of a count
                            int samples = 0;
                            if (attrib(target, formats[i], 0, 1, &q, &samples) && samples <= kMaxSamples && samples > bestSamples) {
                                best = formats[i];
                                bestSamples = samples;
                            }
                        }
                    }
                }
                wglMakeCurrent(nullptr, nullptr);
            }
            wglDeleteContext(rc);
        }
    }
    if (dc) ReleaseDC(w, dc);
    DestroyWindow(w);
    return best;
}

bool HasGlExtension(const char* list, const char* name) {
    if (!list || !name || !*name) return false;
    const std::size_t n = std::strlen(name);
    for (const char* p = list; (p = std::strstr(p, name)) != nullptr; p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
    return false;
}

GLenum BlendOf(std::uint32_t c) {
    switch (c) {
        case 1: return GL_ZERO;
        case 2: return GL_ONE;
        case 3: return GL_SRC_COLOR;
        case 4: return GL_ONE_MINUS_SRC_COLOR;
        case 5: return GL_SRC_ALPHA;
        case 6: return GL_ONE_MINUS_SRC_ALPHA;
        case 7: return GL_ONE;    // DESTALPHA: framebuffer alpha treated as 1 (efp_sim)
        case 8: return GL_ZERO;
        case 9: return GL_DST_COLOR;
        case 10: return GL_ONE_MINUS_DST_COLOR;
        case 11: return GL_ZERO;
        default: return GL_ONE;
    }
}

GLint SrcOf(std::uint32_t code) {
    std::uint32_t b = code & 0x0F;
    if (b == 2) return GL_TEXTURE;
    if (b <= 1) return GL_PRIMARY_COLOR;
    return GL_CONSTANT;
}
GLint OpRgb(std::uint32_t code) {
    if (code & 0x20) return (code & 0x10) ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA;
    return (code & 0x10) ? GL_ONE_MINUS_SRC_COLOR : GL_SRC_COLOR;
}
GLint OpA(std::uint32_t code) { return (code & 0x10) ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA; }

void Combine(const std::uint32_t st[3], bool alpha) {
    const GLenum mode = alpha ? GL_COMBINE_ALPHA : GL_COMBINE_RGB;
    const GLenum s0 = alpha ? GL_SOURCE0_ALPHA : GL_SOURCE0_RGB, s1 = alpha ? GL_SOURCE1_ALPHA : GL_SOURCE1_RGB,
                 s2 = alpha ? GL_SOURCE2_ALPHA : GL_SOURCE2_RGB;
    const GLenum o0 = alpha ? GL_OPERAND0_ALPHA : GL_OPERAND0_RGB, o1 = alpha ? GL_OPERAND1_ALPHA : GL_OPERAND1_RGB,
                 o2 = alpha ? GL_OPERAND2_ALPHA : GL_OPERAND2_RGB;
    const GLenum scale = alpha ? GL_ALPHA_SCALE : GL_RGB_SCALE;
    auto op = [&](std::uint32_t c) { return alpha ? OpA(c) : OpRgb(c); };
    const std::uint32_t a1 = st[0], a2 = st[1];
    GLfloat sc = 1.0f;
    switch (st[2]) {
        case 2: glTexEnvi(GL_TEXTURE_ENV, mode, GL_REPLACE); glTexEnvi(GL_TEXTURE_ENV, s0, SrcOf(a1)); glTexEnvi(GL_TEXTURE_ENV, o0, op(a1)); break;
        case 3: glTexEnvi(GL_TEXTURE_ENV, mode, GL_REPLACE); glTexEnvi(GL_TEXTURE_ENV, s0, SrcOf(a2)); glTexEnvi(GL_TEXTURE_ENV, o0, op(a2)); break;
        case 4: case 5: case 6: case 7: case 8: case 9: case 10: {
            GLint m = GL_MODULATE;
            if (st[2] == 5 || st[2] == 9) sc = 2.0f;
            if (st[2] == 6) sc = 4.0f;
            if (st[2] == 7) m = GL_ADD;
            if (st[2] == 8 || st[2] == 9) m = GL_ADD_SIGNED;
            if (st[2] == 10) m = GL_SUBTRACT;
            glTexEnvi(GL_TEXTURE_ENV, mode, m);
            glTexEnvi(GL_TEXTURE_ENV, s0, SrcOf(a1)); glTexEnvi(GL_TEXTURE_ENV, o0, op(a1));
            glTexEnvi(GL_TEXTURE_ENV, s1, SrcOf(a2)); glTexEnvi(GL_TEXTURE_ENV, o1, op(a2));
            break;
        }
        case 11:   // ADDSMOOTH a + b - ab = interpolate(1, b, a)
            glTexEnvi(GL_TEXTURE_ENV, mode, GL_INTERPOLATE);
            glTexEnvi(GL_TEXTURE_ENV, s0, GL_CONSTANT); glTexEnvi(GL_TEXTURE_ENV, o0, alpha ? GL_SRC_ALPHA : GL_SRC_COLOR);
            glTexEnvi(GL_TEXTURE_ENV, s1, SrcOf(a2)); glTexEnvi(GL_TEXTURE_ENV, o1, op(a2));
            glTexEnvi(GL_TEXTURE_ENV, s2, SrcOf(a1)); glTexEnvi(GL_TEXTURE_ENV, o2, alpha ? OpA(a1) : OpRgb(a1));
            break;
        default:   // DISABLE / unsupported -> diffuse
            glTexEnvi(GL_TEXTURE_ENV, mode, GL_REPLACE);
            glTexEnvi(GL_TEXTURE_ENV, s0, GL_PRIMARY_COLOR);
            glTexEnvi(GL_TEXTURE_ENV, o0, alpha ? GL_SRC_ALPHA : GL_SRC_COLOR);
            break;
    }
    glTexEnvf(GL_TEXTURE_ENV, scale, sc);
}

}  // namespace

PreviewView::PreviewView() { QueryPerformanceFrequency(&m_freq); }

PreviewView::~PreviewView() {
    if (m_hwnd) DestroyWindow(m_hwnd);
}

bool PreviewView::Create(HWND parent, int x, int y, int w, int h, int ctrlId, bool allowGl) {
    m_allowGl = allowGl;
    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC | CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        reg = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    m_hwnd = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP, x, y,
                             w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ctrlId)), inst, this);
    if (!m_hwnd) return false;
    QueryPerformanceCounter(&m_prev);
    SetTimer(m_hwnd, 1, 15, nullptr);
    return true;
}

void PreviewView::Move(int x, int y, int w, int h) {
    if (m_hwnd) MoveWindow(m_hwnd, x, y, w, h, TRUE);
}

void PreviewView::SetScene(Scene* s) {
    m_scene = s;
    m_framed = false;
    Redraw();
}

void PreviewView::SetPlaying(bool p) {
    m_playing = p;
    QueryPerformanceCounter(&m_prev);
}

void PreviewView::SetOverlay(const std::vector<std::wstring>& lines) { m_overlay = lines; }
void PreviewView::SetEmptyText(const std::wstring& t) { m_empty = t; Redraw(); }
void PreviewView::Redraw() { if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE); }

void PreviewView::Step(double ms) {
    if (!m_scene) return;
    if (m_onTick) m_onTick(ms);
    m_scene->Advance(ms);
    Redraw();
}

void PreviewView::SetOrbit(double yawDeg, double pitchDeg, double dist) {
    m_yaw = yawDeg;
    m_pitch = std::clamp(pitchDeg, -85.0, 85.0);
    if (dist > 0) m_dist = dist;
    m_framed = true;
    Redraw();
}

void PreviewView::SetCamera(const V3& target, double yawDeg, double pitchDeg, double dist) {
    m_target = target;
    SetOrbit(yawDeg, pitchDeg, dist);   // sets m_framed and redraws
}

void PreviewView::FrameModels() {
    if (!m_scene) return;
    m_scene->FrameAll(m_yaw, m_pitch);   // models + live particles (effect-only previews too)
    m_target = m_scene->camera.at;
    m_dist = Len(m_scene->camera.eye - m_scene->camera.at);
    m_framed = true;
    Redraw();
}

void PreviewView::ApplyCamera() {
    if (!m_scene) return;
    if (!m_framed) FrameModels();
    double yw = m_yaw * kPi / 180, pt = m_pitch * kPi / 180;
    V3 dir{std::sin(yw) * std::cos(pt), std::sin(pt), std::cos(yw) * std::cos(pt)};
    m_scene->camera.at = m_target;
    m_scene->camera.eye = m_target + dir * m_dist;
    m_scene->camera.Setup();
}

LRESULT CALLBACK PreviewView::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    PreviewView* self = reinterpret_cast<PreviewView*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = static_cast<PreviewView*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = h;
    }
    if (self) return self->Proc(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT PreviewView::Proc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            m_dc = GetDC(m_hwnd);
            if (m_allowGl) InitGl();
            return 0;
        case WM_DESTROY:
            KillTimer(m_hwnd, 1);
            FreeGl();
            if (m_dc) ReleaseDC(m_hwnd, m_dc);
            m_dc = nullptr;
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
            m_hwnd = nullptr;
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER: {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double dt = 1000.0 * static_cast<double>(now.QuadPart - m_prev.QuadPart) / static_cast<double>(m_freq.QuadPart);
            m_prev = now;
            dt = std::min(dt, 100.0);
            m_fpsAcc += dt;
            if (m_playing && m_scene) {
                double d = dt * m_speed;
                if (m_onTick) m_onTick(d);
                m_scene->Advance(d);
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            if (m_fpsAcc >= 1000) { m_fps = m_frames * 1000.0 / m_fpsAcc; m_frames = 0; m_fpsAcc = 0; }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(m_hwnd, &ps);
            Paint();
            EndPaint(m_hwnd, &ps);
            ++m_frames;
            return 0;
        }
        case WM_SIZE:
            Redraw();
            return 0;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
            SetFocus(m_hwnd);
            SetCapture(m_hwnd);
            m_drag = msg == WM_LBUTTONDOWN ? 1 : 2;
            m_last = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            return 0;
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
            m_drag = 0;
            ReleaseCapture();
            return 0;
        case WM_LBUTTONDBLCLK:
            if (m_onFrame) m_onFrame();
            else FrameModels();
            return 0;
        case WM_MOUSEMOVE: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int dx = p.x - m_last.x, dy = p.y - m_last.y;
            m_last = p;
            if (m_drag == 1) {
                m_yaw += dx * 0.4;
                m_pitch = std::clamp(m_pitch + dy * 0.4, -85.0, 85.0);
                m_framed = true;
                Redraw();
            } else if (m_drag == 2 && m_scene) {
                const RCamera& c = m_scene->camera;
                double k = m_dist * 0.0018;
                m_target = m_target - c.x * (dx * k) + c.y * (dy * k);
                m_framed = true;
                Redraw();
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            m_dist = std::clamp(m_dist * std::pow(0.88, d / 120.0), 1.0, 20000.0);
            m_framed = true;
            Redraw();
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN:
            if (!m_scene) return 0;
            switch (wp) {
                case VK_SPACE: SetPlaying(!m_playing); break;
                case VK_HOME:
                    if (m_onFrame) m_onFrame();
                    else FrameModels();
                    break;
                case 'G': m_scene->opt.grid = !m_scene->opt.grid; break;
                case 'B': m_scene->opt.bones = !m_scene->opt.bones; break;
                case 'H': m_scene->opt.human = !m_scene->opt.human; break;
                case 'E': m_scene->opt.effects = !m_scene->opt.effects; break;
                case 'M': m_scene->opt.models = !m_scene->opt.models; break;
                case VK_RIGHT: if (!m_playing) Step(50); break;
                default: break;
            }
            Redraw();
            return 0;
        default:
            break;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

bool PreviewView::InitGl() {
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    // [SroPk2Studio quality] a multisampled format first (most samples <= 16), else the plain one below
    int pf = 0;
    if (const int ms = ChooseMultisampleFormat(m_dc); ms > 0) {
        PIXELFORMATDESCRIPTOR mp{};
        if (DescribePixelFormat(m_dc, ms, sizeof(mp), &mp) && SetPixelFormat(m_dc, ms, &mp)) pf = ms;
    }
    if (!pf) {
        pf = ChoosePixelFormat(m_dc, &pfd);
        if (!pf || !SetPixelFormat(m_dc, pf, &pfd)) return false;
    }
    m_gl = wglCreateContext(m_dc);
    if (!m_gl) return false;
    if (!wglMakeCurrent(m_dc, m_gl)) { wglDeleteContext(m_gl); m_gl = nullptr; return false; }
    // [SroPk2Studio quality] multisampling on, anisotropic filtering at the driver maximum, hardware mipmap
    // generation (GL 1.4 / SGIS_generate_mipmap) with non-power-of-two textures kept at their size when supported
    {
        GLint buffers = 0;
        glGetIntegerv(SE_GL_SAMPLE_BUFFERS_ARB, &buffers);
        if (glGetError() == GL_NO_ERROR && buffers > 0) {
            glEnable(SE_GL_MULTISAMPLE_ARB);
            GLint samples = 0;
            glGetIntegerv(SE_GL_SAMPLES_ARB, &samples);
            m_samples = samples;
        }
        const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        if (HasGlExtension(ext, "GL_EXT_texture_filter_anisotropic") || HasGlExtension(ext, "GL_ARB_texture_filter_anisotropic")) {
            GLfloat maxAniso = 0;
            glGetFloatv(SE_GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso);
            m_aniso = maxAniso > 1.0f ? maxAniso : 0.0f;
        }
        int major = 1, minor = 0;
        if (const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION))) {
            major = std::atoi(ver);
            if (const char* dot = std::strchr(ver, '.')) minor = std::atoi(dot + 1);
        }
        m_hwMip = major > 1 || (major == 1 && minor >= 4) || HasGlExtension(ext, "GL_SGIS_generate_mipmap");
        m_npot = major >= 2 || HasGlExtension(ext, "GL_ARB_texture_non_power_of_two");
        GLint maxTex = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
        m_maxTex = maxTex;
        while (glGetError() != GL_NO_ERROR) {}
    }
    const unsigned char white[4] = {255, 255, 255, 255};
    glGenTextures(1, &m_white);
    glBindTexture(GL_TEXTURE_2D, m_white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    m_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old = SelectObject(m_dc, m_font);
    m_fontBase = glGenLists(0x180);
    if (m_fontBase) wglUseFontBitmapsW(m_dc, 0, 0x180, m_fontBase);
    SelectObject(m_dc, old);
    return true;
}

void PreviewView::FreeGl() {
    if (!m_gl) return;
    wglMakeCurrent(m_dc, m_gl);
    for (auto& t : m_tex) glDeleteTextures(1, &t.id);
    m_tex.clear();
    if (m_white) glDeleteTextures(1, &m_white);
    if (m_fontBase) glDeleteLists(m_fontBase, 0x180);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(m_gl);
    m_gl = nullptr;
    if (m_font) DeleteObject(m_font);
    m_font = nullptr;
}

unsigned PreviewView::GlTexture(const std::shared_ptr<RTexture>& t) {
    if (!t || t->img.Empty()) return m_white;
    for (const auto& g : m_tex)
        if (g.keep.get() == t.get()) return g.id;
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // [SroPk2Studio quality] full mipmap chain for every texture: generated by the driver from the native-size
    // level 0 (RGBA8) when it can, else gluBuild2DMipmaps (rescales to a power of two / the size limit first)
    const int tw = t->img.w, th = t->img.h;
    const bool pot = (tw & (tw - 1)) == 0 && (th & (th - 1)) == 0;
    if (m_hwMip && (pot || m_npot) && tw <= m_maxTex && th <= m_maxTex) {
        glTexParameteri(GL_TEXTURE_2D, SE_GL_GENERATE_MIPMAP, GL_TRUE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, t->img.rgba.data());
    } else {
        gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGBA8, tw, th, GL_RGBA, GL_UNSIGNED_BYTE, t->img.rgba.data());
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    if (m_aniso > 1.0f) glTexParameterf(GL_TEXTURE_2D, SE_GL_TEXTURE_MAX_ANISOTROPY_EXT, m_aniso);   // [SroPk2Studio quality]
    if (m_tex.size() > 4096) {   // keep the cache bounded
        for (auto& g : m_tex) glDeleteTextures(1, &g.id);
        m_tex.clear();
    }
    m_tex.push_back({t, id});
    return id;
}

void PreviewView::DrawTextGl(int x, int y, const std::wstring& s, const float rgb[3], int W, int H) {
    if (!m_fontBase || s.empty()) return;
    (void)W;
    std::wstring t(s);
    for (auto& c : t)
        if (c >= 0x180) c = L'?';
    glColor3f(rgb[0], rgb[1], rgb[2]);
    glRasterPos2i(x, H - y - 12);
    glListBase(m_fontBase);
    glCallLists(static_cast<GLsizei>(t.size()), GL_UNSIGNED_SHORT, t.data());
}

void PreviewView::Paint() {
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    int W = std::max<int>(1, rc.right), H = std::max<int>(1, rc.bottom);
    RFrame f;
    BuildViewFrame(f, W, H);
    if (m_gl) {
        RenderGl(f, W, H);
        SwapBuffers(m_dc);
    } else {
        RenderCpu(m_dc, f, W, H);
    }
}

void PreviewView::BuildViewFrame(RFrame& f, int W, int H) {
    if (m_scene) {
        ApplyCamera();
        m_scene->camera.W = W;
        m_scene->camera.H = H;
        m_scene->BuildFrame(f);
    } else {
        f.cam.W = W; f.cam.H = H;
        f.cam.Setup();
        f.bgGradient = true;
        f.bg[0] = 0.17; f.bg[1] = 0.18; f.bg[2] = 0.21;
        f.bg2[0] = 0.04; f.bg2[1] = 0.04; f.bg2[2] = 0.05;
    }
    int y = 6;
    if (showOverlay)   // [SroPk2Studio] the host app may draw its own UI instead
        for (const auto& l : m_overlay) { RFrame::Label L; L.x = 8; L.y = y; L.text = l; L.rgb[0] = 236; L.rgb[1] = 236; L.rgb[2] = 220; f.labels.push_back(L); y += 16; }
    if (!m_scene && !m_empty.empty()) { RFrame::Label L; L.x = 12; L.y = H / 2 - 8; L.text = m_empty; L.rgb[0] = L.rgb[1] = L.rgb[2] = 170; f.labels.push_back(L); }
    if (m_scene && showOverlay) {
        wchar_t st[160];
        swprintf_s(st, english ? L"%s  t=%.2fs  tick %lld  effects %d  particles %d  %.0f fps" : L"%s  t=%.2fs  tick %lld  efekt %d  parçacık %d  %.0f fps",
                   m_playing ? L">" : L"||",
                   m_scene->TimeMs() / 1000.0, static_cast<long long>(m_scene->Tick()), m_scene->EffectCount(),
                   m_scene->ParticleCount(), m_fps);
        RFrame::Label L; L.x = 8; L.y = H - 20; L.text = st; L.rgb[0] = 150; L.rgb[1] = 160; L.rgb[2] = 175;
        f.labels.push_back(L);
    }
}

void PreviewView::RenderGl(const RFrame& f, int W, int H) {
    wglMakeCurrent(m_dc, m_gl);
    glViewport(0, 0, W, H);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(static_cast<float>(f.bg[0]), static_cast<float>(f.bg[1]), static_cast<float>(f.bg[2]), 1.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    // background
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1, 0, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_LIGHTING);
    if (f.bgGradient) {
        glBegin(GL_QUADS);
        glColor3d(f.bg2[0], f.bg2[1], f.bg2[2]); glVertex2d(0, 0); glVertex2d(1, 0);
        glColor3d(f.bg[0], f.bg[1], f.bg[2]); glVertex2d(1, 1); glVertex2d(0, 1);
        glEnd();
    }
    // camera (view space: x right, y up, z forward; clip w = view z)
    const RCamera& c = f.cam;
    double dist = Len(c.at - c.eye);
    double zn = std::max(0.05, dist * 0.005), zf = dist * 60 + 4000;
    double P[16] = {};
    P[0] = c.f * H / W;
    P[5] = c.f;
    P[10] = (zf + zn) / (zf - zn);
    P[11] = 1;
    P[14] = -2 * zf * zn / (zf - zn);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixd(P);
    double V[16] = {c.x.x, c.y.x, c.z.x, 0, c.x.y, c.y.y, c.z.y, 0, c.x.z, c.y.z, c.z.z, 0,
                    -Dot(c.x, c.eye), -Dot(c.y, c.eye), -Dot(c.z, c.eye), 1};
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixd(V);
    // lines (under everything, no depth)
    glBegin(GL_LINES);
    for (const RLine& l : f.lines) {
        glColor3ub(l.rgb[0], l.rgb[1], l.rgb[2]);
        glVertex3d(l.a.x, l.a.y, l.a.z);
        glVertex3d(l.b.x, l.b.y, l.b.z);
    }
    glEnd();
    // batches
    glEnable(GL_TEXTURE_2D);
    const GLfloat white[4] = {1, 1, 1, 1};
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, white);
    for (const RBatch& b : f.batches) {
        const RState& st = b.st;
        glBindTexture(GL_TEXTURE_2D, GlTexture(b.tex));
        if (st.lit) {
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        } else {
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            Combine(st.cs, false);
            Combine(st.as, true);
        }
        if (st.cull == 3 || st.cull == 2) {
            glEnable(GL_CULL_FACE);
            glFrontFace(GL_CCW);
            glCullFace(st.cull == 3 ? GL_FRONT : GL_BACK);
        } else glDisable(GL_CULL_FACE);
        if (st.ztest) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); }
        else glDisable(GL_DEPTH_TEST);
        glDepthMask(st.zwrite ? GL_TRUE : GL_FALSE);
        if (st.alphaRef >= 0) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GEQUAL, st.alphaRef); }
        else glDisable(GL_ALPHA_TEST);
        std::uint32_t sb = st.src, db = st.dst;
        if (sb == 12) { sb = 5; db = 6; }
        else if (sb == 13) { sb = 6; db = 5; }
        if (sb == 2 && db == 1) glDisable(GL_BLEND);
        else { glEnable(GL_BLEND); glBlendFunc(BlendOf(sb), BlendOf(db)); }
        glBegin(GL_TRIANGLES);
        for (const RVtx& v : b.v) {
            glColor4d(v.c[0], v.c[1], v.c[2], v.c[3]);
            glTexCoord2d(v.u, v.v);
            glVertex3d(v.p.x, v.p.y, v.p.z);
        }
        glEnd();
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_BLEND);
    // labels
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    for (const auto& l : f.labels) {
        const float shadow[3] = {0.0f, 0.0f, 0.0f};  // readable over bright effects
        DrawTextGl(l.x + 1, l.y + 1, l.text, shadow, W, H);
        float rgb[3] = {l.rgb[0] / 255.0f, l.rgb[1] / 255.0f, l.rgb[2] / 255.0f};
        DrawTextGl(l.x, l.y, l.text, rgb, W, H);
    }
    glFlush();
}

void PreviewView::RenderCpu(HDC dc, const RFrame& f, int W, int H) {
    SoftRaster sr;
    RFrame g = f;
    std::vector<RFrame::Label> labels;
    labels.swap(g.labels);
    // [SroPk2Studio quality] the scene kCpuSsaa x supersampled through Scene::Render's SSAA path (camera already
    // applied by BuildViewFrame; box filter down), the labels drawn at 1x on top
    if (m_scene) m_scene->Render(m_cpu, W, H, 6, kCpuSsaa);
    else sr.Draw(g, m_cpu, 6);
    std::vector<RFrame::Label> shadowed;  // a dark copy one pixel down-right: readable over bright effects
    for (const auto& l : labels) {
        RFrame::Label s = l;
        s.x += 1;
        s.y += 1;
        s.rgb[0] = s.rgb[1] = s.rgb[2] = 0;
        shadowed.push_back(s);
    }
    for (const auto& l : labels) shadowed.push_back(l);
    DrawLabels(m_cpu, shadowed);
    std::vector<std::uint8_t> bgra(m_cpu.rgba.size());
    for (std::size_t i = 0; i < m_cpu.rgba.size(); i += 4) {
        bgra[i] = m_cpu.rgba[i + 2]; bgra[i + 1] = m_cpu.rgba[i + 1]; bgra[i + 2] = m_cpu.rgba[i]; bgra[i + 3] = 255;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    SetDIBitsToDevice(dc, 0, 0, static_cast<DWORD>(W), static_cast<DWORD>(H), 0, 0, 0, static_cast<UINT>(H), bgra.data(), &bi, DIB_RGB_COLORS);
}

bool PreviewView::Capture(Image& out) {
    if (!m_hwnd) return false;
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    int W = std::max<int>(1, rc.right), H = std::max<int>(1, rc.bottom);
    RFrame f;
    BuildViewFrame(f, W, H);
    if (!m_gl) {
        SoftRaster sr;
        if (m_scene) {   // [SroPk2Studio quality] supersampled like RenderCpu, labels at 1x
            m_scene->Render(out, W, H, 6, kCpuSsaa);
            DrawLabels(out, f.labels);
        } else {
            sr.Draw(f, out, 6);
        }
        return !out.Empty();
    }
    RenderGl(f, W, H);
    out.w = W;
    out.h = H;
    out.rgba.assign(static_cast<std::size_t>(W) * H * 4, 255);
    std::vector<std::uint8_t> tmp(out.rgba.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
    for (int y = 0; y < H; ++y)
        std::memcpy(&out.rgba[static_cast<std::size_t>(y) * W * 4], &tmp[static_cast<std::size_t>(H - 1 - y) * W * 4], static_cast<std::size_t>(W) * 4);
    for (std::size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
    SwapBuffers(m_dc);
    return true;
}

}  // namespace se::render
