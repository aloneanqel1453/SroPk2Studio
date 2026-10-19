// ============================================================================
//  SRO Asset Studio - formats/Nif2dt.h
//
//  .2dt - NewInterface entry table.  Wiki: NewInterface.md
//
//    4  u32 entryCount
//    per entry (fixed size, no length prefixes):
//       char[64]  Name
//       char[256] Image
//       char[256] Background
//       char[128] Text            (UI_STRING id)
//       char[64]  Description
//       char[64]  Prototype
//       u32 Type, Id, ParentId, GrandParentId, Unk02, Unk03
//       u32 Color                 (RGBA_8888)
//       u32 Rect.X, Rect.Y, Rect.Width, Rect.Height
//       8 x f32 UV corners        (TL, TR, BR, BL)
//       u32 Unk04, ContentId, IsRoot, Unk07 .. Unk19
//       u32 Style
//
//  Total is a constant; kNif2dtEntrySize is asserted below so a bad file is
//  detected immediately instead of drifting through the table.
//
//  Preview: the entry table (sortable) plus a laid-out UI preview built from
//  the parent/child relations and the client rectangles.
//
//  Implementation owner: Agent D (formats).
// ============================================================================
#pragma once

#include "../Common.h"

namespace sro {

constexpr size_t kNif2dtNameLen        = 64;
constexpr size_t kNif2dtImageLen       = 256;
constexpr size_t kNif2dtBackgroundLen  = 256;
constexpr size_t kNif2dtTextLen        = 128;
constexpr size_t kNif2dtDescriptionLen = 64;
constexpr size_t kNif2dtPrototypeLen   = 64;

/// 64 + 256 + 256 + 128 + 64 + 64 = 832 string bytes
/// + 6 u32 + 1 colour + 4 rect = 11 u32 = 44
/// + 8 f32 = 32
/// + ContentId/IsRoot/Unk04/Unk07..Unk19 = 16 u32 = 64
/// + Style = 4
constexpr size_t kNif2dtEntrySize = 832 + 44 + 32 + 64 + 4;
static_assert(kNif2dtEntrySize == 976, "2dt entry size must be 976 bytes");

struct Nif2dtEntry {
    std::wstring name;
    std::wstring image;         ///< .ddj path of the atlas
    std::wstring background;
    std::wstring text;          ///< UI_STRING id
    std::wstring description;
    std::wstring prototype;

    u32 type          = 0;      ///< NewInterfaceType
    u32 id            = 0;
    u32 parentId      = 0;
    u32 grandParentId = 0;
    u32 unk02 = 0, unk03 = 0;
    u32 color         = 0;      ///< RGBA_8888

    i32 x = 0, y = 0, width = 0, height = 0;

    Vec2 uvTopLeft, uvTopRight, uvBottomRight, uvBottomLeft;

    u32 unk04     = 0;
    u32 contentId = 0;
    u32 isRoot    = 0;
    u32 unk[13]   = {};
    u32 style     = 0;          ///< NewInterfaceStyle

    /// Absolute rectangle. The .2dt rect X/Y are already screen coordinates
    /// (NOT parent-relative), so absX/absY == x/y; kept for call-site clarity.
    i32 absX = 0, absY = 0;

    /// False for widgets the engine hides in the default state (popup roots,
    /// inactive tab pages). Rendering only the visible set makes the panel look
    /// like one cohesive window instead of every dialog state at once.
    bool initiallyVisible = true;

    /// True when the widget rectangle is within a sane UI canvas (guards against
    /// junk coordinates in some MaxiGuard-authored .2dt files).
    bool saneRect() const;
};

/// One selectable page of the window's MAIN tab strip. A .2dt stores every page
/// stacked on the same rectangle, so the preview shows one at a time and lets
/// the user step through them.
struct Nif2dtTabPage {
    size_t       entryIndex = 0;   ///< index of the TabButton itself
    u32          contentId  = 0;   ///< the content frame the tab owns
    std::wstring text;             ///< the tab's UI_STRING id (resolved for display)
};

struct Nif2dtFile {
    std::vector<Nif2dtEntry> entries;

    /// The window's main tab strip (empty when the window has no tabs).
    std::vector<Nif2dtTabPage> tabs;

    /// Entry index of the tab currently shown, so the renderer can swap that
    /// tab to its "_on" texture. SIZE_MAX when the window has no tabs.
    size_t activeTabEntry = static_cast<size_t>(-1);

    /// Index lookup by entry id; -1 when absent.
    int indexOfId(u32 id) const;
    /// Direct children of `id` (or of the roots when id == 0).
    std::vector<int> childrenOf(u32 id) const;

    /// Overall size of the laid-out UI, for the preview canvas size.
    void layoutBounds(i32& w, i32& h) const;

    /// Full bounding rectangle (origin + size) of the initially-visible, sane
    /// widgets. The composite subtracts (x,y) so the panel is flush to (0,0).
    void layoutRect(i32& x, i32& y, i32& w, i32& h) const;

    std::wstring summaryTr() const;
};

Result ParseNif2dt(const u8* data, size_t size, Nif2dtFile& out);

/// Recompute `initiallyVisible` with `activeTab` (an index into `file.tabs`) as
/// the selected page. ParseNif2dt applies tab 0; call this to view another page.
void SetNif2dtActiveTab(Nif2dtFile& file, size_t activeTab);

/// Heuristic: entryCount * 976 + 4 == size (or close enough).
bool IsNif2dt(const u8* data, size_t size);

/// Turkish name for a NewInterfaceType value.
const wchar_t* Nif2dtTypeNameTr(u32 type);

} // namespace sro
