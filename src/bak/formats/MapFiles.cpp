// ============================================================================
//  SRO Asset Studio - formats/MapFiles.cpp
//
//  The world / map family: .nvm .dof .m .t .o/.o2 .mfo .cpd .ifo and the
//  binary JMXVENVI environment set.
//
//  The spec only asks for a header summary + counts, so these parsers read the
//  structure and the top-level counts and stop. Where a size is fully implied
//  by the format (.m is exactly 12 + 36 * 2575 = 92,712 bytes) the parser reads
//  it all, because that is both cheap and a strong validity check.
//
//  Every count is divided into the remaining byte budget before it is used.
// ============================================================================
#include "MapFiles.h"
#include "../core/I18n.h"   // UI strings follow the chosen language

namespace sro {

namespace {

constexpr UINT kPathCodePage = 1252;

/// Per-block sizes for JMXVMAPM1000 (see the arithmetic in ParseMapMesh).
constexpr size_t kMapMeshVertexBytes = 4 + 2 + 1;                     // 7
constexpr size_t kMapMeshBlockBytes  = 4 + 2 +
                                       17 * 17 * kMapMeshVertexBytes +  // 2023
                                       1 + 1 + 4 +
                                       16 * 16 * 2 +                    // 512
                                       4 + 4 + 20;                      // = 2575

/// Object placement record sizes.
constexpr size_t kMapObjectRecordO  = 4 + 12 + 2 + 4 + 2 + 2 + 1 + 1;   // 28
constexpr size_t kMapObjectRecordO2 = kMapObjectRecordO + 2;            // 30

std::wstring SigToW(const std::string& s) { return TrimW(MultiByteToWide(s, kPathCodePage)); }

/// Silkroad was authored in Korea: room / object / light NAMES are EUC-KR
/// (code page 949), while the resource PATHS are plain ASCII. Reading a name as
/// Latin-1 turns it into mojibake ("jinsi_floor5_¿°È­·É"), so names go through
/// 949 first and only fall back to 1252 when that yields nothing usable.
constexpr UINT kKoreanCodePage = 949;

std::wstring ReadNameKorean(ByteReader& r, size_t maxLen) {
    const std::string raw = r.readLenPrefixedString(maxLen);
    if (raw.empty()) return std::wstring();

    bool highBytes = false;
    for (unsigned char c : raw) if (c >= 0x80) { highBytes = true; break; }
    if (!highBytes) return MultiByteToWide(raw, kPathCodePage);   // pure ASCII

    const std::wstring kr = MultiByteToWide(raw, kKoreanCodePage);
    if (!kr.empty()) return kr;
    return MultiByteToWide(raw, kPathCodePage);                   // 949 unavailable
}

/// Read "int keyCount, then keyCount * unitBytes"; returns the count.
u32 SkipGraph(ByteReader& r, size_t unitBytes) {
    const u32 n = r.readU32();
    if (r.failed()) return 0;
    if (unitBytes && n > r.remaining() / unitBytes) { r.fail(); return 0; }
    r.skip(static_cast<size_t>(n) * unitBytes);
    return n;
}

/// Split an .ifo line into fields, honouring "quoted strings with spaces" and
/// the {a,b} grass tuples used by tile2d.ifo.
std::vector<std::wstring> SplitIfoLine(const std::wstring& line) {
    std::vector<std::wstring> out;
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        while (i < n && (line[i] == L' ' || line[i] == L'\t')) ++i;
        if (i >= n) break;

        std::wstring field;
        if (line[i] == L'"') {
            ++i;
            while (i < n && line[i] != L'"') field.push_back(line[i++]);
            if (i < n) ++i;                      // closing quote
        } else if (line[i] == L'{') {
            // Grass tuples may be several {a,b} groups separated by spaces;
            // keep each group as one field.
            while (i < n && line[i] != L'}') field.push_back(line[i++]);
            if (i < n) field.push_back(line[i++]);
        } else {
            while (i < n && line[i] != L' ' && line[i] != L'\t') field.push_back(line[i++]);
        }
        out.push_back(std::move(field));
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// RegionId
// ---------------------------------------------------------------------------
std::wstring RegionId::toStringTr() const {
    return FormatW(L"%d,%d%s", x(), z(), isDungeon() ? T(L" (zindan)") : L"");
}

// ===========================================================================
//  .nvm - JMXVNVM 1000
// ===========================================================================
Result ParseNvm(const u8* data, size_t size, NvmFile& out, bool loadObjects) {
    out = NvmFile();

    if (!data || size < 14)
        return Result::Fail(Status::Truncated, T(L"NVM: dosya 14 bayttan kısa."));

    ByteReader r(data, size);
    out.signature = r.readSignature(12);
    out.version   = SigToW(out.signature);
    if (out.signature.compare(0, 7, "JMXVNVM") != 0)
        return Result::Fail(Status::BadFormat, T(L"NVM: imza \"JMXVNVM\" değil."));

    // --- object instances ---------------------------------------------------
    const u16 objCount = r.readU16();
    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"NVM: nesne sayısı okunamadı."));
    out.objectCount = objCount;
    if (objCount > r.remaining() / 32) {         // 30 byte record + 2 byte link count
        out.partial = true;
        return Result::Fail(Status::BadFormat, T(L"NVM: nesne sayısı dosyaya sığmıyor."));
    }

    for (u16 i = 0; i < objCount; ++i) {
        NvmObjectInstance o;
        o.assetId       = r.readI32();
        o.localPosition = r.readVec3();
        o.type          = r.readI16();
        o.yaw           = r.readF32();
        o.localUid      = r.readI16();
        (void)r.readI16();                       // Short0
        o.isBig         = r.readBool8();
        o.isStruct      = r.readBool8();
        o.regionId      = r.readU16();

        const u16 linkCount = r.readU16();
        if (r.failed() || linkCount > r.remaining() / 6) { out.partial = true; break; }
        r.skip(static_cast<size_t>(linkCount) * 6);
        if (r.failed()) { out.partial = true; break; }

        if (loadObjects) out.objects.push_back(o);
    }
    if (out.partial)
        return Result::Fail(Status::Truncated, T(L"NVM: nesne listesi eksik."));

    // --- quad cells ---------------------------------------------------------
    out.cellQuadCount = r.readU32();
    const u32 openCells = r.readU32();
    (void)openCells;
    if (r.failed() || out.cellQuadCount > r.remaining() / 17) {
        out.partial = true;
        return Result::Fail(Status::Truncated, T(L"NVM: hücre listesi okunamadı."));
    }
    for (u32 i = 0; i < out.cellQuadCount; ++i) {
        r.skip(16);                              // NavRect min/max
        const u8 cellObjCount = r.readU8();
        if (r.failed() || cellObjCount > r.remaining() / 2) { out.partial = true; break; }
        r.skip(static_cast<size_t>(cellObjCount) * 2);
        if (r.failed()) { out.partial = true; break; }
    }
    if (out.partial)
        return Result::Fail(Status::Truncated, T(L"NVM: hücre listesi eksik."));

    // --- edges --------------------------------------------------------------
    out.globalEdgeCount = r.readU32();
    if (r.failed() || out.globalEdgeCount > r.remaining() / 27) {
        out.partial = true;
        return Result::Fail(Status::Truncated, T(L"NVM: global kenar listesi okunamadı."));
    }
    r.skip(static_cast<size_t>(out.globalEdgeCount) * 27);

    out.internalEdgeCount = r.readU32();
    if (r.failed() || out.internalEdgeCount > r.remaining() / 23) {
        out.partial = true;
        return Result::Fail(Status::Truncated, T(L"NVM: iç kenar listesi okunamadı."));
    }
    r.skip(static_cast<size_t>(out.internalEdgeCount) * 23);

    // --- fixed size tail ----------------------------------------------------
    if (r.canRead(96 * 96 * 8)) { out.tileCount = 96 * 96; r.skip(96 * 96 * 8); }
    else                        { out.partial = true; }

    if (!out.partial && r.canRead(97 * 97 * 4)) {
        out.heightCount = 97 * 97;
        out.heightMap.resize(97 * 97);
        for (int i = 0; i < 97 * 97; ++i) out.heightMap[i] = r.readF32();
        if (r.failed()) { out.heightMap.clear(); out.partial = true; }
    } else if (!out.partial) { out.partial = true; }

    if (!out.partial && !r.skip(6 * 6 + 6 * 6 * 4)) out.partial = true;

    if (out.partial)
        return Result::Fail(Status::Truncated, T(L"NVM: dosya sonu eksik (tile/height/plane)."));
    return Result::Ok();
}

std::wstring NvmFile::summaryTr() const {
    std::wstring s;
    s += T(L"İmza          : ") + MultiByteToWide(signature, 1252) + L"\n";
    s += T(L"Nesne örneği  : ") + FormatCountTr(objectCount) + L"\n";
    s += T(L"Quad hücre    : ") + FormatCountTr(cellQuadCount) + L"\n";
    s += T(L"Global kenar  : ") + FormatCountTr(globalEdgeCount) + L"\n";
    s += T(L"İç kenar      : ") + FormatCountTr(internalEdgeCount) + L"\n";
    s += T(L"Tile haritası : ") + FormatCountTr(tileCount) + T(L" (96 x 96)\n");
    s += T(L"Yükseklik     : ") + FormatCountTr(heightCount) + T(L" (97 x 97)\n");
    if (partial) s += T(L"UYARI         : Dosya eksik okundu.\n");

    if (!objects.empty()) {
        const size_t shown = (objects.size() < 10) ? objects.size() : 10;
        s += T(L"\nNesneler (ilk ") + FormatCountTr(shown) + T(L"):\n");
        for (size_t i = 0; i < shown; ++i) {
            const NvmObjectInstance& o = objects[i];
            s += FormatW(T(L"    #%d  konum(%.1f, %.1f, %.1f)  yaw %.2f  bölge %u%s%s\n"),
                         o.assetId, o.localPosition.x, o.localPosition.y, o.localPosition.z,
                         o.yaw, o.regionId,
                         o.isBig ? T(L"  [büyük]") : L"",
                         o.isStruct ? T(L"  [yapı]") : L"");
        }
        if (objects.size() > shown)
            s += T(L"    ... ve ") + FormatCountTr(objects.size() - shown) + T(L" nesne daha\n");
    }
    return s;
}

// ===========================================================================
//  .dof - dungeon object file
// ===========================================================================
Result ParseDof(const u8* data, size_t size, DofFile& out) {
    out = DofFile();

    if (!data || size < 44)
        return Result::Fail(Status::Truncated, T(L"DOF: dosya 44 bayttan kısa."));

    ByteReader r(data, size);
    out.signature = r.readSignature(12);
    out.version   = SigToW(out.signature);
    if (out.signature.compare(0, 7, "JMXVDOF") != 0)
        return Result::Fail(Status::BadFormat, T(L"DOF: imza \"JMXVDOF\" değil."));

    out.blockOffset       = r.readU32();
    out.linkOffset        = r.readU32();
    out.gridOffset        = r.readU32();
    out.groupOffset       = r.readU32();
    out.labelOffset       = r.readU32();
    (void)r.readU32();                                   // Offset5, 0 in every file
    (void)r.readU32();                                   // Offset6, 0 in every file
    out.boundingBoxOffset = r.readU32();

    out.objectType = r.readU32();
    out.objectName = r.readLenPrefixedStringW(kPathCodePage, 4096);
    (void)r.readU32();
    (void)r.readU32();
    out.region.raw = r.readU16();

    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"DOF: başlık eksik."));

    // --- bounding boxes -----------------------------------------------------
    r.clearFail();
    if (r.seekSection(out.boundingBoxOffset)) {
        out.collisionBox0 = r.readAabb();
        out.collisionBox1 = r.readAabb();
        if (r.failed()) out.partial = true;
    }

    // --- blocks -------------------------------------------------------------
    r.clearFail();
    if (r.seekSection(out.blockOffset)) {
        const u32 blockCount = r.readU32();
        // Minimum block record is well over 100 bytes; 64 is a safe floor.
        if (r.failed() || blockCount > r.remaining() / 64) {
            out.partial = true;
        } else {
            out.blocks.reserve(blockCount);
            for (u32 i = 0; i < blockCount; ++i) {
                DofBlock b;
                b.path = r.readLenPrefixedStringW(kPathCodePage, 4096);
                b.name = ReadNameKorean(r, 4096);        // room names are EUC-KR
                (void)r.readU32();                       // unkUInt0
                b.position   = r.readVec3();
                b.yaw        = r.readF32();
                b.isEntrance = r.readU32() != 0;
                b.collisionBox = r.readAabb();
                (void)r.readU32();                       // unkUInt1

                // Fog parameters
                b.fog.color     = r.readU32();
                b.fog.nearPlane = r.readF32();
                b.fog.farPlane  = r.readF32();
                b.fog.intensity = r.readF32();
                b.fog.heightFog = r.readU8() != 0;
                if (b.fog.heightFog) r.skip(16);

                const u8 unkByte1 = r.readU8();
                if (unkByte1) r.skip(12 + 12 + 4);

                (void)r.readLenPrefixedStringW(kPathCodePage, 4096);   // unkString
                b.roomIndex  = r.readU32();
                b.floorIndex = r.readU32();

                u32 n = r.readU32();                     // connected blocks
                if (r.failed() || n > r.remaining() / 4) { out.partial = true; break; }
                b.connected.reserve(n);
                for (u32 k = 0; k < n; ++k) b.connected.push_back(r.readU32());

                n = r.readU32();                         // visible blocks
                if (r.failed() || n > r.remaining() / 4) { out.partial = true; break; }
                b.visible.reserve(n);
                for (u32 k = 0; k < n; ++k) b.visible.push_back(r.readU32());

                const u32 objCount = r.readU32();
                (void)r.readU32();                       // dwColObjCount
                if (r.failed() || objCount > r.remaining() / 56) { out.partial = true; break; }
                b.objects.reserve(objCount);
                for (u32 o = 0; o < objCount; ++o) {
                    DofObject ob;
                    ob.name     = ReadNameKorean(r, 4096);
                    ob.path     = r.readLenPrefixedStringW(kPathCodePage, 4096);
                    ob.position = r.readVec3();
                    ob.rotation = r.readVec3();
                    ob.scale    = r.readVec3();
                    ob.flag     = r.readU32();
                    (void)r.readU32();                   // Int0
                    ob.radiusSqrt = r.readF32();
                    if (ob.flag & 4) ob.waterColor = r.readU32();
                    if (r.failed()) break;
                    b.objects.push_back(std::move(ob));
                }
                if (r.failed()) { out.partial = true; break; }

                const u32 lightCount = r.readU32();
                if (r.failed() || lightCount > r.remaining() / 64) { out.partial = true; break; }
                b.lights.reserve(lightCount);
                for (u32 l = 0; l < lightCount; ++l) {
                    DofLight li;
                    li.name     = ReadNameKorean(r, 4096);
                    li.position = r.readVec3();
                    li.diffuse  = r.readVec3();
                    li.ambient  = r.readVec3();
                    li.specular = r.readVec3();
                    li.f0 = r.readF32(); li.f1 = r.readF32(); li.f2 = r.readF32();
                    if (r.failed()) break;
                    b.lights.push_back(std::move(li));
                }
                if (r.failed()) { out.partial = true; break; }

                out.blocks.push_back(std::move(b));
            }
        }
    }

    // --- link / group / label counts ---------------------------------------
    r.clearFail();
    if (r.seekSection(out.linkOffset)) {
        const u32 n = r.readU32();
        if (!r.failed() && n <= r.remaining() / 4) out.linkCount = n;
    }
    r.clearFail();
    if (r.seekSection(out.groupOffset)) {
        const u32 n = r.readU32();
        if (!r.failed() && n <= r.remaining() / 12) {
            out.groupCount = n;
            out.groups.reserve(n);
            for (u32 i = 0; i < n; ++i) {
                DofGroup g;
                g.name = ReadNameKorean(r, 4096);
                g.flag = r.readU32();
                const u32 bc = r.readU32();
                if (r.failed() || bc > r.remaining() / 4) break;
                g.blockIndices.reserve(bc);
                for (u32 k = 0; k < bc; ++k) g.blockIndices.push_back(r.readU32());
                if (r.failed()) break;
                out.groups.push_back(std::move(g));
            }
        }
    }
    // Label table: the human-readable room and floor names the blocks index into.
    r.clearFail();
    if (out.labelOffset && r.seekSection(out.labelOffset)) {
        const u32 rooms = r.readU32();
        if (!r.failed() && rooms <= r.remaining() / 4) {
            out.labelCount = rooms;
            out.roomNames.reserve(rooms);
            for (u32 i = 0; i < rooms; ++i) {
                out.roomNames.push_back(ReadNameKorean(r, 4096));
                if (r.failed()) break;
            }
            if (!r.failed()) {
                const u32 floors = r.readU32();
                if (!r.failed() && floors <= r.remaining() / 4) {
                    // The wiki shows a 16-bit length here, but the SHIPPED files
                    // disagree: with u32 all 31 dungeons that carry floors end
                    // exactly at GroupOffset, with u16 none do (and 9 read past
                    // EOF). Floor names are the same u32 + EUC-KR as room names.
                    out.floorNames.reserve(floors);
                    for (u32 i = 0; i < floors; ++i) {
                        out.floorNames.push_back(ReadNameKorean(r, 4096));
                        if (r.failed()) break;
                    }
                }
            }
        }
    }
    r.clearFail();

    if (out.partial)
        return Result::Fail(Status::Truncated,
                            T(L"DOF: blok listesi kısmi okundu (") +
                            FormatCountTr(out.blocks.size()) + T(L" blok)."));
    return Result::Ok();
}

std::wstring DofFile::summaryTr() const {
    std::wstring s;
    s += T(L"İmza          : ") + MultiByteToWide(signature, 1252) + L"\n";
    if (!objectName.empty()) s += T(L"Zindan adı    : ") + objectName + L"\n";
    s += FormatW(T(L"Nesne türü    : %u\n"), objectType);
    s += T(L"Bölge         : ") + region.toStringTr() + L"\n";
    s += T(L"Blok (oda)    : ") + FormatCountTr(blocks.size()) + L"\n";
    s += T(L"Nesne         : ") + FormatCountTr(objectCount()) + L"\n";
    s += T(L"Işık          : ") + FormatCountTr(lightCount()) + L"\n";
    s += T(L"Bağlantı      : ") + FormatCountTr(linkCount) + L"\n";
    s += T(L"Grup          : ") + FormatCountTr(groups.empty() ? groupCount : groups.size()) + L"\n";
    if (!floorNames.empty()) s += T(L"Kat           : ") + FormatCountTr(floorNames.size()) + L"\n";
    if (!roomNames.empty())  s += T(L"Oda adı       : ") + FormatCountTr(roomNames.size()) + L"\n";
    if (collisionBox0.valid()) {
        const Vec3 e = collisionBox0.extent();
        s += FormatW(T(L"Sınır kutusu  : %.1f x %.1f x %.1f\n"), e.x, e.y, e.z);
    }
    if (partial) s += T(L"UYARI         : Dosya eksik okundu.\n");

    // Blocks with WHERE they are - a dungeon is a layout, so the position is the
    // interesting part, not just the file name.
    const size_t shown = (blocks.size() < 12) ? blocks.size() : 12;
    if (shown) s += T(L"\nBloklar (ilk ") + FormatCountTr(shown) + T(L"):\n");
    for (size_t i = 0; i < shown; ++i) {
        const DofBlock& b = blocks[i];
        s += FormatW(L"    %-28s (%.0f, %.0f, %.0f)  %.0f°",
                     b.name.c_str(), b.position.x, b.position.y, b.position.z,
                     b.yaw * 57.2957795f);
        if (!b.objects.empty()) s += FormatW(T(L"  %zu nesne"), b.objects.size());
        if (b.isEntrance)       s += T(L"  [giriş]");
        s += L"\n        " + b.path + L"\n";
    }
    if (blocks.size() > shown)
        s += T(L"    ... ve ") + FormatCountTr(blocks.size() - shown) + T(L" blok daha\n");

    if (!floorNames.empty()) {
        s += T(L"\nKatlar: ");
        for (size_t i = 0; i < floorNames.size() && i < 12; ++i) {
            if (i) s += L", ";
            s += floorNames[i];
        }
        s += L"\n";
    }
    return s;
}

// ===========================================================================
//  .m - JMXVMAPM1000
// ===========================================================================
Result ParseMapMesh(const u8* data, size_t size, MapMeshFile& out) {
    out = MapMeshFile();

    if (!data || size < 12)
        return Result::Fail(Status::Truncated, T(L".m: dosya 12 bayttan kısa."));

    ByteReader r(data, size);
    out.signature = r.readSignature(12);
    if (out.signature.compare(0, 8, "JMXVMAPM") != 0)
        return Result::Fail(Status::BadFormat, T(L".m: imza \"JMXVMAPM\" değil."));

    const int blockTotal = kMapBlocksPerSide * kMapBlocksPerSide;
    out.blocks.reserve(static_cast<size_t>(blockTotal));

    f32 lo = 1e30f, hi = -1e30f;

    for (int b = 0; b < blockTotal; ++b) {
        if (!r.canRead(kMapMeshBlockBytes)) { out.partial = true; break; }

        MapMeshBlock blk;
        blk.flag          = r.readU32();
        blk.environmentId = r.readU16();

        for (int i = 0; i < kMapVerticesPerSide * kMapVerticesPerSide; ++i) {
            blk.heights[i]    = r.readF32();
            blk.textures[i]   = r.readU16();
            blk.brightness[i] = r.readU8();
            if (blk.heights[i] < lo) lo = blk.heights[i];
            if (blk.heights[i] > hi) hi = blk.heights[i];
        }

        blk.waterType     = r.readI8();
        blk.waterWaveType = r.readU8();
        blk.waterHeight   = r.readF32();

        for (int i = 0; i < kMapTilesPerSide * kMapTilesPerSide; ++i)
            blk.tileFlags[i] = r.readU16();

        blk.heightMax = r.readF32();
        blk.heightMin = r.readF32();
        r.skip(20);                                   // reserved

        if (r.failed()) { out.partial = true; break; }
        out.blocks.push_back(blk);
    }

    if (!out.blocks.empty()) { out.globalMin = lo; out.globalMax = hi; }

    if (out.partial)
        return Result::Fail(Status::Truncated,
                            L".m: " + FormatCountTr(out.blocks.size()) + T(L"/36 blok okunabildi."));
    return Result::Ok();
}

Image MapMeshFile::toHeightImage() const {
    const int side = kMapBlocksPerSide * kMapVerticesPerSide;   // 102
    Image img(side, side);
    if (img.empty() || blocks.empty()) return img;

    const f32 range = (globalMax > globalMin) ? (globalMax - globalMin) : 1.0f;

    for (int bz = 0; bz < kMapBlocksPerSide; ++bz) {
        for (int bx = 0; bx < kMapBlocksPerSide; ++bx) {
            const size_t bi = static_cast<size_t>(bz) * kMapBlocksPerSide + bx;
            if (bi >= blocks.size()) continue;
            const MapMeshBlock& blk = blocks[bi];

            for (int vz = 0; vz < kMapVerticesPerSide; ++vz) {
                for (int vx = 0; vx < kMapVerticesPerSide; ++vx) {
                    const f32 h = blk.heights[vz * kMapVerticesPerSide + vx];
                    f32 t = (h - globalMin) / range;
                    if (t < 0.0f) t = 0.0f;
                    if (t > 1.0f) t = 1.0f;
                    const u8 g = static_cast<u8>(t * 255.0f + 0.5f);
                    // Image is top-down; the map's +Z runs "up", so flip Z.
                    const int px = bx * kMapVerticesPerSide + vx;
                    const int py = side - 1 - (bz * kMapVerticesPerSide + vz);
                    img.set(px, py, Rgba(g, g, g, 255));
                }
            }
        }
    }
    return img;
}

std::wstring MapMeshFile::summaryTr() const {
    std::wstring s;
    s += T(L"İmza          : ") + MultiByteToWide(signature, 1252) + L"\n";
    s += T(L"Blok sayısı   : ") + FormatCountTr(blocks.size()) + T(L" / 36 (6 x 6)\n");
    s += FormatW(T(L"Köşe / blok   : %d x %d\n"), kMapVerticesPerSide, kMapVerticesPerSide);
    s += FormatW(T(L"Yükseklik     : %.2f .. %.2f\n"), globalMin, globalMax);

    size_t culled = 0, water = 0, ice = 0;
    for (const MapMeshBlock& b : blocks) {
        if (b.flag & 1) ++culled;
        if (b.waterType == 0) ++water;
        if (b.waterType == 1) ++ice;
    }
    s += T(L"Gizlenen blok : ") + FormatCountTr(culled) + L"\n";
    s += T(L"Su / buz blok : ") + FormatCountTr(water) + L" / " + FormatCountTr(ice) + L"\n";

    if (!blocks.empty()) {
        s += FormatW(T(L"Ortam kimliği : %u (ilk blok)\n"), blocks[0].environmentId);
        s += FormatW(T(L"Blok 0 yük.   : min %.2f  max %.2f\n"),
                     blocks[0].heightMin, blocks[0].heightMax);
    }
    if (partial) s += T(L"UYARI         : Dosya eksik okundu.\n");
    return s;
}

// ===========================================================================
//  .t - JMXVMAPT1001
// ===========================================================================
Result ParseMapTexture(const u8* data, size_t size, MapTextureFile& out) {
    out = MapTextureFile();

    if (!data || size < 12 + kMapTileLightMapBytes + 8)
        return Result::Fail(Status::Truncated, T(L".t: dosya beklenenden kısa."));

    ByteReader r(data, size);
    out.signature = r.readSignature(12);
    if (out.signature.compare(0, 8, "JMXVMAPT") != 0)
        return Result::Fail(Status::BadFormat, T(L".t: imza \"JMXVMAPT\" değil."));

    out.tileLightMap.resize(kMapTileLightMapBytes);
    if (!r.readBytes(out.tileLightMap.data(), kMapTileLightMapBytes)) {
        out.tileLightMap.clear();
        return Result::Fail(Status::Truncated, T(L".t: tile ışık haritası eksik."));
    }

    out.lightMapBufferSize  = r.readU32();
    out.lightMapTextureType = r.readU32();
    if (r.failed())
        return Result::Fail(Status::Truncated, T(L".t: doku başlığı eksik."));

    out.ddsOffset = r.pos();
    const size_t avail = r.remaining();
    // Never trust the declared size: clamp it to what is actually present. The
    // per-tile lightMap (which the preview visualises) is already fully read, so
    // a short/oversized embedded DDS is only a soft note - NOT a parse failure.
    out.ddsSize = (out.lightMapBufferSize <= avail)
                ? static_cast<size_t>(out.lightMapBufferSize)
                : avail;
    return Result::Ok();
}

Image MapTextureFile::toTileLightImage() const {
    Image img(96, 96);
    if (img.empty() || tileLightMap.size() < kMapTileLightMapBytes) return img;
    for (int y = 0; y < 96; ++y) {
        for (int x = 0; x < 96; ++x) {
            const u8 v = tileLightMap[static_cast<size_t>(y) * 96 + x];
            img.set(x, 95 - y, Rgba(v, v, v, 255));
        }
    }
    return img;
}

std::wstring MapTextureFile::summaryTr() const {
    const wchar_t* typeName = T(L"bilinmeyen");
    switch (lightMapTextureType) {
        case 3: typeName = T(L"Doku");        break;
        case 4: typeName = T(L"Hacim dokusu"); break;
        case 5: typeName = T(L"Küp dokusu");  break;
        default: break;
    }
    std::wstring s;
    s += T(L"İmza          : ") + MultiByteToWide(signature, 1252) + L"\n";
    s += T(L"Tile ışık hrt.: ") + FormatCountTr(tileLightMap.size()) + T(L" bayt (96 x 96)\n");
    s += FormatW(T(L"Doku türü     : %u (%s)\n"), lightMapTextureType, typeName);
    s += T(L"Doku boyutu   : ") + FormatBytesTr(lightMapBufferSize) + L"\n";
    s += T(L"Gömülü DDS    : ofset ") + FormatCountTr(ddsOffset) + L", " +
         FormatBytesTr(ddsSize) + L"\n";

    if (tileLightMap.size() >= kMapTileLightMapBytes) {
        u32 sum = 0; u8 lo = 255, hi = 0;
        for (u8 v : tileLightMap) { sum += v; if (v < lo) lo = v; if (v > hi) hi = v; }
        s += FormatW(T(L"Işık değerleri: min %u, max %u, ort %u\n"),
                     lo, hi, static_cast<unsigned>(sum / tileLightMap.size()));
    }
    return s;
}

// ===========================================================================
//  .o / .o2 - JMXVMAPO1001
// ===========================================================================
namespace {

/// One parse attempt with a fixed layout. Returns false the moment anything
/// does not fit, so the caller can try the other layout.
/// One parse attempt with an EXPLICIT (group count, record size) layout.
///
/// The wiki shows one count per block, but the shipped JMXVMAPO1001 .o files
/// actually store FOUR LOD groups per block (144 counts, not 36) - assuming one
/// dropped ~78% of every region's objects and left blockX/blockZ wrong. So the
/// group count is probed, not inferred from the extension: only the record size
/// differs between .o (28 bytes) and .o2 (+2 for regionId).
bool TryParseMapObject(const u8* data, size_t size, bool o2, int lodGroups,
                       std::vector<MapObjectInstance>& out, size_t& consumed) {
    out.clear();
    consumed = 0;

    ByteReader r(data, size);
    r.skip(12);                                   // signature
    if (r.failed()) return false;

    const size_t recordBytes = o2 ? kMapObjectRecordO2 : kMapObjectRecordO;
    if (lodGroups < 1) return false;

    for (int bz = 0; bz < kMapBlocksPerSide; ++bz) {
        for (int bx = 0; bx < kMapBlocksPerSide; ++bx) {
            for (int lod = 0; lod < lodGroups; ++lod) {
                const u16 count = r.readU16();
                if (r.failed()) return false;
                if (count > r.remaining() / recordBytes) return false;

                for (u16 i = 0; i < count; ++i) {
                    MapObjectInstance o;
                    o.objectId      = r.readU32();
                    o.localPosition = r.readVec3();
                    o.isStatic      = r.readI16();
                    o.yaw           = r.readF32();
                    o.uid           = r.readI16();
                    o.short0        = r.readI16();
                    o.isBig         = r.readBool8();
                    o.isStruct      = r.readBool8();
                    if (o2) o.regionId = r.readU16();
                    o.lodGroup = lod;
                    o.blockX   = bx;
                    o.blockZ   = bz;
                    if (r.failed()) return false;
                    out.push_back(o);
                }
            }
        }
    }
    consumed = r.pos();
    return !r.failed();
}

} // namespace

Result ParseMapObject(const u8* data, size_t size, int forceO2, MapObjectFile& out) {
    out = MapObjectFile();

    if (!data || size < 12)
        return Result::Fail(Status::Truncated, T(L".o: dosya 12 bayttan kısa."));

    ByteReader sig(data, size);
    out.signature = sig.readSignature(12);
    if (out.signature.compare(0, 8, "JMXVMAPO") != 0)
        return Result::Fail(Status::BadFormat, T(L".o: imza \"JMXVMAPO\" değil."));

    // Probe the real layouts and accept only one that consumes the buffer
    // EXACTLY. Order matters: the four-group forms are what ships, the 1-group
    // form is the (wrong) spec literal and is the last resort.
    struct Candidate { bool o2; int groups; };
    static const Candidate kCandidates[] = {
        { false, 4 },   // JMXVMAPO1001 .o   - the common case
        { true,  4 },   // .o2
        { false, 3 },   // JMXVMAPO1000 (a handful of legacy files)
        { false, 1 },   // as documented
        { true,  1 },
    };

    if (forceO2 < 0) {
        std::vector<MapObjectInstance> best;
        size_t bestConsumed = 0;
        bool   bestO2 = false, haveExact = false;

        for (const Candidate& c : kCandidates) {
            std::vector<MapObjectInstance> v;
            size_t consumed = 0;
            if (!TryParseMapObject(data, size, c.o2, c.groups, v, consumed)) continue;
            if (consumed == size) {                       // exact = correct
                out.isO2 = c.o2;
                out.objects.swap(v);
                return Result::Ok();
            }
            if (!haveExact && v.size() > best.size()) {   // best-effort fallback
                best.swap(v); bestConsumed = consumed; bestO2 = c.o2;
            }
        }
        (void)bestConsumed;
        if (best.empty())
            return Result::Fail(Status::BadFormat,
                                T(L".o: yerleşim bilinen düzenlerin hiçbirine uymuyor."));
        out.isO2 = bestO2;
        out.objects.swap(best);
        out.partial = true;
        return Result::Fail(Status::Truncated,
                            T(L".o: dosya tam tüketilemedi - kısmi olarak okundu."));
    }

    out.isO2 = (forceO2 != 0);
    for (int groups : { 4, 3, 1 }) {
        std::vector<MapObjectInstance> v;
        size_t consumed = 0;
        if (TryParseMapObject(data, size, out.isO2, groups, v, consumed) && consumed == size) {
            out.objects.swap(v);
            return Result::Ok();
        }
    }
    size_t consumed = 0;
    if (!TryParseMapObject(data, size, out.isO2, 4, out.objects, consumed))
        return Result::Fail(Status::Truncated, T(L".o: nesne listesi eksik veya bozuk."));
    if (consumed != size) out.partial = true;
    return Result::Ok();
}

std::wstring MapObjectFile::summaryTr() const {
    std::wstring s;
    s += T(L"İmza          : ") + MultiByteToWide(signature, 1252) + L"\n";
    s += T(L"Düzen         : ") + std::wstring(isO2 ? T(L".o2 (bölge kimlikli kayıt)")
                                                 : L".o") + L"\n";
    s += T(L"Nesne sayısı  : ") + FormatCountTr(objects.size()) + L"\n";
    s += T(L"Blok          : 6 x 6\n");

    size_t big = 0, structures = 0;
    for (const MapObjectInstance& o : objects) {
        if (o.isBig) ++big;
        if (o.isStruct) ++structures;
    }
    s += T(L"Büyük nesne   : ") + FormatCountTr(big) + L"\n";
    s += T(L"Yapı (struct) : ") + FormatCountTr(structures) + L"\n";
    if (partial) s += T(L"UYARI         : Dosya eksik okundu.\n");

    const size_t shown = (objects.size() < 10) ? objects.size() : 10;
    if (shown) s += T(L"\nNesneler (ilk ") + FormatCountTr(shown) + T(L"):\n");
    for (size_t i = 0; i < shown; ++i) {
        const MapObjectInstance& o = objects[i];
        s += FormatW(T(L"    obj #%u  blok(%d,%d) lod %d  konum(%.1f, %.1f, %.1f)  yaw %.2f\n"),
                     o.objectId, o.blockX, o.blockZ, o.lodGroup,
                     o.localPosition.x, o.localPosition.y, o.localPosition.z, o.yaw);
    }
    if (objects.size() > shown)
        s += T(L"    ... ve ") + FormatCountTr(objects.size() - shown) + T(L" nesne daha\n");
    return s;
}

// ===========================================================================
//  .mfo - JMXVMFO 1000
// ===========================================================================
Result ParseMfo(const u8* data, size_t size, MfoFile& out) {
    out = MfoFile();

    if (!data || size < 12 + 12 + static_cast<size_t>(kMfoRegionBytes))
        return Result::Fail(Status::Truncated, T(L".mfo: dosya beklenenden kısa."));

    ByteReader r(data, size);
    out.signature = r.readSignature(12);
    if (out.signature.compare(0, 7, "JMXVMFO") != 0)
        return Result::Fail(Status::BadFormat, T(L".mfo: imza \"JMXVMFO\" değil."));

    out.mapWidth  = r.readI16();
    out.mapHeight = r.readI16();
    out.short0    = r.readI16();
    out.short1    = r.readI16();
    out.short2    = r.readI16();
    out.short3    = r.readI16();

    out.regionBits.resize(kMfoRegionBytes);
    if (!r.readBytes(out.regionBits.data(), kMfoRegionBytes)) {
        out.regionBits.clear();
        return Result::Fail(Status::Truncated, T(L".mfo: bölge bit haritası eksik."));
    }
    return Result::Ok();
}

bool MfoFile::regionEnabled(int x, int z) const {
    if (x < 0 || z < 0 || x > 255 || z > 255) return false;
    const size_t bit  = static_cast<size_t>(z) * 256 + static_cast<size_t>(x);
    const size_t byte = bit / 8;
    if (byte >= regionBits.size()) return false;
    return ((regionBits[byte] >> (bit % 8)) & 1) != 0;
}

Image MfoFile::toRegionImage() const {
    Image img(256, 256);
    if (img.empty() || regionBits.empty()) return img;
    for (int z = 0; z < 256; ++z)
        for (int x = 0; x < 256; ++x)
            img.set(x, 255 - z, regionEnabled(x, z) ? Rgba(255, 255, 255, 255)
                                                    : Rgba(0, 0, 0, 255));
    return img;
}

std::wstring MfoFile::summaryTr() const {
    size_t active = 0;
    for (u8 b : regionBits) {
        u8 v = b;
        while (v) { active += (v & 1); v = static_cast<u8>(v >> 1); }
    }
    std::wstring s;
    s += T(L"İmza          : ") + MultiByteToWide(signature, 1252) + L"\n";
    s += FormatW(T(L"Harita boyutu : %d x %d bölge\n"), mapWidth, mapHeight);
    s += T(L"Bit haritası  : ") + FormatCountTr(regionBits.size()) + T(L" bayt (256 x 256 bit)\n");
    s += T(L"Etkin bölge   : ") + FormatCountTr(active) + L"\n";
    s += FormatW(T(L"Ek alanlar    : %d, %d, %d, %d\n"), short0, short1, short2, short3);
    return s;
}

// ===========================================================================
//  .cpd - JMXVCPD 0101
// ===========================================================================
Result ParseCpd(const u8* data, size_t size, CpdFile& out) {
    out = CpdFile();

    if (!data || size < 48)
        return Result::Fail(Status::Truncated, T(L".cpd: dosya 48 bayttan kısa."));

    ByteReader r(data, size);
    out.signature = r.readSignature(12);
    if (out.signature.compare(0, 7, "JMXVCPD") != 0)
        return Result::Fail(Status::BadFormat, T(L".cpd: imza \"JMXVCPD\" değil."));

    out.collisionOffset = r.readU32();
    out.resourceOffset  = r.readU32();
    for (int i = 0; i < 5; ++i) out.unk[i] = r.readU32();

    out.objectType = r.readU32();
    out.objectName = r.readLenPrefixedStringW(kPathCodePage, 4096);
    (void)r.readU32();
    (void)r.readU32();

    if (r.failed())
        return Result::Fail(Status::Truncated, T(L".cpd: başlık eksik."));

    bool partial = false;

    r.clearFail();
    if (r.seekSection(out.collisionOffset)) {
        out.collisionResourcePath = r.readLenPrefixedStringW(kPathCodePage, 4096);
        if (r.failed()) partial = true;
    }

    r.clearFail();
    if (r.seekSection(out.resourceOffset)) {
        const u32 n = r.readU32();
        if (!r.failed() && n <= r.remaining() / 4) {
            out.resourcePaths.reserve(n);
            for (u32 i = 0; i < n; ++i) {
                out.resourcePaths.push_back(r.readLenPrefixedStringW(kPathCodePage, 4096));
                if (r.failed()) { out.resourcePaths.pop_back(); partial = true; break; }
            }
        } else {
            partial = true;
        }
    }
    r.clearFail();

    if (partial)
        return Result::Fail(Status::Truncated, T(L".cpd: kaynak listesi kısmi okundu."));
    return Result::Ok();
}

std::wstring CpdFile::summaryTr() const {
    std::wstring s;
    s += T(L"İmza          : ") + MultiByteToWide(signature, 1252) + L"\n";
    if (!objectName.empty()) s += T(L"Nesne adı     : ") + objectName + L"\n";
    s += FormatW(T(L"Nesne türü    : %u\n"), objectType);
    if (!collisionResourcePath.empty())
        s += T(L"Çarpışma      : ") + collisionResourcePath + L"\n";
    s += T(L"Kaynak sayısı : ") + FormatCountTr(resourcePaths.size()) + L"\n";
    const size_t shown = (resourcePaths.size() < 24) ? resourcePaths.size() : 24;
    for (size_t i = 0; i < shown; ++i) s += L"    " + resourcePaths[i] + L"\n";
    if (resourcePaths.size() > shown)
        s += T(L"    ... ve ") + FormatCountTr(resourcePaths.size() - shown) + T(L" kaynak daha\n");
    return s;
}

// ===========================================================================
//  .ifo - text variants
// ===========================================================================
Result ParseIfo(const u8* data, size_t size, const std::wstring& fileName, IfoFile& out) {
    out = IfoFile();

    if (!data || size == 0)
        return Result::Fail(Status::Truncated, T(L".ifo: dosya boş."));

    // These files are plain 8-bit text; a BOM-ed UTF-16 variant exists on some
    // shards, so route through the same decoder the text viewer uses.
    std::wstring text;
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        text.assign(reinterpret_cast<const wchar_t*>(data + 2), (size - 2) / 2);
    } else {
        text = MultiByteToWide(reinterpret_cast<const char*>(data), size, kPathCodePage);
    }
    const size_t nul = text.find(L'\0');
    if (nul != std::wstring::npos) text.resize(nul);

    // Split into lines.
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find(L'\n', start);
        const bool last = (nl == std::wstring::npos);
        if (last) nl = text.size();
        std::wstring line = text.substr(start, nl - start);
        while (!line.empty() && (line.back() == L'\r')) line.pop_back();
        lines.push_back(std::move(line));
        if (last) break;
        start = nl + 1;
    }

    // --- variant from the file name ----------------------------------------
    const std::wstring name = ToLowerW(PathFileName(fileName));
    if      (name == L"object.ifo")       out.variant = IfoVariant::Object;
    else if (name == L"objectstring.ifo") out.variant = IfoVariant::ObjectString;
    else if (name == L"objext.ifo")       out.variant = IfoVariant::ObjectExt;
    else if (name == L"tile2d.ifo")       out.variant = IfoVariant::Tile2d;
    else if (name == L"environment.ifo")  out.variant = IfoVariant::Environment;

    if (!lines.empty()) out.header = TrimW(lines[0]);
    if (lines.size() > 1) {
        const std::wstring c = TrimW(lines[1]);
        out.declaredCount = static_cast<u32>(_wtoi64(c.c_str()));
    }

    // --- rows ---------------------------------------------------------------
    for (size_t i = 2; i < lines.size(); ++i) {
        const std::wstring trimmed = TrimW(lines[i]);
        if (trimmed.empty()) continue;
        if (trimmed.size() >= 2 && trimmed[0] == L'/' && trimmed[1] == L'/') continue;
        IfoRow row;
        row.fields = SplitIfoLine(lines[i]);
        if (row.fields.empty()) continue;
        out.rows.push_back(std::move(row));
    }

    // --- variant from the shape of the data, when the name did not say -------
    if (out.variant == IfoVariant::Unknown && !out.rows.empty()) {
        const size_t n = out.rows[0].fields.size();
        if      (n == 3)  out.variant = IfoVariant::Object;        // idx flag "path"
        else if (n == 9)  out.variant = IfoVariant::ObjectString;
        else if (n >= 4 && n <= 6) out.variant = IfoVariant::Tile2d;
        else if (n == 2)  out.variant = IfoVariant::ObjectExt;
    }

    switch (out.variant) {
        case IfoVariant::Object:
            out.columnHeadersTr = { T(L"Sıra"), T(L"Bayrak"), T(L"Yol") };
            break;
        case IfoVariant::ObjectString:
            out.columnHeadersTr = { T(L"Harita UID"), T(L"Bayrak"), T(L"X sektör"), T(L"Y sektör"),
                                    L"X", L"Y", L"Z", L"Yaw", T(L"Ad") };
            break;
        case IfoVariant::ObjectExt:
            out.columnHeadersTr = { L"UID", T(L"(boş)"), T(L"Ad") };
            break;
        case IfoVariant::Tile2d:
            out.columnHeadersTr = { T(L"Sıra"), T(L"Tür"), T(L"Kategori"), T(L"Dosya"), T(L"3D çim") };
            break;
        default:
            break;
    }
    if (out.columnHeadersTr.empty() && !out.rows.empty()) {
        for (size_t i = 0; i < out.rows[0].fields.size(); ++i)
            out.columnHeadersTr.push_back(FormatW(T(L"Sütun %zu"), i + 1));
    }

    if (out.rows.empty())
        return Result::Fail(Status::BadFormat, T(L".ifo: veri satırı bulunamadı."));
    return Result::Ok();
}

std::wstring IfoFile::summaryTr() const {
    const wchar_t* variantName = T(L"bilinmeyen");
    switch (variant) {
        case IfoVariant::Object:       variantName = T(L"Nesne listesi (object.ifo)"); break;
        case IfoVariant::ObjectString: variantName = T(L"Nesne etiketleri (objectstring.ifo)"); break;
        case IfoVariant::ObjectExt:    variantName = T(L"Nesne ek bilgisi (objext.ifo)"); break;
        case IfoVariant::Tile2d:       variantName = T(L"Zemin doku listesi (tile2d.ifo)"); break;
        case IfoVariant::Environment:  variantName = T(L"Ortam listesi (environment.ifo)"); break;
        default: break;
    }
    std::wstring s;
    s += T(L"Tür           : ") + std::wstring(variantName) + L"\n";
    s += T(L"Başlık        : ") + header + L"\n";
    s += T(L"Bildirilen    : ") + FormatCountTr(declaredCount) + T(L" kayıt\n");
    s += T(L"Okunan        : ") + FormatCountTr(rows.size()) + T(L" satır\n");
    if (!rows.empty())
        s += T(L"Sütun sayısı  : ") + FormatCountTr(rows[0].fields.size()) + L"\n";
    if (declaredCount && declaredCount != rows.size())
        s += T(L"UYARI         : Bildirilen sayı ile okunan satır sayısı farklı.\n");

    const size_t shown = (rows.size() < 8) ? rows.size() : 8;
    if (shown) s += T(L"\nİlk satırlar:\n");
    for (size_t i = 0; i < shown; ++i) {
        s += L"    ";
        for (size_t f = 0; f < rows[i].fields.size(); ++f) {
            if (f) s += L" | ";
            s += rows[i].fields[f];
        }
        s += L"\n";
    }
    return s;
}

// ===========================================================================
//  JMXVENVI1003 - binary environment set
// ===========================================================================
Result ParseEnvi(const u8* data, size_t size, EnviFile& out) {
    out = EnviFile();

    if (!data || size < 18)
        return Result::Fail(Status::Truncated, T(L"ENVI: dosya 18 bayttan kısa."));

    ByteReader r(data, size);
    out.signature = r.readSignature(12);
    out.version   = SigToW(out.signature);
    if (out.signature.compare(0, 8, "JMXVENVI") != 0)
        return Result::Fail(Status::BadFormat, T(L"ENVI: imza \"JMXVENVI\" değil."));

    // The version digits decide which graphs exist.
    int fileVersion = 1000;
    if (out.signature.size() >= 12) {
        int v = 0; bool numeric = true;
        for (size_t i = 8; i < 12; ++i) {
            const char c = out.signature[i];
            if (c < '0' || c > '9') { numeric = false; break; }
            v = v * 10 + (c - '0');
        }
        if (numeric) fileVersion = v;
    }

    out.profileCount        = r.readI16();
    out.environmentSetName  = r.readLenPrefixedStringW(kPathCodePage, 4096);
    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"ENVI: başlık eksik."));
    if (out.profileCount < 0)
        return Result::Fail(Status::BadFormat, T(L"ENVI: profil sayısı negatif."));

    // Graph order is fixed; entries that a given version does not have are
    // simply skipped (their key count stays 0).
    struct GraphDef { bool isColor; int minVersion; };
    static const GraphDef kGraphs[16] = {
        { true,  1000 },  // SunColor
        { true,  1000 },  // SkyTopColor
        { true,  1000 },  // DiffuseColor
        { true,  1000 },  // ObjectAmbientColor
        { true,  1000 },  // Graph4
        { true,  1000 },  // TerrainAmbientColor
        { true,  1003 },  // TerrainShadowColor
        { false, 1000 },  // FogNearPlane
        { false, 1000 },  // FogFarPlane
        { true,  1000 },  // FogColor
        { false, 1000 },  // Graph10
        { false, 1000 },  // Graph11
        { false, 1001 },  // Graph12
        { true,  1002 },  // SkyBottomColor
        { true,  1002 },  // WaterColor
        { false, 1002 },  // Graph15
    };

    for (i16 p = 0; p < out.profileCount; ++p) {
        EnviProfile prof;
        prof.id       = r.readU16();
        prof.name     = r.readLenPrefixedStringW(kPathCodePage, 1024);
        prof.dayBgm   = r.readLenPrefixedStringW(kPathCodePage, 1024);
        prof.nightBgm = r.readLenPrefixedStringW(kPathCodePage, 1024);
        if (r.failed()) { out.partial = true; break; }

        for (int g = 0; g < 16; ++g) {
            if (fileVersion < kGraphs[g].minVersion) continue;
            prof.graphKeyCounts[g] = SkipGraph(r, kGraphs[g].isColor ? 16u : 8u);
            if (r.failed()) break;
        }
        if (r.failed()) { out.partial = true; out.profiles.push_back(std::move(prof)); break; }

        out.profiles.push_back(std::move(prof));
    }

    if (out.partial)
        return Result::Fail(Status::Truncated,
                            T(L"ENVI: ") + FormatCountTr(out.profiles.size()) + L"/" +
                            FormatCountTr(static_cast<u64>(out.profileCount)) +
                            T(L" profil okunabildi."));
    return Result::Ok();
}

std::wstring EnviFile::summaryTr() const {
    std::wstring s;
    s += T(L"İmza          : ") + MultiByteToWide(signature, 1252) + L"\n";
    s += T(L"Profil sayısı : ") + FormatCountTr(profiles.size()) + L" / " +
         FormatCountTr(static_cast<u64>(profileCount < 0 ? 0 : profileCount)) + L"\n";
    if (!environmentSetName.empty())
        s += T(L"Set adı       : ") + environmentSetName + L"\n";
    if (partial) s += T(L"UYARI         : Dosya eksik okundu.\n");

    const size_t shown = (profiles.size() < 12) ? profiles.size() : 12;
    if (shown) s += T(L"\nProfiller:\n");
    for (size_t i = 0; i < shown; ++i) {
        const EnviProfile& p = profiles[i];
        u32 keys = 0;
        for (u32 k : p.graphKeyCounts) keys += k;
        s += FormatW(L"    #%u  ", p.id) + p.name +
             L"  (" + FormatCountTr(keys) + T(L" grafik anahtarı)\n");
        if (!p.dayBgm.empty())   s += T(L"        gündüz müzik : ") + p.dayBgm + L"\n";
        if (!p.nightBgm.empty()) s += T(L"        gece müzik   : ") + p.nightBgm + L"\n";
    }
    if (profiles.size() > shown)
        s += T(L"    ... ve ") + FormatCountTr(profiles.size() - shown) + T(L" profil daha\n");
    return s;
}

} // namespace sro
