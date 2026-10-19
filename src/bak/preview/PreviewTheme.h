// ============================================================================
//  SRO Asset Studio - preview/PreviewTheme.h   (vendored, UI-free)
//
//  The handful of ui/Theme, ui/UiCommon and ui/TableView pieces that the
//  preview decoder (PreviewEngine.cpp, lifted from ui/PreviewPane.cpp) uses to
//  paint text panels and .2dt layouts into an Image. Values and bodies are
//  copied from the BAK UI so the pictures match; the only omission is the
//  Skin 9-slice window frame in DrawPanelBorder (the plain double border that
//  BAK falls back to is drawn instead). Fonts are created at 96 dpi.
// ============================================================================
#pragma once

#include "../Common.h"

namespace sro {

// --- ui/Theme.h: palette (0xRRGGBB) ------------------------------------------
namespace Palette {
constexpr u32 BgDarkest  = 0x0E0A06;
constexpr u32 BgPanel    = 0x1E1610;
constexpr u32 BorderGold = 0x8B6914;
constexpr u32 BorderDim  = 0x4A3520;
constexpr u32 GoldBright = 0xFFCC33;
constexpr u32 Parchment  = 0xD2C4A0;
constexpr u32 FgDim      = 0x7D6F52;
} // namespace Palette

constexpr COLORREF ToColorRef(u32 rgb) {
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// --- ui/Theme.h: font roles ----------------------------------------------------
enum class FontRole : int {
    Ui = 0,        ///< Segoe UI 9
    UiBold,        ///< Segoe UI 9 bold
    UiLarge,       ///< Segoe UI 10
    Title,         ///< Georgia 15 bold
    Section,       ///< Segoe UI 9 bold
    Mono,          ///< Consolas 9
    MonoSmall,     ///< Consolas 8
    Button,        ///< Segoe UI 10 bold
    Count
};

// --- ui/Theme.cpp: the drawing primitives the preview uses ----------------------
class Theme {
public:
    static HFONT Font(FontRole role) {
        static Fonts fonts;
        const int i = static_cast<int>(role);
        if (i < 0 || i >= static_cast<int>(FontRole::Count)) return fonts.f[0];
        return fonts.f[i];
    }

    static void FillRect(HDC dc, const RECT& rc, u32 rgb) {
        HBRUSH b = ::CreateSolidBrush(ToColorRef(rgb));
        if (!b) return;
        RECT r = rc;
        ::FillRect(dc, &r, b);
        ::DeleteObject(b);
    }

    static void DrawBorder(HDC dc, const RECT& rc, u32 rgb) {
        HPEN pen = ::CreatePen(PS_SOLID, 1, ToColorRef(rgb));
        if (!pen) return;
        HGDIOBJ oldPen   = ::SelectObject(dc, pen);
        HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
        ::Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
        ::SelectObject(dc, oldBrush);
        ::SelectObject(dc, oldPen);
        ::DeleteObject(pen);
    }

    static void DrawPanelBorder(HDC dc, const RECT& rc) {
        DrawBorder(dc, rc, Palette::BorderGold);
        RECT inner = rc;
        ::InflateRect(&inner, -1, -1);
        if (inner.right > inner.left && inner.bottom > inner.top)
            DrawBorder(dc, inner, Palette::BorderDim);
    }

    static void DrawTextThemed(HDC dc, const RECT& rc, const std::wstring& text,
                               FontRole font, u32 rgb, UINT format) {
        if (text.empty()) return;
        HGDIOBJ  oldFont = ::SelectObject(dc, Font(font));
        const COLORREF oldColor = ::SetTextColor(dc, ToColorRef(rgb));
        const int      oldMode  = ::SetBkMode(dc, TRANSPARENT);
        RECT r = rc;
        ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, format);
        ::SetBkMode(dc, oldMode);
        ::SetTextColor(dc, oldColor);
        ::SelectObject(dc, oldFont);
    }

    static SIZE MeasureText(HDC dc, const std::wstring& text, FontRole font) {
        SIZE sz = { 0, 0 };
        HGDIOBJ oldFont = ::SelectObject(dc, Font(font));
        ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
        ::SelectObject(dc, oldFont);
        return sz;
    }

private:
    // ui/Theme.cpp MakeFont + BuildObjects at 96 dpi.
    static HFONT MakeFont(const wchar_t* face, int pointSize, bool bold) {
        LOGFONTW lf = {};
        lf.lfHeight = -MulDiv(pointSize, 96, 72);
        lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfOutPrecision = OUT_TT_PRECIS;
        lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
        wcsncpy_s(lf.lfFaceName, LF_FACESIZE, face, _TRUNCATE);
        HFONT f = ::CreateFontIndirectW(&lf);
        if (!f) f = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
        return f;
    }

    struct Fonts {
        HFONT f[static_cast<int>(FontRole::Count)] = {};
        Fonts() {
            const wchar_t* uiFace = LangUiFace(CurrentLang());
            const bool     latin  = (::wcscmp(uiFace, L"Segoe UI") == 0);
            const wchar_t* titleFace = latin ? L"Georgia"  : uiFace;
            const wchar_t* monoFace  = latin ? L"Consolas" : uiFace;
            f[static_cast<int>(FontRole::Ui)]        = MakeFont(uiFace, 9,  false);
            f[static_cast<int>(FontRole::UiBold)]    = MakeFont(uiFace, 9,  true);
            f[static_cast<int>(FontRole::UiLarge)]   = MakeFont(uiFace, 10, false);
            f[static_cast<int>(FontRole::Title)]     = MakeFont(titleFace, 15, true);
            f[static_cast<int>(FontRole::Section)]   = MakeFont(uiFace, 9,  true);
            f[static_cast<int>(FontRole::Mono)]      = MakeFont(monoFace, 9,  false);
            f[static_cast<int>(FontRole::MonoSmall)] = MakeFont(monoFace, 8,  false);
            f[static_cast<int>(FontRole::Button)]    = MakeFont(uiFace, 10, true);
        }
        ~Fonts() {
            for (HFONT h : f)
                if (h && h != static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT))) ::DeleteObject(h);
        }
        Fonts(const Fonts&) = delete;
        Fonts& operator=(const Fonts&) = delete;
    };
};

// --- ui/UiCommon.h --------------------------------------------------------------
namespace ui {
inline int ClampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
} // namespace ui

// --- ui/TableView.h --------------------------------------------------------------
struct TableColumn {
    std::wstring titleTr;
    int          width      = 100;   ///< logical px at 96 dpi
    bool         numeric    = false; ///< affects sorting and alignment
    bool         rightAlign = false;
};

} // namespace sro
