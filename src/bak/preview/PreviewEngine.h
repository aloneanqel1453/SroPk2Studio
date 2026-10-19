// ============================================================================
//  SRO Asset Studio - preview/PreviewEngine.h   (vendored, UI-free)
//
//  The preview decoder of ui/PreviewPane.cpp without any window: decode one
//  asset exactly as the preview pane does and hand back what the pane would
//  show (image / 3D render / .2dt layout / text) plus the "Dosya Bilgisi" text.
// ============================================================================
#pragma once

#include "../Common.h"
#include "../core/IArchive.h"
#include "../formats/Bsr.h"        // AssetLoader
#include "../formats/FormatId.h"
#include "../render/Image.h"
#include "../render/Raster.h"     // Scene (BuildPreviewScene)

#include <vector>

namespace sro {

/// PreviewPane::RenderPreviewToImage as a free function (the --shot path):
/// decode `data` as `name` and rasterise the preview it would produce.
Result RenderPreviewToImage(const std::wstring& name,
                            const u8* data, size_t size,
                            int width, int height,
                            Image& out,
                            std::wstring* infoTextTr,
                            const AssetLoader& loader = AssetLoader(),
                            IArchive* archive = nullptr);

struct PreviewResult {
    FormatId     format = FormatId::Unknown;
    PreviewKind  kind   = PreviewKind::Hex;  ///< after decoding (an .nvm becomes Mesh, ...)
    std::wstring formatNameTr;               ///< ImageMeta::formatName when set, else FormatDisplayName
    std::wstring infoTr;                     ///< the "Dosya Bilgisi" block
    std::wstring textTr;                     ///< text / table / summary body
    bool         textIsSummary = false;      ///< textTr is a summary (not file text)
    std::wstring detailsTr;                  ///< MeshView::detailsTr() for a composed model
    Image        image;                      ///< texture (native size), 3D render or .2dt layout
    bool         rendered3d = false;
    std::vector<Image>        frames;        ///< EFP particle animation (renderEffect only)
    std::vector<Image>        uiPages;       ///< .2dt tab pages
    std::vector<std::wstring> uiPageLabels;
};

/// Decode `data` as `innerPath` through the pane's DecodePayload. Images keep
/// their native size; meshes / skeletons / animations / composed models (and
/// the .nvm / .cpd / .o2 3D regions) go through the headless renderer at
/// width x height. `renderEffect` false keeps an .efp to its summary.
Result BuildPreview(const std::wstring& innerPath, const u8* data, size_t size,
                    int width, int height, const AssetLoader& loader, IArchive* archive,
                    bool renderEffect, PreviewResult& out);

/// SroPk2Studio: like BuildPreview, but a 3D payload (mesh / composed model / map region /
/// dungeon / navmesh) is handed back as the scene MeshView would draw (upright, materials
/// applied) instead of a picture, for an interactive viewer. `hasScene` tells whether `scene`
/// was filled; every other field is the same as BuildPreview's.
Result BuildPreviewScene(const std::wstring& innerPath, const u8* data, size_t size,
                         const AssetLoader& loader, IArchive* archive,
                         PreviewResult& out, Scene& scene, bool& hasScene);

} // namespace sro
