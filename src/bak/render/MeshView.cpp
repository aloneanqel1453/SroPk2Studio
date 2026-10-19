// ============================================================================
//  SRO Asset Studio - render/MeshView.cpp
//
//  Interactive 3D viewport built on the software Rasterizer, plus the scene
//  builders that turn a parsed .bms / composed .bsr / .bsk / .ban into
//  something drawable.
//
//  Interaction (SPEC.md section 3) - a free ARCBALL (quaternion orientation):
//  drag in ANY direction and the model turns that way; every angle is reachable,
//  with no pole or gimbal lock to fight. Named views build a level basis; free
//  rotation continues from there.
//    left drag            orbit freely (full pane width = 360 deg)
//    Ctrl + left drag     the same at quarter speed for fine framing
//    middle / right drag  pan
//    wheel                dolly
//    arrows               orbit in exact 15 degree steps (Ctrl: 5)
//    Q / E / W            roll left / right / re-level the horizon
//    numpad 1 2 3 4 7 9   front / back / right / left / top / bottom (level)
//    numpad 5, R, Home    reset the view (level, default 3/4 angle)
//    F / double click     fit to the model AABB
//    1..4                 shading mode (textured / flat / wireframe / normals)
//    G / X / B            grid / axes / bounding box toggles
//    Space                play-pause an animation
//
//  Every newly loaded model resets to a clean level 3/4 view, so an angle set on
//  one model never carries over to the next.
//
//  Repainting is lazy: WM_PAINT is the only place that rasterises, and it is
//  only reached after invalidate() or an animation tick, so an idle viewport
//  costs nothing.
//
//  Everything here is bounds-checked against the parsed data - a hostile .bms
//  with face indices past the end of its vertex buffer must be skipped, not
//  trusted.
//
//  Implementation owner: Agent E (render).
// ============================================================================
#include "MeshView.h"
#include "SroPose.h"
#include "../core/I18n.h"   // the on-screen help legend is translated

#include <cstdlib>

#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

namespace sro {

namespace {

const wchar_t* kMeshViewClass = L"SROAssetStudioMeshView";
constexpr UINT kAnimTimerId   = 1;
constexpr UINT kAnimIntervalMs = 16;      // ~60 Hz

/// The reference viewer's SHOWCASE backdrop (main.cpp:4048-4054): mid-grey, DARK
/// at the top and LIGHTER toward the floor, like a horizon. The near-white studio
/// this replaces was tried in the reference and rejected - additive rare/enchant
/// glow is mathematically invisible against white and models wash out - and the
/// old host gradient additionally ran INVERTED (bright sky, dark ground), sinking
/// every model's feet into the darkest band of the frame.
/// const, not constexpr: Rgba's (u8,u8,u8,u8) constructor is not constexpr.
const Rgba kStudioTop   { 107, 112, 122, 0xFF };   // 0.42, 0.44, 0.48
const Rgba kStudioBottom{ 168, 173, 181, 0xFF };   // 0.66, 0.68, 0.71

/// Clip length, ported 1:1 from the reference viewer (Model.h animDuration).
///
/// Most .ban files leave the header duration at ZERO and carry the real length
/// only in the keyframe time table. Trusting the header alone made every such
/// clip fall back to a flat 1000 ms, so a 4-second walk cycle restarted every
/// second - the "her saniye basa sariyor" stutter. Take whichever is longer.
f32 AnimDuration(const BanFile& a) {
    f32 d = static_cast<f32>(a.durationMs);
    if (!a.keyframeTimesMs.empty()) {
        const f32 last = static_cast<f32>(a.keyframeTimesMs.back());
        if (last > d) d = last;
    }
    return d > 0.0f ? d : 1000.0f;
}

/// MeshView::Impl is a private nested type, so the window procedure - a plain
/// free function - cannot name it. It talks to this tiny interface instead;
/// the HWND user data holds an IMeshViewHost*.
struct IMeshViewHost {
    virtual ~IMeshViewHost() = default;
    virtual LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) = 0;
};

/// Silkroad winding is CLOCKWISE in a left-handed system, so the outward face
/// normal of (a,b,c) is cross(b-a, c-a) - verified by hand in Raster.cpp.
inline Vec3 FaceNormal(const Vec3& a, const Vec3& b, const Vec3& c) {
    return (b - a).cross(c - a);
}

/// Some .bms files ship zero or denormal vertex normals. Rebuild them from the
/// geometry so the model is lit instead of flat ambient-dark.
void EnsureNormals(RasterMesh& rm) {
    if (rm.vertices.empty() || rm.indices.size() < 3) return;

    size_t degenerate = 0;
    for (const RasterVertex& v : rm.vertices) {
        if (v.normal.length() < 1e-4f) ++degenerate;
    }
    // Only rebuild when the supplied normals are mostly useless.
    if (degenerate * 2 < rm.vertices.size()) return;

    std::vector<Vec3> acc(rm.vertices.size(), Vec3(0.0f, 0.0f, 0.0f));
    const size_t vcount = rm.vertices.size();
    for (size_t i = 0; i + 2 < rm.indices.size(); i += 3) {
        const u32 ia = rm.indices[i], ib = rm.indices[i + 1], ic = rm.indices[i + 2];
        if (ia >= vcount || ib >= vcount || ic >= vcount) continue;
        const Vec3 n = FaceNormal(rm.vertices[ia].position,
                                  rm.vertices[ib].position,
                                  rm.vertices[ic].position);
        acc[ia] = acc[ia] + n;
        acc[ib] = acc[ib] + n;
        acc[ic] = acc[ic] + n;
    }
    for (size_t i = 0; i < vcount; ++i) {
        const Vec3 n = acc[i].normalized();
        rm.vertices[i].normal = (n.length() > 1e-4f) ? n : Vec3(0.0f, 0.0f, -1.0f);
    }
}

/// .bms -> RasterMesh. Face indices outside the vertex buffer are dropped.
void BuildRasterMesh(const BmsFile& bms, int textureIndex, RasterMesh& rm) {
    rm.vertices.clear();
    rm.indices.clear();
    rm.textureIndex = textureIndex;
    rm.twoSided = false;
    rm.transform = Mat4::Identity();

    // Reference drawOne (Model.h:1445): a mesh with NO texture draws at a neutral
    // grey (0.75, 0.75, 0.78), never at white. Under the studio rig - ambient
    // .88 + key .34 + rim .16, up to 1.38 - a white vertex colour saturates into
    // a featureless blob wherever a material failed to resolve, which reads as a
    // rendering bug rather than a missing texture.
    const Rgba base = (textureIndex >= 0) ? Rgba(255, 255, 255, 255)
                                          : Rgba(191, 191, 199, 255);
    rm.vertices.resize(bms.vertices.size());
    for (size_t i = 0; i < bms.vertices.size(); ++i) {
        rm.vertices[i].position = bms.vertices[i].position;
        rm.vertices[i].normal   = bms.vertices[i].normal;
        rm.vertices[i].uv       = bms.vertices[i].uv0;   // V already top-down
        rm.vertices[i].color    = base;
    }

    const size_t vcount = rm.vertices.size();
    rm.indices.reserve(bms.faces.size() * 3);
    for (const BmsFace& f : bms.faces) {
        if (f.a >= vcount || f.b >= vcount || f.c >= vcount) continue;
        rm.indices.push_back(f.a);
        rm.indices.push_back(f.b);
        rm.indices.push_back(f.c);
    }

    EnsureNormals(rm);
}

/// A small three-axis cross used as a joint marker on the skeleton view.
void AddJointMarker(std::vector<Vec3>& pts, const Vec3& p, f32 s) {
    pts.push_back(Vec3(p.x - s, p.y, p.z)); pts.push_back(Vec3(p.x + s, p.y, p.z));
    pts.push_back(Vec3(p.x, p.y - s, p.z)); pts.push_back(Vec3(p.x, p.y + s, p.z));
    pts.push_back(Vec3(p.x, p.y, p.z - s)); pts.push_back(Vec3(p.x, p.y, p.z + s));
}

/// Local bind transform of a bone (row-vector convention: v * R * T).
inline Mat4 BoneLocalBind(const BskBone& b) {
    return Mat4::FromQuat(b.rotationToParent) * Mat4::Translation(b.translationToParent);
}

inline Vec3 MatrixTranslation(const Mat4& m) {
    return Vec3(m.m[12], m.m[13], m.m[14]);
}

/// Inverse of a rotation+translation matrix: transpose the rotation block and
/// re-project the translation. Exact for rigid transforms and far cheaper than
/// a general 4x4 invert - and every bone matrix here is rigid.
inline Mat4 RigidInverse(const Mat4& M) {
    Mat4 r;                                   // identity
    r.m[0] = M.m[0];  r.m[1] = M.m[4];  r.m[2]  = M.m[8];
    r.m[4] = M.m[1];  r.m[5] = M.m[5];  r.m[6]  = M.m[9];
    r.m[8] = M.m[2];  r.m[9] = M.m[6];  r.m[10] = M.m[10];
    const Vec3 ti = r.transformVector(Vec3(M.m[12], M.m[13], M.m[14]));
    r.m[12] = -ti.x; r.m[13] = -ti.y; r.m[14] = -ti.z;
    return r;
}

/// True when a material/mesh name ends in "_al" + optional digits (SRO's
/// convention for a clean alpha cut-out layer). Matches SHARE's /_al\d*$/i.
bool NameForcesCutout(const std::wstring& name) {
    size_t end = name.size();
    while (end > 0 && iswdigit(name[end - 1])) --end;
    if (end < 3) return false;
    return (name[end - 3] == L'_') &&
           (name[end - 2] == L'a' || name[end - 2] == L'A') &&
           (name[end - 1] == L'l' || name[end - 1] == L'L');
}

std::wstring LowerPath(const std::wstring& s) {
    std::wstring o = s;
    for (wchar_t& c : o) c = static_cast<wchar_t>(towlower(c));
    for (wchar_t& c : o) if (c == L'/') c = L'\\';
    return o;
}

/// Fraction of texels whose alpha is a real MID value. The reference uses this
/// to tell a smooth transparency gradient (cape, ghost robe, glass) from a
/// binary cut-out or a metallic sheen mask.
f32 MidAlphaFraction(const Image& img) {
    if (!img.valid()) return 0.0f;
    const long total = static_cast<long>(img.width()) * img.height();
    if (total <= 0) return 0.0f;
    long mid = 0;
    const u8* p = img.data();
    for (long i = 0; i < total; ++i) {
        const int a = p[static_cast<size_t>(i) * 4 + 3];
        if (a > 12 && a < 244) ++mid;
    }
    return static_cast<f32>(mid) / static_cast<f32>(total);
}

/// Fraction of texels that are FULLY transparent. A genuinely see-through
/// material (cape, wing card, foliage) almost always has some - that is its
/// silhouette. A metallic gloss mask has none: it uses the whole alpha range as
/// a shine map over a solid surface.
f32 ZeroAlphaFraction(const Image& img) {
    if (!img.valid()) return 0.0f;
    const long total = static_cast<long>(img.width()) * img.height();
    if (total <= 0) return 0.0f;
    long zero = 0;
    const u8* p = img.data();
    for (long i = 0; i < total; ++i)
        if (p[static_cast<size_t>(i) * 4 + 3] <= 12) ++zero;
    return static_cast<f32>(zero) / static_cast<f32>(total);
}

/// Mean RGB, 0..255. Used to confirm an all-black diffuse.
/// How coloured a texture is: the spread between its mean channels, 0..255.
/// A greyscale texture sits near 0; anything with its own hue is well above.
f32 ChromaRgb(const Image& img) {
    if (img.width() <= 0 || img.height() <= 0) return 0.0f;
    u64 rs = 0, gs = 0, bs = 0, n = 0;
    const int stepY = (std::max)(1, img.height() / 64);
    const int stepX = (std::max)(1, img.width() / 64);
    for (int y = 0; y < img.height(); y += stepY) {
        const u8* row = img.row(y);
        for (int x = 0; x < img.width(); x += stepX) {
            const u8* p = row + static_cast<size_t>(x) * 4;
            if (p[3] < 16) continue;              // ignore fully transparent texels
            rs += p[0]; gs += p[1]; bs += p[2]; ++n;
        }
    }
    if (!n) return 0.0f;
    const f32 r = static_cast<f32>(rs / n), g = static_cast<f32>(gs / n),
              b = static_cast<f32>(bs / n);
    return (std::max)(r, (std::max)(g, b)) - (std::min)(r, (std::min)(g, b));
}

f32 MeanRgb(const Image& img) {
    if (!img.valid()) return 255.0f;
    const long total = static_cast<long>(img.width()) * img.height();
    if (total <= 0) return 255.0f;
    double sum = 0;
    const u8* p = img.data();
    for (long i = 0; i < total; ++i) {
        const size_t o = static_cast<size_t>(i) * 4;
        sum += p[o] + p[o + 1] + p[o + 2];
    }
    return static_cast<f32>(sum / (total * 3.0));
}

/// Character body armour reuses the alpha channel for a metallic SHEEN mask,
/// never for transparency, so it must stay solid. Same for creature bodies
/// under prim\mtrl\cos\ - blending those turned every PET2 into a ghost.
bool IsSheenMaskPath(const std::wstring& lowerPath) {
    return lowerPath.find(L"man_item")   != std::wstring::npos ||
           lowerPath.find(L"woman_item") != std::wstring::npos;
}
f32 Sat01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

bool IsCreatureBodyPath(const std::wstring& lowerPath) {
    return lowerPath.find(L"\\cos\\") != std::wstring::npos;
}

// --- rich info builders (for the "Dosya Bilgisi" panel; modder-oriented) ----
std::wstring FormatAabbTr(const Aabb& b) {
    if (!b.valid()) return T(L"Sinirlar: (yok)");
    const f32 w = b.max.x - b.min.x, h = b.max.y - b.min.y, d = b.max.z - b.min.z;
    return FormatW(T(L"Sinirlar (GxYxD): %.1f x %.1f x %.1f birim\n"
                   L"Merkez: (%.1f, %.1f, %.1f)"),
                   w, h, d,
                   (b.min.x + b.max.x) * 0.5f,
                   (b.min.y + b.max.y) * 0.5f,
                   (b.min.z + b.max.z) * 0.5f);
}

std::wstring BuildModelDetails(const ComposedModel& model, u64 verts, u64 faces,
                               u64 parts, u64 textures, const Aabb& bounds) {
    std::wstring t;
    t += T(L"═ Model (.bsr) ═\n");
    t += T(L"Toplam: ") + FormatCountTr(verts) + T(L" vertex, ") +
         FormatCountTr(faces) + T(L" ucgen\n");
    t += FormatCountTr(parts) + T(L" parca, ") + FormatCountTr(textures) + T(L" doku\n");
    t += FormatAabbTr(bounds) + L"\n";

    if (!model.texturePaths.empty()) {
        t += T(L"\nDokular:\n");
        const size_t cap = 24;
        for (size_t i = 0; i < model.texturePaths.size() && i < cap; ++i) {
            t += L"  • " + PathFileName(model.texturePaths[i]);
            if (i < model.textures.size() && model.textures[i].valid())
                t += FormatW(L"  (%dx%d)", model.textures[i].width(),
                                            model.textures[i].height());
            t += L"\n";
        }
        if (model.texturePaths.size() > cap)
            t += FormatW(T(L"  ... +%zu doku daha\n"), model.texturePaths.size() - cap);
    }

    t += T(L"\nParcalar (mesh / materyal / doku):\n");
    const size_t pcap = 40;
    for (size_t i = 0; i < model.parts.size() && i < pcap; ++i) {
        const ComposedPart& p = model.parts[i];
        t += L"  • " + PathFileName(p.meshPath);
        t += FormatW(T(L"  [%s v, %s u]"),
                     FormatCountTr(p.mesh.vertices.size()).c_str(),
                     FormatCountTr(p.mesh.faces.size()).c_str());
        if (!p.materialName.empty())      t += T(L"  mtrl:") + p.materialName;
        if (!p.diffuseTexturePath.empty())t += L"  → " + PathFileName(p.diffuseTexturePath);
        t += L"\n";
    }
    if (model.parts.size() > pcap)
        t += FormatW(T(L"  ... +%zu parca daha\n"), model.parts.size() - pcap);

    if (!model.warningsTr.empty()) {
        t += T(L"\nUyarilar:\n");
        for (size_t i = 0; i < model.warningsTr.size() && i < 12; ++i)
            t += L"  ! " + model.warningsTr[i] + L"\n";
    }
    return t;
}

std::wstring BuildMeshDetails(const BmsFile& mesh, const Aabb& bounds) {
    std::wstring t;
    t += L"═ Mesh (.bms " + mesh.version + T(L") ═\n");
    if (!mesh.name.empty()) t += T(L"Ad: ") + mesh.name + L"\n";
    t += T(L"Vertex: ") + FormatCountTr(mesh.vertices.size()) +
         T(L"   Ucgen: ") + FormatCountTr(mesh.faces.size()) + L"\n";
    if (!mesh.material.empty()) t += T(L"Materyal: ") + mesh.material + L"\n";
    if (mesh.hasLightmapUV) {
        t += T(L"Lightmap UV: var");
        if (!mesh.lightmapPath.empty()) t += L" (" + PathFileName(mesh.lightmapPath) + L")";
        t += L"\n";
    }
    if (mesh.isSkinned())
        t += T(L"Iskelet: ") + FormatCountTr(mesh.boneNames.size()) + T(L" kemik (skinned)\n");
    if (mesh.hasMorphData) t += T(L"Morph verisi: var\n");
    if (mesh.clothVertexCount)
        t += T(L"Kumas: ") + FormatCountTr(mesh.clothVertexCount) + T(L" vertex, ") +
             FormatCountTr(mesh.clothEdgeCount) + T(L" kenar\n");
    if (mesh.navCellCount)
        t += T(L"NavMesh: ") + FormatCountTr(mesh.navVertexCount) + T(L" vertex, ") +
             FormatCountTr(mesh.navCellCount) + T(L" hucre\n");
    if (mesh.hasOcclusionPortal)
        t += T(L"Occlusion portal: ") + mesh.occlusionPortalName + L"\n";
    t += FormatAabbTr(bounds) + L"\n";
    return t;
}

std::wstring BuildSkeletonDetails(const BskFile& sk) {
    std::wstring t;
    t += T(L"═ Iskelet (.bsk) ═\n");
    t += T(L"Kemik: ") + FormatCountTr(sk.bones.size()) + L"\n";
    t += T(L"Kok kemik: ") + FormatCountTr(sk.roots().size()) + L"\n";
    t += T(L"\nKemikler (ad ← ebeveyn):\n");
    const size_t cap = 48;
    for (size_t i = 0; i < sk.bones.size() && i < cap; ++i) {
        t += L"  • " + sk.bones[i].name;
        if (!sk.bones[i].parentName.empty()) t += L"  ← " + sk.bones[i].parentName;
        t += L"\n";
    }
    if (sk.bones.size() > cap)
        t += FormatW(T(L"  ... +%zu kemik daha\n"), sk.bones.size() - cap);
    return t;
}

} // namespace

// ===========================================================================
// Impl
// ===========================================================================
struct MeshView::Impl : IMeshViewHost {
    HWND   hwnd   = nullptr;
    HWND   parent = nullptr;
    int    ctrlId = 0;

    Scene           scene;
    Camera          camera;
    RenderOptions   options;
    Rasterizer      raster;
    Image           presentBuf;    ///< supersampled frame downscaled for display
    MeshViewContent content = MeshViewContent::Empty;

    std::wstring titleTr;
    std::wstring detailsTr;         ///< rich multi-line breakdown for the info pane

    // --- statistics for statusTr() ----------------------------------------
    u64 vertexCount  = 0;
    u64 faceCount    = 0;
    u64 textureCount = 0;
    u64 partCount    = 0;
    u64 boneCount    = 0;

    // --- animation ---------------------------------------------------------
    BskFile skeleton;
    BanFile animation;
    bool    hasSkeleton = false;
    bool    hasAnimation = false;

    // Skinned playback: the BIND-POSE model is kept so every frame can be
    // re-skinned from the original vertices (skinning the already-skinned
    // result would drift). `meshPartIndex` maps scene.meshes[i] back to the
    // part it came from, because empty parts are skipped when building.
    ComposedModel       srcModel;
    bool                hasSrcModel = false;
    std::vector<size_t> meshPartIndex;

    /// Skinning is delegated to the 1:1 port of the reference viewer's pose
    /// pipeline (render/SroPose.h) so no convention translation happens here.
    sropose::Poser                 poser;
    std::vector<sropose::PoseMesh> poseMeshes;   ///< parallel to scene.meshes
    bool                           loopAnim = true;

    void buildSkinCache();
    bool applySkinToScene();
    bool    playing    = false;
    f32     timeMs     = 0.0f;
    f32     durationMs = 0.0f;
    DWORD   lastTick   = 0;

    // --- mouse -------------------------------------------------------------
    bool  orbiting = false;
    bool  panning  = false;
    POINT lastMouse{ 0, 0 };

    // On-screen control legend, drawn in the top-right corner. H toggles it.
    bool  showHelp = true;
    void  drawOverlay(HDC dc, const RECT& rc);

    // Owner callback + the last time we fired it, so the parent's status text
    // refreshes a few times a second instead of 60 times.
    std::function<void()> onAnimTick;
    f32                   lastTickNotify = -1.0e9f;

    Impl() {
        // Soft light studio gradient (not glaring pure white - easier on the eyes)
        // with no grid/axes, so only the model shows on a clean backdrop.
        options.gradientBackground = true;
        options.background       = kStudioTop;      // reference showcase: dark top
        options.backgroundBottom = kStudioBottom;   // -> lighter toward the floor
        options.drawGrid = false;
        options.drawAxes = false;
    }

    /// Supersample factor for crisp anti-aliased output. Higher = more samples
    /// per pixel = cleaner edges/textures, at more fill cost. Adaptive so a small
    /// window gets maximum quality while a huge one stays responsive.
    /// While a clip RUNS the budget is per-frame, not per-still: 3x over a real
    /// 1884x564 preview pane means 5652x1692 = 9.6 M samples, measured at about
    /// 115 ms with this software rasteriser - seven times over a single 16 ms
    /// tick, so the clip could only advance once per paint and the pane visibly
    /// rebuilt itself in stages. Motion hides aliasing; a still does not, so the
    /// STOPPED ladder is left untouched and a repaint is forced on every stop.
    int supersampleFor(int w, int h) const {
        const long long px = static_cast<long long>(w) * h;
        if (px <= 0)             return 1;
        if (playing)             return (px <= 400'000LL) ? 2 : 1;
        if (px <= 1'600'000LL)   return 3;   // up to ~1600x1000: 3x (9 samples/px)
        if (px <= 5'000'000LL)   return 2;   // up to ~2560x1900: 2x
        return 1;
    }

    /// Frame the scene for the CURRENT window shape. A wide pane and a tall one
    /// need different distances for the same model, which the aspect-blind fit
    /// could not know.
    void fitNow() {
        f32 aspect = 0.0f;
        if (hwnd && ::IsWindow(hwnd)) {
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            const long w = rc.right - rc.left, h = rc.bottom - rc.top;
            if (w > 0 && h > 0) aspect = static_cast<f32>(w) / static_cast<f32>(h);
        }
        if (aspect > 0.05f) camera.fitToBoundsAspect(scene.bounds, aspect);
        else                camera.fitToBounds(scene.bounds);
    }

    /// Full view reset: default level 3/4 view, no pan, re-framed.
    void resetView() {
        // Reference main.cpp:566 - yaw 215 / pitch 16, a 3/4 FRONT view from
        // above (host yaw = reference yaw - 180, see Camera::Camera). The old
        // dead-front 0 / 0.05 threw the depth axis away: p_bear_a.bsr is 40
        // units deep against 18 wide and read as a flat sticker.
        camera.setView(-0.6108652f, 0.2792527f);
        camera.panOffset = Vec3(0.0f, 0.0f, 0.0f);
        fitNow();
    }

    /// Manually pitch the MODEL 90 degrees about X and re-frame - a one-key
    /// stand-up for any model the auto-upright could not classify (e.g. a pack
    /// pet authored head-down). Four presses return to the original.
    void rotateModel90() {
        const Mat4 R = Mat4::RotationX(1.57079633f);
        for (RasterMesh& m : scene.meshes) m.transform = R * m.transform;
        for (RasterLines& l : scene.lines)  l.transform = R * l.transform;
        scene.computeBounds();
        resetView();
    }

    /// Snap to an exact axis view and re-frame it, so "show me the front" is one
    /// key instead of a hunt with the mouse.
    void axisView(f32 newYaw, f32 newPitch) {
        camera.setView(newYaw, newPitch);
        camera.panOffset = Vec3(0.0f, 0.0f, 0.0f);
        fitNow();
    }

    void resetContent() {
        scene.clear();
        content      = MeshViewContent::Empty;
        vertexCount  = 0;
        faceCount    = 0;
        textureCount = 0;
        partCount    = 0;
        boneCount    = 0;
        hasSkeleton  = false;
        hasAnimation = false;
        playing      = false;
        timeMs       = 0.0f;
        durationMs   = 0.0f;
        skeleton     = BskFile();
        animation    = BanFile();
        titleTr.clear();
    }

    void repaint() { if (hwnd) ::InvalidateRect(hwnd, nullptr, FALSE); }

    /// World-space bone positions for the current pose.
    void computeBonePositions(std::vector<Vec3>& out) const;

    /// Rebuild scene.lines from the skeleton at the current time.
    void rebuildSkeletonLines();

    void syncTimer() {
        if (!hwnd) return;
        if (playing && durationMs > 0.0f) ::SetTimer(hwnd, kAnimTimerId, kAnimIntervalMs, nullptr);
        else                              ::KillTimer(hwnd, kAnimTimerId);
    }

    void onPaint();
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) override;
};

// Build the pose state: hand the skeleton and every skinned part to the ported
// reference pipeline. Called once per model, never per frame.
void MeshView::Impl::buildSkinCache() {
    poseMeshes.clear();
    if (!hasSrcModel || skeleton.bones.empty()) return;

    poser.setup(skeleton);
    poseMeshes.resize(scene.meshes.size());
    bool any = false;
    for (size_t mi = 0; mi < scene.meshes.size() && mi < meshPartIndex.size(); ++mi) {
        const size_t pi = meshPartIndex[mi];
        if (pi >= srcModel.parts.size()) continue;
        const ComposedPart& part = srcModel.parts[pi];
        // A piece already MOVED out of its own bind space (an avatar wing seated
        // on the spine) is skinned to its own skeleton - leave it rigid.
        if (part.rigidPlaced) continue;
        if (part.mesh.vertices.empty()) continue;
        if (scene.meshes[mi].vertices.size() != part.mesh.vertices.size()) continue;
        poser.mapMesh(part.mesh, poseMeshes[mi]);
        if (part.mesh.isSkinned()) any = true;
    }
    if (!any) poseMeshes.clear();
}

// Pose for the current time and copy the result into the scene meshes.
bool MeshView::Impl::applySkinToScene() {
    if (poseMeshes.empty() || !poser.ready()) return false;
    // SRO_BINDONLY=1 runs the skin pass with NO clip: the result must be
    // pixel-identical to the unskinned bind pose. If it is not, the fault is in
    // the bind/bone-map, not in the animation sampling.
    const bool bindOnly = (::GetEnvironmentVariableW(L"SRO_BINDONLY", nullptr, 0) != 0);
    poser.applyPose((hasAnimation && !bindOnly) ? &animation : nullptr, timeMs, poseMeshes);

    bool any = false;
    for (size_t mi = 0; mi < poseMeshes.size() && mi < scene.meshes.size(); ++mi) {
        const sropose::PoseMesh& pm = poseMeshes[mi];
        if (!pm.src || pm.skinnedPos.empty()) continue;
        RasterMesh& rm = scene.meshes[mi];
        if (rm.vertices.size() != pm.skinnedPos.size()) continue;
        for (size_t v = 0; v < pm.skinnedPos.size(); ++v) {
            rm.vertices[v].position = pm.skinnedPos[v];
            rm.vertices[v].normal   = pm.skinnedNrm[v];
        }
        any = true;
    }
    return any;
}

void MeshView::Impl::computeBonePositions(std::vector<Vec3>& out) const {
    const size_t n = skeleton.bones.size();
    out.assign(n, Vec3(0.0f, 0.0f, 0.0f));
    if (n == 0) return;

    // Bind pose: the file already stores every bone relative to the origin, so
    // no hierarchy walk is needed and a broken parent chain cannot break it.
    if (!hasAnimation) {
        for (size_t i = 0; i < n; ++i) out[i] = skeleton.bones[i].translationToOrigin;
        return;
    }

    // Animated pose: sample the .ban and accumulate down the hierarchy.
    std::vector<Quat> rot;
    std::vector<Vec3> tr;
    const bool sampled = SampleBan(animation, timeMs, rot, tr) &&
                         rot.size() == animation.bones.size() &&
                         tr.size()  == animation.bones.size();
    if (!sampled) {
        for (size_t i = 0; i < n; ++i) out[i] = skeleton.bones[i].translationToOrigin;
        return;
    }

    // Map skeleton bone -> animation bone once.
    std::vector<int> animIndex(n, -1);
    for (size_t i = 0; i < animation.bones.size(); ++i) {
        const int b = skeleton.findBone(animation.bones[i].name);
        if (b >= 0 && static_cast<size_t>(b) < n) animIndex[static_cast<size_t>(b)] = static_cast<int>(i);
    }

    std::vector<Mat4> world(n);
    std::vector<bool> done(n, false);

    // Iterative depth-first walk from the roots; a cyclic/garbage parent chain
    // simply leaves the unreached bones at their bind position.
    std::vector<int> stack = skeleton.roots();
    while (!stack.empty()) {
        const int bi = stack.back();
        stack.pop_back();
        if (bi < 0 || static_cast<size_t>(bi) >= n) continue;
        const size_t i = static_cast<size_t>(bi);
        if (done[i]) continue;                       // guards against cycles
        done[i] = true;

        const BskBone& bone = skeleton.bones[i];
        Mat4 local;
        const int ai = animIndex[i];
        if (ai >= 0 && static_cast<size_t>(ai) < rot.size())
            local = Mat4::FromQuat(rot[static_cast<size_t>(ai)]) *
                    Mat4::Translation(tr[static_cast<size_t>(ai)]);
        else
            local = BoneLocalBind(bone);

        const int p = bone.parentIndex;
        world[i] = (p >= 0 && static_cast<size_t>(p) < n && done[static_cast<size_t>(p)])
                 ? (local * world[static_cast<size_t>(p)])
                 : local;

        out[i] = MatrixTranslation(world[i]);

        for (int c : bone.childIndices)
            if (c >= 0 && static_cast<size_t>(c) < n && !done[static_cast<size_t>(c)])
                stack.push_back(c);
    }

    for (size_t i = 0; i < n; ++i)
        if (!done[i]) out[i] = skeleton.bones[i].translationToOrigin;
}

void MeshView::Impl::rebuildSkeletonLines() {
    scene.lines.clear();
    if (skeleton.bones.empty()) return;

    std::vector<Vec3> pos;
    computeBonePositions(pos);

    Aabb b;
    for (const Vec3& p : pos) b.add(p);
    f32 markerSize = b.valid() ? (b.radius() * 0.02f) : 1.0f;
    if (!(markerSize > 1e-5f)) markerSize = 0.5f;

    RasterLines bones;
    bones.color = Rgba(0xFF, 0xCC, 0x33, 0xFF);     // Theme GOLD_BRIGHT
    RasterLines joints;
    joints.color = Rgba(0x44, 0x88, 0xCC, 0xFF);    // Theme BLUE

    for (size_t i = 0; i < skeleton.bones.size(); ++i) {
        const int p = skeleton.bones[i].parentIndex;
        if (p >= 0 && static_cast<size_t>(p) < pos.size()) {
            bones.points.push_back(pos[static_cast<size_t>(p)]);
            bones.points.push_back(pos[i]);
        }
        AddJointMarker(joints.points, pos[i], markerSize);
    }

    if (!bones.points.empty())  scene.lines.push_back(bones);
    if (!joints.points.empty()) scene.lines.push_back(joints);
    scene.computeBounds();
}

// ===========================================================================
// Window procedure
// ===========================================================================
namespace {

LRESULT CALLBACK MeshViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    IMeshViewHost* self =
        reinterpret_cast<IMeshViewHost*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);
    return self->handle(msg, wp, lp);
}

bool EnsureMeshViewClass() {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS | CS_OWNDC;
    wc.lpfnWndProc   = MeshViewProc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;             // we always paint every pixel
    wc.lpszClassName = kMeshViewClass;

    if (!::RegisterClassExW(&wc)) {
        if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }
    registered = true;
    return true;
}

} // namespace

void MeshView::Impl::drawOverlay(HDC dc, const RECT& rc) {
    if (!showHelp) return;
    if (content == MeshViewContent::Empty) return;

    // Split into KEY + DESCRIPTION rather than one pre-padded line: the key
    // column is never translated (it names physical keys), the description is,
    // and the column is re-aligned here at draw time. Pre-padded strings would
    // lose their alignment the moment a translation changed length.
    struct HelpRow { const wchar_t* key; const wchar_t* descTr; };
    static const HelpRow kRows[] = {
        { L"Sol sürükle",  L"döndür (yatay 360°, dikey ±89°)" },
        { L"Ctrl+sürükle", L"yavaş / ince ayar" },
        { L"Orta·Sağ sür", L"kaydır (pan)" },
        { L"Tekerlek",     L"yakınlaş / uzaklaş" },
        { L"Çift tık · R", L"sıfırla" },
        { L"Numpad 1/2",   L"ön / arka" },
        { L"Numpad 3/4",   L"sağ / sol" },
        { L"Numpad 7/9",   L"üst / alt" },
        { L"Q · E · W",    L"yatır · yatır · düzelt" },
        { L"1 2 3 4",      L"doku·düz·tel·normal" },
        { L"F  Boşluk",    L"sığdır · animasyon" },
        { L"P",            L"modeli 90° çevir (yatıksa)" },
        { L"O",            L"serbest yörünge (arcball)" },
        { L"H",            L"bu listeyi gizle" },
    };
    const int n = (int)(sizeof(kRows) / sizeof(kRows[0]));

    std::vector<std::wstring> lines;
    lines.reserve(n);
    {
        // The key column is translated too: rows like "Sol surukle" (left drag)
        // or "Tekerlek" (wheel) name an ACTION, not a physical key, so leaving
        // them in Turkish would strand half of every line. Rows that really are
        // key names ("Numpad 1/2", "Q · E · W", "P") have no table entry and
        // fall through to themselves.
        std::vector<std::wstring> keys;
        keys.reserve(n);
        size_t keyW = 0;
        for (const HelpRow& r : kRows) {
            keys.push_back(T(r.key));
            keyW = (std::max)(keyW, keys.back().size());
        }
        for (int i = 0; i < n; ++i) {
            std::wstring s = keys[static_cast<size_t>(i)];
            s.append(keyW + 2 - s.size(), L' ');
            s += T(kRows[i].descTr);
            lines.push_back(s);
        }
    }

    // Build once per LANGUAGE, not per WM_PAINT (this view repaints at the
    // animation tick rate). Consolas is Latin-only, so a CJK or Thai legend
    // would be drawn by whatever GDI font linking substitutes - which is what
    // clipped the "wheel" row. Ask for that language's own face instead.
    static HFONT s_font = nullptr;
    static Lang  s_fontLang = static_cast<Lang>(-1);
    if (!s_font || s_fontLang != CurrentLang()) {
        if (s_font) ::DeleteObject(s_font);
        const wchar_t* face = LangUiFace(CurrentLang());
        if (::wcscmp(face, L"Segoe UI") == 0) face = L"Consolas";   // Latin: keep the mono look
        s_font = ::CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, FF_DONTCARE, face);
        s_fontLang = CurrentLang();
    }
    HGDIOBJ oldFont = ::SelectObject(dc, s_font ? s_font : (HFONT)::GetStockObject(DEFAULT_GUI_FONT));
    ::SetBkMode(dc, TRANSPARENT);

    // Measure the widest line so the panel hugs the text.
    int wMax = 0, lineH = 0;
    for (int i = 0; i < n; ++i) {
        SIZE s{};
        ::GetTextExtentPoint32W(dc, lines[i].c_str(), (int)lines[i].size(), &s);
        if (s.cx > wMax) wMax = s.cx;
        if (s.cy > lineH) lineH = s.cy;
    }
    const int pad = 8, gap = 2;
    const int boxW = wMax + pad * 2;
    const int boxH = n * (lineH + gap) - gap + pad * 2;
    const int right = rc.right - 8, top = rc.top + 8;
    RECT box{ right - boxW, top, right, top + boxH };

    // Translucent dark panel so the text reads on any background.
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 170, 0 };
    HDC memDc = ::CreateCompatibleDC(dc);
    if (memDc) {
        HBITMAP bmp = ::CreateCompatibleBitmap(dc, boxW, boxH);
        HGDIOBJ ob = ::SelectObject(memDc, bmp);
        RECT fill{ 0, 0, boxW, boxH };
        HBRUSH br = ::CreateSolidBrush(RGB(20, 16, 10));
        if (br) { ::FillRect(memDc, &fill, br); ::DeleteObject(br); }
        ::AlphaBlend(dc, box.left, box.top, boxW, boxH, memDc, 0, 0, boxW, boxH, bf);
        ::SelectObject(memDc, ob);
        ::DeleteObject(bmp);
        ::DeleteDC(memDc);
    }
    // Gold border to match the app theme.
    HPEN pen = ::CreatePen(PS_SOLID, 1, RGB(150, 120, 50));
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBr  = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(dc, box.left, box.top, box.right, box.bottom);
    ::SelectObject(dc, oldPen); ::SelectObject(dc, oldBr);
    if (pen) ::DeleteObject(pen);

    // Text: light with a dark shadow so it stays legible.
    int y = box.top + pad;
    for (int i = 0; i < n; ++i) {
        const int x = box.left + pad;
        ::SetTextColor(dc, RGB(0, 0, 0));
        ::TextOutW(dc, x + 1, y + 1, lines[i].c_str(), (int)lines[i].size());
        ::SetTextColor(dc, RGB(240, 224, 180));
        ::TextOutW(dc, x, y, lines[i].c_str(), (int)lines[i].size());
        y += lineH + gap;
    }

    ::SelectObject(dc, oldFont);   // s_font is cached for the process lifetime
}

void MeshView::Impl::onPaint() {
    PAINTSTRUCT ps{};
    HDC wdc = ::BeginPaint(hwnd, &ps);
    if (!wdc) return;

    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;

    // Compose the WHOLE frame off-screen, then blit it once.
    //
    // This used to paint straight onto the window DC: the full-pane blit below
    // wiped the help legend and drawOverlay() drew it back milliseconds later.
    // At the few frames per second a software rasteriser sustains on a large
    // pane, that reads as the legend blinking away and returning about once a
    // second, and as the viewport rebuilding itself in stages. Every other pane
    // in this program already buffers; the 3D view was the only one that did not.
    HDC     mem    = nullptr;
    HBITMAP bmp    = nullptr;
    HGDIOBJ oldBmp = nullptr;
    HDC     dc     = wdc;
    if (cw > 0 && ch > 0) {
        mem = ::CreateCompatibleDC(wdc);
        if (mem) {
            bmp = ::CreateCompatibleBitmap(wdc, cw, ch);
            if (bmp) { oldBmp = ::SelectObject(mem, bmp); dc = mem; }
            else     { ::DeleteDC(mem); mem = nullptr; }
        }
    }

    if (cw > 0 && ch > 0) {
        const int ss = supersampleFor(cw, ch);
        const int rw = cw * ss, rh = ch * ss;
        if (raster.width() != rw || raster.height() != rh) raster.resize(rw, rh);
        if (raster.width() == rw && raster.height() == rh) {
            raster.render(scene, camera, options);
            // Area-average downscale = anti-aliased, crisp edges & textures.
            // At ss == 1 - which is the ANIMATION path - work straight on the
            // rasteriser's own buffer. Copying it into presentBuf first cost a
            // full-frame allocate + copy every single frame (3.9 MB at
            // 1400x700) for nothing: the buffer is fully overwritten by the
            // next render anyway, so mutating it in place is safe.
            if (ss > 1) {
                presentBuf = raster.colorBuffer().scaledBox(cw, ch);
                presentBuf.applyStudioPost(1.32f, 1.06f, 0.26f);
                presentBuf.blitTo(dc, rc, ChannelView::RGB);
            } else {
                Image& frame = raster.colorBuffer();
                frame.applyStudioPost(1.32f, 1.06f, 0.26f);
                frame.blitTo(dc, rc, ChannelView::RGB);
            }
        } else {
            HBRUSH br = ::CreateSolidBrush(RGB(options.background.r,
                                               options.background.g,
                                               options.background.b));
            if (br) { ::FillRect(dc, &rc, br); ::DeleteObject(br); }
        }
        drawOverlay(dc, rc);
    }

    if (mem) {
        ::BitBlt(wdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
        ::SelectObject(mem, oldBmp);
        ::DeleteObject(bmp);
        ::DeleteDC(mem);
    }
    ::EndPaint(hwnd, &ps);
}

LRESULT MeshView::Impl::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            onPaint();
            return 0;

        case WM_SIZE:
            repaint();
            return 0;

        case WM_LBUTTONDOWN:
            ::SetFocus(hwnd);
            orbiting = true;
            lastMouse.x = GET_X_LPARAM(lp);
            lastMouse.y = GET_Y_LPARAM(lp);
            ::SetCapture(hwnd);
            return 0;

        case WM_MBUTTONDOWN:
        case WM_RBUTTONDOWN:
            ::SetFocus(hwnd);
            panning = true;
            lastMouse.x = GET_X_LPARAM(lp);
            lastMouse.y = GET_Y_LPARAM(lp);
            ::SetCapture(hwnd);
            return 0;

        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
        case WM_RBUTTONUP:
            orbiting = false;
            panning  = false;
            if (::GetCapture() == hwnd) ::ReleaseCapture();
            return 0;

        case WM_CAPTURECHANGED:
            orbiting = false;
            panning  = false;
            return 0;

        case WM_LBUTTONDBLCLK:
            // Double-click = full reset to the initial view.
            resetView();
            repaint();
            return 0;

        case WM_MOUSEMOVE: {
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            const int dx = x - lastMouse.x;
            const int dy = y - lastMouse.y;
            lastMouse.x = x;
            lastMouse.y = y;
            if (orbiting && (dx || dy)) {
                // Reference rate: 0.4 DEGREES per pixel, the SAME on both axes and
                // independent of pane size. The old 2*pi/paneW against pi/paneH
                // worked out to 0.19 deg/px horizontally versus 0.32 vertically on
                // a real 1884x564 pane - 1.67x apart - so a 45 degree diagonal drag
                // ARCED instead of tracking the mouse, and both rates changed on
                // every resize. Ctrl keeps the quarter-speed fine mode.
                constexpr f32 kOrbitRadPerPx = 0.0069813170f;   // 0.4 degrees
                f32 s = kOrbitRadPerPx;
                if (::GetKeyState(VK_CONTROL) < 0) s *= 0.25f;
                // Grab-the-model feel: drag right -> model turns right, drag up ->
                // its top tips toward you.
                camera.orbit(static_cast<f32>(-dx) * s, static_cast<f32>(dy) * s);
                repaint();
            } else if (panning && (dx || dy)) {
                RECT rc{};
                ::GetClientRect(hwnd, &rc);
                camera.pan(static_cast<f32>(dx), static_cast<f32>(dy),
                           rc.right - rc.left, rc.bottom - rc.top);
                repaint();
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            const f32 step = (delta > 0) ? (1.0f / 1.15f) : 1.15f;
            int notches = std::abs(delta) / WHEEL_DELTA;
            if (notches < 1) notches = 1;
            if (notches > 8) notches = 8;
            for (int i = 0; i < notches; ++i) camera.dolly(step);
            repaint();
            return 0;
        }

        case WM_KEYDOWN:
            switch (wp) {
                case 'F':      fitNow(); repaint(); return 0;
                case 'R': case VK_HOME:
                    resetView(); repaint(); return 0;

                // Exact stepped rotation - the mouse cannot hit a round angle.
                // Ctrl drops the step from 15 to 5 degrees.
                case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: {
                    const f32 step = (::GetKeyState(VK_CONTROL) < 0) ? 0.0872665f
                                                                     : 0.2617994f;
                    if      (wp == VK_LEFT)  camera.orbit( step, 0.0f);
                    else if (wp == VK_RIGHT) camera.orbit(-step, 0.0f);
                    else if (wp == VK_UP)    camera.orbit(0.0f, -step);
                    else                     camera.orbit(0.0f,  step);
                    repaint();
                    return 0;
                }

                // Straight-on views on the numpad (1..4 are taken by shading).
                // Same layout Blender uses, so it is already in the fingers.
                case VK_NUMPAD1: axisView(0.0f,  0.0f);        repaint(); return 0; // front
                case VK_NUMPAD2: axisView(3.1415927f, 0.0f);   repaint(); return 0; // back
                case VK_NUMPAD3: axisView(1.5707963f, 0.0f);   repaint(); return 0; // right
                case VK_NUMPAD4: axisView(-1.5707963f, 0.0f);  repaint(); return 0; // left
                case VK_NUMPAD7: axisView(0.0f,  1.5533f);     repaint(); return 0; // top
                case VK_NUMPAD9: axisView(0.0f, -1.5533f);     repaint(); return 0; // bottom
                case VK_NUMPAD5: resetView();                  repaint(); return 0;

                // Q/E roll the horizon; W puts it back level.
                case 'Q': camera.roll(-0.1308997f);  repaint(); return 0;
                case 'E': camera.roll( 0.1308997f);  repaint(); return 0;
                case 'W': camera.levelHorizon();     repaint(); return 0;
                // O leaves the level turntable for the old free arcball - the only
                // way past the +/-89 pitch clamp to look straight down the pole.
                case 'O': camera.freeOrbit = !camera.freeOrbit; repaint(); return 0;
                case 'H': showHelp = !showHelp;      repaint(); return 0;
                case 'P': rotateModel90();           repaint(); return 0;
                case '1':      options.shading = ShadingMode::Textured;  repaint(); return 0;
                case '2':      options.shading = ShadingMode::FlatLit;   repaint(); return 0;
                case '3':      options.shading = ShadingMode::Wireframe; repaint(); return 0;
                case '4':      options.shading = ShadingMode::Normals;   repaint(); return 0;
                case 'G':      options.drawGrid   = !options.drawGrid;   repaint(); return 0;
                case 'X':      options.drawAxes   = !options.drawAxes;   repaint(); return 0;
                case 'B':      options.drawBounds = !options.drawBounds; repaint(); return 0;
                case VK_SPACE:
                    if (durationMs > 0.0f) {
                        playing = !playing;
                        lastTick = ::GetTickCount();
                        syncTimer();
                        repaint();                      // the ss ladder just changed
                        if (onAnimTick) onAnimTick();   // "(durdu)" must show at once
                    }
                    return 0;
                default: break;
            }
            break;

        case WM_TIMER:
            if (wp == kAnimTimerId) {
                if (playing && durationMs > 0.0f) {
                    const DWORD now = ::GetTickCount();
                    const DWORD elapsed = (lastTick == 0) ? kAnimIntervalMs : (now - lastTick);
                    lastTick = now;
                    timeMs += static_cast<f32>(elapsed);
                    // A stand clip authored as one-shot would freeze on its last
                    // frame and look exactly like broken skinning, so the model's
                    // auto-picked idle loops regardless of the file's own type.
                    if (loopAnim || animation.typeEnum() == BanType::Cyclic) {
                        while (timeMs > durationMs) timeMs -= durationMs;
                    } else if (timeMs >= durationMs) {
                        timeMs = durationMs;
                        playing = false;
                        syncTimer();
                    }
                    // A skinned model deforms its meshes; a bare .ban redraws
                    // its bone lines.
                    if (!applySkinToScene()) rebuildSkeletonLines();
                    repaint();
                    if (onAnimTick && std::fabs(timeMs - lastTickNotify) > 120.0f) {
                        lastTickNotify = timeMs;
                        onAnimTick();
                    }
                }
                return 0;
            }
            break;

        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;

        case WM_DESTROY:
            ::KillTimer(hwnd, kAnimTimerId);
            hwnd = nullptr;
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ===========================================================================
// MeshView
// ===========================================================================
MeshView::MeshView() : m_impl(new Impl()) {}

MeshView::~MeshView() { destroy(); }

bool MeshView::create(HWND parent, const RECT& rc, int id) {
    if (m_impl->hwnd) return true;
    if (!EnsureMeshViewClass()) return false;

    m_impl->parent = parent;
    m_impl->ctrlId = id;

    const DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    m_impl->hwnd = ::CreateWindowExW(
        0, kMeshViewClass, L"", style,
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        ::GetModuleHandleW(nullptr),
        static_cast<IMeshViewHost*>(m_impl.get()));

    return m_impl->hwnd != nullptr;
}

void MeshView::destroy() {
    if (m_impl && m_impl->hwnd) {
        HWND h = m_impl->hwnd;
        m_impl->hwnd = nullptr;
        ::SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        ::DestroyWindow(h);
    }
}

HWND MeshView::hwnd() const { return m_impl->hwnd; }

void MeshView::setBounds(const RECT& rc) {
    if (!m_impl->hwnd) return;
    ::MoveWindow(m_impl->hwnd, rc.left, rc.top,
                 rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

void MeshView::show(bool visible) {
    if (m_impl->hwnd) ::ShowWindow(m_impl->hwnd, visible ? SW_SHOW : SW_HIDE);
}

void MeshView::clear() {
    m_impl->resetContent();
    m_impl->syncTimer();
    m_impl->repaint();
}

// ---------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------
Result MeshView::setMesh(const BmsFile& mesh, const std::wstring& titleTr) {
    m_impl->resetContent();

    if (mesh.vertices.empty())
        return Result::Fail(Status::BadFormat, T(L"Mesh bos: gosterilecek vertex yok."));

    RasterMesh rm;
    BuildRasterMesh(mesh, -1, rm);
    if (rm.indices.empty())
        return Result::Fail(Status::BadFormat, T(L"Mesh bos: gecerli ucgen yok."));

    m_impl->vertexCount = rm.vertices.size();
    m_impl->faceCount   = rm.indices.size() / 3;
    m_impl->partCount   = 1;
    m_impl->scene.meshes.push_back(std::move(rm));

    // Trust the file's own AABB when it looks sane, otherwise derive it.
    m_impl->scene.computeBounds();
    if (!m_impl->scene.bounds.valid() && mesh.boundingBox.valid())
        m_impl->scene.bounds = mesh.boundingBox;

    m_impl->content = MeshViewContent::Mesh;
    m_impl->options.background       = kStudioTop;      // reference showcase backdrop
    m_impl->options.backgroundBottom = kStudioBottom;
    m_impl->titleTr = titleTr.empty() ? mesh.name : titleTr;
    m_impl->detailsTr = BuildMeshDetails(mesh, m_impl->scene.bounds.valid()
                                               ? m_impl->scene.bounds : mesh.boundingBox);
    m_impl->scene.orientUpright();   // stand up Z-up / lying-authored imports
    m_impl->resetView();   // fresh model -> clean, level default view (not a stuck tilt)
    m_impl->syncTimer();
    m_impl->repaint();
    return Result::Ok();
}

Result MeshView::setModel(const ComposedModel& model) {
    m_impl->resetContent();

    if (model.parts.empty())
        return Result::Fail(Status::BadFormat, T(L"Model bos: hicbir mesh yuklenemedi."));

    m_impl->scene.textures = model.textures;
    m_impl->textureCount   = m_impl->scene.textures.size();

    // Keep the bind-pose source so animation can re-skin it every frame.
    m_impl->srcModel    = model;
    m_impl->hasSrcModel = true;
    m_impl->meshPartIndex.clear();

    for (size_t partIdx = 0; partIdx < model.parts.size(); ++partIdx) {
        const ComposedPart& part = model.parts[partIdx];
        if (part.mesh.vertices.empty()) continue;
        RasterMesh rm;
        int texIndex = part.textureIndex;
        if (texIndex >= 0 && static_cast<size_t>(texIndex) >= m_impl->scene.textures.size())
            texIndex = -1;
        BuildRasterMesh(part.mesh, texIndex, rm);
        if (rm.indices.empty()) continue;

        // Materials flagged as alpha/sheen are drawn double sided: they are the
        // capes, hair cards and leaf planes, and culling them looks broken.
        if (part.hasMaterial && part.material.usesAlpha()) rm.twoSided = true;

        // Alpha cut-out (SHARE ddsAlphaTest + Bit10 gate): only when the material's
        // Bit10 (0x200) says the alpha channel is actually used. "_al" names are a
        // forced clean cut. Otherwise 0 -> fully opaque (no black patches, no erosion).
        rm.alphaTest = 0.0f;
        const bool haveTex = texIndex >= 0 &&
                             static_cast<size_t>(texIndex) < m_impl->scene.textures.size();
        if (part.hasMaterial && part.material.usesAlpha() && haveTex) {
            rm.alphaTest = ComputeTextureAlphaTest(m_impl->scene.textures[texIndex]);
            if (NameForcesCutout(part.materialName)) rm.alphaTest = 0.5f;
        }

        // --- per-material transparency, ported from the reference viewer ----
        if (part.hasMaterial && haveTex) {
            const Image&       tex   = m_impl->scene.textures[texIndex];
            // Use the RESOLVED texture path: the raw .bmt entry is usually just
            // a file name, so a "\cos\" or "man_item" test against it silently
            // never matches - which is exactly what turned every pet body into
            // a translucent ghost the first time this landed.
            std::wstring lpath = LowerPath(part.diffuseTexturePath);
            if (lpath.empty()) lpath = LowerPath(part.material.diffuseMap.path);
            if (lpath.find(L'\\') == std::wstring::npos)
                lpath = LowerPath(part.meshPath);   // last resort: the mesh's own folder
            const BmtMaterial& mat   = part.material;

            // SRO_MATDEBUG=1 prints the decision inputs for every part - the
            // path tests below match on the RESOLVED texture path and a wrong
            // one fails silently, as a ghost body rather than an error.
            if (::GetEnvironmentVariableW(L"SRO_MATDEBUG", nullptr, 0) != 0) {
                ::fwprintf(stderr,
                           L"[mat] %s | a=%d midA=%.3f zeroA=%.3f mean=%.1f | emis=%.2f,%.2f,%.2f "
                           L"dif=%.2f,%.2f,%.2f,%.2f flags=%08X\n",
                           part.materialName.c_str(),
                           part.material.usesAlpha() ? 1 : 0,
                           MidAlphaFraction(tex), ZeroAlphaFraction(tex), MeanRgb(tex),
                           part.material.emissive.r, part.material.emissive.g,
                           part.material.emissive.b,
                           part.material.diffuse.r, part.material.diffuse.g,
                           part.material.diffuse.b, part.material.diffuse.a,
                           part.material.flags);
            }

            // 1) TRUE ALPHA BLEND - a smooth transparency gradient. Capes, ghost
            //    robes and wings live here. Excluded on purpose: character body
            //    armour (man_item/woman_item) and creature bodies (\cos\), whose
            //    alpha is a metallic SHEEN mask - blending those turned every
            //    PET2 body into a translucent ghost.
            //    The gate is deliberately TIGHT. A 2% mid-alpha threshold alone
            //    (which is what the reference uses) only works there because it
            //    first excludes equipment and creature bodies BY PATH; costume
            //    and mob textures outside those paths slipped through here and
            //    rendered as pale ghosts - measured on avatar_m_arabia, whose
            //    alpha is 98.9% mid-range. That is a metallic GLOSS mask painted
            //    over the whole surface, not a transparency gradient. A real
            //    translucent material has a moderate mid band AND a genuinely
            //    transparent silhouette, so require both.
            const f32 midA  = MidAlphaFraction(tex);
            const f32 zeroA = ZeroAlphaFraction(tex);
            if (rm.alphaTest < 0.5f && mat.usesAlpha() &&
                !IsSheenMaskPath(lpath) && !IsCreatureBodyPath(lpath) &&
                midA >= 0.02f && midA <= 0.60f && zeroA >= 0.02f) {
                rm.blendAlpha = true;
                rm.alphaTest  = 0.0f;
                f32 op = mat.diffuse.a * (mat.diffuseMap.unkFloat > 0.0f
                                            ? mat.diffuseMap.unkFloat : 1.0f);
                rm.opacity = (op < 0.02f) ? 0.02f : (op > 1.0f ? 1.0f : op);
            }

            // 2) Character body armour is ALWAYS solid: its alpha is gloss, and
            //    any cut-out there punches the chainmail through to bare skin.
            if (rm.alphaTest > 0.0f && !rm.blendAlpha && IsSheenMaskPath(lpath))
                rm.alphaTest = 0.0f;

            // 3) ADDITIVE. Silkroad shares one pure-black placeholder texture for
            //    weapon glow panels and energy sails. Drawn opaque it is a SOLID
            //    BLACK flag; the client draws it additively so black adds nothing
            //    and only the glow shows. Gate on the shared "black" basename,
            //    with an all-black content check as the backup.
            {
                std::wstring base = lpath;
                const size_t slash = base.find_last_of(L'\\');
                if (slash != std::wstring::npos) base = base.substr(slash + 1);
                const bool blackShared = base.compare(0, 5, L"black") == 0;
                const bool blackContent = !blackShared && mat.usesAlpha() &&
                                          MeanRgb(tex) < 4.0f;
                if (blackShared || blackContent) {
                    rm.additive   = true;
                    rm.blendAlpha = false;
                    rm.alphaTest  = 0.0f;
                }
            }

            // 4) Colour tint (flag 0x40). Flag 0x40 is set on virtually every
            //    material carrying the D3D default grey diffuse, so tinting by it
            //    unconditionally just darkens every model - only a CHROMATIC
            //    diffuse (a real hue) is a tint, and it is normalised so it
            //    changes hue, not brightness.
            //    ...and only onto a texture that has no hue of its own. A
            //    chromatic diffuse is how a GREYSCALE texture gets coloured;
            //    when the texture is already coloured the diffuse is exporter
            //    noise, and honouring it repaints the model. Measured on
            //    isyutaru: an ice-blue texture (mean R84 G95 B118) times a
            //    (1.00,0.43,0.43) diffuse came out crimson, while the client
            //    draws it blue.
            if ((mat.flags & kMtrlFlagColorTint) &&
                (std::fabs(mat.diffuse.r - mat.diffuse.g) > 0.02f ||
                 std::fabs(mat.diffuse.g - mat.diffuse.b) > 0.02f) &&
                ChromaRgb(tex) < 18.0f) {
                f32 mx = (std::max)(mat.diffuse.r, (std::max)(mat.diffuse.g, mat.diffuse.b));
                if (mx < 1e-3f) mx = 1e-3f;
                rm.tint = Vec3(Sat01(mat.diffuse.r / mx),
                               Sat01(mat.diffuse.g / mx),
                               Sat01(mat.diffuse.b / mx));
            }

            // 5) Self-lit materials - but NOT the exporter's default.
            //    Measured on the corpus: virtually every SRO material ships
            //    emissive == diffuse == the D3D default grey 0.588. Taking that
            //    literally adds 0.59 to every channel and washes the whole model
            //    out to a pale ghost (caught on p_bear_a). This is the same trap
            //    the reference documents for the 0x40 tint flag. Treat emissive
            //    as real self-illumination only when it is CHROMATIC (a hue) or
            //    clearly brighter than the diffuse it ships with.
            {
                const f32 er = mat.emissive.r, eg = mat.emissive.g, eb = mat.emissive.b;
                const bool chromatic = std::fabs(er - eg) > 0.02f || std::fabs(eg - eb) > 0.02f;
                const f32  eMax = (std::max)(er, (std::max)(eg, eb));
                const f32  dMax = (std::max)(mat.diffuse.r,
                                             (std::max)(mat.diffuse.g, mat.diffuse.b));
                const bool brighter = eMax > dMax + 0.15f;
                if (eMax > 0.0f && (chromatic || brighter))
                    rm.emissive = Vec3(Sat01(er), Sat01(eg), Sat01(eb));
            }
        }

        m_impl->vertexCount += rm.vertices.size();
        m_impl->faceCount   += rm.indices.size() / 3;
        ++m_impl->partCount;
        m_impl->meshPartIndex.push_back(partIdx);
        m_impl->scene.meshes.push_back(std::move(rm));
    }

    if (m_impl->scene.meshes.empty())
        return Result::Fail(Status::BadFormat, T(L"Model bos: gecerli ucgen yok."));

    m_impl->scene.computeBounds();
    if (!m_impl->scene.bounds.valid() && model.bounds.valid())
        m_impl->scene.bounds = model.bounds;

    m_impl->content = MeshViewContent::Model;
    m_impl->options.background       = kStudioTop;      // reference showcase backdrop
    m_impl->options.backgroundBottom = kStudioBottom;
    m_impl->titleTr = model.sourcePath;
    m_impl->detailsTr = BuildModelDetails(model, m_impl->vertexCount, m_impl->faceCount,
                                          m_impl->partCount, m_impl->textureCount,
                                          m_impl->scene.bounds);
    // NEVER reorient a RIGGED model - reference parity.
    //
    // A rigged .bms is authored Y-up in bind-pose space and the reference draws
    // it RAW under a camera-only view (no model matrix at all). orientToUp
    // fought that already-correct space: upHint is derived as head-pelvis, which
    // is vertical for a biped but nearly HORIZONTAL for a quadruped, so the
    // baked rotation pitched horses/bears ~90 degrees onto their nose. Only a
    // skeleton-less prop - which really can be exported lying flat - gets the
    // conservative bounding-box stand-up. ('P' still rotates by hand.)
    if (!(model.upHint.length() > 0.5f))
        m_impl->scene.orientUpright();
    m_impl->resetView();   // fresh model -> clean, level default view (not a stuck tilt)
    m_impl->syncTimer();
    m_impl->repaint();
    return Result::Ok();
}

Result MeshView::setSkeleton(const BskFile& skeleton) {
    m_impl->resetContent();

    if (skeleton.bones.empty())
        return Result::Fail(Status::BadFormat, T(L"Iskelet bos: kemik yok."));

    m_impl->skeleton    = skeleton;
    m_impl->hasSkeleton = true;
    m_impl->boneCount   = skeleton.bones.size();
    m_impl->rebuildSkeletonLines();

    if (!m_impl->scene.bounds.valid() && skeleton.bounds.valid())
        m_impl->scene.bounds = skeleton.bounds;

    m_impl->content = MeshViewContent::Skeleton;
    // Skeletons are thin gold/blue lines - they read far better on a DARK studio
    // backdrop than on the light one used for solid models.
    m_impl->options.background       = Rgba(0x24, 0x1A, 0x0E, 0xFF);
    m_impl->options.backgroundBottom = Rgba(0x0E, 0x0A, 0x06, 0xFF);
    m_impl->detailsTr = BuildSkeletonDetails(skeleton);
    m_impl->scene.orientUpright();   // stand up Z-up / lying-authored imports
    m_impl->resetView();   // fresh model -> clean, level default view (not a stuck tilt)
    m_impl->syncTimer();
    m_impl->repaint();
    return Result::Ok();
}

Result MeshView::setAnimation(const BskFile& skeleton, const BanFile& animation) {
    Result r = setSkeleton(skeleton);
    if (!r.ok()) return r;

    if (animation.bones.empty())
        return Result::Fail(Status::BadFormat, T(L"Animasyon bos: hareketli kemik yok."));

    m_impl->animation    = animation;
    m_impl->hasAnimation = true;
    m_impl->durationMs   = AnimDuration(animation);
    m_impl->timeMs       = 0.0f;
    m_impl->loopAnim     = false;        // a lone .ban honours its own type field
    m_impl->content      = MeshViewContent::Animation;
    m_impl->rebuildSkeletonLines();
    m_impl->fitNow();
    m_impl->repaint();
    return Result::Ok();
}

Result MeshView::setModelAnimation(const ComposedModel& model,
                                   const BskFile& skeleton,
                                   const BanFile& animation) {
    Result r = setModel(model);                 // builds the scene + keeps srcModel
    if (!r.ok()) return r;

    if (skeleton.bones.empty() || animation.bones.empty())
        return Result::Ok();                    // static model, nothing to play

    m_impl->skeleton     = skeleton;
    m_impl->hasSkeleton  = true;
    // setSkeleton() is deliberately NOT used here (it would repaint the view in
    // skeleton-line mode and swap in the dark backdrop), but its bookkeeping is
    // still needed - without this the status line reported "0 kemik" for every
    // animated model, which reads as "the skeleton failed to load".
    m_impl->boneCount    = skeleton.bones.size();
    m_impl->animation    = animation;
    m_impl->hasAnimation = true;
    m_impl->durationMs   = AnimDuration(animation);
    m_impl->timeMs       = 0.0f;
    m_impl->lastTick     = 0;
    m_impl->loopAnim     = true;         // an auto-picked idle always loops

    m_impl->buildSkinCache();

    // Only claim animation when the meshes actually carry skin weights - a rigid
    // prop with a skeleton would otherwise "play" while never moving.
    if (!m_impl->applySkinToScene()) {
        m_impl->hasAnimation = false;
        m_impl->durationMs   = 0.0f;
        return Result::Ok();
    }

    // CRITICAL: setModel() baked an orientToUp/orientUpright rotation into every
    // RasterMesh::transform to stand a STATIC model up. Skinned vertices are
    // already in the skeleton's upright bind/world space (like the reference,
    // which draws skinnedPos raw with only the camera), so that transform would
    // be applied a SECOND time and topple the model - which is exactly why a
    // quadruped collapsed. Clear it and re-frame from the skinned bounds.
    for (RasterMesh& m : m_impl->scene.meshes) m.transform = Mat4::Identity();
    m_impl->scene.computeBounds();
    m_impl->content = MeshViewContent::Animation;
    m_impl->playing = m_impl->durationMs > 0.0f;
    m_impl->syncTimer();
    m_impl->fitNow();
    m_impl->repaint();
    return Result::Ok();
}

MeshViewContent MeshView::content() const { return m_impl->content; }

// ---------------------------------------------------------------------------
// View state
// ---------------------------------------------------------------------------
void MeshView::setShadingMode(ShadingMode mode) {
    m_impl->options.shading = mode;
    m_impl->repaint();
}
ShadingMode MeshView::shadingMode() const { return m_impl->options.shading; }

void MeshView::setRenderOptions(const RenderOptions& opt) {
    m_impl->options = opt;
    m_impl->repaint();
}
const RenderOptions& MeshView::renderOptions() const { return m_impl->options; }

Camera&       MeshView::camera()       { return m_impl->camera; }
const Camera& MeshView::camera() const { return m_impl->camera; }

void MeshView::fitToContent() {
    m_impl->fitNow();
    m_impl->repaint();
}

void MeshView::resetView() {
    // Restore the default orbit orientation (Camera's own defaults) then re-fit,
    // so the model returns to exactly how it first appeared.
    m_impl->resetView();
    m_impl->repaint();
}

void MeshView::invalidate() { m_impl->repaint(); }

// ---------------------------------------------------------------------------
// Animation playback
// ---------------------------------------------------------------------------
void MeshView::play() {
    if (m_impl->durationMs <= 0.0f) return;
    m_impl->playing  = true;
    m_impl->lastTick = ::GetTickCount();
    m_impl->syncTimer();
}

void MeshView::pause() {
    m_impl->playing = false;
    m_impl->syncTimer();
    m_impl->repaint();                                    // redraw the stopped frame at full quality
    if (m_impl->onAnimTick) m_impl->onAnimTick();
}

bool MeshView::isPlaying() const { return m_impl->playing; }

void MeshView::setTimeMs(f32 ms) {
    if (!std::isfinite(ms)) ms = 0.0f;
    if (ms < 0.0f) ms = 0.0f;
    if (m_impl->durationMs > 0.0f && ms > m_impl->durationMs) ms = m_impl->durationMs;
    m_impl->timeMs = ms;
    // Skinned model first: scrubbing must deform the mesh, not just the bones.
    if (!m_impl->applySkinToScene() && m_impl->hasSkeleton)
        m_impl->rebuildSkeletonLines();
    m_impl->repaint();
}

f32 MeshView::timeMs() const     { return m_impl->timeMs; }
f32 MeshView::durationMs() const { return m_impl->durationMs; }

void MeshView::setOnAnimTick(std::function<void()> fn) { m_impl->onAnimTick = std::move(fn); }

// ---------------------------------------------------------------------------
// Headless
// ---------------------------------------------------------------------------
Result MeshView::renderToImage(int width, int height, Image& out, int ssOverride) {
    if (m_impl->content == MeshViewContent::Empty &&
        m_impl->scene.meshes.empty() && m_impl->scene.lines.empty()) {
        out.clear();
        return Result::Fail(Status::NotFound, T(L"Goruntulenecek 3B icerik yok."));
    }
    // Render at high supersampling and area-downscale for an ultra-crisp still.
    const long long px = static_cast<long long>(width) * height;
    const int ss = (ssOverride > 0)
                     ? ssOverride
                     : ((px <= 0) ? 1 : (px <= 1'500'000LL ? 4 : (px <= 4'000'000LL ? 3 : 2)));
    if (ss > 1) {
        Image big;
        Result r = RenderSceneToImage(m_impl->scene, m_impl->camera, m_impl->options,
                                      width * ss, height * ss, big);
        if (!r.ok()) return r;
        out = big.scaledBox(width, height);
        out.applyStudioPost(1.32f, 1.06f, 0.26f);   // same present chain as onPaint()
        return Result::Ok();
    }
    {
        Result r = RenderSceneToImage(m_impl->scene, m_impl->camera, m_impl->options,
                                      width, height, out);
        if (r.ok()) out.applyStudioPost(1.32f, 1.06f, 0.26f);
        return r;
    }
}

const Scene& MeshView::scene() const { return m_impl->scene; }

std::wstring MeshView::statusTr() const {
    const Impl& s = *m_impl;
    switch (s.content) {
        case MeshViewContent::Empty:
            return T(L"Bos");

        case MeshViewContent::Mesh:
            return FormatCountTr(s.vertexCount) + T(L" vertex, ") +
                   FormatCountTr(s.faceCount)   + T(L" ucgen");

        case MeshViewContent::Model: {
            std::wstring t = FormatCountTr(s.vertexCount) + T(L" vertex, ") +
                             FormatCountTr(s.faceCount)   + T(L" ucgen, ") +
                             FormatCountTr(s.partCount)   + T(L" parca");
            if (s.textureCount) t += L", " + FormatCountTr(s.textureCount) + T(L" doku");
            return t;
        }

        case MeshViewContent::Skeleton:
            return FormatCountTr(s.boneCount) + T(L" kemik");

        case MeshViewContent::Animation:
            return FormatCountTr(s.boneCount) + T(L" kemik, ") +
                   FormatW(L"%.0f / %.0f ms", s.timeMs, s.durationMs) +
                   (s.playing ? T(L" (oynatiliyor)") : T(L" (durdu)"));
    }
    return T(L"Bos");
}

std::wstring MeshView::detailsTr() const { return m_impl->detailsTr; }

} // namespace sro
