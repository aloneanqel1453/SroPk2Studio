// SkillEffect Studio - render/RScene.h
// Preview scene: model instances (animated, skinned) + effect instances (exact .efp runtime) + camera.
// Clock: models advance in milliseconds; effects run on the client's fixed 20 Hz tick (one Update per 50 ms,
// dt = 1), with bone attachments re-evaluated at each tick time (EFP_RUNTIME §2, §10). The skill player
// (skillplay module) drives this API through its host; the scene itself has no skill logic.
#pragma once

#include "EfpSim.h"
#include "RModel.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace se::render {

struct ModelPlace {
    V3 pos;
    double yawDeg = 0;          // turn around +Y (degrees)
    double scale = 1.0;         // characterdata Scale / 100 when known
    int materialSet = 0;        // characterdata TextureType
    bool idle = true;           // start the STAND1 clip looped (falls back to clip 0)
    std::wstring idleGroup;     // aniGroup of the stand clip (players: the held weapon's group; "" = default)
    bool palette = false;       // spawn the .bsr mod-palette bone particles (looped)
};

enum class FxAttach : std::uint8_t {
    World,          // fixed world position / rotation (SetWorld by the caller for missiles)
    ModelOrigin,    // model position, model facing (StartBone "none": rotation = character world rotation)
    Bone,           // bone position; rotation = character facing unless boneRot (then bone x character)
    BoneSnapshot,   // '#': bone position at creation only
};

struct EffectSpec {
    std::wstring path;           // inside Particles.pk2
    double scale = 1.0;          // effect scale (character scale x row scale)
    bool hold = false;           // SetHold(1) (AT_LOOP / ACT_L / deco loop)
    int alpha = 255;
    std::uint32_t seed = 0;      // 0 = automatic (1 + effect id)
    FxAttach attach = FxAttach::World;
    int model = -1;              // model id for ModelOrigin / Bone
    std::wstring bone;
    V3 offset;                   // StartOffset, in the character's frame (rotated by its facing)
    bool boneRot = false;        // use the bone rotation (flag bit1); default = position only
    V3 worldPos;                 // World attach
    bool hasWorldRot = false;
    M4 worldRot;                 // rows = local x, y, z in world (World attach)
    int delayTicks = 0;          // start after N ticks (keeps the 20 Hz phase)
};

struct SceneOptions {
    bool grid = true;
    bool human = false;          // 18-unit reference stick figure at the origin
    bool bones = false;          // skeleton lines
    bool effects = true;
    bool models = true;
    bool studio = true;          // gradient background (else efp_sim flat colour)
    bool shadow = true;          // soft contact shadow under every model
    ModelLook look;
};

class Scene {
public:
    explicit Scene(AssetCache* cache);
    ~Scene();
    AssetCache* Cache() const { return m_cache; }

    // models
    int AddModel(const std::wstring& path, const ModelPlace& place, std::wstring* err = nullptr);
    ModelInstance* Model(int id);
    void RemoveModel(int id);
    std::vector<int> ModelIds() const;

    // effects (returns id, -1 on failure)
    int AddEffect(const EffectSpec& spec, std::string* err = nullptr);
    EfpEffect* Effect(int id);
    bool EffectAlive(int id) const;
    void SetEffectWorld(int id, const V3& pos, const M4* rot);   // World attach (missiles): every frame
    void SetEffectHold(int id, bool hold);
    void TranslateEffect(int id, const V3& d);
    void ReleaseEffect(int id);
    void ClearEffects();
    int EffectCount() const;
    std::vector<int> EffectIds() const;
    const EffectSpec* EffectSpecOf(int id) const;
    int ParticleCount() const;

    // clock
    void Advance(double ms);        // sub-steps at 20 Hz tick boundaries
    double TimeMs() const { return m_time; }
    std::int64_t Tick() const { return m_tick; }
    void Reset();                   // time 0, effects cleared, models back to their idle start

    // view
    RCamera camera;
    SceneOptions opt;
    void FrameModels(double yawDeg = 215, double pitchDeg = 16, double distMul = 2.6);   // fit camera
    // fit models AND live effect particles (effect-only previews); false when nothing is visible
    bool FrameAll(double yawDeg = 215, double pitchDeg = 16, double distMul = 2.6);
    void Shake(double amplitude, double ms);   // camera shake (SCT_SHAKECAM), world units
    void BuildFrame(RFrame& f);     // camera.W/H used as the viewport size
    // ssaa > 1 renders at ssaa x size and box-filters down (screenshots / thumbnails)
    bool RenderPng(const std::wstring& path, int W, int H, std::string& err, int threads = 4, int ssaa = 1);
    bool Render(Image& out, int W, int H, int threads = 4, int ssaa = 1);
    // render a clip of the scene from now: frames every 1000/fps ms (advancing the scene), GIF and/or PNG frames
    bool RecordGif(const std::wstring& path, int W, int H, double ms, int fps, std::string& err,
                   const std::function<void(double dtMs)>& onTick = {});

private:
    struct FxSlot {
        std::unique_ptr<EfpEffect> fx;
        EffectSpec spec;
        bool started = false;
        V3 snapPos;
        bool snapped = false;
    };
    void PlaceEffect(FxSlot& s);
    void TickEffects();
    AssetCache* m_cache;
    std::map<int, std::unique_ptr<ModelInstance>> m_models;
    std::map<int, ModelPlace> m_place;
    std::map<int, FxSlot> m_fx;
    int m_nextModel = 1, m_nextFx = 1;
    double m_time = 0, m_tickAcc = 0;
    double m_shakeAmp = 0, m_shakeUntil = 0;
    std::int64_t m_tick = 0;
};

}  // namespace se::render
