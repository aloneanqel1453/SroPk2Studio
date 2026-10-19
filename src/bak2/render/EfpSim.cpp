// SkillEffect Studio - render/EfpSim.cpp  (port of efp_sim.py v2 - keep the two in lock-step)
#include "EfpSim.h"

#include "RBytes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace se::render {

// ------------------------------------------------------------------------------------------------ RNG
void PyRandom::Seed(std::uint32_t seed) {
    // init_genrand(19650218) + init_by_array({seed})  (CPython _randommodule.c)
    const int N = 624;
    m_mt[0] = 19650218u;
    for (int i = 1; i < N; ++i) m_mt[i] = 1812433253u * (m_mt[i - 1] ^ (m_mt[i - 1] >> 30)) + static_cast<std::uint32_t>(i);
    std::uint32_t key[1] = {seed};
    int keyLen = 1, i = 1, j = 0;
    for (int k = N > keyLen ? N : keyLen; k; --k) {
        m_mt[i] = (m_mt[i] ^ ((m_mt[i - 1] ^ (m_mt[i - 1] >> 30)) * 1664525u)) + key[j] + static_cast<std::uint32_t>(j);
        ++i; ++j;
        if (i >= N) { m_mt[0] = m_mt[N - 1]; i = 1; }
        if (j >= keyLen) j = 0;
    }
    for (int k = N - 1; k; --k) {
        m_mt[i] = (m_mt[i] ^ ((m_mt[i - 1] ^ (m_mt[i - 1] >> 30)) * 1566083941u)) - static_cast<std::uint32_t>(i);
        ++i;
        if (i >= N) { m_mt[0] = m_mt[N - 1]; i = 1; }
    }
    m_mt[0] = 0x80000000u;
    m_i = N;
}

std::uint32_t PyRandom::Next() {
    const int N = 624, M = 397;
    if (m_i >= N) {
        for (int k = 0; k < N; ++k) {
            std::uint32_t y = (m_mt[k] & 0x80000000u) | (m_mt[(k + 1) % N] & 0x7fffffffu);
            m_mt[k] = m_mt[(k + M) % N] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
        }
        m_i = 0;
    }
    std::uint32_t y = m_mt[m_i++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= (y >> 18);
    return y;
}

double PyRandom::Random() {
    std::uint32_t a = Next() >> 5, b = Next() >> 6;
    return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
}

// ------------------------------------------------------------------------------------------------ helpers
namespace {

std::int64_t PyInt(double x) {  // Python int(float): truncation toward zero (clamped for huge values)
    if (std::isnan(x)) return 0;
    constexpr std::int64_t kBig = 9000000000000000000LL;
    if (x > 9.0e18) return kBig;
    if (x < -9.0e18) return -kBig;
    return static_cast<std::int64_t>(x);
}

struct VarInfo { const char* n; EfxCmd c; int variants; };
const VarInfo kVariants[] = {
    {"SetRotationMat", EfxCmd::SetRotationMat, 4}, {"SetRVelocityMat", EfxCmd::SetRVelocityMat, 1},
    {"SetRotation", EfxCmd::SetRotation, 4},       {"SetRVelocity", EfxCmd::SetRVelocity, 1},
    {"SetRotationAxis", EfxCmd::SetRotationAxis, 4}, {"SetRVelocityAxis", EfxCmd::SetRVelocityAxis, 1},
    {"SetBANRot", EfxCmd::SetBANRot, 4},           {"SetPosition", EfxCmd::SetPosition, 12},
    {"SetConePos", EfxCmd::SetConePos, 12},        {"SetBANPos", EfxCmd::SetBANPos, 12},
    {"SetSpherePos", EfxCmd::SetSpherePos, 3},     {"SetVelocity", EfxCmd::SetVelocity, 4},
    {"SetConeVel", EfxCmd::SetConeVel, 4},         {"Force", EfxCmd::Force, 4},
    {"ConeForce", EfxCmd::ConeForce, 4},           {"Attraction", EfxCmd::Attraction, 2},
    {"TextureSlide", EfxCmd::TextureSlide, 1},     {"SetGraphScale", EfxCmd::SetGraphScale, 1},
    {"SetGraphRandomScale", EfxCmd::SetGraphRandomScale, 1}, {"SetGraphDiffuse", EfxCmd::SetGraphDiffuse, 1},
    {"SetShapeRot", EfxCmd::SetShapeRot, 1},       {"SetShapeRotVel", EfxCmd::SetShapeRotVel, 1},
};
const VarInfo* FindVar(const std::string& n) {
    for (const auto& v : kVariants)
        if (n == v.n) return &v;
    return nullptr;
}

V3 RdV3(RReader& r) { double x = r.F32(), y = r.F32(), z = r.F32(); return {x, y, z}; }
M4 RdM4(RReader& r) { float f[16]; for (float& x : f) x = r.F32(); return M4::FromFloats(f); }

bool DecodeOp(const EfpSource& d, EfxOp& op) {
    const VarInfo* vi = FindVar(d.command);
    if (!vi) return false;
    op.name = vi->c;
    op.v = std::min<int>(d.sourceType, vi->variants - 1);
    RReader r(d.payload.data(), d.payload.size());
    switch (op.name) {
        case EfxCmd::SetPosition: case EfxCmd::SetSpherePos: case EfxCmd::SetVelocity: case EfxCmd::Force:
            op.val = RdV3(r);
            break;
        case EfxCmd::SetConePos: case EfxCmd::SetConeVel: case EfxCmd::ConeForce: {
            RdV3(r);                       // authored
            V3 c = RdV3(r);                // converted (min, max, half-angle radians)
            op.cone[0] = c.x; op.cone[1] = c.y; op.cone[2] = c.z;
            break;
        }
        case EfxCmd::SetRotation: case EfxCmd::SetRVelocity:
            RdV3(r);
            op.mat = RdM4(r); op.hasMat = true;
            break;
        case EfxCmd::SetRotationAxis: case EfxCmd::SetRVelocityAxis: case EfxCmd::SetShapeRot: case EfxCmd::SetShapeRotVel:
            r.Skip(16);
            op.mat = RdM4(r); op.hasMat = true;
            break;
        case EfxCmd::SetRotationMat: case EfxCmd::SetRVelocityMat:
            op.mat = RdM4(r); op.hasMat = true;
            break;
        case EfxCmd::Attraction:
            op.f = r.F32();
            break;
        case EfxCmd::SetGraphScale: {
            std::uint32_t n = r.U32();
            for (std::uint32_t i = 0; i < n && r.Ok(); ++i) op.vecs.push_back(RdV3(r));
            break;
        }
        case EfxCmd::SetGraphDiffuse: {
            std::uint32_t n = r.U32();
            for (std::uint32_t i = 0; i < n && r.Ok(); ++i) {
                std::uint8_t b[4];
                r.Raw(b, 4);
                op.quads.push_back({double(b[0]), double(b[1]), double(b[2]), double(b[3])});
            }
            break;
        }
        case EfxCmd::TextureSlide: {
            r.Skip(12);
            std::uint32_t n = r.U32();
            for (std::uint32_t i = 0; i < n && r.Ok(); ++i) {
                double a = r.F32(), b = r.F32(), c = r.F32(), e = r.F32();
                op.quads.push_back({a, b, c, e});
            }
            break;
        }
        case EfxCmd::SetBANPos: {
            r.Skip(4);
            std::uint32_t n = r.U32();
            for (std::uint32_t i = 0; i < n && r.Ok(); ++i) op.vecs.push_back(RdV3(r));
            break;
        }
        case EfxCmd::SetBANRot: {
            r.Skip(4);
            std::uint32_t n = r.U32();
            for (std::uint32_t i = 0; i < n && r.Ok(); ++i) op.mats.push_back(RdM4(r));
            break;
        }
        default: break;
    }
    return true;
}

template <class T> const T* Item(const std::vector<T>& v, int k) {
    if (v.empty()) return nullptr;
    return &v[std::min<std::size_t>(static_cast<std::size_t>(k), v.size() - 1)];
}

template <class T> void EraseOne(std::vector<T*>& v, T* p) {
    auto it = std::find(v.begin(), v.end(), p);
    if (it != v.end()) v.erase(it);
}

std::unique_ptr<EfxNode> BuildNode(const EfpObject& o, EfxNode* parent, int depth) {
    auto nd = std::make_unique<EfxNode>();
    nd->o = &o;
    nd->parent = parent;
    nd->depth = depth;
    nd->name = o.name;
    nd->L = o.lifeFrames;
    const std::string& ls = o.life.has ? o.life.command : std::string("NormalTimeExtinct");
    nd->life = ls == "NormalTimeLoop" ? EfxLife::Loop : (ls == "NeverExtinct" ? EfxLife::Never : EfxLife::Extinct);
    for (const auto& d : o.emitList)
        if (d.has && d.command == "StaticEmit") {
            nd->hasEmit = DecodeStaticEmit(d.payload, nd->emit);
            break;
        }
    const std::uint8_t* s = o.scalars.data();
    auto i32 = [&](int off) { std::int32_t v; std::memcpy(&v, s + off, 4); return v; };
    nd->saveRot = s[0] != 0; nd->savePos = s[1] != 0;
    nd->lkOrbit = i32(2); nd->lkRot = i32(6); nd->lkVel = i32(10);
    nd->hasRvel = s[14] != 0;
    nd->lkTrans = i32(15);
    nd->hasSrv = s[19] != 0;
    const std::string vs = o.view.has ? o.view.command : std::string("ViewNone");
    nd->view = vs == "ViewBillboard" ? EfxView::Billboard : vs == "ViewYBillboard" ? EfxView::YBillboard
             : vs == "ViewVBillboard" ? EfxView::VBillboard : EfxView::None;
    const std::string rs = o.render.has ? o.render.command : std::string("RenderNone");
    nd->render = rs == "RenderPlate" ? EfxRender::Plate : rs == "RenderMesh" ? EfxRender::Mesh
               : rs == "RenderLinkPipe" ? EfxRender::LinkPipe : rs == "RenderLinkDPipe" ? EfxRender::LinkDPipe
               : rs == "RenderLinkObj" ? EfxRender::LinkObj : EfxRender::None;
    nd->cull = o.res.cull == 2 ? 3 : o.res.cull;   // sub_CC1760
    nd->blend[0] = o.res.src; nd->blend[1] = o.res.dst;
    for (int i = 0; i < 3; ++i) { nd->cstage[i] = o.res.stage1[i]; nd->astage[i] = o.res.stage2[i]; }
    nd->tex = o.res.Texture();
    nd->mesh = o.res.Mesh();
    if (const EfpNamed* g = o.Global("BlendScaleGraph")) {
        nd->hasScaleGraph = true;
        nd->scaleGraph = DecodeBlendScaleGraph(g->payload);
    }
    std::int64_t n = std::max<std::int64_t>(nd->L, 1);
    nd->table.assign(static_cast<std::size_t>(n), {});
    for (const auto& d : o.baked) {
        if (!d.has || !FindVar(d.command)) continue;
        std::int64_t start, end;
        double step;
        if (!EfxWindow(d.sourceByte, d.times[0], d.times[1], d.times[2], nd->L, start, step, end)) continue;
        auto op = std::make_unique<EfxOp>();
        if (!DecodeOp(d, *op)) continue;
        float t = static_cast<float>(start);
        const float fs = static_cast<float>(step);
        int k = 0;
        while (static_cast<double>(t) <= static_cast<double>(end) && k < 4096) {   // CA9070: no tolerance
            std::int64_t it = static_cast<std::int64_t>(t);
            if (it >= 0 && it < n) nd->table[static_cast<std::size_t>(it)].push_back({op.get(), k});
            t = t + fs;
            ++k;
        }
        nd->ops.push_back(std::move(op));
    }
    for (const auto& c : o.children) nd->children.push_back(BuildNode(*c, nd.get(), depth + 1));
    return nd;
}

V3 SampleBlend(const EfxNode& nd, double t) {   // sub_C89140
    const auto& keys = nd.scaleGraph;
    if (!nd.hasScaleGraph || keys.empty()) return {1, 1, 1};
    auto v = [&](const EfpBlendV3Key& k) { return V3{k.v[0], k.v[1], k.v[2]}; };
    if (t <= keys[0].t) return v(keys[0]);
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
        double t0 = keys[i].t, t1 = keys[i + 1].t;
        if (t < t1) {
            double u = t1 <= t0 ? 0.0 : (t - t0) / (t1 - t0);
            return v(keys[i]) * (1 - u) + v(keys[i + 1]) * u;
        }
    }
    return v(keys.back());
}

}  // namespace

bool EfxWindow(int mode, double a, double b, double c, std::int64_t L, std::int64_t& start, double& step, std::int64_t& end) {
    if (L < 1) return false;
    for (double x : {a, b, c})
        if (std::isnan(x) || std::isinf(x)) return false;
    start = (mode & 1) ? L * PyInt(a / 100.0) : PyInt(a);
    step = ((mode >> 1) & 1) ? static_cast<double>(L) * b / 100.0 : b;
    int em = (mode >> 2) % 5;
    if (em == 0) end = PyInt(c);
    else if (em == 1) end = L * PyInt(c / 100.0);
    else if (em == 2) end = start + PyInt(c);
    else if (em == 3) end = start - L * PyInt(c / -100.0);
    else {
        if (c < 1.0) return false;
        end = start + PyInt(c * step);
    }
    if (end >= L) end = L - 1;
    if (end > L || start > end || step < 1e-6) return false;
    return true;
}

// ------------------------------------------------------------------------------------------------ effect
EfpEffect::EfpEffect(std::shared_ptr<const EfpFile> file, double scale, int alpha, std::uint32_t seed)
    : m_file(std::move(file)), m_alpha(alpha), m_rng(seed) {
    double rs = m_file->hasRootScale ? static_cast<double>(m_file->rootScale) : 1.0;
    m_s = (rs > 0 ? rs : 1.0) * scale;
    m_el.age = -1;   // Restart (sub_CA9440)
    m_root = BuildNode(*m_file->root, nullptr, 0);
    EfxGroup* g = NewGroup(m_root.get(), &m_el);
    m_root->groups.push_back(g);
    m_rootGroups.push_back(g);
}

EfpEffect::~EfpEffect() {
    // every particle / group still reachable is owned by the node lists
    std::vector<EfxParticle*> parts;
    std::vector<EfxGroup*> groups;
    ForEachNode([&](EfxNode& nd) {
        for (EfxParticle* p : nd.parts) parts.push_back(p);
        for (EfxParticle* p : nd.killed) parts.push_back(p);
        for (EfxParticle* p : nd.lifeList) parts.push_back(p);
        for (EfxGroup* g : nd.groups) groups.push_back(g);
        for (EfxParticle* p : nd.parts) {
            if (p->group) groups.push_back(p->group);
            for (EfxGroup* c : p->groups) groups.push_back(c);
        }
    });
    for (EfxGroup* g : m_rootGroups) groups.push_back(g);
    for (EfxParticle* p : m_deadParts) parts.push_back(p);
    for (EfxGroup* g : m_deadGroups) groups.push_back(g);
    std::sort(parts.begin(), parts.end());
    parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
    std::sort(groups.begin(), groups.end());
    groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
    for (EfxParticle* p : parts) delete p;
    for (EfxGroup* g : groups) delete g;
}

void EfpEffect::ForEachNode(const std::function<void(EfxNode&)>& fn) {
    std::vector<EfxNode*> st{m_root.get()};
    while (!st.empty()) {
        EfxNode* n = st.back();
        st.pop_back();
        fn(*n);
        for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) st.push_back(it->get());
    }
}

EfxGroup* EfpEffect::NewGroup(EfxNode* node, EfxBody* parent) {   // sub_C86DB0
    auto* g = new EfxGroup();
    g->node = node;
    g->parent = parent;
    auto anc = [&](int n) -> EfxBody* {
        if (n <= 0) return nullptr;
        EfxBody* a = parent;
        for (int i = 0; i < n - 1; ++i) {
            if (!a->group) break;   // clamp at the effect element
            a = a->group->parent;
        }
        return a;
    };
    g->aTrans = anc(node->lkTrans);
    g->aOrbit = anc(node->lkOrbit);
    g->aRot = anc(node->lkRot);
    g->aVel = anc(node->lkVel);
    g->rotIdentity = node->lkRot < 0;
    return g;
}

void EfpEffect::SetWorld(const V3& pos, const M4* rot3) {
    V3 np = pos / m_s;
    if (np.x != m_el.pos.x || np.y != m_el.pos.y || np.z != m_el.pos.z || rot3) m_el.moved = true;
    m_el.pos = np;
    if (rot3) {
        M4 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r.m[i][j] = rot3->m[i][j];
        m_el.rot = r;
        m_el.rotated = true;
    }
}

void EfpEffect::Translate(const V3& d) {
    V3 ds = d / m_s;
    for (EfxParticle* p : m_all) { p->pos += ds; p->prevPos += ds; }
    m_el.pos += ds;
    m_el.prevPos += ds;
}

bool EfpEffect::Update() {   // CAF550 + CA9EE0, dt = 1 tick
    if (!m_alive) return false;
    EfxElement& el = m_el;
    if (!el.inited) {
        el.prevRot = el.rot;
        el.prevPos = el.pos;
        el.rotated = true;
        el.inited = true;
    }
    el.drot = Mul(Inverse(el.prevRot), el.rot);
    el.age += 1;
    Step(*m_root);
    bool alive = false;
    for (EfxGroup* g : m_root->groups)
        if (!g->finished || g->ref > 0) { alive = true; break; }
    m_alive = alive;
    if (!alive) { Collect(); return false; }
    if (m_hold) {
        for (EfxParticle* p : m_root->killed) { p->dead = false; p->age = 0; }
        m_root->killed.clear();
    }
    el.prevRot = el.rot;
    el.prevPos = el.pos;
    el.rotated = false;
    el.moved = false;
    Collect();
    return true;
}

void EfpEffect::RenderFollow() {
    EfxElement& el = m_el;
    if (!m_alive || !el.inited || (!el.moved && !el.rotated)) return;
    el.drot = Mul(Inverse(el.prevRot), el.rot);
    Follow(*m_root);
    el.prevRot = el.rot;
    el.prevPos = el.pos;
    el.rotated = false;
    el.moved = false;
}

void EfpEffect::Follow(EfxNode& nd) {   // C863A0: LinkMode tracking only
    for (EfxParticle* p : nd.parts) {
        EfxGroup* g = p->group;
        p->rotated = false;
        if (nd.saveRot) p->saved = p->rot;
        if (nd.savePos) p->prevPos = p->pos;
        if (EfxBody* a = g->aOrbit; a && a->rotated) p->pos = Xf(p->pos - a->prevPos, a->drot) + a->prevPos;
        if (EfxBody* a = g->aRot; a && a->rotated) { p->rot = Mul(p->rot, a->drot); p->rotated = true; }
        if (EfxBody* a = g->aVel; a && a->rotated) p->vel = Xf(p->vel, a->drot);
        if (EfxBody* a = g->aTrans) p->pos = p->pos - a->prevPos + a->pos;
    }
    if (nd.saveRot)
        for (EfxParticle* p : nd.parts) p->drot = Mul(Inverse(p->saved), p->rot);
    for (auto& c : nd.children) Follow(*c);
}

void EfpEffect::Collect() {
    for (EfxParticle* p : m_deadParts) delete p;
    m_deadParts.clear();
    for (EfxGroup* g : m_deadGroups) delete g;
    m_deadGroups.clear();
}

void EfpEffect::Step(EfxNode& nd) {   // CEFUpdater::Update (sub_C85F20)
    {
        std::vector<EfxParticle*> k;
        k.swap(nd.killed);
        for (EfxParticle* p : k) Free(p);                         // 1. free particles killed last step
    }
    {
        std::vector<EfxGroup*> gs = nd.groups;
        for (EfxGroup* g : gs) Emit(g);                           // 2. StaticEmit
    }
    for (EfxParticle* p : nd.parts) {                             // 3. links + kinematics (zombies too)
        EfxGroup* g = p->group;
        p->rotated = false;
        p->hasDiff = false;
        if (nd.saveRot) p->saved = p->rot;
        if (nd.savePos) p->prevPos = p->pos;
        if (EfxBody* a = g->aOrbit; a && a->rotated) p->pos = Xf(p->pos - a->prevPos, a->drot) + a->prevPos;
        if (EfxBody* a = g->aRot; a && a->rotated) { p->rot = Mul(p->rot, a->drot); p->rotated = true; }
        if (EfxBody* a = g->aVel; a && a->rotated) p->vel = Xf(p->vel, a->drot);
        if (nd.hasRvel) { p->rot = Mul(p->rot, p->rvel); p->rotated = true; }
        if (EfxBody* a = g->aTrans) p->pos = p->pos - a->prevPos + a->pos;
        p->pos = p->pos + p->vel;
        if (nd.hasSrv) p->srot = Mul(p->srv, p->srot);
    }
    const std::int64_t n = static_cast<std::int64_t>(nd.table.size());   // 4. program (life list only)
    for (EfxParticle* p : nd.lifeList)
        if (p->age >= 0 && p->age < n)
            for (const auto& [op, k] : nd.table[static_cast<std::size_t>(p->age)]) Op(p, *op, k);
    if (nd.saveRot)                                               // 5. delta rotation
        for (EfxParticle* p : nd.parts) p->drot = Mul(Inverse(p->saved), p->rot);
    for (auto& c : nd.children) Step(*c);                         // 6. children
    const std::int64_t L = nd.L;                                  // 7. life source
    std::vector<EfxParticle*> ll = nd.lifeList;
    for (EfxParticle* p : ll) {
        if (nd.life == EfxLife::Loop) {
            p->age = L > 0 ? (p->age + 1) % L : 0;
        } else if (nd.life == EfxLife::Never) {
            if (p->age != 1) p->age += 1;
            if (L == 0) Kill(p);
        } else {
            p->age += 1;
            if (p->age >= L) { p->age = L - 1; Kill(p); }
        }
    }
}

void EfpEffect::Kill(EfxParticle* p) {
    p->dead = true;
    p->node->killed.push_back(p);
}

void EfpEffect::Free(EfxParticle* p) {   // sub_C87A20
    if (p->freed || !p->dead) return;
    p->freed = true;
    EfxGroup* g = p->group;
    g->acc -= 1.0;                        // alive cap
    EraseOne(p->node->lifeList, p);
    EraseOne(g->elems, p);
    EraseOne(m_all, p);
    std::vector<EfxGroup*> cgs = p->groups;
    for (EfxGroup* cg : cgs) ReleaseGroup(cg);
    p->released = true;
    if (p->ref == 0) Final(p);            // else ZOMBIE: stays in the update list
}

void EfpEffect::ReleaseGroup(EfxGroup* cg) {   // sub_C879B0
    cg->gone = true;
    EraseOne(cg->node->groups, cg);
    if (cg->parent->isParticle) EraseOne(static_cast<EfxParticle*>(cg->parent)->groups, cg);
    if (cg->ref == 0) DestroyGroup(cg);
}

void EfpEffect::Final(EfxParticle* p) {   // sub_C87AA0
    EraseOne(p->node->parts, p);
    EfxGroup* g = p->group;
    g->ref -= 1;
    m_deadParts.push_back(p);
    if (g->ref == 0 && g->gone) DestroyGroup(g);
}

void EfpEffect::DestroyGroup(EfxGroup* g) {   // sub_C879F0
    if (std::find(m_deadGroups.begin(), m_deadGroups.end(), g) != m_deadGroups.end()) return;
    m_deadGroups.push_back(g);
    if (g->parent->isParticle) {
        auto* P = static_cast<EfxParticle*>(g->parent);
        P->ref -= 1;
        if (P->ref == 0 && P->released) Final(P);
    }
}

void EfpEffect::Emit(EfxGroup* g) {   // sub_C84E70
    if (g->gone || !g->node->hasEmit) return;   // no StaticEmit: never finishes
    const EfpStaticEmit& e = g->node->emit;
    std::int64_t a = g->parent->age - static_cast<std::int64_t>(e.start);
    if (a < static_cast<std::int64_t>(e.duration)) {
        g->finished = false;
        if (e.interval > 0 && a >= 0 && a % static_cast<std::int64_t>(e.interval) == 0) {
            double old = g->acc;
            double v = static_cast<double>(e.perEvent) + g->acc;
            g->acc = static_cast<double>(e.maxAlive) >= v ? v : static_cast<double>(e.maxAlive);
            std::int64_t nsp = PyInt(g->acc) - PyInt(old);
            for (std::int64_t i = 0; i < nsp; ++i) Spawn(g);
        }
    } else {
        g->finished = true;
    }
}

void EfpEffect::Spawn(EfxGroup* g) {   // sub_C86CD0 + sub_C88490
    EfxNode* nd = g->node;
    EfxBody* P = g->parent;
    auto* p = new EfxParticle();
    p->isParticle = true;
    p->node = nd;
    p->group = g;
    p->serial = ++m_serial;
    p->rot = P->rot;
    M4 pd = P->drot;
    if (g->rotIdentity) {
        M4 r; r.SetRow(3, p->rot.Row(3)); p->rot = r;
        M4 q; q.SetRow(3, pd.Row(3)); pd = q;
    }
    p->drot = Inverse(pd);
    p->rot = Mul(p->rot, p->drot);
    p->saved = p->rot;
    p->pos = P->pos; p->prevPos = P->pos; p->vel = P->vel;
    if (g->aTrans) {
        p->vel = p->vel - g->aTrans->vel;
        p->pos = p->pos + (g->aTrans->prevPos - g->aTrans->pos);
    }
    nd->parts.push_back(p);
    nd->lifeList.push_back(p);
    g->elems.push_back(p);
    m_all.push_back(p);
    g->ref += 1;
    for (auto& c : nd->children) {
        EfxGroup* cg = NewGroup(c.get(), p);
        c->groups.push_back(cg);
        p->groups.push_back(cg);
        p->ref += 1;
    }
}

V3 EfpEffect::Cone(const double c[3]) {
    double mn = c[0], mx = c[1], ang = c[2];
    V3 v{0.0, m_rng.U() * (mx - mn) + mn, 0.0};
    v = Xf(v, RotZ(m_rng.U() * ang));
    return Xf(v, RotY(m_rng.U() * 6.283185482025146));
}

void EfpEffect::Op(EfxParticle* p, const EfxOp& c, int k) {   // sub_C893F0
    EfxBody* P = p->group->parent;
    const auto& els = p->group->elems;
    auto it = std::find(els.begin(), els.end(), p);
    EfxParticle* Lp = (it != els.end() && it != els.begin()) ? *(it - 1) : nullptr;
    int v = c.v;
    switch (c.name) {
        case EfxCmd::SetRotation: case EfxCmd::SetRotationMat: case EfxCmd::SetRotationAxis: case EfxCmd::SetBANRot: {
            const M4* M = c.name == EfxCmd::SetBANRot ? Item(c.mats, k) : (c.hasMat ? &c.mat : nullptr);
            if (!M) return;
            if (v == 0) p->rot = *M;
            else if (v == 1) p->rot = c.name != EfxCmd::SetBANRot ? Mul(*M, p->rot) : Mul(p->rot, *M);
            else if (v == 2) p->rot = Mul(*M, P->rot);
            else if (Lp) p->rot = Mul(*M, Lp->rot);
            else return;
            p->rotated = true;
            break;
        }
        case EfxCmd::SetRVelocity: case EfxCmd::SetRVelocityMat: case EfxCmd::SetRVelocityAxis:
            if (c.hasMat) p->rvel = c.mat;
            break;
        case EfxCmd::SetPosition: case EfxCmd::SetConePos: case EfxCmd::SetBANPos: {
            V3 val;
            if (c.name == EfxCmd::SetPosition) val = c.val;
            else if (c.name == EfxCmd::SetConePos) val = Cone(c.cone);
            else { const V3* x = Item(c.vecs, k); if (!x) return; val = *x; }
            int org = v % 3, fr = v / 3;
            if ((org == 2 || fr == 3) && !Lp) return;
            V3 w = val;
            if (fr == 1) w = Xf(val, p->rot);
            else if (fr == 2) w = Xf(val, P->rot);
            else if (fr == 3) w = Xf(val, Lp->rot);
            if (org == 0) p->pos = p->pos + w;
            else if (org == 1) p->pos = P->pos + w;
            else p->pos = Lp->pos + w;
            break;
        }
        case EfxCmd::SetSpherePos: {
            V3 b;
            for (;;) {
                double x = m_rng.S(), y = m_rng.S(), z = m_rng.S();
                b = {x, y, z};
                if (Dot(b, b) <= 1.0) break;
            }
            if (v == 0) p->pos = b;                                   // quirk: unscaled unit ball
            else if (v == 1) p->pos = Hadamard(b, c.val) + P->pos;
            else if (Lp) p->pos = Hadamard(b, c.val) + Lp->pos;
            break;
        }
        case EfxCmd::SetVelocity: case EfxCmd::SetConeVel: case EfxCmd::Force: case EfxCmd::ConeForce: {
            bool isSet = c.name == EfxCmd::SetVelocity || c.name == EfxCmd::SetConeVel;
            V3 val = (c.name == EfxCmd::SetVelocity || c.name == EfxCmd::Force) ? c.val : Cone(c.cone);
            if (v == 3 && !Lp) return;
            if (c.name == EfxCmd::ConeForce && v == 0) {   // quirk: local cone, then falls into v1
                p->vel = p->vel + val;
                val = Cone(c.cone);
                v = 1;
            }
            V3 w = val;
            if (v == 1) w = Xf(val, p->rot);
            else if (v == 2) w = Xf(val, P->rot);
            else if (v == 3) w = Xf(val, Lp->rot);
            p->vel = isSet ? w : p->vel + w;
            break;
        }
        case EfxCmd::Attraction: {
            EfxBody* o = v == 0 ? P : static_cast<EfxBody*>(Lp);
            if (!o) return;
            V3 d = p->pos - o->pos;
            double ln = std::sqrt(Dot(d, d));
            if (ln > 1e-9) p->vel = p->vel + d / ln * c.f;
            break;
        }
        case EfxCmd::TextureSlide: {
            const auto* q = Item(c.quads, k);
            if (q) for (int i = 0; i < 4; ++i) p->uv[i] = (*q)[i];
            break;
        }
        case EfxCmd::SetGraphScale: {
            const V3* s = Item(c.vecs, k);
            if (s) p->scale = *s;
            break;
        }
        case EfxCmd::SetGraphRandomScale:
            p->scale = SampleBlend(*p->node, m_rng.U());
            break;
        case EfxCmd::SetGraphDiffuse: {
            const auto* q = Item(c.quads, k);
            if (!q) return;
            double d[4] = {(*q)[0], (*q)[1], (*q)[2], (*q)[3]};
            if (m_alpha != 255) d[3] = static_cast<double>((static_cast<std::int64_t>(d[3]) * m_alpha) >> 8);
            for (int i = 0; i < 4; ++i) p->diffuse[i] = d[i];
            p->hasDiff = true;
            break;
        }
        case EfxCmd::SetShapeRot: if (c.hasMat) p->srot = c.mat; break;
        case EfxCmd::SetShapeRotVel: if (c.hasMat) p->srv = c.mat; break;
        default: break;
    }
}

void EfpEffect::SetNodeHidden(int idx, bool hidden) {
    int i = 0;
    ForEachNode([&](EfxNode& n) { if (i++ == idx) n.hidden = hidden; });
}

bool EfpEffect::Bounds(V3& mn, V3& mx) const {
    bool any = false;
    for (const EfxParticle* p : m_all) {
        if (p->node->render == EfxRender::None || p->node->hidden) continue;
        double r = std::max({std::fabs(p->scale.x), std::fabs(p->scale.y), std::fabs(p->scale.z)}) * 0.5;
        V3 a = (p->pos - V3{r, r, r}) * m_s, b = (p->pos + V3{r, r, r}) * m_s;
        if (!any) { mn = a; mx = b; any = true; continue; }
        mn = {std::min(mn.x, a.x), std::min(mn.y, a.y), std::min(mn.z, a.z)};
        mx = {std::max(mx.x, b.x), std::max(mx.y, b.y), std::max(mx.z, b.z)};
    }
    return any;
}

std::vector<EfxNodeInfo> DescribeEffect(const EfpFile& f) {
    std::vector<EfxNodeInfo> out;
    if (!f.root) return out;
    auto node = BuildNode(*f.root, nullptr, 0);
    std::vector<const EfxNode*> st{node.get()};
    while (!st.empty()) {
        const EfxNode* n = st.back();
        st.pop_back();
        EfxNodeInfo i;
        i.depth = n->depth;
        i.index = static_cast<int>(out.size());
        i.name = n->name;
        i.life = n->o->life.has ? n->o->life.command : "NormalTimeExtinct";
        i.view = n->o->view.has ? n->o->view.command : "ViewNone";
        i.render = n->o->render.has ? n->o->render.command : "RenderNone";
        i.tex = n->tex;
        i.mesh = n->mesh;
        i.lifeFrames = n->L;
        i.hasEmit = n->hasEmit;
        i.emit = n->emit;
        i.cull = n->cull; i.src = n->blend[0]; i.dst = n->blend[1];
        for (int k = 0; k < 3; ++k) { i.cs[k] = n->cstage[k]; i.as[k] = n->astage[k]; }
        i.linkTrans = n->lkTrans; i.linkOrbit = n->lkOrbit; i.linkRot = n->lkRot; i.linkVel = n->lkVel;
        i.ops = static_cast<int>(n->ops.size());
        for (const auto& d : n->o->baked)
            if (d.has && std::find(i.commands.begin(), i.commands.end(), d.command) == i.commands.end()) i.commands.push_back(d.command);
        out.push_back(std::move(i));
        for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) st.push_back(it->get());
    }
    return out;
}

const char* BlendName(std::uint32_t b) {
    static const char* k[] = {"?", "ZERO", "ONE", "SRCCOLOR", "INVSRCCOLOR", "SRCALPHA", "INVSRCALPHA", "DESTALPHA",
                              "INVDESTALPHA", "DESTCOLOR", "INVDESTCOLOR", "SRCALPHASAT", "BOTHSRCALPHA", "BOTHINVSRCALPHA"};
    return b < 14 ? k[b] : "?";
}

const char* TopName(std::uint32_t o) {
    static const char* k[] = {"?", "DISABLE", "SELECTARG1", "SELECTARG2", "MODULATE", "MODULATE2X", "MODULATE4X", "ADD",
                              "ADDSIGNED", "ADDSIGNED2X", "SUBTRACT", "ADDSMOOTH"};
    return o < 12 ? k[o] : "?";
}

int EfpEffect::Count() const {
    int n = 0;
    for (const EfxParticle* p : m_all)
        if (p->node->render != EfxRender::None) ++n;
    return n;
}

void EfpEffect::DrawOrder(std::vector<const EfxParticle*>& outList) const {   // CAEFF0
    struct Ent { std::uint32_t st[9]; std::vector<const EfxParticle*> ps; };
    struct Bucket { const std::string* tex; bool zw; std::vector<Ent> ents; };
    const bool lastOnly = m_file->version == "0013" && (m_file->ints0013[0] & 1);
    std::vector<Bucket> out;
    auto lower = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    };
    for (auto rit = m_all.rbegin(); rit != m_all.rend(); ++rit) {
        const EfxParticle* p = *rit;
        const EfxNode* nd = p->node;
        const bool zw = !p->hasDiff && nd->render == EfxRender::Mesh;
        int bi = -1;
        if (zw) {
            if (!out.empty() && out.back().zw && lower(*out.back().tex, nd->tex)) bi = static_cast<int>(out.size()) - 1;
        } else {
            int lo = (lastOnly && out.size() > 1) ? static_cast<int>(out.size()) - 1 : 0;
            for (int j = static_cast<int>(out.size()) - 1; j >= lo; --j) {
                if (out[j].zw) break;
                if (lower(*out[j].tex, nd->tex)) { bi = j; break; }
            }
        }
        if (bi < 0) { out.push_back({&nd->tex, zw, {}}); bi = static_cast<int>(out.size()) - 1; }
        std::uint32_t st[9] = {nd->cull, nd->blend[0], nd->blend[1], nd->cstage[0], nd->cstage[1], nd->cstage[2],
                               nd->astage[0], nd->astage[1], nd->astage[2]};
        auto& ents = out[bi].ents;
        int ei = -1;
        int lo = (lastOnly && ents.size() > 1) ? static_cast<int>(ents.size()) - 1 : 0;
        for (int j = static_cast<int>(ents.size()) - 1; j >= lo; --j)
            if (!std::memcmp(ents[j].st, st, sizeof(st))) { ei = j; break; }
        if (ei < 0) { Ent e; std::memcpy(e.st, st, sizeof(st)); ents.push_back(std::move(e)); ei = static_cast<int>(ents.size()) - 1; }
        ents[ei].ps.push_back(p);
    }
    outList.clear();
    for (const auto& b : out)
        for (const auto& e : b.ents)
            for (const EfxParticle* q : e.ps) outList.push_back(q);
}

}  // namespace se::render
