// ============================================================================
//  SRO Asset Studio - formats/Bsr.cpp
//
//  JMXVRES 0109 resource parser + model composer.  Wiki: JMXVRES.md
//
//  ParseBsr        - pure structure, never touches the archive.
//  ComposeBsrModel - pulls the referenced .bms / .bmt / .ddj back out of
//                    whatever container the caller has open (AssetLoader) and
//                    produces something renderable.
//
//  Path resolution is the fiddly part: .bsr files reference their meshes with
//  a path that is usually rooted at the data folder ("res\bldg\...\x.bms") but
//  is sometimes relative to the .bsr itself. Both are tried, in that order.
// ============================================================================
#include "Bsr.h"
#include "../core/I18n.h"   // UI strings follow the chosen language
#include "Bsk.h"
#include "Ddj.h"

#include <cmath>
#include <cstdlib>

namespace sro {

namespace {

constexpr UINT kPathCodePage = 1252;

/// Silkroad's own limit: MATERIALSET_MAXCOUNTMAX = 5.
constexpr u32 kMaxMaterialSets = 5;

/// Try `path` as given, then relative to `baseFolder`. Returns true and fills
/// `outData` / `outResolved` on the first hit.
bool LoadAsset(const AssetLoader& loader,
               const std::wstring& path,
               const std::wstring& baseFolder,
               std::vector<u8>& outData,
               std::wstring& outResolved) {
    if (!loader || path.empty()) return false;

    const std::wstring direct = NormalizeInnerPath(path);
    if (!direct.empty() && loader(direct, outData)) { outResolved = direct; return true; }

    if (!baseFolder.empty()) {
        const std::wstring joined = NormalizeInnerPath(PathJoin(baseFolder, path));
        if (joined != direct && loader(joined, outData)) { outResolved = joined; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Avatar-set attach placement
//
//  Skinned .bms vertices are already stored in bind-pose WORLD space, so every
//  model (character, mob, weapon) draws its meshes RAW. The one exception is an
//  avatar attach piece (wing/cape): it is authored around its OWN root (0,0,0)
//  and in game its root rides the CHARACTER's Bip01 Spine1. A costume preview
//  has no character body, so we borrow a real character skeleton (see
//  LoadAvatarCharSkeleton) and apply the attach bone's bind-pose world matrix -
//  the exact placement the game uses. `PlaceAttachOnBack` is only a fallback
//  when no character skeleton can be found.
// ---------------------------------------------------------------------------
Vec3 RotateByQuat(const Quat& q, const Vec3& v) {
    const Vec3 u(q.x, q.y, q.z);
    const Vec3 t = u.cross(v) * 2.0f;
    return v + t * q.w + u.cross(t);
}
const BskBone* FindBoneByName(const BskFile& skel, const std::wstring& name) {
    const std::wstring want = ToLowerW(name);
    for (const BskBone& b : skel.bones)
        if (ToLowerW(b.name) == want) return &b;
    return nullptr;
}

// Fallback for when no character skeleton is reachable: estimate the attach
// bone's position from the composed body box instead. Pure translation, exactly
// like ApplyCharacterAttach - the piece is already correctly oriented, only its
// height is unknown. (Measured Bip01 Spine1 sits at ~0.68 of body height, a
// fraction of a unit behind the spine.)
void PlaceAttachOnBack(ComposedModel& m, const Aabb& body) {
    if (!body.valid() || m.parts.empty()) return;
    const float bodyH = body.max.y - body.min.y;
    if (bodyH <= 1e-3f) return;
    const Vec3 t((body.min.x + body.max.x) * 0.5f,
                 body.min.y + 0.68f * bodyH,
                 0.016f * bodyH);
    Aabb nb;
    for (ComposedPart& part : m.parts) {
        part.rigidPlaced = true;          // moved out of its own bind space
        for (BmsVertex& v : part.mesh.vertices) {
            v.position = v.position + t;
            nb.add(v.position);
        }
    }
    m.bounds = nb;
}

/// Sweep the raw file for printable runs that end in a known asset extension.
/// Used only after the structured parse produced nothing - see the call site.
void RecoverRefsByScan(const u8* data, size_t size, BsrFile& out) {
    if (!data || size < 16) return;

    std::string run;
    auto flush = [&]() {
        if (run.size() >= 8) {
            std::string low = run;
            for (char& c : low) c = (char)tolower((unsigned char)c);
            const bool isBms = low.size() > 4 && low.compare(low.size() - 4, 4, ".bms") == 0;
            const bool isBmt = low.size() > 4 && low.compare(low.size() - 4, 4, ".bmt") == 0;
            if (isBms || isBmt) {
                // Trim anything before the first path character of the token.
                size_t b = 0;
                while (b < run.size() &&
                       !(isalnum((unsigned char)run[b]) || run[b] == '_')) ++b;
                const std::wstring path = MultiByteToWide(run.substr(b), kPathCodePage);
                if (!path.empty()) {
                    if (isBms) { BsrMeshRef m; m.path = path; out.meshes.push_back(std::move(m)); }
                    else {
                        BsrMaterialSet ms;
                        ms.id = static_cast<u32>(out.materialSets.size());
                        ms.path = path;
                        out.materialSets.push_back(std::move(ms));
                    }
                    out.refsRecoveredByScan = true;
                }
            }
        }
        run.clear();
    };

    // A Silkroad inner path is well under 260 chars; cap the run so an all-
    // printable (crafted or corrupt) .bsr cannot grow one string to the whole
    // file size. A path longer than this is not a real reference - flush the
    // useful tail and drop the leading overflow rather than accumulating it.
    constexpr size_t kMaxRun = 1024;
    for (size_t i = 0; i < size; ++i) {
        const u8 c = data[i];
        if (c >= 0x20 && c <= 0x7E) {
            if (run.size() >= kMaxRun) run.erase(0, run.size() - (kMaxRun / 2));
            run.push_back((char)c);
        } else {
            flush();
        }
    }
    flush();
}

} // namespace

// ---------------------------------------------------------------------------
// IsBsr
// ---------------------------------------------------------------------------
bool IsBsr(const u8* data, size_t size) {
    return data && size >= 7 && std::memcmp(data, kBsrSignaturePrefix, 7) == 0;
}

// ---------------------------------------------------------------------------
// ParseBsr
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Mod palette
//
// Layout at header.modPaletteOffset:
//     i32 systemModSetCount ; that many ModDataSet      <-- SYSTEM FIRST
//     i32 aniModSetCount    ; that many ModDataSet
//
// ModDataSet: i32 Type, i32 AnimationType, string Name, i32 dataCount,
//             dataCount x { i32 tag ; payload }
//
// Every payload opens with the 28-byte IModData base:
//     f32, u32, u32, i32 BMT_MtrlIndex, u32, u32, byte, byte, byte, byte
// ---------------------------------------------------------------------------
namespace {

constexpr size_t kIModDataBase = 28;

/// Consume one payload. Returns false when the tag is unknown or the reader ran
/// out - either way the caller must stop, because without a length prefix there
/// is no way to find the next entry.
bool ReadModData(ByteReader& r, u32 tag, BsrModPalette& out) {
    if (!r.skip(kIModDataBase)) return false;

    switch (tag) {
        case kModDataMtrl: {
            r.readU32();
            const u32 flags = r.readU32();
            r.readU32();
            const i32 gradients = r.readI32();
            if (gradients < 0 || !r.skip(static_cast<size_t>(gradients) * 20)) return false;
            if (flags & 4u) {
                const i32 curves = r.readI32();
                if (curves < 0 || !r.skip(static_cast<size_t>(curves) * 8)) return false;
            }
            if (!r.skip(9 * 4)) return false;
            break;
        }
        case kModDataTexAni:
            if (!r.skip(5 * 4 + 64)) return false;      // 5 x u32 + Matrix4x4
            break;

        case kModDataMultiTex:
        case kModDataMultiTexRev: {
            r.readU32();
            const std::wstring p = r.readLenPrefixedStringW(kPathCodePage, 4096);
            r.readU32();
            if (!p.empty()) out.multiTextures.push_back(p);
            break;
        }
        case kModDataParticle: {
            const i32 n = r.readI32();
            if (n < 0 || n > 4096) return false;
            for (i32 i = 0; i < n && r.ok(); ++i) {
                BsrParticleRef pr;
                pr.enabled      = r.readI32() != 0;
                pr.path         = r.readLenPrefixedStringW(kPathCodePage, 4096);
                pr.boneRelative = r.readLenPrefixedStringW(kPathCodePage, 4096);
                r.skip(12);                            // Vector3
                r.readU32();                           // BirthTime
                r.readU8(); r.readU8(); r.readU8();
                const u8 last = r.readU8();
                if (last == 1 && !r.skip(12)) return false;   // trailing Vector3
                // A disabled slot still names a real file, and an export that
                // dropped it would break the model on a shard that enables it.
                if (!pr.path.empty()) out.particles.push_back(pr);
            }
            break;
        }
        case kModDataEnvMap:
            if (!r.skip(4 * 4)) return false;
            break;

        case kModDataBumpEnv: {
            if (!r.skip(4 * 4 + 6 * 4)) return false;  // EnvMap base + 6 floats
            const i32 n = r.readI32();
            if (n < 0 || n > 4096) return false;
            for (i32 i = 0; i < n && r.ok(); ++i) {
                if (r.readU8() == 0) continue;         // 1-byte gate, no string
                const std::wstring p = r.readLenPrefixedStringW(kPathCodePage, 4096);
                if (!p.empty()) out.multiTextures.push_back(p);
            }
            break;
        }
        case kModDataSound: {
            const i32 sets = r.readI32();
            if (sets <= 0) break;                      // the 44-byte config is ABSENT
            if (!r.skip(11 * 4)) return false;
            if (sets > 4096) return false;
            for (i32 i = 0; i < sets && r.ok(); ++i) {
                BsrSoundSet set;
                set.name = r.readLenPrefixedStringW(kPathCodePage, 4096);
                const i32 tracks = r.readI32();
                if (tracks < 0 || tracks > 8192) return false;
                for (i32 t = 0; t < tracks && r.ok(); ++t) {
                    if (r.readU32() == 0) continue;    // 4-byte gate, no body
                    BsrSoundTrack tr;
                    tr.path   = r.readLenPrefixedStringW(kPathCodePage, 4096);
                    tr.timeMs = r.readI32();
                    tr.event  = r.readLenPrefixedStringW(kPathCodePage, 4096);
                    if (!tr.path.empty()) set.tracks.push_back(tr);
                }
                if (!set.tracks.empty()) out.soundSets.push_back(set);
            }
            break;
        }
        case kModDataDyVertex:
        case kModDataDyJoint:
        case kModDataDyLattice:
        case kModDataProgEquipPow:
            break;                                     // base only, nothing more

        default:
            return false;                              // unknown: cannot resync
    }
    return r.ok();
}

bool ReadModDataSet(ByteReader& r, BsrModPalette& out) {
    r.readI32();                                       // Type
    r.readI32();                                       // AnimationType
    r.readLenPrefixedStringW(kPathCodePage, 4096);     // Name - a label, not a file
    const i32 count = r.readI32();
    if (!r.ok() || count < 0 || count > 4096) return false;
    for (i32 i = 0; i < count; ++i) {
        const u32 tag = r.readU32();
        if (!r.ok() || !ReadModData(r, tag, out)) return false;
        ++out.entriesRead;
    }
    ++out.setsRead;
    return true;
}

void ReadModPalette(ByteReader& r, u32 offset, BsrModPalette& out) {
    if (offset == 0 || !r.seekSection(offset)) return;

    for (int list = 0; list < 2; ++list) {             // system first, then ani
        const i32 n = r.readI32();
        if (!r.ok() || n < 0 || n > 4096) { out.stoppedEarly = true; return; }
        for (i32 i = 0; i < n; ++i) {
            if (!ReadModDataSet(r, out)) { out.stoppedEarly = true; return; }
        }
    }
}

} // namespace

Result ParseBsr(const u8* data, size_t size, BsrFile& out) {
    out = BsrFile();

    if (!data || size < 64)
        return Result::Fail(Status::Truncated, T(L"BSR: dosya 64 bayttan kısa."));
    if (!IsBsr(data, size))
        return Result::Fail(Status::BadFormat, T(L"BSR: imza \"JMXVRES\" değil."));

    ByteReader r(data, size);
    out.signature = r.readSignature(12);
    if (out.signature.size() >= 12)
        out.version = TrimW(MultiByteToWide(out.signature.substr(8, 4), kPathCodePage));

    BsrHeader& h = out.header;
    h.materialOffset      = r.readU32();
    h.meshOffset          = r.readU32();
    h.skeletonOffset      = r.readU32();
    h.animationOffset     = r.readU32();
    h.primMeshGroupOffset = r.readU32();
    h.primAniGroupOffset  = r.readU32();
    h.modPaletteOffset    = r.readU32();
    h.collisionOffset     = r.readU32();
    h.primMeshFlag        = r.readU32();
    h.modDataFlag         = r.readU32();
    h.int2                = r.readU32();
    h.int3                = r.readU32();
    h.int4                = r.readU32();

    out.objectInfo.type     = r.readU16();
    out.objectInfo.category = r.readU16();
    out.objectInfo.name     = r.readLenPrefixedStringW(kPathCodePage, 4096);
    out.objectInfo.unk0     = r.readU32();
    out.objectInfo.unk1     = r.readU32();

    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"BSR: başlık eksik."));

    bool partial = false;

    // -----------------------------------------------------------------------
    // Collision
    // -----------------------------------------------------------------------
    r.clearFail();
    if (r.seekSection(h.collisionOffset)) {
        out.collision.meshPath = r.readLenPrefixedStringW(kPathCodePage, 4096);
        out.collision.box0 = r.readAabb();
        out.collision.box1 = r.readAabb();
        const u32 needMatrix = r.readU32();
        if (!r.failed() && needMatrix) {
            out.collision.hasMatrix = true;
            for (int i = 0; i < 16; ++i) out.collision.matrix[i] = r.readF32();
        }
        if (r.failed()) partial = true;
    }

    // -----------------------------------------------------------------------
    // Material sets
    // -----------------------------------------------------------------------
    r.clearFail();
    if (r.seekSection(h.materialOffset)) {
        const u32 n = r.readU32();
        if (!r.failed() && n <= kMaxMaterialSets && n <= r.remaining() / 8) {
            for (u32 i = 0; i < n; ++i) {
                BsrMaterialSet ms;
                ms.id   = r.readU32();
                ms.path = r.readLenPrefixedStringW(kPathCodePage, 4096);
                if (r.failed()) { partial = true; break; }
                out.materialSets.push_back(std::move(ms));
            }
        } else if (!r.failed() && n > kMaxMaterialSets) {
            partial = true;   // MATERIALSET_MAXCOUNTMAX violated - do not trust it
        } else {
            partial = true;
        }
    }

    // -----------------------------------------------------------------------
    // Mesh list
    // -----------------------------------------------------------------------
    r.clearFail();
    if (r.seekSection(h.meshOffset)) {
        const u32 n = r.readU32();
        if (!r.failed() && n <= r.remaining() / 4) {
            out.meshes.reserve(n);
            for (u32 i = 0; i < n; ++i) {
                BsrMeshRef m;
                m.path = r.readLenPrefixedStringW(kPathCodePage, 4096);
                if (h.primMeshFlag & 1) m.unk0 = r.readU32();
                if (r.failed()) { partial = true; break; }
                out.meshes.push_back(std::move(m));
            }
        } else {
            partial = true;
        }
    }

    // -----------------------------------------------------------------------
    // Animation list
    // -----------------------------------------------------------------------
    r.clearFail();
    if (r.seekSection(h.animationOffset)) {
        out.animationTypeVersion    = r.readU32();
        out.animationTypeUserDefine = r.readU32();
        const u32 n = r.readU32();
        if (!r.failed() && n <= r.remaining() / 4) {
            out.animations.reserve(n);
            for (u32 i = 0; i < n; ++i) {
                BsrAnimationRef a;
                a.path = r.readLenPrefixedStringW(kPathCodePage, 4096);
                if (r.failed()) { partial = true; break; }
                out.animations.push_back(std::move(a));
            }
        } else {
            partial = true;
        }
    }

    // -----------------------------------------------------------------------
    // Skeleton (a branch, not a list)
    // -----------------------------------------------------------------------
    r.clearFail();
    if (r.seekSection(h.skeletonOffset)) {
        const u32 has = r.readU32();
        if (!r.failed() && has == 1) {
            out.hasSkeleton    = true;
            out.skeletonPath   = r.readLenPrefixedStringW(kPathCodePage, 4096);
            out.attachmentBone = r.readLenPrefixedStringW(kPathCodePage, 1024);
            if (r.failed()) partial = true;
        }
    }

    // -----------------------------------------------------------------------
    // Mesh groups
    // -----------------------------------------------------------------------
    r.clearFail();
    if (r.seekSection(h.primMeshGroupOffset)) {
        const u32 n = r.readU32();
        if (!r.failed() && n <= r.remaining() / 8) {
            out.meshGroups.reserve(n);
            for (u32 i = 0; i < n; ++i) {
                BsrMeshGroup g;
                g.name = r.readLenPrefixedStringW(kPathCodePage, 1024);
                const u32 fileCount = r.readU32();
                if (r.failed() || fileCount > r.remaining() / 4) { partial = true; break; }
                g.meshIndices.reserve(fileCount);
                for (u32 k = 0; k < fileCount; ++k) g.meshIndices.push_back(r.readU32());
                if (r.failed()) { partial = true; break; }
                out.meshGroups.push_back(std::move(g));
            }
        } else {
            partial = true;
        }
    }

    // -----------------------------------------------------------------------
    // Animation groups
    // -----------------------------------------------------------------------
    r.clearFail();
    if (r.seekSection(h.primAniGroupOffset)) {
        const u32 groupCount = r.readU32();
        if (!r.failed() && groupCount <= r.remaining() / 8) {
            out.animationGroups.reserve(groupCount);
            for (u32 gi = 0; gi < groupCount && !r.failed(); ++gi) {
                BsrAnimationGroup g;
                g.name = r.readLenPrefixedStringW(kPathCodePage, 1024);
                const u32 aniCount = r.readU32();
                if (r.failed() || aniCount > r.remaining() / 20) { partial = true; break; }

                g.entries.reserve(aniCount);
                for (u32 ai = 0; ai < aniCount; ++ai) {
                    BsrAnimationEntry e;
                    e.type      = r.readU32();
                    e.fileIndex = r.readU32();

                    const u32 eventCount = r.readU32();
                    if (r.failed() || eventCount > r.remaining() / 16) { partial = true; break; }
                    e.events.reserve(eventCount);
                    for (u32 ei = 0; ei < eventCount; ++ei) {
                        BsrAnimationEntry::Event ev;
                        ev.keyTime = r.readU32();
                        ev.type    = r.readU32();
                        ev.index   = r.readU32();
                        ev.unk     = r.readU32();
                        e.events.push_back(ev);
                    }
                    if (r.failed()) { partial = true; break; }

                    const u32 walkPointCount = r.readU32();
                    e.walkLength = r.readF32();
                    if (r.failed() || walkPointCount > r.remaining() / 8) { partial = true; break; }
                    e.walkGraph.reserve(walkPointCount);
                    for (u32 wi = 0; wi < walkPointCount; ++wi) e.walkGraph.push_back(r.readVec2());
                    if (r.failed()) { partial = true; break; }

                    g.entries.push_back(std::move(e));
                }
                out.animationGroups.push_back(std::move(g));
                if (partial) break;
            }
        } else {
            partial = true;
        }
    }
    r.clearFail();

    // The mod palette: sounds, bone-mounted particle effects and extra texture
    // layers. Parsed on a COPY of the reader so a malformed palette can never
    // disturb the recovery pass below - the model itself stays loadable even
    // when its effect table is broken.
    {
        ByteReader mr(data, size);
        ReadModPalette(mr, out.header.modPaletteOffset, out.modPalette);
    }
    // The CResAttachable slot table follows; it holds indices, not files.

    // -----------------------------------------------------------------------
    // Last resort: some exporters write section offsets that are a few bytes
    // off from the sections they point at (measured: Electus "vikingss" and
    // Kim Hoa "test"/"tt" avatars, whose material offset is 2 low and mesh
    // offset 4 high). The file itself is fine - every path is there in plain
    // text - so rather than discard a loadable model, lift the paths straight
    // out of the bytes. Nothing is invented: only strings the file actually
    // contains are taken, and ComposeBsrModel drops any that do not resolve.
    // -----------------------------------------------------------------------
    if (partial && out.meshes.empty()) RecoverRefsByScan(data, size, out);

    if (partial)
        return Result::Fail(Status::Truncated,
                            T(L"BSR: bazı bölümler eksik veya bozuk - kısmi olarak okundu."));
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// summaryTr
// ---------------------------------------------------------------------------
std::wstring BsrFile::summaryTr() const {
    std::wstring s;
    s += T(L"İmza          : ") + MultiByteToWide(signature, 1252) + L"\n";
    if (!objectInfo.name.empty())
        s += T(L"Nesne adı     : ") + objectInfo.name + L"\n";
    s += FormatW(T(L"Tür / kategori: %u / %u\n"), objectInfo.type, objectInfo.category);

    s += T(L"Malzeme seti  : ") + FormatCountTr(materialSets.size()) + L"\n";
    for (const BsrMaterialSet& ms : materialSets)
        s += FormatW(L"    [%u] ", ms.id) + ms.path + L"\n";

    s += T(L"Ağ (mesh)     : ") + FormatCountTr(meshes.size()) + L"\n";
    {
        const size_t shown = (meshes.size() < 12) ? meshes.size() : 12;
        for (size_t i = 0; i < shown; ++i) s += L"    " + meshes[i].path + L"\n";
        if (meshes.size() > shown)
            s += T(L"    ... ve ") + FormatCountTr(meshes.size() - shown) + T(L" ağ daha\n");
    }

    s += T(L"İskelet       : ") + (hasSkeleton ? skeletonPath : std::wstring(T(L"yok"))) + L"\n";
    if (hasSkeleton && !attachmentBone.empty())
        s += T(L"Bağlantı kemiği: ") + attachmentBone + L"\n";

    s += T(L"Animasyon     : ") + FormatCountTr(animations.size()) + L"\n";
    {
        const size_t shown = (animations.size() < 8) ? animations.size() : 8;
        for (size_t i = 0; i < shown; ++i) s += L"    " + animations[i].path + L"\n";
        if (animations.size() > shown)
            s += T(L"    ... ve ") + FormatCountTr(animations.size() - shown) + T(L" animasyon daha\n");
    }

    if (!meshGroups.empty()) {
        s += T(L"Ağ grupları   : ") + FormatCountTr(meshGroups.size()) + L"\n";
        const size_t shown = (meshGroups.size() < 8) ? meshGroups.size() : 8;
        for (size_t i = 0; i < shown; ++i)
            s += L"    " + meshGroups[i].name + L"  (" +
                 FormatCountTr(meshGroups[i].meshIndices.size()) + T(L" ağ)\n");
    }
    if (!animationGroups.empty()) {
        s += T(L"Anim. grupları: ") + FormatCountTr(animationGroups.size()) + L"\n";
        const size_t shown = (animationGroups.size() < 8) ? animationGroups.size() : 8;
        for (size_t i = 0; i < shown; ++i)
            s += L"    " + animationGroups[i].name + L"  (" +
                 FormatCountTr(animationGroups[i].entries.size()) + T(L" giriş)\n");
    }

    if (!collision.meshPath.empty())
        s += T(L"Çarpışma ağı  : ") + collision.meshPath + L"\n";
    if (collision.box0.valid()) {
        const Vec3 e = collision.box0.extent();
        s += FormatW(T(L"Çarpışma kutusu: %.2f x %.2f x %.2f\n"), e.x, e.y, e.z);
    }

    return s;
}

// ---------------------------------------------------------------------------
// ComposedModel accessors
// ---------------------------------------------------------------------------
u64 ComposedModel::totalVertices() const {
    u64 n = 0;
    for (const ComposedPart& p : parts) n += p.mesh.vertices.size();
    return n;
}
u64 ComposedModel::totalFaces() const {
    u64 n = 0;
    for (const ComposedPart& p : parts) n += p.mesh.faces.size();
    return n;
}

// ---------------------------------------------------------------------------
// ComposeBsrModel
// ---------------------------------------------------------------------------
Result ComposeBsrModel(const BsrFile& bsr,
                       const std::wstring& bsrInnerPath,
                       const AssetLoader& loader,
                       int materialSetIndex,
                       bool loadTextures,
                       ComposedModel& out) {
    out = ComposedModel();
    out.sourcePath = bsrInnerPath;
    out.resource   = bsr;

    if (!loader)
        return Result::Fail(Status::InvalidArgument,
                            T(L"BSR birleştirme: varlık yükleyici verilmedi."));
    if (bsr.meshes.empty())
        return Result::Fail(Status::NotFound, T(L"BSR: ağ (mesh) listesi boş."));

    const std::wstring bsrFolder = PathParent(NormalizeInnerPath(bsrInnerPath));

    // -----------------------------------------------------------------------
    // Material set
    // -----------------------------------------------------------------------
    BmtFile      mtrlSet;
    bool         haveMaterialSet = false;
    std::wstring bmtFolder;

    if (!bsr.materialSets.empty()) {
        int idx = materialSetIndex;
        if (idx < 0 || idx >= static_cast<int>(bsr.materialSets.size())) idx = 0;
        const BsrMaterialSet& ms = bsr.materialSets[static_cast<size_t>(idx)];

        std::vector<u8> bmtBytes;
        std::wstring    resolved;
        if (LoadAsset(loader, ms.path, bsrFolder, bmtBytes, resolved)) {
            const Result pr = ParseBmt(bmtBytes.data(), bmtBytes.size(), mtrlSet);
            if (pr.ok() || !mtrlSet.materials.empty()) {
                haveMaterialSet = true;
                bmtFolder = PathParent(resolved);
                if (!pr.ok())
                    out.warningsTr.push_back(T(L"Malzeme seti kısmi okundu: ") + resolved +
                                             L" - " + pr.text());
            } else {
                out.warningsTr.push_back(T(L"Malzeme seti çözümlenemedi: ") + resolved +
                                         L" - " + pr.text());
            }
        } else {
            out.warningsTr.push_back(T(L"Malzeme seti bulunamadı: ") + ms.path);
        }
    }

    // -----------------------------------------------------------------------
    // Skeleton (loaded up front: it drives BOTH bind-pose skinning of the
    // meshes below AND the orientation hints further down).
    // -----------------------------------------------------------------------
    BskFile skel;
    bool    haveSkel = false;
    if (bsr.hasSkeleton && !bsr.skeletonPath.empty()) {
        std::vector<u8> skBytes; std::wstring skResolved;
        if (LoadAsset(loader, bsr.skeletonPath, bsrFolder, skBytes, skResolved) &&
            ParseBsk(skBytes.data(), skBytes.size(), skel).ok() && !skel.bones.empty())
            haveSkel = true;
    }

    // -----------------------------------------------------------------------
    // Meshes
    // -----------------------------------------------------------------------
    for (const BsrMeshRef& mr : bsr.meshes) {
        std::vector<u8> bytes;
        std::wstring    resolved;
        if (!LoadAsset(loader, mr.path, bsrFolder, bytes, resolved)) {
            out.warningsTr.push_back(T(L"Ağ bulunamadı: ") + mr.path);
            continue;
        }

        ComposedPart part;
        part.meshPath = resolved;
        const Result pr = ParseBms(bytes.data(), bytes.size(), part.mesh, true);
        if (!pr.ok() && part.mesh.vertices.empty()) {
            out.warningsTr.push_back(T(L"Ağ çözümlenemedi: ") + resolved + L" - " + pr.text());
            continue;
        }
        if (!pr.ok())
            out.warningsTr.push_back(T(L"Ağ kısmi okundu: ") + resolved + L" - " + pr.text());

        part.materialName = part.mesh.material;

        if (haveMaterialSet && !part.materialName.empty()) {
            if (const BmtMaterial* m = mtrlSet.find(part.materialName)) {
                part.hasMaterial = true;
                part.material    = *m;
                part.diffuseTexturePath = m->diffuseMap.resolve(bmtFolder);
            } else {
                out.warningsTr.push_back(T(L"Malzeme sette yok: ") + part.materialName +
                                         L" (" + PathFileName(resolved) + L")");
            }
        }

        // NOTE: skinned .bms vertices are ALREADY in bind-pose world space, so
        // they are drawn RAW - the same as every character/mob model. Do NOT
        // per-vertex transform them here (that scatters characters into debris).
        // Avatar-set wings are the only special case, and they are placed by
        // ComposeAvatarPieces -> PlaceAttachOnBack, not here.
        for (const BmsVertex& v : part.mesh.vertices) out.bounds.add(v.position);
        out.parts.push_back(std::move(part));
    }

    if (out.parts.empty())
        return Result::Fail(Status::NotFound,
                            T(L"BSR: hiçbir ağ yüklenemedi (") +
                            FormatCountTr(bsr.meshes.size()) + T(L" referans denendi)."));

    // -----------------------------------------------------------------------
    // Skeleton-based orientation. The mesh may be authored on any axis (many
    // imported models are Z-up or even stored upside down); the SKELETON is the
    // ground truth. pelvis -> head is the body's up (sign included), and the
    // shoulders/hips give left-right, hence the facing direction. This makes the
    // model stand up and face the camera without guessing from the bounding box.
    // -----------------------------------------------------------------------
    if (haveSkel) {
        auto find = [&](const wchar_t* needle, Vec3& p) -> bool {
            const std::wstring nd = ToLowerW(needle);
            for (const BskBone& b : skel.bones)
                if (ToLowerW(b.name).find(nd) != std::wstring::npos) {
                    p = b.translationToOrigin; return true;
                }
            return false;
        };
        Vec3 head, pelvis, foot, spine, neck;
        const bool hHead   = find(L"head", head);
        const bool hPelvis = find(L"pelvis", pelvis);
        const bool hFoot    = find(L"foot", foot);
        const bool hSpine  = find(L"spine", spine);
        const bool hNeck   = find(L"neck", neck);

        Vec3 up(0, 0, 0);
        const Vec3 top = hHead ? head : (hNeck ? neck : spine);
        if      ((hHead || hNeck) && hPelvis) up = top - pelvis;
        else if ((hHead || hNeck) && hFoot)   up = top - foot;
        else if (hSpine && hPelvis)           up = spine - pelvis;
        else if (hPelvis && hFoot)            up = pelvis - foot;
        if (up.length() > 1e-3f) out.upHint = up.normalized();

        // Facing: right = R-side bone minus L-side bone; forward = up x right.
        Vec3 lft, rgt;
        const bool hL = find(L"l upperarm", lft) || find(L"l clavicle", lft) ||
                        find(L"l thigh", lft)    || find(L"l hand", lft);
        const bool hR = find(L"r upperarm", rgt) || find(L"r clavicle", rgt) ||
                        find(L"r thigh", rgt)    || find(L"r hand", rgt);
        if (hL && hR && out.upHint.length() > 0.5f) {
            const Vec3 right = (rgt - lft);
            if (right.length() > 1e-3f) {
                // right x up (not up x right): the SR skeletons put the FACE on
                // this side, so the model turns to face the -Z camera instead of
                // showing its back.
                const Vec3 fwd = right.normalized().cross(out.upHint);
                if (fwd.length() > 1e-3f) out.forwardHint = fwd.normalized();
            }
        }
    }

    // -----------------------------------------------------------------------
    // Textures (deduplicated by resolved path)
    // -----------------------------------------------------------------------
    if (loadTextures) {
        for (ComposedPart& part : out.parts) {
            if (part.diffuseTexturePath.empty()) continue;

            const std::wstring key = ToLowerW(part.diffuseTexturePath);
            int found = -1;
            for (size_t i = 0; i < out.texturePaths.size(); ++i) {
                if (ToLowerW(out.texturePaths[i]) == key) { found = static_cast<int>(i); break; }
            }
            if (found >= 0) { part.textureIndex = found; continue; }

            std::vector<u8> bytes;
            std::wstring    resolved;
            if (!LoadAsset(loader, part.diffuseTexturePath, bmtFolder, bytes, resolved)) {
                out.warningsTr.push_back(T(L"Doku bulunamadı: ") + part.diffuseTexturePath);
                continue;
            }

            Image        img;
            std::wstring err;
            if (!DecodeDdj(bytes.data(), bytes.size(), 0, 0, img, nullptr, &err) || img.empty()) {
                out.warningsTr.push_back(T(L"Doku çözümlenemedi: ") + resolved +
                                         (err.empty() ? std::wstring() : (L" - " + err)));
                continue;
            }

            part.textureIndex = static_cast<int>(out.textures.size());
            out.textures.push_back(std::move(img));
            out.texturePaths.push_back(part.diffuseTexturePath);
        }
    }

    return Result::Ok();
}

// ---------------------------------------------------------------------------
// ApplyCharacterAttach - seat a skinned attach piece at the CHARACTER's attach
// bone (its root rides that bone in game). `charSkel` is a real character
// skeleton borrowed via LoadAvatarCharSkeleton; the piece's vertices, already in
// the attach frame, are moved by the bone's bind-pose world matrix.
// ---------------------------------------------------------------------------
void ApplyCharacterAttach(ComposedModel& m, const std::wstring& attachBone,
                          const BskFile& charSkel) {
    if (m.parts.empty() || charSkel.bones.empty()) return;
    const BskBone* b = FindBoneByName(charSkel, attachBone.empty() ? L"Bip01 Spine1" : attachBone);
    if (!b) b = FindBoneByName(charSkel, L"Bip01 Spine1");
    if (!b) b = FindBoneByName(charSkel, L"Bip01 Spine");
    if (!b) return;
    // PURE TRANSLATION - no rotation, by design.
    //
    // The attach piece is already authored in the character's own axis frame:
    // a Silkroad character FACES -Z (verified on europewoman_skel: Bip01 L Foot
    // bindWorld z = +0.515 while its child Toe0 is z = -0.478), so the wing's
    // +Z long axis is ALREADY sweeping backwards, +Y is ALREADY up and +-X is
    // already the span. It only needs lifting onto the spine.
    //
    // Applying the attach bone's own quaternion on top double-rotates it (the
    // wing shoots out sideways), and so does any hand-tuned "stand it up" tilt -
    // that tilt was this code's one real bug. The reference viewer discards the
    // bone rotation too and multiplies by the translation alone.
    //
    // The wing's own stand01.ban is likewise unnecessary: its keyframe 0 is
    // byte-identical to the .bsk bind pose, so at rest it is a mathematical
    // no-op and only adds a small flap later.
    const Vec3 t = b->translationToOrigin;
    Aabb nb;
    for (ComposedPart& part : m.parts) {
        part.rigidPlaced = true;          // moved out of its own bind space
        for (BmsVertex& v : part.mesh.vertices) {
            v.position = v.position + t;
            nb.add(v.position);
        }
    }
    m.bounds = nb;
}

// ---------------------------------------------------------------------------
// ComposeAvatarPieces
//
//  Merge additional avatar-set pieces into an already-composed model. Each
//  extra .bsr is loaded, composed on its own, then folded into `inout`: its
//  textures append (deduplicated by resolved path against what is already
//  there), its parts append with their texture indices rebased, and its bounds
//  extend the whole. The orientation hints of `inout` (the main piece) are
//  kept - every piece is skinned to the same character skeleton, so one set of
//  hints stands the whole costume up correctly.
// ---------------------------------------------------------------------------
Result ComposeAvatarPieces(const std::vector<std::wstring>& extraBsrRel,
                           const std::wstring& baseFolder,
                           const AssetLoader& loader,
                           ComposedModel& inout,
                           int& addedOut,
                           const BskFile* charSkel) {
    addedOut = 0;
    if (!loader)
        return Result::Fail(Status::InvalidArgument,
                            T(L"Avatar seti: varlık yükleyici verilmedi."));

    // The body box as it stands BEFORE any attach piece is added - used to seat
    // skinned wings/capes on the upper back.
    const Aabb bodyBox = inout.bounds;

    for (const std::wstring& rel : extraBsrRel) {
        std::vector<u8> bytes;
        std::wstring    resolved;

        // itemdata gives a LOGICAL path (item\Avatar\x.bsr) that does not match
        // the physical location (res\item\avatar\x.bsr). Every piece of an avatar
        // family ships in the SAME folder as the selected one, so resolve by
        // "selected folder + file name" FIRST - that is what actually loads.
        bool got = false;
        if (!baseFolder.empty()) {
            const std::wstring sameFolder =
                NormalizeInnerPath(PathJoin(baseFolder, PathFileName(rel)));
            if (loader(sameFolder, bytes) && !bytes.empty()) { resolved = sameFolder; got = true; }
        }
        // Fall back to the logical path as-is / joined with the base folder.
        if (!got) got = LoadAsset(loader, rel, baseFolder, bytes, resolved);
        if (!got) {
            inout.warningsTr.push_back(T(L"Set parçası bulunamadı: ") + PathFileName(rel));
            continue;
        }

        BsrFile sib;
        const Result pr = ParseBsr(bytes.data(), bytes.size(), sib);
        const bool usable = pr.ok() ||
                            (pr.status == Status::Truncated && !sib.meshes.empty());
        if (!usable) {
            inout.warningsTr.push_back(T(L"Set parçası okunamadı: ") + resolved +
                                       L" - " + pr.text());
            continue;
        }

        ComposedModel tmp;
        const Result cr = ComposeBsrModel(sib, resolved, loader, 0, true, tmp);
        if ((!cr.ok() && tmp.parts.empty()) || tmp.parts.empty()) {
            inout.warningsTr.push_back(T(L"Set parçası çizilemedi: ") + resolved);
            continue;
        }

        // A skinned attach piece (wing/cape) rides the character's spine in game.
        // Prefer the real character skeleton's attach bone; fall back to the
        // geometric upper-back placement when no character skeleton is available.
        if (sib.hasSkeleton) {
            if (charSkel) ApplyCharacterAttach(tmp, sib.attachmentBone, *charSkel);
            else          PlaceAttachOnBack(tmp, bodyBox);
        }

        // Fold tmp into inout. Textures first, deduplicated by resolved path so
        // pieces that share a texture atlas do not store it twice.
        std::vector<int> remap(tmp.textures.size(), -1);
        for (size_t i = 0; i < tmp.textures.size(); ++i) {
            const std::wstring key = ToLowerW(tmp.texturePaths[i]);
            int found = -1;
            for (size_t j = 0; j < inout.texturePaths.size(); ++j)
                if (ToLowerW(inout.texturePaths[j]) == key) { found = static_cast<int>(j); break; }
            if (found >= 0) { remap[i] = found; continue; }
            remap[i] = static_cast<int>(inout.textures.size());
            inout.textures.push_back(std::move(tmp.textures[i]));
            inout.texturePaths.push_back(tmp.texturePaths[i]);
        }
        for (ComposedPart& part : tmp.parts) {
            if (part.textureIndex >= 0 && static_cast<size_t>(part.textureIndex) < remap.size())
                part.textureIndex = remap[static_cast<size_t>(part.textureIndex)];
            else
                part.textureIndex = -1;
            inout.parts.push_back(std::move(part));
        }
        inout.bounds.add(tmp.bounds);
        for (std::wstring& w : tmp.warningsTr) inout.warningsTr.push_back(std::move(w));
        ++addedOut;
    }
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// ComposedModelToObj
//
//  Winding and V-flip follow the same rules as BmsToObj.
// ---------------------------------------------------------------------------
Result ComposedModelToObj(const ComposedModel& model,
                          const std::wstring& mtlName,
                          std::string& outObj,
                          std::string& outMtl) {
    outObj.clear();
    outMtl.clear();

    if (model.parts.empty())
        return Result::Fail(Status::InvalidArgument, L"OBJ: modelde parça yok.");

    std::string obj;
    std::string mtl;
    char buf[512];

    obj += "# SRO Asset Studio - Wavefront OBJ\n";
    obj += "# source: ";
    obj += WideToUtf8(model.sourcePath);
    obj += "\n";
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "# %llu vertices, %llu faces, %zu parts\n",
                static_cast<unsigned long long>(model.totalVertices()),
                static_cast<unsigned long long>(model.totalFaces()),
                model.parts.size());
    obj += buf;

    if (!mtlName.empty()) {
        obj += "mtllib ";
        obj += WideToUtf8(mtlName);
        obj += "\n";
        mtl += "# SRO Asset Studio - Wavefront MTL\n";
    }

    size_t vertexBase = 0;
    size_t partIndex  = 0;

    for (const ComposedPart& part : model.parts) {
        const BmsFile& m = part.mesh;
        if (m.vertices.empty()) { ++partIndex; continue; }

        std::wstring objName = m.name.empty() ? PathFileName(part.meshPath) : m.name;
        if (objName.empty()) objName = FormatW(L"part%zu", partIndex);

        obj += "o ";
        obj += WideToUtf8(SanitizeFileName(objName));
        obj += "\n";

        for (const BmsVertex& v : m.vertices) {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "v %.6f %.6f %.6f\n",
                        v.position.x, v.position.y, v.position.z);
            obj += buf;
        }
        for (const BmsVertex& v : m.vertices) {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "vt %.6f %.6f\n",
                        v.uv0.x, 1.0f - v.uv0.y);
            obj += buf;
        }
        for (const BmsVertex& v : m.vertices) {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "vn %.6f %.6f %.6f\n",
                        v.normal.x, v.normal.y, v.normal.z);
            obj += buf;
        }

        if (!mtlName.empty()) {
            std::wstring matName = part.materialName.empty()
                                 ? FormatW(L"material%zu", partIndex)
                                 : part.materialName;
            const std::string matUtf8 = WideToUtf8(SanitizeFileName(matName));

            obj += "usemtl ";
            obj += matUtf8;
            obj += "\n";

            mtl += "\nnewmtl ";
            mtl += matUtf8;
            mtl += "\n";
            if (part.hasMaterial) {
                const Color4& d = part.material.diffuse;
                const Color4& a = part.material.ambient;
                const Color4& sp = part.material.specular;
                _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                            "Kd %.4f %.4f %.4f\nKa %.4f %.4f %.4f\nKs %.4f %.4f %.4f\n",
                            d.r, d.g, d.b, a.r, a.g, a.b, sp.r, sp.g, sp.b);
                mtl += buf;
            }
            if (!part.diffuseTexturePath.empty()) {
                // OBJ consumers expect a real image; the .ddj is exported as
                // .png alongside, so point at that name.
                std::wstring tex = part.diffuseTexturePath;
                const size_t dot = tex.find_last_of(L'.');
                if (dot != std::wstring::npos) tex = tex.substr(0, dot);
                tex += L".png";
                mtl += "map_Kd ";
                mtl += WideToUtf8(PathFileName(tex));
                mtl += "\n";
            }
        }

        const size_t vc = m.vertices.size();
        for (const BmsFace& f : m.faces) {
            if (f.a >= vc || f.b >= vc || f.c >= vc) continue;
            const unsigned long long a = vertexBase + f.a + 1ull;
            const unsigned long long b = vertexBase + f.b + 1ull;
            const unsigned long long c = vertexBase + f.c + 1ull;
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "f %llu/%llu/%llu %llu/%llu/%llu %llu/%llu/%llu\n",
                        a, a, a, c, c, c, b, b, b);
            obj += buf;
        }

        vertexBase += vc;
        ++partIndex;
    }

    if (vertexBase == 0)
        return Result::Fail(Status::InvalidArgument, L"OBJ: modelde köşe yok.");

    outObj.swap(obj);
    outMtl.swap(mtl);
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// RecoverCustomBsr
// ---------------------------------------------------------------------------
bool RecoverCustomBsr(const std::wstring& innerPath, BsrFile& out) {
    if (innerPath.empty()) return false;
    std::wstring low = ToLowerW(innerPath);
    for (wchar_t& c : low) if (c == L'/') c = L'\\';

    if (low.find(L"p_teemo_01.bsr") != std::wstring::npos) {
        out = BsrFile();
        out.signature = "JMXVRES 0109";
        out.version   = L"0109";
        out.objectInfo.type     = 1;
        out.objectInfo.category = 2;
        out.objectInfo.name     = L"p_teemo_01";

        out.materialSets.push_back({ 0, L"prim\\mtrl\\cos\\pet_teemo_a.bmt" });
        out.skeletonPath = L"prim\\skel\\cos\\skin_teemo_a.bsk";
        out.hasSkeleton  = true;

        for (int i = 1; i <= 6; ++i) {
            wchar_t mpath[128];
            swprintf_s(mpath, L"prim\\mesh\\cos\\pet_teemo_a_part%d.bms", i);
            out.meshes.push_back({ mpath, 0 });
        }

        const wchar_t* anis[] = {
            L"prim\\ani\\cos\\p_teemo\\pet_teemo_a_attack01.ban",
            L"prim\\ani\\cos\\p_teemo\\pet_teemo_emoticon01.ban",
            L"prim\\ani\\cos\\p_teemo\\pet_teemo_pick.ban",
            L"prim\\ani\\cos\\p_teemo\\pet_teemo_run.ban",
            L"prim\\ani\\cos\\p_teemo\\pet_teemo_stand01.ban",
            L"prim\\ani\\cos\\p_teemo\\pet_teemo_stand02.ban",
            L"prim\\ani\\cos\\p_teemo\\pet_teemo_stand03.ban",
            L"prim\\ani\\cos\\p_teemo\\pet_teemo_stand04.ban",
            L"prim\\ani\\cos\\p_teemo\\pet_teemo_walk.ban"
        };
        for (const wchar_t* a : anis) out.animations.push_back({ a });

        BsrMeshGroup mg;
        mg.name = L"default";
        for (size_t i = 0; i < out.meshes.size(); ++i) mg.meshIndices.push_back((u32)i);
        out.meshGroups.push_back(std::move(mg));
        return true;
    }

    if (low.find(L"p_teemo_02.bsr") != std::wstring::npos) {
        out = BsrFile();
        out.signature = "JMXVRES 0109";
        out.version   = L"0109";
        out.objectInfo.type     = 1;
        out.objectInfo.category = 2;
        out.objectInfo.name     = L"p_teemo_02";

        out.materialSets.push_back({ 0, L"prim\\mtrl\\cos\\pet_teemo_b.bmt" });
        out.skeletonPath = L"prim\\skel\\cos\\skin_teemo_b.bsk";
        out.hasSkeleton  = true;

        for (int i = 1; i <= 13; ++i) {
            wchar_t mpath[128];
            swprintf_s(mpath, L"prim\\mesh\\cos\\pet_teemo_b_part%d.bms", i);
            out.meshes.push_back({ mpath, 0 });
        }

        const wchar_t* anis[] = {
            L"prim\\ani\\cos\\p_teemo\\pet_teemo_b_attack01.ban",
            L"prim\\ani\\cos\\p_teemo\\pet_teemo_b_emoticon01.ban",
            L"prim\\ani\\cos\\p_teemo\\pet_teemo_b_pick.ban",
            L"prim\\ani\\cos\\p_teemo\\pet_teemo_b_run.ban",
            L"prim\\ani\\cos\\p_teemo\\pet_teemo_b_stand01.ban",
            L"prim\\ani\\cos\\p_teemo\\pet_teemo_b_stand02.ban",
            L"prim\\ani\\cos\\p_teemo\\pet_teemo_b_walk.ban"
        };
        for (const wchar_t* a : anis) out.animations.push_back({ a });

        BsrMeshGroup mg;
        mg.name = L"default";
        for (size_t i = 0; i < out.meshes.size(); ++i) mg.meshIndices.push_back((u32)i);
        out.meshGroups.push_back(std::move(mg));
        return true;
    }

    if (low.find(L"coiny_a_map_01.bsr") != std::wstring::npos) {
        out = BsrFile();
        out.signature = "JMXVRES 0109";
        out.version   = L"0109";
        out.objectInfo.name = L"coiny_a_map_01";
        for (int i = 1; i <= 5; ++i) {
            wchar_t mpath[128];
            swprintf_s(mpath, L"prim\\mesh\\dun\\tower\\defence\\coiny_a_map_0%d.bms", i);
            out.meshes.push_back({ mpath, 0 });
        }
        out.meshes.push_back({ L"prim\\mesh\\dun\\tower\\defence\\coiny_a_map_we_01.bms", 0 });
        BsrMeshGroup mg;
        mg.name = L"default";
        for (size_t i = 0; i < out.meshes.size(); ++i) mg.meshIndices.push_back((u32)i);
        out.meshGroups.push_back(std::move(mg));
        return true;
    }

    if (low.find(L"panda_blue.bsr") != std::wstring::npos) {
        out = BsrFile();
        out.signature = "JMXVRES 0109";
        out.version   = L"0109";
        out.objectInfo.name = L"panda_blue";
        out.materialSets.push_back({ 0, L"prim\\mtrl\\cos\\help\\panda_blue.bmt" });
        out.skeletonPath = L"prim\\skel\\cos\\help\\panda.bsk";
        out.hasSkeleton = true;
        out.meshes.push_back({ L"prim\\mesh\\cos\\help\\panda_eyes_happy.bms", 0 });
        return true;
    }

    if (low.find(L"panda_pink.bsr") != std::wstring::npos) {
        out = BsrFile();
        out.signature = "JMXVRES 0109";
        out.version   = L"0109";
        out.objectInfo.name = L"panda_pink";
        out.materialSets.push_back({ 0, L"prim\\mtrl\\cos\\help\\panda_pink.bmt" });
        out.skeletonPath = L"prim\\skel\\cos\\help\\panda.bsk";
        out.hasSkeleton = true;
        out.meshes.push_back({ L"prim\\mesh\\cos\\help\\panda_eyes_happy.bms", 0 });
        return true;
    }

    if (low.find(L"pet_achatles_a_saddle.bsr") != std::wstring::npos) {
        out = BsrFile();
        out.signature = "JMXVRES 0109";
        out.version   = L"0109";
        out.objectInfo.name = L"pet_achatles_a_saddle";
        out.materialSets.push_back({ 0, L"prim\\mtrl\\pet2\\pet_achatles_a.bmt" });
        out.skeletonPath = L"prim\\skel\\pet2\\pet_achatles_a.bsk";
        out.hasSkeleton = true;
        out.meshes.push_back({ L"prim\\mesh\\pet2\\pet_achatles_a_part1.bms", 0 });
        out.meshes.push_back({ L"prim\\mesh\\pet2\\pet_achatles_a_part2.bms", 0 });
        return true;
    }

    if (low.find(L"pet_lord_lyon_white_a_saddle.bsr") != std::wstring::npos) {
        out = BsrFile();
        out.signature = "JMXVRES 0109";
        out.version   = L"0109";
        out.objectInfo.name = L"pet_lord_lyon_white_a_saddle";
        out.materialSets.push_back({ 0, L"prim\\mtrl\\pet2\\pet_lord_lyon_white_a.bmt" });
        out.skeletonPath = L"prim\\skel\\pet2\\pet_lord_lyon_white_a.bsk";
        out.hasSkeleton = true;
        out.meshes.push_back({ L"prim\\mesh\\pet2\\pet_lord_lyon_white_a_part1.bms", 0 });
        out.meshes.push_back({ L"prim\\mesh\\pet2\\pet_lord_lyon_white_a_part2.bms", 0 });
        return true;
    }

    return false;
}

} // namespace sro
