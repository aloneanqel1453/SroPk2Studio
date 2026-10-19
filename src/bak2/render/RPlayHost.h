// SkillEffect Studio - render/RPlayHost.h
// Ready-made se::sp::IPlayHost (skillplay module) on top of render::Scene, so "skill click" = SkillPlayer drives
// the real models / clips / .efp runtime. Uses only the inline parts of skillplay/SkillPlay.h (no link dependency).
//   ScenePlayHost host(scene, cache);
//   host.SetActor(0, casterModelId); host.SetActor(1, targetModelId);   // -1 = virtual actor
//   player.Start(timeline, host);  ... every frame: host.Update(); player.Update(nowMs); scene.Advance(dtMs);
#pragma once

#include "RScene.h"
#include "../skillplay/SkillPlay.h"

#include <functional>
#include <map>

namespace se::render {

class ScenePlayHost : public se::sp::IPlayHost {
public:
    ScenePlayHost(Scene& scene, AssetCache& cache) : m_scene(scene), m_cache(cache) {}
    void SetActor(int actor, int modelId) { if (actor >= 0 && actor < 2) m_actor[actor] = modelId; }
    int Actor(int actor) const { return actor >= 0 && actor < 2 ? m_actor[actor] : -1; }
    double targetDistance = 60;           // virtual target position (0,0,-distance) when actor 1 has no model
    std::function<void(const std::wstring& dataPath, int volume, int actor)> onSound;   // optional audio
    std::function<void(int actor, int hitIndex, int kinds)> onDamage;                    // optional numbers
    bool damageReaction = true;           // target plays DAMAGE1 once on a hit, then idles
    double shakeUnits = 0.6;              // camera shake amplitude per level
    std::wstring idleGroup[2];            // aniGroup of the stand / idle clip (players: the held weapon's group)

    void Update();                         // per frame: tint expiry, return-to-idle after reactions
    void ReleaseAll();                     // effects + props spawned through this host

    // IPlayHost
    void PlayClip(int actor, int role, bool loop, double startMs) override;
    void PlayClipIn(int actor, int role, const std::wstring& group, bool loop, double startMs) override;
    se::sp::Mat4 ActorWorld(int actor) override;
    bool BoneWorld(int actor, const std::wstring& bone, se::sp::Mat4& out) override;
    se::sp::FxId Spawn(const se::sp::FxSpawn& s) override;
    void SetWorld(se::sp::FxId id, const se::sp::Mat4& m) override;
    void SetHold(se::sp::FxId id, bool hold) override;
    void SetAlpha(se::sp::FxId id, double a) override;
    bool Alive(se::sp::FxId id) override;
    void Release(se::sp::FxId id) override;
    void Sound(const std::wstring& dataPath, int volume, int actor) override;
    void Shake(int level, bool moving) override;
    void Tint(int actor, const double rgb[6], double ms) override;
    void Damage(int actor, int hitIndex, int kinds) override;

private:
    static constexpr se::sp::FxId kPropBit = 1ull << 40;
    Scene& m_scene;
    AssetCache& m_cache;
    int m_actor[2] = {-1, -1};
    std::map<se::sp::FxId, int> m_props;   // prop id -> model id
    std::map<se::sp::FxId, int> m_fx;      // fx id -> scene effect id
    se::sp::FxId m_next = 1;
    double m_tintUntil[2] = {0, 0};
    bool m_reacting[2] = {false, false};
};

}  // namespace se::render
