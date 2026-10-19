// bak2::EfpEditor - glue between the app and the vendored BAK-2 efpwrite module (see bak2_edit.hpp).
#include "bak2_edit.hpp"

#include "i18n.hpp"
#include "bak2/efpwrite/EfpVariant.h"
#include "bak2/glue/TextMap.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <format>
#include <map>
#include <utility>

namespace bak2 {

namespace W = se::efpw;

namespace {

// efpwrite reads Particles inner paths ('\' separated); the app's ReadFn takes lower case '/' paths.
std::wstring AppPath(std::wstring p) {
    for (auto& c : p) c = c == L'\\' ? L'/' : static_cast<wchar_t>(std::towlower(c));
    return p;
}

std::wstring BackSlashed(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    return p;
}

std::string Lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// efpwrite notes are UTF-8 (EfpVariant Utf8()).
std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// efpwrite errors and notes (EfpVariant.cpp, EfpEdit.cpp, EfpDoc.cpp): BAK-2 writes them in English only.
// Parser notes about unknown blocks (offsets / sizes) stay as written.
constexpr glue::TextMap kEfpwTexts[] = {
    {L"parse: {}", "b2_efp_parse"},
    {L"tag must be plain ASCII without spaces / path characters", "b2_efpw_tag"},
    {L"node index out of range", "b2_efpw_node_range"},
    {L"scale must be > 0", "b2_efpw_scale"},
    {L"self-check: {}", "b2_efpw_selfcheck"},
    {L"render reader refuses the result: {}", "b2_efpw_render"},
    {L"the new .efp needs a new name (same as the source)", "b2_efpw_same_name"},
    {L"name clash", "b2_efpw_name_clash"},
    {L"swap target not in the client (pack it too): {}", "b2_efpw_swap_missing"},
    {L"texture missing, kept: {}", "b2_efpw_tex_missing"},
    {L"texture not recoloured ({}): {}", "b2_efpw_tex_failed"},
    {L"texture unchanged by the colour edit (grey / black), kept: {}", "b2_efpw_tex_grey"},
    {L"texture name not representable in CP949: {}", "b2_efpw_tex_name"},
    {L"{}: matrix is not a pure rotation, kept", "b2_efpw_rotation"},
    {L"node '{}': birth SetGraphDiffuse added", "b2_efpw_birth_diffuse"},
    {L"node '{}': per-tick SetGraphDiffuse added", "b2_efpw_tick_diffuse"},
    {L"node '{}': birth SetGraphScale added", "b2_efpw_birth_scale"},
    {L"header {} -> 0012 (root scale added)", "b2_efpw_header"},
    {L"{} trailing bytes kept raw", "b2_efpw_trailing"},
};

std::wstring EfpwText(const std::wstring& s) { return glue::Localize(s, kEfpwTexts); }

}  // namespace

struct EfpEditor::Impl {
    W::Bytes original;
    W::Doc doc;                                    // the parsed original (listing / summary)
    std::wstring inner;
    ReadFn read;
    Params params;
    std::vector<std::string> texOrig, meshOrig;    // W::ListResources order, raw CP949
    std::map<std::string, std::string> texSwap, meshSwap;   // original raw path -> new raw path
    mutable std::vector<std::string> lastNotes;

    W::ReadFn Reader() const {
        if (!read) return {};
        return [r = read](const std::wstring& particlesInner, W::Bytes& out) { return r(AppPath(particlesInner), out); };
    }

    bool ColorIdentity() const {
        return !params.colorize && params.hue == 0 && params.saturation == 1 && params.brightness == 1;
    }

    W::VariantSpec Spec() const {
        W::VariantSpec s;
        s.nodes = params.nodes;
        s.color.hueShift = params.hue;
        s.color.saturation = params.saturation;
        s.color.brightness = params.brightness;
        s.color.colorize = params.colorize;
        s.addMissingColor = params.add_missing_color;
        s.recolorTextures = params.recolor_textures;
        s.scale = params.scale;
        s.particleSize = params.particle_size;
        s.speed = params.speed;
        s.duration = params.duration;
        for (const auto& [from, to] : texSwap) s.textureSwap.emplace_back(from, to);
        for (const auto& [from, to] : meshSwap) s.meshSwap.emplace_back(from, to);
        return s;   // tag "" = efpwrite AutoTag (names the recoloured texture copies)
    }

    bool Make(W::VariantResult& res, std::wstring& err) const {
        const std::wstring name = inner.empty() ? std::wstring(L"bak2_edit.efp") : BackSlashed(inner);
        const bool ok = W::MakeVariantFromBytes(original, name, Spec(), Reader(), res, err);
        if (!err.empty()) err = EfpwText(err);
        lastNotes = res.report.notes;
        lastNotes.insert(lastNotes.end(), res.notes.begin(), res.notes.end());
        return ok && !res.files.empty();
    }

    static std::vector<std::wstring> Current(const std::vector<std::string>& orig, const std::map<std::string, std::string>& swap) {
        std::vector<std::wstring> out;
        for (const auto& o : orig) {
            const auto it = swap.find(o);
            out.push_back(W::Cp949ToW(it == swap.end() ? o : it->second));
        }
        return out;
    }

    static bool Replace(const std::vector<std::string>& orig, std::map<std::string, std::string>& swap, std::size_t i,
                        const std::wstring& newRef, std::wstring& err) {
        if (i >= orig.size()) {
            err = pk2::Lf("b2_efp_ref_range", i, orig.size());
            return false;
        }
        const std::wstring ref = BackSlashed(newRef);
        if (ref.empty()) {
            err = pk2::L("b2_efp_ref_empty");
            return false;
        }
        const std::string raw = W::WToCp949(ref);
        if (raw.empty() || W::Cp949ToW(raw) != ref) {
            err = pk2::Lf("b2_efp_ref_cp949", ref);
            return false;
        }
        if (Lower(raw) == Lower(orig[i])) swap.erase(orig[i]);
        else swap[orig[i]] = raw;
        return true;
    }
};

EfpEditor::EfpEditor() : p_(std::make_unique<Impl>()) {}
EfpEditor::~EfpEditor() = default;

std::unique_ptr<EfpEditor> EfpEditor::open(const std::vector<std::uint8_t>& efp, std::wstring& err, const std::wstring& innerPath,
                                           ReadFn read) {
    err.clear();
    std::unique_ptr<EfpEditor> e(new EfpEditor());
    Impl& m = *e->p_;
    std::string pe;
    if (!W::Parse(efp, m.doc, pe)) {
        err = pk2::Lf("b2_efp_parse", W::Cp949ToW(pe));
        return nullptr;
    }
    m.original = efp;
    m.inner = innerPath;
    m.read = std::move(read);
    for (const W::ResUse& u : W::ListResources(m.doc)) (u.mesh ? m.meshOrig : m.texOrig).push_back(u.path);
    return e;
}

void EfpEditor::set_params(const Params& p) { p_->params = p; }
EfpEditor::Params EfpEditor::params() const { return p_->params; }

std::vector<std::wstring> EfpEditor::textures() const { return Impl::Current(p_->texOrig, p_->texSwap); }
std::vector<std::wstring> EfpEditor::meshes() const { return Impl::Current(p_->meshOrig, p_->meshSwap); }

bool EfpEditor::replace_texture(std::size_t i, const std::wstring& newRef, std::wstring& err) {
    err.clear();
    return Impl::Replace(p_->texOrig, p_->texSwap, i, newRef, err);
}

bool EfpEditor::replace_mesh(std::size_t i, const std::wstring& newRef, std::wstring& err) {
    err.clear();
    return Impl::Replace(p_->meshOrig, p_->meshSwap, i, newRef, err);
}

bool EfpEditor::build(std::vector<std::uint8_t>& out, std::wstring& err) const {
    err.clear();
    W::VariantResult res;
    if (!p_->Make(res, err)) return false;
    out = std::move(res.files.front().data);
    return true;
}

bool EfpEditor::build_files(std::vector<OutFile>& files, std::wstring& err) const {
    err.clear();
    files.clear();
    W::VariantResult res;
    if (!p_->Make(res, err)) return false;
    for (auto& f : res.files) files.push_back({f.inner, f.from, std::move(f.data)});
    return true;
}

bool EfpEditor::modified() const {
    const Params& p = p_->params;
    return !p_->ColorIdentity() || p.scale != 1 || p.particle_size != 1 || p.speed != 1 || p.duration != 1 || !p_->texSwap.empty() ||
           !p_->meshSwap.empty();
}

int EfpEditor::node_count() const { return p_->doc.root.CountNodes(); }

std::vector<std::wstring> EfpEditor::notes() const {
    std::vector<std::wstring> out;
    for (const auto& n : p_->doc.notes) out.push_back(EfpwText(W::Cp949ToW(n)));
    for (const auto& n : p_->lastNotes) out.push_back(EfpwText(FromUtf8(n)));
    return out;
}

std::wstring EfpEditor::summary() const {
    const Impl& m = *p_;
    const Params& p = m.params;
    std::wstring s = pk2::Lf("b2_efp_sum_head", W::Cp949ToW(m.doc.version), m.doc.root.CountNodes(), m.doc.RootScale(), m.original.size()) + L"\n";
    for (const W::NodeInfo& n : W::DescribeNodes(m.doc)) {
        const std::wstring ind(static_cast<std::size_t>(n.depth) * 2, L' ');
        s += std::format(L"{}#{} {}  {} / {} / {}  L={}", ind, n.index, W::Cp949ToW(n.name), W::Cp949ToW(n.life), W::Cp949ToW(n.view),
                         W::Cp949ToW(n.render), n.lifeFrames);
        if (n.hasEmit)
            s += std::format(L"  emit {}/{}/{} max {} x{:.2f}", n.emit.start, n.emit.duration, n.emit.interval, n.emit.maxAlive, n.emit.perEvent);
        if (n.colourKeys) s += L"  " + pk2::Lf("b2_efp_colour_keys", n.colourKeys);
        if (n.commands) s += L"  " + pk2::Lf("b2_efp_commands", n.commands);
        s += L"\n";
        if (!n.texture.empty() || !n.mesh.empty())
            s += std::format(L"{}   tex={}{}{}\n", ind, W::Cp949ToW(n.texture), n.mesh.empty() ? L"" : L"  mesh=", W::Cp949ToW(n.mesh));
    }
    std::wstring flags;
    if (p.colorize) flags += pk2::L("b2_efp_flag_colorize");
    if (p.add_missing_color) flags += pk2::L("b2_efp_flag_missing");
    if (p.recolor_textures) flags += pk2::L("b2_efp_flag_textures");
    s += pk2::Lf("b2_efp_sum_params", p.hue, p.saturation, p.brightness, flags, p.scale, p.particle_size, p.speed, p.duration,
                 p.nodes.empty() ? pk2::L("b2_efp_all") : std::to_wstring(p.nodes.size())) +
         L"\n";
    for (const auto& [from, to] : m.texSwap) s += pk2::Lf("b2_efp_sum_tex", W::Cp949ToW(from), W::Cp949ToW(to)) + L"\n";
    for (const auto& [from, to] : m.meshSwap) s += pk2::Lf("b2_efp_sum_mesh", W::Cp949ToW(from), W::Cp949ToW(to)) + L"\n";
    for (const auto& n : notes()) s += pk2::Lf("b2_efp_sum_note", n) + L"\n";
    s += (modified() ? pk2::L("b2_efp_modified") : pk2::L("b2_efp_unchanged")) + L"\n";
    return s;
}

}  // namespace bak2
