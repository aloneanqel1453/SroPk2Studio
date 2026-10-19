// ============================================================================
//  SRO Asset Studio - preview/PreviewEngine.cpp   (vendored, UI-free)
//
//  The preview DECODER of ui/PreviewPane.cpp, lifted out of the window code:
//  PreviewPayload, DecodePayload (the FormatId switch with every format's
//  summary, image decode, EFP / NVM / .2dt / model composition), its helpers
//  (LoadAssetBytes, LoadTextureAsset, ComposePlacedRegion, ...) and the
//  headless renderer of PreviewPane::RenderPreviewToImage. Copied verbatim
//  except for: the ui/ includes (replaced by PreviewTheme.h), the window
//  class / message / option-strip constants (dropped), the --shot globals
//  (file-local, defaults), a renderEffect switch on the payload, and
//  RenderPreviewToImage split into decode + render (BuildPreview added).
// ============================================================================
#include "PreviewEngine.h"
#include "PreviewTheme.h"
#include "../core/AssetCrypt.h"
#include "../core/I18n.h"
#include "../core/SiblingArchives.h"

#include "../formats/Ddj.h"
#include "../formats/DdsDecode.h"
#include "../formats/Bms.h"
#include "../formats/Bmt.h"
#include "../formats/Bsr.h"
#include "../formats/AvatarSet.h"
#include "../formats/Bsk.h"
#include "../formats/Ban.h"
#include "../formats/Efp.h"
#include "../fx/EffectScene.h"
#include "../formats/Nif2dt.h"
#include "../formats/MapFiles.h"
#include "../formats/ClientData.h"
#include "../formats/TextFile.h"
#include "../render/ImageIO.h"
#include "../render/MeshView.h"

#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <cmath>

namespace sro {

namespace {

/// --shot globals of ui/PreviewPane.cpp; nothing sets them here, so the
/// default view / page 0 / first animation frame are always used.
int   g_shotPageIndex = 0;
float g_shotYawDeg    = 0.0f;
float g_shotPitchDeg  = 0.0f;
float g_shotRollDeg   = 0.0f;
bool  g_shotFreeOrbit = false;
float g_shotAnimMs    = 0.0f;

/// Apply --yaw / --pitch / --roll to a camera sitting at the DEFAULT view.
/// They stay OFFSETS from the default 3/4 view, but they are now exact turntable
/// offsets: under the old arcball the azimuth was already right while the
/// ELEVATION decayed (16 -> 0 by --yaw 90, flipping to -16 at 180) and up to 16
/// degrees of roll crept in. --freeorbit reproduces the old pictures exactly.
void ApplyShotCamera(Camera& cam) {
    constexpr f32 kDeg2Rad = 3.14159265358979f / 180.0f;
    cam.freeOrbit = g_shotFreeOrbit;
    if (g_shotYawDeg != 0.0f || g_shotPitchDeg != 0.0f)
        cam.orbit(g_shotYawDeg * kDeg2Rad, g_shotPitchDeg * kDeg2Rad);
    if (g_shotRollDeg != 0.0f) cam.roll(g_shotRollDeg * kDeg2Rad);
}

constexpr size_t kTextPreviewBytes = 8ull * 1024 * 1024;

// ---------------------------------------------------------------------------
// Everything one decode produces. Moved between threads exactly once.
// ---------------------------------------------------------------------------
struct PreviewPayload {
    u64          generation = 0;
    std::wstring displayName;
    std::wstring innerPath;
    FormatId     format = FormatId::Unknown;
    PreviewKind  kind   = PreviewKind::Hex;

    std::vector<u8> bytes;          ///< kept for the hex view
    Result          status;
    std::wstring    errorTr;
    std::wstring    infoTr;

    // image
    Image              image;
    ImageMeta          meta;
    std::vector<Image> mips;      ///< flat grid: face * mipCount + mip
    int                faceCount = 1;
    int                mipCount  = 1;
    std::vector<Image> animFrames;   ///< EFP: pre-rendered particle animation
    /// .2dt: one pre-rendered image per tab page, with the tabs' labels. A .2dt
    /// holds every page in one file; only one may be drawn at a time, so they are
    /// rendered up front and the user steps through them.
    std::vector<Image>        uiPages;
    std::vector<std::wstring> uiPageLabels;

    // text / summary / properties
    std::wstring text;
    bool         textIsSummary = false;

    // table
    std::vector<TableColumn>               columns;
    std::vector<std::vector<std::wstring>> rows;

    // geometry
    BmsFile       bms;   bool hasBms   = false;
    BskFile       bsk;   bool hasBsk   = false;
    BanFile       ban;   bool hasBan   = false;
    ComposedModel model; bool hasModel = false;
    bool          animateModel = false;   ///< model + bsk + ban -> skinned playback

    /// EVERY clip the model itself declares, in file order, so the user picks
    /// one instead of being given whatever the code guessed. `animLabels` are
    /// the bare file names (no ".ban"), exactly what the reference viewer lists.
    std::vector<std::wstring> animPaths;
    std::vector<std::wstring> animLabels;
    int                       animIndex = -1;   ///< which one is loaded
    std::wstring              animFolder;       ///< base folder for resolution

    // .2dt layout canvas
    Image layout;

    /// (vendored addition) false skips the EFP texture decode, the 32-frame
    /// particle render and the effect-mesh fallback; the summary is kept.
    bool renderEffect = true;
};

// ---------------------------------------------------------------------------
// GDI text -> Image, used by the headless --shot path for text/table previews.
// ---------------------------------------------------------------------------
bool RenderTextBlockToImage(const std::wstring& titleTr, const std::wstring& body,
                            int w, int h, Image& out) {
    if (w <= 0 || h <= 0) return false;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;              // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HDC screen = ::GetDC(nullptr);
    HDC dc  = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (!dc || !bmp || !pixels) {
        if (bmp) ::DeleteObject(bmp);
        if (dc) ::DeleteDC(dc);
        return false;
    }
    HGDIOBJ old = ::SelectObject(dc, bmp);

    RECT rc = { 0, 0, w, h };
    Theme::FillRect(dc, rc, Palette::BgPanel);
    Theme::DrawPanelBorder(dc, rc);

    RECT t = rc;
    t.left  += 12; t.right -= 12; t.top += 8;
    t.bottom = t.top + 26;
    Theme::DrawTextThemed(dc, t, titleTr, FontRole::UiBold, Palette::GoldBright,
                          DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX);

    RECT b = rc;
    b.left += 12; b.right -= 12;
    b.top   = t.bottom + 6;
    b.bottom -= 8;
    Theme::DrawTextThemed(dc, b, body, FontRole::Mono, Palette::Parchment,
                          DT_LEFT | DT_TOP | DT_NOPREFIX | DT_EXPANDTABS | DT_EDITCONTROL);

    ::GdiFlush();

    if (!out.resize(w, h)) {
        ::SelectObject(dc, old);
        ::DeleteObject(bmp);
        ::DeleteDC(dc);
        return false;
    }
    const u8* src = static_cast<const u8*>(pixels);
    u8* dst = out.data();
    for (int y = 0; y < h; ++y) {
        const u8* s = src + static_cast<size_t>(y) * w * 4;
        u8* d = dst + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
            s += 4; d += 4;
        }
    }

    ::SelectObject(dc, old);
    ::DeleteObject(bmp);
    ::DeleteDC(dc);
    return true;
}

/// Render a .2dt entry tree as a laid-out UI canvas.
bool RenderNif2dtLayout(const Nif2dtFile& file, int maxW, int maxH, Image& out) {
    i32 ox = 0, oy = 0, w = 0, h = 0;
    file.layoutRect(ox, oy, w, h);
    if (w <= 0 || h <= 0) { ox = 0; oy = 0; w = 800; h = 600; }
    const int cw = ui::ClampInt(static_cast<int>(w), 64, maxW);
    const int ch = ui::ClampInt(static_cast<int>(h), 64, maxH);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = cw;
    bi.bmiHeader.biHeight      = -ch;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HDC screen = ::GetDC(nullptr);
    HDC dc  = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (!dc || !bmp || !pixels) {
        if (bmp) ::DeleteObject(bmp);
        if (dc) ::DeleteDC(dc);
        return false;
    }
    HGDIOBJ old = ::SelectObject(dc, bmp);

    RECT rc = { 0, 0, cw, ch };
    Theme::FillRect(dc, rc, Palette::BgDarkest);

    const double sx = static_cast<double>(cw) / (std::max)(1, static_cast<int>(w));
    const double sy = static_cast<double>(ch) / (std::max)(1, static_cast<int>(h));

    for (const Nif2dtEntry& e : file.entries) {
        if (!e.saneRect() || !e.initiallyVisible) continue;
        RECT r;
        r.left   = static_cast<LONG>((e.absX - ox) * sx);
        r.top    = static_cast<LONG>((e.absY - oy) * sy);
        r.right  = static_cast<LONG>((e.absX + e.width - ox) * sx);
        r.bottom = static_cast<LONG>((e.absY + e.height - oy) * sy);
        Theme::DrawBorder(dc, r, e.isRoot ? Palette::GoldBright : Palette::BorderDim);
        if ((r.right - r.left) > 40 && (r.bottom - r.top) > 12) {
            RECT t = r;
            ::InflateRect(&t, -2, -1);
            Theme::DrawTextThemed(dc, t, e.name, FontRole::MonoSmall, Palette::FgDim,
                                  DT_SINGLELINE | DT_LEFT | DT_TOP |
                                  DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    ::GdiFlush();

    bool ok = out.resize(cw, ch);
    if (ok) {
        const u8* src = static_cast<const u8*>(pixels);
        u8* dstp = out.data();
        for (int y = 0; y < ch; ++y) {
            const u8* s = src + static_cast<size_t>(y) * cw * 4;
            u8* d = dstp + static_cast<size_t>(y) * cw * 4;
            for (int x = 0; x < cw; ++x) {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
                s += 4; d += 4;
            }
        }
    }
    ::SelectObject(dc, old);
    ::DeleteObject(bmp);
    ::DeleteDC(dc);
    return ok;
}

// ---------------------------------------------------------------------------
// textdata (UI_STRING) resolution so .2dt widgets show their REAL labels
// (Yes / No / OK / ...). The actual text lives in
// server_dep\silkroad\textdata\textuisystem.txt; a .2dt entry's Text field is
// just the string id. Cached and rebuilt when the archive changes.
// ---------------------------------------------------------------------------
struct UiStringTable {
    std::unordered_map<std::wstring, std::wstring> map;
    u64 sig = 0;
    const std::wstring* find(const std::wstring& id) const {
        auto it = map.find(id);
        return it == map.end() ? nullptr : &it->second;
    }
};

// Score a candidate cell for "is this clean English/Latin text". ASCII letters
// score +1; characters outside the basic Latin printable range (e.g. Vietnamese
// or mojibake) are penalised, so the English column wins over other languages.
int EnglishScore(const std::wstring& s) {
    int n = 0;
    for (wchar_t c : s) {
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')) ++n;
        else if (c > 0x7E) n -= 3;      // non-ASCII -> likely another language
    }
    return n;
}

void ParseUiStringDoc(const std::vector<u8>& bytes,
                      std::unordered_map<std::wstring, std::wstring>& map) {
    if (bytes.empty()) return;
    TextDocument doc;
    if (!LoadTextDocument(bytes.data(), bytes.size(), doc, 0).ok()) return;
    const std::wstring& t = doc.text;
    size_t start = 0;
    while (start <= t.size()) {
        const size_t nl = t.find(L'\n', start);
        std::wstring line = t.substr(start, (nl == std::wstring::npos ? t.size() : nl) - start);
        start = (nl == std::wstring::npos) ? t.size() + 1 : nl + 1;
        while (!line.empty() && (line.front() == L'\xFEFF' || line.front() == L'\r')) line.erase(0, 1);
        while (!line.empty() && (line.back() == L'\r')) line.pop_back();
        if (line.size() < 3 || (line[0] == L'/' && line[1] == L'/')) continue;

        std::vector<std::wstring> cols;
        size_t p = 0;
        for (;;) {
            const size_t tb = line.find(L'\t', p);
            cols.push_back(TrimW(line.substr(p, (tb == std::wstring::npos ? line.size() : tb) - p)));
            if (tb == std::wstring::npos) break;
            p = tb + 1;
        }
        if (cols.size() < 3 || cols[1].empty()) continue;

        // value = the column (after the key) richest in ASCII letters -> the
        // English text; else first non-"0" text column.
        int bestI = -1, bestScore = 0;
        for (size_t c = 2; c < cols.size(); ++c) {
            const int sc = EnglishScore(cols[c]);
            if (sc > bestScore) { bestScore = sc; bestI = static_cast<int>(c); }
        }
        std::wstring val;
        if (bestI >= 0) val = cols[static_cast<size_t>(bestI)];
        else for (size_t c = 2; c < cols.size(); ++c)
            if (!cols[c].empty() && cols[c] != L"0") { val = cols[c]; break; }
        if (!val.empty() && val != L"0") map[cols[1]] = val;
    }
}

const UiStringTable& GetUiStrings(const AssetLoader& loader) {
    static UiStringTable tbl;
    if (!loader) return tbl;
    std::vector<u8> bytes;
    if (!loader(L"server_dep\\silkroad\\textdata\\textuisystem.txt", bytes) || bytes.empty())
        loader(L"server_dep\\silkroad\\textdata\\textuisystem_1.txt", bytes);
    // Signature = size folded with a cheap FNV-1a content hash, so a DIFFERENT
    // archive whose textuisystem.txt is the same size still misses the cache.
    u64 sig = 1469598103934665603ull ^ static_cast<u64>(bytes.size());
    { const size_t step = (bytes.size() / 4096) + 1;              // sample the file
      for (size_t i = 0; i < bytes.size(); i += step) { sig ^= bytes[i]; sig *= 1099511628211ull; } }
    if (sig != 0 && sig == tbl.sig && !tbl.map.empty()) return tbl;   // cached
    tbl.map.clear(); tbl.sig = sig;
    if (!bytes.empty()) ParseUiStringDoc(bytes, tbl.map);
    std::vector<u8> more;
    if (loader(L"server_dep\\silkroad\\textdata\\textdata_object.txt", more) && !more.empty())
        ParseUiStringDoc(more, tbl.map);
    return tbl;
}

bool LooksLikeUiStringId(const std::wstring& s) {
    if (s.size() < 3) return false;
    bool hasAlpha = false;
    for (wchar_t c : s) {
        if (c == L'_') continue;
        if (c >= L'A' && c <= L'Z') { hasAlpha = true; continue; }
        if (c >= L'0' && c <= L'9') continue;
        return false;
    }
    return hasAlpha;
}

/// Resolve a .2dt entry's Text field to a display label.
std::wstring ResolveWidgetLabel(const Nif2dtEntry& e, const UiStringTable* strings) {
    if (e.text.empty()) return std::wstring();
    if (strings && LooksLikeUiStringId(e.text)) {
        if (const std::wstring* v = strings->find(e.text)) return *v;
        return std::wstring();                 // unknown id -> don't show the raw code
    }
    if (LooksLikeUiStringId(e.text)) return std::wstring();  // id but no table
    return e.text;                              // already plain text
}

/// Composite the REAL UI panel: for every entry, blit the sub-rectangle of its
/// atlas (given by the 4 UV corners) into the entry's client rectangle. This is
/// how the wiki renders a .2dt - the actual textured widget, not a wireframe.
/// Returns false (so the caller keeps the wireframe) when no texture loaded.
bool RenderNif2dtComposite(const Nif2dtFile& file, const AssetLoader& loader,
                           const std::wstring& baseFolder, int maxW, int maxH,
                           Image& out, const UiStringTable* strings = nullptr) {
    if (!loader) return false;
    i32 ox = 0, oy = 0, w = 0, h = 0;
    file.layoutRect(ox, oy, w, h);   // origin + size of the visible widgets
    if (w <= 0 || h <= 0) return false;

    const int cw = ui::ClampInt(static_cast<int>(w), 16, maxW);
    const int ch = ui::ClampInt(static_cast<int>(h), 16, maxH);
    const double sx = static_cast<double>(cw) / (std::max)(1, static_cast<int>(w));
    const double sy = static_cast<double>(ch) / (std::max)(1, static_cast<int>(h));
    if (!out.resize(cw, ch)) return false;

    // Dark, flat backdrop so semi-transparent UI art reads clearly.
    for (size_t i = 0, n = static_cast<size_t>(cw) * ch; i < n; ++i) {
        u8* d = out.data() + i * 4; d[0] = 26; d[1] = 20; d[2] = 14; d[3] = 255;
    }

    std::unordered_map<std::wstring, Image> cache;
    auto getAtlas = [&](const std::wstring& path) -> Image* {
        const std::wstring key = ToLowerW(NormalizeInnerPath(path));
        auto it = cache.find(key);
        if (it != cache.end()) return it->second.valid() ? &it->second : nullptr;
        Image img;
        std::vector<u8> bytes;
        std::wstring direct = NormalizeInnerPath(path);
        bool got = (!direct.empty() && loader(direct, bytes));
        if (!got && !baseFolder.empty()) {
            std::wstring joined = NormalizeInnerPath(PathJoin(baseFolder, path));
            got = (!joined.empty() && loader(joined, bytes));
        }
        if (got && !bytes.empty()) {
            if (!DecodeDdj(bytes.data(), bytes.size(), 0, 0, img, nullptr, nullptr)) {
                if (!DecodeDds(bytes.data(), bytes.size(), img, nullptr, nullptr)) {
                    ImageIO::Scope wic; ImageMeta m2;
                    ImageIO::DecodeAny(bytes.data(), bytes.size(), img, &m2);
                }
            }
        }
        Image& slot = cache[key];
        slot = std::move(img);
        return slot.valid() ? &slot : nullptr;
    };
    auto clamp01 = [](float a) { return a < 0.f ? 0.f : (a > 1.f ? 1.f : a); };

    // Standard client art for controls the .2dt leaves untextured.
    static const wchar_t* kStdBlackField  = L"interface\\ifcommon\\com_blacksquare_";
    static const wchar_t* kStdDownArrow   = L"interface\\ifcommon\\com_qst_downarrow_button.ddj";
    static const wchar_t* kStdScrollBar   = L"interface\\ifcommon\\com_scroll_bar.ddj";
    static const wchar_t* kStdScrollButton= L"interface\\ifcommon\\com_scroll_button.ddj";

    auto fillRect = [&](int dx0, int dy0, int dw, int dh, u8 r8, u8 g8, u8 b8) {
        for (int yy = 0; yy < dh; ++yy) {
            const int py = dy0 + yy; if (py < 0 || py >= ch) continue;
            for (int xx = 0; xx < dw; ++xx) {
                const int px = dx0 + xx; if (px < 0 || px >= cw) continue;
                u8* d = out.data() + (static_cast<size_t>(py) * cw + px) * 4;
                d[0] = r8; d[1] = g8; d[2] = b8; d[3] = 255;
            }
        }
    };

    // Blit one texture into a destination rect, stretching it (alpha blended).
    auto blitStretch = [&](const Image& src, int dx0, int dy0, int dw, int dh) {
        const int tw = src.width(), th = src.height();
        if (tw <= 0 || th <= 0 || dw <= 0 || dh <= 0) return;
        for (int yy = 0; yy < dh; ++yy) {
            const int py = dy0 + yy; if (py < 0 || py >= ch) continue;
            int syy = static_cast<int>((yy + 0.5) * th / dh);
            if (syy < 0) syy = 0; if (syy >= th) syy = th - 1;
            for (int xx = 0; xx < dw; ++xx) {
                const int px = dx0 + xx; if (px < 0 || px >= cw) continue;
                int sxx = static_cast<int>((xx + 0.5) * tw / dw);
                if (sxx < 0) sxx = 0; if (sxx >= tw) sxx = tw - 1;
                const u8* s = src.data() + (static_cast<size_t>(syy) * tw + sxx) * 4;
                const int a = s[3];
                if (a <= 0) continue;
                u8* d = out.data() + (static_cast<size_t>(py) * cw + px) * 4;
                if (a >= 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
                else {
                    d[0] = static_cast<u8>((s[0] * a + d[0] * (255 - a)) / 255);
                    d[1] = static_cast<u8>((s[1] * a + d[1] * (255 - a)) / 255);
                    d[2] = static_cast<u8>((s[2] * a + d[2] * (255 - a)) / 255);
                    d[3] = 255;
                }
            }
        }
    };

    // A frame whose Image/Background ends with '_' is NOT a file - it is the
    // PREFIX of an 8-piece frame set that the client assembles as a nine-slice:
    //   <p>left_up   <p>mid_up    <p>right_up
    //   <p>left_side   (open)     <p>right_side
    //   <p>left_down <p>mid_down  <p>right_down
    // This is how every window border, title bar and inner panel is authored
    // (mframe_wnd_, int_window_, opt_inner_box_ ...). Treating the prefix as a
    // literal filename fails to load, which silently dropped ALL window chrome.
    auto drawNineSlice = [&](const std::wstring& prefix,
                             int dx0, int dy0, int dw, int dh, int* outTopH = nullptr) -> bool {
        auto part = [&](const wchar_t* suffix) { return getAtlas(prefix + suffix + L".ddj"); };
        Image* lu = part(L"left_up");    Image* mu = part(L"mid_up");    Image* ru = part(L"right_up");
        Image* ls = part(L"left_side");                                  Image* rs = part(L"right_side");
        Image* ld = part(L"left_down");  Image* md = part(L"mid_down");  Image* rd = part(L"right_down");
        if (!lu && !mu && !ru && !ls && !rs && !ld && !md && !rd) return false;

        const int topH  = mu ? mu->height() : (lu ? lu->height() : 0);
        const int botH  = md ? md->height() : (ld ? ld->height() : 0);
        const int leftW = ls ? ls->width()  : (lu ? lu->width()  : 0);
        const int rightW= rs ? rs->width()  : (ru ? ru->width()  : 0);
        const int luW = lu ? lu->width() : leftW, ruW = ru ? ru->width() : rightW;
        const int ldW = ld ? ld->width() : leftW, rdW = rd ? rd->width() : rightW;

        // edges first, corners on top so the corner art always wins
        if (mu) blitStretch(*mu, dx0 + luW, dy0, (std::max)(0, dw - luW - ruW), topH);
        if (md) blitStretch(*md, dx0 + ldW, dy0 + dh - botH, (std::max)(0, dw - ldW - rdW), botH);
        const int sideY = dy0 + topH, sideH = (std::max)(0, dh - topH - botH);
        if (ls) blitStretch(*ls, dx0, sideY, leftW, sideH);
        if (rs) blitStretch(*rs, dx0 + dw - rightW, sideY, rightW, sideH);
        if (lu) blitStretch(*lu, dx0, dy0, lu->width(), lu->height());
        if (ru) blitStretch(*ru, dx0 + dw - ru->width(), dy0, ru->width(), ru->height());
        if (ld) blitStretch(*ld, dx0, dy0 + dh - ld->height(), ld->width(), ld->height());
        if (rd) blitStretch(*rd, dx0 + dw - rd->width(), dy0 + dh - rd->height(),
                            rd->width(), rd->height());
        if (outTopH) *outTopH = topH;   // the title-bar strip, for the caption
        return true;
    };

    // Horizontal 3-slice: art authored as "<name>_left.ddj" is the LEFT CAP of a
    // left/mid/right set (com_bar01_left/_mid/_right). The caps are only a few
    // pixels wide, so stretching the cap across a 400px bar smears one column of
    // pixels - the client draws caps at native width and repeats the middle.
    auto drawThreeSliceH = [&](const std::wstring& leftPath,
                               int dx0, int dy0, int dw, int dh) -> bool {
        const size_t us = leftPath.rfind(L'_');
        if (us == std::wstring::npos) return false;
        const std::wstring pre = leftPath.substr(0, us + 1);
        Image* l = getAtlas(pre + L"left.ddj");
        Image* m = getAtlas(pre + L"mid.ddj");
        Image* r = getAtlas(pre + L"right.ddj");
        if (!m && !r) return false;           // not a set - draw it normally
        const int lw = l ? (std::min)(l->width(), dw) : 0;
        const int rw = r ? (std::min)(r->width(), dw - lw) : 0;
        if (m) {
            const int midW = (std::max)(0, dw - lw - rw);
            for (int tx = 0; tx < midW; tx += m->width())
                blitStretch(*m, dx0 + lw + tx, dy0,
                            (std::min)(m->width(), midW - tx), dh);
        }
        if (l) blitStretch(*l, dx0, dy0, lw, dh);
        if (r) blitStretch(*r, dx0 + dw - rw, dy0, rw, dh);
        return true;
    };

    // Painter's algorithm bookkeeping: remember, in draw order, the rectangle of
    // every widget that actually painted a texture. The label pass below uses it
    // to cull captions that a LATER panel covers - without this, the text of a
    // page that is visually behind another page floats on top and the window
    // reads as "overlapping / broken".
    struct PaintedRect { size_t entryIndex; i32 x, y, w, h; };
    std::vector<PaintedRect> painted;
    painted.reserve(file.entries.size());
    /// entry -> height of its nine-slice top strip (the title bar), so the
    /// window caption lands on the bar instead of the middle of the window.
    std::unordered_map<size_t, int> captionStrip;

    int drawn = 0;
    for (size_t entryIndex = 0; entryIndex < file.entries.size(); ++entryIndex) {
        const Nif2dtEntry& e = file.entries[entryIndex];
        if (!e.saneRect() || !e.initiallyVisible) continue;   // only the visible, sane set
        // Widgets carry EITHER an Image (a UV sub-rect of an atlas) OR a
        // Background (a whole texture filling the widget). Many real UI panels
        // use Background, so we must honour both or the panel renders empty.
        const bool useImage = !e.image.empty();
        std::wstring atlasPath = useImage ? e.image : e.background;

        // The ACTIVE tab of a strip is drawn with its "_on" texture; the file
        // authors every tab "_off" and the engine swaps it. Without this the
        // selected page's tab looks identical to the inactive ones.
        if (e.type == 16 && file.activeTabEntry == entryIndex) {
            const std::wstring lp = ToLowerW(atlasPath);
            const size_t off = lp.rfind(L"_off.");
            if (off != std::wstring::npos) {
                std::wstring on = atlasPath.substr(0, off) + L"_on." + atlasPath.substr(off + 5);
                if (getAtlas(on)) atlasPath = on;
            }
        }
        const int fx0 = static_cast<int>((e.absX - ox) * sx);
        const int fy0 = static_cast<int>((e.absY - oy) * sy);
        const int fw  = (std::max)(1, static_cast<int>(e.width  * sx));
        const int fh  = (std::max)(1, static_cast<int>(e.height * sy));

        // Controls the client skins from its OWN built-in art carry no texture
        // path in the .2dt at all - combo boxes, edit fields, text boxes and
        // scroll bars. Skipping them left real, positioned widgets invisible
        // (measured: 62 across the archive). Draw them with the game's standard
        // interface\ifcommon art so the window matches what the client shows.
        if (atlasPath.empty()) {
            const bool isField  = (e.type == 12 || e.type == 6 || e.type == 7);
            const bool isScroll = (e.type == 13);
            if (!isField && !isScroll) continue;
            fillRect(fx0, fy0, fw, fh, 8, 8, 8);                   // inset interior
            drawNineSlice(kStdBlackField, fx0, fy0, fw, fh);
            if (e.type == 12) {                                    // combo: [v] button
                if (Image* arrow = getAtlas(kStdDownArrow)) {
                    const int aw = (std::min)(arrow->width(),  fw);
                    const int ah = (std::min)(arrow->height(), fh);
                    blitStretch(*arrow, fx0 + fw - aw - 1, fy0 + (fh - ah) / 2, aw, ah);
                }
            } else if (isScroll) {
                if (Image* bar = getAtlas(kStdScrollBar)) blitStretch(*bar, fx0, fy0, fw, fh);
                if (Image* thumb = getAtlas(kStdScrollButton))
                    blitStretch(*thumb, fx0, fy0, fw, (std::min)(fh, thumb->height() * 2));
            }
            painted.push_back({ entryIndex, e.absX, e.absY, e.width, e.height });
            ++drawn;
            continue;
        }

        // Nine-slice frame set (window borders, title bars, inner panels).
        if (atlasPath.back() == L'_') {
            int topH = 0;
            // "blacksquare" sets ship only corners and side rails - the interior
            // is meant to be solid black (it is the inset well behind lists and
            // input fields). Without the fill the tiled panel underneath shows
            // through as a grey slab.
            if (ToLowerW(atlasPath).find(L"blacksquare") != std::wstring::npos)
                fillRect(fx0, fy0, fw, fh, 8, 8, 8);
            if (drawNineSlice(atlasPath, fx0, fy0, fw, fh, &topH)) {
                if (topH > 0) captionStrip[entryIndex] = topH;
                painted.push_back({ entryIndex, e.absX, e.absY, e.width, e.height });
                ++drawn;
                continue;
            }
        }

        // Left cap of a horizontal 3-slice bar set (column headers, list rows).
        {
            const std::wstring lp = ToLowerW(atlasPath);
            if (lp.size() > 9 && lp.compare(lp.size() - 9, 9, L"_left.ddj") == 0 &&
                drawThreeSliceH(atlasPath, fx0, fy0, fw, fh)) {
                painted.push_back({ entryIndex, e.absX, e.absY, e.width, e.height });
                ++drawn;
                continue;
            }
        }

        Image* atlas = getAtlas(atlasPath);
        if (!atlas) continue;
        const int tw = atlas->width(), th = atlas->height();
        if (tw <= 0 || th <= 0) continue;

        // CNIFNormaltile REPEATS its texture across the widget - the whole point
        // of the type. Stretching a small tile instead produced a flat grey slab
        // where the client shows a patterned background.
        const std::wstring lowerArt = ToLowerW(atlasPath);
        const bool tiled = (e.type == 2) ||
                           lowerArt.find(L"bg_tile") != std::wstring::npos;
        if (tiled) {
            for (int yy = 0; yy < fh; ++yy) {
                const int py = fy0 + yy; if (py < 0 || py >= ch) continue;
                const int syy = yy % th;
                for (int xx = 0; xx < fw; ++xx) {
                    const int px = fx0 + xx; if (px < 0 || px >= cw) continue;
                    const u8* s = atlas->data() + (static_cast<size_t>(syy) * tw + (xx % tw)) * 4;
                    const int a = s[3];
                    if (a <= 0) continue;
                    u8* dp = out.data() + (static_cast<size_t>(py) * cw + px) * 4;
                    if (a >= 255) { dp[0] = s[0]; dp[1] = s[1]; dp[2] = s[2]; dp[3] = 255; }
                    else {
                        dp[0] = static_cast<u8>((s[0] * a + dp[0] * (255 - a)) / 255);
                        dp[1] = static_cast<u8>((s[1] * a + dp[1] * (255 - a)) / 255);
                        dp[2] = static_cast<u8>((s[2] * a + dp[2] * (255 - a)) / 255);
                        dp[3] = 255;
                    }
                }
            }
            painted.push_back({ entryIndex, e.absX, e.absY, e.width, e.height });
            ++drawn;
            continue;
        }

        // UV corners -> source rect (Image only). Background = whole texture.
        // The UV corners describe the atlas sub-rect for EITHER slot - a
        // Background-only widget can still carve a third out of a shared sheet
        // (chat_window.ddj split into three panels), so honour them both.
        auto norm = [](float a, float dim) { return (a > 1.5f) ? a / dim : a; };
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        {
            u0 = norm(e.uvTopLeft.x, (float)tw); u1 = norm(e.uvBottomRight.x, (float)tw);
            v0 = norm(e.uvTopLeft.y, (float)th); v1 = norm(e.uvBottomRight.y, (float)th);
            if (!(u1 > u0) || !(v1 > v0)) { u0 = 0; v0 = 0; u1 = 1; v1 = 1; }
        }
        u0 = clamp01(u0); u1 = clamp01(u1); v0 = clamp01(v0); v1 = clamp01(v1);

        const int dx0 = static_cast<int>((e.absX - ox) * sx);
        const int dy0 = static_cast<int>((e.absY - oy) * sy);
        const int dw  = (std::max)(1, static_cast<int>(e.width  * sx));
        const int dh  = (std::max)(1, static_cast<int>(e.height * sy));

        for (int yy = 0; yy < dh; ++yy) {
            const int py = dy0 + yy; if (py < 0 || py >= ch) continue;
            const float fv = v0 + (v1 - v0) * ((yy + 0.5f) / dh);
            int syy = static_cast<int>(fv * th); if (syy < 0) syy = 0; if (syy >= th) syy = th - 1;
            for (int xx = 0; xx < dw; ++xx) {
                const int px = dx0 + xx; if (px < 0 || px >= cw) continue;
                const float fu = u0 + (u1 - u0) * ((xx + 0.5f) / dw);
                int sxx = static_cast<int>(fu * tw); if (sxx < 0) sxx = 0; if (sxx >= tw) sxx = tw - 1;
                const u8* s = atlas->data() + (static_cast<size_t>(syy) * tw + sxx) * 4;
                const int a = s[3];
                if (a <= 0) continue;
                u8* d = out.data() + (static_cast<size_t>(py) * cw + px) * 4;
                if (a >= 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
                else {
                    d[0] = static_cast<u8>((s[0] * a + d[0] * (255 - a)) / 255);
                    d[1] = static_cast<u8>((s[1] * a + d[1] * (255 - a)) / 255);
                    d[2] = static_cast<u8>((s[2] * a + d[2] * (255 - a)) / 255);
                    d[3] = 255;
                }
            }
        }
        painted.push_back({ entryIndex, e.absX, e.absY, e.width, e.height });
        ++drawn;
    }

    // --- label pass: draw the real textdata strings on text-bearing widgets --
    // (Button/Static/Edit/TextBox/CheckBox/TabButton). Done via a short-lived
    // GDI DC over the composed image so DrawTextW (Unicode) can render CJK etc.
    if (strings && drawn > 0) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = cw; bi.bmiHeader.biHeight = -ch;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC screen = ::GetDC(nullptr);
        HDC dc = ::CreateCompatibleDC(screen);
        HBITMAP bmp = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ::ReleaseDC(nullptr, screen);
        if (dc && bmp && bits) {
            HGDIOBJ oldbmp = ::SelectObject(dc, bmp);
            const size_t px = static_cast<size_t>(cw) * ch;
            const u8* src = out.data(); u8* db = static_cast<u8*>(bits);
            for (size_t i = 0; i < px; ++i) { db[i*4+0]=src[i*4+2]; db[i*4+1]=src[i*4+1]; db[i*4+2]=src[i*4+0]; db[i*4+3]=255; }
            // PAGE DISAMBIGUATION (label level only - never blanks the panel):
            // captions that belong to different tab pages / mode panels are
            // authored on the same rectangle. Group the captions by their
            // container (parentId), take each container's bounding box, then walk
            // the containers in file order and drop any whose box lands on top of
            // a container already kept. One page's captions survive per region,
            // so the window reads as ONE window instead of several stacked.
            // A .2dt stores every tab page / mode panel authored on the SAME
            // rectangle, and the file itself never says which one is active. Any
            // graph rule for that was measured to blank the window, so overlap is
            // resolved where the user actually sees it: an OCCUPANCY MAP. Each
            // caption claims the cells its text covers; a caption whose area is
            // already claimed by an earlier one is skipped. Two captions can then
            // never be drawn on top of each other.
            const int kCell = 4;
            const int occW = (cw + kCell - 1) / kCell;
            const int occH = (ch + kCell - 1) / kCell;
            std::vector<char> occupied(static_cast<size_t>(occW) * occH, 0);
            auto claimIfFree = [&](const RECT& r) -> bool {
                const int cx0 = ui::ClampInt(static_cast<int>(r.left)   / kCell, 0, occW - 1);
                const int cx1 = ui::ClampInt(static_cast<int>(r.right)  / kCell, 0, occW - 1);
                const int cy0 = ui::ClampInt(static_cast<int>(r.top)    / kCell, 0, occH - 1);
                const int cy1 = ui::ClampInt(static_cast<int>(r.bottom) / kCell, 0, occH - 1);
                int total = 0, taken = 0;
                for (int cy = cy0; cy <= cy1; ++cy)
                    for (int cx = cx0; cx <= cx1; ++cx) {
                        ++total;
                        if (occupied[static_cast<size_t>(cy) * occW + cx]) ++taken;
                    }
                if (total > 0 && taken * 100 >= total * 12) return false;   // already used
                for (int cy = cy0; cy <= cy1; ++cy)
                    for (int cx = cx0; cx <= cx1; ++cx)
                        occupied[static_cast<size_t>(cy) * occW + cx] = 1;
                return true;
            };

            // A caption is culled when a LATER-painted panel covers it: that panel
            // physically painted over this widget, so in the real UI the caption is
            // behind it (a different tab page / mode panel) and must not float on top.
            auto coveredByLaterPanel = [&](size_t entryIndex, const Nif2dtEntry& e) -> bool {
                const long long myArea = static_cast<long long>(e.width) * e.height;
                bool seenSelf = false;
                for (const PaintedRect& p : painted) {
                    if (!seenSelf) { if (p.entryIndex == entryIndex) seenSelf = true; continue; }
                    const i32 ix = (std::min)(p.x + p.w, e.absX + e.width)  - (std::max)(p.x, e.absX);
                    const i32 iy = (std::min)(p.y + p.h, e.absY + e.height) - (std::max)(p.y, e.absY);
                    if (ix <= 0 || iy <= 0) continue;
                    const long long inter = static_cast<long long>(ix) * iy;
                    const long long pArea = static_cast<long long>(p.w) * p.h;
                    // Only a panel BIGGER than the label's own widget can bury it;
                    // a same-size sibling (the widget's own art) must not.
                    if (pArea > myArea * 2 && inter * 100 >= myArea * 80) return true;
                }
                return false;
            };
            for (size_t entryIndex = 0; entryIndex < file.entries.size(); ++entryIndex) {
                const Nif2dtEntry& e = file.entries[entryIndex];
                if (!e.saneRect() || !e.initiallyVisible) continue;
                const u32 ty = e.type;
                // A frame that painted a title bar carries the WINDOW CAPTION
                // ("Option", "Guild"...) in its Text field - draw it on the bar.
                auto strip = captionStrip.find(entryIndex);
                if (strip != captionStrip.end()) {
                    const std::wstring cap = ResolveWidgetLabel(e, strings);
                    if (!cap.empty()) {
                        RECT tb{ static_cast<LONG>((e.absX - ox) * sx),
                                 static_cast<LONG>((e.absY - oy) * sy),
                                 static_cast<LONG>((e.absX + e.width - ox) * sx),
                                 static_cast<LONG>((e.absY - oy) * sy + strip->second) };
                        RECT sh2 = tb; ::OffsetRect(&sh2, 1, 1);
                        Theme::DrawTextThemed(dc, sh2, cap, FontRole::Ui, 0x120A02,
                            DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
                        Theme::DrawTextThemed(dc, tb, cap, FontRole::Ui, 0xF6E9BE,
                            DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
                    }
                    continue;
                }
                // Data-driven: draw the caption of ANY widget that actually
                // resolves one, excluding pure containers (frames, tiles, page
                // hosts) whose Text is structural. A fixed type whitelist kept
                // silently dropping real labels - e.g. CNIFBarWnd (15) column
                // headers like "Item".
                if (ty==0 || ty==1 || ty==2 || ty==3 || ty==14 || ty==18) continue;
                const std::wstring label = ResolveWidgetLabel(e, strings);
                if (label.empty()) continue;
                if (coveredByLaterPanel(entryIndex, e)) continue;
                RECT r{ static_cast<LONG>((e.absX - ox) * sx), static_cast<LONG>((e.absY - oy) * sy),
                        static_cast<LONG>((e.absX + e.width - ox) * sx),
                        static_cast<LONG>((e.absY + e.height - oy) * sy) };
                if (r.right - r.left < 8 || r.bottom - r.top < 6) continue;
                // Claim only the pixels the TEXT itself will cover (centred in the
                // widget), so wide buttons don't reserve their whole rectangle.
                {
                    const SIZE ts = Theme::MeasureText(dc, label, FontRole::Ui);
                    const LONG cxm = (r.left + r.right) / 2, cym = (r.top + r.bottom) / 2;
                    LONG halfW = (std::min<LONG>)(ts.cx, r.right - r.left) / 2 + 4;
                    LONG halfH = (std::min<LONG>)(ts.cy, r.bottom - r.top) / 2 + 2;
                    RECT tr{ cxm - halfW, cym - halfH, cxm + halfW, cym + halfH };
                    if (!claimIfFree(tr)) continue;      // an earlier caption owns this spot
                }
                // NewInterfaceStyle drives alignment: 0x200 = right, 0x100 =
                // centre, neither = LEFT (the common case - forcing centre put
                // hundreds of row labels in the middle of their row).
                // 0x10000 (LineCenter) = single line, vertically centred;
                // without it a text block wraps from the top.
                UINT flags = DT_NOPREFIX;
                if      (e.style & 0x200) flags |= DT_RIGHT;
                else if (e.style & 0x100) flags |= DT_CENTER;
                else                      flags |= DT_LEFT;
                if (e.style & 0x10000) flags |= DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
                else                   flags |= DT_WORDBREAK | DT_TOP;

                // A widget's own Color is the caption colour. Measured layout is
                // 0xAARRGGBB (stallnetwork's column captions are 0xFFEFDAA4, the
                // game's warm cream). Opaque white and 0 both mean "default".
                // NOTE: alpha 0 is deliberately NOT treated as "hidden" - in
                // these files 0 reads as "unset", and hiding on it would blank
                // widgets that the client clearly draws.
                u32 ink = 0xF6E9BE;
                const u32 rgb24 = e.color & 0x00FFFFFFu;
                if (rgb24 != 0 && rgb24 != 0x00FFFFFFu) ink = rgb24;

                RECT sh = r; ::OffsetRect(&sh, 1, 1);
                Theme::DrawTextThemed(dc, sh, label, FontRole::Ui, 0x120A02, flags);
                Theme::DrawTextThemed(dc, r, label, FontRole::Ui, ink, flags);
            }
            ::GdiFlush();
            u8* o = out.data(); const u8* b2 = static_cast<const u8*>(bits);
            for (size_t i = 0; i < px; ++i) { o[i*4+0]=b2[i*4+2]; o[i*4+1]=b2[i*4+1]; o[i*4+2]=b2[i*4+0]; o[i*4+3]=255; }
            ::SelectObject(dc, oldbmp);
        }
        if (bmp) ::DeleteObject(bmp);
        if (dc) ::DeleteDC(dc);
    }
    return drawn > 0;
}

/// Read a referenced asset's raw bytes via the loader, trying the path as-is
/// then relative to `baseFolder` - the same two-step every asset reference in
/// an effect needs, whether it ends up a texture or a mesh.
/// How big to render an effect's frames.
///
/// The frames are pre-rendered and then shown through the image view, which
/// never upscales on fit - so whatever size is chosen here IS the size the user
/// sees, however large the window is. A fixed 512 px meant a small picture in
/// the middle of a big pane, which is exactly what "the preview viewport is too
/// small" describes.
///
/// Scaled to the actual screen, capped because 32 frames of it are held in
/// memory at once: at 1024 px that is already ~130 MB of RGBA.
int EffectPreviewSize() {
    const int screenH = ::GetSystemMetrics(SM_CYSCREEN);
    int size = screenH > 0 ? static_cast<int>(screenH * 0.75) : 768;
    if (size < 512)  size = 512;
    if (size > 1024) size = 1024;
    return size;
}

bool LoadAssetBytes(const std::wstring& path, const AssetLoader& loader,
                    const std::wstring& baseFolder, std::vector<u8>& out) {
    if (!loader || path.empty()) return false;
    const std::wstring direct = NormalizeInnerPath(path);
    if (!direct.empty() && loader(direct, out) && !out.empty()) return true;
    if (!baseFolder.empty()) {
        const std::wstring joined = NormalizeInnerPath(PathJoin(baseFolder, path));
        if (!joined.empty() && loader(joined, out) && !out.empty()) return true;
    }
    return false;
}

/// Decode a referenced asset texture (.ddj, or a raw .dds) via the loader,
/// trying the path as-is then relative to `baseFolder`.
bool LoadTextureAsset(const std::wstring& path, const AssetLoader& loader,
                      const std::wstring& baseFolder, Image& out) {
    if (!loader || path.empty()) return false;
    std::vector<u8> bytes;
    std::wstring direct = NormalizeInnerPath(path);
    bool got = (!direct.empty() && loader(direct, bytes));
    if (!got && !baseFolder.empty()) {
        std::wstring joined = NormalizeInnerPath(PathJoin(baseFolder, path));
        got = (!joined.empty() && loader(joined, bytes));
    }
    if (!got || bytes.empty()) return false;
    if (DecodeDdj(bytes.data(), bytes.size(), 0, 0, out, nullptr, nullptr)) return out.valid();
    if (DecodeDds(bytes.data(), bytes.size(), out, nullptr, nullptr)) return out.valid();
    ImageIO::Scope wic; ImageMeta m;
    return ImageIO::DecodeAny(bytes.data(), bytes.size(), out, &m).ok() && out.valid();
}

/// Build a contact-sheet image from a list of textures. Used by formats that
/// genuinely ARE a set of images; an .efp is not one of them and no longer
/// goes through here.
int RenderTextureSheet(const std::vector<std::wstring>& paths, const AssetLoader& loader,
                       const std::wstring& baseFolder, int cap, Image& out) {
    std::vector<Image> tiles;
    for (const std::wstring& p : paths) {
        if (static_cast<int>(tiles.size()) >= cap) break;
        Image t;
        if (LoadTextureAsset(p, loader, baseFolder, t) && t.valid())
            tiles.push_back(std::move(t));
    }
    if (tiles.empty()) return 0;

    const int n    = static_cast<int>(tiles.size());
    int cols = 1; while (cols * cols < n) ++cols;      // ceil(sqrt)
    const int rows = (n + cols - 1) / cols;
    constexpr int cell = 256, pad = 8;
    const int cw = cols * cell + (cols + 1) * pad;
    const int ch = rows * cell + (rows + 1) * pad;
    if (!out.resize(cw, ch)) return 0;
    for (size_t i = 0, tot = static_cast<size_t>(cw) * ch; i < tot; ++i) {
        u8* d = out.data() + i * 4; d[0] = 18; d[1] = 14; d[2] = 10; d[3] = 255;
    }

    for (int i = 0; i < n; ++i) {
        const Image& src = tiles[i];
        int sw = src.width(), sh = src.height();
        double sc = (std::min)(static_cast<double>(cell) / (std::max)(1, sw),
                               static_cast<double>(cell) / (std::max)(1, sh));
        int dw = (std::max)(1, static_cast<int>(sw * sc));
        int dh = (std::max)(1, static_cast<int>(sh * sc));
        Image scaled = src.scaledSmooth(dw, dh);
        if (!scaled.valid()) continue;
        const int cx = (i % cols) * cell + ((i % cols) + 1) * pad + (cell - dw) / 2;
        const int cy = (i / cols) * cell + ((i / cols) + 1) * pad + (cell - dh) / 2;
        for (int y = 0; y < dh; ++y) {
            const int py = cy + y; if (py < 0 || py >= ch) continue;
            for (int x = 0; x < dw; ++x) {
                const int px = cx + x; if (px < 0 || px >= cw) continue;
                const u8* s = scaled.data() + (static_cast<size_t>(y) * dw + x) * 4;
                const int a = s[3];
                u8* d = out.data() + (static_cast<size_t>(py) * cw + px) * 4;
                // Additive-ish: particle art glows, so max() RGB over the dark cell
                // keeps faint sprites visible while true alpha still blends edges.
                const int cov = (a > 0) ? a : 255;
                d[0] = static_cast<u8>((s[0] * cov + d[0] * (255 - cov)) / 255);
                d[1] = static_cast<u8>((s[1] * cov + d[1] * (255 - cov)) / 255);
                d[2] = static_cast<u8>((s[2] * cov + d[2] * (255 - cov)) / 255);
                d[3] = 255;
            }
        }
    }
    return n;
}

/// Composite an EFP's particle textures the way the engine does: bright-on-black
/// sprites blended ADDITIVELY over each other (glow), so the preview reads as one
/// real effect burst - not a catalogue grid of separate .ddj tiles. Returns the
/// count blended.
int RenderEffectGlow(const std::vector<std::wstring>& paths, const AssetLoader& loader,
                     const std::wstring& baseFolder, int cap, Image& out) {
    std::vector<Image> tiles;
    for (const std::wstring& p : paths) {
        if (static_cast<int>(tiles.size()) >= cap) break;
        Image t;
        if (LoadTextureAsset(p, loader, baseFolder, t) && t.valid())
            tiles.push_back(std::move(t));
    }
    if (tiles.empty()) return 0;

    const int S = 640;
    if (!out.resize(S, S)) return 0;
    for (size_t i = 0, tot = static_cast<size_t>(S) * S; i < tot; ++i) {
        u8* d = out.data() + i * 4; d[0] = 8; d[1] = 6; d[2] = 12; d[3] = 255;  // near-black
    }

    const int n = static_cast<int>(tiles.size());
    for (int i = 0; i < n; ++i) {
        const Image& src = tiles[i];
        const int sw = src.width(), sh = src.height();
        const double sc = (std::min)(static_cast<double>(S * 0.82) / (std::max)(1, sw),
                                     static_cast<double>(S * 0.82) / (std::max)(1, sh));
        const int dw = (std::max)(1, static_cast<int>(sw * sc));
        const int dh = (std::max)(1, static_cast<int>(sh * sc));
        Image scaled = src.scaledSmooth(dw, dh);
        if (!scaled.valid()) continue;
        const int ox = (S - dw) / 2, oy = (S - dh) / 2;   // emitters overlap at centre
        for (int y = 0; y < dh; ++y) {
            const int py = oy + y; if (py < 0 || py >= S) continue;
            for (int x = 0; x < dw; ++x) {
                const int px = ox + x; if (px < 0 || px >= S) continue;
                const u8* s = scaled.data() + (static_cast<size_t>(y) * dw + x) * 4;
                const int a = s[3]; if (a <= 0) continue;
                u8* d = out.data() + (static_cast<size_t>(py) * S + px) * 4;
                const int r = d[0] + s[0] * a / 255;
                const int g = d[1] + s[1] * a / 255;
                const int b = d[2] + s[2] * a / 255;
                d[0] = static_cast<u8>(r > 255 ? 255 : r);
                d[1] = static_cast<u8>(g > 255 ? 255 : g);
                d[2] = static_cast<u8>(b > 255 ? 255 : b);
                d[3] = 255;
            }
        }
    }
    return n;
}

/// Render an EFP as an ANIMATED, looping particle burst: its textures radiate
/// outward from the centre, pulsing (fade in/out) and growing, blended
/// additively. Produces `frameCount` frames the ImageView plays on a timer.
// ---------------------------------------------------------------------------
// Terrain: turn a JMXVMAPM height field into a real 3D mesh so a .m previews
// exactly like a .bsr (orbit, shade, reset) instead of a wall of text.
// 6x6 blocks x 17x17 vertices share edges -> a 97x97 global grid; 20 units/tile.
// ---------------------------------------------------------------------------
// Build a terrain BmsFile from a square height grid (used by both .m and .nvm).
bool BuildHeightGridBms(const std::vector<float>& H, int GRID, float CELL, BmsFile& out) {
    if (GRID < 2 || H.size() < static_cast<size_t>(GRID) * GRID) return false;
    out = BmsFile();
    out.vertices.resize(static_cast<size_t>(GRID) * GRID);
    for (int gz = 0; gz < GRID; ++gz)
        for (int gx = 0; gx < GRID; ++gx) {
            BmsVertex& v = out.vertices[static_cast<size_t>(gz) * GRID + gx];
            v.position = Vec3(gx * CELL, H[static_cast<size_t>(gz) * GRID + gx], gz * CELL);
            const int xl = (gx > 0) ? gx - 1 : gx, xr = (gx < GRID - 1) ? gx + 1 : gx;
            const int zl = (gz > 0) ? gz - 1 : gz, zr = (gz < GRID - 1) ? gz + 1 : gz;
            const float dhx = H[(size_t)gz * GRID + xr] - H[(size_t)gz * GRID + xl];
            const float dhz = H[(size_t)zr * GRID + gx] - H[(size_t)zl * GRID + gx];
            v.normal = Vec3(-dhx, 2.0f * CELL, -dhz).normalized();
            v.uv0 = Vec2(static_cast<float>(gx) / (GRID - 1), static_cast<float>(gz) / (GRID - 1));
        }
    out.faces.reserve(static_cast<size_t>(GRID - 1) * (GRID - 1) * 4);
    for (int gz = 0; gz < GRID - 1; ++gz)
        for (int gx = 0; gx < GRID - 1; ++gx) {
            const u16 a = static_cast<u16>(gz * GRID + gx);
            const u16 b = static_cast<u16>(gz * GRID + gx + 1);
            const u16 c = static_cast<u16>((gz + 1) * GRID + gx + 1);
            const u16 d = static_cast<u16>((gz + 1) * GRID + gx);
            out.faces.push_back(BmsFace{ a, d, b });
            out.faces.push_back(BmsFace{ b, d, c });
            out.faces.push_back(BmsFace{ a, b, d });
            out.faces.push_back(BmsFace{ b, c, d });
        }
    out.headerVertexCount = static_cast<u32>(out.vertices.size());
    out.headerFaceCount   = static_cast<u32>(out.faces.size());
    return !out.faces.empty();
}

bool BuildTerrainBms(const MapMeshFile& m, BmsFile& out) {
    if (m.blocks.size() < static_cast<size_t>(kMapBlocksPerSide * kMapBlocksPerSide))
        return false;
    constexpr int TPB  = kMapTilesPerSide;                 // 16 tiles per block side
    constexpr int GRID = kMapBlocksPerSide * TPB + 1;      // 97 verts per side
    constexpr float CELL = 20.0f;                          // world units per tile

    std::vector<float> H(static_cast<size_t>(GRID) * GRID, 0.0f);
    // Empty/ocean regions store sentinel garbage heights (e.g. -1.7e38); clamp
    // anything absurd to 0 so the mesh bounds stay sane and framable.
    auto sane = [](float h) -> float {
        if (!(h == h) || h > 1.0e6f || h < -1.0e6f) return 0.0f;  // NaN / sentinel
        return h;
    };
    for (int bz = 0; bz < kMapBlocksPerSide; ++bz)
        for (int bx = 0; bx < kMapBlocksPerSide; ++bx) {
            const MapMeshBlock& b = m.blocks[static_cast<size_t>(bz) * kMapBlocksPerSide + bx];
            for (int z = 0; z < kMapVerticesPerSide; ++z)
                for (int x = 0; x < kMapVerticesPerSide; ++x) {
                    const int gx = bx * TPB + x, gz = bz * TPB + z;
                    if (gx < GRID && gz < GRID)
                        H[static_cast<size_t>(gz) * GRID + gx] =
                            sane(b.heights[static_cast<size_t>(z) * kMapVerticesPerSide + x]);
                }
        }

    if (!BuildHeightGridBms(H, GRID, CELL, out)) return false;
    out.name = T(L"Arazi (JMXVMAPM)");
    return true;
}

// SroPk2Studio: a .m painted the way the client paints it, for the interactive viewer. Every
// vertex names a ground texture of tile2d.ifo (low 10 bits). The region's most used texture covers
// the whole ground; every other one is blended over it by vertex alpha (1 at the vertices that use
// it, 0 at the others), so ground types fade into each other instead of meeting in squares. Blocks
// with water or ice get a translucent sheet at their water height.
bool BuildTerrainScene(const MapMeshFile& m, const AssetLoader& loader, Scene& scene) {
    if (m.blocks.size() < static_cast<size_t>(kMapBlocksPerSide * kMapBlocksPerSide))
        return false;
    constexpr int TPB   = kMapTilesPerSide;
    constexpr int GRID  = kMapBlocksPerSide * TPB + 1;
    constexpr int TILES = GRID - 1;
    constexpr float CELL = 20.0f;              // world units per tile
    constexpr float kTilesPerRepeat = 4.0f;    // one ground texture spans 4 x 4 tiles
    constexpr u8 kShade = 200;                 // tones the viewer's bright studio light down
    auto sane = [](float h) -> float { return (!(h == h) || h > 1.0e6f || h < -1.0e6f) ? 0.0f : h; };
    auto at = [](int x, int z) { return static_cast<size_t>(z) * GRID + static_cast<size_t>(x); };

    std::vector<float> H(static_cast<size_t>(GRID) * GRID, 0.0f);
    std::vector<u16>   TX(static_cast<size_t>(GRID) * GRID, 0);
    for (int bz = 0; bz < kMapBlocksPerSide; ++bz)
        for (int bx = 0; bx < kMapBlocksPerSide; ++bx) {
            const MapMeshBlock& b = m.blocks[static_cast<size_t>(bz) * kMapBlocksPerSide + bx];
            for (int z = 0; z < kMapVerticesPerSide; ++z)
                for (int x = 0; x < kMapVerticesPerSide; ++x) {
                    const size_t i = static_cast<size_t>(z) * kMapVerticesPerSide + x;
                    H[at(bx * TPB + x, bz * TPB + z)]  = sane(b.heights[i]);
                    TX[at(bx * TPB + x, bz * TPB + z)] = static_cast<u16>(b.textures[i] & 0x3FF);
                }
        }

    // tile2d.ifo: texture index -> file under tile2d\.
    std::map<u16, std::wstring> files;
    {
        std::vector<u8> ifo;
        IfoFile f;
        if (loader && loader(L"tile2d.ifo", ifo) && !ifo.empty() &&
            ParseIfo(ifo.data(), ifo.size(), L"tile2d.ifo", f).ok())
            for (const IfoRow& row : f.rows)
                if (row.fields.size() >= 4 && !row.fields[0].empty() && iswdigit(row.fields[0][0]))
                    files[static_cast<u16>(std::wcstoul(row.fields[0].c_str(), nullptr, 10))] = row.fields[3];
    }
    std::map<u16, int> texOf;
    auto texture = [&](u16 id) -> int {
        const auto known = texOf.find(id);
        if (known != texOf.end()) return known->second;
        int idx = -1;
        const auto f = files.find(id);
        Image img;
        if (f != files.end() && LoadTextureAsset(L"tile2d\\" + f->second, loader, L"", img)) {
            idx = static_cast<int>(scene.textures.size());
            scene.textures.push_back(std::move(img));
        }
        texOf.emplace(id, idx);
        return idx;
    };

    auto normal = [&](int x, int z) {
        const int xl = std::max(x - 1, 0), xr = std::min(x + 1, GRID - 1);
        const int zl = std::max(z - 1, 0), zr = std::min(z + 1, GRID - 1);
        return Vec3(-(H[at(xr, z)] - H[at(xl, z)]), 2.0f * CELL, -(H[at(x, zr)] - H[at(x, zl)])).normalized();
    };
    static constexpr int kCorner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    auto emit = [&](RasterMesh& rm, int gx, int gz, const u8 (&alpha)[4]) {
        const u32 first = static_cast<u32>(rm.vertices.size());
        for (int c = 0; c < 4; ++c) {
            const int x = gx + kCorner[c][0], z = gz + kCorner[c][1];
            RasterVertex v;
            v.position = Vec3(x * CELL, H[at(x, z)], z * CELL);
            v.normal   = normal(x, z);
            v.uv       = Vec2(static_cast<float>(x) / kTilesPerRepeat, static_cast<float>(z) / kTilesPerRepeat);
            v.color    = Rgba(kShade, kShade, kShade, alpha[c]);
            rm.vertices.push_back(v);
        }
        for (u32 k : {0u, 3u, 1u, 1u, 3u, 2u}) rm.indices.push_back(first + k);
    };

    u16 ground = 0;
    {
        std::map<u16, size_t> uses;
        for (u16 t : TX) ++uses[t];
        size_t best = 0;
        for (const auto& [id, n] : uses)
            if (n > best) { best = n; ground = id; }
    }
    std::map<u16, RasterMesh> base, over;
    for (int gz = 0; gz < TILES; ++gz)
        for (int gx = 0; gx < TILES; ++gx) {
            u16 t[4];
            for (int c = 0; c < 4; ++c) t[c] = TX[at(gx + kCorner[c][0], gz + kCorner[c][1])];
            static constexpr u8 kFull[4] = {255, 255, 255, 255};
            emit(base[ground], gx, gz, kFull);
            for (int i = 0; i < 4; ++i) {
                if (t[i] == ground || std::find(t, t + i, t[i]) != t + i) continue;
                u8 a[4];
                for (int c = 0; c < 4; ++c) a[c] = t[c] == t[i] ? 255 : 0;
                emit(over[t[i]], gx, gz, a);
            }
        }
    auto add = [&](std::map<u16, RasterMesh>& layers, bool layer) {
        for (auto& [id, rm] : layers) {
            rm.textureIndex = texture(id);
            if (rm.textureIndex < 0)           // a ground texture this client lacks: neutral soil
                for (RasterVertex& v : rm.vertices) { v.color.r = 120; v.color.g = 128; v.color.b = 96; }
            rm.vertexColors     = true;
            rm.vertexAlphaLayer = layer;
            rm.blendAlpha       = layer;
            rm.transform        = Mat4::Identity();
            scene.meshes.push_back(std::move(rm));
        }
    };
    add(base, false);
    add(over, true);

    // Water and ice: one translucent sheet per block, at its water height.
    RasterMesh water;
    for (int bz = 0; bz < kMapBlocksPerSide; ++bz)
        for (int bx = 0; bx < kMapBlocksPerSide; ++bx) {
            const MapMeshBlock& b = m.blocks[static_cast<size_t>(bz) * kMapBlocksPerSide + bx];
            if (b.waterType < 0 || !(b.waterHeight == b.waterHeight) || b.waterHeight < b.heightMin) continue;
            const Rgba c = b.waterType == 1 ? Rgba(190, 210, 225, 185) : Rgba(34, 84, 112, 165);
            const u32 first = static_cast<u32>(water.vertices.size());
            for (int k = 0; k < 4; ++k) {
                RasterVertex v;
                v.position = Vec3((bx + kCorner[k][0]) * TPB * CELL, b.waterHeight, (bz + kCorner[k][1]) * TPB * CELL);
                v.normal   = Vec3(0.0f, 1.0f, 0.0f);
                v.color    = c;
                water.vertices.push_back(v);
            }
            for (u32 k : {0u, 3u, 1u, 1u, 3u, 2u}) water.indices.push_back(first + k);
        }
    if (!water.vertices.empty()) {
        water.vertexColors = water.vertexAlphaLayer = water.blendAlpha = true;
        water.transform = Mat4::Identity();
        scene.meshes.push_back(std::move(water));
    }
    return !scene.meshes.empty();
}

// ---------------------------------------------------------------------------
// Real multi-model composition: place N .bsr instances (each transformed) into
// one ComposedModel so a .cpd compound or a .o map region renders as the ACTUAL
// referenced geometry - not a text list. Sources are composed once and cached;
// each instance appends its transformed copy. A face cap keeps a huge region
// (hundreds of buildings) responsive in the software rasteriser.
// ---------------------------------------------------------------------------
/// Try `path` as given, then relative to `baseFolder`, then baseFolder + the
/// bare file name (private-server repacks carry stale folders in .bsr refs).
bool LoadAssetLoose(const AssetLoader& loader, const std::wstring& path,
                    const std::wstring& baseFolder,
                    std::vector<u8>& out, std::wstring& resolved) {
    if (!loader || path.empty()) return false;
    const std::wstring direct = NormalizeInnerPath(path);
    out.clear();
    if (!direct.empty() && loader(direct, out) && !out.empty()) { resolved = direct; return true; }
    if (!baseFolder.empty()) {
        const std::wstring joined = NormalizeInnerPath(PathJoin(baseFolder, path));
        out.clear();
        if (joined != direct && loader(joined, out) && !out.empty()) { resolved = joined; return true; }
        const std::wstring same = NormalizeInnerPath(PathJoin(baseFolder, PathFileName(path)));
        out.clear();
        if (same != joined && loader(same, out) && !out.empty()) { resolved = same; return true; }
    }
    return false;
}

struct PlacedInstance {
    std::wstring path;
    Vec3  pos;
    float yaw = 0.0f;              ///< map objects only carry a yaw
    Vec3  scale{ 1, 1, 1 };
    /// Dungeon decorations carry a full 3-axis rotation AND sit inside a block
    /// that is itself placed, so the caller bakes the whole chain into one
    /// matrix rather than trying to add up Euler angles (which does not compose).
    Mat4  matrix;
    bool  useMatrix = false;
};

bool ComposePlacedRegion(const std::vector<PlacedInstance>& insts,
                         const AssetLoader& loader, u64 maxFaces,
                         ComposedModel& out, int* placedOut, int* skippedOut) {
    if (!loader) return false;
    std::unordered_map<std::wstring, ComposedModel> cache;
    std::unordered_map<std::wstring, int> texBase;
    u64 faceTotal = 0; int placed = 0, skipped = 0;

    for (const PlacedInstance& inst : insts) {
        if (inst.path.empty()) { ++skipped; continue; }
        if (faceTotal >= maxFaces) { ++skipped; continue; }

        auto it = cache.find(inst.path);
        if (it == cache.end()) {
            ComposedModel src;
            std::vector<u8> bytes;
            if (loader(inst.path, bytes) && !bytes.empty()) {
                BsrFile bsr;
                if (ParseBsr(bytes.data(), bytes.size(), bsr).ok())
                    ComposeBsrModel(bsr, inst.path, loader, 0, true, src);
            }
            it = cache.emplace(inst.path, std::move(src)).first;
        }
        const ComposedModel& src = it->second;
        if (src.parts.empty()) { ++skipped; continue; }

        int base;
        auto tb = texBase.find(inst.path);
        if (tb == texBase.end()) {
            base = static_cast<int>(out.textures.size());
            for (size_t i = 0; i < src.textures.size(); ++i) {
                out.textures.push_back(src.textures[i]);
                out.texturePaths.push_back(i < src.texturePaths.size() ? src.texturePaths[i] : L"");
            }
            texBase[inst.path] = base;
        } else {
            base = tb->second;
        }

        Mat4 M, Mn;
        if (inst.useMatrix) {
            M  = inst.matrix;
            Mn = inst.matrix;
            Mn.m[12] = Mn.m[13] = Mn.m[14] = 0.0f;      // rotation/scale only
        } else {
            const Mat4 R = Mat4::RotationY(inst.yaw);
            M  = Mat4::Scale(inst.scale) * R * Mat4::Translation(inst.pos);
            Mn = Mat4::Scale(inst.scale) * R;
        }
        for (const ComposedPart& sp : src.parts) {
            ComposedPart np = sp;                 // copies mesh (bounded by the face cap)
            for (BmsVertex& v : np.mesh.vertices) {
                const Vec4 wp = M.transformVec4(Vec4(v.position.x, v.position.y, v.position.z, 1.0f));
                v.position = Vec3(wp.x, wp.y, wp.z);
                v.normal   = Mn.transformVector(v.normal).normalized();
            }
            np.textureIndex = (sp.textureIndex >= 0) ? base + sp.textureIndex : -1;
            faceTotal += np.mesh.faces.size();
            out.parts.push_back(std::move(np));
        }
        ++placed;
        if (faceTotal >= maxFaces) break;
    }
    if (placedOut)  *placedOut  = placed;
    if (skippedOut) *skippedOut = skipped;
    return !out.parts.empty();
}

// object.ifo (text): "<index> <flag> \"<path>\"" -> map objectId to its .bsr path.
// Tried at a few well-known locations relative to the archive root.
std::unordered_map<u32, std::wstring> LoadObjectIfo(const AssetLoader& loader) {
    std::unordered_map<u32, std::wstring> map;
    if (!loader) return map;
    std::vector<u8> bytes;
    const wchar_t* candidates[] = { L"object.ifo", L"map\\object.ifo", L"navmesh\\object.ifo" };
    bool got = false;
    for (const wchar_t* c : candidates) { if (loader(c, bytes) && !bytes.empty()) { got = true; break; } }
    if (!got) return map;

    // Decode as text (usually plain ANSI/UTF-8) and parse line by line.
    const std::wstring text = MultiByteToWide(
        std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), CP_UTF8);
    size_t i = 0;
    auto nextLine = [&](std::wstring& line) -> bool {
        if (i >= text.size()) return false;
        size_t e = text.find(L'\n', i);
        if (e == std::wstring::npos) { line = text.substr(i); i = text.size(); }
        else { line = text.substr(i, e - i); i = e + 1; }
        return true;
    };
    std::wstring line;
    while (nextLine(line)) {
        // index (decimal) ... "path"
        size_t q0 = line.find(L'"');
        size_t q1 = (q0 == std::wstring::npos) ? std::wstring::npos : line.find(L'"', q0 + 1);
        if (q0 == std::wstring::npos || q1 == std::wstring::npos) continue;
        u32 idx = 0; bool haveIdx = false;
        for (wchar_t ch : line) {
            if (ch >= L'0' && ch <= L'9') { idx = idx * 10 + (ch - L'0'); haveIdx = true; }
            else if (haveIdx) break;
        }
        if (!haveIdx) continue;
        std::wstring path = line.substr(q0 + 1, q1 - q0 - 1);
        if (!path.empty()) map[idx] = path;
    }
    return map;
}

// ---------------------------------------------------------------------------
// The decode itself. Runs on the preview worker thread (and, for --shot, on
// the CLI thread) - it must not touch any window.
// ---------------------------------------------------------------------------
void DecodePayload(PreviewPayload& p, const AssetLoader& loader,
                   IArchive* archive = nullptr) {
    const u8* data = p.bytes.data();
    const size_t size = p.bytes.size();

    p.format = Identify(p.displayName, data, size);
    p.kind   = PreviewKindFor(p.format);

    std::wstring info;
    info += T(L"Yol:   ") + (p.innerPath.empty() ? p.displayName : p.innerPath) + L"\n";
    info += T(L"Tür:   ") + std::wstring(FormatDisplayName(p.format)) + L"\n";
    info += T(L"Boyut: ") + FormatBytesTr(size) + L"\n";

    auto fallbackToHex = [&](const std::wstring& whyTr) {
        // A JMXV-family file whose signature isn't JMXV* is almost always a
        // per-file encrypted asset (some servers protect premium COS/items).
        // The user does NOT want a raw hex wall for these: show a clean panel
        // that explains it and points at "RAM'den Çıkar".
        const bool expectedJmx =
            p.format == FormatId::Bsr || p.format == FormatId::Bms ||
            p.format == FormatId::Bmt || p.format == FormatId::Bsk ||
            p.format == FormatId::Ban || p.format == FormatId::Cpd ||
            p.format == FormatId::Efp || p.format == FormatId::Nvm ||
            p.format == FormatId::Dof || p.format == FormatId::Ddj ||
            p.format == FormatId::Nif2dt;
        const bool notJmxv = (size >= 4) &&
            !(data[0] == 'J' && data[1] == 'M' && data[2] == 'X' && data[3] == 'V');
        if (expectedJmx && notJmxv) {
            p.kind = PreviewKind::Summary;
            p.textIsSummary = true;
            p.text =
                std::wstring(T(L"🔒  Bu dosya sunucuya özel ayrıca ŞİFRELENMİŞ\n"
                L"    (premium / COS koruması — örn. AES).\n\n"
                L"Dosyanın kendisi kırılamaz; anahtarı oyun çalışırken üretiliyor.\n"
                L"Çözülmüş halini almak için:  araç çubuğu → \x201CRAM'den Çıkar\x201D\n"
                L"(oyunu açıp ilgili varlığı yükledikten sonra).\n\n")) +
                T(L"Dosya: ") + p.displayName + L"\n" +
                T(L"Boyut: ") + FormatBytesTr(size) + L"\n" +
                T(L"Tür  : ") + std::wstring(FormatDisplayName(p.format));
            info += T(L"Not:   İmza JMXV değil — sunucuya özel şifreli varlık.\n");
            return;
        }
        p.kind = PreviewKind::Hex;
        p.errorTr = whyTr;
        if (!whyTr.empty()) info += T(L"Not:   ") + whyTr + L"\n";
    };

    switch (p.kind) {
        case PreviewKind::Image: {
            std::wstring err;
            ImageMeta meta;
            bool ok = false;
            if (p.format == FormatId::Ddj) {
                ok = DecodeDdj(data, size, 0, 0, p.image, &meta, &err);
                if (ok) {
                    DdjFile ddj;
                    if (ParseDdj(data, size, ddj, nullptr)) {
                        info += T(L"DDJ:   ") + std::wstring(ddj.textureTypeNameTr()) + L"\n";
                        // The viewer indexes this as a FLAT GRID [face*mipCount+mip],
                        // so it must start at mip 0 and cover every face. Starting at
                        // mip 1 made mips[0] be level 1, i.e. every mipped texture was
                        // shown at HALF resolution; hard-coding face 0 meant the five
                        // other faces of a cube map were never decoded at all.
                        const int mipN  = (std::max)(1, (std::min)(ddj.dds.mipCount, 16));
                        const int faceN = (std::max)(1, ddj.dds.faceCount);
                        p.mips.reserve(static_cast<size_t>(mipN) * faceN);
                        for (int f = 0; f < faceN; ++f) {
                            for (int m = 0; m < mipN; ++m) {
                                Image s;
                                if (!DecodeDdj(data, size, m, f, s, nullptr, nullptr) || !s.valid())
                                    s = Image();          // keep the stride exact
                                p.mips.push_back(std::move(s));
                            }
                        }
                        p.mipCount  = mipN;
                        p.faceCount = faceN;
                    }
                }
            } else if (p.format == FormatId::Dds) {
                ok = DecodeDds(data, size, p.image, &meta, &err);
            } else {
                ImageIO::Scope wic;
                ok = ImageIO::DecodeAny(data, size, p.image, &meta).ok();
                if (!ok) err = T(L"Görüntü çözümlenemedi.");
            }
            if (ok && p.image.valid()) {
                p.meta = meta;
                info += T(L"Doku:  ") + meta.summaryTr() + L"\n";
                if (meta.hasAlpha)
                    info += T(L"Not:   Alfa kanalı SRO'da parlaklık taşır; varsayılan görünüm RGB.\n");
            } else {
                fallbackToHex(err.empty() ? T(L"Görüntü çözümlenemedi.") : err);
            }
            break;
        }

        case PreviewKind::Mesh: {
            if (p.format == FormatId::Bms) {
                const Result r = ParseBms(data, size, p.bms, true);
                if (r.ok()) { p.hasBms = true; info += p.bms.summaryTr() + L"\n"; }
                else fallbackToHex(r.text());
            } else {
                BsrFile bsr;
                Result r = ParseBsr(data, size, bsr);
                if (!r.ok() && RecoverCustomBsr(p.innerPath, bsr)) {
                    r = Result::Ok();
                    info += T(L"Özel varlık profili başarıyla yapılandırıldı.\n");
                }
                // Truncated only means the TAIL was unreadable. If the mesh list
                // survived, the model still draws - throwing the whole file at
                // the hex view over a damaged trailer loses a perfectly good
                // preview. Anything else really is unparseable.
                const bool usable = r.ok() ||
                                    (r.status == Status::Truncated && !bsr.meshes.empty());
                if (!usable) { fallbackToHex(r.text()); break; }
                if (!r.ok()) info += T(L"Uyarı: ") + r.text() + L"\n";
                info += bsr.summaryTr() + L"\n";
                const Result cr = ComposeBsrModel(bsr, p.innerPath, loader, 0, true, p.model);
                if (cr.ok() && !p.model.parts.empty()) {
                    p.hasModel = true;
                    // If this .bsr is a catalogued avatar, dress the WHOLE set:
                    // pull the hat / attach / wings / transform pieces of the same
                    // family and merge them so the preview shows the complete
                    // costume, not one loose piece. (Ported from SroEffectViewer's
                    // equipAvatarSet.) Best-effort and cached; a non-avatar or an
                    // archive without itemdata simply leaves the single piece.
                    if (archive) {
                        const AvatarCatalog& cat = GetAvatarCatalog(archive, loader);
                        const AvatarSetPlan plan = PlanAvatarSet(cat, p.innerPath);
                        if (!plan.empty()) {
                            const std::wstring baseFolder =
                                PathParent(NormalizeInnerPath(p.innerPath));
                            // Borrow a real character skeleton so wings seat on the
                            // spine exactly like the game (falls back to geometric).
                            BskFile charSkel;
                            const bool haveChar =
                                LoadAvatarCharSkeleton(p.innerPath, loader, charSkel);
                            // If the SELECTED piece is itself a skinned attach (the
                            // user clicked the wing), seat it too.
                            if (haveChar && bsr.hasSkeleton)
                                ApplyCharacterAttach(p.model, bsr.attachmentBone, charSkel);
                            int added = 0;
                            ComposeAvatarPieces(plan.pieces, baseFolder, loader, p.model, added,
                                                haveChar ? &charSkel : nullptr);
                            if (added > 0)
                                info += FormatW(T(L"Avatar seti: %s ailesinin %d ek parçası "
                                                L"giydirildi (toplam %d parça).\n"),
                                                plan.family.c_str(), added, plan.totalPieces);
                        }
                    }
                    // Animate the model from its OWN clips, following the
                    // reference viewer 1:1 (SroEffectViewer main.cpp pickIdle /
                    // idleByAniType, Model.h playAnim(idx, true)).
                    if (bsr.hasSkeleton && !bsr.animations.empty() && loader) {
                        const std::wstring folder = PathParent(NormalizeInnerPath(p.innerPath));
                        std::vector<u8> sb; std::wstring res;
                        BskFile sk;
                        if (LoadAssetLoose(loader, bsr.skeletonPath, folder, sb, res) &&
                            ParseBsk(sb.data(), sb.size(), sk).ok() && !sk.bones.empty()) {

                            // The full clip list, in file order: index i is
                            // bsr.animations[i] - the same identity the reference
                            // relies on when the user clicks a row.
                            p.animFolder = folder;
                            for (const BsrAnimationRef& ar : bsr.animations) {
                                p.animPaths.push_back(ar.path);
                                std::wstring lbl = PathFileName(ar.path);
                                if (lbl.size() > 4 &&
                                    ToLowerW(lbl).compare(lbl.size() - 4, 4, L".ban") == 0)
                                    lbl.resize(lbl.size() - 4);
                                p.animLabels.push_back(lbl);
                            }

                            // --- pickIdle, ported ---------------------------------
                            std::vector<int> cands;
                            auto push = [&](int i) {
                                if (i < 0 || (size_t)i >= bsr.animations.size()) return;
                                for (int e : cands) if (e == i) return;
                                cands.push_back(i);
                            };
                            // aniType 0 = STAND1. A group literally named "default"
                            // is authoritative (it is the body stance); a weapon
                            // group's type 0 is a WEAPON stance and must not be
                            // used for an unarmed preview, so it ranks below the
                            // name search - that is what stopped the character
                            // standing in a spear pose with no spear.
                            int typeDefault = -1, typeAny = -1;
                            for (const BsrAnimationGroup& g : bsr.animationGroups) {
                                const bool isDefault = (ToLowerW(g.name) == L"default");
                                for (const BsrAnimationEntry& e : g.entries) {
                                    if (e.type != 0 || e.fileIndex >= bsr.animations.size()) continue;
                                    if (isDefault) { typeDefault = (int)e.fileIndex; break; }
                                    if (typeAny < 0) typeAny = (int)e.fileIndex;
                                }
                                if (typeDefault >= 0) break;
                            }
                            push(typeDefault);

                            auto baseLower = [&](size_t i) {
                                return ToLowerW(PathFileName(bsr.animations[i].path));
                            };
                            static const wchar_t* kWeaponPrefixes[] = {
                                L"spear_", L"bow_", L"staff_", L"harp_", L"cart_",
                                L"skill_", L"sit", L"roll" };
                            auto weaponPrefixed = [&](const std::wstring& n) {
                                for (const wchar_t* px : kWeaponPrefixes)
                                    if (n.rfind(px, 0) == 0) return true;
                                return false;
                            };
                            for (const wchar_t* key : { L"standcity", L"standbattle", L"stand",
                                                        L"idle", L"wait" })
                                for (size_t i = 0; i < bsr.animations.size(); ++i)
                                    if (baseLower(i).find(key) != std::wstring::npos &&
                                        !weaponPrefixed(baseLower(i))) push((int)i);
                            push(typeAny);                       // creature without a default group
                            for (size_t i = 0; i < bsr.animations.size(); ++i)
                                if (baseLower(i).find(L"stand") != std::wstring::npos) push((int)i);
                            push(0);

                            // Walk the candidates until one actually parses, so a
                            // missing clip never demotes the model to a statue.
                            for (int idx : cands) {
                                std::vector<u8> ab; std::wstring ares;
                                BanFile ban;
                                if (!LoadAssetLoose(loader, bsr.animations[(size_t)idx].path,
                                                    folder, ab, ares)) continue;
                                if (!ParseBan(ab.data(), ab.size(), ban, true).ok() ||
                                    ban.bones.empty()) continue;
                                p.bsk = std::move(sk);  p.hasBsk = true;
                                p.ban = std::move(ban); p.hasBan = true;
                                p.animateModel = true;
                                p.animIndex = idx;
                                info += FormatW(T(L"Animasyon: %s   (%zu klip - ◀ Anim / Anim ▶ ile değiştirin)\n"),
                                                PathFileName(ares).c_str(), bsr.animations.size());
                                break;
                            }
                        }
                    }
                    for (const std::wstring& w : p.model.warningsTr) info += T(L"Uyarı: ") + w + L"\n";
                } else {
                    // No geometry. Say WHY, loudly, in the preview itself - a bare
                    // structure dump reads as "the program is broken" when the
                    // real story is that the client never shipped these meshes.
                    std::wstring why = T(L"\n⚠  3B görünüm yok — model çizilemedi.\n");
                    if (!cr.ok()) why += L"    " + cr.text() + L"\n";
                    // "no warnings AND the file names no meshes" = an empty .bsr,
                    // which is a real condition, not a missing file. (The old
                    // guard tested cr.ok() here, which is impossible in this
                    // else-branch and never fired.)
                    if (p.model.warningsTr.empty() && bsr.meshes.empty())
                        why += T(L"    Bu .bsr hiç ağ (mesh) referansı içermiyor.\n");
                    for (const std::wstring& w : p.model.warningsTr) {
                        why  += L"    • " + w + L"\n";
                        info += T(L"Uyarı: ") + w + L"\n";
                    }
                    why += T(L"\n    Bu dosyalar açık arşivde de, yanındaki diğer "
                           L"arşivlerde de\n    bulunamadı; istemci bunları hiç "
                           L"paketlememiş olabilir.\n");
                    p.kind = PreviewKind::Summary;
                    p.text = bsr.summaryTr() + why;
                    p.textIsSummary = true;
                }
            }
            break;
        }

        case PreviewKind::Skeleton: {
            const Result r = ParseBsk(data, size, p.bsk);
            if (r.ok()) { p.hasBsk = true; info += p.bsk.summaryTr() + L"\n"; }
            else fallbackToHex(r.text());
            break;
        }

        case PreviewKind::Animation: {
            const Result r = ParseBan(data, size, p.ban, true);
            if (r.ok()) {
                p.hasBan = true;
                p.kind   = PreviewKind::Summary;
                p.text   = p.ban.summaryTr();
                p.textIsSummary = true;
                info += p.ban.summaryTr() + L"\n";

                // A .ban is a pose track for a SKELETON, and the viewer can already
                // play one (MeshView::setAnimation) - it was simply never handed
                // the data, so every animation showed as text. Find the skeleton
                // that goes with it: the .bsk sitting next to the .ban, else the
                // one named by a .bsr in the same folder.
                if (loader && !p.ban.bones.empty()) {
                    const std::wstring folder = PathParent(NormalizeInnerPath(p.innerPath));
                    std::wstring stem = PathFileName(p.innerPath);
                    const size_t dot = stem.find_last_of(L'.');
                    if (dot != std::wstring::npos) stem.resize(dot);

                    auto tryBsk = [&](const std::wstring& path) -> bool {
                        std::vector<u8> b;
                        if (path.empty() || !loader(path, b) || b.empty()) return false;
                        BskFile sk;
                        if (!ParseBsk(b.data(), b.size(), sk).ok() || sk.bones.empty()) return false;
                        p.bsk = std::move(sk);
                        p.hasBsk = true;
                        return true;
                    };
                    // Silkroad keeps clips and skeletons in PARALLEL trees:
                    //   prim\ani\etc\x_stand01.ban  <->  prim\skel\etc\x.bsk
                    // so search both folders, and peel the clip suffix off the
                    // stem (x_stand01 -> x_stand -> x) since one skeleton serves
                    // every clip of a creature.
                    std::vector<std::wstring> folders{ folder };
                    {
                        std::wstring alt = folder;
                        const std::wstring low = ToLowerW(alt);
                        const size_t a = low.find(L"\\ani\\");
                        if (a != std::wstring::npos) alt.replace(a, 5, L"\\skel\\");
                        else if (low.compare(0, 4, L"ani\\") == 0) alt.replace(0, 4, L"skel\\");
                        if (alt != folder) folders.push_back(alt);
                    }

                    bool got = false;
                    for (std::wstring s = stem; !got && !s.empty(); ) {
                        for (const std::wstring& fd : folders) {
                            if (tryBsk(NormalizeInnerPath(PathJoin(fd, s + L".bsk")))) { got = true; break; }
                        }
                        if (got) break;
                        const size_t cut = s.find_last_of(L'_');
                        if (cut == std::wstring::npos || cut == 0) break;
                        s.resize(cut);
                    }
                    if (got) {
                        p.kind = PreviewKind::Animation;      // real playback
                        info += T(L"Animasyon: iskeletle oynatılıyor.\n");
                    } else {
                        info += T(L"Not:   Eşleşen .bsk bulunamadı; yalnızca özet.\n");
                    }
                }
            } else {
                fallbackToHex(r.text());
            }
            break;
        }

        case PreviewKind::Material: {
            BmtFile bmt;
            const Result r = ParseBmt(data, size, bmt);
            if (!r.ok()) { fallbackToHex(r.text()); break; }
            info += bmt.summaryTr() + L"\n";
            p.kind = PreviewKind::Table;
            p.columns = {
                { T(L"Malzeme"),   200, false, false },
                { T(L"Diffuse Doku"), 320, false, false },
                { T(L"Mutlak"),     70, false, false },
                { T(L"Bayraklar"), 100, false, false },
                { L"Normal Map", 240, false, false },
            };
            for (const BmtMaterial& m : bmt.materials) {
                p.rows.push_back({
                    m.name,
                    m.diffuseMap.path,
                    m.diffuseMap.isAbsolute ? T(L"evet") : T(L"hayır"),
                    FormatW(L"0x%08X", m.flags),
                    m.hasNormalMap ? m.normalMap.path : std::wstring()
                });
            }
            break;
        }

        case PreviewKind::Ui2dt: {
            Nif2dtFile nif;
            const Result r = ParseNif2dt(data, size, nif);
            if (!r.ok()) { fallbackToHex(r.text()); break; }
            info += nif.summaryTr() + L"\n";
            p.columns = {
                { T(L"Ad"),     180, false, false },
                { T(L"Tür"),    120, false, false },
                { L"Id",      80, true,  true  },
                { T(L"Üst Id"),  80, true,  true  },
                { L"X",       60, true,  true  },
                { L"Y",       60, true,  true  },
                { T(L"En"),      60, true,  true  },
                { T(L"Boy"),     60, true,  true  },
                { T(L"Renk"),    90, false, false },
                { T(L"Görsel"), 260, false, false },
            };
            for (const Nif2dtEntry& e : nif.entries) {
                p.rows.push_back({
                    e.name,
                    Nif2dtTypeNameTr(e.type),
                    FormatW(L"%u", e.id),
                    FormatW(L"%u", e.parentId),
                    FormatW(L"%d", e.x),
                    FormatW(L"%d", e.y),
                    FormatW(L"%d", e.width),
                    FormatW(L"%d", e.height),
                    FormatW(L"%08X", e.color),
                    e.image
                });
            }
            // Prefer the REAL textured panel (blit each widget's atlas region);
            // fall back to the wireframe of rectangles when no .ddj could load.
            const std::wstring baseFolder =
                PathParent(p.innerPath.empty() ? p.displayName : p.innerPath);
            const UiStringTable& uistr = GetUiStrings(loader);  // real button labels
            if (!RenderNif2dtComposite(nif, loader, baseFolder, 2048, 2048, p.layout, &uistr))
                RenderNif2dtLayout(nif, 2048, 2048, p.layout);

            // A tabbed window keeps every page in the same file. Pre-render each
            // page so the user can step through them ("Sekme ▶") instead of only
            // ever seeing the default one.
            if (nif.tabs.size() > 1) {
                p.uiPages.reserve(nif.tabs.size());
                p.uiPageLabels.reserve(nif.tabs.size());
                std::vector<u64> pageHashes;
                for (size_t t = 0; t < nif.tabs.size(); ++t) {
                    Image page;
                    if (t == 0) {
                        page = p.layout;
                    } else {
                        SetNif2dtActiveTab(nif, t);
                        if (!RenderNif2dtComposite(nif, loader, baseFolder, 2048, 2048, page, &uistr))
                            continue;
                    }
                    if (!page.valid()) continue;
                    // Some tabs switch nothing the file can express (their pages
                    // are built by the client). Those render identically, so drop
                    // duplicates rather than offer the same picture several times.
                    u64 h = 1469598103934665603ull;
                    if (const u8* px = page.data()) {
                        const size_t bytes = static_cast<size_t>(page.height()) * page.stride();
                        for (size_t k = 0; k < bytes; k += 7) { h ^= px[k]; h *= 1099511628211ull; }
                    }
                    bool dup = false;
                    for (u64 prev : pageHashes) if (prev == h) { dup = true; break; }
                    if (dup) continue;
                    pageHashes.push_back(h);

                    std::wstring label = ResolveWidgetLabel(nif.entries[nif.tabs[t].entryIndex], &uistr);
                    if (label.empty()) label = FormatW(T(L"Sayfa %zu"), p.uiPages.size() + 1);
                    p.uiPages.push_back(std::move(page));
                    p.uiPageLabels.push_back(label);
                }
                SetNif2dtActiveTab(nif, 0);
                if (p.uiPages.size() > 1)
                    info += FormatW(T(L"Sekme sayfaları: %zu (Sekme ▶ ile gezin)\n"), p.uiPages.size());
                else
                    p.uiPages.clear(), p.uiPageLabels.clear();
            }
            break;
        }

        case PreviewKind::Properties: {
            ClientDataKind kind = ClientDataKind::None;
            std::wstring props;
            const Result r = DescribeClientData(p.innerPath.empty() ? p.displayName : p.innerPath,
                                                data, size, kind, props);
            if (r.ok()) {
                p.kind = PreviewKind::Summary;
                p.text = props;
                p.textIsSummary = true;
                info += std::wstring(ClientDataNameTr(kind)) + L"\n";
            } else {
                fallbackToHex(r.text());
            }
            break;
        }

        case PreviewKind::Summary: {
            std::wstring summary;
            Result r = Result::Fail(Status::Unsupported);
            switch (p.format) {
                case FormatId::Efp:  { EfpFile f;        ParseEfp(data, size, f);
                    // EFP has many controller types the tree-walker doesn't fully
                    // model, so ParseEfp often returns "partial" - but it still
                    // scavenges the referenced assets. So we DON'T gate on r.ok():
                    // an EFP always previews (its particle textures), never hex.
                    r = Result::Ok();
                    summary = f.summaryTr();
                    if (!p.renderEffect) break;   // (vendored) summary only
                    const std::wstring baseFolder =
                        PathParent(p.innerPath.empty() ? p.displayName : p.innerPath);
                    std::vector<std::wstring> texs, meshes;
                    auto addRef = [&](const std::wstring& rp) {
                        const std::wstring low = ToLowerW(rp);
                        std::vector<std::wstring>& v =
                            EndsWithI(low, L".ddj") ? texs :
                            EndsWithI(low, L".bms") ? meshes : texs;
                        if (!EndsWithI(low, L".ddj") && !EndsWithI(low, L".bms")) return;
                        for (const std::wstring& x : v) if (ToLowerW(x) == low) return;  // dedup
                        v.push_back(rp);
                    };
                    for (const std::wstring& rp : f.allResourcePaths()) addRef(rp);
                    // Raw byte scan fallback: harvest any ".ddj"/".bms" ascii path
                    // (covers refs the tree-walk consumed without storing).
                    {
                        std::string run;
                        for (size_t i = 0; i <= size; ++i) {
                            const int c = (i < size) ? data[i] : 0;
                            if (c >= 0x20 && c < 0x7F) { run.push_back((char)c); continue; }
                            if (run.size() >= 5) {
                                std::string low = run; for (char& ch : low) ch = (char)tolower((unsigned char)ch);
                                const size_t dj = low.rfind(".ddj"), bm = low.rfind(".bms");
                                size_t end = std::string::npos;
                                if (dj != std::string::npos && dj + 4 == low.size()) end = dj + 4;
                                else if (bm != std::string::npos && bm + 4 == low.size()) end = bm + 4;
                                if (end != std::string::npos)
                                    addRef(MultiByteToWide(run.substr(0, end), CP_ACP));
                            }
                            run.clear();
                        }
                    }
                    // Decode the effect's particle textures, then build an
                    // ANIMATED (looping) particle burst - not a static grid.
                    std::vector<Image> decoded;
                    for (const std::wstring& tp : texs) {
                        if (decoded.size() >= 12) break;
                        Image t;
                        if (LoadTextureAsset(tp, loader, baseFolder, t) && t.valid())
                            decoded.push_back(std::move(t));
                    }
                    // Draw the effect. Every value below is the file's own, and
                    // the same path is checked against the reference tool's
                    // output by --efpshot / --efpinfo.
                    if (!decoded.empty()) {
                        std::vector<std::wstring> texKeys;
                        for (const std::wstring& tp : texs)
                            texKeys.push_back(ToLowerW(PathFileName(tp)));

                        fx::Effect effect;
                        effect.build(f, [&](const std::wstring& want) -> int {
                            const std::wstring w = ToLowerW(PathFileName(want));
                            for (size_t k = 0; k < texKeys.size() && k < decoded.size(); ++k)
                                if (texKeys[k] == w) return static_cast<int>(k);
                            return -1;
                        }, [&](const std::wstring& want, BmsFile& bms) -> bool {
                            // A RenderMesh emitter draws nothing without its
                            // .bms, and no caller used to pass a loader - so
                            // every mesh emitter in the preview was empty.
                            std::vector<u8> mb;
                            if (!LoadAssetBytes(want, loader, baseFolder, mb) || mb.empty()) return false;
                            return ParseBms(mb.data(), mb.size(), bms, true).ok();
                        });

                        if (!effect.empty()) {
                            Scene scene;
                            scene.textures = decoded;
                            Camera cam;
                            cam.target      = Vec3(0, 0, 0);
                            cam.sceneRadius = effect.contentRadius();
                            cam.distance    = effect.cameraDistance();

                            RenderOptions ro;
                            ro.background  = Rgba{ 4, 6, 12, 255 };
                            ro.studioFloor = false;

                            constexpr int kFrames = 32;
                            // Sized to the pane, not a constant. A fixed 512 px
                            // left the effect sitting in a small box in the
                            // middle of a large window, because the image view
                            // deliberately never upscales on fit.
                            const int kSize = EffectPreviewSize();
                            const f32 dur = effect.durationMs();
                            p.animFrames.clear();
                            for (int fr = 0; fr < kFrames; ++fr) {
                                effect.emitScene(dur * (static_cast<f32>(fr) / kFrames), cam, scene);
                                Image img;
                                if (!RenderSceneToImage(scene, cam, ro, kSize, kSize, img).ok()) break;
                                p.animFrames.push_back(std::move(img));
                            }
                            if (!p.animFrames.empty()) {
                                p.image = p.animFrames.front();
                                p.kind  = PreviewKind::Image;
                                p.meta.formatName = T(L"Parçacık efekti (EFP) — animasyonlu");
                                p.meta.width  = p.image.width();
                                p.meta.height = p.image.height();
                                p.meta.dataSize = size;
                                summary += FormatW(T(L"\n\nEfekt: %zu yayıcı, %zu kare, %.0f ms."),
                                                   effect.emitterCount(), p.animFrames.size(),
                                                   static_cast<double>(dur));
                            }
                        }
                    }
                    if (p.animFrames.empty() && !meshes.empty()) {
                        std::vector<u8> mb;
                        std::wstring direct = NormalizeInnerPath(meshes.front());
                        bool got = loader && !direct.empty() && loader(direct, mb);
                        if (!got && !baseFolder.empty())
                            got = loader && loader(NormalizeInnerPath(PathJoin(baseFolder, meshes.front())), mb);
                        if (got && ParseBms(mb.data(), mb.size(), p.bms, true).ok()) {
                            p.hasBms = true; p.kind = PreviewKind::Mesh;
                            summary += T(L"\n\n3B: efektin mesh kaynağı gösteriliyor.");
                        }
                    }
                    break; }
                case FormatId::Nvm:  { NvmFile f;        r = ParseNvm(data, size, f, true);
                    if (r.ok()) { summary = f.summaryTr();
                        // Real 3D: the walkable ground (97x97 heightmap) plus the
                        // actual object meshes placed at their positions.
                        BmsFile terrain;
                        if (!f.heightMap.empty() && BuildHeightGridBms(f.heightMap, 97, 20.0f, terrain)) {
                            ComposedPart tp; tp.mesh = std::move(terrain);
                            tp.meshPath = L"navmesh-terrain"; tp.textureIndex = -1;
                            p.model.parts.push_back(std::move(tp));
                        }
                        auto ifo = LoadObjectIfo(loader);
                        std::vector<PlacedInstance> insts;
                        for (const NvmObjectInstance& o : f.objects) {
                            auto pit = ifo.find(static_cast<u32>(o.assetId));
                            if (pit != ifo.end())
                                insts.push_back({ pit->second, o.localPosition, o.yaw, Vec3(1,1,1) });
                        }
                        int placed = 0, skipped = 0;
                        if (!insts.empty())
                            ComposePlacedRegion(insts, loader, 1'500'000ull, p.model, &placed, &skipped);
                        if (!p.model.parts.empty()) {
                            p.model.sourcePath = p.displayName;
                            p.hasModel = true; p.kind = PreviewKind::Mesh;
                            if (placed) summary += FormatW(T(L"\n\n3B: zemin + %d nesne"), placed);
                            else        summary += T(L"\n\n3B: navmesh zemini");
                        }
                    } break; }
                case FormatId::Dof:  { DofFile f;        r = ParseDof(data, size, f);
                    // A .dof is a whole DUNGEON, not a list: every block is a room
                    // .bsr placed at a position + yaw, and every block carries its
                    // own decorations with a full position / rotation / scale. Build
                    // the real scene instead of printing the paths.
                    if (r.ok() || !f.blocks.empty()) {
                        summary = f.summaryTr();
                        std::vector<PlacedInstance> insts;
                        insts.reserve(f.blocks.size() + f.objectCount());
                        for (const DofBlock& b : f.blocks) {
                            const Mat4 blockM =
                                Mat4::RotationY(b.yaw) * Mat4::Translation(b.position);
                            if (!b.path.empty()) {
                                PlacedInstance pi;
                                pi.path = b.path; pi.matrix = blockM; pi.useMatrix = true;
                                insts.push_back(std::move(pi));
                            }
                            for (const DofObject& ob : b.objects) {
                                // Collision-only proxies are invisible in game.
                                if (ob.path.empty() || ob.isCollision()) continue;
                                const Mat4 objM =
                                    Mat4::Scale(ob.scale) *
                                    Mat4::RotationX(ob.rotation.x) *
                                    Mat4::RotationY(ob.rotation.y) *
                                    Mat4::RotationZ(ob.rotation.z) *
                                    Mat4::Translation(ob.position);
                                PlacedInstance pi;
                                pi.path = ob.path;
                                pi.matrix = objM * blockM;      // block-local -> dungeon
                                pi.useMatrix = true;
                                insts.push_back(std::move(pi));
                            }
                        }
                        int placed = 0, skipped = 0;
                        if (!insts.empty() &&
                            ComposePlacedRegion(insts, loader, 2'500'000ull, p.model,
                                                &placed, &skipped)) {
                            p.model.sourcePath = p.displayName;
                            p.hasModel = true;
                            p.kind = PreviewKind::Mesh;
                            summary += FormatW(T(L"\n\n3B: %d parça yerleştirildi "
                                               L"(%zu blok + %zu nesne)"),
                                               placed, f.blocks.size(), f.objectCount());
                            if (skipped) summary += FormatW(T(L", %d atlandı"), skipped);
                        } else if (!insts.empty()) {
                            summary += T(L"\n\n3B yok: blokların .bsr dosyaları bu "
                                       L"arşivde (ve komşularında) bulunamadı.");
                        }
                        if (!r.ok()) r = Result::Ok();      // partial parse still draws
                    }
                    break; }
                case FormatId::MapM: { MapMeshFile f;    r = ParseMapMesh(data, size, f);
                    if (r.ok()) { summary = f.summaryTr();
                        // Real 3D terrain mesh, previewed like a .bsr.
                        if (BuildTerrainBms(f, p.bms)) { p.hasBms = true; p.kind = PreviewKind::Mesh; }
                    } break; }
                case FormatId::MapT: { MapTextureFile f; r = ParseMapTexture(data, size, f);
                    if (r.ok()) { summary = f.summaryTr();
                        // A .t carries a COMPLETE 512x512 DXT1 DDS - that is the
                        // whole point of the file. Show it, and demote the small
                        // per-tile shadow proxy to a secondary surface.
                        // The header over-declares the payload by a few bytes in
                        // shipped files, so CLAMP to what is actually there
                        // instead of rejecting it.
                        Image real;
                        bool haveDds = false;
                        if (f.ddsOffset + 128 < size) {
                            const size_t avail = size - f.ddsOffset;
                            const size_t take  = (f.ddsSize && f.ddsSize < avail) ? f.ddsSize : avail;
                            haveDds = DecodeDds(data + f.ddsOffset, take, real, nullptr, nullptr) &&
                                      real.valid();
                        }
                        Image tiles = f.toTileLightImage();
                        if (haveDds) {
                            p.image = std::move(real);
                            p.kind  = PreviewKind::Image;
                            p.meta.formatName = T(L"Arazi dokusu (.t)");
                            if (tiles.valid()) {          // browsable as "mip" 2
                                p.mips.push_back(p.image);
                                p.mips.push_back(std::move(tiles));
                                p.mipCount = 2; p.faceCount = 1;
                            }
                        } else if (tiles.valid()) {
                            p.image = std::move(tiles);
                            p.kind  = PreviewKind::Image;
                            p.meta.formatName = T(L"Arazi ışık haritası (.t)");
                        }
                        if (p.image.valid()) {
                            p.meta.width = p.image.width(); p.meta.height = p.image.height();
                            p.meta.dataSize = size;
                            summary += FormatW(T(L"Gösterilen    : %s (%d x %d)\n"),
                                               haveDds ? T(L"gömülü doku")
                                                       : T(L"tile ışık haritası"),
                                               p.image.width(), p.image.height());
                        }
                    } break; }
                case FormatId::MapO: { MapObjectFile f;  r = ParseMapObject(data, size, -1, f);
                    // Robust: if auto-detect fails or yields nothing, try both
                    // fixed layouts and keep whichever recovers the most objects.
                    if (!r.ok() || f.objects.empty()) {
                        for (int mode : { 0, 1 }) {
                            MapObjectFile g; Result rg = ParseMapObject(data, size, mode, g);
                            if (rg.ok() && g.objects.size() > f.objects.size()) { f = std::move(g); r = rg; }
                        }
                        if (!f.objects.empty()) r = Result::Ok();
                    }
                    if (r.ok()) { summary = f.summaryTr();
                        // Real map region: place each object's actual .bsr mesh
                        // (object.ifo maps ObjID -> path) at its position + yaw.
                        auto ifo = LoadObjectIfo(loader);
                        std::vector<PlacedInstance> insts;
                        int matched = 0;
                        // The same object is listed once per block it overlaps and
                        // once per LOD group, so a region draws 2-4 copies of every
                        // tree on top of itself (z-fighting, and the face cap burns
                        // out early). Keep one instance per uid+position.
                        std::unordered_set<std::wstring> seenInst;
                        for (const MapObjectInstance& o : f.objects) {
                            auto pit = ifo.find(o.objectId);
                            if (pit == ifo.end()) continue;
                            ++matched;
                            const std::wstring key =
                                FormatW(L"%u|%d|%.1f|%.1f|%.1f", o.objectId, (int)o.uid,
                                        o.localPosition.x, o.localPosition.y, o.localPosition.z);
                            if (!seenInst.insert(key).second) continue;
                            insts.push_back({ pit->second, o.localPosition, o.yaw, Vec3(1,1,1) });
                        }
                        if (!insts.empty()) {
                            int placed = 0, skipped = 0;
                            if (ComposePlacedRegion(insts, loader, 1'500'000ull, p.model, &placed, &skipped)) {
                                p.model.sourcePath = p.displayName;
                                p.hasModel = true; p.kind = PreviewKind::Mesh;
                                summary += FormatW(T(L"\n\n3B: %d nesne yerleştirildi (object.ifo %zu kayıt)"),
                                                   placed, ifo.size());
                                if (skipped) summary += FormatW(T(L", %d atlandı (sınır)"), skipped);
                            }
                        } else if (ifo.empty()) {
                            summary += T(L"\n\n(3B için object.ifo bulunamadı; nesne mesh'leri yerleştirilemedi)");
                        }
                        (void)matched;
                    } break; }
                case FormatId::Mfo:  { MfoFile f;        r = ParseMfo(data, size, f);
                    if (r.ok()) { summary = f.summaryTr();
                        Image img = f.toRegionImage();
                        if (img.valid()) { p.image = std::move(img); p.kind = PreviewKind::Image;
                            p.meta.formatName = T(L"Harita bölge haritası (MFO)");
                            p.meta.width = p.image.width(); p.meta.height = p.image.height();
                            p.meta.dataSize = size; }
                    } break; }
                case FormatId::Cpd:  { CpdFile f;        r = ParseCpd(data, size, f);
                    if (r.ok()) { summary = f.summaryTr();
                        // Compound: compose its referenced .bsr resources as one 3D model.
                        if (!f.resourcePaths.empty()) {
                            std::vector<PlacedInstance> insts;
                            for (const std::wstring& rp : f.resourcePaths)
                                insts.push_back({ rp, Vec3(0,0,0), 0.0f, Vec3(1,1,1) });
                            int placed = 0, skipped = 0;
                            if (ComposePlacedRegion(insts, loader, 1'500'000ull, p.model, &placed, &skipped)) {
                                p.model.sourcePath = p.displayName;
                                p.hasModel = true; p.kind = PreviewKind::Mesh;
                                summary += FormatW(T(L"\n3B: %d kaynak birleştirildi"), placed);
                            }
                        }
                    } break; }
                case FormatId::Envi: { EnviFile f;       r = ParseEnvi(data, size, f);           if (r.ok()) summary = f.summaryTr(); break; }
                case FormatId::Bsk:  { BskFile f;        r = ParseBsk(data, size, f);            if (r.ok()) summary = f.summaryTr(); break; }
                default: break;
            }
            if (r.ok() && !summary.empty()) {
                p.text = summary;
                p.textIsSummary = true;
                info += summary + L"\n";
            } else {
                fallbackToHex(r.ok() ? T(L"Özet üretilemedi.") : r.text());
            }
            break;
        }

        case PreviewKind::Text:
        case PreviewKind::Table: {
            // .ifo variants have their own parser; everything else goes through
            // the generic encoding-detecting text loader.
            if (p.format == FormatId::IfoObject || p.format == FormatId::IfoObjectString ||
                p.format == FormatId::IfoObjectExt || p.format == FormatId::IfoTile2d ||
                p.format == FormatId::IfoEnvironment) {
                IfoFile ifo;
                const Result r = ParseIfo(data, size, p.displayName, ifo);
                if (r.ok() && !ifo.rows.empty()) {
                    p.kind = PreviewKind::Table;
                    size_t cols = ifo.columnHeadersTr.size();
                    for (const IfoRow& row : ifo.rows) cols = (std::max)(cols, row.fields.size());
                    for (size_t i = 0; i < cols; ++i) {
                        TableColumn c;
                        c.titleTr = (i < ifo.columnHeadersTr.size())
                                        ? ifo.columnHeadersTr[i] : FormatW(T(L"Sütun %zu"), i + 1);
                        c.width = (i == 0) ? 120 : 140;
                        p.columns.push_back(c);
                    }
                    for (const IfoRow& row : ifo.rows) p.rows.push_back(row.fields);
                    info += ifo.summaryTr() + L"\n";
                    break;
                }
                if (!r.ok()) info += T(L"Not:   ") + r.text() + L"\n";
            }

            TextDocument doc;
            const Result r = LoadTextDocument(data, size, doc, kTextPreviewBytes,
                                              TextEncoding::Unknown);
            if (!r.ok()) { fallbackToHex(r.text()); break; }
            info += T(L"Metin: ") + doc.summaryTr() + L"\n";

            const TableDelimiter delim = DetectDelimiter(doc.text);
            if (delim != TableDelimiter::None) {
                TableData table;
                if (ParseTable(doc.text, delim, false, 200000, table).ok() && !table.rows.empty()) {
                    p.kind = PreviewKind::Table;
                    const size_t cols = (std::max<size_t>)(table.columnCount, 1);
                    for (size_t i = 0; i < cols; ++i) {
                        TableColumn c;
                        c.titleTr = (i < table.headers.size() && !table.headers[i].empty())
                                        ? table.headers[i] : FormatW(T(L"Sütun %zu"), i + 1);
                        c.width = (i == 0) ? 140 : 130;
                        p.columns.push_back(c);
                    }
                    p.rows = table.rows;
                    info += T(L"Tablo: ") + table.summaryTr() + L"\n";
                    break;
                }
            }
            p.kind = PreviewKind::Text;
            p.text = doc.text;
            break;
        }

        case PreviewKind::Audio: {
            // Keep the Audio kind so the transport (Oynat / Durdur) shows.
            p.text = FormatW(T(L"Ses dosyası\n\nBiçim : %s\nBoyut : %s\n\n"
                             L"▶ Oynat ile çalın, çift tıklayarak da başlatabilirsiniz.\n"
                             L"OGG dosyaları için sistem OGG kod çözücüsü gerekir."),
                             FormatDisplayName(p.format), FormatBytesTr(size).c_str());
            p.textIsSummary = true;
            break;
        }

        default:
            p.kind = PreviewKind::Hex;
            break;
    }

    if (p.kind == PreviewKind::Hex)
        info += T(L"İmza:  ") + DescribeSignature(data, size) + L"\n";

    p.infoTr = info;
    p.status = Result::Ok();
}

/// The render half of PreviewPane::RenderPreviewToImage, run on an already
/// decoded payload. `detailsTr` (vendored addition, may be null) receives
/// MeshView::detailsTr() when a composed model is rendered.
Result RenderDecodedPayload(PreviewPayload& p, const std::wstring& name,
                            int width, int height, Image& out,
                            std::wstring* infoTextTr, std::wstring* detailsTr) {
    const u8* data = p.bytes.data();
    const size_t size = p.bytes.size();

    switch (p.kind) {
        case PreviewKind::Image: {
            if (!p.image.valid())
                return Result::Fail(Status::BadFormat, T(L"Goruntu cozumlenemedi."));
            // Fit into the requested canvas on the theme background.
            if (!out.resize(width, height))
                return Result::Fail(Status::OutOfMemory, T(L"Tuval olusturulamadi."));
            out.fill(Rgba::FromRgb(Palette::BgDarkest));
            const Image shown = p.image.applyChannelView(ChannelView::RGB);
            const double zoom = (std::min)(static_cast<double>(width) / shown.width(),
                                           static_cast<double>(height) / shown.height());
            const int w = (std::max)(1, static_cast<int>(shown.width() * zoom));
            const int h = (std::max)(1, static_cast<int>(shown.height() * zoom));
            const Image scaled = (w == shown.width() && h == shown.height())
                                     ? shown : shown.scaledBilinear(w, h);
            const int ox = (width - w) / 2, oy = (height - h) / 2;
            for (int y = 0; y < h; ++y) {
                const u8* s = scaled.row(y);
                u8* dst = out.at(ox, oy + y);
                if (!s || !dst) continue;
                std::memcpy(dst, s, static_cast<size_t>(w) * 4);
            }
            return Result::Ok();
        }

        case PreviewKind::Mesh:
        case PreviewKind::Skeleton:
        case PreviewKind::Animation: {
            Scene scene;
            // EVERY composed model goes through MeshView, animated or not.
            //
            // This used to build its own Scene for the static case, and the two
            // copies had already drifted twice: --shot still reoriented rigged
            // models with orientToUp (which pitches a quadruped onto its nose),
            // and it never applied the "_al" forced cut-out or any of the
            // per-material blend/additive/tint work. A headless check that does
            // not render what the window renders is worse than no check, so
            // there is now exactly ONE builder.
            if (p.hasModel) {
                MeshView mv;
                const bool animated = p.animateModel && p.hasBsk && p.hasBan;
                Result mr = animated ? mv.setModelAnimation(p.model, p.bsk, p.ban)
                                     : mv.setModel(p.model);
                if (mr.ok()) {
                    if (detailsTr) *detailsTr = mv.detailsTr();   // (vendored)
                    if (animated) mv.setTimeMs(g_shotAnimMs);
                    // setModel/setModelAnimation both end in resetView(), so the
                    // requested angle has to be applied HERE - and the frame
                    // re-fitted afterwards, or an off-axis view crops the model.
                    ApplyShotCamera(mv.camera());
                    mv.camera().fitToBoundsAspect(
                        mv.scene().bounds,
                        (height > 0) ? static_cast<f32>(width) / static_cast<f32>(height) : 0.0f);
                    Image img;
                    if (mv.renderToImage(width, height, img).ok() && img.valid()) {
                        out = std::move(img);
                        return Result::Ok();
                    }
                }
            }
            if (p.hasModel) {
                scene.textures = p.model.textures;
                for (const ComposedPart& part : p.model.parts) {
                    RasterMesh rm;
                    rm.vertices.reserve(part.mesh.vertices.size());
                    for (const BmsVertex& v : part.mesh.vertices) {
                        RasterVertex rv;
                        rv.position = v.position;
                        rv.normal   = v.normal;
                        rv.uv       = v.uv0;
                        rm.vertices.push_back(rv);
                    }
                    rm.indices.reserve(part.mesh.faces.size() * 3);
                    for (const BmsFace& f : part.mesh.faces) {
                        rm.indices.push_back(f.a);
                        rm.indices.push_back(f.b);
                        rm.indices.push_back(f.c);
                    }
                    rm.textureIndex = part.textureIndex;
                    if (part.hasMaterial && part.material.usesAlpha()) rm.twoSided = true;
                    // Same alpha cut-out decision as the interactive MeshView.
                    if (part.hasMaterial && part.material.usesAlpha() &&
                        part.textureIndex >= 0 &&
                        static_cast<size_t>(part.textureIndex) < scene.textures.size()) {
                        rm.alphaTest = ComputeTextureAlphaTest(scene.textures[part.textureIndex]);
                    }
                    scene.meshes.push_back(std::move(rm));
                }
            } else if (p.hasBms) {
                RasterMesh rm;
                rm.vertices.reserve(p.bms.vertices.size());
                for (const BmsVertex& v : p.bms.vertices) {
                    RasterVertex rv;
                    rv.position = v.position;
                    rv.normal   = v.normal;
                    rv.uv       = v.uv0;
                    rm.vertices.push_back(rv);
                }
                rm.indices.reserve(p.bms.faces.size() * 3);
                for (const BmsFace& f : p.bms.faces) {
                    rm.indices.push_back(f.a);
                    rm.indices.push_back(f.b);
                    rm.indices.push_back(f.c);
                }
                scene.meshes.push_back(std::move(rm));
            } else if (p.hasBsk) {
                // Bone positions: the ANIMATED pose when this is a .ban (sampled
                // at --anim ms, default the first frame), else the bind pose.
                std::vector<Vec3> world(p.bsk.bones.size());
                bool posed = false;
                if (p.hasBan && !p.ban.bones.empty()) {
                    std::vector<Quat> rot; std::vector<Vec3> tr;
                    std::vector<Mat4> wmats(p.bsk.bones.size());
                    if (SampleBan(p.ban, g_shotAnimMs, rot, tr) &&
                        rot.size() == p.ban.bones.size()) {
                        // Local -> world down the parent chain, matching bone by NAME.
                        for (size_t i = 0; i < p.bsk.bones.size(); ++i) {
                            const BskBone& b = p.bsk.bones[i];
                            Quat q = b.rotationToParent; Vec3 t = b.translationToParent;
                            for (size_t a = 0; a < p.ban.bones.size(); ++a) {
                                if (ToLowerW(p.ban.bones[a].name) == ToLowerW(b.name)) {
                                    q = rot[a]; t = tr[a]; break;
                                }
                            }
                            const Mat4 local = Mat4::FromQuat(q) * Mat4::Translation(t);
                            const Mat4 wm = (b.parentIndex >= 0 &&
                                             static_cast<size_t>(b.parentIndex) < i)
                                          ? local * wmats[static_cast<size_t>(b.parentIndex)]
                                          : local;
                            wmats[i] = wm;
                            world[i] = Vec3(wm.m[12], wm.m[13], wm.m[14]);
                        }
                        posed = true;
                    }
                }
                if (!posed)
                    for (size_t i = 0; i < p.bsk.bones.size(); ++i)
                        world[i] = p.bsk.bones[i].translationToOrigin;

                RasterLines lines;
                for (size_t i = 0; i < p.bsk.bones.size(); ++i) {
                    const BskBone& b = p.bsk.bones[i];
                    if (b.parentIndex < 0 ||
                        static_cast<size_t>(b.parentIndex) >= world.size()) continue;
                    lines.points.push_back(world[static_cast<size_t>(b.parentIndex)]);
                    lines.points.push_back(world[i]);
                }
                scene.lines.push_back(std::move(lines));
            } else {
                return RenderTextBlockToImage(name, p.text.empty() ? p.infoTr : p.text,
                                              width, height, out)
                           ? Result::Ok()
                           : Result::Fail(Status::Unknown, T(L"Metin cizilemedi."));
            }
            scene.computeBounds();
            // Only a bare .bms / skeleton reaches here now - a composed model went
            // through MeshView above. Those have no skeleton hint, so the
            // conservative bounding-box stand-up is the right and only rule; the
            // old orientToUp branch is what pitched rigged quadrupeds onto a nose.
            scene.orientUpright();
            Camera cam;
            // --shot --yaw/--pitch/--roll: orbit before framing, so a headless
            // check can inspect from the side or back (a front view alone hides a
            // misplaced back attachment).
            ApplyShotCamera(cam);
            cam.fitToBoundsAspect(scene.bounds,
                                  (height > 0) ? static_cast<f32>(width) / static_cast<f32>(height)
                                               : 0.0f);
            RenderOptions opt;
            opt.shading = scene.meshes.empty() ? ShadingMode::Wireframe : ShadingMode::Textured;
            opt.gradientBackground = true;
            opt.drawGrid = false;                            // no grid, no axes - just the model
            opt.drawAxes = false;
            if (scene.meshes.empty()) {                      // skeleton / line-only: dark bg so bones pop
                opt.background       = Rgba(0x24, 0x1A, 0x0E, 0xFF);
                opt.backgroundBottom = Rgba(0x0E, 0x0A, 0x06, 0xFF);
            } else {                       // solid model: reference showcase backdrop
                // Must match MeshView's kStudioTop/kStudioBottom so --shot and the
                // interactive pane produce the same picture.
                opt.background       = Rgba(107, 112, 122, 0xFF);
                opt.backgroundBottom = Rgba(168, 173, 181, 0xFF);
            }
            // High supersample then area-downscale for an ultra-crisp still.
            const long long spx = static_cast<long long>(width) * height;
            const int ss = (spx <= 0) ? 1 : (spx <= 1'500'000LL ? 4 : (spx <= 4'000'000LL ? 3 : 2));
            if (ss > 1) {
                Image big;
                const Result r = RenderSceneToImage(scene, cam, opt, width * ss, height * ss, big);
                if (!r.ok()) return r;
                out = big.scaledBox(width, height);
                out.applyStudioPost(1.32f, 1.06f, 0.26f);   // window / CLI parity
                return Result::Ok();
            }
            return RenderSceneToImage(scene, cam, opt, width, height, out);
        }

        case PreviewKind::Ui2dt:
            // `--page N` selects one of the window's tab pages (all of them live
            // in the same .2dt); page 0 is the default view.
            if (g_shotPageIndex > 0 && static_cast<size_t>(g_shotPageIndex) < p.uiPages.size()
                && p.uiPages[g_shotPageIndex].valid()) {
                out = p.uiPages[g_shotPageIndex];
                if (infoTextTr && static_cast<size_t>(g_shotPageIndex) < p.uiPageLabels.size())
                    *infoTextTr += T(L"Sekme: ") + p.uiPageLabels[g_shotPageIndex] + L"\n";
                return Result::Ok();
            }
            if (p.layout.valid()) {
                out = p.layout;
                return Result::Ok();
            }
            break;

        default:
            break;
    }

    // Text / table / summary / hex all render as a text block.
    std::wstring body;
    if (!p.text.empty()) {
        body = p.text;
    } else if (!p.rows.empty()) {
        for (const TableColumn& c : p.columns) { body += c.titleTr; body += L"\t"; }
        body += L"\n";
        for (size_t r = 0; r < p.rows.size() && r < 200; ++r) {
            for (const std::wstring& f : p.rows[r]) { body += f; body += L"\t"; }
            body += L"\n";
        }
    } else {
        // Hex dump of the first bytes.
        const size_t n = (std::min<size_t>)(size, 2048);
        for (size_t i = 0; i < n; i += 16) {
            body += FormatW(L"%08X  ", static_cast<unsigned>(i));
            for (size_t k = 0; k < 16; ++k) {
                if (i + k < n) body += FormatW(L"%02X ", data[i + k]);
                else           body += L"   ";
            }
            body += L" ";
            for (size_t k = 0; k < 16 && i + k < n; ++k) {
                const u8 b = data[i + k];
                body.push_back((b >= 0x20 && b < 0x7F) ? static_cast<wchar_t>(b) : L'.');
            }
            body += L"\n";
        }
    }
    if (body.empty()) body = p.infoTr;
    return RenderTextBlockToImage(name, body, width, height, out)
               ? Result::Ok()
               : Result::Fail(Status::Unknown, T(L"Metin cizilemedi."));
}

/// Shared front half of both entry points (PreviewPane::RenderPreviewToImage).
Result DecodeForPreview(const std::wstring& name, const u8* data, size_t size,
                        int width, int height, const AssetLoader& loader,
                        IArchive* archive, bool renderEffect, PreviewPayload& p) {
    if (!data || size == 0)
        return Result::Fail(Status::InvalidArgument, T(L"Onizlenecek veri yok."));
    if (width < 16 || height < 16)
        return Result::Fail(Status::InvalidArgument, T(L"Gecersiz olculer."));

    p.displayName  = name;
    p.innerPath    = name;
    p.renderEffect = renderEffect;
    try { p.bytes.assign(data, data + size); }
    catch (...) { return Result::Fail(Status::OutOfMemory, T(L"Bellek yetersiz.")); }

    AssetLoader none = [](const std::wstring&, std::vector<u8>&) { return false; };
    DecodePayload(p, loader ? loader : none, archive);
    return Result::Ok();
}

} // namespace

Result RenderPreviewToImage(const std::wstring& name,
                            const u8* data, size_t size,
                            int width, int height,
                            Image& out,
                            std::wstring* infoTextTr,
                            const AssetLoader& loader,
                            IArchive* archive) {
    ImageIO::Scope wic;
    PreviewPayload p;
    const Result r = DecodeForPreview(name, data, size, width, height, loader, archive, true, p);
    if (!r.ok()) return r;
    if (infoTextTr) *infoTextTr = p.infoTr;
    return RenderDecodedPayload(p, name, width, height, out, infoTextTr, nullptr);
}

Result BuildPreview(const std::wstring& innerPath, const u8* data, size_t size,
                    int width, int height, const AssetLoader& loader, IArchive* archive,
                    bool renderEffect, PreviewResult& out) {
    out = PreviewResult();
    ImageIO::Scope wic;
    PreviewPayload p;
    const Result r = DecodeForPreview(innerPath, data, size, width, height, loader, archive,
                                      renderEffect, p);
    if (!r.ok()) return r;

    out.format        = p.format;
    out.kind          = p.kind;
    out.formatNameTr  = p.meta.formatName.empty() ? std::wstring(FormatDisplayName(p.format))
                                                  : p.meta.formatName;
    out.infoTr        = p.infoTr;
    out.textIsSummary = p.textIsSummary;
    if (!p.text.empty()) {
        out.textTr = p.text;
    } else if (!p.rows.empty()) {
        // Same text form the --shot path draws for a table.
        for (const TableColumn& c : p.columns) { out.textTr += c.titleTr; out.textTr += L"\t"; }
        out.textTr += L"\n";
        for (size_t row = 0; row < p.rows.size() && row < 200; ++row) {
            for (const std::wstring& f : p.rows[row]) { out.textTr += f; out.textTr += L"\t"; }
            out.textTr += L"\n";
        }
    }
    out.uiPages      = p.uiPages;
    out.uiPageLabels = p.uiPageLabels;
    out.frames       = p.animFrames;

    switch (p.kind) {
        case PreviewKind::Image:
            if (p.image.valid()) out.image = p.image;
            break;
        case PreviewKind::Mesh:
        case PreviewKind::Skeleton:
        case PreviewKind::Animation:
            if (p.hasModel || p.hasBms || p.hasBsk) {
                Image img;
                const Result rr = RenderDecodedPayload(p, innerPath, width, height, img,
                                                       nullptr, &out.detailsTr);
                if (!rr.ok()) return rr;
                out.image      = std::move(img);
                out.rendered3d = true;
            }
            break;
        case PreviewKind::Ui2dt:
            if (p.layout.valid()) out.image = p.layout;
            break;
        default:
            break;
    }
    return Result::Ok();
}

// SroPk2Studio: the scene RenderDecodedPayload would rasterise, for an interactive viewer.
Result BuildPreviewScene(const std::wstring& innerPath, const u8* data, size_t size,
                         const AssetLoader& loader, IArchive* archive,
                         PreviewResult& out, Scene& scene, bool& hasScene) {
    out = PreviewResult();
    scene = Scene();
    hasScene = false;
    ImageIO::Scope wic;
    PreviewPayload p;
    const Result r = DecodeForPreview(innerPath, data, size, 512, 512, loader, archive, false, p);
    if (!r.ok()) return r;

    out.format        = p.format;
    out.kind          = p.kind;
    out.formatNameTr  = p.meta.formatName.empty() ? std::wstring(FormatDisplayName(p.format))
                                                  : p.meta.formatName;
    out.infoTr        = p.infoTr;
    out.textIsSummary = p.textIsSummary;
    out.textTr        = p.text;
    out.uiPages       = p.uiPages;
    out.uiPageLabels  = p.uiPageLabels;

    const bool is3d = p.kind == PreviewKind::Mesh || p.kind == PreviewKind::Skeleton ||
                      p.kind == PreviewKind::Animation;
    // Map data (regions, terrain, navmesh, dungeons) is world space, Y up, already level: the
    // lying-prop stand-up heuristic must not touch it (it tips a flat region onto its side).
    const bool world = p.format == FormatId::MapO || p.format == FormatId::MapM ||
                       p.format == FormatId::Nvm || p.format == FormatId::Dof;
    MapMeshFile terrain;
    if (p.format == FormatId::MapM && ParseMapMesh(data, size, terrain).ok() &&
        BuildTerrainScene(terrain, loader, scene)) {
        // Terrain: textured ground, blended ground types and water.
        scene.computeBounds();
        hasScene = true;
    } else if (is3d && p.hasModel) {
        // Same builder as the picture: MeshView (orientation, cut-outs, blending, tints).
        MeshView mv;
        const Result mr = mv.setModel(p.model);
        if (mr.ok()) {
            out.detailsTr = mv.detailsTr();
            scene = mv.scene();
            if (world) {
                for (RasterMesh& rm : scene.meshes) rm.transform = Mat4::Identity();
                scene.computeBounds();
            }
            hasScene = !scene.meshes.empty();
        }
    } else if (is3d && p.hasBms) {
        RasterMesh rm;
        rm.vertices.reserve(p.bms.vertices.size());
        for (const BmsVertex& v : p.bms.vertices) {
            RasterVertex rv;
            rv.position = v.position;
            rv.normal   = v.normal;
            rv.uv       = v.uv0;
            rm.vertices.push_back(rv);
        }
        rm.indices.reserve(p.bms.faces.size() * 3);
        for (const BmsFace& f : p.bms.faces) {
            rm.indices.push_back(f.a);
            rm.indices.push_back(f.b);
            rm.indices.push_back(f.c);
        }
        scene.meshes.push_back(std::move(rm));
        scene.computeBounds();
        if (!world) scene.orientUpright();
        hasScene = true;
    }
    if (!hasScene) {
        // Skeleton-only / image / text payloads: the regular preview.
        PreviewResult full;
        const Result fr = BuildPreview(innerPath, data, size, 512, 512, loader, archive, false, full);
        if (!fr.ok()) return fr;
        out = std::move(full);
    }
    return Result::Ok();
}

} // namespace sro
