// SkillEffect Studio - efpwrite/EfpVariant.h  (v6 M22)
// One call = one new effect variant: read an .efp (and its textures) through a read-only callback, apply the
// edits, re-check the result with both parsers (efpwrite + render), name every output file NEW, and write only
// into an output folder (never the source, never a client folder; atomic temp + move; manifest of old -> new).
#pragma once

#include "EfpDoc.h"
#include "EfpEdit.h"

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace se::efpw {

// Particles.pk2 inner path (backslash, case-insensitive) -> bytes. Read-only; false when missing.
using ReadFn = std::function<bool(const std::wstring& particlesInner, Bytes& out)>;

struct VariantSpec {
    NodeList nodes;                                    // pre-order indices; empty = every node
    ColorAdjust color;                                 // hue / saturation / brightness (alpha kept)
    bool recolorTextures = false;                      // also clone + recolour the textures of the nodes (new names)
    bool addMissingColor = false;                      // Recolor addWhereMissing
    std::map<int, std::vector<ColorKey>> colorKeys;    // node -> new colour curve (alpha kept)
    double scale = 1.0;                                // whole effect (root scale; 0010/0011 -> 0012 header)
    double particleSize = 1.0;                         // particle size only
    double speed = 1.0;                                // play speed (2 = twice as fast)
    double duration = 1.0;                             // emission length
    std::vector<std::pair<std::string, std::string>> textureSwap;   // raw (CP949) paths, from "" = every texture
    std::vector<std::pair<std::string, std::string>> meshSwap;
    std::wstring tag;                                  // file-name suffix; "" = AutoTag()
    std::wstring newEfpName;                           // explicit new inner path; "" = <name>_<tag>.efp
};

struct OutFile {
    std::wstring inner;        // Particles-relative path of the new file
    std::wstring from;         // the original it was made from
    Bytes data;
};

struct VariantResult {
    std::wstring efpInner;     // new .efp inner path
    std::vector<OutFile> files;   // .efp first, then recoloured textures
    EditReport report;
    std::vector<std::string> notes;
    std::string manifest;      // UTF-8 text written as efpwrite_<name>.txt (WriteOptions::manifest)
};

std::wstring AutoTag(const VariantSpec& s);                               // e.g. "h120_s150_x2"
std::wstring NewInnerName(const std::wstring& inner, const std::wstring& tag);   // dir\name_tag.ext
bool SamePath(const std::wstring& a, const std::wstring& b);              // case-insensitive, '/' == '\'

bool MakeVariant(const std::wstring& efpInner, const VariantSpec& spec, const ReadFn& read, VariantResult& out,
                 std::wstring& err);
bool MakeVariantFromBytes(const Bytes& efp, const std::wstring& efpInner, const VariantSpec& spec, const ReadFn& read,
                          VariantResult& out, std::wstring& err);

struct WriteOptions {
    bool overwrite = false;                    // replace existing files in the output folder
    std::vector<std::wstring> protectedRoots;  // client folders etc.: output there is refused
    bool manifest = true;                      // efpwrite_<name>.txt in the output root
};
// Refuses: output folder inside / equal to a protected root, or inside any folder that holds a client
// (sro_client.exe / Particles.pk2 / Media.pk2); a file named like its source; existing files without overwrite.
bool CheckOutputDir(const std::wstring& outDir, const std::vector<std::wstring>& protectedRoots, std::wstring& err);
bool WriteVariant(const VariantResult& v, const std::wstring& outDir, const WriteOptions& opt,
                  std::vector<std::wstring>* written, std::wstring& err);
std::string ManifestText(const VariantResult& v, const VariantSpec& spec, const std::wstring& sourceEfp);

// raw efp strings (CP949) <-> UTF-16
std::wstring Cp949ToW(const std::string& s);
std::string WToCp949(const std::wstring& s);

}  // namespace se::efpw
