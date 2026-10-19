// ============================================================================
//  SRO Asset Studio - render/Raster.h
//
//  Software rasterizer. No GPU, no D3D, no OpenGL - it must work over RDP and
//  it must work headlessly so `--shot` can render a PNG without a window.
//
//  Engine conventions (Formats.md, "SR engine specifications"):
//    * LEFT-HANDED coordinate system, Y up.
//    * Vertex winding is CLOCKWISE - back-face culling must use that.
//    * UV: U left->right, V top->bottom (no V flip needed for our top-down
//      Image layout).
//
//  Render target is an Image (RGBA8) plus a parallel float depth buffer, so
//  the exact same buffer feeds StretchDIBits in the GUI and the PNG writer in
//  the CLI.
//
//  Implementation owner: Agent E (render).
// ============================================================================
#pragma once

#include "../Common.h"
#include "Image.h"

namespace sro {

// ---------------------------------------------------------------------------
// Minimal 4x4 matrix, row-major, row-vector convention: v' = v * M
// ---------------------------------------------------------------------------
struct Mat4 {
    f32 m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    static Mat4 Identity();
    static Mat4 Translation(const Vec3& t);
    static Mat4 Scale(const Vec3& s);
    static Mat4 RotationX(f32 radians);
    static Mat4 RotationY(f32 radians);
    static Mat4 RotationZ(f32 radians);
    static Mat4 FromQuat(const Quat& q);
    /// Left-handed look-at.
    static Mat4 LookAtLH(const Vec3& eye, const Vec3& target, const Vec3& up);
    /// Left-handed perspective; fovY in radians.
    static Mat4 PerspectiveFovLH(f32 fovY, f32 aspect, f32 zNear, f32 zFar);
    /// Left-handed orthographic.
    static Mat4 OrthoLH(f32 width, f32 height, f32 zNear, f32 zFar);

    Mat4 operator*(const Mat4& o) const;
    Vec3 transformPoint(const Vec3& p) const;      ///< w divide applied
    Vec3 transformVector(const Vec3& v) const;     ///< no translation, no divide
    Vec4 transformVec4(const Vec4& v) const;
    Mat4 transposed() const;
    bool invert(Mat4& out) const;
};

// ---------------------------------------------------------------------------
// Geometry submitted to the rasterizer
// ---------------------------------------------------------------------------
struct RasterVertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    Rgba color{ 255, 255, 255, 255 };
};

/// One draw call's worth of geometry. Indices are triangle lists.
struct RasterMesh {
    std::vector<RasterVertex> vertices;
    std::vector<u32>          indices;
    int   textureIndex = -1;      ///< index into Scene::textures, -1 = untextured
    bool  twoSided     = false;   ///< disable back-face culling for this mesh
    /// Per-mesh alpha cut-out threshold (0..1). 0 = fully opaque (default).
    /// Decided by ComputeTextureAlphaTest() gated on material Bit10; see Raster.cpp.
    f32   alphaTest    = 0.0f;

    // --- per-material transparency (reference Model.h drawOne) --------------
    // The reference draws in TWO passes: opaque/cut-out first with depth write
    // on, then everything blended or additive with depth write OFF, painter
    // style over the solid body. Without this a cape, a ghost robe, a wing or a
    // rare-item glow renders fully opaque - the single most visible gap left.
    bool  blendAlpha   = false;   ///< SRC_ALPHA / ONE_MINUS_SRC_ALPHA
    bool  additive     = false;   ///< D3DTOP_ADD: black texels vanish, glow adds
    /// Authored D3DBLEND render states. 0 = "not authored, use the bools above".
    /// A particle effect stores these per emitter; models leave them at 0.
    u32   srcBlend     = 0;
    u32   dstBlend     = 0;
    /// MeshBasicMaterial: no lighting, no specular, no fog. Particles are
    /// emissive sprites - lighting them washes them out toward flat white.
    bool  unlit        = false;
    f32   opacity      = 1.0f;    ///< clamp(diffuse.a * mapWeight) for blended meshes
    Vec3  tint  { 1.0f, 1.0f, 1.0f };   ///< flag 0x40 chromatic diffuse tint
    Vec3  emissive { 0.0f, 0.0f, 0.0f };///< self-lit materials
    /// SroPk2Studio interactive viewer: modulate by RasterVertex::color (rgb tint, alpha
    /// coverage); with `vertexAlphaLayer` the mesh is a splat layer blended by vertex alpha
    /// over the coplanar surface drawn before it (terrain ground types, water).
    bool  vertexColors     = false;
    bool  vertexAlphaLayer = false;

    Mat4  transform;              ///< model matrix

    Aabb computeBounds() const;
};

/// Decide the alpha cut-out threshold for a decoded RGBA texture, matching the
/// SHARE WebGL reference viewer's ddsAlphaTest():
///   0.0  = opaque (sheen/gloss alpha, or unused alpha) - the default
///   0.5  = clean binary cut-out (foliage/capes/leaves: near-black low-alpha)
///   0.06 = trim only fully-transparent-black gaps (gem/glow recesses)
/// This is the fix for the pet "siyahlik" (black patches): DXT1 punch-through and
/// baked transparent-black areas are cut out instead of rendering as black, while
/// metallic-sheen alpha stays fully opaque.
f32 ComputeTextureAlphaTest(const Image& rgba);

/// Bone line list for the skeleton viewer.
struct RasterLines {
    std::vector<Vec3> points;     ///< pairs: [0]-[1], [2]-[3], ...
    Rgba              color{ 0xFF, 0xCC, 0x33, 0xFF };
    Mat4              transform;
};

struct Scene {
    std::vector<RasterMesh>  meshes;
    std::vector<RasterLines> lines;
    std::vector<Image>       textures;
    Aabb                     bounds;

    void computeBounds();
    void clear();

    /// Rotate the model so its model-space `up` (recovered from the skeleton)
    /// points to world +Y, and - when `forward` is non-zero - so it also faces
    /// the camera (-Z). This is the RELIABLE orientation fix: the skeleton is
    /// ground truth, unlike a bounding-box guess. No-op when `up` is degenerate.
    void orientToUp(const Vec3& up, const Vec3& forward);

    /// Fallback for models with no usable skeleton: stand a clearly lying,
    /// elongated model up from its bounding box. Conservative - leaves upright
    /// or bulky models untouched. Recomputes bounds.
    void orientUpright();
};

// ---------------------------------------------------------------------------
// Camera - orbit style, which is what the viewer exposes
// ---------------------------------------------------------------------------
// The orientation is a QUATERNION, not yaw/pitch/roll, so the view is a true
// arcball: dragging in any direction rotates the model that way, every angle is
// reachable, and there is no pole or gimbal lock to fight. yaw/pitch survive only
// as a convenience for building named views (default 3/4, numpad snaps).
struct Camera {
    Vec3 target{ 0, 0, 0 };
    f32  distance  = 5.0f;
    Quat orient;                ///< view rotation: right=+X, up=+Y, forward=+Z
    f32  fovY      = 0.7853982f; ///< radians (45 deg, reference main.cpp:4070)
    f32  zNear     = 0.01f;
    f32  zFar      = 10000.0f;
    /// Bounding-SPHERE radius this camera was last framed for. projMatrix()
    /// derives near/far from it EVERY frame (reference main.cpp:4067-4069), so
    /// the depth range cannot go stale when the wheel changes `distance`.
    /// 0 = never fitted, fall back to the stored planes.
    f32  sceneRadius = 0.0f;
    Vec3 panOffset{ 0, 0, 0 };  ///< added to target, in view space axes
    bool orthographic = false;
    /// 'O' key / --shot --freeorbit: leave the level turntable for the historical
    /// screen-space arcball. The arcball reaches every orientation (including
    /// straight down the pole, which the +/-89 degree clamp forbids) at the cost
    /// of a level horizon - measured 16 degrees of roll on a 90 degree horizontal
    /// drag and 34.8 on a diagonal one, because the default view is pitched 16
    /// degrees so upVector() is not world-Y.
    bool freeOrbit = false;

    Camera();                   ///< starts at the level default 3/4 view

    Vec3 forwardVec() const;    ///< target - eye, normalized (camera +Z)
    Vec3 upVector() const;      ///< camera +Y
    Vec3 rightVec() const;      ///< camera +X
    Vec3 eyePosition() const;
    Mat4 viewMatrix() const;
    Mat4 projMatrix(int width, int height) const;

    /// Frame an AABB so the whole model is visible with a small margin.
    /// Tight default so the model fills the viewport (minimal empty backdrop).
    void fitToBounds(const Aabb& b, f32 marginFactor = 1.08f);

    /// Aspect-aware fit. The bounding SPHERE that fitToBounds() falls back on
    /// is the circumscribed one, so a tall thin model (every character) ends up
    /// framed for a diagonal it never occupies and renders small in the middle
    /// of an empty viewport. This projects the eight AABB corners onto the
    /// current view axes and fits the real silhouette in both axes instead.
    void fitToBoundsAspect(const Aabb& b, f32 aspect, f32 marginFactor = 1.06f);

    /// Screen-space trackball: `dYaw` rotates about the current up axis,
    /// `dPitch` about the current right axis. Reaches any orientation.
    void orbit(f32 dYaw, f32 dPitch);
    void roll(f32 dRadians);                ///< tilt about the view axis
    void levelHorizon();                    ///< remove roll, keep looking direction
    void dolly(f32 factor);                 ///< multiplicative, e.g. 1.1 / 0.9
    void pan(f32 dxPixels, f32 dyPixels, int viewportW, int viewportH);

    /// Build a level, world-up-framed view from yaw/pitch (default & numpad snaps).
    void setView(f32 yaw, f32 pitch);
};

// ---------------------------------------------------------------------------
// Shading
// ---------------------------------------------------------------------------
enum class ShadingMode : int {
    Textured = 0,   ///< texture x headlight lambert
    FlatLit,        ///< headlight lambert only
    Wireframe,
    Normals         ///< normal vector as RGB
};

struct RenderOptions {
    ShadingMode shading      = ShadingMode::Textured;
    Rgba        background{ 0x1A, 0x12, 0x09, 0xFF };   ///< top colour (or flat bg)
    /// Soft vertical studio gradient (top=background -> bottom=backgroundBottom).
    /// Easier on the eyes than flat white and makes light models read cleanly.
    bool        gradientBackground = false;
    Rgba        backgroundBottom{ 0x1A, 0x12, 0x09, 0xFF };
    /// Reference main.cpp:4059 disables GL_CULL_FACE for the whole studio frame -
    /// SRO winding is not reliable across .bms files - and compensates with
    /// GL_LIGHT_MODEL_TWO_SIDE, which fillTriangle already emulates by flipping
    /// away-facing normals. Only half of that port had landed: the shading half
    /// assumed culling was off while the state half still culled, so any opaque
    /// mesh with reversed winding silently vanished ("eksik parca").
    bool        backFaceCull = false;
    bool        bilinear     = true;
    bool        drawGrid     = false;
    bool        drawAxes     = false;
    bool        drawBounds   = false;

    /// Reference studio dressing (main.cpp:4144-4182): a soft contact shadow, an
    /// edge-faded floor grid and two gold podium rings, all drawn UNDER the
    /// model. This is the always-on showcase look - drawGrid/drawAxes/drawBounds
    /// above stay what they were, debug toggles on G / X / B.
    bool        studioFloor  = true;
    /// Linear depth fog (main.cpp:4106-4113). The colour is welded to the
    /// backdrop so the floor dissolves into it with no visible border, which is
    /// exactly the intent stated in the reference's own comment.
    bool        fogEnabled   = true;
    Vec3        fogColor { 0.60f, 0.62f, 0.67f };
    f32         fogStart     = 0.0f;   ///< filled per frame from the camera
    f32         fogEnd       = 0.0f;
    /// SRO alpha carries sheen, not transparency. Textures are sampled RGB and
    /// alpha is IGNORED unless this is explicitly turned on.
    bool        honourTextureAlpha = false;
    /// Alpha cut-out threshold for the CURRENT mesh (0 = none). The scene render
    /// loop sets this per mesh from RasterMesh::alphaTest. A texel whose sampled
    /// alpha is below this is discarded (cut out) - never blended.
    f32         alphaTest    = 0.0f;
    f32         ambient      = 0.25f;

    /// Scanline band this call may touch, [bandTop, bandBot). The mesh fill is
    /// split across worker threads by giving each one a disjoint band, so no two
    /// threads ever write the same pixel or depth slot - that is what makes the
    /// parallel rasteriser safe without a single lock. Full frame by default.
    int         bandTop      = 0;
    int         bandBot      = 0x7FFFFFFF;

    /// Per-mesh material state for the CURRENT mesh, set by the scene loop.
    bool        blendAlpha   = false;   ///< src-alpha blend, no depth write
    bool        additive     = false;   ///< add over the destination, no depth write
    bool        depthWrite   = true;    ///< false on the blended second pass
    f32         opacity      = 1.0f;
    Vec3        tint     { 1.0f, 1.0f, 1.0f };
    Vec3        emissive { 0.0f, 0.0f, 0.0f };

    // --- 3-POINT STUDIO LIGHTING (reference viewer, main.cpp studio block) --
    // Ported verbatim from SroEffectViewer: a high ambient keeps the
    // game-bright look, a warm KEY from the upper front-left gives volume, a
    // cool RIM from behind-right lifts the silhouette off the backdrop, and
    // specular is added AFTER texturing (GL_SEPARATE_SPECULAR_COLOR) so metal
    // catches real glints. This replaces the old single headlight lambert.
    bool        studioLight  = true;
    Vec3        lightAmbient { 0.88f, 0.88f, 0.90f };   // GL_LIGHT_MODEL_AMBIENT
    Vec3        keyDiffuse   { 0.34f, 0.33f, 0.30f };   // GL_LIGHT0 diffuse
    Vec3        rimDiffuse   { 0.16f, 0.19f, 0.26f };   // GL_LIGHT1 diffuse
    Vec3        keySpecular  { 0.55f, 0.55f, 0.60f };   // GL_LIGHT0 specular
    Vec3        matSpecular  { 0.35f, 0.35f, 0.38f };   // GL_FRONT_AND_BACK specular
    f32         shininess    = 26.0f;                   // GL_SHININESS
    /// World-space light directions (w=0 directional, pointing TOWARDS the
    /// light) exactly as the reference passes them to glLightfv(GL_POSITION).
    Vec3        keyDirWorld  { -0.4f, 0.85f,  0.5f };
    Vec3        rimDirWorld  {  0.55f, 0.35f, -0.8f };
    /// Filled by render() each frame: the above rotated into eye space, plus
    /// the key light's half-vector for the infinite-viewer specular.
    Vec3        keyDirEye    { 0.0f, 0.0f, -1.0f };
    Vec3        rimDirEye    { 0.0f, 0.0f, -1.0f };
    Vec3        keyHalfEye   { 0.0f, 0.0f, -1.0f };

    // --- authored particle render state ------------------------------------
    /// D3DBLEND states straight out of the .efp resource block. 0 means "not
    /// authored", and the blendAlpha/additive bools above decide instead, so
    /// model rendering is untouched.
    u32  srcBlend = 0;
    u32  dstBlend = 0;
    /// MeshBasicMaterial: no lighting, no specular, no fog. Particles are
    /// emissive sprites; lighting them washes them toward flat white.
    bool unlit    = false;
};

// ---------------------------------------------------------------------------
// Rasterizer
// ---------------------------------------------------------------------------
class Rasterizer {
public:
    Rasterizer();
    ~Rasterizer();

    /// Allocate/resize the colour + depth buffers. False on absurd sizes.
    bool resize(int width, int height);
    int  width()  const;
    int  height() const;

    /// Clear colour to `bg` and depth to +inf.
    void clear(Rgba bg);

    /// Render a whole scene. Returns false when the target is unallocated.
    bool render(const Scene& scene, const Camera& camera, const RenderOptions& opt);

    /// The rendered colour buffer (valid until the next resize).
    const Image& colorBuffer() const;
    Image&       colorBuffer();

    /// Depth buffer, one float per pixel, for picking.
    const std::vector<f32>& depthBuffer() const;

    /// Blit into a DC.
    bool present(HDC dc, const RECT& dst) const;

    // --- individual primitives (also used by the 2D previews) -------------
    void drawTriangle(const RasterVertex& a, const RasterVertex& b, const RasterVertex& c,
                      const Mat4& mvp, const Image* texture, const RenderOptions& opt);
    void drawLine3D(const Vec3& a, const Vec3& b, const Mat4& mvp, Rgba color);
    void drawGrid(const Mat4& mvp, f32 spacing, int halfCount, Rgba color);
    void drawAxes(const Mat4& mvp, f32 length);
    void drawAabb(const Aabb& box, const Mat4& mvp, Rgba color);

    /// Reference main.cpp:4144: the soft radial contact shadow on the floor
    /// plane. Depth-TESTED but never depth-WRITTEN, so the model still draws
    /// over it. This is what stops a model reading as a cut-out floating in air.
    void drawContactShadow(const Vec3& centre, f32 radius, const Mat4& mvp, f32 alphaCentre);

    /// Bilinear / nearest texture fetch. u,v are wrapped into [0,1).
    static Rgba SampleTexture(const Image& tex, f32 u, f32 v, bool bilinear);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// One-shot headless render, used by `--shot`.
/// Returns an image of exactly width x height.
Result RenderSceneToImage(const Scene& scene, const Camera& camera,
                          const RenderOptions& opt,
                          int width, int height, Image& out);

} // namespace sro
