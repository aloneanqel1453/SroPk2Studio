// ============================================================================
//  SRO Asset Studio - formats/AssetDeps.cpp   (see AssetDeps.h)
// ============================================================================
#include "AssetDeps.h"

#include "Bms.h"
#include "Bmt.h"
#include "Bsk.h"
#include "Efp.h"
#include "MapFiles.h"   // .cpd / .dof / .o / .o2 also reference other assets
#include "Nif2dt.h"     // .2dt widgets name their atlas textures

#include <algorithm>
#include <unordered_set>

namespace sro {

namespace {

/// Same resolution ladder the preview uses, so exported == drawable.
bool TryLoad(const AssetLoader& loader, const std::wstring& path,
             const std::wstring& baseFolder, std::vector<u8>& out, std::wstring& resolved) {
    if (!loader || path.empty()) return false;

    const std::wstring direct = NormalizeInnerPath(path);
    if (!direct.empty()) { out.clear(); if (loader(direct, out) && !out.empty()) { resolved = direct; return true; } }

    if (!baseFolder.empty()) {
        const std::wstring joined = NormalizeInnerPath(PathJoin(baseFolder, path));
        if (joined != direct) { out.clear(); if (loader(joined, out) && !out.empty()) { resolved = joined; return true; } }

        // A reference may carry a stale folder while the file sits next to the
        // referrer (very common in private-server repacks).
        const std::wstring sameFolder = NormalizeInnerPath(PathJoin(baseFolder, PathFileName(path)));
        if (sameFolder != joined && sameFolder != direct) {
            out.clear(); if (loader(sameFolder, out) && !out.empty()) { resolved = sameFolder; return true; }
        }
    }
    return false;
}

struct Job {
    std::wstring path;         ///< as referenced
    std::wstring baseFolder;   ///< folder of the file that referenced it
};

} // namespace

// ---------------------------------------------------------------------------
void CollectAssetDependenciesInto(const std::wstring& innerPath, const AssetLoader& loader,
                                  AssetDeps& inout) {
    if (!loader || innerPath.empty()) return;

    // `seen` spans calls (shared textures collected once); rebuilt from paths so
    // callers can chain several roots into one AssetDeps.
    std::unordered_set<std::wstring> seen;
    seen.reserve(inout.paths.size() * 2 + 16);
    for (const std::wstring& p : inout.paths) seen.insert(ToLowerW(p));
    std::unordered_set<std::wstring> missSeen;
    for (const std::wstring& m : inout.missingTr) missSeen.insert(ToLowerW(m));

    std::vector<Job> queue;
    queue.push_back({ NormalizeInnerPath(innerPath), PathParent(NormalizeInnerPath(innerPath)) });

    // Hard cap: a pathological reference cycle must not spin forever. Real
    // models resolve in far fewer than this.
    constexpr size_t kMaxAssets = 4000;

    while (!queue.empty() && inout.paths.size() < kMaxAssets) {
        const Job job = queue.back();
        queue.pop_back();
        if (job.path.empty()) continue;

        std::vector<u8> bytes;
        std::wstring    resolved;
        if (!TryLoad(loader, job.path, job.baseFolder, bytes, resolved)) {
            const std::wstring key = ToLowerW(job.path);
            if (missSeen.insert(key).second)
                inout.missingTr.push_back(job.path);
            continue;
        }

        const std::wstring key = ToLowerW(resolved);
        if (!seen.insert(key).second) continue;          // already collected
        inout.paths.push_back(resolved);

        const std::wstring folder = PathParent(resolved);
        const std::wstring ext    = PathExtension(resolved);

        auto push = [&](const std::wstring& p, const std::wstring& base) {
            if (!p.empty()) queue.push_back({ NormalizeInnerPath(p), base });
        };

        if (ext == L".bsr") {
            BsrFile bsr;
            if (!ParseBsr(bytes.data(), bytes.size(), bsr).ok()) {
                RecoverCustomBsr(resolved, bsr);
            }
            for (const BsrMaterialSet& ms : bsr.materialSets) push(ms.path, folder);
            for (const BsrMeshRef& mr : bsr.meshes)         push(mr.path, folder);
            for (const BsrAnimationRef& ar : bsr.animations) push(ar.path, folder);
            if (bsr.hasSkeleton) push(bsr.skeletonPath, folder);
            push(bsr.collision.meshPath, folder);

            // The mod palette: the ONLY place a model names its sounds, its
            // bone-mounted effects and its extra texture layers. Skipping it is
            // what made an export ship a silent, effect-less, half-textured
            // model - the exact defect reported against this tool.
            for (const std::wstring& t : bsr.modPalette.multiTextures) push(t, folder);
            for (const BsrParticleRef& p : bsr.modPalette.particles)    push(p.path, folder);
            for (const BsrSoundSet& ss : bsr.modPalette.soundSets)
                for (const BsrSoundTrack& tr : ss.tracks)               push(tr.path, folder);
        } else if (ext == L".bmt") {
            ++inout.materials;
            BmtFile bmt;
            ParseBmt(bytes.data(), bytes.size(), bmt);
            for (const BmtMaterial& m : bmt.materials) {
                push(m.diffuseMap.resolve(folder), folder);
                if (m.hasNormalMap) push(m.normalMap.resolve(folder), folder);
            }
        } else if (ext == L".bms") {
            ++inout.meshes;
            BmsFile bms;
            ParseBms(bytes.data(), bytes.size(), bms, false);   // headers only: we want paths
            push(bms.lightmapPath, folder);
        } else if (ext == L".efp") {
            EfpFile efp;
            ParseEfp(bytes.data(), bytes.size(), efp);   // partial trees still list resources
            const std::vector<std::wstring> rps = efp.allResourcePaths();
            for (const std::wstring& rp : rps) push(rp, folder);
            if (rps.empty()) ++inout.others;
        } else if (ext == L".bsk") {
            ++inout.skeletons;
        } else if (ext == L".ban") {
            ++inout.animations;
        } else if (ext == L".ddj" || ext == L".dds") {
            ++inout.textures;
        } else if (ext == L".cpd") {
            // Compound object: a list of .bsr parts plus a collision resource.
            // Exporting one without these gives the importer an empty shell.
            CpdFile cpd;
            ParseCpd(bytes.data(), bytes.size(), cpd);
            for (const std::wstring& rp : cpd.resourcePaths) push(rp, folder);
            push(cpd.collisionResourcePath, folder);
            ++inout.others;
        } else if (ext == L".dof") {
            // Dungeon: every block shell and every placed object is its own .bsr.
            DofFile dof;
            ParseDof(bytes.data(), bytes.size(), dof);
            for (const DofBlock& b : dof.blocks) {
                push(b.path, folder);
                for (const DofObject& o : b.objects) push(o.path, folder);
            }
            ++inout.others;
        } else if (ext == L".o" || ext == L".o2") {
            // Map object placement carries only an INDEX into the client-global
            // object.ifo, never a path, so the models cannot be followed from
            // here. Pull object.ifo itself in so the importer at least has the
            // table the indices mean something against.
            push(L"map\\object.ifo", std::wstring());
            ++inout.others;
        } else if (ext == L".2dt") {
            // Interface layout: each widget names its atlas texture.
            Nif2dtFile ui;
            ParseNif2dt(bytes.data(), bytes.size(), ui);
            for (const Nif2dtEntry& e : ui.entries) {
                push(e.image, folder);
                push(e.background, folder);
            }
            ++inout.others;
        } else {
            ++inout.others;
        }
    }
}

// ---------------------------------------------------------------------------
void CollectCompanionAssets(const std::vector<std::wstring>& rootPaths,
                            const AssetEnumerator& enumerate,
                            const AssetLoader& loader,
                            AssetDeps& inout,
                            const ProgressFn& progress) {
    if (!enumerate || !loader || rootPaths.empty()) return;

    // Base names to look for = the stems of what the user actually selected.
    // Deriving them from every collected texture as well would match half the
    // archive, so the sweep stays anchored to the user's choice.
    std::vector<std::wstring> bases;
    for (const std::wstring& r : rootPaths) {
        std::wstring stem = ToLowerW(PathFileName(NormalizeInnerPath(r)));
        const size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) stem.resize(dot);
        if (stem.size() >= 4 &&
            std::find(bases.begin(), bases.end(), stem) == bases.end())
            bases.push_back(stem);
    }
    if (bases.empty()) return;

    std::unordered_set<std::wstring> have;
    have.reserve(inout.paths.size() * 2 + 16);
    for (const std::wstring& p : inout.paths) have.insert(ToLowerW(p));

    auto related = [&](const std::wstring& stem) -> bool {
        for (const std::wstring& b : bases) {
            if (stem == b) return true;
            // "mall_avatar_m_arabia" (icon) - the base, prefixed
            if (stem.size() > b.size() + 1 &&
                stem.compare(stem.size() - b.size(), b.size(), b) == 0 &&
                stem[stem.size() - b.size() - 1] == L'_') return true;
            // "avatar_m_arabia_hat" - the base, suffixed (but never "..._arabia2")
            if (stem.size() > b.size() + 1 &&
                stem.compare(0, b.size(), b) == 0 && stem[b.size()] == L'_') return true;
        }
        return false;
    };

    // Pass 1: find candidates (cheap - string work only, no reads).
    std::vector<std::wstring> found;
    constexpr size_t kMaxCompanions = 2000;
    u64 scanned = 0;
    bool cancelled = false;
    enumerate([&](const std::wstring& innerPath) -> bool {
        if ((++scanned & 0x0FFF) == 0 &&
            !ReportProgress(progress, scanned, 0, L"Diğer arşivler taranıyor...")) {
            cancelled = true;
            return false;
        }
        if (found.size() >= kMaxCompanions) return false;
        std::wstring stem = ToLowerW(PathFileName(innerPath));
        const size_t dot = stem.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0) return true;      // no extension: skip
        stem.resize(dot);
        if (!related(stem)) return true;
        if (have.count(ToLowerW(NormalizeInnerPath(innerPath)))) return true;
        found.push_back(innerPath);
        return true;
    });
    if (cancelled) return;

    // Pass 2: pull each one in WITH its own references (a discovered .efp drags
    // its textures, a discovered .bsr its meshes).
    for (const std::wstring& f : found)
        CollectAssetDependenciesInto(f, loader, inout);
}

// ---------------------------------------------------------------------------
std::wstring ArchiveFolderName(const std::wstring& archivePath) {
    std::wstring name = PathFileName(NormalizeInnerPath(archivePath));
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);   // drop .pk2/.pk3/.vp/.lib
    name = ToLowerW(name);
    if (name.empty()) name = L"arsiv";
    return SanitizeFileName(name);
}

// ---------------------------------------------------------------------------
AssetDeps CollectAssetDependencies(const std::wstring& innerPath, const AssetLoader& loader) {
    AssetDeps d;
    CollectAssetDependenciesInto(innerPath, loader, d);
    return d;
}

} // namespace sro

