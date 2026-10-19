// ============================================================================
//  SRO Asset Studio - formats/Nif2dt.cpp
//
//  .2dt NewInterface entry table.  Wiki: NewInterface.md
//
//  The file has NO signature: a u32 count followed by that many fixed 976-byte
//  records. That relation IS the signature - a file whose size is not exactly
//  4 + count * 976 is not a .2dt, and IsNif2dt checks precisely that.
//
//  Entry rectangles are stored relative to the parent widget, so absX/absY are
//  computed by walking the parent chain (with a depth cap, because a corrupt
//  file can trivially describe a cycle).
// ============================================================================
#include "Nif2dt.h"
#include "../core/I18n.h"   // UI strings follow the chosen language

#include <unordered_map>
#include <vector>

namespace sro {

namespace {

constexpr UINT kPathCodePage   = 1252;
constexpr UINT kKoreanCodePage = 949;    ///< CP949 / EUC-KR - the game's native text

/// Guard for the parent-chain walk. Real interfaces nest 4-5 deep.
constexpr int kMaxParentDepth = 64;

/// A fixed-size char field, NUL terminated, decoded to UTF-16.
///
/// Silkroad is a Korean product: the Name/Description/Prototype fields carry the
/// original designer annotations in CP949 (e.g. "이름표시 스태틱"). Reading them
/// as CP1252 turns every one of those into mojibake, so decode high-byte strings
/// as CP949 first and only fall back to CP1252 when that fails. Pure-ASCII
/// fields (all the .ddj paths and UI_STRING ids) decode identically either way.
std::wstring ReadFixed(ByteReader& r, size_t n) {
    const std::string raw = r.readFixedString(n);
    bool highByte = false;
    for (unsigned char c : raw) if (c >= 0x80) { highByte = true; break; }
    if (!highByte) return MultiByteToWide(raw, kPathCodePage);
    const std::wstring korean = MultiByteToWide(raw, kKoreanCodePage);
    if (!korean.empty()) return korean;
    return MultiByteToWide(raw, kPathCodePage);
}

} // namespace

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------
int Nif2dtFile::indexOfId(u32 id) const {
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].id == id) return static_cast<int>(i);
    return -1;
}

std::vector<int> Nif2dtFile::childrenOf(u32 id) const {
    std::vector<int> out;
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].parentId == id) out.push_back(static_cast<int>(i));
    return out;
}

// A widget rectangle is "sane" for layout purposes when it sits inside a
// reasonable UI canvas. Some (esp. MaxiGuard-authored) .2dt files carry junk
// coordinates that would otherwise blow the bounds up to ~1,000,000 px and
// scatter everything; those entries are ignored for sizing/compositing.
bool Nif2dtEntry::saneRect() const {
    // Real Silkroad interfaces live inside ~2048 px; use 4096 as a generous
    // ceiling. MaxiGuard junk widgets (x/y in the hundred-thousands) fall out.
    return width  > 0 && width  <= 4096 &&
           height > 0 && height <= 4096 &&
           absX >= -2048 && absX <= 4096 &&
           absY >= -2048 && absY <= 4096;
}

void Nif2dtFile::layoutRect(i32& x, i32& y, i32& w, i32& h) const {
    i32 minX = 0x7FFFFFFF, minY = 0x7FFFFFFF, maxX = -0x7FFFFFFF, maxY = -0x7FFFFFFF;
    bool any = false;
    for (const Nif2dtEntry& e : entries) {
        if (!e.saneRect() || !e.initiallyVisible) continue;
        any = true;
        minX = (std::min)(minX, e.absX);
        minY = (std::min)(minY, e.absY);
        maxX = (std::max)(maxX, e.absX + e.width);
        maxY = (std::max)(maxY, e.absY + e.height);
    }
    if (!any) { x = 0; y = 0; w = 800; h = 600; return; }
    x = minX; y = minY;
    w = maxX - minX; h = maxY - minY;
    if (w <= 0) w = 1; if (w > 8192) w = 8192;
    if (h <= 0) h = 1; if (h > 8192) h = 8192;
}

void Nif2dtFile::layoutBounds(i32& w, i32& h) const {
    i32 x, y;
    layoutRect(x, y, w, h);
}

// ---------------------------------------------------------------------------
// IsNif2dt
// ---------------------------------------------------------------------------
bool IsNif2dt(const u8* data, size_t size) {
    if (!data || size < 4 + kNif2dtEntrySize) return false;
    u32 count = 0;
    std::memcpy(&count, data, 4);
    if (count == 0 || count > 100000) return false;
    const u64 expect = 4ull + static_cast<u64>(count) * kNif2dtEntrySize;
    return expect == size;
}

// ---------------------------------------------------------------------------
// ParseNif2dt
// ---------------------------------------------------------------------------
Result ParseNif2dt(const u8* data, size_t size, Nif2dtFile& out) {
    out = Nif2dtFile();

    if (!data || size < 4)
        return Result::Fail(Status::Truncated, T(L"2DT: dosya 4 bayttan kısa."));

    ByteReader r(data, size);
    const u32 count = r.readU32();
    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"2DT: girdi sayısı okunamadı."));

    if (count == 0)
        return Result::Fail(Status::BadFormat, T(L"2DT: girdi sayısı sıfır."));
    if (count > r.remaining() / kNif2dtEntrySize)
        return Result::Fail(Status::BadFormat,
                            T(L"2DT: girdi sayısı dosya boyutuna sığmıyor (") +
                            FormatCountTr(count) + T(L" x 976 bayt gerekiyor, ") +
                            FormatBytesTr(r.remaining()) + T(L" var)."));

    out.entries.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        const size_t entryStart = r.pos();
        Nif2dtEntry e;

        e.name        = ReadFixed(r, kNif2dtNameLen);
        e.image       = ReadFixed(r, kNif2dtImageLen);
        e.background  = ReadFixed(r, kNif2dtBackgroundLen);
        e.text        = ReadFixed(r, kNif2dtTextLen);
        e.description = ReadFixed(r, kNif2dtDescriptionLen);
        e.prototype   = ReadFixed(r, kNif2dtPrototypeLen);

        e.type          = r.readU32();
        e.id            = r.readU32();
        e.parentId      = r.readU32();
        e.grandParentId = r.readU32();
        e.unk02         = r.readU32();
        e.unk03         = r.readU32();
        e.color         = r.readU32();

        e.x      = r.readI32();
        e.y      = r.readI32();
        e.width  = r.readI32();
        e.height = r.readI32();

        e.uvTopLeft     = r.readVec2();
        e.uvTopRight    = r.readVec2();
        e.uvBottomRight = r.readVec2();
        e.uvBottomLeft  = r.readVec2();

        e.unk04     = r.readU32();
        e.contentId = r.readU32();
        e.isRoot    = r.readU32();
        for (int k = 0; k < 13; ++k) e.unk[k] = r.readU32();
        e.style = r.readU32();

        if (r.failed())
            return Result::Fail(Status::Truncated,
                                L"2DT: " + FormatCountTr(i) + T(L". girdide dosya bitti."));

        // The record is a fixed size; realign in case a shard added a field.
        if (!r.seek(entryStart + kNif2dtEntrySize))
            return Result::Fail(Status::Truncated, T(L"2DT: girdi hizalaması bozuk."));

        out.entries.push_back(std::move(e));
    }

    // -----------------------------------------------------------------------
    // The rectangle X/Y are ABSOLUTE screen coordinates (verified against the
    // reference renderer) - NOT parent-relative. Accumulating parent offsets
    // double-counts and scatters widgets, so absX/absY == x/y.
    // -----------------------------------------------------------------------
    for (Nif2dtEntry& e : out.entries) { e.absX = e.x; e.absY = e.y; }

    // -----------------------------------------------------------------------
    // Main tab strip: the strip with the most tabs whose members carry a page
    // (contentId). That is the window's top-level pager - the preview shows one
    // of its pages at a time and lets the user step through them.
    // -----------------------------------------------------------------------
    {
        std::unordered_map<u32, std::vector<size_t>> strips;
        for (size_t i = 0; i < out.entries.size(); ++i)
            if (out.entries[i].type == 16 && out.entries[i].contentId != 0)
                strips[out.entries[i].parentId].push_back(i);
        const std::vector<size_t>* best = nullptr;
        for (const auto& s : strips)
            if (!best || s.second.size() > best->size()) best = &s.second;
        if (best && best->size() > 1) {
            // Only offer a page whose content frame actually EXISTS in this file.
            // Several windows (guild.2dt) have tabs pointing at pages the client
            // builds elsewhere - listing those would hand the user a blank page.
            for (size_t idx : *best) {
                const u32 cid = out.entries[idx].contentId;
                bool hasContent = false;
                for (const Nif2dtEntry& e : out.entries)
                    if (e.id == cid && e.type != 16) { hasContent = true; break; }
                if (hasContent)
                    out.tabs.push_back({ idx, cid, out.entries[idx].text });
            }
            if (out.tabs.size() < 2) out.tabs.clear();   // nothing to page through
        }
    }

    SetNif2dtActiveTab(out, 0);
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// SetNif2dtActiveTab - (re)compute initiallyVisible for one selected page.
//
// A .2dt packs EVERY dialog state (popups, all tab pages) into one file, all
// stacked on the same rectangle. The engine draws only the active set; drawing
// everything is what made the preview look like several windows on top of each
// other.
// ---------------------------------------------------------------------------
void SetNif2dtActiveTab(Nif2dtFile& out, size_t activeTab) {
    {
        const size_t n = out.entries.size();
        std::unordered_map<u32, std::vector<size_t>> byId;
        for (size_t i = 0; i < n; ++i)
            byId[out.entries[i].id].push_back(i);
        std::vector<char> hidden(n, 0);
        auto lower = [](std::wstring s) { for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c)); return s; };

        // Build a REAL tree by index.
        //
        // ids are NOT unique in a .2dt (29 duplicates in optionwnd): id 78 is
        // both the "Video" page frame and a tab button inside the "Key Map"
        // page, so "the entry with id 78" is ambiguous and a naive walk leaks
        // between pages. grandParentId resolves it exactly - a child's parent is
        // the entry whose id == parentId AND whose own parentId == grandParentId.
        // Measured on optionwnd: 207/207 entries resolve to exactly one parent.
        std::vector<std::vector<size_t>> childrenOf(n);
        for (size_t i = 0; i < n; ++i) {
            const Nif2dtEntry& e = out.entries[i];
            if (e.parentId == 0) continue;
            auto cand = byId.find(e.parentId);
            if (cand == byId.end()) continue;
            size_t parentIdx = SIZE_MAX;
            for (size_t p : cand->second)
                if (out.entries[p].parentId == e.grandParentId) {
                    if (parentIdx != SIZE_MAX) { parentIdx = SIZE_MAX; break; }  // still ambiguous
                    parentIdx = p;
                }
            if (parentIdx == SIZE_MAX && cand->second.size() == 1)
                parentIdx = cand->second[0];                 // only one candidate anyway
            if (parentIdx != SIZE_MAX && parentIdx != i)
                childrenOf[parentIdx].push_back(i);
        }

        std::vector<size_t> stack;
        auto hideSubtree = [&](size_t start) {
            stack.clear();
            stack.push_back(start);
            while (!stack.empty()) {
                const size_t i = stack.back();
                stack.pop_back();
                if (hidden[i]) continue;
                hidden[i] = 1;
                for (size_t c : childrenOf[i])
                    if (!hidden[c]) stack.push_back(c);
            }
        };
        // Hide the content frame a tab owns. Skip TabButton entries that merely
        // reuse the same id - a tab button is not the page.
        auto hidePagesWithId = [&](u32 id) {
            if (id == 0 || id == 0xFFFFFFFFu) return;
            auto it = byId.find(id);
            if (it == byId.end()) return;
            for (size_t idx : it->second)
                if (out.entries[idx].type != 16) hideSubtree(idx);
        };

        // 1. Popup roots are never part of the default view. The authoring tool
        //    records "popup" in the Korean designer annotation (Description /
        //    Prototype), not in Text - matching only name+text found nothing.
        for (size_t i = 0; i < n; ++i) {
            const Nif2dtEntry& e = out.entries[i];
            if (e.parentId == 0) continue;
            const std::wstring lbl = lower(e.name + L" " + e.text + L" " +
                                           e.description + L" " + e.prototype);
            if (lbl.find(L"popup") != std::wstring::npos ||
                lbl.find(L"팝업") != std::wstring::npos)
                hideSubtree(i);
        }

        // 2. Tab pages: every TabButton points at the content frame it owns
        //    (contentId). Exactly ONE page per strip may be drawn. For the MAIN
        //    strip the caller picks which one (activeTab); every other strip
        //    keeps its first tab. Origin authors every tab with the "_off"
        //    texture, so file order - not the texture name - is the default.
        {
            std::unordered_map<u32, std::vector<size_t>> strips;   // parentId -> tab indices
            for (size_t i = 0; i < n; ++i)
                if (out.entries[i].type == 16) strips[out.entries[i].parentId].push_back(i);

            // Which entry index is the selected page of the main strip?
            size_t keepIndex = SIZE_MAX;
            u32    mainParent = 0xFFFFFFFFu;
            if (!out.tabs.empty()) {
                const size_t sel = (activeTab < out.tabs.size()) ? activeTab : 0;
                keepIndex  = out.tabs[sel].entryIndex;
                mainParent = out.entries[keepIndex].parentId;
            }
            out.activeTabEntry = keepIndex;

            // Collect the pages that MUST survive first. Sibling tabs sometimes
            // share one contentId (sub-views of a single page); hiding "the
            // other tabs' pages" without this keep-set wipes the page we chose.
            std::unordered_map<u32, char> keepContent;
            for (auto& strip : strips) {
                const bool isMain = (!out.tabs.empty() && strip.first == mainParent);
                bool first = true;
                for (size_t idx : strip.second) {
                    const bool keep = isMain ? (idx == keepIndex) : first;
                    if (!isMain) first = false;
                    if (keep) keepContent[out.entries[idx].contentId] = 1;
                }
            }
            for (auto& strip : strips) {
                for (size_t idx : strip.second) {
                    const u32 cid = out.entries[idx].contentId;
                    if (keepContent.count(cid)) continue;            // still in use
                    hidePagesWithId(cid);
                }
            }
        }

        for (size_t i = 0; i < n; ++i)
            out.entries[i].initiallyVisible = !hidden[i];
        {
            bool anyVisible = false;
            for (size_t i = 0; i < n; ++i)
                if (out.entries[i].initiallyVisible && out.entries[i].saneRect()) { anyVisible = true; break; }
            // Safety net: never return a blank panel. If the rules hid everything
            // (unexpected authoring), fall back to showing the whole file.
            if (!anyVisible)
                for (size_t i = 0; i < n; ++i) out.entries[i].initiallyVisible = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Nif2dtTypeNameTr - NewInterfaceType (wiki: NewInterfaceType.md)
// ---------------------------------------------------------------------------
const wchar_t* Nif2dtTypeNameTr(u32 type) {
    switch (type) {
        case 0:  return T(L"Ana çerçeve (CNIFMainFrame)");
        case 1:  return T(L"Çerçeve (CNIFrame)");
        case 2:  return T(L"Döşeme (CNIFNormaltile)");
        case 3:  return T(L"Esnek döşeme (CNIFStretch)");
        case 4:  return T(L"Düğme (CNIFButton)");
        case 5:  return T(L"Etiket (CNIFStatic)");
        case 6:  return T(L"Metin kutusu (CNIFEdit)");
        case 7:  return T(L"Metin alanı (CNIFTextBox)");
        case 8:  return T(L"Yuva (CNIFSlot)");
        case 9:  return T(L"Izgara (CNIFLattice)");
        case 10: return T(L"Gösterge (CNIFGauge)");
        case 11: return T(L"Onay kutusu (CNIFCheckBox)");
        case 12: return T(L"Açılır liste (CNIFComboBox)");
        case 13: return T(L"Dikey kaydırma (CNIFVirticalScroll)");
        case 14: return T(L"Sayfa yöneticisi (CNIFPageManager)");
        case 15: return T(L"Bar penceresi (CNIFBarWnd)");
        case 16: return T(L"Sekme düğmesi (CNIFTabButton)");
        case 17: return T(L"Çift yönlü gösterge (CNIFBothSidesGauge)");
        case 18: return T(L"Pencere (CNIFWnd)");
        case 19: return T(L"Kaydırıcı (CNIFSlideCtrl)");
        case 20: return T(L"Sayı seçici (CNIFSpinButtonCtrl)");
        default: return T(L"Bilinmeyen tür");
    }
}

// ---------------------------------------------------------------------------
// summaryTr
// ---------------------------------------------------------------------------
std::wstring Nif2dtFile::summaryTr() const {
    std::wstring s;
    s += T(L"Girdi sayısı  : ") + FormatCountTr(entries.size()) + L"\n";

    i32 w = 0, h = 0;
    layoutBounds(w, h);
    s += FormatW(T(L"Yerleşim alanı: %d x %d piksel\n"), w, h);

    // Root widgets
    size_t roots = 0;
    for (const Nif2dtEntry& e : entries)
        if (e.isRoot || e.parentId == 0) ++roots;
    s += T(L"Kök öğe       : ") + FormatCountTr(roots) + L"\n";

    // Distinct atlas textures
    std::vector<std::wstring> atlases;
    for (const Nif2dtEntry& e : entries) {
        if (e.image.empty()) continue;
        const std::wstring key = ToLowerW(e.image);
        bool seen = false;
        for (const std::wstring& a : atlases) if (ToLowerW(a) == key) { seen = true; break; }
        if (!seen) atlases.push_back(e.image);
    }
    s += T(L"Kullanılan doku: ") + FormatCountTr(atlases.size()) + L"\n";
    {
        const size_t shown = (atlases.size() < 8) ? atlases.size() : 8;
        for (size_t i = 0; i < shown; ++i) s += L"    " + atlases[i] + L"\n";
        if (atlases.size() > shown)
            s += T(L"    ... ve ") + FormatCountTr(atlases.size() - shown) + T(L" doku daha\n");
    }

    // Type histogram
    u32 typeCount[24] = {};
    u32 otherTypes = 0;
    for (const Nif2dtEntry& e : entries) {
        if (e.type < 21) ++typeCount[e.type];
        else             ++otherTypes;
    }
    s += T(L"\nTür dağılımı:\n");
    for (u32 t = 0; t < 21; ++t)
        if (typeCount[t])
            s += L"    " + std::wstring(Nif2dtTypeNameTr(t)) + L" : " +
                 FormatCountTr(typeCount[t]) + L"\n";
    if (otherTypes)
        s += T(L"    Bilinmeyen tür : ") + FormatCountTr(otherTypes) + L"\n";

    const size_t shown = (entries.size() < 10) ? entries.size() : 10;
    if (shown) s += T(L"\nGirdiler (ilk ") + FormatCountTr(shown) + T(L"):\n");
    for (size_t i = 0; i < shown; ++i) {
        const Nif2dtEntry& e = entries[i];
        s += FormatW(L"    #%u  ", e.id) + e.name +
             FormatW(L"  [%d,%d %dx%d]  ", e.absX, e.absY, e.width, e.height) +
             Nif2dtTypeNameTr(e.type) + L"\n";
    }

    return s;
}

} // namespace sro
