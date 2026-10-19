// SkillEffect Studio - render/EfpSim.h
// .efp runtime = literal C++ port of TERA\_re\efp\efp_sim.py v2 (EFP_RUNTIME.md; CEFUpdater semantics of
// sro_client.exe): 20 Hz tick, dt=1, pos += vel, program table list[age] baked from (start, step, end)+mode,
// firing index k into baked arrays, StaticEmit alive cap per (parent particle, child node) group, LinkMode
// ancestor following, zombie parents + refcounted effect lifetime, SetHold revival, draw order = CAEFF0
// buckets (newest first, texture -> 9-dword state, 0013 header flag bit0).
// The RNG is CPython's random.Random (MT19937, init_by_array) so a given seed reproduces efp_sim.py exactly.
#pragma once

#include "Efp.h"
#include "RMath.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace se::render {

class PyRandom {  // CPython random.Random(seed) for a non-negative int seed < 2^32
public:
    explicit PyRandom(std::uint32_t seed = 1) { Seed(seed); }
    void Seed(std::uint32_t seed);
    double Random();                                   // random.random()
    double U() { return Random(); }
    double S() { return 2.0 * (Random() - 0.5); }      // sub_C877F0
private:
    std::uint32_t Next();
    std::uint32_t m_mt[624];
    int m_i = 625;
};

enum class EfxCmd : std::uint8_t {
    SetRotationMat, SetRVelocityMat, SetRotation, SetRVelocity, SetRotationAxis, SetRVelocityAxis, SetBANRot,
    SetPosition, SetConePos, SetBANPos, SetSpherePos, SetVelocity, SetConeVel, Force, ConeForce, Attraction,
    TextureSlide, SetGraphScale, SetGraphRandomScale, SetGraphDiffuse, SetShapeRot, SetShapeRotVel, None
};
enum class EfxLife : std::uint8_t { Extinct, Loop, Never };
enum class EfxView : std::uint8_t { None, Billboard, YBillboard, VBillboard };
enum class EfxRender : std::uint8_t { None, Plate, Mesh, LinkPipe, LinkDPipe, LinkObj };

struct EfxOp {
    EfxCmd name = EfxCmd::None;
    int v = 0;                         // variant (sourceType clamped)
    V3 val;                            // vector payload
    double cone[3] = {0, 0, 0};        // converted (min, max, half-angle radians)
    double f = 0;                      // Attraction
    bool hasMat = false;
    M4 mat;
    std::vector<M4> mats;              // SetBANRot
    std::vector<V3> vecs;              // SetBANPos / SetGraphScale
    std::vector<std::array<double, 4>> quads;  // TextureSlide frames / SetGraphDiffuse (b,g,r,a)
};

struct EfxGroup;
struct EfxParticle;

struct EfxNode {
    const EfpObject* o = nullptr;
    EfxNode* parent = nullptr;
    int depth = 0;
    std::string name;
    std::int64_t L = 0;
    EfxLife life = EfxLife::Extinct;
    bool hasEmit = false;
    EfpStaticEmit emit;
    bool saveRot = false, savePos = false, hasRvel = false, hasSrv = false;
    int lkTrans = 0, lkOrbit = 0, lkRot = 0, lkVel = 0;
    EfxView view = EfxView::None;
    EfxRender render = EfxRender::None;
    std::uint32_t cull = 3, blend[2] = {5, 6}, cstage[3] = {2, 0, 2}, astage[3] = {2, 0, 2};
    std::string tex, mesh;                     // raw (CP949) paths inside Particles.pk2
    bool hasScaleGraph = false;
    std::vector<EfpBlendV3Key> scaleGraph;
    std::vector<std::unique_ptr<EfxOp>> ops;
    std::vector<std::vector<std::pair<const EfxOp*, int>>> table;   // list[age] -> (op, k)
    std::vector<std::unique_ptr<EfxNode>> children;
    // updater lists
    std::vector<EfxGroup*> groups;
    std::vector<EfxParticle*> parts, lifeList, killed;
    int texId = -1, meshId = -1;               // renderer-side resource handles (filled by EfxDraw)
    bool hidden = false;                       // preview only: not drawn (simulation unchanged)
};

struct EfxBody {  // shared by the effect element and particles (ancestor operand)
    std::int64_t age = 0;
    V3 pos, prevPos, vel;
    M4 rot, drot;
    bool rotated = false;
    EfxGroup* group = nullptr;   // nullptr for the element
    bool isParticle = false;
};

struct EfxParticle : EfxBody {
    EfxNode* node = nullptr;
    M4 saved, srot, srv, rvel;
    V3 scale{1, 1, 1};
    double diffuse[4] = {255, 255, 255, 255};   // b, g, r, a (0..255)
    double uv[4] = {0, 0, 1, 1};                // u0, v0, du, dv
    bool dead = false, freed = false, hasDiff = false, released = false;
    int ref = 0;
    std::vector<EfxGroup*> groups;
    std::uint64_t serial = 0;
};

struct EfxElement : EfxBody {
    M4 prevRot;
    bool inited = false;
    bool moved = false;   // +75: SetPosition / SetWorldMatrix since the last Update / Render
};

struct EfxGroup {
    EfxNode* node = nullptr;
    EfxBody* parent = nullptr;
    double acc = 0;
    bool finished = false, gone = false;
    std::vector<EfxParticle*> elems;
    EfxBody *aTrans = nullptr, *aOrbit = nullptr, *aRot = nullptr, *aVel = nullptr;
    bool rotIdentity = false;
    int ref = 0;
};

class EfpEffect {
public:
    // scale = caller scale (SetScale); world = sim * S().  alpha = SetAlpha (255 = none).  seed: PyRandom seed.
    EfpEffect(std::shared_ptr<const EfpFile> file, double scale = 1.0, int alpha = 255, std::uint32_t seed = 1);
    ~EfpEffect();
    EfpEffect(const EfpEffect&) = delete;
    EfpEffect& operator=(const EfpEffect&) = delete;

    // SetWorldMatrix (rot3 != nullptr: rotation rows used, translation ignored) / SetPosition (rot3 == nullptr).
    // pos in WORLD units (divided by S()).
    void SetWorld(const V3& pos, const M4* rot3);
    bool Update();                               // one 20 Hz tick; false once the effect ended (Release)
    // CAC6A0 render-time LinkMode follow (VERIFY 2.8): when SetWorld moved the element since the last Update /
    // RenderFollow (frames between 20 Hz ticks), apply only the LinkMode tracking (no pos += vel, no program).
    void RenderFollow();
    void SetHold(bool h) { m_hold = h; }         // SetHold(1): loop by reviving killed root particles
    bool Hold() const { return m_hold; }
    bool Alive() const { return m_alive; }
    double S() const { return m_s; }
    void SetAlpha(int a) { m_alpha = a; }
    void Translate(const V3& worldDelta);        // IEasyFxEffect::Translate: moves EVERY particle
    int Count() const;                           // drawn particles (render != RenderNone) in the effect list
    int CountAll() const { return static_cast<int>(m_all.size()); }
    std::int64_t Frame() const { return m_el.age; }
    const EfpFile& File() const { return *m_file; }
    EfxNode* Root() { return m_root.get(); }
    const EfxElement& Element() const { return m_el; }
    const std::vector<EfxParticle*>& All() const { return m_all; }
    // CAEFF0 draw order (killed-but-not-freed included, zombies excluded)
    void DrawOrder(std::vector<const EfxParticle*>& out) const;
    void ForEachNode(const std::function<void(EfxNode&)>& fn);
    void SetNodeHidden(int depthFirstIndex, bool hidden);   // index = DescribeEffect order
    // world-space bounds of the drawn particles (false when none)
    bool Bounds(V3& mn, V3& mx) const;

private:
    void Step(EfxNode& nd);
    void Follow(EfxNode& nd);
    void Kill(EfxParticle* p);
    void Free(EfxParticle* p);
    void ReleaseGroup(EfxGroup* g);
    void Final(EfxParticle* p);
    void DestroyGroup(EfxGroup* g);
    void Emit(EfxGroup* g);
    void Spawn(EfxGroup* g);
    EfxGroup* NewGroup(EfxNode* node, EfxBody* parent);
    V3 Cone(const double c[3]);
    void Op(EfxParticle* p, const EfxOp& c, int k);
    void Collect();

    std::shared_ptr<const EfpFile> m_file;
    std::unique_ptr<EfxNode> m_root;
    EfxElement m_el;
    double m_s = 1.0;
    int m_alpha = 255;
    bool m_hold = false, m_alive = true;
    PyRandom m_rng;
    std::vector<EfxParticle*> m_all;               // effect-wide list (+600) = render order base
    std::vector<EfxParticle*> m_deadParts;         // finally removed, deleted at the end of Update
    std::vector<EfxGroup*> m_deadGroups;
    std::vector<EfxGroup*> m_rootGroups;           // never destroyed
    std::uint64_t m_serial = 0;
};

// node table for inspectors (depth-first, root first)
struct EfxNodeInfo {
    int depth = 0, index = 0;
    std::string name, life, view, render, tex, mesh;   // raw (CP949) strings
    std::int64_t lifeFrames = 0;
    bool hasEmit = false;
    EfpStaticEmit emit;
    std::uint32_t cull = 0, src = 0, dst = 0, cs[3] = {}, as[3] = {};
    int linkTrans = 0, linkOrbit = 0, linkRot = 0, linkVel = 0, ops = 0;
    std::vector<std::string> commands;         // baked program command names (unique, file order)
};
std::vector<EfxNodeInfo> DescribeEffect(const EfpFile& f);
const char* BlendName(std::uint32_t d3dblend);   // "SRCALPHA" ...
const char* TopName(std::uint32_t d3dtop);       // "MODULATE2X" ...

// sub_CA0F40 command window: false when the command never fires
bool EfxWindow(int mode, double a, double b, double c, std::int64_t L, std::int64_t& start, double& step, std::int64_t& end);

}  // namespace se::render
