// ============================================================================
//  SRO Asset Studio - formats/AvatarSet.h
//
//  Avatar / costume "wear the WHOLE set" support.
//
//  A Silkroad avatar costume is not one file: the client composes several
//  skinned pieces onto the same character skeleton at once -
//      dress / suit  (the body garment)          itemdata  kind 2
//      hat / cap     (the head piece)            itemdata  kind 1
//      attach        (back flag / ensign)        itemdata  kind 3
//      wings         (ADDITIONAL attached piece) charactervisualchange cond 4
//      transform     (REPLACES the body model)   charactervisualchange cond 7
//
//  SRO Asset Studio previews ONE .bsr at a time, so selecting an avatar showed
//  a single piece (just the dress, or just the hat). This module rebuilds the
//  catalog from the archive's textdata and, given the .bsr the user selected,
//  works out every OTHER piece of the same family so the preview can compose
//  the complete, dressed costume - exactly what the old SroEffectViewer did
//  with equipAvatarSet() / famOf().
//
//  Read-only, archive-agnostic (PK2/LIB/VP). The catalog is cached per open
//  archive; building it walks the textdata once.
// ============================================================================
#pragma once

#include "../Common.h"
#include "Bsr.h"   // AssetLoader

namespace sro {

class IArchive;   // forward - catalog build enumerates the archive's textdata

// ---------------------------------------------------------------------------
// One wearable avatar piece, distilled from itemdata + charactervisualchange.
// Paths are stored slash-normalised (backslashes) but case-preserved.
// ---------------------------------------------------------------------------
struct AvatarSetItem {
    std::wstring code;        ///< ITEM_MALL_AVATAR_W_ANGEL_WING_DRESS (upper case)
    std::wstring bsrRel;      ///< item\Avatar\avatar_w_angel_wing_dress.bsr (skinned piece)
    std::wstring bsrBase;     ///< avatar_w_angel_wing_dress.bsr (lower) - match key
    std::wstring attachBsr;   ///< cond 4 wings: ADDITIONAL attached piece ("" when none)
    std::wstring replaceBsr;  ///< cond 7 transform: REPLACES the body model ("" when none)
    wchar_t      gender = L'a';  ///< 'm' / 'w' / 'a' (unknown)
    int          kind   = 0;     ///< 1 = hat, 2 = dress, 3 = attach
};

// ---------------------------------------------------------------------------
// The whole avatar catalog for one archive.
// ---------------------------------------------------------------------------
struct AvatarCatalog {
    std::vector<AvatarSetItem> items;

    /// Every .bsr that physically lives under an "avatar" folder of the archive
    /// the user opened (the geometry archive, e.g. Data.pk2). This is what makes
    /// "wear the set" robust on private servers whose itemdata names do NOT match
    /// the real file names: the hat/attach pieces sit right next to the dress
    /// with a shared name stem, so they are found by folder+stem, not by catalog.
    std::vector<std::wstring> avatarBsrPaths;

    bool empty() const { return items.empty() && avatarBsrPaths.empty(); }

    /// Find the piece whose main .bsr file name (lower case) equals `bsrBaseLower`.
    const AvatarSetItem* findByBsrBase(const std::wstring& bsrBaseLower) const;
};

// ---------------------------------------------------------------------------
// Build (or return a cached) catalog for `archive`. `loader` reads the actual
// bytes (the same loader the preview already uses, so sibling archives and any
// per-file decryption are honoured). Returns an empty catalog - never null -
// when the archive has no readable itemdata (e.g. Map.pk2). Safe to call from
// the preview worker thread; the enumeration runs under the archive lock.
// ---------------------------------------------------------------------------
const AvatarCatalog& GetAvatarCatalog(IArchive* archive, const AssetLoader& loader);

// ---------------------------------------------------------------------------
// Given the .bsr the user selected, the EXTRA pieces needed to show the whole
// costume. `pieces` are inner .bsr paths to compose ON TOP of the selected one
// (the selected piece itself is never repeated). Empty when the selection is
// not a catalogued avatar, so the caller simply keeps the single-piece preview.
// ---------------------------------------------------------------------------
struct AvatarSetPlan {
    std::wstring              family;       ///< family stem, for the info line
    std::vector<std::wstring> pieces;       ///< extra inner .bsr paths to add
    int                       totalPieces = 0;   ///< pieces incl. the selected one
    bool empty() const { return pieces.empty(); }
};

AvatarSetPlan PlanAvatarSet(const AvatarCatalog& cat, const std::wstring& selectedBsrInnerPath);

// ---------------------------------------------------------------------------
// Borrow a real CHARACTER skeleton so skinned attach pieces (wings) can be
// seated at the character's attach bone exactly like the game. Gender is read
// from the avatar path (`avatar_m_` / `avatar_w_`); a standard reference body's
// skeleton is loaded (cached). Returns false when none is reachable - the caller
// then falls back to the geometric placement.
// ---------------------------------------------------------------------------
struct BskFile;   // fwd
bool LoadAvatarCharSkeleton(const std::wstring& avatarInnerPath,
                            const AssetLoader& loader, BskFile& out);

} // namespace sro
