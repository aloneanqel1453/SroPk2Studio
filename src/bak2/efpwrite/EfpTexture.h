// SkillEffect Studio - efpwrite/EfpTexture.h  (v6 M22)
// Texture recolour for .efp variants: .ddj (JMXVDDJ + DDS) or bare .dds in, the same container out with an
// uncompressed 32-bit A8R8G8B8 DDS (lossless, every source mip level decoded and recoloured, alpha kept as decoded).
// Decoding uses the render module (render\RImage.h DecodeDdj: DXT1-5, ATI1/2, uncompressed masks, P8).
#pragma once

#include "EfpDoc.h"
#include "EfpEdit.h"

#include <string>

namespace se::efpw {

// in: .ddj / .dds bytes. out: same container kind (JMXVDDJ header re-sized) with A8R8G8B8 levels.
// changedPx (optional): texels whose RGB changed in level 0.
bool RecolorTexture(const Bytes& in, const ColorAdjust& adj, Bytes& out, std::string& err, std::size_t* changedPx = nullptr);

// 32-bit A8R8G8B8 DDS from RGBA8 levels (level 0 first, each top-down rows). Used by RecolorTexture.
Bytes EncodeDdsArgb(const std::vector<std::vector<std::uint8_t>>& rgbaLevels, int w, int h);

}  // namespace se::efpw
