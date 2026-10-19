// ============================================================================
//  SRO Asset Studio - formats/Ddj.cpp
//
//  JMXVDDJ - Silkroad's texture container. Wiki: JMXVDDJ.md
//
//     12  char[]  Signature          "JMXVDDJ 1000"
//      4  int     TextureBufferSize  in bytes
//      4  int     TextureType        3 = Texture, 4 = VolumeTexture, 5 = Cube
//      *  byte[]  TextureBuffer      a plain DDS file, starting with "DDS "
//
//  => a 20 byte header, then the DDS payload handed to DdsDecode.
//
//  Defensive handling of other versions: only the 8 character "JMXVDDJ "
//  prefix is required. If the payload does not start with "DDS " at offset 20
//  (a differently sized header on some other revision) the first 256 bytes are
//  scanned for the DDS magic and that offset is used instead. A declared
//  TextureBufferSize larger than the file is clamped and reported, never
//  trusted.
// ============================================================================
#include "Ddj.h"
#include "../core/I18n.h"   // UI strings follow the chosen language

namespace sro {

namespace {

/// Longest header we are willing to skip when hunting for the DDS magic in a
/// DDJ revision whose header is not 20 bytes.
constexpr size_t kDdsScanWindow = 256;

/// Offset of the "DDS " magic, or SIZE_MAX when it is not in the window.
size_t FindDdsMagic(const u8* data, size_t size) {
    if (!data || size < 4) return static_cast<size_t>(-1);
    const size_t limit = (std::min)(size - 4, kDdsScanWindow);
    for (size_t i = 0; i <= limit; ++i) {
        if (data[i] == 'D' && data[i + 1] == 'D' && data[i + 2] == 'S' && data[i + 3] == ' ')
            return i;
    }
    return static_cast<size_t>(-1);
}

} // namespace

// ---------------------------------------------------------------------------
const wchar_t* DdjFile::textureTypeNameTr() const {
    switch (static_cast<DdjTextureType>(textureType)) {
        case DdjTextureType::Texture:       return T(L"Doku");
        case DdjTextureType::VolumeTexture: return T(L"Hacim dokusu");
        case DdjTextureType::CubeTexture:   return T(L"Kup dokusu");
        default:                            return T(L"Bilinmeyen");
    }
}

// ---------------------------------------------------------------------------
bool IsDdj(const u8* data, size_t size) {
    constexpr size_t kPrefix = sizeof(kDdjSignature) - 1;   // "JMXVDDJ "
    return data && size >= kPrefix && std::memcmp(data, kDdjSignature, kPrefix) == 0;
}

// ---------------------------------------------------------------------------
bool ParseDdj(const u8* data, size_t size, DdjFile& out, std::wstring* errorTr) {
    out = DdjFile();

    if (!IsDdj(data, size)) {
        if (errorTr) *errorTr = T(L"JMXVDDJ imzasi bulunamadi.");
        return false;
    }
    if (size < kDdjHeaderSize) {
        if (errorTr) *errorTr = T(L"DDJ basligi eksik (20 bayt gerekiyor).");
        return false;
    }

    ByteReader r(data, size);
    out.signature        = r.readSignature(12);
    out.textureBufferSize = r.readU32();
    out.textureType       = r.readU32();
    if (r.failed()) {
        if (errorTr) *errorTr = T(L"DDJ basligi okunamadi.");
        return false;
    }

    // "JMXVDDJ 1000" -> version "1000"
    if (out.signature.size() >= 12)
        out.version = MultiByteToWide(out.signature.substr(8, 4), 1252);
    else if (out.signature.size() > 8)
        out.version = MultiByteToWide(out.signature.substr(8), 1252);

    // Locate the DDS payload. Normally exactly at kDdjHeaderSize.
    size_t payloadOffset = kDdjHeaderSize;
    if (size < payloadOffset + 4 ||
        std::memcmp(data + payloadOffset, "DDS ", 4) != 0) {
        const size_t found = FindDdsMagic(data, size);
        if (found == static_cast<size_t>(-1)) {
            if (errorTr) *errorTr = T(L"DDJ icinde DDS verisi bulunamadi.");
            return false;
        }
        payloadOffset = found;
    }
    out.ddsOffset = payloadOffset;

    const size_t present = size - payloadOffset;
    out.ddsSize = present;
    if (out.textureBufferSize != 0 && static_cast<size_t>(out.textureBufferSize) < present)
        out.ddsSize = static_cast<size_t>(out.textureBufferSize);

    std::wstring note;
    if (out.textureBufferSize != 0 && static_cast<size_t>(out.textureBufferSize) > present) {
        note = FormatW(T(L"DDJ basligi %u bayt doku bildiriyor, dosyada %llu bayt var."),
                       out.textureBufferSize, static_cast<unsigned long long>(present));
    }

    std::wstring ddsError;
    if (!ParseDds(data + out.ddsOffset, out.ddsSize, out.dds, &ddsError)) {
        if (errorTr) *errorTr = note.empty() ? ddsError : (note + L" " + ddsError);
        return false;
    }
    if (errorTr) *errorTr = note.empty() ? ddsError : (note + L" " + ddsError);
    return true;
}

// ---------------------------------------------------------------------------
const u8* DdjDdsPayload(const u8* data, size_t size, size_t* payloadSize) {
    if (payloadSize) *payloadSize = 0;
    if (!IsDdj(data, size) || size <= kDdjHeaderSize) return nullptr;

    size_t offset = kDdjHeaderSize;
    if (size < offset + 4 || std::memcmp(data + offset, "DDS ", 4) != 0) {
        const size_t found = FindDdsMagic(data, size);
        if (found == static_cast<size_t>(-1)) return nullptr;
        offset = found;
    }

    u32 declared = 0;
    std::memcpy(&declared, data + 12, 4);

    size_t present = size - offset;
    if (declared != 0 && static_cast<size_t>(declared) < present)
        present = static_cast<size_t>(declared);

    if (payloadSize) *payloadSize = present;
    return data + offset;
}

// ---------------------------------------------------------------------------
bool DecodeDdj(const u8* data, size_t size, int mip, int face,
               Image& out, ImageMeta* metaOut, std::wstring* errorTr) {
    out.clear();
    if (metaOut) *metaOut = ImageMeta();

    DdjFile ddj;
    std::wstring parseNote;
    if (!ParseDdj(data, size, ddj, &parseNote)) {
        if (errorTr) *errorTr = parseNote.empty() ? std::wstring(T(L"DDJ cozulemedi.")) : parseNote;
        return false;
    }

    if (mip  < 0) mip  = 0;
    if (face < 0) face = 0;
    if (mip  >= ddj.dds.mipCount)  mip  = ddj.dds.mipCount - 1;
    if (face >= ddj.dds.faceCount) face = ddj.dds.faceCount - 1;

    if (!DecodeDdsSurface(data + ddj.ddsOffset, ddj.ddsSize, ddj.dds, mip, face, out, errorTr))
        return false;

    if (metaOut) {
        metaOut->formatName = ddj.dds.formatName.empty()
                            ? std::wstring(DdsFormatName(ddj.dds.format)) : ddj.dds.formatName;
        metaOut->width     = ddj.dds.width;
        metaOut->height    = ddj.dds.height;
        metaOut->mipCount  = ddj.dds.mipCount;
        metaOut->faceCount = ddj.dds.faceCount;
        metaOut->depth     = ddj.dds.depth;
        metaOut->dataSize  = static_cast<u64>(size);
        metaOut->hasAlpha  = ddj.dds.hasAlphaChannel;
        metaOut->isCube    = ddj.dds.isCube;
        metaOut->isVolume  = ddj.dds.isVolume;
    }
    if (errorTr) *errorTr = parseNote;
    return true;
}

} // namespace sro
