// SkillEffect Studio - render/RPlayHost.cpp
#include "RPlayHost.h"

#include <algorithm>
#include <cmath>

namespace se::render {

namespace {
se::sp::Mat4 ToSp(const M4& m) {
    se::sp::Mat4 o;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) o.m[i][j] = m.m[i][j];
    return o;
}
M4 FromSp(const se::sp::Mat4& m) {
    M4 o;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) o.m[i][j] = m.m[i][j];
    return o;
}
M4 Orthonormal(const M4& m) {   // rotation rows normalised, translation kept
    M4 r = m;
    for (int i = 0; i < 3; ++i) r.SetRow(i, Norm(m.Row(i)));
    r.m[0][3] = r.m[1][3] = r.m[2][3] = 0;
    r.m[3][3] = 1;
    return r;
}
}  // namespace

void ScenePlayHost::Update() {
    for (int a = 0; a < 2; ++a) {
        ModelInstance* m = m_scene.Model(m_actor[a]);
        if (!m) continue;
        if (m_tintUntil[a] > 0 && m_scene.TimeMs() >= m_tintUntil[a]) {
            m->tint[0] = m->tint[1] = m->tint[2] = 1;
            m_tintUntil[a] = 0;
        }
        if (m_reacting[a] && m->Finished()) {
            m_reacting[a] = false;
            if (!m->Play(m_cache, 0x00, true, idleGroup[a])) m->PlayIndex(m_cache, 0, true);
        }
    }
}

void ScenePlayHost::ReleaseAll() {
    for (auto& [id, sid] : m_fx) m_scene.ReleaseEffect(sid);
    for (auto& [id, mid] : m_props) m_scene.RemoveModel(mid);
    m_fx.clear();
    m_props.clear();
}

void ScenePlayHost::PlayClip(int actor, int role, bool loop, double startMs) { PlayClipIn(actor, role, L"", loop, startMs); }

// role -1 = stand (in the idle group: players hold their weapon); a clip in its aniGroup (the timeline resolved it)
void ScenePlayHost::PlayClipIn(int actor, int role, const std::wstring& group, bool loop, double startMs) {
    ModelInstance* m = m_scene.Model(Actor(actor));
    if (!m) return;
    const std::wstring idle = actor >= 0 && actor < 2 ? idleGroup[actor] : std::wstring();
    if (actor >= 0 && actor < 2) m_reacting[actor] = false;
    bool ok = role < 0 ? m->Play(m_cache, 0x00, true, idle) : m->Play(m_cache, role, loop, group.empty() ? idle : group);
    if (!ok && role < 0) ok = m->PlayIndex(m_cache, 0, true);
    if (ok && startMs > 0) m->SetTime(startMs);
}

se::sp::Mat4 ScenePlayHost::ActorWorld(int actor) {
    if (ModelInstance* m = m_scene.Model(Actor(actor))) return ToSp(Orthonormal(m->World()));
    M4 w;
    if (actor == 1) { w = RotY(kPi); w.SetRow(3, V3{0, 0, -targetDistance}); }   // faces the caster
    return ToSp(w);
}

bool ScenePlayHost::BoneWorld(int actor, const std::wstring& bone, se::sp::Mat4& out) {
    ModelInstance* m = m_scene.Model(Actor(actor));
    if (!m) return false;
    M4 bw;
    if (!m->BoneWorld(bone, bw)) return false;
    out = ToSp(Orthonormal(bw));
    return true;
}

se::sp::FxId ScenePlayHost::Spawn(const se::sp::FxSpawn& s) {
    if (s.kind == se::sp::ObjKind::Efp) {
        EffectSpec sp;
        sp.path = s.res;
        sp.scale = s.scale > 0 ? s.scale : 1.0;
        sp.hold = s.hold;
        sp.attach = FxAttach::World;
        // until the player's first SetWorld: the caster's position
        if (ModelInstance* m = m_scene.Model(m_actor[0])) sp.worldPos = m->pos;
        int sid = m_scene.AddEffect(sp, nullptr);
        if (sid < 0) return 0;
        se::sp::FxId id = m_next++;
        m_fx[id] = sid;
        return id;
    }
    if (s.kind == se::sp::ObjKind::Bsr) {
        ModelPlace pl;
        pl.scale = s.scale > 0 ? s.scale : 1.0;
        pl.idle = true;
        int mid = m_scene.AddModel(s.res, pl, nullptr);
        if (mid < 0) return 0;
        se::sp::FxId id = (m_next++) | kPropBit;
        m_props[id] = mid;
        return id;
    }
    return 0;   // .wav / .ddj objects are not drawn by the preview
}

void ScenePlayHost::SetWorld(se::sp::FxId id, const se::sp::Mat4& m) {
    M4 w = Orthonormal(FromSp(m));
    if (auto it = m_fx.find(id); it != m_fx.end()) {
        M4 rot = RotPart(w);
        m_scene.SetEffectWorld(it->second, w.T(), &rot);
        return;
    }
    if (auto it = m_props.find(id); it != m_props.end())
        if (ModelInstance* mi = m_scene.Model(it->second)) mi->SetWorldMatrix(w);
}

void ScenePlayHost::SetHold(se::sp::FxId id, bool hold) {
    if (auto it = m_fx.find(id); it != m_fx.end()) m_scene.SetEffectHold(it->second, hold);
}

void ScenePlayHost::SetAlpha(se::sp::FxId id, double a) {
    if (auto it = m_fx.find(id); it != m_fx.end())
        if (EfpEffect* f = m_scene.Effect(it->second)) f->SetAlpha(static_cast<int>(std::clamp(a, 0.0, 1.0) * 255.0 + 0.5));
}

bool ScenePlayHost::Alive(se::sp::FxId id) {
    if (auto it = m_fx.find(id); it != m_fx.end()) return m_scene.EffectAlive(it->second);
    return m_props.count(id) != 0;
}

void ScenePlayHost::Release(se::sp::FxId id) {
    if (auto it = m_fx.find(id); it != m_fx.end()) { m_scene.ReleaseEffect(it->second); m_fx.erase(it); return; }
    if (auto it = m_props.find(id); it != m_props.end()) { m_scene.RemoveModel(it->second); m_props.erase(it); }
}

void ScenePlayHost::Sound(const std::wstring& dataPath, int volume, int actor) {
    if (onSound) onSound(dataPath, volume, actor);
}

void ScenePlayHost::Shake(int level, bool moving) {
    m_scene.Shake(shakeUnits * (level + 1) * (moving ? 0.6 : 1.0), moving ? 600 : 350);
}

void ScenePlayHost::Tint(int actor, const double rgb[6], double ms) {
    ModelInstance* m = m_scene.Model(Actor(actor));
    if (!m || actor < 0 || actor > 1) return;
    const bool bytes = rgb[0] > 2 || rgb[1] > 2 || rgb[2] > 2;   // SCT_MAT args are 0..255
    for (int i = 0; i < 3; ++i) m->tint[i] = std::clamp(bytes ? rgb[i] / 255.0 : rgb[i], 0.0, 2.0);
    m_tintUntil[actor] = m_scene.TimeMs() + std::max(ms, 50.0);
}

void ScenePlayHost::Damage(int actor, int hitIndex, int kinds) {
    if (onDamage) onDamage(actor, hitIndex, kinds);
    if (!damageReaction) return;
    ModelInstance* m = m_scene.Model(Actor(actor));
    if (!m || actor < 0 || actor > 1) return;
    if (m->Play(m_cache, 0x03, false)) m_reacting[actor] = true;   // DAMAGE1 once
}

}  // namespace se::render
