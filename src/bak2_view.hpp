#pragma once
// Bridge to the vendored "SkillEffect Studio" (BAK-2) renderer (src/bak2/render,
// namespace se::render): se::render::PreviewView (OpenGL child window with the
// CPU rasterizer as fallback) + Scene + AssetCache behind a pimpl. The header
// carries std types and HWND only; no BAK-2 header leaks into app code.
//
// Effects run the exact .efp runtime (20 Hz ticks, bit-exact vs efp_sim);
// models are .bsr / .cpd compositions (skinned, idle clip looping) or a lone
// .bms. Everything is read through the ReadFn callback; nothing is written.
// Single-threaded: call every member from the thread that owns the parent
// window (the view animates on its own WM_TIMER while playing).
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

namespace bak2 {

// Reads one asset. innerPath is relative to the archive root, lower case, '/'
// separated (e.g. "res/mob/china/tiger.bsr", "skill/europe/x.efp"). The app
// resolves it against its archive and the sibling archives (Data / Particles /
// Media). Returns false when the file is missing.
using ReadFn = std::function<bool(const std::wstring& innerPath, std::vector<std::uint8_t>& out)>;

class SkillDb;   // bak2_skill.hpp

class View {
public:
    View();
    ~View();
    View(const View&) = delete;
    View& operator=(const View&) = delete;

    // Creates the child window inside parent; it stays hidden until show(true).
    bool create(HWND parent, int x, int y, int w, int h);
    void move(int x, int y, int w, int h);
    void show(bool visible);
    HWND hwnd() const;
    bool using_gl() const;                    // false = CPU rasterizer fallback

    // Asset source for every later load (drops the loaded item and the cache).
    void set_source(ReadFn read);

    // .efp: looping (SetHold), camera fitted to the particle bounds.
    bool load_effect(const std::wstring& efpInnerPath, std::wstring& err);
    // .bsr / .cpd / .bms: idle (STAND1, else the first) clip looping, framed.
    bool load_model(const std::wstring& innerPath, std::wstring& err);
    // .efp bytes (e.g. bak2::EfpEditor::build): played like load_effect. innerPathForRefs = where the
    // effect lives in Particles (relative texture / mesh references resolve next to it). extraFiles =
    // resources the bytes reference that the archives do not have yet (EfpEditor::build_files'
    // recoloured textures), by inner path; they shadow the archives while this effect is loaded.
    bool load_effect_bytes(const std::vector<std::uint8_t>& efp, const std::wstring& innerPathForRefs, std::wstring& err);
    bool load_effect_bytes(const std::vector<std::uint8_t>& efp, const std::wstring& innerPathForRefs,
                           const std::vector<std::pair<std::wstring, std::vector<std::uint8_t>>>& extraFiles, std::wstring& err);
    // A skill as the client plays it (BAK-2 skillplay: READY / WAIT / SHOT clips, effects on bones,
    // projectiles, hits, sounds, camera shake). Caster: player skills = the race's default body in
    // the outfit of the skill's build holding the weapon the skill needs (BAK-2 PlayerDress); other
    // skills = the monster / NPC that uses it. Target = a common monster at the client's distance.
    // Replays 0.7 s after the end while looping (set_skill_loop). Models / effects are read through
    // set_source; db is only used here (it may be destroyed afterwards).
    bool play_skill(const SkillDb& db, int skillId, std::wstring& err);
    void set_skill_loop(bool loop);           // default true
    // Called for every skill sound (effectsound / SndBegin / SndEnd): Data inner path in the ReadFn
    // form ("prim/snd/..."), volume 0..100. The view plays nothing itself.
    void set_sound_callback(std::function<void(const std::wstring& dataPath, int volume)> cb);
    void clear();

    void set_playing(bool p);
    bool playing() const;
    void step(double ms);                     // advance while paused (frame stepping)
    void frame();                             // re-fit the camera to what is loaded
    // BAK-2's GDI text over the 3D view (top-left file / skill lines, bottom status + fps line).
    // Default false: the app shows its own UI.
    void set_overlay(bool visible);

    // Readable text: node table of an effect, composition / clips of a model, timeline of a skill.
    // Texts (here, in errors and in the view) follow the UI language (pk2::L) when they are made.
    std::wstring describe() const;

    // Render quality actually in use (GL path): MSAA samples (0 = none), anisotropic filtering
    // level (0 = none), hardware mipmap generation. The CPU fallback renders 2x supersampled.
    int gl_samples() const;
    float gl_anisotropy() const;
    bool gl_hw_mipmaps() const;

    // Renders the current frame and reads it back (top-down rows, 4 bytes per
    // pixel B,G,R,A with A = 255).
    bool capture(std::vector<std::uint8_t>& bgra, int& w, int& h);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace bak2
