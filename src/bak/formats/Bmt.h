// ============================================================================
//  SRO Asset Studio - formats/Bmt.h
//
//  JMXVBMT 0102 - material set.  Wiki: JMXVBMT.md
//
//    12  signature "JMXVBMT 0102"
//     4  u32 materialCount
//     per material:
//        len-prefixed Name
//        4 x Color4 (diffuse, ambient, specular, emissive)   16 bytes each
//        f32 unkFloat0
//        u32 flags                                  (PrimMtrlFlag)
//        diffuse map: len-prefixed Path, f32, u8, u8, u8 isAbsolute
//        if (flags & 0x2000) normal map: len-prefixed Path, u32
//
//  IsAbsolute tells us whether DiffuseMap.Path is an absolute path inside the
//  archive or relative to the .bmt's own folder - the .bsr composer needs that
//  to resolve textures.
//
//  Implementation owner: Agent D (formats).
// ============================================================================
#pragma once

#include "../Common.h"

namespace sro {

constexpr char kBmtSignaturePrefix[] = "JMXVBMT";

/// PrimMtrlFlag bits we actually act on.
constexpr u32 kMtrlFlagColorTint = 0x00000040;  ///< Bit7
constexpr u32 kMtrlFlagDiffuse   = 0x00000100;  ///< Bit9
constexpr u32 kMtrlFlagAlpha     = 0x00000200;  ///< Bit10 - alpha / metallic sheen
constexpr u32 kMtrlFlagBumpMap   = 0x00002000;  ///< Bit14 - normal map block follows

struct Color4 {
    f32 r = 0, g = 0, b = 0, a = 0;
};

struct BmtTextureRef {
    std::wstring path;          ///< as stored, backslash separated
    f32          unkFloat  = 0;
    u8           unkByte0  = 0;
    u8           unkByte1  = 0;
    bool         isAbsolute = false;   ///< true = path is absolute inside the pk2

    /// Resolve against the .bmt's own folder when the path is relative.
    std::wstring resolve(const std::wstring& bmtFolder) const;
};

struct BmtMaterial {
    std::wstring  name;
    Color4        diffuse;
    Color4        ambient;
    Color4        specular;
    Color4        emissive;
    f32           unkFloat0 = 0;
    u32           flags     = 0;
    BmtTextureRef diffuseMap;
    bool          hasNormalMap = false;
    BmtTextureRef normalMap;
    u32           normalMapUnk = 0;

    bool usesAlpha() const { return (flags & kMtrlFlagAlpha) != 0; }
};

struct BmtFile {
    std::string  signature;
    std::wstring version;
    std::vector<BmtMaterial> materials;

    /// Case-insensitive lookup by material name; nullptr when absent.
    const BmtMaterial* find(const std::wstring& name) const;

    std::wstring summaryTr() const;
};

Result ParseBmt(const u8* data, size_t size, BmtFile& out);

bool IsBmt(const u8* data, size_t size);

} // namespace sro
