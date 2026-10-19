#include "model3d.hpp"

#include <windowsx.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

using namespace DirectX;

namespace pk2 {
namespace {
template <class T> void rel(void*& p) { if (p) { static_cast<T*>(p)->Release(); p = nullptr; } }

const char* kShader = R"(
cbuffer CB : register(b0) {
  row_major float4x4 wvp;
  row_major float4x4 world;
  float3 lightDir; float pad0;
  float3 lightDir2; float pad1;
  float alphaRef; int mode; float2 pad2;   // mode 0 solid / cut-out, 1 alpha blend, 2 additive, 3 vertex-alpha layer
};
struct VSIn { float3 pos:POSITION; float3 nrm:NORMAL; float2 uv:TEXCOORD; float4 col:COLOR; };
struct PSIn { float4 pos:SV_POSITION; float3 wn:NORMAL; float2 uv:TEXCOORD; float4 col:COLOR; };
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
PSIn VS(VSIn i){ PSIn o; o.pos=mul(float4(i.pos,1),wvp); o.wn=mul(i.nrm,(float3x3)world); o.uv=i.uv; o.col=i.col; return o; }
float4 PS(PSIn i):SV_TARGET{
  float3 n=normalize(i.wn);
  float3 L1=-normalize(lightDir);
  float3 L2=-normalize(lightDir2);
  // Two-sided lighting so back-facing (CW) triangles are still lit, not black.
  float d1=max(abs(dot(n,L1))*0.35, dot(n,L1));
  float d2=max(abs(dot(n,L2))*0.35, dot(n,L2));
  d1=saturate(d1); d2=saturate(d2);
  float4 texc=tex0.Sample(samp0,i.uv);
  if(mode!=3 && texc.a < alphaRef) discard;   // cut-out: transparent texels must not show as black
  float3 base=texc.rgb*i.col.rgb;
  // Bright studio-style key+fill with a strong ambient so models look lively, not dark.
  float3 lit=base*(0.95 + 0.85*d1 + 0.45*d2);
  // Gentle saturation + contrast lift for a vivid look.
  float g=dot(lit,float3(0.299,0.587,0.114));
  lit=lerp(float3(g,g,g),lit,1.18);
  lit=pow(saturate(lit),0.82);
  if(mode==3) { if(i.col.a < 0.004) discard; return float4(lit,i.col.a); }
  if(mode==1) return float4(lit,texc.a*i.col.a);
  if(mode==2) return float4(lit*texc.a,1);
  return float4(lit,1);
}
)";

struct CB {
    XMFLOAT4X4 wvp;
    XMFLOAT4X4 world;
    XMFLOAT3 lightDir; float pad0;
    XMFLOAT3 lightDir2; float pad1;
    float alphaRef; int mode; float pad2[2];
};
} // namespace

ModelView::~ModelView() { destroy(); }

LRESULT CALLBACK ModelView::proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) { auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp); ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams)); }
    ModelView* self = reinterpret_cast<ModelView*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(h, m, wp, lp);
    if (m == WM_NCCREATE) self->hwnd_ = h;
    return self->handle(m, wp, lp);
}
// Turntable camera: the model turns the way the mouse moves (drag right = it turns right,
// drag down = its top tilts toward you), always level with the ground, never upside down.
// Right / middle drag pans, the wheel zooms, a double-click (or Home) resets the view.
constexpr float kTurnPerPixel = 0.008f;   // radians
constexpr float kPitchLimit = 1.45f;      // ~83 degrees above / below

LRESULT ModelView::handle(UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        drag_ = (m == WM_LBUTTONDOWN) ? 1 : 2;
        auto_rotate_ = false;
        last_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ::SetFocus(hwnd_);
        ::SetCapture(hwnd_);
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        drag_ = 0;
        if (::GetCapture() == hwnd_) ::ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED: drag_ = 0; return 0;
    case WM_LBUTTONDBLCLK: reset_view(); render(); return 0;
    case WM_MOUSEMOVE: {
        if (!drag_) return 0;
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        const float dx = static_cast<float>(x - last_.x), dy = static_cast<float>(y - last_.y);
        last_ = {x, y};
        if (drag_ == 1) {
            yaw_ -= dx * kTurnPerPixel;
            pitch_ = std::clamp(pitch_ + dy * kTurnPerPixel, -kPitchLimit, kPitchLimit);
        } else {
            // Pan in the view plane, scaled so the point under the cursor follows it.
            const float k = dist_ * 0.0015f;
            const XMVECTOR fwd = XMVector3Normalize(XMVectorSet(-cosf(pitch_) * cosf(yaw_), -sinf(pitch_), -cosf(pitch_) * sinf(yaw_), 0));
            const XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), fwd));
            const XMVECTOR up = XMVector3Cross(fwd, right);
            XMFLOAT3 r3, u3;
            XMStoreFloat3(&r3, right); XMStoreFloat3(&u3, up);
            pan_x_ += -r3.x * dx * k + u3.x * dy * k;
            pan_y_ += -r3.y * dx * k + u3.y * dy * k;
            pan_z_ += -r3.z * dx * k + u3.z * dy * k;
        }
        render();
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int d = GET_WHEEL_DELTA_WPARAM(wp);
        dist_ = std::clamp(dist_ * std::pow(0.88f, static_cast<float>(d) / WHEEL_DELTA), radius_ * 0.05f, radius_ * 12.0f);
        render();
        return 0;
    }
    case WM_KEYDOWN: {
        const float step = 0.12f;
        switch (wp) {
        case VK_LEFT:  yaw_ += step; break;
        case VK_RIGHT: yaw_ -= step; break;
        case VK_UP:    pitch_ = std::clamp(pitch_ - step, -kPitchLimit, kPitchLimit); break;
        case VK_DOWN:  pitch_ = std::clamp(pitch_ + step, -kPitchLimit, kPitchLimit); break;
        case VK_ADD: case VK_OEM_PLUS:       dist_ = std::max(dist_ * 0.88f, radius_ * 0.05f); break;
        case VK_SUBTRACT: case VK_OEM_MINUS: dist_ = std::min(dist_ / 0.88f, radius_ * 12.0f); break;
        case VK_HOME: case 'R': reset_view(); break;
        case VK_SPACE: auto_rotate_ = !auto_rotate_; break;
        default: return ::DefWindowProcW(hwnd_, m, wp, lp);
        }
        render();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; ::BeginPaint(hwnd_, &ps); render(); ::EndPaint(hwnd_, &ps); return 0; }
    }
    return ::DefWindowProcW(hwnd_, m, wp, lp);
}

bool ModelView::init(HWND parent, HINSTANCE hinst) {
    hinst_ = hinst;
    static bool reg = false;
    if (!reg) { WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.style = CS_DBLCLKS; wc.lpfnWndProc = &ModelView::proc; wc.hInstance = hinst; wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"SroPk2ModelView"; ::RegisterClassExW(&wc); reg = true; }
    hwnd_ = ::CreateWindowExW(0, L"SroPk2ModelView", L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, w_, h_, parent, nullptr, hinst, this);
    if (!hwnd_) return false;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = w_; sd.BufferDesc.Height = h_;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60; sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd_;
    sd.SampleDesc.Count = 8; sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; IDXGISwapChain* sc = nullptr;
    D3D_FEATURE_LEVEL fl;
    UINT flags = 0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) {
        // retry with 4x MSAA then no MSAA
        sd.SampleDesc.Count = 4;
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
        if (FAILED(hr)) { sd.SampleDesc.Count = 1; hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx); }
    }
    if (FAILED(hr)) return false;
    device_ = dev; context_ = ctx; swap_ = sc;

    // Stop DXGI from hooking the top-level window. With a child-window swapchain, DXGI's default
    // WndProc interception (Alt+Enter fullscreen / automatic window changes) acts on the main
    // application window and can resize it or swap its style, corrupting our frameless UI.
    {
        IDXGIDevice* dxdev = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxdev))) && dxdev) {
            IDXGIAdapter* adp = nullptr;
            if (SUCCEEDED(dxdev->GetAdapter(&adp)) && adp) {
                IDXGIFactory* fac = nullptr;
                if (SUCCEEDED(adp->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&fac))) && fac) {
                    fac->MakeWindowAssociation(parent, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
                    fac->Release();
                }
                adp->Release();
            }
            dxdev->Release();
        }
    }

    // shaders
    ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsb, &err))) { if (err) err->Release(); return false; }
    if (FAILED(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psb, &err))) { if (err) err->Release(); vsb->Release(); return false; }
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    vs_ = vs; ps_ = ps;
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11InputLayout* layout = nullptr;
    dev->CreateInputLayout(il, 4, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout);
    layout_ = layout;
    vsb->Release(); psb->Release();

    D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = sizeof(CB); cbd.Usage = D3D11_USAGE_DYNAMIC; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* cb = nullptr; dev->CreateBuffer(&cbd, nullptr, &cb); cbuf_ = cb;

    // Anisotropic + full mip chains (upload): sharp at any angle and distance.
    D3D11_SAMPLER_DESC spd{};
    spd.Filter = D3D11_FILTER_ANISOTROPIC;
    spd.AddressU = spd.AddressV = spd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    spd.MaxLOD = D3D11_FLOAT32_MAX;
    spd.MaxAnisotropy = 16;
    spd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ID3D11SamplerState* samp = nullptr; dev->CreateSamplerState(&spd, &samp); sampler_ = samp;

    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.MultisampleEnable = TRUE; rd.AntialiasedLineEnable = TRUE;
    ID3D11RasterizerState* rs = nullptr; dev->CreateRasterizerState(&rd, &rs); raster_ = rs;

    D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS;
    ID3D11DepthStencilState* ds = nullptr; dev->CreateDepthStencilState(&dd, &ds); depth_state_ = ds;
    // Blended parts: depth-tested, not depth-written, drawn after everything solid. Less-or-equal so
    // a splat layer lying exactly on the surface below it passes.
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    ID3D11DepthStencilState* dr = nullptr; dev->CreateDepthStencilState(&dd, &dr); depth_read_ = dr;
    {
        D3D11_BLEND_DESC bd{};
        auto& t = bd.RenderTarget[0];
        t.BlendEnable = TRUE;
        t.SrcBlend = D3D11_BLEND_SRC_ALPHA; t.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; t.BlendOp = D3D11_BLEND_OP_ADD;
        t.SrcBlendAlpha = D3D11_BLEND_ONE; t.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ID3D11BlendState* ba = nullptr; dev->CreateBlendState(&bd, &ba); blend_alpha_ = ba;
        t.SrcBlend = D3D11_BLEND_ONE; t.DestBlend = D3D11_BLEND_ONE;
        ID3D11BlendState* bp = nullptr; dev->CreateBlendState(&bd, &bp); blend_add_ = bp;
    }

    // 1x1 white fallback texture
    std::uint32_t white = 0xFFFFFFFF;
    D3D11_TEXTURE2D_DESC td{}; td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sr{}; sr.pSysMem = &white; sr.SysMemPitch = 4;
    ID3D11Texture2D* wt = nullptr; dev->CreateTexture2D(&td, &sr, &wt);
    if (wt) { ID3D11ShaderResourceView* wsrv = nullptr; dev->CreateShaderResourceView(wt, nullptr, &wsrv); white_srv_ = wsrv; wt->Release(); }

    create_targets(w_, h_);
    return true;
}

void ModelView::release_targets() { rel<ID3D11RenderTargetView>(rtv_); rel<ID3D11DepthStencilView>(dsv_); }

void ModelView::create_targets(int w, int h) {
    if (!device_ || w < 1 || h < 1) return;
    auto* dev = static_cast<ID3D11Device*>(device_);
    auto* sc = static_cast<IDXGISwapChain*>(swap_);
    release_targets();
    sc->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* bb = nullptr; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
    if (bb) { ID3D11RenderTargetView* rtv = nullptr; dev->CreateRenderTargetView(bb, nullptr, &rtv); rtv_ = rtv;
        D3D11_TEXTURE2D_DESC bd{}; bb->GetDesc(&bd); bb->Release();
        D3D11_TEXTURE2D_DESC dsd{}; dsd.Width = w; dsd.Height = h; dsd.MipLevels = 1; dsd.ArraySize = 1; dsd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dsd.SampleDesc = bd.SampleDesc; dsd.Usage = D3D11_USAGE_DEFAULT; dsd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ID3D11Texture2D* depth = nullptr; dev->CreateTexture2D(&dsd, nullptr, &depth);
        if (depth) { ID3D11DepthStencilView* dsv = nullptr; dev->CreateDepthStencilView(depth, nullptr, &dsv); dsv_ = dsv; depth->Release(); }
    }
}

void ModelView::set_rect(int x, int y, int w, int h) {
    if (!hwnd_) return;
    if (w < 1) w = 1; if (h < 1) h = 1;
    ::MoveWindow(hwnd_, x, y, w, h, TRUE);
    // Round the child window so its corners follow the preview panel (no black square corners).
    HRGN rgn = ::CreateRoundRectRgn(0, 0, w + 1, h + 1, 20, 20);
    ::SetWindowRgn(hwnd_, rgn, TRUE);
    if ((w != w_ || h != h_) && device_) { w_ = w; h_ = h; create_targets(w, h); }
    else { w_ = w; h_ = h; }
}
void ModelView::show(bool v) { visible_ = v; if (hwnd_) ::ShowWindow(hwnd_, v ? SW_SHOW : SW_HIDE); }

void ModelView::clear_model() {
    for (auto& g : gpu_) { rel<ID3D11Buffer>(g.vb); rel<ID3D11Buffer>(g.ib); }
    gpu_.clear();
    for (void*& s : srvs_) rel<ID3D11ShaderResourceView>(s);
    srvs_.clear();
    index_total_ = 0;
    bone_parent_.clear(); bind_local_.clear(); inv_bind_.clear();
    anim_dur_ = 0; anim_times_.clear(); anim_rot_.clear(); anim_trans_.clear();
}

static XMMATRIX compose(const float q[4], const float t[3]) {
    XMVECTOR quat = XMVectorSet(q[0], q[1], q[2], q[3]);
    return XMMatrixRotationQuaternion(quat) * XMMatrixTranslation(t[0], t[1], t[2]);
}

void ModelView::upload(const ModelData& model) {
    if (!device_) return;
    clear_model();
    auto* dev = static_cast<ID3D11Device*>(device_);
    cx_ = model.cx; cy_ = model.cy; cz_ = model.cz; radius_ = model.radius > 0 ? model.radius : 1.0f;
    yaw0_ = model.yaw0; pitch0_ = model.pitch0;
    reset_view();

    for (const Image& img : model.textures) {
        void* srv = nullptr;
        if (img.width && img.height && !img.bgra.empty()) {
            // Full mip chain generated on the GPU (anisotropic sampling needs it to stay crisp, not shimmer).
            auto* ctx = static_cast<ID3D11DeviceContext*>(context_);
            D3D11_TEXTURE2D_DESC td{}; td.Width = img.width; td.Height = img.height; td.MipLevels = 0; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            ID3D11Texture2D* t = nullptr; dev->CreateTexture2D(&td, nullptr, &t);
            if (t) {
                ctx->UpdateSubresource(t, 0, nullptr, img.bgra.data(), img.width * 4, 0);
                ID3D11ShaderResourceView* s = nullptr; dev->CreateShaderResourceView(t, nullptr, &s);
                if (s) ctx->GenerateMips(s);
                srv = s; t->Release();
            }
        }
        srvs_.push_back(srv);
    }

    // --- Skeleton: parent indices, bind-local matrices, inverse bind-world matrices ---
    std::map<std::wstring, int> boneIndex;
    const std::size_t nb = model.bones.size();
    for (std::size_t i = 0; i < nb; ++i) boneIndex[model.bones[i].name] = static_cast<int>(i);
    bone_parent_.assign(nb, -1);
    bind_local_.assign(nb * 16, 0.0f);
    inv_bind_.assign(nb * 16, 0.0f);
    std::vector<XMMATRIX> bindLocal(nb);
    for (std::size_t i = 0; i < nb; ++i) {
        const MBone& b = model.bones[i];
        auto it = boneIndex.find(b.parent);
        bone_parent_[i] = (it != boneIndex.end() && it->second != static_cast<int>(i)) ? it->second : -1;
        bindLocal[i] = compose(b.rotP, b.transP);
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(&bind_local_[i * 16]), bindLocal[i]);
    }
    // Bind-world from the bind-local hierarchy (self-consistent with the animated world),
    // so the inverse bind cancels exactly at rest and only animation deltas deform the mesh.
    std::vector<XMMATRIX> bindWorld(nb);
    std::vector<char> bdone(nb, 0);
    for (std::size_t pass = 0; pass <= nb; ++pass) {
        bool progress = false, anyLeft = false;
        for (std::size_t i = 0; i < nb; ++i) {
            if (bdone[i]) continue;
            int p = bone_parent_[i];
            if (p < 0) { bindWorld[i] = bindLocal[i]; bdone[i] = 1; progress = true; }
            else if (bdone[static_cast<std::size_t>(p)]) { bindWorld[i] = bindLocal[i] * bindWorld[static_cast<std::size_t>(p)]; bdone[i] = 1; progress = true; }
            else anyLeft = true;
        }
        if (!anyLeft) break;
        if (!progress) { for (std::size_t i = 0; i < nb; ++i) if (!bdone[i]) { bindWorld[i] = bindLocal[i]; bdone[i] = 1; } break; }
    }
    for (std::size_t i = 0; i < nb; ++i)
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(&inv_bind_[i * 16]), XMMatrixInverse(nullptr, bindWorld[i]));

    // --- Animation: map animated bone names to skeleton indices ---
    bool haveAnim = !model.anim.empty() && nb > 0;
    if (haveAnim) {
        anim_dur_ = model.anim.duration ? model.anim.duration : (model.anim.times.empty() ? 0 : model.anim.times.back());
        anim_times_ = model.anim.times;
        anim_rot_.assign(nb, {}); anim_trans_.assign(nb, {});
        for (const MAnimBone& ab : model.anim.bones) {
            auto it = boneIndex.find(ab.name);
            if (it == boneIndex.end()) continue;
            anim_rot_[static_cast<std::size_t>(it->second)] = ab.rot;
            anim_trans_[static_cast<std::size_t>(it->second)] = ab.trans;
        }
    }

    for (const MMesh& m : model.meshes) {
        if (m.verts.empty() || m.idx.empty()) continue;
        GpuMesh g{}; g.index_count = static_cast<UINT>(m.idx.size()); g.tex = m.tex;
        g.alpha_test = m.alpha_test; g.blend = m.blend;
        // Resolve this mesh's local bone list to skeleton indices for per-vertex skinning.
        bool skin = haveAnim && !m.skin_bones.empty() && m.bone_idx.size() == m.verts.size() * 2 && m.bone_w.size() == m.verts.size() * 2;
        if (skin) {
            std::vector<int> local2skel(m.skin_bones.size(), -1);
            for (std::size_t i = 0; i < m.skin_bones.size(); ++i) {
                auto it = boneIndex.find(m.skin_bones[i]);
                local2skel[i] = (it != boneIndex.end()) ? it->second : -1;
            }
            g.base = m.verts;
            g.vb0.resize(m.verts.size()); g.vb1.resize(m.verts.size()); g.vw0.resize(m.verts.size());
            for (std::size_t v = 0; v < m.verts.size(); ++v) {
                std::uint8_t l0 = m.bone_idx[v * 2], l1 = m.bone_idx[v * 2 + 1];
                int s0 = (l0 < local2skel.size()) ? local2skel[l0] : -1;
                int s1 = (l1 < local2skel.size()) ? local2skel[l1] : -1;
                float w0 = m.bone_w[v * 2];
                // Drop any weight on an unresolved bone onto the valid one (avoid root-drag spikes).
                if (s0 < 0 && s1 < 0) { s0 = s1 = 0; w0 = 1.0f; }
                else if (s0 < 0) { s0 = s1; w0 = 0.0f; }
                else if (s1 < 0) { s1 = s0; w0 = 1.0f; }
                g.vb0[v] = s0; g.vb1[v] = s1; g.vw0[v] = w0;
            }
            g.skinned = true;
        }
        D3D11_BUFFER_DESC vbd{}; vbd.ByteWidth = static_cast<UINT>(m.verts.size() * sizeof(MVertex));
        vbd.Usage = g.skinned ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        if (g.skinned) vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SUBRESOURCE_DATA vd{}; vd.pSysMem = m.verts.data();
        ID3D11Buffer* vb = nullptr; dev->CreateBuffer(&vbd, &vd, &vb); g.vb = vb;
        D3D11_BUFFER_DESC ibd{}; ibd.ByteWidth = static_cast<UINT>(m.idx.size() * sizeof(std::uint32_t)); ibd.Usage = D3D11_USAGE_IMMUTABLE; ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA id{}; id.pSysMem = m.idx.data();
        ID3D11Buffer* ib = nullptr; dev->CreateBuffer(&ibd, &id, &ib); g.ib = ib;
        gpu_.push_back(std::move(g));
        index_total_ += g.index_count;
    }
    anim_start_ = ::GetTickCount64();
}

// CPU linear-blend skinning: compute the animated bone palette at the current
// time, then transform each skinned vertex into its dynamic vertex buffer.
void ModelView::skin_frame() {
    if (anim_times_.empty() || bone_parent_.empty()) return;
    const std::size_t nb = bone_parent_.size();
    std::uint64_t ms = ::GetTickCount64() - anim_start_;
    std::uint32_t dur = anim_dur_ ? anim_dur_ : 1000;
    std::uint32_t t = static_cast<std::uint32_t>(ms % dur);
    // keyframe interval
    std::size_t k = 0; const std::size_t kn = anim_times_.size();
    while (k + 1 < kn && anim_times_[k + 1] <= t) ++k;
    std::size_t k2 = (k + 1 < kn) ? k + 1 : k;
    float span = (k2 > k) ? static_cast<float>(anim_times_[k2] - anim_times_[k]) : 1.0f;
    float f = span > 0 ? static_cast<float>(t - anim_times_[k]) / span : 0.0f;
    if (f < 0) f = 0; if (f > 1) f = 1;

    // Animated local matrices for every bone (animated pose, else bind-local).
    std::vector<XMMATRIX> local(nb);
    for (std::size_t i = 0; i < nb; ++i) {
        const std::vector<float>& R = anim_rot_[i]; const std::vector<float>& T = anim_trans_[i];
        std::size_t kcount = R.size() / 4;
        if (kcount > 0) {
            std::size_t ka = std::min(k, kcount - 1), kb = std::min(k2, kcount - 1);
            XMVECTOR qa = XMVectorSet(R[ka * 4], R[ka * 4 + 1], R[ka * 4 + 2], R[ka * 4 + 3]);
            XMVECTOR qb = XMVectorSet(R[kb * 4], R[kb * 4 + 1], R[kb * 4 + 2], R[kb * 4 + 3]);
            XMVECTOR q = XMQuaternionSlerp(qa, qb, f);
            XMVECTOR ta = XMVectorSet(T[ka * 3], T[ka * 3 + 1], T[ka * 3 + 2], 0);
            XMVECTOR tb = XMVectorSet(T[kb * 3], T[kb * 3 + 1], T[kb * 3 + 2], 0);
            XMVECTOR tr = XMVectorLerp(ta, tb, f);
            local[i] = XMMatrixRotationQuaternion(q) * XMMatrixTranslationFromVector(tr);
        } else {
            local[i] = XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(&bind_local_[i * 16]));
        }
    }
    // Resolve world matrices in any bone order (parent may appear after child) via passes.
    std::vector<XMMATRIX> world(nb);
    std::vector<char> done(nb, 0);
    for (std::size_t pass = 0; pass <= nb; ++pass) {
        bool progress = false, anyLeft = false;
        for (std::size_t i = 0; i < nb; ++i) {
            if (done[i]) continue;
            int p = bone_parent_[i];
            if (p < 0) { world[i] = local[i]; done[i] = 1; progress = true; }
            else if (done[static_cast<std::size_t>(p)]) { world[i] = local[i] * world[static_cast<std::size_t>(p)]; done[i] = 1; progress = true; }
            else anyLeft = true;
        }
        if (!anyLeft) break;
        if (!progress) { for (std::size_t i = 0; i < nb; ++i) if (!done[i]) { world[i] = local[i]; done[i] = 1; } break; }
    }
    std::vector<XMMATRIX> skin(nb);
    for (std::size_t i = 0; i < nb; ++i) {
        XMMATRIX invBind = XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(&inv_bind_[i * 16]));
        skin[i] = invBind * world[i];
    }

    auto* ctx = static_cast<ID3D11DeviceContext*>(context_);
    std::vector<MVertex> tmp;
    for (GpuMesh& g : gpu_) {
        if (!g.skinned) continue;
        tmp = g.base;
        for (std::size_t v = 0; v < tmp.size(); ++v) {
            int b0 = g.vb0[v], b1 = g.vb1[v]; float w0 = g.vw0[v], w1 = 1.0f - w0;
            if (b0 < 0 || static_cast<std::size_t>(b0) >= nb) b0 = 0;
            if (b1 < 0 || static_cast<std::size_t>(b1) >= nb) b1 = b0;
            XMMATRIX m = skin[b0] * w0 + skin[b1] * w1;
            const MVertex& s = g.base[v];
            XMVECTOR p = XMVector3Transform(XMVectorSet(s.px, s.py, s.pz, 1), m);
            XMVECTOR n = XMVector3TransformNormal(XMVectorSet(s.nx, s.ny, s.nz, 0), m);
            n = XMVector3Normalize(n);
            XMFLOAT3 pf, nf; XMStoreFloat3(&pf, p); XMStoreFloat3(&nf, n);
            tmp[v].px = pf.x; tmp[v].py = pf.y; tmp[v].pz = pf.z;
            tmp[v].nx = nf.x; tmp[v].ny = nf.y; tmp[v].nz = nf.z;
        }
        D3D11_MAPPED_SUBRESOURCE ms2;
        if (SUCCEEDED(ctx->Map(static_cast<ID3D11Buffer*>(g.vb), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms2))) {
            std::memcpy(ms2.pData, tmp.data(), tmp.size() * sizeof(MVertex));
            ctx->Unmap(static_cast<ID3D11Buffer*>(g.vb), 0);
        }
    }
}

void ModelView::render() {
    if (!device_ || !rtv_ || !visible_) return;
    auto* ctx = static_cast<ID3D11DeviceContext*>(context_);
    auto* sc = static_cast<IDXGISwapChain*>(swap_);
    auto* rtv = static_cast<ID3D11RenderTargetView*>(rtv_);
    auto* dsv = static_cast<ID3D11DepthStencilView*>(dsv_);

    if (auto_rotate_) yaw_ += 0.006f;
    if (animate_ && !anim_times_.empty()) skin_frame();

    D3D11_VIEWPORT vp{}; vp.Width = static_cast<float>(w_); vp.Height = static_cast<float>(h_); vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    float clear[4] = {0.13f, 0.15f, 0.19f, 1.0f}; // lighter studio backdrop
    ctx->ClearRenderTargetView(rtv, clear);
    if (dsv) ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    ctx->OMSetRenderTargets(1, &rtv, dsv);
    ctx->OMSetDepthStencilState(static_cast<ID3D11DepthStencilState*>(depth_state_), 0);
    ctx->RSSetState(static_cast<ID3D11RasterizerState*>(raster_));

    if (!gpu_.empty()) {
        // world: SR engine is Y-up, left-handed (per SilkroadDoc). Mesh data is already
        // in that space, so only center it — no axis swap (an X rotation makes it lie down).
        XMMATRIX world = XMMatrixTranslation(-(cx_ + pan_x_), -(cy_ + pan_y_), -(cz_ + pan_z_));

        float ex = cosf(pitch_) * cosf(yaw_), ey = sinf(pitch_), ez = cosf(pitch_) * sinf(yaw_);
        XMVECTOR eye = XMVectorSet(ex * dist_, ey * dist_, ez * dist_, 0);
        XMMATRIX view = XMMatrixLookAtLH(eye, XMVectorSet(0, 0, 0, 0), XMVectorSet(0, 1, 0, 0));
        float aspect = h_ > 0 ? static_cast<float>(w_) / static_cast<float>(h_) : 1.0f;
        XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, std::max(radius_ * 0.01f, dist_ * 0.02f), radius_ * 40.0f);
        XMMATRIX wvp = world * view * proj;

        CB cb{};
        XMStoreFloat4x4(&cb.wvp, wvp);
        XMStoreFloat4x4(&cb.world, world);
        cb.lightDir = XMFLOAT3(-0.4f, -0.7f, -0.55f);
        cb.lightDir2 = XMFLOAT3(0.5f, 0.3f, 0.6f);
        auto* cbuf = static_cast<ID3D11Buffer*>(cbuf_);

        ctx->IASetInputLayout(static_cast<ID3D11InputLayout*>(layout_));
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(static_cast<ID3D11VertexShader*>(vs_), nullptr, 0);
        ctx->PSSetShader(static_cast<ID3D11PixelShader*>(ps_), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cbuf);
        ID3D11SamplerState* samp = static_cast<ID3D11SamplerState*>(sampler_);
        ctx->PSSetSamplers(0, 1, &samp);

        // Solid and cut-out parts first, then the blended ones over them without depth writes.
        UINT stride = sizeof(MVertex), offset = 0;
        for (int pass = 0; pass < 2; ++pass) {
            for (const GpuMesh& g : gpu_) {
                if ((g.blend != MBlend::Opaque) != (pass == 1)) continue;
                cb.alphaRef = g.blend == MBlend::Opaque ? g.alpha_test : 0.004f;
                cb.mode = g.blend == MBlend::Alpha ? 1 : g.blend == MBlend::Additive ? 2 : g.blend == MBlend::Layer ? 3 : 0;
                D3D11_MAPPED_SUBRESOURCE ms;
                if (SUCCEEDED(ctx->Map(cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { std::memcpy(ms.pData, &cb, sizeof(cb)); ctx->Unmap(cbuf, 0); }
                const float factor[4] = {0, 0, 0, 0};
                const bool alpha_blend = g.blend == MBlend::Alpha || g.blend == MBlend::Layer;
                ctx->OMSetBlendState(static_cast<ID3D11BlendState*>(alpha_blend ? blend_alpha_ : g.blend == MBlend::Additive ? blend_add_ : nullptr), factor, 0xFFFFFFFF);
                ctx->OMSetDepthStencilState(static_cast<ID3D11DepthStencilState*>(pass == 1 ? depth_read_ : depth_state_), 0);
                ID3D11Buffer* vb = static_cast<ID3D11Buffer*>(g.vb);
                ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
                ctx->IASetIndexBuffer(static_cast<ID3D11Buffer*>(g.ib), DXGI_FORMAT_R32_UINT, 0);
                ID3D11ShaderResourceView* srv = static_cast<ID3D11ShaderResourceView*>(
                    (g.tex >= 0 && g.tex < static_cast<int>(srvs_.size()) && srvs_[g.tex]) ? srvs_[g.tex] : white_srv_);
                ctx->PSSetShaderResources(0, 1, &srv);
                ctx->DrawIndexed(g.index_count, 0, 0);
            }
        }
        const float factor[4] = {0, 0, 0, 0};
        ctx->OMSetBlendState(nullptr, factor, 0xFFFFFFFF);
    }
    sc->Present(1, 0);
}

void ModelView::destroy() {
    clear_model();
    rel<ID3D11ShaderResourceView>(white_srv_);
    rel<ID3D11DepthStencilState>(depth_state_);
    rel<ID3D11DepthStencilState>(depth_read_);
    rel<ID3D11BlendState>(blend_alpha_);
    rel<ID3D11BlendState>(blend_add_);
    rel<ID3D11RasterizerState>(raster_);
    rel<ID3D11SamplerState>(sampler_);
    rel<ID3D11Buffer>(cbuf_);
    rel<ID3D11InputLayout>(layout_);
    rel<ID3D11PixelShader>(ps_);
    rel<ID3D11VertexShader>(vs_);
    release_targets();
    rel<IDXGISwapChain>(swap_);
    rel<ID3D11DeviceContext>(context_);
    rel<ID3D11Device>(device_);
    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

} // namespace pk2
