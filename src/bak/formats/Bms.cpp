// ============================================================================
//  SRO Asset Studio - formats/Bms.cpp
//
//  JMXVBMS 0110 mesh parser.  Wiki: JMXVBMS.md
//
//  Every section lives at an absolute offset stored in the header, so they are
//  parsed independently: a corrupt navmesh block cannot destroy the geometry we
//  already read. Each section resets the reader's fail latch, seeks, and on
//  failure only marks the file partial.
//
//  Nothing is allocated on a count that has not first been divided into the
//  remaining byte budget - a 4 GB vertexCount can therefore never reserve.
// ============================================================================
#include "Bms.h"
#include "../core/I18n.h"   // UI strings follow the chosen language

namespace sro {

namespace {

/// Silkroad stores its inner paths as western ANSI.
constexpr UINT kPathCodePage = 1252;

/// Reject counts that cannot physically fit in what is left of the buffer.
/// `unitBytes` is the MINIMUM size of one element.
bool CountFits(const ByteReader& r, u64 count, size_t unitBytes) {
    if (count == 0) return true;
    if (unitBytes == 0) return false;
    return count <= (r.remaining() / unitBytes);
}

/// Bytes one vertex occupies given the vertex flags.
size_t VertexStride(u32 vertexFlag) {
    size_t n = 12 /*pos*/ + 12 /*normal*/ + 8 /*uv0*/;
    if (vertexFlag & kBmsVertexLightMapUV)   n += 8;
    if (vertexFlag & kBmsVertexMorphingData) n += 36;
    n += 4 /*float0*/ + 4 /*int0*/ + 4 /*int1*/;
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// IsBms
// ---------------------------------------------------------------------------
bool IsBms(const u8* data, size_t size) {
    return data && size >= 7 && std::memcmp(data, kBmsSignaturePrefix, 7) == 0;
}

// ---------------------------------------------------------------------------
// ParseBms
// ---------------------------------------------------------------------------
Result ParseBms(const u8* data, size_t size, BmsFile& out, bool loadGeometry) {
    out = BmsFile();

    if (!data || size < 12)
        return Result::Fail(Status::Truncated, T(L"BMS: dosya 12 bayttan kısa."));
    if (!IsBms(data, size))
        return Result::Fail(Status::BadFormat, T(L"BMS: imza \"JMXVBMS\" değil."));

    ByteReader r(data, size);

    out.signature = r.readSignature(12);
    if (out.signature.size() >= 12)
        out.version = MultiByteToWide(out.signature.substr(8, 4), kPathCodePage);

    BmsHeader& h = out.header;
    h.vertexOffset          = r.readU32();
    h.skinOffset            = r.readU32();
    h.faceOffset            = r.readU32();
    h.clothVertexOffset     = r.readU32();
    h.clothEdgeOffset       = r.readU32();
    h.boundingBoxOffset     = r.readU32();
    h.occlusionPortalOffset = r.readU32();
    h.navMeshOffset         = r.readU32();
    h.skinnedNavMeshOffset  = r.readU32();
    h.unknown9Offset        = r.readU32();
    h.unkUInt0              = r.readU32();
    h.navFlag               = r.readU32();

    out.subPrimCount = r.readU32();
    out.vertexFlag   = r.readU32();
    (void)r.readU32();                     // unkUInt2, 0 in every known file

    out.name     = r.readLenPrefixedStringW(kPathCodePage, 4096);
    out.material = r.readLenPrefixedStringW(kPathCodePage, 4096);
    (void)r.readU32();                     // unkUInt3

    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"BMS: başlık eksik."));

    out.hasLightmapUV = (out.vertexFlag & kBmsVertexLightMapUV) != 0;
    out.hasMorphData  = (out.vertexFlag & kBmsVertexMorphingData) != 0;

    bool partial = false;
    u32  vertexCount = 0;

    // -----------------------------------------------------------------------
    // VertexBuffer
    // -----------------------------------------------------------------------
    if (r.seekSection(h.vertexOffset)) {
        vertexCount = r.readU32();
        const size_t stride = VertexStride(out.vertexFlag);

        if (r.failed() || !CountFits(r, vertexCount, stride)) {
            partial = true;
            vertexCount = 0;
        } else if (loadGeometry && vertexCount) {
            out.vertices.resize(vertexCount);
            for (u32 i = 0; i < vertexCount; ++i) {
                BmsVertex& v = out.vertices[i];
                v.position = r.readVec3();
                v.normal   = r.readVec3();
                v.uv0      = r.readVec2();
                if (out.hasLightmapUV) v.uv1 = r.readVec2();
                if (out.hasMorphData)  r.skip(36);
                r.skip(12);               // float0 + int0 + int1
                if (r.failed()) { out.vertices.resize(i); partial = true; break; }
            }
        } else if (vertexCount) {
            r.skip(stride * static_cast<size_t>(vertexCount));
        }

        // Lightmap path follows the vertex array.
        if (!r.failed() && out.hasLightmapUV)
            out.lightmapPath = r.readLenPrefixedStringW(kPathCodePage, 4096);

        // Newer ISRO-R models append an extra vector pair list.
        if (!r.failed() && (out.vertexFlag & kBmsVertexExtraData)) {
            const u32 extraCount = r.readU32();
            if (!r.failed() && CountFits(r, extraCount, 24))
                r.skip(static_cast<size_t>(extraCount) * 24);
            else
                partial = true;
        }
        if (r.failed()) partial = true;
    } else {
        partial = true;
    }

    // -----------------------------------------------------------------------
    // SkinningData
    // -----------------------------------------------------------------------
    r.clearFail();
    if (r.seekSection(h.skinOffset)) {
        const u32 boneCount = r.readU32();
        // Minimum bone record is a 4 byte length + 1 char.
        if (!r.failed() && boneCount && CountFits(r, boneCount, 5)) {
            out.boneNames.reserve(boneCount);
            for (u32 i = 0; i < boneCount; ++i) {
                out.boneNames.push_back(r.readLenPrefixedStringW(kPathCodePage, 1024));
                if (r.failed()) { partial = true; break; }
            }
            if (!r.failed() && vertexCount) {
                if (CountFits(r, vertexCount, 6)) {
                    if (loadGeometry) {
                        out.skinWeights.resize(vertexCount);
                        for (u32 i = 0; i < vertexCount; ++i) {
                            BmsSkinWeight& w = out.skinWeights[i];
                            w.boneIndex0 = r.readU8();
                            w.weight0    = r.readU16();
                            w.boneIndex1 = r.readU8();
                            w.weight1    = r.readU16();
                        }
                        if (r.failed()) { out.skinWeights.clear(); partial = true; }
                    } else {
                        // Header-only still has to CONSUME the weights: the stale
                        // offset recovery below uses where this section really
                        // ended, and skipping the skip left it short by 6 bytes
                        // per vertex.
                        r.skip(static_cast<size_t>(vertexCount) * 6);
                    }
                } else {
                    partial = true;
                }
            }
        } else if (boneCount) {
            partial = true;
        }
    }
    // Where the skin section ACTUALLY ended. Hand-edited meshes exist whose
    // header was never recomputed (measured: an avatar with one extra bone added
    // to the skin table - 4 + 15 name bytes - leaves every offset from faceOffset
    // on exactly 19 bytes short), so this is the ground truth to fall back on.
    const size_t afterSkin = r.ok() ? r.pos() : 0;

    // -----------------------------------------------------------------------
    // IndexBuffer
    // -----------------------------------------------------------------------
    r.clearFail();
    // Trust the header first, but verify: a stale offset yields a nonsense count.
    // Then try the sequential position, then a short scan around it. Only a
    // count that actually FITS the remaining bytes is accepted, so this can
    // never invent geometry.
    size_t faceAt = 0;
    {
        const auto plausible = [&](size_t at) -> bool {
            r.clearFail();
            if (!r.seekSection(at)) return false;
            const u32 n = r.readU32();
            const bool good = !r.failed() && n > 0 && CountFits(r, n, 6);
            r.clearFail();
            return good;
        };
        if (plausible(h.faceOffset)) {
            faceAt = h.faceOffset;
        } else if (afterSkin && plausible(afterSkin)) {
            faceAt = afterSkin;
            partial = false;                    // header was stale, data is fine
        } else if (afterSkin) {
            // Bounded scan: the shift is always small (a name length or two).
            const size_t lo = (afterSkin > 64) ? afterSkin - 64 : 0;
            for (size_t at = lo; at <= afterSkin + 64; ++at) {
                if (plausible(at)) { faceAt = at; break; }
            }
        }
    }
    r.clearFail();
    if (faceAt && r.seekSection(faceAt)) {
        const u32 faceCount = r.readU32();
        if (r.failed() || !CountFits(r, faceCount, 6)) {
            partial = true;
        } else {
            out.headerFaceCount = faceCount;
            if (loadGeometry && faceCount) {
                out.faces.resize(faceCount);
                for (u32 i = 0; i < faceCount; ++i) {
                    BmsFace& f = out.faces[i];
                    f.a = r.readU16(); f.b = r.readU16(); f.c = r.readU16();
                }
                if (r.failed()) { out.faces.clear(); partial = true; }
            }
        }
    } else if (vertexCount > 0) {
        partial = true;
    }
    out.headerVertexCount = vertexCount;

    // Everything after the index buffer carries the SAME staleness, so shift the
    // remaining offsets by however far the face section really was. Zero for a
    // healthy file, which is the overwhelming majority.
    const ptrdiff_t tailShift =
        (faceAt && h.faceOffset) ? static_cast<ptrdiff_t>(faceAt) - static_cast<ptrdiff_t>(h.faceOffset)
                                 : 0;
    const auto shifted = [&](u32 off) -> size_t {
        if (off == 0) return 0;
        const ptrdiff_t v = static_cast<ptrdiff_t>(off) + tailShift;
        return (v > 0) ? static_cast<size_t>(v) : 0;
    };

    // -----------------------------------------------------------------------
    // Cloth simulation data (counts only)
    // -----------------------------------------------------------------------
    r.clearFail();
    if (r.seekSection(shifted(h.clothVertexOffset))) {
        const u32 n = r.readU32();
        if (!r.failed() && CountFits(r, n, 8)) out.clothVertexCount = n;
    }
    r.clearFail();
    if (r.seekSection(shifted(h.clothEdgeOffset))) {
        const u32 n = r.readU32();
        if (!r.failed() && CountFits(r, n, 12)) out.clothEdgeCount = n;
    }

    // -----------------------------------------------------------------------
    // Bounding box
    // -----------------------------------------------------------------------
    r.clearFail();
    bool haveBox = false;
    if (r.seekSection(shifted(h.boundingBoxOffset))) {
        const Aabb box = r.readAabb();
        if (!r.failed() && box.valid()) { out.boundingBox = box; haveBox = true; }
    }
    if (!haveBox) {
        // Derive it from the vertices we actually loaded.
        Aabb box;
        for (const BmsVertex& v : out.vertices) box.add(v.position);
        if (box.valid()) out.boundingBox = box;
    }

    // -----------------------------------------------------------------------
    // Occlusion portal (name + presence only)
    // -----------------------------------------------------------------------
    r.clearFail();
    if (r.seekSection(shifted(h.occlusionPortalOffset))) {
        const u32 has = r.readU32();
        if (!r.failed() && has == 1) {
            out.hasOcclusionPortal = true;
            out.occlusionPortalName = r.readLenPrefixedStringW(kPathCodePage, 1024);
        }
    }

    // -----------------------------------------------------------------------
    // NavMesh (counts only - the 3D preview does not draw collision)
    // -----------------------------------------------------------------------
    r.clearFail();
    if (h.navMeshOffset > 0 && r.seekSection(shifted(h.navMeshOffset))) {
        const u32 navVerts = r.readU32();
        if (!r.failed() && CountFits(r, navVerts, 13)) {
            out.navVertexCount = navVerts;
            r.skip(static_cast<size_t>(navVerts) * 13);

            const u32 cells = r.readU32();
            const size_t cellSize = (h.navFlag & kBmsNavCell) ? 9u : 8u;
            if (!r.failed() && CountFits(r, cells, cellSize)) {
                out.navCellCount = cells;
                r.skip(static_cast<size_t>(cells) * cellSize);

                const size_t edgeSize = (h.navFlag & kBmsNavEdge) ? 10u : 9u;
                const u32 outline = r.readU32();
                if (!r.failed() && CountFits(r, outline, edgeSize)) {
                    out.navOutlineCount = outline;
                    r.skip(static_cast<size_t>(outline) * edgeSize);

                    const u32 inl = r.readU32();
                    if (!r.failed() && CountFits(r, inl, edgeSize))
                        out.navInlineCount = inl;
                }
            }
        }
    }
    r.clearFail();

    if (partial)
        return Result::Fail(Status::Truncated,
                            T(L"BMS: dosya eksik veya bozuk - kısmi olarak okundu."));
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// summaryTr
// ---------------------------------------------------------------------------
std::wstring BmsFile::summaryTr() const {
    std::wstring s;
    s += T(L"İmza          : ") + MultiByteToWide(signature, 1252) + L"\n";
    if (!name.empty())     s += T(L"Ad            : ") + name + L"\n";
    if (!material.empty()) s += T(L"Malzeme       : ") + material + L"\n";

    const u64 vc = vertices.empty() ? headerVertexCount : vertices.size();
    const u64 fc = faces.empty() ? headerFaceCount : faces.size();
    s += T(L"Köşe sayısı   : ") + FormatCountTr(vc) + L"\n";
    s += T(L"Üçgen sayısı  : ") + FormatCountTr(fc) + L"\n";

    s += T(L"Vertex flag   : 0x") + FormatW(L"%03X", vertexFlag);
    if (hasLightmapUV) s += T(L"  (lightmap UV)");
    if (hasMorphData)  s += T(L"  (morph)");
    s += L"\n";

    if (!lightmapPath.empty())
        s += T(L"Lightmap      : ") + lightmapPath + L"\n";

    if (isSkinned()) {
        s += T(L"Kemik sayısı  : ") + FormatCountTr(boneNames.size()) + L"\n";
        std::wstring list;
        const size_t shown = (boneNames.size() < 6) ? boneNames.size() : 6;
        for (size_t i = 0; i < shown; ++i) {
            if (i) list += L", ";
            list += boneNames[i];
        }
        if (boneNames.size() > shown) list += L", ...";
        s += T(L"Kemikler      : ") + list + L"\n";
    } else {
        s += T(L"Kemik sayısı  : yok (statik ağ)\n");
    }

    if (boundingBox.valid()) {
        const Vec3 e = boundingBox.extent();
        s += FormatW(T(L"Sınır kutusu  : min(%.2f, %.2f, %.2f)  max(%.2f, %.2f, %.2f)\n"),
                     boundingBox.min.x, boundingBox.min.y, boundingBox.min.z,
                     boundingBox.max.x, boundingBox.max.y, boundingBox.max.z);
        s += FormatW(T(L"Boyut         : %.2f x %.2f x %.2f\n"), e.x, e.y, e.z);
    }

    if (clothVertexCount || clothEdgeCount)
        s += T(L"Kumaş         : ") + FormatCountTr(clothVertexCount) + T(L" köşe, ") +
             FormatCountTr(clothEdgeCount) + T(L" kenar\n");

    if (hasOcclusionPortal)
        s += T(L"Okluzyon      : ") + (occlusionPortalName.empty() ? std::wstring(T(L"var"))
                                                                : occlusionPortalName) + L"\n";

    if (navVertexCount || navCellCount)
        s += T(L"Navigasyon    : ") + FormatCountTr(navVertexCount) + T(L" köşe, ") +
             FormatCountTr(navCellCount) + T(L" hücre, ") +
             FormatCountTr(navOutlineCount) + L"/" + FormatCountTr(navInlineCount) +
             T(L" kenar\n");

    return s;
}

// ---------------------------------------------------------------------------
// BmsToObj
//
//  * SRO winding is CLOCKWISE / left-handed; Wavefront OBJ readers assume
//    counter-clockwise front faces, so the indices are emitted a, c, b.
//  * SRO's V axis runs top->bottom, OBJ's runs bottom->top, so V is flipped.
// ---------------------------------------------------------------------------
Result BmsToObj(const BmsFile& mesh, const std::wstring& mtlName, std::string& outObj) {
    outObj.clear();

    if (mesh.vertices.empty())
        return Result::Fail(Status::InvalidArgument,
                            L"OBJ: ağda köşe yok (geometri yüklenmemiş olabilir).");

    std::string s;
    s.reserve(mesh.vertices.size() * 64 + mesh.faces.size() * 32 + 256);

    char buf[256];
    s += "# SRO Asset Studio - Wavefront OBJ\n";
    s += "# source: ";
    s += WideToUtf8(mesh.name.empty() ? std::wstring(T(L"(isimsiz)")) : mesh.name);
    s += "\n";
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "# %zu vertices, %zu faces\n",
                mesh.vertices.size(), mesh.faces.size());
    s += buf;

    if (!mtlName.empty()) {
        s += "mtllib ";
        s += WideToUtf8(mtlName);
        s += "\n";
    }

    s += "o ";
    s += WideToUtf8(mesh.name.empty() ? std::wstring(L"mesh") : mesh.name);
    s += "\n";

    for (const BmsVertex& v : mesh.vertices) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "v %.6f %.6f %.6f\n",
                    v.position.x, v.position.y, v.position.z);
        s += buf;
    }
    for (const BmsVertex& v : mesh.vertices) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "vt %.6f %.6f\n",
                    v.uv0.x, 1.0f - v.uv0.y);
        s += buf;
    }
    for (const BmsVertex& v : mesh.vertices) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "vn %.6f %.6f %.6f\n",
                    v.normal.x, v.normal.y, v.normal.z);
        s += buf;
    }

    if (!mtlName.empty() && !mesh.material.empty()) {
        s += "usemtl ";
        s += WideToUtf8(mesh.material);
        s += "\n";
    }

    const size_t vc = mesh.vertices.size();
    for (const BmsFace& f : mesh.faces) {
        if (f.a >= vc || f.b >= vc || f.c >= vc) continue;   // drop bad triangles
        const unsigned a = f.a + 1u, b = f.b + 1u, c = f.c + 1u;
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "f %u/%u/%u %u/%u/%u %u/%u/%u\n",
                    a, a, a, c, c, c, b, b, b);
        s += buf;
    }

    outObj.swap(s);
    return Result::Ok();
}

} // namespace sro
