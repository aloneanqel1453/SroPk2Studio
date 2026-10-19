#pragma once
// Assembled 3D model data + a Direct3D 11 child-window viewer.
// Renders Silkroad .bsr objects (meshes from .bms, textures from .bmt/.ddj)
// with per-pixel lighting, MSAA, orbit/zoom. Static bind pose (no skinning).
#include "dds.hpp"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pk2 {

struct MVertex { float px, py, pz, nx, ny, nz, u, v; float r = 1, g = 1, b = 1, a = 1; };
// Layer: blended by vertex alpha over the coplanar surface drawn before it (terrain splats, water).
enum class MBlend : std::uint8_t { Opaque, Alpha, Additive, Layer };
struct MMesh {
    std::vector<MVertex> verts;
    std::vector<std::uint32_t> idx;
    int tex = -1; // index into ModelData::textures, or -1
    float alpha_test = 0.35f;            // texels below this alpha are cut out (0 = never)
    MBlend blend = MBlend::Opaque;       // blended parts are drawn after the solid ones
    // Skinning: mesh-local bone names + per-vertex 2-bone weights (empty = static mesh).
    std::vector<std::wstring> skin_bones;
    std::vector<std::uint8_t> bone_idx; // 2 per vertex
    std::vector<float> bone_w;          // 2 per vertex
};

// Raw skeleton + animation (matrix work happens in ModelView).
struct MBone {
    std::wstring name, parent;
    float rotP[4]{0, 0, 0, 1}, transP[3]{};  // bind local (rel. parent)
    float rotO[4]{0, 0, 0, 1}, transO[3]{};  // bind world (rel. origin)
};
struct MAnimBone {
    std::wstring name;
    std::vector<float> rot;    // 4 per keyframe
    std::vector<float> trans;  // 3 per keyframe
};
struct MAnim {
    std::uint32_t duration = 0;
    std::vector<std::uint32_t> times;
    std::vector<MAnimBone> bones;
    [[nodiscard]] bool empty() const { return times.empty() || bones.empty(); }
};

struct ModelData {
    std::vector<MMesh> meshes;
    std::vector<Image> textures;
    std::vector<MBone> bones;   // skeleton (JMXVBSK), may be empty
    MAnim anim;                 // default animation (JMXVBAN), may be empty
    float cx = 0, cy = 0, cz = 0, radius = 1.0f;
    // Starting view (radians): a model faces the camera from slightly above; a map region is
    // seen from higher up.
    float yaw0 = -1.4f, pitch0 = 0.12f;
    [[nodiscard]] bool empty() const { return meshes.empty(); }
};
class ModelView {
public:
    ModelView() = default;
    ~ModelView();

    bool init(HWND parent, HINSTANCE hinst);
    void destroy();
    [[nodiscard]] bool ready() const noexcept { return device_ != nullptr; }

    void set_rect(int x, int y, int w, int h);
    void show(bool visible);
    [[nodiscard]] bool visible() const noexcept { return visible_; }

    void upload(const ModelData& model);
    void clear_model();
    [[nodiscard]] bool has_model() const noexcept { return index_total_ > 0; }
    void render(); // draws one frame (call per tick while visible)

    void set_auto_rotate(bool b) noexcept { auto_rotate_ = b; }
    void toggle_auto_rotate() noexcept { auto_rotate_ = !auto_rotate_; }
    [[nodiscard]] bool auto_rotate() const noexcept { return auto_rotate_; }
    // Back to the starting view of the loaded model (angles, distance, no pan).
    void reset_view() noexcept {
        yaw_ = yaw0_; pitch_ = pitch0_; dist_ = radius_ * 1.9f; pan_x_ = pan_y_ = pan_z_ = 0; auto_rotate_ = false;
    }

    void toggle_animate() noexcept { animate_ = !animate_; }
    [[nodiscard]] bool animating() const noexcept { return animate_; }
    [[nodiscard]] bool has_anim() const noexcept { return !anim_times_.empty(); }

    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void create_targets(int w, int h);
    void release_targets();

    HWND hwnd_ = nullptr;
    HINSTANCE hinst_ = nullptr;
    bool visible_ = false;
    int w_ = 16, h_ = 16;

    void* device_ = nullptr;      // ID3D11Device*
    void* context_ = nullptr;     // ID3D11DeviceContext*
    void* swap_ = nullptr;        // IDXGISwapChain*
    void* rtv_ = nullptr;         // ID3D11RenderTargetView*
    void* dsv_ = nullptr;         // ID3D11DepthStencilView*
    void* vs_ = nullptr; void* ps_ = nullptr; void* layout_ = nullptr;
    void* cbuf_ = nullptr; void* sampler_ = nullptr; void* raster_ = nullptr; void* depth_state_ = nullptr;
    void* depth_read_ = nullptr;      // depth test without writes (blended parts)
    void* blend_alpha_ = nullptr; void* blend_add_ = nullptr;
    void* white_srv_ = nullptr;

    struct GpuMesh {
        void* vb; void* ib; UINT index_count; int tex;
        float alpha_test = 0.35f;
        MBlend blend = MBlend::Opaque;
        bool skinned = false;
        std::vector<MVertex> base;      // CPU bind-pose verts (for per-frame skinning)
        std::vector<int> vb0, vb1;      // per-vertex skeleton bone indices
        std::vector<float> vw0;         // per-vertex weight of bone0 (w1 = 1-w0)
    };
    std::vector<GpuMesh> gpu_;
    std::vector<void*> srvs_; // ID3D11ShaderResourceView* per texture
    UINT index_total_ = 0;

    // Skeleton + default animation (filled in upload()).
    std::vector<int> bone_parent_;
    std::vector<float> bind_local_;   // 16 floats per bone
    std::vector<float> inv_bind_;     // 16 floats per bone
    std::uint32_t anim_dur_ = 0;
    std::vector<std::uint32_t> anim_times_;
    std::vector<std::vector<float>> anim_rot_;   // per bone: 4*keyframe (empty = not animated)
    std::vector<std::vector<float>> anim_trans_; // per bone: 3*keyframe
    bool animate_ = true;
    std::uint64_t anim_start_ = 0;
    void skin_frame();                // CPU-skin all skinned meshes for the current time

    float cx_ = 0, cy_ = 0, cz_ = 0, radius_ = 1.0f;
    float yaw_ = -1.4f, pitch_ = 0.12f, dist_ = 3.0f;
    float yaw0_ = -1.4f, pitch0_ = 0.12f;
    float pan_x_ = 0, pan_y_ = 0, pan_z_ = 0;   // look-at point offset (world units)
    int drag_ = 0;                              // 1 = rotate (left), 2 = pan (right / middle)
    POINT last_{};
    bool auto_rotate_ = false;
};

} // namespace pk2
