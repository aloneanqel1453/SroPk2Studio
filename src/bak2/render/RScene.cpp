// SkillEffect Studio - render/RScene.cpp
#include "RScene.h"

#include "EfxDraw.h"
#include "EfxSheet.h"
#include "RGif.h"
#include "SoftRaster.h"

#include <algorithm>
#include <cmath>

namespace se::render {

Scene::Scene(AssetCache* cache) : m_cache(cache) { camera.Setup(); }
Scene::~Scene() = default;

int Scene::AddModel(const std::wstring& path, const ModelPlace& place, std::wstring* err) {
    std::wstring e;
    auto def = ModelDef::Load(*m_cache, path, place.materialSet, e);
    if (!def) { if (err) *err = e; return -1; }
    auto inst = std::make_unique<ModelInstance>(def);
    inst->pos = place.pos;
    inst->yaw = place.yawDeg * kPi / 180.0;
    inst->scale = place.scale > 0 ? place.scale : 1.0;
    if (place.idle && !inst->Play(*m_cache, 0x00, true, place.idleGroup) && !inst->PlayIndex(*m_cache, 0, true)) inst->Stop();
    int id = m_nextModel++;
    m_models[id] = std::move(inst);
    m_place[id] = place;
    if (place.palette || def->parts.empty())   // effect-only resources always show their particles
        for (const auto& b : def->bsrs)
            for (const auto& p : b->particles) {
                EffectSpec s;
                s.path = p.path;
                s.hold = true;
                s.attach = p.bone.empty() ? FxAttach::ModelOrigin : FxAttach::Bone;
                s.model = id;
                s.bone = p.bone;
                s.scale = place.scale > 0 ? place.scale : 1.0;
                AddEffect(s, nullptr);
            }
    return id;
}

ModelInstance* Scene::Model(int id) {
    auto it = m_models.find(id);
    return it == m_models.end() ? nullptr : it->second.get();
}

void Scene::RemoveModel(int id) {
    m_models.erase(id);
    m_place.erase(id);
    for (auto it = m_fx.begin(); it != m_fx.end();)
        if (it->second.spec.model == id && it->second.spec.attach != FxAttach::World) it = m_fx.erase(it);
        else ++it;
}

std::vector<int> Scene::ModelIds() const {
    std::vector<int> v;
    for (const auto& [id, m] : m_models) v.push_back(id);
    return v;
}

int Scene::AddEffect(const EffectSpec& spec, std::string* err) {
    std::string e;
    auto file = m_cache->Efp(spec.path, &e);
    if (!file) { if (err) *err = e; return -1; }
    int id = m_nextFx++;
    FxSlot s;
    s.spec = spec;
    std::uint32_t seed = spec.seed ? spec.seed : static_cast<std::uint32_t>(1 + id);
    s.fx = std::make_unique<EfpEffect>(file, spec.scale > 0 ? spec.scale : 1.0, spec.alpha, seed);
    s.fx->SetHold(spec.hold);
    m_fx[id] = std::move(s);
    return id;
}

EfpEffect* Scene::Effect(int id) {
    auto it = m_fx.find(id);
    return it == m_fx.end() ? nullptr : it->second.fx.get();
}

bool Scene::EffectAlive(int id) const {
    auto it = m_fx.find(id);
    return it != m_fx.end() && it->second.fx && it->second.fx->Alive();
}

void Scene::SetEffectWorld(int id, const V3& pos, const M4* rot) {
    auto it = m_fx.find(id);
    if (it == m_fx.end()) return;
    it->second.spec.attach = FxAttach::World;
    it->second.spec.worldPos = pos;
    it->second.spec.hasWorldRot = rot != nullptr;
    if (rot) it->second.spec.worldRot = *rot;
}

void Scene::SetEffectHold(int id, bool hold) {
    if (EfpEffect* f = Effect(id)) f->SetHold(hold);
}

void Scene::TranslateEffect(int id, const V3& d) {
    if (EfpEffect* f = Effect(id)) f->Translate(d);
}

void Scene::ReleaseEffect(int id) { m_fx.erase(id); }
void Scene::ClearEffects() { m_fx.clear(); }

int Scene::EffectCount() const { return static_cast<int>(m_fx.size()); }

std::vector<int> Scene::EffectIds() const {
    std::vector<int> v;
    for (const auto& [id, s] : m_fx) v.push_back(id);
    return v;
}

const EffectSpec* Scene::EffectSpecOf(int id) const {
    auto it = m_fx.find(id);
    return it == m_fx.end() ? nullptr : &it->second.spec;
}

int Scene::ParticleCount() const {
    int n = 0;
    for (const auto& [id, s] : m_fx)
        if (s.fx && s.started) n += s.fx->Count();
    return n;
}

void Scene::PlaceEffect(FxSlot& s) {
    const EffectSpec& sp = s.spec;
    static const M4 I;
    if (sp.attach == FxAttach::World) {
        s.fx->SetWorld(sp.worldPos, sp.hasWorldRot ? &sp.worldRot : nullptr);
        return;
    }
    ModelInstance* m = Model(sp.model);
    if (!m) return;
    M4 face = RotY(m->yaw);
    V3 off = Xf3(sp.offset, face) * m->scale;
    if (sp.attach == FxAttach::ModelOrigin) {
        s.fx->SetWorld(m->pos + off, &face);   // no bone: rotation = character world rotation (A47EE0)
        return;
    }
    M4 bw;
    if (sp.attach == FxAttach::BoneSnapshot && s.snapped) { s.fx->SetWorld(s.snapPos, &I); return; }
    if (!m->BoneWorld(sp.bone, bw)) {   // missing bone: behave like StartBone "none"
        s.fx->SetWorld(m->pos + off, &face);
        return;
    }
    V3 p = bw.T() + off;
    if (sp.attach == FxAttach::BoneSnapshot) { s.snapPos = p; s.snapped = true; }
    if (sp.boneRot) {
        M4 r = RotPart(bw);
        for (int i = 0; i < 3; ++i) r.SetRow(i, Norm(r.Row(i)));
        s.fx->SetWorld(p, &r);
    } else {
        s.fx->SetWorld(p, &I);   // bit1 unset: identity rotation, position only
    }
}

void Scene::TickEffects() {
    for (auto it = m_fx.begin(); it != m_fx.end();) {
        FxSlot& s = it->second;
        if (s.spec.delayTicks > 0) { --s.spec.delayTicks; ++it; continue; }
        PlaceEffect(s);
        s.started = true;
        if (!s.fx->Update()) { it = m_fx.erase(it); continue; }
        ++it;
    }
}

void Scene::Advance(double ms) {
    if (!(ms > 0)) return;
    if (ms > 10000) ms = 10000;
    while (ms > 1e-9) {
        double step = std::min(ms, 50.0 - m_tickAcc);
        for (auto& [id, m] : m_models) m->Advance(step);
        m_time += step;
        m_tickAcc += step;
        ms -= step;
        if (m_tickAcc >= 50.0 - 1e-9) {
            m_tickAcc = 0;
            ++m_tick;
            TickEffects();
        }
    }
}

void Scene::Reset() {
    m_fx.clear();
    m_shakeAmp = m_shakeUntil = 0;
    m_time = 0;
    m_tickAcc = 0;
    m_tick = 0;
    for (auto& [id, m] : m_models) {
        auto pl = m_place[id];
        if (pl.idle && !m->Play(*m_cache, 0x00, true, pl.idleGroup) && !m->PlayIndex(*m_cache, 0, true)) m->Stop();
    }
}

void Scene::FrameModels(double yawDeg, double pitchDeg, double distMul) {
    V3 mn, mx;
    bool any = false;
    for (auto& [id, m] : m_models) {
        V3 a, b;
        m->Bounds(a, b);
        if (!any) { mn = a; mx = b; any = true; }
        else {
            mn = {std::min(mn.x, a.x), std::min(mn.y, a.y), std::min(mn.z, a.z)};
            mx = {std::max(mx.x, b.x), std::max(mx.y, b.y), std::max(mx.z, b.z)};
        }
    }
    if (!any) { mn = {-9, 0, -9}; mx = {9, 18, 9}; }
    V3 c = (mn + mx) * 0.5;
    double r = std::max(Len(mx - mn) * 0.5, 4.0);
    double yw = yawDeg * kPi / 180, pt = pitchDeg * kPi / 180;
    V3 dir{std::sin(yw) * std::cos(pt), std::sin(pt), std::cos(yw) * std::cos(pt)};
    camera.at = c;
    camera.eye = c + dir * (r * distMul);
    camera.Setup();
}

void Scene::Shake(double amplitude, double ms) {
    m_shakeAmp = amplitude;
    m_shakeUntil = m_time + ms;
}

void Scene::BuildFrame(RFrame& f) {
    f.Clear();
    camera.Setup();
    f.cam = camera;
    if (m_time < m_shakeUntil && m_shakeAmp > 0) {
        double k = m_shakeAmp * (m_shakeUntil - m_time) / 400.0;
        k = std::min(k, m_shakeAmp);
        V3 j = camera.x * (std::sin(m_time * 0.11) * k) + camera.y * (std::sin(m_time * 0.083 + 1.3) * k);
        f.cam.eye = camera.eye + j;
        f.cam.at = camera.at + j;
        f.cam.Setup();
    }
    if (opt.studio) {
        f.bgGradient = true;
        f.bg[0] = 0.17; f.bg[1] = 0.18; f.bg[2] = 0.21;
        f.bg2[0] = 0.04; f.bg2[1] = 0.04; f.bg2[2] = 0.05;
    }
    if (opt.grid) {
        V3 c = camera.at;
        double ext = std::max(20.0, Len(camera.eye - camera.at) * 0.8);
        double stepG = ext > 120 ? 20 : (ext > 50 ? 10 : 5);
        int n = static_cast<int>(ext / stepG);
        double cx = std::round(c.x / stepG) * stepG, cz = std::round(c.z / stepG) * stepG;
        for (int i = -n; i <= n; ++i) {
            RLine a, b;
            a.a = {cx - n * stepG, 0, cz + i * stepG}; a.b = {cx + n * stepG, 0, cz + i * stepG};
            b.a = {cx + i * stepG, 0, cz - n * stepG}; b.b = {cx + i * stepG, 0, cz + n * stepG};
            for (auto* l : {&a, &b}) { l->rgb[0] = 52; l->rgb[1] = 56; l->rgb[2] = 64; f.lines.push_back(*l); }
        }
    }
    if (opt.human) AddHumanLines(f, {0, 0, 0});
    if (opt.shadow && opt.models)   // soft contact shadow: radial alpha disc on the ground under each model
        for (auto& [id, m] : m_models) {
            if (!m->visible || m->Def()->parts.empty()) continue;
            V3 a, b;
            m->Bounds(a, b);
            double r = std::max(b.x - a.x, b.z - a.z) * 0.55;
            if (!(r > 0.5) || a.y > r * 2) continue;
            V3 c{(a.x + b.x) * 0.5, 0.02, (a.z + b.z) * 0.5};
            RState st;
            st.lit = true; st.cull = 1; st.ztest = true; st.zwrite = false; st.src = 5; st.dst = 6;
            RBatch& bt = f.Batch(st, nullptr);
            const int seg = 28;
            for (int i = 0; i < seg; ++i) {
                double a0 = 2 * kPi * i / seg, a1 = 2 * kPi * (i + 1) / seg;
                RVtx v0, v1, v2;
                v0.p = c; v0.c[0] = v0.c[1] = v0.c[2] = 0; v0.c[3] = 0.42;
                v1.p = c + V3{std::cos(a0) * r, 0, std::sin(a0) * r}; v1.c[0] = v1.c[1] = v1.c[2] = 0; v1.c[3] = 0;
                v2.p = c + V3{std::cos(a1) * r, 0, std::sin(a1) * r}; v2.c[0] = v2.c[1] = v2.c[2] = 0; v2.c[3] = 0;
                bt.v.push_back(v0); bt.v.push_back(v1); bt.v.push_back(v2);
            }
        }
    if (opt.models) {
        for (auto& [id, m] : m_models) m->Draw(f, opt.look, 0);
        for (auto& [id, m] : m_models) m->Draw(f, opt.look, 1);
    }
    if (opt.bones)
        for (auto& [id, m] : m_models) {
            auto def = m->Def();
            if (!def->HasSkeleton()) continue;
            for (std::size_t i = 0; i < def->skel->bones.size(); ++i) {
                int p = def->skel->bones[i].parentIndex;
                M4 a, b;
                if (p < 0 || !m->BoneWorld(static_cast<int>(i), a) || !m->BoneWorld(p, b)) continue;
                RLine l;
                l.a = a.T(); l.b = b.T();
                l.rgb[0] = 90; l.rgb[1] = 220; l.rgb[2] = 120;
                f.lines.push_back(l);
            }
        }
    // render-time LinkMode follow (CAC6A0): attached effects track their anchor every frame between 20 Hz ticks
    for (auto& [id, s] : m_fx)
        if (s.started && s.fx && s.fx->Alive()) {
            PlaceEffect(s);
            s.fx->RenderFollow();
        }
    if (opt.effects) {
        EfxDrawOptions eo;
        eo.ztest = true;
        for (auto& [id, s] : m_fx)
            if (s.started && s.fx && s.fx->Alive()) DrawEffect(*s.fx, *m_cache, f, eo);
    }
}

bool Scene::Render(Image& out, int W, int H, int threads, int ssaa) {
    ssaa = std::clamp(ssaa, 1, 4);
    camera.W = W * ssaa;
    camera.H = H * ssaa;
    RFrame f;
    BuildFrame(f);
    for (auto& l : f.labels) { l.x *= ssaa; l.y *= ssaa; }
    SoftRaster sr;
    Image big;
    sr.Draw(f, ssaa == 1 ? out : big, threads);
    if (ssaa > 1) {
        out.w = W;
        out.h = H;
        out.rgba.assign(static_cast<std::size_t>(W) * H * 4, 255);
        const int n = ssaa * ssaa;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int c = 0; c < 3; ++c) {
                    int s = 0;
                    for (int j = 0; j < ssaa; ++j)
                        for (int i = 0; i < ssaa; ++i) s += big.Px(x * ssaa + i, y * ssaa + j)[c];
                    out.Px(x, y)[c] = static_cast<std::uint8_t>(s / n);
                }
    }
    camera.W = W;
    camera.H = H;
    return !out.Empty();
}

bool Scene::RenderPng(const std::wstring& path, int W, int H, std::string& err, int threads, int ssaa) {
    Image img;
    if (!Render(img, W, H, threads, ssaa)) { err = "render failed"; return false; }
    return WritePng(path, img, err);
}

bool Scene::RecordGif(const std::wstring& path, int W, int H, double ms, int fps, std::string& err,
                      const std::function<void(double)>& onTick) {
    fps = std::clamp(fps, 1, 50);
    const double dt = 1000.0 / fps;
    GifWriter g;
    if (!g.Begin(path, W, H, static_cast<int>(std::lround(100.0 / fps)))) { err = "gif begin"; return false; }
    for (double t = 0; t <= ms; t += dt) {
        Image img;
        Render(img, W, H, 6, 1);
        g.AddFrame(img);
        if (onTick) onTick(dt);
        Advance(dt);
    }
    return g.End(err);
}

bool Scene::FrameAll(double yawDeg, double pitchDeg, double distMul) {
    V3 mn, mx;
    bool any = false;
    auto add = [&](const V3& a, const V3& b) {
        if (!any) { mn = a; mx = b; any = true; return; }
        mn = {std::min(mn.x, a.x), std::min(mn.y, a.y), std::min(mn.z, a.z)};
        mx = {std::max(mx.x, b.x), std::max(mx.y, b.y), std::max(mx.z, b.z)};
    };
    for (auto& [id, m] : m_models) {
        if (m->Def()->parts.empty()) continue;
        V3 a, b;
        m->Bounds(a, b);
        add(a, b);
    }
    for (auto& [id, s] : m_fx) {
        V3 a, b;
        if (s.started && s.fx && s.fx->Bounds(a, b)) add(a, b);
    }
    if (!any) { FrameModels(yawDeg, pitchDeg, distMul); return false; }
    V3 c = (mn + mx) * 0.5;
    double r = std::max(Len(mx - mn) * 0.5, 4.0);
    double yw = yawDeg * kPi / 180, pt = pitchDeg * kPi / 180;
    V3 dir{std::sin(yw) * std::cos(pt), std::sin(pt), std::cos(yw) * std::cos(pt)};
    camera.at = c;
    camera.eye = c + dir * (r * distMul);
    camera.Setup();
    return true;
}

}  // namespace se::render
