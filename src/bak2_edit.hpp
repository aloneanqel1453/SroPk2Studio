#pragma once
// Bridge to the vendored BAK-2 .efp writer (src/bak2/efpwrite, namespace se::efpw): lossless
// JMXVEFF 0010-0013 document (Parse -> edit -> Serialize, an unedited document is written back
// byte-identical) and the edits efpwrite offers, applied through its own variant pipeline
// (se::efpw::MakeVariantFromBytes: edit order, self-check, re-parse by the render reader).
// std types only, pimpl; nothing here writes a file or touches an archive.
//
// What efpwrite can change (Params): colour as HSV (hue shift / saturation / brightness, or
// colorize = absolute hue + saturation), whole-effect scale (root scale), particle size, play
// speed, emission duration, texture / mesh references, and optionally recoloured texture copies.
// What it cannot: an RGB tint multiplier and an alpha multiplier (every colour edit keeps alpha).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bak2 {

// Same alias as bak2_view.hpp: reads one asset by inner path (lower case, '/' separated,
// relative to the archive root, siblings resolved by the app). false = missing.
using ReadFn = std::function<bool(const std::wstring& innerPath, std::vector<std::uint8_t>& out)>;

class EfpEditor {
public:
    // efp = the .efp bytes. innerPath = where it lives in Particles (names recoloured texture
    // copies, optional). read = optional asset reader: textures for recolor_textures, and a check
    // that a swap target exists (a missing one is allowed and reported in notes()).
    static std::unique_ptr<EfpEditor> open(const std::vector<std::uint8_t>& efp, std::wstring& err,
                                           const std::wstring& innerPath = {}, ReadFn read = {});
    ~EfpEditor();
    EfpEditor(const EfpEditor&) = delete;
    EfpEditor& operator=(const EfpEditor&) = delete;

    // Applied to the ORIGINAL document on every build (not cumulative). Identity = no change.
    struct Params {
        double hue = 0;               // degrees added to the hue (colorize: absolute hue 0..360)
        double saturation = 1;        // multiplier (colorize: absolute saturation 0..1)
        double brightness = 1;        // multiplier of the HSV value
        bool colorize = false;        // every colour gets hue + saturation (white / grey become coloured)
        bool add_missing_color = false;   // drawn nodes without a colour bake get one (texture-only colour)
        bool recolor_textures = false;    // also clone + recolour the textures (new names, build_files)
        double scale = 1;             // whole effect (root scale; a 0010 / 0011 header becomes 0012)
        double particle_size = 1;     // particle size only
        double speed = 1;             // play speed (2 = twice as fast, same look)
        double duration = 1;          // emission length (parent lives / continuous emitters)
        std::vector<int> nodes;       // pre-order node indices the edits apply to; empty = every node
    };
    void set_params(const Params& p);
    Params params() const;

    // Referenced resources (Particles / Data inner paths as written in the file), current state
    // (pending replacements applied), each listed once.
    std::vector<std::wstring> textures() const;
    std::vector<std::wstring> meshes() const;
    // Replaces every use of textures()[i] / meshes()[i] (node resource and its Shape controller twin).
    bool replace_texture(std::size_t i, const std::wstring& newRef, std::wstring& err);
    bool replace_mesh(std::size_t i, const std::wstring& newRef, std::wstring& err);

    // The edited .efp. Unchanged document => byte-identical to the input.
    bool build(std::vector<std::uint8_t>& out, std::wstring& err) const;
    // The edited .efp first, then the recoloured texture copies (recolor_textures); inner = new
    // Particles path, from = the original it was made from.
    struct OutFile {
        std::wstring inner, from;
        std::vector<std::uint8_t> data;
    };
    bool build_files(std::vector<OutFile>& files, std::wstring& err) const;

    bool modified() const;
    // Texts here and in err follow the UI language (pk2::L).
    std::wstring summary() const;                 // version, root scale, node table, params, swaps
    std::vector<std::wstring> notes() const;      // efpwrite notes of the last build (reader / edit remarks)
    int node_count() const;

private:
    EfpEditor();
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace bak2
