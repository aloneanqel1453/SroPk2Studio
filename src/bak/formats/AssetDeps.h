// ============================================================================
//  SRO Asset Studio - formats/AssetDeps.h
//
//  "Export everything this asset needs."
//
//  A Silkroad asset is never self-contained: a .bsr names meshes, a material
//  set, a skeleton and animations; the material set names textures; a mesh may
//  name a lightmap; an .efp names its textures and meshes. Exporting only the
//  file the user clicked produces something nobody can open, so the export
//  command walks these references and takes the whole closure with it.
//
//  Resolution follows exactly the same rules the preview uses (direct path,
//  then relative to the referrer's folder, then referrer-folder + file name),
//  so whatever the 3D view can draw is also what gets exported.
// ============================================================================
#pragma once

#include "../Common.h"
#include "Bsr.h"   // AssetLoader

namespace sro {

struct AssetDeps {
    /// Every asset needed, INCLUDING the root file itself. Archive inner paths,
    /// resolved (so they can be read back and written out directly).
    std::vector<std::wstring> paths;

    /// References that could not be resolved anywhere (Turkish, for the log).
    std::vector<std::wstring> missingTr;

    /// Per-kind counts for the summary line.
    size_t meshes = 0, materials = 0, textures = 0, skeletons = 0, animations = 0, others = 0;

    size_t count() const { return paths.size(); }
};

/// Walk everything `innerPath` references, recursively.
///   .bsr -> .bmt (+its .ddj), .bms (+lightmap .ddj), .bsk, .ban, collision .bms
///   .bmt -> .ddj (diffuse + normal map)
///   .bms -> lightmap .ddj
///   .efp -> .ddj / .bms (and those meshes' own references)
/// Anything else resolves to just itself. Never throws; unresolved references
/// are reported in `missingTr` instead of failing the whole export.
AssetDeps CollectAssetDependencies(const std::wstring& innerPath, const AssetLoader& loader);

/// Add the dependencies of `innerPath` into an existing set (used when several
/// files - or a whole folder - are exported at once, so shared textures are
/// collected only once).
void CollectAssetDependenciesInto(const std::wstring& innerPath, const AssetLoader& loader,
                                  AssetDeps& inout);

/// Folder name an exported asset is filed under, derived from the container it
/// came from: "C:\...\Data.pk2" -> "data", "Media.pk3" -> "media". Exports are
/// grouped this way so a costume pulled from Data plus its textures from Media
/// land in the client's own layout instead of one flat mixed tree.
std::wstring ArchiveFolderName(const std::wstring& archivePath);

/// Walks every file of every open container (primary + siblings). `visit`
/// returns false to stop early.
using AssetEnumerator = std::function<void(const std::function<bool(const std::wstring&)>& visit)>;

/// Companion sweep across ALL containers.
///
/// The reference closure only finds what a file explicitly names. A costume
/// also has an inventory ICON in Media, an aura .efp in Particles, a .wav in
/// Music - none of which the .bsr mentions. Those ship under the SAME asset
/// name, so they are found by name across every container:
///     stem == base | stem ends with "_"+base | stem starts with base+"_"
/// (e.g. base "avatar_m_arabia" also picks up "mall_avatar_m_arabia" and
/// "avatar_m_arabia_hat", but never "avatar_m_arabia2"). Whatever is found is
/// itself expanded through CollectAssetDependenciesInto, so a discovered .efp
/// brings its own textures along.
void CollectCompanionAssets(const std::vector<std::wstring>& rootPaths,
                            const AssetEnumerator& enumerate,
                            const AssetLoader& loader,
                            AssetDeps& inout,
                            const ProgressFn& progress = ProgressFn());

/// The client's TEXT DATA half of an export: the row that names this model
/// also names its inventory ICON, and rows keyed by the object's codename name
/// its sounds and its ambient effects. Every *.txt under any textdata folder
/// is scanned - the numbering differs from client to client
/// (characterdata_38800 / _5000 / _12313), so NO file name is assumed.
/// `matchedRowsOut` receives "<file> TAB <row>" for each row used, so the importer
/// can paste them into their own tables.
/// One source table, reduced to the rows that concern this object.
struct DataTableRows {
    std::wstring              innerPath;   ///< server_dep\silkroad\textdata\x.txt
    std::vector<std::wstring> header;      ///< the table's own leading // lines
    std::vector<std::wstring> rows;        ///< only the matching rows
    bool                      utf16 = false;   ///< source encoding, mirrored back
};

void CollectDataTableCompanions(const std::vector<std::wstring>& rootPaths,
                                const AssetEnumerator& enumerate,
                                const AssetLoader& loader,
                                AssetDeps& inout,
                                std::vector<DataTableRows>& tablesOut,
                                const ProgressFn& progress);


} // namespace sro
