// bak2::View - glue between the app and the vendored BAK-2 renderer (see bak2_view.hpp).
#include "bak2_view.hpp"

#include "bak2_skill.hpp"
#include "i18n.hpp"
#include "bak2/glue/SkillGlue.h"
#include "bak2/glue/TextMap.h"
#include "bak2/render/RView.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <format>
#include <map>
#include <utility>

namespace bak2 {

namespace R = se::render;

namespace {

// BAK-2 convention: '\' separated, lower case, ".." resolved, no leading separator.
std::wstring Key(const std::wstring& p) { return R::LowerW(R::NormPath(p)); }

std::wstring Slashed(std::wstring s) {
    std::replace(s.begin(), s.end(), L'\\', L'/');
    return s;
}

bool EndsWith(const std::wstring& s, const wchar_t* suffix) {
    const std::size_t n = std::wcslen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::wstring BaseName(const std::wstring& p) {
    const std::size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(k + 1);
}

std::wstring Stem(const std::wstring& p) {
    std::wstring b = BaseName(p);
    const std::size_t dot = b.find_last_of(L'.');
    return dot == std::wstring::npos ? b : b.substr(0, dot);
}

// One leading archive-name component ("data\", "media\", "particles\") removed: references written
// relative to the client folder instead of the archive root.
std::wstring StripArchiveRoot(const std::wstring& key) {
    for (const wchar_t* root : {L"data\\", L"media\\", L"particles\\"}) {
        const std::size_t n = std::wcslen(root);
        if (key.size() > n && key.compare(0, n, root) == 0) return key.substr(n);
    }
    return key;
}

// UTF-16 -> CP949 (the code page the JMXV formats store their paths in).
std::string ToCp949(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(949, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(949, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// Minimal JMXVRES 0109 resource for a lone .bms: the layout R::ParseBsr reads (12-byte signature,
// 13 section offsets, type, category, name, 2 dwords), one material set (when a .bmt is known) and
// one mesh. No skeleton / animations: the mesh is drawn in its bind pose.
std::vector<std::uint8_t> MakeBsr(const std::wstring& name, const std::wstring& bms, const std::wstring& bmt) {
    std::vector<std::uint8_t> b;
    auto u16 = [&b](std::uint16_t v) { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v >> 8)); };
    auto u32 = [&b](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto str = [&](const std::wstring& w) {
        const std::string s = ToCp949(w);
        u32(static_cast<std::uint32_t>(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    };
    auto patch = [&b](std::size_t at, std::size_t value) {
        const auto v = static_cast<std::uint32_t>(value);
        for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    static constexpr char kSig[] = "JMXVRES 0109";
    b.insert(b.end(), kSig, kSig + 12);
    const std::size_t offsets = b.size();
    for (int i = 0; i < 13; ++i) u32(0);
    u16(0);
    u16(0);
    str(name);
    u32(0);
    u32(0);
    if (!bmt.empty()) {
        patch(offsets + 0 * 4, b.size());   // material sets
        u32(1);
        u32(0);
        str(bmt);
    }
    patch(offsets + 1 * 4, b.size());       // meshes (mesh flag 0: no dword after each path)
    u32(1);
    str(bms);
    return b;
}

// IAssetSource over the app's ReadFn.
class Source final : public R::IAssetSource {
public:
    ReadFn read;
    std::wstring effectDir;                                    // folder of the loaded .efp (Key form)
    std::map<std::wstring, std::vector<std::uint8_t>> virt;    // synthesized resources (Key -> bytes)

    bool Read(R::Arc arc, const std::wstring& innerPath, std::vector<std::uint8_t>& out) override {
        out.clear();
        const std::wstring key = Key(innerPath);
        if (key.empty()) return false;
        if (auto it = virt.find(key); it != virt.end()) {
            out = it->second;
            return true;
        }
        if (!read) return false;
        for (const std::wstring& c : Candidates(arc, key)) {
            if (read(Slashed(c), out) && !out.empty()) return true;
            out.clear();
        }
        return false;
    }

private:
    // The reference as written, without a leading archive-name component, relative to the effect's
    // folder (effect textures / meshes), and finally under the archive's folder name (an extracted
    // client opened as one folder).
    std::vector<std::wstring> Candidates(R::Arc arc, const std::wstring& key) const {
        std::vector<std::wstring> out;
        auto add = [&out](const std::wstring& p) {
            if (!p.empty() && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
        };
        add(key);
        const std::wstring rel = StripArchiveRoot(key);
        add(rel);
        if (arc == R::Arc::Particles && !effectDir.empty() && rel.find(L'\\') == std::wstring::npos)
            add(Key(R::JoinPath(effectDir, rel)));
        static constexpr const wchar_t* kFolder[] = {L"data\\", L"particles\\", L"media\\"};   // R::Arc order
        const int a = static_cast<int>(arc);
        if (a >= 0 && a < 3) add(kFolder[a] + rel);
        return out;
    }
};

std::wstring W(const std::string& cp949) { return R::Cp949ToW(cp949); }

std::wstring Num(double v) { return std::format(L"{:.1f}", v); }

std::wstring V3Text(const R::V3& v) { return L"(" + Num(v.x) + L", " + Num(v.y) + L", " + Num(v.z) + L")"; }

}  // namespace

struct View::Impl {
    enum class Kind { None, Effect, Model, Skill };

    Source src;
    R::AssetCache cache{&src};
    std::unique_ptr<R::Scene> scene;
    std::unique_ptr<glue::SkillSession> skill;    // after scene: released first (its host drives the scene)
    R::PreviewView view;   // after scene: destroyed first, so the window never paints a dead scene

    Kind kind = Kind::None;
    std::wstring shownPath;                       // what the user opened (inner path, BAK-2 form)
    int fxId = -1, modelId = -1;
    std::shared_ptr<const R::EfpFile> efp;
    bool fxBox = false;
    R::V3 fxMin, fxMax;
    bool overlay = false;
    bool skillLoop = true;
    std::function<void(const std::wstring&, int)> soundCb;
    unsigned virtualSerial = 0;

    ~Impl() {
        view.SetOnTick(nullptr);
        view.SetOnFrame(nullptr);
        if (skill) glue::StopSkill(*skill);
        skill.reset();
    }

    // BAK-2's own view texts in the current language: the empty-view line, and the status line language
    // (TR / EN in BAK-2) of the overlay.
    void Texts() {
        view.english = glue::Bak2English();
        if (view.Hwnd()) view.SetEmptyText(pk2::L("b2_nothing_loaded"));
    }

    void Unload() {
        view.SetOnTick(nullptr);
        view.SetOnFrame(nullptr);
        if (skill) glue::StopSkill(*skill);
        skill.reset();
        view.SetScene(nullptr);
        view.SetOverlay({});
        Texts();
        scene.reset();
        kind = Kind::None;
        shownPath.clear();
        fxId = modelId = -1;
        efp.reset();
        fxBox = false;
        src.effectDir.clear();
        src.virt.clear();
        cache.Trim(256u << 20);
    }

    void NewScene() {
        Unload();
        scene = std::make_unique<R::Scene>(&cache);
        scene->opt.grid = true;
        scene->opt.shadow = true;
    }

    void Show() {
        view.SetScene(scene.get());
        view.SetOverlay({shownPath});
        Frame();
    }

    // Effect: Scene::FrameAll's camera fit over the particle bounds of a private copy of the effect
    // (same file, seed, scale and hold as the scene's) sampled at 0.3 / 0.6 / 0.9 / 1.2 s like
    // EfxThumb's pass 1, with the effect preview parameters of BAK-2's editor (yaw 215, pitch 20,
    // distance x2.2). Models: PreviewView::FrameModels (Scene::FrameAll at the current orbit).
    void Frame() {
        if (!scene) return;
        if (kind == Kind::Skill && skill) {
            glue::FrameSkill(*skill, *scene, view);
            return;
        }
        if (kind == Kind::Effect && fxBox) {
            const R::V3 c = (fxMin + fxMax) * 0.5;
            const double r = std::max(R::Len(fxMax - fxMin) * 0.5, 4.0);
            view.SetCamera(c, 215, 20, r * 2.2);
            return;
        }
        view.FrameModels();
    }

    void FitEffect(std::uint32_t seed) {
        fxBox = false;
        if (!efp) return;
        R::EfpEffect fx(efp, 1.0, 255, seed);
        fx.SetHold(true);
        const R::V3 origin{0, 0, 0};
        for (int tick = 1; tick <= 24; ++tick) {
            fx.SetWorld(origin, nullptr);
            if (!fx.Update()) break;
            if (tick % 6 != 0) continue;
            R::V3 a, b;
            if (!fx.Bounds(a, b)) continue;
            if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z) || !std::isfinite(b.x) ||
                !std::isfinite(b.y) || !std::isfinite(b.z))
                continue;
            if (!fxBox) {
                fxMin = a;
                fxMax = b;
                fxBox = true;
            } else {
                fxMin = {std::min(fxMin.x, a.x), std::min(fxMin.y, a.y), std::min(fxMin.z, a.z)};
                fxMax = {std::max(fxMax.x, b.x), std::max(fxMax.y, b.y), std::max(fxMax.z, b.z)};
            }
        }
    }

    // Effect looping at the origin (scene from NewScene). shown = what describe() / the overlay name,
    // path = what the asset cache loads (the same, or a virtual file for edited bytes).
    bool StartEffect(const std::wstring& shown, const std::wstring& path, std::wstring& err) {
        src.effectDir = Key(R::DirOf(shown));
        shownPath = shown;
        std::string e;
        efp = cache.Efp(path, &e);
        if (!efp) {
            err = pk2::Lf("b2_err_fx_read", shown, W(e));
            Unload();
            return false;
        }
        R::EffectSpec es;
        es.path = path;
        es.hold = true;                    // SetHold(1): loops by reviving its root particles
        es.worldPos = {0, 0, 0};
        fxId = scene->AddEffect(es, &e);
        if (fxId < 0) {
            err = pk2::Lf("b2_err_fx_start", shown, W(e));
            Unload();
            return false;
        }
        kind = Kind::Effect;
        FitEffect(static_cast<std::uint32_t>(1 + fxId));   // Scene::AddEffect's automatic seed
        Show();
        return true;
    }

    std::wstring DescribeSkill() const {
        std::wstring s;
        if (!skill) return s;
        const glue::SkillSession& k = *skill;
        s += pk2::Lf("b2_sk_title", k.title, k.code, k.skillId) + L"\n";
        s += pk2::Lf("b2_sk_key", k.key) + L"\n";
        s += pk2::Lf("b2_sk_caster", k.casterCode.empty() ? pk2::L("b2_sk_virtual") : k.casterCode,
                     k.casterModel.empty() ? std::wstring() : L"  " + k.casterModel, k.wgroup.empty() ? std::wstring() : L"  [" + k.wgroup + L"]") +
             L"\n";
        if (!k.targetCode.empty()) s += pk2::Lf("b2_sk_target", k.targetCode, k.targetModel, k.dist) + L"\n";
        if (scene)
            s += pk2::Lf("b2_sk_time", std::min(k.now, k.tl.totalMs), k.tl.totalMs, scene->EffectCount(), scene->ParticleCount()) + L"\n";
        for (const auto& n : k.notes) s += L"  · " + n + L"\n";
        s += k.dump;
        return s;
    }

    // A lone .bms becomes a one-mesh resource; its .bmt is looked up by the file stem and by the mesh's
    // material name, next to it and in the matching prim\mtrl folder (prim\mesh\a\b.bms -> prim\mtrl\a\).
    std::wstring VirtualBsr(const std::wstring& bms) {
        const std::wstring dir = R::DirOf(bms), stem = Stem(bms);
        std::vector<std::wstring> dirs{dir};
        const std::wstring low = R::LowerW(dir) + L"\\";
        if (const std::size_t k = low.find(L"\\mesh\\"); k != std::wstring::npos || low.rfind(L"mesh\\", 0) == 0) {
            const std::size_t at = k != std::wstring::npos ? k + 1 : 0;
            std::wstring mtrl = dir + L"\\";
            mtrl.replace(at, 4, L"mtrl");
            dirs.push_back(R::NormPath(mtrl));
        }
        std::vector<std::wstring> names{stem};
        if (const auto mesh = cache.Bms(R::Arc::Data, bms); mesh && !mesh->material.empty() && R::LowerW(mesh->material) != R::LowerW(stem))
            names.push_back(mesh->material);
        if (const std::size_t cut = stem.find_last_of(L'_'); cut != std::wstring::npos && cut > 0)
            names.push_back(stem.substr(0, cut));   // tiger_01.bms / tiger_part1.bms -> tiger.bmt
        std::vector<std::wstring> bmts;
        for (const auto& d : dirs)
            for (const auto& n : names) bmts.push_back(R::JoinPath(d, n + L".bmt"));
        std::wstring bmt;
        for (const auto& c : bmts) {
            std::vector<std::uint8_t> probe;
            if (src.Read(R::Arc::Data, c, probe) && probe.size() >= 7 && std::memcmp(probe.data(), "JMXVBMT", 7) == 0) {
                bmt = c;
                break;
            }
        }
        const std::wstring path = std::format(L"bak2-virtual\\{}\\{}.bsr", ++virtualSerial, stem);
        src.virt[Key(path)] = MakeBsr(stem, bms, bmt);
        return path;
    }

    std::wstring DescribeEffect() const {
        std::wstring s;
        const auto nodes = R::DescribeEffect(*efp);
        s += pk2::Lf("b2_fx_title", shownPath) + L"\n";
        s += pk2::Lf("b2_fx_header", W(efp->version), nodes.size(), efp->hasRootScale ? efp->rootScale : 1.0f) + L"\n";
        if (scene) s += pk2::Lf("b2_fx_live", scene->EffectCount(), scene->ParticleCount(), scene->TimeMs() / 1000.0) + L"\n";
        if (fxBox) s += pk2::Lf("b2_fx_bounds", V3Text(fxMin), V3Text(fxMax)) + L"\n";
        for (const auto& n : nodes) {
            const std::wstring ind(static_cast<std::size_t>(n.depth) * 2, L' ');
            s += std::format(L"{}#{} {}  {} / {} / {}  L={}\n", ind, n.index, W(n.name), W(n.render), W(n.view), W(n.life),
                             n.lifeFrames);
            if (!n.tex.empty() || !n.mesh.empty())
                s += std::format(L"{}   tex={}{}{}\n", ind, W(n.tex), n.mesh.empty() ? L"" : L"  mesh=", W(n.mesh));
            s += std::format(L"{}   blend {}/{}  {} {}/{}  cull {}\n", ind, W(R::BlendName(n.src)), W(R::BlendName(n.dst)),
                             pk2::L("b2_fx_stage_op"), W(R::TopName(n.cs[2])), W(R::TopName(n.as[2])), n.cull);
            if (n.hasEmit)
                s += std::format(L"{}   emit start={} {}={} interval={} max={} {}={:.2f}\n", ind, n.emit.start,
                                 pk2::L("b2_fx_duration"), n.emit.duration, n.emit.interval, n.emit.maxAlive,
                                 pk2::L("b2_fx_per_event"), n.emit.perEvent);
            if (n.linkTrans || n.linkOrbit || n.linkRot || n.linkVel)
                s += std::format(L"{}   link trans/orbit/rot/vel {}/{}/{}/{}\n", ind, n.linkTrans, n.linkOrbit, n.linkRot,
                                 n.linkVel);
            if (!n.commands.empty()) {
                std::wstring cmds;
                for (const auto& c : n.commands) cmds += (cmds.empty() ? L"" : L", ") + W(c);
                s += std::format(L"{}   {} ({}): {}\n", ind, pk2::L("b2_fx_program"), n.ops, cmds);
            }
        }
        return s;
    }

    std::wstring DescribeModel() const {
        std::wstring s;
        R::ModelInstance* m = scene ? scene->Model(modelId) : nullptr;
        if (!m) return s;
        const auto def = m->Def();
        s += pk2::Lf("b2_model_title", def->name) + L"\n";
        s += pk2::Lf("b2_model_file", shownPath) + L"\n";
        for (std::size_t i = 0; i < def->bsrPaths.size(); ++i)
            if (def->bsrPaths[i].rfind(L"bak2-virtual\\", 0) != 0) s += L"  bsr: " + def->bsrPaths[i] + L"\n";
        if (def->HasSkeleton()) s += pk2::Lf("b2_model_skeleton", def->skel->bones.size()) + L"\n";
        else s += pk2::L("b2_model_no_skeleton") + L"\n";
        s += pk2::Lf("b2_model_parts", def->parts.size()) + L"\n";
        static constexpr const wchar_t* kBlend[] = {L"opaque", L"cutout", L"blend", L"additive"};
        for (const auto& p : def->parts) {
            s += L"  " + p.meshPath;
            if (!p.materialName.empty()) s += L"  [" + p.materialName + L"]";
            s += L"  -> " + (p.texturePath.empty() ? pk2::L("b2_model_no_texture") : p.texturePath);
            s += std::format(L"  {}{}\n", kBlend[static_cast<int>(p.blend)], p.attachBone >= 0 ? L"  (rigid)" : L"");
        }
        const auto clips = def->ClipList();
        s += pk2::Lf("b2_model_anims", clips.size()) + L"\n";
        for (const auto& [group, type] : clips) {
            const std::wstring name = R::AniTypeName(type);
            s += std::format(L"  {}: {} (0x{:02X})\n", group, name.empty() ? L"?" : name, type);
        }
        if (const R::ModelClip* c = m->Current())
            s += pk2::Lf("b2_model_playing", R::AniTypeName(c->typeId).empty() ? std::to_wstring(c->typeId) : R::AniTypeName(c->typeId),
                         c->group.empty() ? std::wstring(L"-") : c->group, c->lengthMs / 1000.0, c->banPath) +
                 L"\n";
        R::V3 a, b;
        m->Bounds(a, b);
        s += pk2::Lf("b2_model_bounds", V3Text(a), V3Text(b), Num(def->Radius())) + L"\n";
        if (!def->warnings.empty()) {
            s += pk2::L("b2_model_warnings") + L"\n";
            for (const auto& w : def->warnings) s += L"  " + glue::Localize(w, glue::kRenderTexts) + L"\n";
        }
        return s;
    }
};

View::View() : p_(std::make_unique<Impl>()) {}
View::~View() = default;

bool View::create(HWND parent, int x, int y, int w, int h) {
    if (p_->view.Hwnd()) return true;
    if (!p_->view.Create(parent, x, y, w, h)) return false;
    ShowWindow(p_->view.Hwnd(), SW_HIDE);
    p_->view.showOverlay = p_->overlay;
    p_->Texts();
    return true;
}

void View::move(int x, int y, int w, int h) { p_->view.Move(x, y, w, h); }

void View::show(bool visible) {
    if (visible) p_->Texts();   // the UI language may have changed while hidden
    if (HWND h = p_->view.Hwnd()) ShowWindow(h, visible ? SW_SHOWNA : SW_HIDE);
}

HWND View::hwnd() const { return p_->view.Hwnd(); }
bool View::using_gl() const { return p_->view.UsingGl(); }

void View::set_source(ReadFn read) {
    p_->Unload();
    p_->cache.Clear();
    p_->src.read = std::move(read);
}

bool View::load_effect(const std::wstring& efpInnerPath, std::wstring& err) {
    err.clear();
    Impl& m = *p_;
    const std::wstring inner = R::NormPath(efpInnerPath);
    m.NewScene();
    return m.StartEffect(inner, inner, err);
}

bool View::load_effect_bytes(const std::vector<std::uint8_t>& efp, const std::wstring& innerPathForRefs, std::wstring& err) {
    return load_effect_bytes(efp, innerPathForRefs, {}, err);
}

bool View::load_effect_bytes(const std::vector<std::uint8_t>& efp, const std::wstring& innerPathForRefs,
                             const std::vector<std::pair<std::wstring, std::vector<std::uint8_t>>>& extraFiles, std::wstring& err) {
    err.clear();
    Impl& m = *p_;
    if (efp.empty()) {
        err = pk2::L("b2_err_no_fx_bytes");
        return false;
    }
    const std::wstring inner = R::NormPath(innerPathForRefs.empty() ? std::wstring(L"bak2_edit.efp") : innerPathForRefs);
    m.NewScene();
    // a new name next to the original for every load: the asset cache keys by path, so an earlier load of the
    // original (or of a previous edit) is never reused; extra files (recoloured texture copies, new names each
    // edit) replace what the cache may hold under their names
    const std::wstring dir = R::DirOf(inner);
    const std::wstring loadPath = R::JoinPath(dir, std::format(L"{}.bak2edit{}.efp", Stem(inner), ++m.virtualSerial));
    m.src.virt[Key(loadPath)] = efp;
    if (!extraFiles.empty()) {
        m.cache.Clear();
        for (const auto& [path, bytes] : extraFiles) m.src.virt[Key(path)] = bytes;
    }
    return m.StartEffect(inner, loadPath, err);
}

bool View::load_model(const std::wstring& innerPath, std::wstring& err) {
    err.clear();
    Impl& m = *p_;
    const std::wstring inner = R::NormPath(innerPath);
    const std::wstring low = R::LowerW(inner);
    if (!EndsWith(low, L".bsr") && !EndsWith(low, L".cpd") && !EndsWith(low, L".bms")) {
        err = pk2::Lf("b2_err_not_model", inner);
        return false;
    }
    m.NewScene();
    m.shownPath = inner;
    const std::wstring path = EndsWith(low, L".bms") ? m.VirtualBsr(inner) : inner;
    R::ModelPlace place;               // idle: STAND1 looped, else the first clip
    std::wstring e;
    m.modelId = m.scene->AddModel(path, place, &e);
    if (m.modelId < 0) {
        err = pk2::Lf("b2_err_model_load", inner) + (e.empty() ? L"" : L" (" + glue::Localize(e, glue::kRenderTexts) + L")");
        m.Unload();
        return false;
    }
    m.kind = Impl::Kind::Model;
    m.Show();
    return true;
}

bool View::play_skill(const SkillDb& db, int skillId, std::wstring& err) {
    err.clear();
    Impl& m = *p_;
    m.NewScene();
    auto s = std::make_unique<glue::SkillSession>();
    s->loop = m.skillLoop;
    Impl* self = &m;
    s->onSound = [self](const std::wstring& path, int volume) {
        if (self->soundCb) self->soundCb(path, volume);
    };
    if (!glue::StartSkill(*db.p_, skillId, *m.scene, m.cache, *s, err)) {
        m.Unload();
        return false;
    }
    m.skill = std::move(s);
    m.kind = Impl::Kind::Skill;
    m.shownPath = m.skill->title + L"  ·  " + m.skill->key;
    m.view.SetScene(m.scene.get());
    m.view.SetOverlay({m.shownPath});
    // GUI: pv.view.SetOnTick(PreviewTick) - runs before every Scene::Advance (timer and step())
    m.view.SetOnTick([self](double dt) {
        if (!self->skill || !self->scene) return;
        glue::TickSkill(*self->skill, *self->scene, dt);
        if (self->overlay) self->view.SetOverlay(glue::SkillOverlay(*self->skill, *self->scene));
    });
    m.view.SetOnFrame([self] { self->Frame(); });
    m.Frame();
    m.view.SetPlaying(true);
    return true;
}

void View::set_skill_loop(bool loop) {
    p_->skillLoop = loop;
    if (p_->skill) p_->skill->loop = loop;
}

void View::set_sound_callback(std::function<void(const std::wstring& dataPath, int volume)> cb) { p_->soundCb = std::move(cb); }

void View::set_overlay(bool visible) {
    p_->overlay = visible;
    p_->view.showOverlay = visible;
    p_->Texts();
    p_->view.Redraw();
}

int View::gl_samples() const { return p_->view.GlSamples(); }
float View::gl_anisotropy() const { return p_->view.GlAnisotropy(); }
bool View::gl_hw_mipmaps() const { return p_->view.GlHwMipmaps(); }

void View::clear() { p_->Unload(); }

void View::set_playing(bool p) { p_->view.SetPlaying(p); }
bool View::playing() const { return p_->view.Playing(); }
void View::step(double ms) { p_->view.Step(ms); }
void View::frame() { p_->Frame(); }

std::wstring View::describe() const {
    switch (p_->kind) {
        case Impl::Kind::Effect: return p_->DescribeEffect();
        case Impl::Kind::Model: return p_->DescribeModel();
        case Impl::Kind::Skill: return p_->DescribeSkill();
        case Impl::Kind::None: break;
    }
    return {};
}

bool View::capture(std::vector<std::uint8_t>& bgra, int& w, int& h) {
    R::Image img;
    if (!p_->view.Capture(img) || img.Empty()) return false;
    w = img.w;
    h = img.h;
    bgra.resize(img.rgba.size());
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        bgra[i] = img.rgba[i + 2];
        bgra[i + 1] = img.rgba[i + 1];
        bgra[i + 2] = img.rgba[i];
        bgra[i + 3] = 255;
    }
    return true;
}

}  // namespace bak2
