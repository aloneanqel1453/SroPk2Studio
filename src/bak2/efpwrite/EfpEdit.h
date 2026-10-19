// SkillEffect Studio - efpwrite/EfpEdit.h  (v6 M22)
// Edits on a lossless efpw::Doc: recolour (hue / saturation / brightness, alpha kept), per-emitter colour keys,
// whole-effect scale (root scale), particle size, speed (time scale), duration (emission length), texture / mesh
// swap, node table for an inspector. Every edit is a no-op (byte-identical output) when its parameters are the
// identity. Node index = depth-first pre-order (render DescribeEffect / EfpEffect::SetNodeHidden order).
// Runtime rules followed: EFP_RUNTIME.md §3 (window), §4 (StaticEmit = live cap), §6 (interpreter: per-firing
// arrays F[k] clamp to the last entry), §9 (s = rootScale x caller scale).
#pragma once

#include "EfpDoc.h"

#include <cstdint>
#include <string>
#include <vector>

namespace se::efpw {

using NodeList = std::vector<int>;   // pre-order node indices; empty = every node

struct EditReport {
    int nodes = 0;          // nodes touched
    int values = 0;         // colours / floats / payloads changed
    std::vector<std::string> notes;
};

// --- colour ---------------------------------------------------------------------------------------------
struct ColorAdjust {
    double hueShift = 0;       // degrees added to the hue (colorize: absolute hue 0..360)
    double saturation = 1;     // multiplier (colorize: absolute saturation 0..1)
    double brightness = 1;     // multiplier of V (HSV value)
    bool colorize = false;     // set hue + saturation for every colour (white / grey become coloured)
    bool IsIdentity() const { return !colorize && hueShift == 0 && saturation == 1 && brightness == 1; }
};
// BGRA byte quadruple (file order), alpha never changed
void AdjustBgra(std::uint8_t* bgra, const ColorAdjust& a);
void AdjustRgba(std::uint8_t* rgba, const ColorAdjust& a);

// Recolour every stored colour of the nodes: BlendDiffuseGraph global keys, DiffuseGraph controller colour keys,
// SetGraphDiffuse bakes in every program list (+ the Program controller copy). addWhereMissing: a drawn node
// without any SetGraphDiffuse gets a birth-only SetGraphDiffuse (window 0,1,0 mode 0) = adjusted white, so
// colorize / darkening also reaches nodes whose colour comes only from the texture. Returns colours changed.
int Recolor(Doc& d, const ColorAdjust& a, const NodeList& nodes = {}, EditReport* rep = nullptr,
            bool addWhereMissing = false);

struct ColorKey {
    float t = 0;               // 0..1 of the particle life
    std::uint8_t r = 255, g = 255, b = 255, a = 255;
};
// authored colour curve of a node: BlendDiffuseGraph global, else DiffuseGraph controller, else the baked
// SetGraphDiffuse (t = i / (n-1)); empty when the node has no colour data
std::vector<ColorKey> GetColorKeys(const Node& n);
// Replace the node's colour curve: global + controller colour keys rewritten, every baked SetGraphDiffuse entry
// re-sampled at i / (n-1). keepAlpha: alpha of every stored entry / key stays as it was (the curve's a ignored).
// A drawn node without SetGraphDiffuse gets a per-tick bake (window 0,1,100 mode 5, L entries).
bool SetColorKeys(Node& n, const std::vector<ColorKey>& keys, bool keepAlpha = true, EditReport* rep = nullptr);

// --- size / time ----------------------------------------------------------------------------------------
// Whole effect x k via the header root scale (positions, sizes, speeds, distances together: s = rootScale x caller
// scale). 0010 / 0011 have no root scale: upgradeVersion -> header becomes 0012 (+ f32 rootScale; the rest of the
// layout is identical), else false + err.
bool ScaleEffect(Doc& d, double k, std::string& err, bool upgradeVersion = true, EditReport* rep = nullptr);
// Particle size only (spread / motion unchanged): SetGraphScale bakes, BlendScaleGraph global, ScaleGraph
// controller. addWhereMissing: a drawn node without SetGraphScale / SetGraphRandomScale gets a birth SetGraphScale (k,k,k).
int ScaleParticles(Doc& d, double k, const NodeList& nodes = {}, EditReport* rep = nullptr, bool addWhereMissing = true);
// Play speed x k (2 = twice as fast, same look): life, StaticEmit (start / duration / interval, per-event count so
// the emitted total stays), command windows (exact step scaling: the firing count and every per-firing array
// stay), Force / ConeForce / Attraction / SetVelocity / SetConeVel x k, rotation speeds (SetRVelocity*,
// SetShapeRotVel) angle x k. Accumulating SetPosition / SetRotation per firing stay (they fire k times as often).
int ChangeSpeed(Doc& d, double k, const NodeList& nodes = {}, EditReport* rep = nullptr);
// Emission length x k: parent nodes (nodes with children) live k times longer (per-life bakes re-sampled to the
// new firing count, windows that ran to the end of life keep doing so); continuous StaticEmit (duration >
// interval) emits k times longer at the same rate; single bursts and leaf particle lives stay.
int ChangeDuration(Doc& d, double k, const NodeList& nodes = {}, EditReport* rep = nullptr);

// --- resources ------------------------------------------------------------------------------------------
struct ResUse {
    std::string path;          // raw file string (CP949)
    bool mesh = false;
    std::vector<int> nodes;
};
std::vector<ResUse> ListResources(const Doc& d);
// Replace texture (mesh) paths equal (ASCII case-insensitive) to `from` ("" = every texture of the nodes) with `to`
// in the node resource AND the Shape controller twin. Returns replacements.
int SwapTexture(Doc& d, const std::string& from, const std::string& to, const NodeList& nodes = {}, EditReport* rep = nullptr);
int SwapMesh(Doc& d, const std::string& from, const std::string& to, const NodeList& nodes = {}, EditReport* rep = nullptr);

// --- inspection -----------------------------------------------------------------------------------------
struct StaticEmit { std::uint32_t start = 0, duration = 0, interval = 0, maxAlive = 0; float perEvent = 0; };
bool GetStaticEmit(const Node& n, StaticEmit& e);
struct NodeInfo {
    int index = 0, depth = 0, parent = -1;
    std::string name, path, life, view, render, texture, mesh;
    std::uint32_t lifeFrames = 0;
    bool hasEmit = false;
    StaticEmit emit;
    int colourKeys = 0;        // GetColorKeys().size()
    bool diffuse = false;      // baked SetGraphDiffuse present
    bool scaleBake = false;    // baked SetGraphScale / SetGraphRandomScale present
    int commands = 0;          // baked program commands
};
std::vector<NodeInfo> DescribeNodes(const Doc& d);

// sub_CA0F40 firing window (efp_sim window()); false = never fires
bool FiringWindow(std::uint8_t mode, float a, float b, float c, std::uint32_t L, double& start, double& step, double& end);
int FiringCount(const Source& s, std::uint32_t L);   // float32 accumulation like CA9070 (max 4096)

// Structural self-check: serialize -> parse -> serialize is stable, counts / payload sizes consistent.
bool Validate(const Doc& d, std::vector<std::string>& problems);

}  // namespace se::efpw
