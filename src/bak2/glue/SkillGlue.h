// SroPk2Studio glue (not BAK-2 code): the state behind bak2::SkillDb and one playing skill of
// bak2::View. Private to src/bak2_skill.cpp and src/bak2_view.cpp. The logic is BAK-2's
// gui/gui_object.inc object-page preview (EnsureDb, BuildScene, EquipWeapon, PreviewSkill,
// PreviewTick, FrameView) without any window code, driven by a skill id instead of a selected
// object (see bak2_skill.cpp).
#pragma once

#include "../../bak2_skill.hpp"

#include "../client/Catalog.h"
#include "../render/PlayerDress.h"
#include "../render/RPlayHost.h"
#include "../render/RView.h"
#include "../skillplay/SkillPlay.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct bak2::SkillDb::Impl {
    ReadFn read;
    se::client::Catalog cat;
    se::sp::FxDb db;
    std::vector<std::function<std::wstring()>> problems;   // load remarks, worded when asked (UI language)
    int files = 0;
    std::size_t bytes = 0;

    // lazily built / cached (GUI DressIx, sh.weaponOf, pv.bsr; existence instead of Catalog::CheckFiles)
    mutable se::render::DressIndex dress;
    mutable bool dressBuilt = false;
    mutable std::map<int, int> weaponOf;
    mutable std::map<std::wstring, bool> exists;
    mutable std::map<std::wstring, std::shared_ptr<se::sp::BsrModel>> bsr;

    bool Exists(const std::wstring& dataPath) const;                 // Data inner path readable
    se::render::DressIndex& Dress() const;
    std::shared_ptr<se::sp::BsrModel> ModelInfo(const std::wstring& dataPath) const;
    se::sp::SkillInfo ToSkillInfo(const se::client::CatSkill& cs) const;
    std::wstring KeyOf(const se::client::CatSkill& cs) const;
    std::wstring Group(const se::client::CatSkill& cs) const;
};

namespace bak2::glue {

// one playing skill (GUI pv.* for one preview)
struct SkillSession {
    int skillId = 0;
    std::wstring code, title, key;
    bool isPlayer = false;
    int race = 0, mastery = 0, wtype = 0;
    std::wstring wgroup;
    int caster = -1, target = -1;
    double dist = 60;
    std::wstring casterCode, targetCode, casterModel, targetModel;
    std::vector<std::wstring> notes;   // what was chosen / what is missing (UI language at start)
    se::sp::Timeline tl;               // rows / aniSet cleared after start (they point into the FxDb)
    std::wstring dump;                 // tl.Dump() taken at start (notes in the UI language)
    std::shared_ptr<se::sp::BsrModel> casterInfo, targetInfo;
    std::unique_ptr<se::render::ScenePlayHost> host;
    se::sp::SkillPlayer player;
    double now = 0, restartAt = -1;
    bool running = false;
    bool loop = true;
    std::uint32_t seed = 1;
    std::function<void(const std::wstring& dataPath, int volume)> onSound;
};

// GUI BuildScene + PreviewSkill: caster (player: default body of the race, outfit, the weapon the
// skill needs), target monster, timeline, ScenePlayHost, player started. scene = a fresh scene.
bool StartSkill(const SkillDb::Impl& db, int skillId, se::render::Scene& scene, se::render::AssetCache& cache,
                SkillSession& s, std::wstring& err);
// GUI PreviewTick (host update, player update, replay 700 ms after the end when looping)
void TickSkill(SkillSession& s, se::render::Scene& scene, double dtMs);
// GUI PreviewStop
void StopSkill(SkillSession& s);
// GUI FrameView (players: PlayerDress FramePlayer over the skill's clips; else FrameModels)
void FrameSkill(SkillSession& s, se::render::Scene& scene, se::render::PreviewView& view);
// GUI PreviewTick overlay lines (UI language)
std::vector<std::wstring> SkillOverlay(const SkillSession& s, const se::render::Scene& scene);

}  // namespace bak2::glue
