// ============================================================================
//  SRO Asset Studio - formats/Ddj.h
//
//  JMXVDDJ 1000 - Silkroad's texture container. Wiki: JMXVDDJ.md
//
//    12  char[]  Signature          "JMXVDDJ 1000"
//     4  int     TextureBufferSize  in bytes
//     4  int     TextureType        3 = Texture, 4 = VolumeTexture, 5 = CubeTexture
//     *  byte[]  TextureBuffer      a plain DDS file, starting with "DDS "
//
//  => 20-byte header, then hand the rest to DdsDecode.
//
//  Implementation owner: Agent D (formats).
// ============================================================================
#pragma once

#include "../Common.h"
#include "../render/Image.h"
#include "DdsDecode.h"

namespace sro {

constexpr size_t kDdjHeaderSize = 20;
constexpr char   kDdjSignature[] = "JMXVDDJ ";   ///< version follows: "1000"

/// D3DRESOURCETYPE values that appear in the header.
enum class DdjTextureType : u32 {
    Texture       = 3,
    VolumeTexture = 4,
    CubeTexture   = 5
};

struct DdjFile {
    std::string signature;          ///< "JMXVDDJ 1000"
    std::wstring version;           ///< "1000"
    u32    textureBufferSize = 0;   ///< as declared in the header
    u32    textureType       = 0;
    size_t ddsOffset         = 0;   ///< always kDdjHeaderSize
    size_t ddsSize           = 0;   ///< clamped to what is actually present
    DdsInfo dds;                    ///< filled by ParseDdj on success

    const wchar_t* textureTypeNameTr() const;
};

/// Parse the DDJ header and the embedded DDS header.
/// Bounds-checked: a declared textureBufferSize larger than the buffer is
/// clamped and reported, never trusted.
bool ParseDdj(const u8* data, size_t size, DdjFile& out, std::wstring* errorTr);

/// Decode one surface of a .ddj straight into RGBA8.
bool DecodeDdj(const u8* data, size_t size, int mip, int face,
               Image& out, ImageMeta* metaOut, std::wstring* errorTr);

/// True when the buffer starts with the JMXVDDJ signature.
bool IsDdj(const u8* data, size_t size);

/// Pointer to the embedded DDS payload (nullptr when invalid).
/// Useful for "extract as .dds" without a decode round trip.
const u8* DdjDdsPayload(const u8* data, size_t size, size_t* payloadSize);

} // namespace sro
