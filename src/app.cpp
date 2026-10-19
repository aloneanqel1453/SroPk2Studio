// SRO PK2 Studio — Win32 + Direct2D/DirectWrite premium frameless GUI.
// Single portable exe, static runtime, OS text engine, fully custom chrome.
// Left action rail, type column, adaptive preview (real texture / text),
// right-click menu, Notepad++ round-trip, in-app editor, drag & drop.
#include "app.hpp"
#include "bak2_edit.hpp"
#include "bak2_skill.hpp"
#include "bak2_tools.hpp"
#include "bak2_view.hpp"

#include "binary_info.hpp"
#include "blowfish.hpp"
#include "dds.hpp"
#include "file_browser.hpp"
#include "file_types.hpp"
#include "i18n.hpp"
#include "jmx_parse.hpp"
#include "model3d.hpp"
#include "pk2_archive.hpp"
#include "store.hpp"
#include "text.hpp"

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

// Embedded public-domain Ogg Vorbis decoder (stb_vorbis, compiled as C, no DLL).
extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate, short** output);

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace pk2;

namespace {

template <class T> void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Temporarily changes the alignment of a SHARED IDWriteTextFormat and restores the exact previous
// state on scope exit. Text formats are shared by every draw call, so a manual "set, draw, set back
// to <guess>" leaks a wrong alignment into all later draws (icons jumping above their labels).
class AlignScope {
public:
    AlignScope(IDWriteTextFormat* f, DWRITE_TEXT_ALIGNMENT ta) : f_(f) {
        if (!f_) return;
        ta_ = f_->GetTextAlignment(); pa_ = f_->GetParagraphAlignment();
        f_->SetTextAlignment(ta);
    }
    AlignScope(IDWriteTextFormat* f, DWRITE_TEXT_ALIGNMENT ta, DWRITE_PARAGRAPH_ALIGNMENT pa) : AlignScope(f, ta) {
        if (f_) f_->SetParagraphAlignment(pa);
    }
    ~AlignScope() { if (f_) { f_->SetTextAlignment(ta_); f_->SetParagraphAlignment(pa_); } }
    AlignScope(const AlignScope&) = delete;
    AlignScope& operator=(const AlignScope&) = delete;
private:
    IDWriteTextFormat* f_;
    DWRITE_TEXT_ALIGNMENT ta_ = DWRITE_TEXT_ALIGNMENT_LEADING;
    DWRITE_PARAGRAPH_ALIGNMENT pa_ = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
};

// The .efp edits BAK-2's efpwrite offers, as stepper rows of the editor panel.
struct EfpParamDef {
    const char* key;   // i18n key of the row label (looked up when drawn)
    double bak2::EfpEditor::Params::* field;
    double step, lo, hi;
    bool degrees;
};
constexpr EfpParamDef kEfpParams[] = {
    {"app_efp_hue",        &bak2::EfpEditor::Params::hue,           15.0, -180.0, 360.0, true},
    {"app_efp_saturation", &bak2::EfpEditor::Params::saturation,    0.1,  0.0,    3.0,   false},
    {"app_efp_brightness", &bak2::EfpEditor::Params::brightness,    0.1,  0.0,    3.0,   false},
    {"app_efp_scale",      &bak2::EfpEditor::Params::scale,         0.1,  0.1,    10.0,  false},
    {"app_efp_particle",   &bak2::EfpEditor::Params::particle_size, 0.1,  0.1,    10.0,  false},
    {"app_efp_speed",      &bak2::EfpEditor::Params::speed,         0.1,  0.1,    10.0,  false},
    {"app_efp_duration",   &bak2::EfpEditor::Params::duration,      0.1,  0.1,    10.0,  false},
};

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

constexpr UINT WM_APP_DONE = WM_APP + 1;
constexpr UINT WM_APP_BROWSED = WM_APP + 2;   // the file picker closed with a choice
constexpr int ID_KEY_EDIT = 101;
constexpr int ID_SEARCH_EDIT = 102;
constexpr int ID_CODE_EDIT = 103;
constexpr int ID_SKILL_EDIT = 104;
constexpr int kTabCount = 5;   // 3B Model, Doku, Ses, Metin, Skill
constexpr int kTabSkill = 4;
constexpr const char* kTabKeys[kTabCount] = {"tab_3d", "tab_tex", "tab_snd", "tab_txt", "tab_skill"};

namespace col {
constexpr UINT32 bg_top  = 0x0B1019;
constexpr UINT32 bg_bot  = 0x05070C;
constexpr UINT32 panel   = 0x121A28;
constexpr UINT32 field   = 0x0A0F19;
constexpr UINT32 text    = 0xEDF1F8;
constexpr UINT32 subtext = 0x9BA7BC;
constexpr UINT32 dim     = 0x5C687E;
constexpr UINT32 accent  = 0x5AD1FF;
constexpr UINT32 accent2 = 0x9B6BFF;
constexpr UINT32 green   = 0x45E0A0;
constexpr UINT32 red     = 0xFF6585;
constexpr UINT32 folder  = 0x6FD0FF;
}

D2D1_COLOR_F rgb(UINT32 hex, float a = 1.0f) { return D2D1::ColorF(hex, a); }

// Notepad++ when it is installed (very large tables open faster there); empty otherwise, and the
// in-app editor is used instead.
std::wstring notepadpp_exe() {
    for (const wchar_t* p : {L"C:\\Program Files\\Notepad++\\notepad++.exe", L"C:\\Program Files (x86)\\Notepad++\\notepad++.exe"})
        if (::GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return p;
    return {};
}

// ---- language ----------------------------------------------------------------
// Each language's name in the current UI language (the cards also show it in itself).
constexpr const char* kLangNameKey[kLangCount] = {"lang_en", "lang_tr", "lang_zh", "lang_ja", "lang_vi"};
constexpr const wchar_t* kRegKey = L"Software\\SroPk2Studio";

// The language saved by the picker (HKCU\Software\SroPk2Studio\Language), if any.
bool saved_language(Lang& out) {
    wchar_t buf[32] = {};
    DWORD bytes = sizeof(buf);
    if (::RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"Language", RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS) return false;
    return parse_language(buf, out);
}
void save_language(Lang l) {
    HKEY k = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    const std::wstring tag = language_tag(l);
    ::RegSetValueExW(k, L"Language", 0, REG_SZ, reinterpret_cast<const BYTE*>(tag.c_str()), static_cast<DWORD>((tag.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(k);
}

// Appends `more` to `into` (texture indices shifted, bounds widened).
void merge_mesh3d(bakb::Mesh3D& into, bakb::Mesh3D&& more) {
    if (more.empty()) return;
    const int base = static_cast<int>(into.textures.size());
    for (bakb::PreviewImage& t : more.textures) into.textures.push_back(std::move(t));
    const bool had = !into.empty();
    for (bakb::Mesh3D::Part& p : more.parts) {
        if (p.tex >= 0) p.tex += base;
        into.parts.push_back(std::move(p));
    }
    for (int k = 0; k < 3; ++k) {
        into.min[k] = had ? std::min(into.min[k], more.min[k]) : more.min[k];
        into.max[k] = had ? std::max(into.max[k], more.max[k]) : more.max[k];
    }
}

// ---- popup menu ----------------------------------------------------------------
// A menu in its own borderless top-level window drawn with Direct2D, so it shows above child
// windows (text fields, the 3D view) and may reach outside the main window. Modal: returns the
// chosen item's id, or 0.
struct PopupItem {
    std::wstring text, keys;   // label and shortcut hint
    int id = 0;
    bool enabled = true, sep = false;
};

struct PopupState {
    const std::vector<PopupItem>* items = nullptr;
    float scale = 1.0f;
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dw = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    IDWriteTextFormat* left = nullptr;
    IDWriteTextFormat* right = nullptr;
    int hover = -1, chosen = 0;
    bool done = false;
    float S(float px) const { return px * scale; }
    float item_h() const { return S(32); }
    float sep_h() const { return S(9); }
    float pad() const { return S(6); }
    int item_at(int y) const {
        float yy = pad();
        for (std::size_t i = 0; i < items->size(); ++i) {
            const PopupItem& it = (*items)[i];
            const float h = it.sep ? sep_h() : item_h();
            if (!it.sep && y >= yy && y < yy + h) return static_cast<int>(i);
            yy += h;
        }
        return -1;
    }
    void step(int dir) {   // keyboard: next enabled item
        const int n = static_cast<int>(items->size());
        for (int k = 1; k <= n; ++k) {
            const int i = ((hover < 0 ? (dir > 0 ? -1 : 0) : hover) + dir * k + n * 4) % n;
            if (!(*items)[static_cast<std::size_t>(i)].sep && (*items)[static_cast<std::size_t>(i)].enabled) { hover = i; return; }
        }
    }
};

LRESULT CALLBACK popup_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE)
        ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* st = reinterpret_cast<PopupState*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!st) return ::DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; ::BeginPaint(h, &ps);
        RECT rc; ::GetClientRect(h, &rc);
        if (!st->rt) {
            st->d2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0f, 96.0f),
                                            D2D1::HwndRenderTargetProperties(h, D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom))), &st->rt);
            if (st->rt) st->rt->CreateSolidColorBrush(rgb(col::text), &st->brush);
        }
        if (st->rt && st->brush) {
            ID2D1HwndRenderTarget* rt = st->rt;
            ID2D1SolidColorBrush* b = st->brush;
            const float W = static_cast<float>(rc.right), H = static_cast<float>(rc.bottom);
            rt->BeginDraw();
            b->SetColor(rgb(0x0C1220)); rt->FillRectangle(D2D1::RectF(0, 0, W, H), b);
            b->SetColor(rgb(0xFFFFFF, 0.10f)); rt->DrawRectangle(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), b, 1.0f);
            float yy = st->pad();
            for (std::size_t i = 0; i < st->items->size(); ++i) {
                const PopupItem& it = (*st->items)[i];
                if (it.sep) {
                    b->SetColor(rgb(0xFFFFFF, 0.07f));
                    rt->FillRectangle(D2D1::RectF(st->S(10), yy + st->sep_h() / 2 - 0.5f, W - st->S(10), yy + st->sep_h() / 2 + 0.5f), b);
                    yy += st->sep_h();
                    continue;
                }
                const D2D1_RECT_F ir = D2D1::RectF(st->S(6), yy, W - st->S(6), yy + st->item_h());
                if (static_cast<int>(i) == st->hover && it.enabled) {
                    b->SetColor(rgb(col::accent, 0.14f));
                    rt->FillRoundedRectangle(D2D1::RoundedRect(ir, st->S(8), st->S(8)), b);
                }
                b->SetColor(rgb(it.enabled ? col::text : col::dim, it.enabled ? 1.0f : 0.8f));
                rt->DrawTextW(it.text.c_str(), static_cast<UINT32>(it.text.size()), st->left,
                              D2D1::RectF(ir.left + st->S(12), ir.top, ir.right - st->S(70), ir.bottom), b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                if (!it.keys.empty()) {
                    b->SetColor(rgb(col::dim));
                    rt->DrawTextW(it.keys.c_str(), static_cast<UINT32>(it.keys.size()), st->right,
                                  D2D1::RectF(ir.left, ir.top, ir.right - st->S(12), ir.bottom), b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                yy += st->item_h();
            }
            if (rt->EndDraw() == D2DERR_RECREATE_TARGET) { safe_release(st->brush); safe_release(st->rt); }
        }
        ::EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int nh = st->item_at(GET_Y_LPARAM(l));
        RECT rc; ::GetClientRect(h, &rc);
        const int x = GET_X_LPARAM(l);
        const int hv = (x >= 0 && x < rc.right) ? nh : -1;
        if (hv != st->hover) { st->hover = hv; ::InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
        RECT rc; ::GetClientRect(h, &rc);
        const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (!::PtInRect(&rc, p)) st->done = true;   // a click outside closes the menu
        return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: {
        const int i = st->item_at(GET_Y_LPARAM(l));
        RECT rc; ::GetClientRect(h, &rc);
        const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (::PtInRect(&rc, p) && i >= 0 && (*st->items)[static_cast<std::size_t>(i)].enabled) {
            st->chosen = (*st->items)[static_cast<std::size_t>(i)].id;
            st->done = true;
        }
        return 0;
    }
    case WM_CAPTURECHANGED: st->done = true; return 0;
    default: break;
    }
    return ::DefWindowProcW(h, m, w, l);
}

int run_popup_menu(HWND owner, POINT screen, const std::vector<PopupItem>& items, float scale,
                   ID2D1Factory* d2d, IDWriteFactory* dw) {
    if (items.empty() || !d2d || !dw) return 0;
    static const wchar_t* kClass = L"SroPk2StudioMenu";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.style = CS_DROPSHADOW; wc.lpfnWndProc = &popup_proc;
        wc.hInstance = ::GetModuleHandleW(nullptr); wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = kClass;
        registered = ::RegisterClassExW(&wc) != 0;
    }
    PopupState st;
    st.items = &items; st.scale = scale; st.d2d = d2d; st.dw = dw;
    dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f * scale, L"", &st.left);
    dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * scale, L"", &st.right);
    for (IDWriteTextFormat* f : {st.left, st.right})
        if (f) { f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    if (st.right) st.right->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    // Size: the widest label plus its shortcut.
    float wmax = 0, h = st.pad() * 2;
    for (const PopupItem& it : items) {
        h += it.sep ? st.sep_h() : st.item_h();
        IDWriteTextLayout* tl = nullptr;
        if (!it.sep && SUCCEEDED(dw->CreateTextLayout(it.text.c_str(), static_cast<UINT32>(it.text.size()), st.left, 10000.0f, 100.0f, &tl)) && tl) {
            DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); tl->Release();
            wmax = std::max(wmax, m.widthIncludingTrailingWhitespace);
        }
    }
    const int W = static_cast<int>(std::max(wmax + st.S(110), st.S(190))), H = static_cast<int>(h);
    // Keep it on the monitor.
    MONITORINFO mi{sizeof(mi)};
    ::GetMonitorInfoW(::MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST), &mi);
    int x = screen.x, y = screen.y;
    if (x + W > mi.rcWork.right) x = std::max<int>(mi.rcWork.left, x - W);
    if (y + H > mi.rcWork.bottom) y = std::max<int>(mi.rcWork.top, y - H);
    HWND pop = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClass, L"", WS_POPUP, x, y, W, H, owner, nullptr,
                                 ::GetModuleHandleW(nullptr), &st);
    int chosen = 0;
    if (pop) {
        BOOL dark = TRUE; ::DwmSetWindowAttribute(pop, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        int corner = 2; ::DwmSetWindowAttribute(pop, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        ::ShowWindow(pop, SW_SHOWNOACTIVATE);
        ::SetCapture(pop);
        MSG msg;
        while (!st.done && ::GetMessageW(&msg, nullptr, 0, 0)) {
            if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {   // the menu owns the keyboard
                if (msg.wParam == VK_ESCAPE) st.done = true;
                else if (msg.wParam == VK_DOWN) { st.step(1); ::InvalidateRect(pop, nullptr, FALSE); }
                else if (msg.wParam == VK_UP) { st.step(-1); ::InvalidateRect(pop, nullptr, FALSE); }
                else if (msg.wParam == VK_RETURN && st.hover >= 0 && items[static_cast<std::size_t>(st.hover)].enabled) {
                    st.chosen = items[static_cast<std::size_t>(st.hover)].id; st.done = true;
                }
                continue;
            }
            if (msg.message == WM_KEYUP || msg.message == WM_CHAR || msg.message == WM_SYSKEYUP || msg.message == WM_SYSCHAR) continue;
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (::GetCapture() != pop) st.done = true;
        }
        chosen = st.chosen;
        if (::GetCapture() == pop) ::ReleaseCapture();
        ::DestroyWindow(pop);
    }
    safe_release(st.brush); safe_release(st.rt); safe_release(st.left); safe_release(st.right);
    return chosen;
}

// Five-point star; `rot` is the angle of one point (radians, -pi/2 = up).
ID2D1PathGeometry* star_geometry(ID2D1Factory* f, float cx, float cy, float r, float rot) {
    ID2D1PathGeometry* g = nullptr;
    if (!f || FAILED(f->CreatePathGeometry(&g))) return nullptr;
    ID2D1GeometrySink* s = nullptr;
    if (FAILED(g->Open(&s))) { g->Release(); return nullptr; }
    const float inner = r * 0.381966f;   // regular star
    for (int i = 0; i < 10; ++i) {
        const float a = rot + static_cast<float>(i) * 0.62831853f;
        const float rr = (i % 2) ? inner : r;
        const D2D1_POINT_2F p = D2D1::Point2F(cx + rr * std::cos(a), cy + rr * std::sin(a));
        if (i == 0) s->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED); else s->AddLine(p);
    }
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    s->Release();
    return g;
}

enum class Op { None, ExtractAll, ExtractSel, AddFiles, AddFolder, Defrag, OpenDeep, Recover };

enum ButtonId {
    B_OPEN, B_NEW, B_EXTRACT_ALL, B_EXTRACT_SEL, B_ADD_FILE, B_ADD_FOLDER, B_DELETE, B_DEFRAG, B_COUNT
};
struct ButtonDef { const char* key; wchar_t icon; UINT32 accent; };
const ButtonDef kButtons[B_COUNT] = {
    {"open",        L'\uE8E5', col::accent},
    {"new_pk2",     L'\uE710', col::accent},
    {"extract_all", L'\uE896', col::green},
    {"extract_sel", L'\uEDE1', col::green},
    {"add_file",    L'\uE7C3', col::accent},
    {"add_folder",  L'\uE8F4', col::accent},
    {"remove",      L'\uE74D', col::red},
    {"defrag",      L'\uEBE7', col::accent2},
};

struct Row {
    int depth = 0;
    bool is_folder = false;
    std::wstring name;
    std::wstring path;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    TypeInfo type{};
};

std::wstring fmt_size(std::uint64_t n) {
    wchar_t b[64];
    if (n < 1024) swprintf(b, 64, L"%llu B", static_cast<unsigned long long>(n));
    else if (n < 1024ull * 1024) swprintf(b, 64, L"%.1f KB", static_cast<double>(n) / 1024.0);
    else if (n < 1024ull * 1024 * 1024) swprintf(b, 64, L"%.1f MB", static_cast<double>(n) / (1024.0 * 1024));
    else swprintf(b, 64, L"%.2f GB", static_cast<double>(n) / (1024.0 * 1024 * 1024));
    return b;
}
std::wstring fmt_time(std::int64_t ft) {
    if (ft <= 0) return L"";
    FILETIME f; f.dwLowDateTime = static_cast<DWORD>(ft & 0xFFFFFFFF); f.dwHighDateTime = static_cast<DWORD>((ft >> 32) & 0xFFFFFFFF);
    FILETIME lf; SYSTEMTIME st;
    if (!::FileTimeToLocalFileTime(&f, &lf) || !::FileTimeToSystemTime(&lf, &st)) return L"";
    wchar_t b[64]; swprintf(b, 64, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return b;
}
std::wstring now_hhmmss() {
    SYSTEMTIME st; ::GetLocalTime(&st);
    wchar_t b[32]; swprintf(b, 32, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return b;
}
std::wstring base_name(const std::wstring& p) {
    std::size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

// Editable hex text: every row "OFFSET  XX XX .. XX  |ascii|" (no collapsing), parsed back by
// parse_hex_editable, which reads only the byte columns (offset and |ascii| are ignored).
std::wstring make_hex_editable(const std::vector<std::uint8_t>& b) {
    std::wstring out;
    out.reserve(b.size() / 16 * 80 + 80);
    wchar_t cell[16];
    for (std::size_t off = 0; off < b.size(); off += 16) {
        std::size_t rowlen = std::min<std::size_t>(16, b.size() - off);
        swprintf(cell, 16, L"%08zX  ", off); out += cell;
        for (std::size_t i = 0; i < 16; ++i) {
            if (i < rowlen) { swprintf(cell, 16, L"%02X ", b[off + i]); out += cell; }
            else out += L"   ";
            if (i == 7) out += L' ';
        }
        out += L" |";
        for (std::size_t i = 0; i < rowlen; ++i) { std::uint8_t c = b[off + i]; out += (c >= 0x20 && c <= 0x7E) ? static_cast<wchar_t>(c) : L'.'; }
        out += L"|\r\n";
    }
    return out;
}

bool parse_hex_editable(const std::wstring& text, std::vector<std::uint8_t>& out, std::wstring& err) {
    out.clear();
    auto hexval = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    std::size_t lineNo = 0, pos = 0;
    while (pos <= text.size()) {
        std::size_t eol = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, eol == std::wstring::npos ? std::wstring::npos : eol - pos);
        pos = (eol == std::wstring::npos) ? text.size() + 1 : eol + 1;
        ++lineNo;
        if (std::size_t bar = line.find(L'|'); bar != std::wstring::npos) line.resize(bar);
        std::vector<std::wstring> toks;
        std::wstring cur;
        for (wchar_t c : line) {
            if (c == L' ' || c == L'\t' || c == L'\r') { if (!cur.empty()) { toks.push_back(cur); cur.clear(); } }
            else cur.push_back(c);
        }
        if (!cur.empty()) toks.push_back(cur);
        std::size_t first = (!toks.empty() && toks[0].size() == 8) ? 1 : 0;   // row offset column
        for (std::size_t i = first; i < toks.size(); ++i) {
            const std::wstring& t = toks[i];
            int hi = t.size() == 2 ? hexval(t[0]) : (t.size() == 1 ? 0 : -1);
            int lo = t.empty() ? -1 : hexval(t.back());
            if (hi < 0 || lo < 0) { err = Lf("app_hex_bad_byte", lineNo, t); return false; }
            out.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
        }
    }
    return true;
}

// Exception text: our engine and the bridges throw UTF-8; anything else is the ANSI code page.
std::wstring exc_text(const std::exception& ex) {
    std::string s = ex.what();
    if (!s.empty() && ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0) > 0)
        return utf8_to_wide(s);
    return acp_to_wide(s);
}

// One-line decode of a few known client config records (SV.T version, SilkCfg.dat).
std::wstring decode_known(const std::wstring& name, const std::vector<std::uint8_t>& b) {
    std::wstring base = base_name(name);
    std::wstring low = to_lower(base);
    // SV.T — Silkroad version (blowfish key = first 8 bytes of "SILKROADVERSION").
    if (low == L"sv.t" && b.size() >= 12) {
        std::uint32_t len; std::memcpy(&len, b.data(), 4);
        if (len == 8) {
            const char key8[8] = {'S','I','L','K','R','O','A','D'};
            Blowfish bf; bf.initialize(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(key8), 8));
            auto dec = bf.decode(std::span<const std::uint8_t>(b.data() + 4, 8));
            std::string ver;
            for (std::size_t i = 0; i < dec.size() && dec[i] >= 0x20 && dec[i] <= 0x7E; ++i) ver.push_back(static_cast<char>(dec[i]));
            if (!ver.empty()) return Lf("app_svt_version", utf8_to_wide(ver));
        }
    }
    // SilkCfg.dat — client graphics/resolution config.
    if (low.find(L"silkcfg") != std::wstring::npos && b.size() >= 15) {
        std::uint32_t ver, w, h; std::memcpy(&ver, b.data(), 4); std::memcpy(&w, b.data() + 8, 4); std::memcpy(&h, b.data() + 12, 4);
        if (ver <= 8 && w <= 16384 && h <= 16384) {
            wchar_t buf[128]; swprintf(buf, 128, L"SilkCfg v%u — %u×%u", ver, w, h);
            return buf;
        }
    }
    return {};
}
std::vector<std::uint8_t> encode_text(const std::wstring& w, int enc) {
    std::vector<std::uint8_t> out;
    if (enc == 2) {
        out.push_back(0xFF); out.push_back(0xFE);
        const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(w.data());
        out.insert(out.end(), p, p + w.size() * 2);
    } else if (enc == 3) {
        out.push_back(0xEF); out.push_back(0xBB); out.push_back(0xBF);
        std::string s = wide_to_utf8(w); out.insert(out.end(), s.begin(), s.end());
    } else if (enc == 1) {
        std::string s = wide_to_utf8(w); out.insert(out.end(), s.begin(), s.end());
    } else if (enc == 4) {
        int n = ::WideCharToMultiByte(949, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(n), '\0');
        ::WideCharToMultiByte(949, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        out.insert(out.end(), s.begin(), s.end());
    } else {
        std::string s = wide_to_acp(w); out.insert(out.end(), s.begin(), s.end());
    }
    return out;
}

// ===========================================================================
class App {
public:
    int run(HINSTANCE hinst, int nCmdShow);

private:
    static LRESULT CALLBACK wndproc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    void create_device_independent();
    void create_device_resources();
    void discard_device_resources();
    void recreate_text_formats();
    void reset_text_formats();
    void make_tex_bitmap();
    void make_prev3d_bitmap();
    ID2D1Bitmap* make_bitmap(const Image& img);
    bool bak_preview(const Row& r, const std::vector<std::uint8_t>& bytes, bakb::Preview& out, bool want_mesh = false);
    bool show_mesh3d(const bakb::Mesh3D& mesh, bool map_view);
    bool block_terrain(const std::wstring& path, bakb::Mesh3D& out);
    [[nodiscard]] bool can_replace_ddj() const;
    void replace_ddj_from_image();
    void replace_ddj_with(const std::wstring& inner, const std::wstring& src);

    void render();
    void layout();
    void layout_controls();

    void rebuild_rows();
    void add_rows(const Pk2Folder& folder, int depth);
    void select_row(int i);

    int row_at(int x, int y) const;
    int rail_at(int x, int y) const;
    int tab_at(int x, int y) const;
    void toggle_expand(int row);
    void run_button(int id);
    [[nodiscard]] bool button_enabled(int i) const;

    void clear_texture();
    void play_sound(const Row& r);
    void stop_sound();
    bool load_texture_bytes(const std::vector<std::uint8_t>& bytes);
    const Pk2File* resolve_inner(const std::wstring& ref, const std::wstring& src_dir);
    bool resolve_and_load_texture(const Row& r);
    bool build_and_load_model(const Row& r);
    void update_model_view();

    // Opens the entry (or `content`, e.g. a decrypted skill table) in Notepad++ and syncs saves back;
    // `reencrypt` applies BAK-2's skill cipher again before writing.
    void edit_in_notepadpp(const Row& r, const std::vector<std::uint8_t>* content = nullptr, bool reencrypt = false);
    void open_inapp_editor(const Row& r);
    void save_inapp_editor();
    void close_inapp_editor();
    void poll_watches();

    void do_open(); void open_path(const std::wstring& path);
    void offer_deep_open(const std::wstring& path); void start_deep_open(const std::wstring& path);
    void after_open(const std::wstring& path); void recover_keys(int sel);
    int recover_row_ = -1; bool recover_ok_ = false; std::wstring recover_msg_; void do_new(); void do_extract_all(); void do_extract_sel();
    void do_add_files(); void do_add_folder(); void do_delete(); void do_defrag();
    void create_pk2(std::wstring path); void extract_all_to(const std::wstring& dest);
    void extract_row_to(const Row& r, const std::wstring& dest);
    void add_paths(const std::vector<std::wstring>& paths, const std::wstring& base);

    void start_op(Op op, std::function<void()> work);
    void finish_op();
    void log(const std::wstring& msg);
    void set_status(const std::wstring& s);
    void ensure_timer();
    void refresh() { if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE); }

    // In-app file / folder picker (no Windows dialog). `done` gets the chosen paths after the
    // picker has closed, from the message loop; nothing runs when it is cancelled.
    using Picked = std::function<void(std::vector<std::wstring>)>;
    void browse(ui::BrowseRequest request, Picked done);
    [[nodiscard]] ui::Theme browser_theme() const;
    ui::FileBrowser browser_;
    Picked browse_done_;
    std::vector<std::wstring> browse_result_;

    float S(float px) const { return px * scale_; }

    HWND hwnd_ = nullptr;
    HINSTANCE hinst_ = nullptr;
    float scale_ = 1.0f;
    HFONT edit_font_ = nullptr, code_font_ = nullptr;
    HWND key_edit_ = nullptr, search_edit_ = nullptr, code_edit_ = nullptr;
    HBRUSH edit_bg_ = nullptr, code_bg_ = nullptr;
    int cap_hover_ = 0;

    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    ID2D1LinearGradientBrush* bg_brush_ = nullptr;
    ID2D1LinearGradientBrush* accent_brush_ = nullptr;
    ID2D1RadialGradientBrush* aurora_a_ = nullptr;
    ID2D1RadialGradientBrush* aurora_b_ = nullptr;
    ID2D1SolidColorBrush* solid_ = nullptr;
    ID2D1Bitmap* tex_bitmap_ = nullptr;
    ID2D1Bitmap* prev3d_bitmap_ = nullptr;   // BAK 3D render (.nvm/.bsk/.ban/...) when no live viewer applies
    IDWriteTextFormat* f_ui_ = nullptr;
    IDWriteTextFormat* f_ui_right_ = nullptr;
    IDWriteTextFormat* f_semibold_ = nullptr;
    IDWriteTextFormat* f_title_ = nullptr;
    IDWriteTextFormat* f_icon_ = nullptr;
    IDWriteTextFormat* f_small_ = nullptr;
    IDWriteTextFormat* f_mono_ = nullptr;

    std::unique_ptr<Store> archive_ = std::make_unique<Store>(); // our pk2 engine, or BAK's backends (.vp, PK2v2, folders, SailSafe)
    std::unique_ptr<Store> pending_store_;   // deep-key open runs on the worker, swapped in on completion
    std::wstring pending_path_;
    std::wstring key_ = L"169841";
    std::wstring search_;
    std::vector<Row> rows_;
    std::set<std::wstring> expanded_;
    int selected_ = -1;
    int hover_row_ = -1;
    int hover_rail_ = -1;
    float scroll_ = 0.0f;
    bool sb_drag_ = false;
    float rail_anim_[B_COUNT] = {};

    int preview_tab_ = 3;
    Image tex_img_{};
    bool tex_ok_ = false;
    // .2dt with tabs: every tab page BAK rendered (the Doku tab steps through them).
    std::vector<Image> tex_pages_;
    std::vector<std::wstring> tex_page_labels_;
    std::size_t tex_page_ = 0;
    D2D1_RECT_F btn_page_prev_{}, btn_page_next_{};
    void show_tex_page(std::size_t i);
    std::wstring text_preview_;
    int text_enc_ = 0;
    bool text_is_info_ = false;              // text_preview_ is a readable description / listing, not the file's text
    // Structured editor behind the description view (instead of raw bytes).
    enum class InfoEdit { None, Text, Version, UiTable, SkillTable, Division, GatePort, Readable };
    std::wstring goto_path_;                 // InfoEdit::Readable: the readable copy of this table
    void reveal_path(const std::wstring& path);
    // The rows a textdata split "<dir><table>_<n>.txt" covers, taken from the table's readable copy.
    std::wstring split_rows_from_copy(const std::wstring& dir, const std::wstring& table, unsigned n,
                                      const std::wstring& copy) const;
    InfoEdit info_edit_ = InfoEdit::None;
    D2D1_RECT_F btn_info_edit_{};
    // The description view's action: the structured editor, else the byte editor (files up to 4 MB).
    bool info_action_visible() const {
        // A playing skill's description is not the selected file: no file action under it.
        if (skill_playing_ || selected_ < 0 || selected_ >= static_cast<int>(rows_.size())) return false;
        if (info_edit_ == InfoEdit::Readable) return true;   // navigation, allowed read-only too
        if (archive_->read_only()) return false;
        const std::uint64_t n = rows_[static_cast<std::size_t>(selected_)].size;
        return info_edit_ != InfoEdit::None || (n > 0 && n <= 4'000'000);
    }
    Image prev3d_img_{};
    std::wstring sel_type_name_;

    bool ctx_open_ = false;
    float ctx_x_ = 0, ctx_y_ = 0;
    int ctx_hover_ = -1;
    struct CtxItem { const char* key; int id; bool danger; bool sep; };
    std::vector<CtxItem> ctx_items_;

    bool editor_open_ = false;
    int editor_mode_ = 0; // 0 = text, 1 = SV.T version, 2 = hex bytes
    // In-app editor: custom-drawn scrollbar (the EDIT has no native bars).
    D2D1_RECT_F ed_bar_{};
    bool ed_sb_drag_ = false;
    int code_line_h_ = 16;
    WNDPROC edit_proc_ = nullptr;
    static LRESULT CALLBACK code_edit_proc(HWND h, UINT m, WPARAM w, LPARAM l);
    // Text fields get our menu (Undo / Cut / Copy / Paste / Delete / Select All), not Windows'.
    static LRESULT CALLBACK field_proc(HWND h, UINT m, WPARAM w, LPARAM l);
    void subclass_field(HWND h);
    void edit_context_menu(HWND edit, LPARAM lp);
    void open_editor(const std::wstring& inner, int mode, const std::wstring& content, std::size_t limit);
    void open_hex_editor(const Row& r);
    void editor_scroll_to(float y);
    // In-app dialog (replaces native message boxes): message or yes/no confirmation, modal.
    struct Dialog {
        bool open = false, confirm = false;
        std::wstring title, text;
        std::function<void()> on_yes;
        D2D1_RECT_F ok{}, cancel{};
    } dlg_;
    void show_message(const std::wstring& text, const std::wstring& title = {});
    void show_confirm(const std::wstring& title, const std::wstring& text, std::function<void()> on_yes);
    void close_dialog(bool yes);
    std::wstring editor_inner_;
    void open_svt_editor(const Row& r);

    bool audio_playing_ = false;
    std::wstring audio_name_;
    std::wstring audio_temp_;
    HWAVEOUT wave_out_ = nullptr;   // in-process PCM playback (ogg via stb_vorbis)
    WAVEHDR wave_hdr_{};
    std::vector<short> wave_pcm_;
    D2D1_RECT_F btn_audio_{};
    D2D1_RECT_F btn_rotate_{}, btn_resetview_{};

    ModelView model_view_;
    bool model_ready_ = false;
    // BAK-2 (SkillEffect Studio) live viewer: .efp effects and .bsr/.cpd/.bms models (OpenGL, CPU fallback).
    bak2::View fxview_;
    bool fxview_ready_ = false, fxview_has_ = false, fxview_vis_ = false, fxview_play_ = true;
    RECT fxview_rect_{};
    D2D1_RECT_F btn_replace_{};              // Doku tab: replace the selected .ddj from a picture file

    // Skill tab (BAK-2 skillplay through the same viewer).
    std::unique_ptr<bak2::SkillDb> skill_db_;
    bool skill_db_tried_ = false, skill_playing_ = false, skill_loop_ = true, skill_edit_vis_ = false;
    std::vector<bak2::SkillItem> skill_results_;
    int skill_sel_ = -1, skill_hover_ = -1;
    float skill_scroll_ = 0;
    HWND skill_edit_ = nullptr;
    RECT skill_edit_rect_{};
    D2D1_RECT_F sk_search_{}, sk_list_{}, sk_view_{}, btn_sk_play_{}, btn_sk_loop_{}, btn_sk_reset_{};
    std::vector<std::uint8_t> skill_snd_buf_;
    void ensure_skill_db();
    void skill_search();
    void play_skill_row(int k);
    void play_skill_sound(const std::wstring& path);
    int skill_row_at(int x, int y) const;

    // .efp editor (BAK-2 efpwrite): live preview in the viewer, save back into the archive.
    std::unique_ptr<bak2::EfpEditor> efp_ed_;
    bool efp_panel_ = false;
    std::wstring efp_path_;
    std::vector<std::uint8_t> efp_bytes_;
    bak2::EfpEditor::Params efp_p_{};
    D2D1_RECT_F btn_efpedit_{}, efp_panel_rect_{}, btn_efp_reset_{}, btn_efp_save_{}, btn_efp_colorize_{}, btn_efp_recolor_{};
    struct StepHit { int param; int dir; D2D1_RECT_F r; };
    std::vector<StepHit> efp_hits_;
    void efp_toggle_panel();
    void efp_apply();
    void efp_save();
    bool model_has_ = false;

    struct Watch { std::wstring temp; std::wstring inner; FILETIME t; bool reencrypt = false; };
    std::vector<Watch> watches_;
    std::wstring tmp_dir_;

    D2D1_RECT_F r_cap_{}, r_rail_{}, r_sub_{}, r_tree_{}, r_tree_head_{}, r_prev_{}, r_log_{}, r_status_{};
    D2D1_RECT_F rail_rect_[B_COUNT]{};
    D2D1_RECT_F cap_min_{}, cap_max_{}, cap_close_{}, cap_group_{};
    int cap_pressed_ = 0;                       // window button held down (1 min, 2 max, 3 close)
    [[nodiscard]] int cap_button_at(int x, int y) const;
    ID2D1StrokeStyle* round_cap_ = nullptr;     // rounded line ends for the drawn icons
    [[nodiscard]] float text_width(const std::wstring& s, IDWriteTextFormat* f) const;
    D2D1_RECT_F tab_rect_[kTabCount]{};
    D2D1_RECT_F prev_stage_{}, prev_detail_{};
    D2D1_RECT_F btn_edit_{}, btn_npp_{}, btn_editor_save_{}, btn_editor_cancel_{};
    float col_type_x_ = 0, col_size_x_ = 0, col_mod_x_ = 0, scrollbar_x_ = 0, tree_right_ = 0;
    float row_h_ = 30, cap_h_ = 46, rail_w_ = 206;

    std::deque<std::wstring> log_;

    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<std::uint64_t> prog_done_{0}, prog_total_{0};
    std::mutex prog_mtx_;
    std::wstring prog_current_, result_msg_;
    bool result_ok_ = false;
    Op cur_op_ = Op::None;
    ULONGLONG op_start_ = 0;
    std::uint64_t op_size_before_ = 0;
    bool timer_on_ = false;
    int anim_phase_ = 0;
    std::wstring status_;
    // The archive line of the title bar: counts, size and how much Ultra Compact would free.
    [[nodiscard]] std::wstring archive_status() const;

    // Language picker: the flag button in the title bar opens a panel of flag cards.
    bool lang_open_ = false, lang_btn_hover_ = false;
    int lang_hover_ = -1;
    D2D1_RECT_F cap_lang_{}, lang_panel_{}, lang_cards_[kLangCount]{};
    void draw_flag(Lang l, D2D1_RECT_F r);
    void apply_language(Lang l);
    [[nodiscard]] int lang_card_at(int x, int y) const;
};

App* g_app = nullptr;

// ---- D2D resources ---------------------------------------------------------
void App::create_device_independent() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_);
    if (d2d_)
        d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                            D2D1_LINE_JOIN_ROUND), nullptr, 0, &round_cap_);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dw_));
    recreate_text_formats();
}
void App::recreate_text_formats() {
    safe_release(f_ui_); safe_release(f_ui_right_); safe_release(f_semibold_);
    safe_release(f_title_); safe_release(f_icon_); safe_release(f_small_); safe_release(f_mono_);
    auto make = [&](const wchar_t* fam, float sz, DWRITE_FONT_WEIGHT w, IDWriteTextFormat** out) {
        dw_->CreateTextFormat(fam, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sz, L"", out);
    };
    make(L"Segoe UI", S(14.5f), DWRITE_FONT_WEIGHT_NORMAL, &f_ui_);
    make(L"Segoe UI", S(13.5f), DWRITE_FONT_WEIGHT_NORMAL, &f_ui_right_);
    make(L"Segoe UI Semibold", S(13.5f), DWRITE_FONT_WEIGHT_SEMI_BOLD, &f_semibold_);
    make(L"Segoe UI Semibold", S(16.5f), DWRITE_FONT_WEIGHT_SEMI_BOLD, &f_title_);
    make(L"Segoe Fluent Icons", S(15.0f), DWRITE_FONT_WEIGHT_NORMAL, &f_icon_);
    make(L"Segoe UI", S(12.0f), DWRITE_FONT_WEIGHT_NORMAL, &f_small_);
    make(L"Consolas", S(12.0f), DWRITE_FONT_WEIGHT_NORMAL, &f_mono_);
    if (f_ui_) f_ui_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (f_title_) f_title_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (f_semibold_) f_semibold_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);   // labels never break into two lines
    if (f_mono_) f_mono_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    reset_text_formats();
}
// The one canonical alignment of every shared text format. Applied at creation and at the start
// of every frame, so no state can survive from an earlier draw, tab or file.
void App::reset_text_formats() {
    struct K { IDWriteTextFormat* f; DWRITE_TEXT_ALIGNMENT t; DWRITE_PARAGRAPH_ALIGNMENT p; };
    const K ks[] = {
        {f_ui_, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER},
        {f_ui_right_, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER},
        {f_semibold_, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER},
        {f_title_, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER},
        {f_icon_, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER},
        {f_small_, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER},
        {f_mono_, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR}, // text/hex views start at the top
    };
    for (const K& k : ks) if (k.f) { k.f->SetTextAlignment(k.t); k.f->SetParagraphAlignment(k.p); }
}
void App::create_device_resources() {
    if (rt_) return;
    RECT rc; ::GetClientRect(hwnd_, &rc);
    d2d_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f),
        D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)), &rt_);
    if (!rt_) return;
    rt_->CreateSolidColorBrush(rgb(col::text), &solid_);
    auto grad2 = [&](UINT32 c0, UINT32 c1, D2D1_POINT_2F a, D2D1_POINT_2F b, ID2D1LinearGradientBrush** out) {
        ID2D1GradientStopCollection* s = nullptr; D2D1_GRADIENT_STOP gs[2] = {{0, rgb(c0)}, {1, rgb(c1)}};
        rt_->CreateGradientStopCollection(gs, 2, &s);
        if (s) { rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), s, out); s->Release(); }
    };
    grad2(col::bg_top, col::bg_bot, D2D1::Point2F(0, 0), D2D1::Point2F(0, 1000), &bg_brush_);
    grad2(col::accent, col::accent2, D2D1::Point2F(0, 0), D2D1::Point2F(40, 40), &accent_brush_);
    auto radial = [&](UINT32 c, float a, ID2D1RadialGradientBrush** out) {
        ID2D1GradientStopCollection* s = nullptr; D2D1_GRADIENT_STOP gs[2] = {{0, rgb(c, a)}, {1, rgb(c, 0)}};
        rt_->CreateGradientStopCollection(gs, 2, &s);
        if (s) { rt_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 0), 1, 1), s, out); s->Release(); }
    };
    radial(col::accent2, 0.22f, &aurora_a_);
    radial(col::accent, 0.16f, &aurora_b_);
    if (tex_ok_ && tex_img_.width && !tex_bitmap_) make_tex_bitmap();
    if (prev3d_img_.width && !prev3d_bitmap_) make_prev3d_bitmap();
}
void App::discard_device_resources() {
    safe_release(bg_brush_); safe_release(accent_brush_); safe_release(aurora_a_); safe_release(aurora_b_);
    safe_release(tex_bitmap_); safe_release(prev3d_bitmap_); safe_release(solid_); safe_release(rt_);
}

// ---- model -----------------------------------------------------------------
void App::add_rows(const Pk2Folder& folder, int depth) {
    for (const Pk2Folder& c : folder.folders) {
        Row r; r.depth = depth; r.is_folder = true; r.name = c.name; r.path = c.path;
        r.type.color = col::folder; r.type.category = FileCategory::Folder;
        rows_.push_back(r);
        if (expanded_.count(c.path)) add_rows(c, depth + 1);
    }
    for (const Pk2File& f : folder.files) {
        Row r; r.depth = depth; r.is_folder = false; r.name = f.name; r.path = f.path;
        r.size = f.size; r.mtime = f.mtime; r.type = classify(f.name);
        rows_.push_back(r);
    }
}
void App::rebuild_rows() {
    rows_.clear();
    if (!archive_->is_open()) { selected_ = -1; return; }
    if (!search_.empty()) {
        std::wstring q = to_lower(search_);
        std::vector<const Pk2Folder*> st{&archive_->root()};
        while (!st.empty()) {
            const Pk2Folder* f = st.back(); st.pop_back();
            for (const Pk2File& file : f->files)
                if (to_lower(file.name).find(q) != std::wstring::npos || file.path.find(q) != std::wstring::npos) {
                    Row r; r.is_folder = false; r.name = file.path; r.path = file.path;
                    r.size = file.size; r.mtime = file.mtime; r.type = classify(file.name);
                    rows_.push_back(r);
                }
            for (const Pk2Folder& c : f->folders) st.push_back(&c);
        }
    } else add_rows(archive_->root(), 0);
    if (selected_ >= static_cast<int>(rows_.size())) selected_ = -1;
}

void App::clear_texture() {
    safe_release(tex_bitmap_); tex_ok_ = false; tex_img_ = Image{};
    tex_pages_.clear(); tex_page_labels_.clear(); tex_page_ = 0;
}

// Play a sound entry in-process: .ogg decoded by stb_vorbis into waveOut, .wav/.mp3/.wma/.mid
// through MCI (winmm). No external player.
void App::play_sound(const Row& r) {
    stop_sound();
    if (r.type.category != FileCategory::Sound) { log(Lf("app_snd_not_sound", base_name(r.name))); return; }
    const Pk2File* f = archive_->find_file(r.path);
    if (!f) return;
    auto bytes = archive_->read_file(*f);
    std::wstring nameext; { std::size_t d = r.name.find_last_of(L'.'); if (d != std::wstring::npos) nameext = to_lower(r.name.substr(d + 1)); }

    // .ogg: decode in-memory with stb_vorbis and play PCM via waveOut (in-process, no external player).
    if (nameext == L"ogg") {
        int ch = 0, rate = 0; short* out = nullptr;
        int samples = stb_vorbis_decode_memory(bytes.data(), static_cast<int>(bytes.size()), &ch, &rate, &out);
        if (samples > 0 && out && ch > 0 && rate > 0) {
            wave_pcm_.assign(out, out + static_cast<std::size_t>(samples) * ch);
            free(out);
            WAVEFORMATEX wf{}; wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = static_cast<WORD>(ch);
            wf.nSamplesPerSec = static_cast<DWORD>(rate); wf.wBitsPerSample = 16;
            wf.nBlockAlign = static_cast<WORD>(ch * 2); wf.nAvgBytesPerSec = static_cast<DWORD>(rate * ch * 2);
            if (::waveOutOpen(&wave_out_, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
                wave_hdr_ = WAVEHDR{};
                wave_hdr_.lpData = reinterpret_cast<LPSTR>(wave_pcm_.data());
                wave_hdr_.dwBufferLength = static_cast<DWORD>(wave_pcm_.size() * sizeof(short));
                ::waveOutPrepareHeader(wave_out_, &wave_hdr_, sizeof(WAVEHDR));
                ::waveOutWrite(wave_out_, &wave_hdr_, sizeof(WAVEHDR));
                audio_playing_ = true; audio_name_ = base_name(r.name);
                log(L"▶ " + audio_name_); refresh(); return;
            }
            wave_pcm_.clear();
        } else if (out) { free(out); }
        log(L("app_snd_ogg_fail"));
    }

    if (tmp_dir_.empty()) { wchar_t tp[MAX_PATH]; ::GetTempPathW(MAX_PATH, tp); tmp_dir_ = std::wstring(tp) + L"SroPk2Studio\\"; ::CreateDirectoryW(tmp_dir_.c_str(), nullptr); }
    std::wstring tmp = tmp_dir_ + base_name(r.name);
    { HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h == INVALID_HANDLE_VALUE) { log(L("app_snd_write_fail")); return; }
      DWORD wr = 0; if (!bytes.empty()) ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wr, nullptr); ::CloseHandle(h); }
    audio_temp_ = tmp; audio_name_ = base_name(r.name);
    std::wstring ext; { std::size_t d = tmp.find_last_of(L'.'); if (d != std::wstring::npos) ext = to_lower(tmp.substr(d + 1)); }
    bool mci = (ext == L"wav" || ext == L"mp3" || ext == L"wma" || ext == L"mid" || ext == L"midi");
    if (mci) {
        ::mciSendStringW(L"close sroaud", nullptr, 0, nullptr);
        std::wstring open = L"open \"" + tmp + L"\" alias sroaud";
        wchar_t err[256] = {};
        MCIERROR e = ::mciSendStringW(open.c_str(), nullptr, 0, nullptr);
        if (e == 0) e = ::mciSendStringW(L"play sroaud", nullptr, 0, nullptr);
        if (e == 0) { audio_playing_ = true; log(L"▶ " + audio_name_); refresh(); return; }
        ::mciGetErrorStringW(e, err, 256);
        log(std::wstring(L"✖ MCI: ") + err);
    }
    // Everything plays inside the app; no external player window is ever opened.
    audio_playing_ = false; log(Lf("snd_cannot_play", audio_name_)); refresh();
}

void App::stop_sound() {
    if (wave_out_) {
        ::waveOutReset(wave_out_);
        ::waveOutUnprepareHeader(wave_out_, &wave_hdr_, sizeof(WAVEHDR));
        ::waveOutClose(wave_out_);
        wave_out_ = nullptr;
        wave_pcm_.clear();
    }
    if (!audio_playing_ && audio_temp_.empty()) return;
    ::mciSendStringW(L"stop sroaud", nullptr, 0, nullptr);
    ::mciSendStringW(L"close sroaud", nullptr, 0, nullptr);
    audio_playing_ = false;
}

bool App::load_texture_bytes(const std::vector<std::uint8_t>& bytes) {
    const std::uint8_t* p = bytes.data();
    std::size_t n = bytes.size();
    // JMXVMAPT (.t / SV.T): baked terrain lightmap whose buffer is a DDS at a fixed offset.
    // Layout: 12 sig + 9216 lightMap + 4 size + 4 type, then the DDS buffer.
    if (n >= 9236 && std::memcmp(p, "JMXVMAPT", 8) == 0) { p += 9236; n -= 9236; }
    auto img = decode_image(p, n);
    if (!img) return false;
    tex_img_ = std::move(*img); tex_ok_ = true; create_device_resources();
    make_tex_bitmap();
    return true;
}

// (Re)create the D2D bitmap of the current texture. Premultiplied, so transparent texels show the
// checkerboard instead of their (usually black) RGB. An all-zero alpha channel means the alpha byte
// is unused (X8 / BI_RGB) and the image is really opaque.
void App::make_tex_bitmap() {
    safe_release(tex_bitmap_);
    if (tex_ok_) tex_bitmap_ = make_bitmap(tex_img_);
}
void App::make_prev3d_bitmap() {
    safe_release(prev3d_bitmap_);
    prev3d_bitmap_ = make_bitmap(prev3d_img_);
}
ID2D1Bitmap* App::make_bitmap(const Image& img) {
    if (!rt_ || !img.width || !img.height || img.bgra.size() < static_cast<std::size_t>(img.width) * img.height * 4) return nullptr;
    std::vector<std::uint8_t> px = img.bgra;
    bool any_alpha = false;
    for (std::size_t i = 3; i < px.size(); i += 4) if (px[i]) { any_alpha = true; break; }
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
        unsigned a = any_alpha ? px[i + 3] : 255u;
        px[i + 3] = static_cast<std::uint8_t>(a);
        if (a != 255u)
            for (std::size_t k = 0; k < 3; ++k) px[i + k] = static_cast<std::uint8_t>((px[i + k] * a + 127u) / 255u);
    }
    ID2D1Bitmap* bmp = nullptr;
    rt_->CreateBitmap(D2D1::SizeU(img.width, img.height), px.data(), img.width * 4,
                      D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bmp);
    return bmp;
}

// Resolve a resource path (as stored in a JMX file) to a file inside the pk2.
const Pk2File* App::resolve_inner(const std::wstring& ref, const std::wstring& src_dir) {
    std::wstring r = ref;
    for (wchar_t& c : r) if (c == L'\\') c = L'/';
    while (!r.empty() && r.front() == L'/') r.erase(r.begin());
    // 1) as-is from root
    if (const Pk2File* f = archive_->find_file(r)) return f;
    // 2) relative to the source file's directory
    if (!src_dir.empty()) { if (const Pk2File* f = archive_->find_file(src_dir + L"/" + r)) return f; }
    // 3) basename in the source directory
    std::size_t sl = r.find_last_of(L'/');
    std::wstring bn = sl == std::wstring::npos ? r : r.substr(sl + 1);
    if (!src_dir.empty()) { if (const Pk2File* f = archive_->find_file(src_dir + L"/" + bn)) return f; }
    // 4) strip leading first segment (e.g. "data/") and retry from root
    std::size_t fs = r.find(L'/');
    if (fs != std::wstring::npos) { if (const Pk2File* f = archive_->find_file(r.substr(fs + 1))) return f; }
    return nullptr;
}

// For a .bsr / .cpd / .bmt, resolve its first diffuse texture and load it.
bool App::resolve_and_load_texture(const Row& r) {
    std::wstring dir = r.path.find_last_of(L'/') == std::wstring::npos ? std::wstring() : r.path.substr(0, r.path.find_last_of(L'/'));
    const Pk2File* self = archive_->find_file(r.path);
    if (!self) return false;
    auto bytes = archive_->read_file(*self);

    auto try_bmt = [&](const Pk2File* bmtFile, const std::wstring& bmtDir) -> bool {
        auto b = archive_->read_file(*bmtFile);
        JmxBmt bmt = parse_bmt(b.data(), b.size());
        for (const JmxMaterial& m : bmt.materials) {
            if (m.diffuse.empty()) continue;
            const Pk2File* tex = resolve_inner(m.diffuse, m.absolute ? std::wstring() : bmtDir);
            if (tex) { auto tb = archive_->read_file(*tex); if (load_texture_bytes(tb)) return true; }
        }
        return false;
    };

    if (r.type.category == FileCategory::Material) {
        return try_bmt(self, dir);
    }
    if (r.type.category == FileCategory::Resource) {
        JmxBsr bsr = parse_bsr(bytes.data(), bytes.size());
        for (const std::wstring& mp : bsr.material_paths) {
            const Pk2File* bmtFile = resolve_inner(mp, dir);
            if (!bmtFile) continue;
            std::wstring bmtDir = bmtFile->path.find_last_of(L'/') == std::wstring::npos ? std::wstring() : bmtFile->path.substr(0, bmtFile->path.find_last_of(L'/'));
            if (try_bmt(bmtFile, bmtDir)) return true;
        }
        return false;
    }
    if (r.type.category == FileCategory::Compound) {
        JmxCpd cpd = parse_cpd(bytes.data(), bytes.size());
        for (const std::wstring& rp : cpd.resource_paths) {
            const Pk2File* res = resolve_inner(rp, dir);
            if (!res) continue;
            Row rr; rr.path = res->path; rr.type = classify(res->name);
            if (resolve_and_load_texture(rr)) return true;
        }
        return false;
    }
    return false;
}

// Assemble a 3D model from a .bsr (or single .bms) and upload it to the viewer.
bool App::build_and_load_model(const Row& r) {
    if (!model_ready_) return false;
    const Pk2File* self = archive_->find_file(r.path);
    if (!self) return false;
    std::wstring dir = r.path.find_last_of(L'/') == std::wstring::npos ? std::wstring() : r.path.substr(0, r.path.find_last_of(L'/'));
    ModelData md;

    auto add_mesh = [&](const std::wstring& meshRef, const std::wstring& srcDir, const std::map<std::wstring, int>& matTex) {
        const Pk2File* mf = resolve_inner(meshRef, srcDir);
        if (!mf) return;
        auto mb = archive_->read_file(*mf);
        JmxMesh jm = parse_bms(mb.data(), mb.size());
        if (!jm.ok) return;
        MMesh mm;
        std::size_t vc = jm.positions.size() / 3;
        mm.verts.resize(vc);
        for (std::size_t k = 0; k < vc; ++k) {
            MVertex& v = mm.verts[k];
            v.px = jm.positions[k * 3]; v.py = jm.positions[k * 3 + 1]; v.pz = jm.positions[k * 3 + 2];
            if (k * 3 + 2 < jm.normals.size()) { v.nx = jm.normals[k * 3]; v.ny = jm.normals[k * 3 + 1]; v.nz = jm.normals[k * 3 + 2]; }
            if (k * 2 + 1 < jm.uvs.size()) { v.u = jm.uvs[k * 2]; v.v = jm.uvs[k * 2 + 1]; }
        }
        mm.idx.assign(jm.indices.begin(), jm.indices.end());
        if (jm.skinned) { mm.skin_bones = std::move(jm.skin_bones); mm.bone_idx = std::move(jm.bone_idx); mm.bone_w = std::move(jm.bone_w); }
        auto it = matTex.find(to_lower(jm.material));
        mm.tex = it != matTex.end() ? it->second : -1;
        md.meshes.push_back(std::move(mm));
    };

    std::map<std::wstring, int> matTex; // material name (lower) -> texture index
    if (r.type.category == FileCategory::Resource) {
        auto bytes = archive_->read_file(*self);
        JmxBsr bsr = parse_bsr(bytes.data(), bytes.size());
        for (const std::wstring& mp : bsr.material_paths) {
            const Pk2File* bmtF = resolve_inner(mp, dir);
            if (!bmtF) continue;
            std::wstring bmtDir = bmtF->path.find_last_of(L'/') == std::wstring::npos ? std::wstring() : bmtF->path.substr(0, bmtF->path.find_last_of(L'/'));
            auto bb = archive_->read_file(*bmtF);
            JmxBmt bmt = parse_bmt(bb.data(), bb.size());
            for (const JmxMaterial& m : bmt.materials) {
                int ti = -1;
                if (!m.diffuse.empty()) {
                    const Pk2File* tx = resolve_inner(m.diffuse, m.absolute ? std::wstring() : bmtDir);
                    if (tx) { auto tb = archive_->read_file(*tx); auto img = decode_dds(tb.data(), tb.size()); if (img) { ti = static_cast<int>(md.textures.size()); md.textures.push_back(std::move(*img)); } }
                }
                matTex[to_lower(m.name)] = ti;
            }
        }
        for (const std::wstring& mp : bsr.mesh_paths) add_mesh(mp, dir, matTex);
        // Skeleton (.bsk) + default animation (.ban) for skinned playback.
        if (!bsr.skeleton_path.empty()) {
            if (const Pk2File* sk = resolve_inner(bsr.skeleton_path, dir)) {
                auto sb = archive_->read_file(*sk);
                JmxSkeleton skel = parse_bsk(sb.data(), sb.size());
                if (skel.ok) {
                    md.bones.reserve(skel.bones.size());
                    for (const JmxBone& jb : skel.bones) {
                        MBone mb2; mb2.name = jb.name; mb2.parent = jb.parent;
                        for (int k = 0; k < 4; ++k) { mb2.rotP[k] = jb.rot_parent[k]; mb2.rotO[k] = jb.rot_origin[k]; }
                        for (int k = 0; k < 3; ++k) { mb2.transP[k] = jb.trans_parent[k]; mb2.transO[k] = jb.trans_origin[k]; }
                        md.bones.push_back(std::move(mb2));
                    }
                }
            }
        }
        if (!md.bones.empty() && !bsr.animation_paths.empty()) {
            if (const Pk2File* an = resolve_inner(bsr.animation_paths[0], dir)) {
                auto ab = archive_->read_file(*an);
                JmxAnim anim = parse_ban(ab.data(), ab.size());
                if (anim.ok) {
                    md.anim.duration = anim.duration;
                    md.anim.times = std::move(anim.times);
                    md.anim.bones.reserve(anim.bones.size());
                    for (JmxAnimBone& jab : anim.bones) {
                        MAnimBone mab; mab.name = std::move(jab.name); mab.rot = std::move(jab.rot); mab.trans = std::move(jab.trans);
                        md.anim.bones.push_back(std::move(mab));
                    }
                }
            }
        }
    } else if (r.type.category == FileCategory::Mesh) {
        add_mesh(r.path, std::wstring(), matTex);
    } else if (r.type.category == FileCategory::Compound) {
        auto bytes = archive_->read_file(*self);
        JmxCpd cpd = parse_cpd(bytes.data(), bytes.size());
        for (const std::wstring& rp : cpd.resource_paths) {
            const Pk2File* res = resolve_inner(rp, dir);
            if (!res) continue;
            Row rr; rr.path = res->path; rr.type = classify(res->name);
            if (rr.type.category == FileCategory::Resource && build_and_load_model(rr)) return true;
        }
        return false;
    }

    if (md.meshes.empty()) return false;
    // bounding box
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (const MMesh& m : md.meshes)
        for (const MVertex& v : m.verts) {
            mn[0] = std::min(mn[0], v.px); mn[1] = std::min(mn[1], v.py); mn[2] = std::min(mn[2], v.pz);
            mx[0] = std::max(mx[0], v.px); mx[1] = std::max(mx[1], v.py); mx[2] = std::max(mx[2], v.pz);
        }
    md.cx = (mn[0] + mx[0]) * 0.5f; md.cy = (mn[1] + mx[1]) * 0.5f; md.cz = (mn[2] + mx[2]) * 0.5f;
    float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
    md.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    if (md.radius < 0.001f) md.radius = 1.0f;
    model_view_.upload(md);
    return true;
}

void App::update_model_view() {
    const bool live = archive_->is_open() && !dlg_.open && !lang_open_ && !browser_.is_open();   // child windows would cover the overlay
    const bool on3d = preview_tab_ == 0 && live;
    const bool fx_skill = fxview_ready_ && preview_tab_ == kTabSkill && live && skill_playing_;
    const bool fx_vis = fxview_ready_ && ((on3d && fxview_has_) || fx_skill);
    const bool d3d_vis = model_ready_ && on3d && model_has_ && !fxview_has_;
    float barH = S(40); // reserve a strip at the top for the play / reset buttons
    float bottom = (efp_panel_ && fxview_has_) ? efp_panel_rect_.top - S(6) : prev_stage_.bottom - S(4);
    RECT rc{static_cast<LONG>(prev_stage_.left + S(4)), static_cast<LONG>(prev_stage_.top + barH),
            static_cast<LONG>(prev_stage_.right - S(4)), static_cast<LONG>(bottom)};
    if (fx_skill) rc = RECT{static_cast<LONG>(sk_view_.left), static_cast<LONG>(sk_view_.top), static_cast<LONG>(sk_view_.right), static_cast<LONG>(sk_view_.bottom)};
    // Skill search box: a themed EDIT only while the Skill tab shows.
    if (skill_edit_) {
        const bool sv = preview_tab_ == kTabSkill && live;
        RECT er{static_cast<LONG>(sk_search_.left + S(36)), static_cast<LONG>(sk_search_.top + S(9)),
                static_cast<LONG>(sk_search_.right - S(12)), static_cast<LONG>(sk_search_.bottom - S(8))};
        if (sv && !::EqualRect(&er, &skill_edit_rect_)) { ::MoveWindow(skill_edit_, er.left, er.top, er.right - er.left, er.bottom - er.top, TRUE); skill_edit_rect_ = er; }
        if (sv != skill_edit_vis_) { ::ShowWindow(skill_edit_, sv ? SW_SHOW : SW_HIDE); skill_edit_vis_ = sv; if (sv) ::SetFocus(skill_edit_); }
    }
    if (model_ready_) {
        if (d3d_vis) model_view_.set_rect(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
        if (d3d_vis != model_view_.visible()) { model_view_.show(d3d_vis); ensure_timer(); }
    }
    if (fxview_ready_) {
        if (fx_vis && !::EqualRect(&rc, &fxview_rect_)) {
            const int w = rc.right - rc.left, h = rc.bottom - rc.top;
            fxview_.move(rc.left, rc.top, w, h);
            // Rounded like the preview panel (the system owns the region after SetWindowRgn).
            if (HWND fh = fxview_.hwnd()) ::SetWindowRgn(fh, ::CreateRoundRectRgn(0, 0, w + 1, h + 1, 20, 20), TRUE);
            fxview_rect_ = rc;
        }
        if (fx_vis != fxview_vis_) {
            fxview_.show(fx_vis);
            fxview_.set_playing(fx_vis && fxview_play_);   // hidden: stop simulating
            fxview_vis_ = fx_vis;
        }
    }
}

bool App::bak_preview(const Row& r, const std::vector<std::uint8_t>& bytes, bakb::Preview& out, bool want_mesh) {
    if (bytes.empty()) return false;
    std::wstring inner = r.path;
    std::replace(inner.begin(), inner.end(), L'/', L'\\');
    // Rendered at 2x the stage and shown downscaled (linear): supersampled, clean edges.
    const int w = std::min(4096, 2 * std::max(320, static_cast<int>(prev_stage_.right - prev_stage_.left) - static_cast<int>(S(16))));
    const int h = std::min(4096, 2 * std::max(240, static_cast<int>(prev_stage_.bottom - prev_stage_.top) - static_cast<int>(S(16))));
    std::wstring err;
    try { return bakb::build_preview(archive_->assets(), inner, bytes, w, h, out, err, want_mesh); }
    catch (const std::exception& ex) { log(L"⚠ " + base_name(r.name) + L": " + utf8_to_wide(ex.what())); return false; }
}

// BAK's 3D scene into the live D3D viewer: turn it with the mouse instead of a still picture.
bool App::show_mesh3d(const bakb::Mesh3D& mesh, bool map_view) {
    if (!model_ready_ || mesh.empty()) return false;
    ModelData md;
    md.textures.reserve(mesh.textures.size());
    for (const bakb::PreviewImage& t : mesh.textures) {
        Image im; im.width = t.width; im.height = t.height; im.bgra = t.bgra; im.format = "BAK";
        md.textures.push_back(std::move(im));
    }
    for (const bakb::Mesh3D::Part& p : mesh.parts) {
        MMesh mm;
        const std::size_t n = p.pos.size() / 3;
        mm.verts.resize(n);
        for (std::size_t k = 0; k < n; ++k) {
            MVertex& v = mm.verts[k];
            v.px = p.pos[k * 3]; v.py = p.pos[k * 3 + 1]; v.pz = p.pos[k * 3 + 2];
            if (k * 3 + 2 < p.nrm.size()) { v.nx = p.nrm[k * 3]; v.ny = p.nrm[k * 3 + 1]; v.nz = p.nrm[k * 3 + 2]; }
            if (k * 2 + 1 < p.uv.size()) { v.u = p.uv[k * 2]; v.v = p.uv[k * 2 + 1]; }
            if (k * 4 + 3 < p.col.size()) { v.r = p.col[k * 4]; v.g = p.col[k * 4 + 1]; v.b = p.col[k * 4 + 2]; v.a = p.col[k * 4 + 3]; }
        }
        mm.idx = p.idx;
        mm.tex = p.tex;
        mm.alpha_test = p.alpha_test;
        mm.blend = p.layer ? MBlend::Layer : p.additive ? MBlend::Additive : p.blend ? MBlend::Alpha : MBlend::Opaque;
        md.meshes.push_back(std::move(mm));
    }
    md.cx = (mesh.min[0] + mesh.max[0]) * 0.5f; md.cy = (mesh.min[1] + mesh.max[1]) * 0.5f; md.cz = (mesh.min[2] + mesh.max[2]) * 0.5f;
    const float dx = mesh.max[0] - mesh.min[0], dy = mesh.max[1] - mesh.min[1], dz = mesh.max[2] - mesh.min[2];
    md.radius = std::max(0.001f, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz));
    if (map_view) { md.yaw0 = -2.2f; md.pitch0 = 0.62f; }   // a region: three-quarter view from above
    create_device_resources();
    model_view_.upload(md);
    return true;
}

// The terrain (.m) of the block a map file (.o / .o2 / .t) belongs to: "<folder>/<n>.m".
bool App::block_terrain(const std::wstring& path, bakb::Mesh3D& out) {
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring mpath = path.substr(0, dot) + L".m";
    if (mpath == path) return false;
    const Pk2File* mf = archive_->find_file(mpath);
    if (!mf || mf->size == 0 || mf->size > 16'000'000) return false;
    try {
        const std::vector<std::uint8_t> mb = archive_->read_file(*mf);
        Row mr; mr.path = mf->path; mr.name = mf->name; mr.type = classify(mf->name);
        bakb::Preview pv;
        if (!bak_preview(mr, mb, pv, true) || pv.mesh.empty()) return false;
        out = std::move(pv.mesh);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void App::select_row(int i) {
    selected_ = i;
    stop_sound();
    clear_texture();
    model_has_ = false;
    text_preview_.clear();
    text_is_info_ = false;
    info_edit_ = InfoEdit::None;
    goto_path_.clear();
    sel_type_name_.clear();
    safe_release(prev3d_bitmap_); prev3d_img_ = Image{};
    if (fxview_has_ || skill_playing_) { fxview_.clear(); fxview_has_ = false; skill_playing_ = false; }
    efp_panel_ = false; efp_ed_.reset(); efp_path_.clear(); efp_bytes_.clear(); efp_hits_.clear();
    if (i < 0 || i >= static_cast<int>(rows_.size())) { refresh(); return; }
    const Row& r = rows_[static_cast<std::size_t>(i)];
    if (r.is_folder) { refresh(); return; }
    sel_type_name_ = L(r.type.name_key);
    const Pk2File* f = archive_->find_file(r.path);
    if (!f) { refresh(); return; }
    FileCategory c = r.type.category;
    std::vector<std::uint8_t> bytes;
    // Text tables are read whole up to 128 MB (the client's *_all.txt tables are tens of MB);
    // other formats up to 16 MB (larger ones are navmesh / AI data: described from their size).
    if (f->size <= 16'000'000 || (is_text(c) && f->size <= 128'000'000)) {
        try { bytes = archive_->read_file(*f); }
        catch (const std::exception& ex) {
            std::wstring msg = exc_text(ex);
            log(L"⚠ " + base_name(r.name) + L": " + msg);
            // Protected payload (VP L33T key, per-asset cipher): offer BAK's recovery.
            show_confirm(L("recover_key"), base_name(r.name) + L": " + msg + L"\n\n" + L("app_recover_ask"),
                         [this, i]() { recover_keys(i); });
            refresh(); return;
        }
    }
    // BAK's preview engine: readable description for every format it knows, a texture decode for
    // the variants ours lacks, a 3D render for content no live viewer here shows, the drawn layout
    // (and tab pages) of a .2dt. Where BAK itself would fall back to a hex view, the structural
    // analysis (describe_binary) is shown instead, with BAK's notes on why the decode failed.
    const bool map_file = c == FileCategory::MapObject || c == FileCategory::MapMesh ||
                          c == FileCategory::MapNav || c == FileCategory::MapDungeon;
    auto use_bak = [&](bool want_image, bool want_3d) {
        bakb::Preview pv;
        const bool live = want_3d && model_ready_;   // 3D goes to the interactive viewer
        if (!bak_preview(r, bytes, pv, live)) return false;
        // BAK guesses "plain text" from a short head (an .exe reads as "MZ"): binary bytes are never text.
        const bool binary_guess = pv.text_guess && !is_text_bytes(bytes);
        if (!pv.text.empty() && readable_text(pv.text) && !binary_guess) text_preview_ = pv.text;
        else if (pv.generic || pv.text_guess) {
            std::wstring notes;
            // BAK's note lines, in the languages its own translation table gives them (tr, en, zh, ja, vi).
            static constexpr const wchar_t* kNotePrefixes[] = {L"Not:", L"Note:", L"备注:", L"注意:", L"備考:", L"Lưu ý:"};
            for (std::size_t a = 0; a < pv.summary.size();) {
                std::size_t e = pv.summary.find(L'\n', a);
                if (e == std::wstring::npos) e = pv.summary.size();
                const std::wstring line = pv.summary.substr(a, e - a);
                if (std::any_of(std::begin(kNotePrefixes), std::end(kNotePrefixes), [&](const wchar_t* p) { return line.starts_with(p); }))
                    notes += line + L"\n";
                a = e + 1;
            }
            text_preview_ = (notes.empty() ? L"" : notes + L"\n") + describe_binary(r.name, bytes);
            text_is_info_ = true;
        } else if (!pv.summary.empty()) { text_preview_ = pv.summary; text_is_info_ = true; }
        auto to_image = [](const bakb::PreviewImage& pi) {
            Image im; im.width = pi.width; im.height = pi.height; im.bgra = pi.bgra; im.format = "BAK"; return im;
        };
        if (live && c == FileCategory::MapObject) {
            // A region block: its objects standing on the block's own terrain (.m next to it).
            // A block without objects still shows its ground.
            bakb::Mesh3D terrain;
            const bool ground = block_terrain(r.path, terrain);
            if (pv.mesh.empty() && ground) {
                pv.mesh = std::move(terrain);
                text_preview_ = L("map_no_objects") + (text_preview_.empty() ? L"" : L"\n\n" + text_preview_);
                text_is_info_ = true;
            } else if (ground) {
                // Framed on the block itself: a wall or road running far out of it must not
                // shrink the block to a speck.
                const float tmin[3] = {terrain.min[0], terrain.min[1], terrain.min[2]};
                const float tmax[3] = {terrain.max[0], terrain.max[1], terrain.max[2]};
                merge_mesh3d(pv.mesh, std::move(terrain));
                for (int k : {0, 2}) { pv.mesh.min[k] = tmin[k]; pv.mesh.max[k] = tmax[k]; }
            }
        }
        if (live && !pv.mesh.empty()) model_has_ = show_mesh3d(pv.mesh, map_file);
        else if (pv.image.width && pv.is_3d && want_3d) { prev3d_img_ = to_image(pv.image); make_prev3d_bitmap(); }
        else if (pv.image.width && !pv.is_3d && want_image && !tex_ok_) {
            tex_img_ = to_image(pv.image); tex_ok_ = true; make_tex_bitmap();
            if (pv.pages.size() > 1) {
                for (const bakb::PreviewImage& pg : pv.pages) tex_pages_.push_back(to_image(pg));
                tex_page_labels_ = pv.page_labels;
                tex_page_ = 0;
            }
        }
        return !text_preview_.empty() || prev3d_img_.width || tex_ok_ || model_has_;
    };
    const std::wstring lname = to_lower(base_name(r.name));
    const bool is_svt = lname == L"sv.t";
    if (f->size == 0) {
        preview_tab_ = 3;
        text_preview_ = describe_binary(r.name, bytes);
        text_is_info_ = true;
        if (is_text(c)) info_edit_ = InfoEdit::Text;   // an empty .txt is still written as text
    } else if (c == FileCategory::Texture || c == FileCategory::Image) {
        preview_tab_ = 1;
        if (!load_texture_bytes(bytes)) use_bak(true, false);
        else { text_preview_ = bakb::describe(r.path, bytes); text_is_info_ = !text_preview_.empty(); }
    } else if (c == FileCategory::Resource || c == FileCategory::Compound || c == FileCategory::Mesh) {
        // BAK-2 renderer first (skinned clips, equipment, mod palettes); our D3D viewer as fallback.
        std::wstring ferr;
        fxview_has_ = fxview_ready_ && fxview_.load_model(r.path, ferr);
        if (fxview_has_) fxview_.set_playing(fxview_vis_ && fxview_play_); // else starts when the 3B tab shows it
        else model_has_ = build_and_load_model(r);
        if (c != FileCategory::Mesh) resolve_and_load_texture(r); // texture for the Doku tab too
        if (fxview_has_ || model_has_) {
            text_preview_ = bakb::describe(r.path, bytes);
            if (fxview_has_) {
                std::wstring d = fxview_.describe();
                if (!d.empty()) text_preview_ = d + (text_preview_.empty() ? L"" : L"\n\n" + text_preview_);
            }
            text_is_info_ = !text_preview_.empty();
        } else use_bak(false, true);
        preview_tab_ = (fxview_has_ || model_has_ || prev3d_img_.width) ? 0 : (tex_ok_ ? 1 : 3);
    } else if (c == FileCategory::Effect) {
        // .efp: BAK-2's bit-exact effect runtime plays it live; BAK adds the structural description.
        std::wstring ferr;
        fxview_has_ = fxview_ready_ && fxview_.load_effect(r.path, ferr);
        if (fxview_has_) { fxview_.set_playing(fxview_vis_ && fxview_play_); efp_path_ = r.path; efp_bytes_ = bytes; }
        else if (!ferr.empty()) log(L"⚠ " + base_name(r.name) + L": " + ferr);
        use_bak(false, false);
        if (fxview_has_) {
            std::wstring d = fxview_.describe();
            if (!d.empty()) { text_preview_ = d + (text_preview_.empty() ? L"" : L"\n\n" + text_preview_); text_is_info_ = true; }
        }
        preview_tab_ = fxview_has_ ? 0 : 3;
    } else if (c == FileCategory::Material) {
        preview_tab_ = 1; resolve_and_load_texture(r);
        text_preview_ = bakb::describe(r.path, bytes); text_is_info_ = !text_preview_.empty();
    } else if (c == FileCategory::Sound) {
        preview_tab_ = 2;
    } else if (bytes.empty()) {
        // Not read: too large for a preview (200+ MB navmesh / AI .dat) — described from its size.
        preview_tab_ = 3;
        text_is_info_ = true;
        text_preview_ = Lf("app_too_large", L(r.type.name_key), static_cast<double>(f->size) / (1024.0 * 1024.0));
    } else if (load_texture_bytes(bytes)) {
        preview_tab_ = 1; // image wrapped in a .dat / unknown container
    } else if (is_svt) {
        // Client version (Blowfish "SILKROAD"): decoded here, edited with the version editor.
        preview_tab_ = 3;
        text_is_info_ = true;
        info_edit_ = InfoEdit::Version;
        const std::wstring known = decode_known(r.name, bytes);
        text_preview_ = L("app_svt_title") + L"\n\n" + (known.empty() ? L("app_svt_undecoded") : known) +
                        L"\n\n" + Lf("app_svt_change_hint", L("edit_version"));
    } else if (lname == L"divisioninfo.txt" && !division_text(bytes).empty()) {
        // Server divisions and gateway hosts (binary despite .txt), edited as plain lines.
        preview_tab_ = 3;
        text_is_info_ = true;
        info_edit_ = InfoEdit::Division;
        text_preview_ = L("app_division_title") + L"\n\n" + division_text(bytes);
    } else if (lname == L"gateport.txt" && !gateport_text(bytes).empty()) {
        preview_tab_ = 3;
        text_is_info_ = true;
        info_edit_ = InfoEdit::GatePort;
        text_preview_ = L("app_gateport_title") + L"\n\n" + Lf("app_gateport_port", gateport_text(bytes));
    } else if (bytes.size() <= 4096 && (is_text(c) || is_text_bytes(bytes)) && [&] {
                   int enc = 0;
                   const std::wstring t = decode_text(bytes, enc);
                   return t.find_first_not_of(L" \t\r\n﻿") == std::wstring::npos;
               }()) {
        // A text file with no text (often just the UTF-16 BOM FF FE): empty, still edited as text.
        preview_tab_ = 3;
        text_is_info_ = true;
        info_edit_ = InfoEdit::Text;
        int enc = 0;
        (void)decode_text(bytes, enc);
        const wchar_t* encs[] = {L"ANSI", L"UTF-8", L"UTF-16LE", L"UTF-8 (BOM)", L"CP949"};
        text_preview_ = Lf("app_empty_text", encs[std::clamp(enc, 0, 4)], bytes.size());
    } else if (bak2::is_skill_enc(bytes)) {
        // skilldata_*ENC.txt: BAK-2's skill cipher -> readable rows; the editor re-encrypts on save.
        preview_tab_ = 3;
        std::vector<std::uint8_t> plain;
        if (bak2::skill_decrypt(bytes, plain)) {
            if (plain.size() > 400'000) plain.resize(400'000);
            text_preview_ = decode_text(plain, text_enc_);
            text_is_info_ = true;
            info_edit_ = InfoEdit::SkillTable;
        }
    } else if ((is_text(c) || is_text_bytes(bytes)) && [&] {
                   std::vector<std::uint8_t> tb = bytes;
                   if (tb.size() > 400'000) tb.resize(400'000);
                   text_preview_ = decode_text(tb, text_enc_);
                   if (readable_text(text_preview_)) return true;
                   text_preview_.clear();      // wrong encoding guess: never show garbage, treat as binary
                   return false;
               }()) {
        preview_tab_ = 3;
        if (bytes.size() > 400'000)
            log(Lf("app_large_table", base_name(r.name)));
        // Client data tables: BAK-2's schema validator reports problems in the activity log.
        bak2::TableCheck tc;
        if (bytes.size() <= 16'000'000 && bak2::table_validate(r.path, bytes, tc) && (tc.errors || tc.warnings)) {
            std::wstring first = tc.report.substr(0, tc.report.find(L'\n'));
            log(L"⚠ " + base_name(r.name) + L": " + first);
        }
    } else if (std::wstring rec = describe_client_record(r.name, bytes); !rec.empty()) {
        // Small client records BAK has no parser for: plugin.dat, JMXVCAMR camera, JMXVIMG1 font
        // glyphs, palettes, vssver2.scc. Glyphs and palettes open on their picture.
        text_preview_ = std::move(rec);
        text_is_info_ = true;
        if (Image pic; record_image(r.name, bytes, pic)) { tex_img_ = std::move(pic); tex_ok_ = true; create_device_resources(); make_tex_bitmap(); }
        preview_tab_ = tex_ok_ ? 1 : 3;
    } else {
        // .2dt, .dat, skeletons, animations, maps, client data ...: BAK's decoders (text, table, summary,
        // 3D, layout); the structural analysis when no decoder knows the format.
        use_bak(true, true);
        if (bakb::ui2dt_is(bytes)) {
            // NewInterface UI: BAK's summary + every widget; the drawn window is on the Doku tab.
            text_preview_ = bakb::ui2dt_listing(bytes);
            text_is_info_ = true;
            info_edit_ = InfoEdit::UiTable;
        }
        if (text_preview_.empty()) { text_preview_ = describe_binary(r.name, bytes); text_is_info_ = true; }
        preview_tab_ = (prev3d_img_.width || model_has_) ? 0 : (tex_ok_ && (info_edit_ == InfoEdit::UiTable || text_preview_.empty()) ? 1 : 3);
    }
    // A textdata split "<table>_<N>.txt" that does not read as text: point at the table's readable
    // copy in the same folder ("merged_<table>.txt" / "<table>_all.txt"), which the client loads.
    if (text_is_info_ && info_edit_ == InfoEdit::None && is_text(c) && lname.ends_with(L".txt")) {
        const std::size_t us = lname.find_last_of(L'_');
        const std::wstring stem = lname.substr(0, lname.size() - 4);
        const bool split = us != std::wstring::npos && us + 1 < stem.size() &&
                           std::all_of(stem.begin() + static_cast<std::ptrdiff_t>(us + 1), stem.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
        if (split) {
            const std::size_t slash = r.path.find_last_of(L"\\/");
            const std::wstring dir = slash == std::wstring::npos ? L"" : r.path.substr(0, slash + 1);
            const std::wstring table = stem.substr(0, us);
            const unsigned n = static_cast<unsigned>(std::wcstoul(stem.c_str() + us + 1, nullptr, 10));
            for (const std::wstring& cand : {dir + L"merged_" + table + L".txt", dir + table + L"_all.txt"})
                if (archive_->find_file(cand)) {
                    goto_path_ = cand;
                    info_edit_ = InfoEdit::Readable;
                    const std::wstring rows = split_rows_from_copy(dir, table, n, cand);
                    text_preview_ = rows.empty()
                        ? Lf("app_split_unreadable", base_name(cand)) + L"\n\n" + text_preview_
                        : rows + L"\n\n" + L("app_split_file_itself") + L"\n" + text_preview_;
                    break;
                }
        }
    }
    refresh();
}

// A split "<table>_<n>.txt" holds the IDs from the previous split's number up to n - 1. The ID column
// is the one a readable neighbour split keeps inside its own range; those rows are then taken from
// the readable copy (merged_<table>.txt / <table>_all.txt). Empty when no neighbour shows the column.
std::wstring App::split_rows_from_copy(const std::wstring& dir, const std::wstring& table, unsigned n,
                                       const std::wstring& copy) const {
    std::wstring folder_path = dir;
    if (!folder_path.empty() && (folder_path.back() == L'\\' || folder_path.back() == L'/')) folder_path.pop_back();
    const Pk2Folder* folder = archive_->find_folder(folder_path);
    if (!folder) return {};
    std::vector<std::pair<unsigned, const Pk2File*>> splits;   // every "<table>_<k>.txt" of the folder
    const std::wstring prefix = table + L"_";
    for (const Pk2File& f : folder->files) {
        const std::wstring nm = to_lower(f.name);
        if (!nm.starts_with(prefix) || !nm.ends_with(L".txt")) continue;
        const std::wstring digits = nm.substr(prefix.size(), nm.size() - prefix.size() - 4);
        if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; })) continue;
        splits.emplace_back(static_cast<unsigned>(std::wcstoul(digits.c_str(), nullptr, 10)), &f);
    }
    std::sort(splits.begin(), splits.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    auto lower_of = [&](unsigned k) {
        unsigned lo = 0;
        for (const auto& s : splits) if (s.first < k) lo = s.first;
        return lo;
    };
    auto lines_of = [&](const Pk2File& f, std::vector<std::vector<std::wstring>>& rows, std::vector<std::wstring>* raw) {
        std::vector<std::uint8_t> b;
        try { b = archive_->read_file(f); } catch (const std::exception&) { return false; }
        int enc = 0;
        const std::wstring t = decode_text(b, enc);
        if (!readable_text(t)) return false;
        for (std::size_t a = 0; a < t.size();) {
            std::size_t e = t.find(L'\n', a);
            if (e == std::wstring::npos) e = t.size();
            std::wstring line = t.substr(a, e - a);
            a = e + 1;
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (line.empty() || line.starts_with(L"//")) continue;
            std::vector<std::wstring> cols;   // the leading columns are enough to find the ID
            for (std::size_t c = 0; cols.size() < 5;) {
                const std::size_t tab = line.find(L'\t', c);
                cols.push_back(line.substr(c, tab == std::wstring::npos ? std::wstring::npos : tab - c));
                if (tab == std::wstring::npos) break;
                c = tab + 1;
            }
            rows.push_back(std::move(cols));
            if (raw) raw->push_back(std::move(line));
        }
        return true;
    };
    auto in_range = [](const std::wstring& v, unsigned lo, unsigned hi) {
        if (v.empty() || !std::all_of(v.begin(), v.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; })) return false;
        const unsigned long x = std::wcstoul(v.c_str(), nullptr, 10);
        return x >= lo && x < hi;
    };
    // The ID column, from the readable neighbours closest to n.
    int id_col = -1;
    std::vector<std::pair<unsigned, const Pk2File*>> nearest = splits;
    std::sort(nearest.begin(), nearest.end(), [n](const auto& a, const auto& b) {
        return (a.first > n ? a.first - n : n - a.first) < (b.first > n ? b.first - n : n - b.first);
    });
    for (std::size_t i = 0, tried = 0; i < nearest.size() && tried < 4 && id_col < 0; ++i) {
        if (nearest[i].first == n) continue;
        std::vector<std::vector<std::wstring>> rows;
        if (!lines_of(*nearest[i].second, rows, nullptr) || rows.empty()) continue;
        ++tried;
        const unsigned lo = lower_of(nearest[i].first), hi = nearest[i].first;
        for (int c = 0; c < 4 && id_col < 0; ++c) {
            std::size_t hits = 0;
            for (const auto& r : rows) hits += r.size() > static_cast<std::size_t>(c) && in_range(r[static_cast<std::size_t>(c)], lo, hi);
            if (hits * 10 >= rows.size() * 9) id_col = c;
        }
    }
    const Pk2File* cf = archive_->find_file(copy);
    if (id_col < 0 || !cf) return {};
    std::vector<std::vector<std::wstring>> rows;
    std::vector<std::wstring> raw;
    if (!lines_of(*cf, rows, &raw)) return {};
    const unsigned lo = lower_of(n);
    std::wstring body;
    std::size_t count = 0;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].size() > static_cast<std::size_t>(id_col) && in_range(rows[i][static_cast<std::size_t>(id_col)], lo, n)) {
            if (body.size() < 400'000) body += raw[i] + L"\n";
            ++count;
        }
    if (count == 0) return {};
    return Lf("app_split_rows_head", table + L"_" + std::to_wstring(n) + L".txt", lo, n - 1, count, base_name(copy)) +
           L"\n\n" + body;
}

// Selects `path` in the tree: clears the search, opens every folder above it, scrolls it into view.
void App::reveal_path(const std::wstring& path) {
    if (!search_.empty()) { search_.clear(); ::SetWindowTextW(search_edit_, L""); }
    for (std::size_t i = path.find_first_of(L"\\/"); i != std::wstring::npos; i = path.find_first_of(L"\\/", i + 1))
        expanded_.insert(path.substr(0, i));
    rebuild_rows();
    const std::wstring want = to_lower(path);
    for (std::size_t k = 0; k < rows_.size(); ++k)
        if (to_lower(rows_[k].path) == want) {
            scroll_ = std::max(0.0f, static_cast<float>(k) * row_h_ - (r_tree_.bottom - r_tree_.top) / 3);
            select_row(static_cast<int>(k));
            return;
        }
    refresh();
}

void App::show_tex_page(std::size_t i) {
    if (i >= tex_pages_.size()) return;
    tex_page_ = i;
    tex_img_ = tex_pages_[i];
    tex_ok_ = true;
    make_tex_bitmap();
    refresh();
}

// ---- layout ----------------------------------------------------------------
void App::layout() {
    RECT rc; ::GetClientRect(hwnd_, &rc);
    // Floor the dimensions used for layout math so rects never invert at tiny/transient
    // sizes (e.g. the maximize->restore animation), which otherwise overlaps/hides controls.
    float w = std::max(static_cast<float>(rc.right), S(900));
    float h = std::max(static_cast<float>(rc.bottom), S(560));
    cap_h_ = S(46); rail_w_ = S(206);
    float barH = S(56), stH = S(46), lgH = S(96);
    r_cap_ = D2D1::RectF(0, 0, w, cap_h_);
    r_rail_ = D2D1::RectF(0, cap_h_, rail_w_, h - stH);
    r_status_ = D2D1::RectF(0, h - stH, w, h);
    float cx0 = rail_w_, cy0 = cap_h_;
    r_sub_ = D2D1::RectF(cx0, cy0, w, cy0 + barH);
    float contentTop = cy0 + barH, contentBot = h - stH - lgH;
    r_log_ = D2D1::RectF(cx0, contentBot, w, h - stH);
    float prevW = S(548), gap = S(14), pad = S(14);
    r_prev_ = D2D1::RectF(w - pad - prevW, contentTop, w - pad, contentBot);
    r_tree_ = D2D1::RectF(cx0 + pad, contentTop, r_prev_.left - gap, contentBot);
    r_tree_head_ = D2D1::RectF(r_tree_.left, contentTop, r_tree_.right, contentTop + S(34));
    tree_right_ = r_tree_.right;
    scrollbar_x_ = r_tree_.right - S(12);
    col_mod_x_ = scrollbar_x_ - S(130);
    col_size_x_ = col_mod_x_ - S(86);
    col_type_x_ = col_size_x_ - S(118);
    row_h_ = S(30);

    // Window buttons: three orbs in a capsule at the right of the title bar (our own drawing).
    {
        const float od = S(26), ogap = S(9), oy = (cap_h_ - od) / 2;
        cap_close_ = D2D1::RectF(w - S(18) - od, oy, w - S(18), oy + od);
        cap_max_ = D2D1::RectF(cap_close_.left - ogap - od, oy, cap_close_.left - ogap, oy + od);
        cap_min_ = D2D1::RectF(cap_max_.left - ogap - od, oy, cap_max_.left - ogap, oy + od);
        cap_group_ = D2D1::RectF(cap_min_.left - S(7), oy - S(5), cap_close_.right + S(7), oy + od + S(5));
    }
    // Language button left of the window buttons; its panel of flag cards drops below it.
    cap_lang_ = D2D1::RectF(cap_group_.left - S(12) - S(82), S(8), cap_group_.left - S(12), cap_h_ - S(8));
    {
        const float cw = S(112), ch = S(112), cgap = S(10), cpad = S(18), head = S(52);
        const float pw = kLangCount * cw + (kLangCount - 1) * cgap + 2 * cpad;
        // Right-aligned with the content panels, so it covers the preview tabs below it whole.
        const float pl = std::max(S(8), w - S(14) - pw), pt = cap_h_ + S(6);
        lang_panel_ = D2D1::RectF(pl, pt, pl + pw, pt + cpad + head + ch + cpad);
        for (int i = 0; i < kLangCount; ++i) {
            const float x = pl + cpad + static_cast<float>(i) * (cw + cgap), y = pt + cpad + head;
            lang_cards_[i] = D2D1::RectF(x, y, x + cw, y + ch);
        }
    }

    float y = r_rail_.top + S(20), ih = S(42);
    for (int i = 0; i < B_COUNT; ++i) {
        if (i == B_DEFRAG) { rail_rect_[i] = D2D1::RectF(r_rail_.left + S(12), r_rail_.bottom - S(56), rail_w_ - S(12), r_rail_.bottom - S(14)); continue; }
        if (i == B_EXTRACT_ALL || i == B_ADD_FILE) y += S(16);
        rail_rect_[i] = D2D1::RectF(r_rail_.left + S(12), y, rail_w_ - S(12), y + ih);
        y += ih + S(5);
    }

    // Preview tabs as wide as their label in the current language; shrunk together when they
    // would not fit (labels never wrap).
    float ty = r_prev_.top + S(10), th = S(32), tx = r_prev_.left + S(12);
    {
        const float tgap = S(4), avail = r_prev_.right - S(12) - tx - tgap * (kTabCount - 1);
        float want[kTabCount], total = 0;
        for (int i = 0; i < kTabCount; ++i) {
            want[i] = std::max(S(84), S(28) + text_width(L(kTabKeys[i]), f_semibold_) + S(14));
            total += want[i];
        }
        const float k = total > avail ? avail / total : 1.0f;
        for (int i = 0; i < kTabCount; ++i) {
            const float tw = want[i] * k;
            tab_rect_[i] = D2D1::RectF(tx, ty, tx + tw, ty + th);
            tx += tw + tgap;
        }
    }
    float detailH = S(84);
    prev_stage_ = D2D1::RectF(r_prev_.left, ty + th + S(8), r_prev_.right, r_prev_.bottom - detailH);
    prev_detail_ = D2D1::RectF(r_prev_.left, r_prev_.bottom - detailH, r_prev_.right, r_prev_.bottom);
    btn_edit_ = D2D1::RectF(prev_stage_.right - S(220), prev_stage_.bottom - S(40), prev_stage_.right - S(118), prev_stage_.bottom - S(10));
    btn_npp_  = D2D1::RectF(prev_stage_.right - S(112), prev_stage_.bottom - S(40), prev_stage_.right - S(10), prev_stage_.bottom - S(10));
    btn_editor_save_ = D2D1::RectF(prev_stage_.right - S(220), prev_stage_.bottom - S(42), prev_stage_.right - S(118), prev_stage_.bottom - S(10));
    btn_editor_cancel_ = D2D1::RectF(prev_stage_.right - S(112), prev_stage_.bottom - S(42), prev_stage_.right - S(10), prev_stage_.bottom - S(10));
    // Skill tab: search field, result list, control strip, viewer.
    sk_search_ = D2D1::RectF(prev_stage_.left + S(10), prev_stage_.top + S(6), prev_stage_.right - S(10), prev_stage_.top + S(42));
    sk_list_ = D2D1::RectF(prev_stage_.left + S(8), sk_search_.bottom + S(8), prev_stage_.right - S(8), sk_search_.bottom + S(8) + S(28) * 6);
    {
        float by = sk_list_.bottom + S(8), bh = S(28);
        btn_sk_play_ = D2D1::RectF(prev_stage_.left + S(10), by, prev_stage_.left + S(130), by + bh);
        btn_sk_loop_ = D2D1::RectF(btn_sk_play_.right + S(8), by, btn_sk_play_.right + S(8) + S(120), by + bh);
        btn_sk_reset_ = D2D1::RectF(btn_sk_loop_.right + S(8), by, btn_sk_loop_.right + S(8) + S(140), by + bh);
        sk_view_ = D2D1::RectF(prev_stage_.left + S(4), by + bh + S(6), prev_stage_.right - S(4), prev_stage_.bottom - S(4));
    }
    efp_panel_rect_ = D2D1::RectF(prev_stage_.left + S(6), prev_stage_.bottom - S(190), prev_stage_.right - S(6), prev_stage_.bottom - S(4));
    // Description view: one action at the bottom right (structured editor, else byte editor).
    btn_info_edit_ = D2D1::RectF(prev_stage_.right - S(190), prev_stage_.bottom - S(40), prev_stage_.right - S(10), prev_stage_.bottom - S(10));
}

void App::layout_controls() {
    if (!key_edit_) return;
    RECT rc; ::GetClientRect(hwnd_, &rc);
    float w = static_cast<float>(rc.right);
    float eh = S(36), ey = cap_h_ + (S(56) - eh) / 2.0f;
    float keyW = S(150), searchW = S(236), gap = S(16);
    float searchX = w - gap - searchW;
    float keyX = searchX - S(14) - keyW;
    ::MoveWindow(search_edit_, static_cast<int>(searchX + S(34)), static_cast<int>(ey + S(8)), static_cast<int>(searchW - S(44)), static_cast<int>(eh - S(16)), TRUE);
    ::MoveWindow(key_edit_, static_cast<int>(keyX + S(34)), static_cast<int>(ey + S(8)), static_cast<int>(keyW - S(44)), static_cast<int>(eh - S(16)), TRUE);
    // The language panel and the file picker are drawn over this area: child windows would stay
    // on top of them.
    const int show = (lang_open_ || browser_.is_open()) ? SW_HIDE : SW_SHOW;
    ::ShowWindow(search_edit_, show);
    ::ShowWindow(key_edit_, show);
    if (editor_open_ && code_edit_ && !dlg_.open) ::ShowWindow(code_edit_, show);
    if (editor_open_ && code_edit_) {
        float sw = prev_stage_.right - prev_stage_.left, sh = prev_stage_.bottom - prev_stage_.top;
        if (editor_mode_ == 1) {
            // SV.T version: a small, centered single-line field — not a giant black box.
            float ew = S(200), ehv = S(36);
            ::MoveWindow(code_edit_, static_cast<int>(prev_stage_.left + (sw - ew) / 2),
                         static_cast<int>(prev_stage_.top + (sh - ehv) / 2 - S(10)),
                         static_cast<int>(ew), static_cast<int>(ehv), TRUE);
        } else {
            // Room on the right for the themed scrollbar the parent draws.
            ::MoveWindow(code_edit_, static_cast<int>(prev_stage_.left + S(8)), static_cast<int>(prev_stage_.top + S(8)),
                         static_cast<int>(sw - S(16) - S(16)), static_cast<int>(sh - S(56)), TRUE);
        }
    }
}

// ---- archive line & language -------------------------------------------------
std::wstring App::archive_status() const {
    if (!archive_->is_open()) return {};
    std::uint64_t files = 0, folders = 0;
    std::vector<const Pk2Folder*> st{&archive_->root()};
    while (!st.empty()) {
        const Pk2Folder* f = st.back(); st.pop_back();
        files += f->files.size(); folders += f->folders.size();
        for (const Pk2Folder& c : f->folders) st.push_back(&c);
    }
    std::wstring s = Lf("ui_open_status", files, folders, fmt_size(archive_->file_size()));
    if (archive_->bak_backed()) {
        s += L" · " + archive_->kind() + (archive_->read_only() ? L" " + L("ui_read_only") : std::wstring());
    } else {
        const SpaceInfo sp = archive_->space();
        s += L" · " + (sp.reclaimable() ? Lf("space_compact", fmt_size(sp.reclaimable())) : L("space_tight"));
    }
    return s;
}

int App::cap_button_at(int x, int y) const {
    auto in_orb = [&](const D2D1_RECT_F& r) {   // round hit area, a little larger than the orb
        const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, rad = (r.right - r.left) / 2 + S(3);
        const float dx = static_cast<float>(x) - cx, dy = static_cast<float>(y) - cy;
        return dx * dx + dy * dy <= rad * rad;
    };
    return in_orb(cap_min_) ? 1 : in_orb(cap_max_) ? 2 : in_orb(cap_close_) ? 3 : 0;
}

float App::text_width(const std::wstring& s, IDWriteTextFormat* f) const {
    if (!dw_ || !f || s.empty()) return 0;
    IDWriteTextLayout* tl = nullptr;
    if (FAILED(dw_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 100000.0f, 1000.0f, &tl)) || !tl) return 0;
    DWRITE_TEXT_METRICS m{};
    tl->GetMetrics(&m);
    tl->Release();
    return m.widthIncludingTrailingWhitespace;
}

int App::lang_card_at(int x, int y) const {
    if (!lang_open_) return -1;
    for (int i = 0; i < kLangCount; ++i) {
        const D2D1_RECT_F& c = lang_cards_[i];
        if (x >= c.left && x <= c.right && y >= c.top && y <= c.bottom) return i;
    }
    return -1;
}

// Switches every text at once: the table, BAK's own texts, the native controls, the archive line
// and the selected file's description (rebuilt in the new language). Remembered for next start.
void App::apply_language(Lang l) {
    lang_open_ = false; lang_hover_ = -1;
    const bool changed = l != current_language();
    set_language(l);
    bakb::set_ui_language(language_tag(l));
    save_language(l);
    if (changed) {
        ::SetWindowTextW(hwnd_, L("app_title").c_str());
        ::SendMessageW(search_edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L("search").c_str()));
        ::SendMessageW(skill_edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L("skill_search").c_str()));
        if (archive_->is_open() && !busy_) {
            status_ = archive_status();
            const int sel = selected_;
            const bool skill_shown = skill_playing_ && skill_db_ && skill_sel_ >= 0 && skill_sel_ < static_cast<int>(skill_results_.size());
            const auto skill_id = skill_shown ? skill_results_[static_cast<std::size_t>(skill_sel_)].id : decltype(skill_results_[0].id){};
            rebuild_rows();
            if (skill_shown) {   // the playing skill's description, worded again
                text_preview_ = skill_db_->describe(skill_id);
                text_is_info_ = !text_preview_.empty();
            } else if (sel >= 0 && sel < static_cast<int>(rows_.size())) {
                select_row(sel);
            }
            if (skill_db_) {     // skill names follow the language too
                skill_search();
                for (std::size_t k = 0; skill_shown && k < skill_results_.size(); ++k)
                    if (skill_results_[k].id == skill_id) { skill_sel_ = static_cast<int>(k); break; }
            }
        }
        log(Lf("lang_changed", language_name(l)));
    }
    layout_controls();
    refresh();
}

// Flags drawn from their official proportions (no image files): UK, Türkiye, China, Japan, Viet Nam.
void App::draw_flag(Lang l, D2D1_RECT_F r) {
    const float w = r.right - r.left, h = r.bottom - r.top;
    auto fill = [&](D2D1_RECT_F rc, UINT32 c) { solid_->SetColor(rgb(c)); rt_->FillRectangle(rc, solid_); };
    auto star = [&](float cx, float cy, float rad, float rot, UINT32 c) {
        if (ID2D1PathGeometry* g = star_geometry(d2d_, cx, cy, rad, rot)) { solid_->SetColor(rgb(c)); rt_->FillGeometry(g, solid_); g->Release(); }
    };
    rt_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    switch (l) {
    case Lang::en: {   // Union Jack: saltires under the St George cross
        fill(r, 0x012169);
        const D2D1_POINT_2F tl{r.left, r.top}, br{r.right, r.bottom}, tr{r.right, r.top}, bl{r.left, r.bottom};
        solid_->SetColor(rgb(0xFFFFFF));
        rt_->DrawLine(tl, br, solid_, h * 0.2f); rt_->DrawLine(tr, bl, solid_, h * 0.2f);
        solid_->SetColor(rgb(0xC8102E));
        rt_->DrawLine(tl, br, solid_, h * 0.0667f); rt_->DrawLine(tr, bl, solid_, h * 0.0667f);
        const float cx = r.left + w / 2, cy = r.top + h / 2;
        fill(D2D1::RectF(r.left, cy - h / 6, r.right, cy + h / 6), 0xFFFFFF);
        fill(D2D1::RectF(cx - h / 6, r.top, cx + h / 6, r.bottom), 0xFFFFFF);
        fill(D2D1::RectF(r.left, cy - h / 10, r.right, cy + h / 10), 0xC8102E);
        fill(D2D1::RectF(cx - h / 10, r.top, cx + h / 10, r.bottom), 0xC8102E);
        break;
    }
    case Lang::tr: {   // crescent circles 0.5G / 0.4G, 0.0625G apart; star 0.25G wide, a point toward the hoist
        fill(r, 0xE30A17);
        const float x0 = r.left, cy = r.top + h / 2;
        solid_->SetColor(rgb(0xFFFFFF)); rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x0 + 0.5f * h, cy), 0.25f * h, 0.25f * h), solid_);
        solid_->SetColor(rgb(0xE30A17)); rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x0 + 0.5625f * h, cy), 0.2f * h, 0.2f * h), solid_);
        star(x0 + 0.8958f * h, cy, 0.125f * h, 3.14159265f, 0xFFFFFF);
        break;
    }
    case Lang::zh: {   // 30 x 20 grid: big star at (5,5) r 3; small stars r 1, each pointing at it
        fill(r, 0xEE1C25);
        const float u = h / 20.0f;
        star(r.left + 5 * u, r.top + 5 * u, 3 * u, -1.5707963f, 0xFFDE00);
        const float sx[4] = {10, 12, 12, 10}, sy[4] = {2, 4, 7, 9};
        for (int i = 0; i < 4; ++i)
            star(r.left + sx[i] * u, r.top + sy[i] * u, u, std::atan2(5 - sy[i], 5 - sx[i]), 0xFFDE00);
        break;
    }
    case Lang::ja:
        fill(r, 0xFFFFFF);
        solid_->SetColor(rgb(0xBC002D));
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + w / 2, r.top + h / 2), h * 0.3f, h * 0.3f), solid_);
        break;
    case Lang::vi:
        fill(r, 0xDA251D);
        star(r.left + w / 2, r.top + h / 2, h * 0.3f, -1.5707963f, 0xFFFF00);
        break;
    }
    rt_->PopAxisAlignedClip();
    solid_->SetColor(rgb(0x000000, 0.35f));
    rt_->DrawRectangle(r, solid_, 1.0f);
}

// ---- rendering -------------------------------------------------------------
void App::render() {
    create_device_resources();
    if (!rt_) return;
    layout();
    reset_text_formats();
    // Position/hide the 3D child window BEFORE the D2D draw: switching away from the 3B Model
    // tab must hide it first, or (with WS_CLIPCHILDREN) its old rect shows a stale frame.
    update_model_view();
    RECT rc; ::GetClientRect(hwnd_, &rc);
    float W = static_cast<float>(rc.right), H = static_cast<float>(rc.bottom);
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());

    if (bg_brush_) { bg_brush_->SetEndPoint(D2D1::Point2F(0, H)); rt_->FillRectangle(D2D1::RectF(0, 0, W, H), bg_brush_); }
    else { solid_->SetColor(rgb(col::bg_bot)); rt_->FillRectangle(D2D1::RectF(0, 0, W, H), solid_); }
    if (aurora_a_) { aurora_a_->SetCenter(D2D1::Point2F(W * 0.95f, -H * 0.06f)); float r = (W + H) * 0.42f; aurora_a_->SetRadiusX(r); aurora_a_->SetRadiusY(r); rt_->FillRectangle(D2D1::RectF(0, 0, W, H), aurora_a_); }
    if (aurora_b_) { aurora_b_->SetCenter(D2D1::Point2F(-W * 0.05f, H * 1.05f)); float r = (W + H) * 0.44f; aurora_b_->SetRadiusX(r); aurora_b_->SetRadiusY(r); rt_->FillRectangle(D2D1::RectF(0, 0, W, H), aurora_b_); }

    auto fill_round = [&](D2D1_RECT_F r, float rad, UINT32 c, float a) { solid_->SetColor(rgb(c, a)); rt_->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), solid_); };
    auto stroke_round = [&](D2D1_RECT_F r, float rad, UINT32 c, float a, float wd) { solid_->SetColor(rgb(c, a)); rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), solid_, wd); };
    auto dtext = [&](const std::wstring& s, IDWriteTextFormat* fmt, D2D1_RECT_F r, UINT32 c, float a = 1.0f) {
        if (!fmt) return; solid_->SetColor(rgb(c, a)); rt_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, solid_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    // caption
    float mk = S(26), mkx = S(16), mky = (cap_h_ - mk) / 2;
    if (accent_brush_) { accent_brush_->SetStartPoint(D2D1::Point2F(mkx, mky)); accent_brush_->SetEndPoint(D2D1::Point2F(mkx + mk, mky + mk));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(mkx, mky, mkx + mk, mky + mk), S(8), S(8)), accent_brush_); }
    dtext(L"\uE7B8", f_icon_, D2D1::RectF(mkx, 0, mkx + mk, cap_h_), 0xFFFFFF);
    dtext(L("app_title"), f_title_, D2D1::RectF(mkx + mk + S(12), 0, S(320), cap_h_), col::text);
    if (archive_->is_open()) dtext(status_, f_small_, D2D1::RectF(mkx + mk + S(188), 0, cap_lang_.left - S(16), cap_h_), col::dim, 0.9f);
    {   // window buttons: three glowing orbs in a glass capsule, icons drawn as lines
        const bool zoomed = ::IsZoomed(hwnd_) != 0;
        const float gr = (cap_group_.bottom - cap_group_.top) / 2;
        fill_round(cap_group_, gr, 0xFFFFFF, 0.035f);
        stroke_round(cap_group_, gr, 0xFFFFFF, 0.08f, 1.0f);
        struct Orb { D2D1_RECT_F r; UINT32 c; int id; };
        const Orb orbs[3] = {{cap_min_, col::accent, 1}, {cap_max_, col::accent2, 2}, {cap_close_, col::red, 3}};
        for (const Orb& o : orbs) {
            const bool hv = cap_hover_ == o.id, down = cap_pressed_ == o.id && hv;
            const D2D1_POINT_2F c = D2D1::Point2F((o.r.left + o.r.right) / 2, (o.r.top + o.r.bottom) / 2);
            const float rad = (o.r.right - o.r.left) / 2 * (down ? 0.92f : 1.0f);
            if (hv) {   // soft halo
                solid_->SetColor(rgb(o.c, 0.10f)); rt_->FillEllipse(D2D1::Ellipse(c, rad + S(6), rad + S(6)), solid_);
                solid_->SetColor(rgb(o.c, 0.18f)); rt_->FillEllipse(D2D1::Ellipse(c, rad + S(3), rad + S(3)), solid_);
            }
            solid_->SetColor(rgb(o.c, hv ? (o.id == 3 ? 0.92f : 0.42f) : 0.13f));
            rt_->FillEllipse(D2D1::Ellipse(c, rad, rad), solid_);
            solid_->SetColor(rgb(o.c, hv ? 0.95f : 0.38f));
            rt_->DrawEllipse(D2D1::Ellipse(c, rad - S(0.5f), rad - S(0.5f)), solid_, S(1.0f));
            solid_->SetColor(rgb((hv && o.id == 3) ? 0xFFFFFF : hv ? col::text : o.c, hv ? 1.0f : 0.9f));
            const float a = S(4.5f), sw = S(1.7f);
            switch (o.id) {
            case 1:
                rt_->DrawLine(D2D1::Point2F(c.x - a, c.y), D2D1::Point2F(c.x + a, c.y), solid_, sw, round_cap_);
                break;
            case 2:
                if (zoomed) {   // restore: a square with another behind it
                    const float o2 = S(2.5f);
                    rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.x - a, c.y - a + o2, c.x + a - o2, c.y + a), S(1.5f), S(1.5f)), solid_, sw, round_cap_);
                    rt_->DrawLine(D2D1::Point2F(c.x - a + o2, c.y - a + o2), D2D1::Point2F(c.x - a + o2, c.y - a), solid_, sw, round_cap_);
                    rt_->DrawLine(D2D1::Point2F(c.x - a + o2, c.y - a), D2D1::Point2F(c.x + a, c.y - a), solid_, sw, round_cap_);
                    rt_->DrawLine(D2D1::Point2F(c.x + a, c.y - a), D2D1::Point2F(c.x + a, c.y + a - o2), solid_, sw, round_cap_);
                    rt_->DrawLine(D2D1::Point2F(c.x + a, c.y + a - o2), D2D1::Point2F(c.x + a - o2, c.y + a - o2), solid_, sw, round_cap_);
                } else {
                    rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.x - a, c.y - a, c.x + a, c.y + a), S(2), S(2)), solid_, sw, round_cap_);
                }
                break;
            default:
                rt_->DrawLine(D2D1::Point2F(c.x - a, c.y - a), D2D1::Point2F(c.x + a, c.y + a), solid_, sw, round_cap_);
                rt_->DrawLine(D2D1::Point2F(c.x + a, c.y - a), D2D1::Point2F(c.x - a, c.y + a), solid_, sw, round_cap_);
                break;
            }
        }
    }
    {   // language button: current flag, its tag and a chevron
        const D2D1_RECT_F& lb = cap_lang_;
        fill_round(lb, S(8), 0xFFFFFF, lang_open_ ? 0.12f : lang_btn_hover_ ? 0.08f : 0.04f);
        stroke_round(lb, S(8), lang_open_ ? col::accent : 0xFFFFFF, lang_open_ ? 0.55f : 0.08f, 1.0f);
        const float fh = S(16), fw = S(24), fy = (lb.top + lb.bottom - fh) / 2;
        draw_flag(current_language(), D2D1::RectF(lb.left + S(9), fy, lb.left + S(9) + fw, fy + fh));
        std::wstring tag = language_tag(current_language());
        for (wchar_t& c : tag) c = static_cast<wchar_t>(std::towupper(c));
        dtext(tag, f_semibold_, D2D1::RectF(lb.left + S(40), lb.top, lb.right - S(18), lb.bottom), col::text);
        dtext(lang_open_ ? L"\uE70E" : L"\uE70D", f_icon_, D2D1::RectF(lb.right - S(22), lb.top, lb.right - S(4), lb.bottom), col::subtext, 0.8f);
    }

    // rail
    fill_round(D2D1::RectF(S(6), cap_h_ + S(4), rail_w_ - S(2), r_rail_.bottom - S(2)), S(12), col::panel, 0.4f);
    bool has_open = archive_->is_open();
    bool has_sel = has_open && selected_ >= 0 && selected_ < static_cast<int>(rows_.size());
    dtext(L("ui_rail_archive"), f_small_, D2D1::RectF(r_rail_.left + S(22), r_rail_.top + S(4), rail_w_, r_rail_.top + S(18)), col::dim);
    for (int i = 0; i < B_COUNT; ++i) {
        bool enabled = button_enabled(i);
        float hv = rail_anim_[i];
        D2D1_RECT_F br = rail_rect_[i];
        UINT32 ic = enabled ? kButtons[i].accent : col::dim;
        UINT32 tc = enabled ? col::text : col::dim;
        if (i == B_DEFRAG) {
            fill_round(br, S(11), col::accent2, enabled ? (0.2f + 0.12f * hv) : 0.05f);
            stroke_round(br, S(11), col::accent2, enabled ? (0.45f + 0.3f * hv) : 0.1f, S(1.1f));
            ic = enabled ? col::accent2 : col::dim; tc = enabled ? 0xFFFFFF : col::dim;
        } else if (i == hover_rail_ && enabled) fill_round(br, S(11), 0xFFFFFF, 0.06f);
        dtext(std::wstring(1, kButtons[i].icon), f_icon_, D2D1::RectF(br.left + S(10), br.top, br.left + S(34), br.bottom), ic);
        dtext(L(kButtons[i].key), f_semibold_, D2D1::RectF(br.left + S(40), br.top, br.right - S(8), br.bottom), tc);
    }

    // sub bar
    std::wstring crumb = archive_->is_open() ? base_name(archive_->path()) : L"";
    dtext(crumb, f_semibold_, D2D1::RectF(r_sub_.left + S(16), r_sub_.top, r_sub_.left + S(360), r_sub_.bottom), col::text);
    {
        float eh = S(36), ey = cap_h_ + (S(56) - eh) / 2.0f, keyW = S(150), searchW = S(236), gap = S(16);
        float searchX = W - gap - searchW, keyX = searchX - S(14) - keyW;
        fill_round(D2D1::RectF(keyX, ey, keyX + keyW, ey + eh), S(10), col::field, 0.9f);
        stroke_round(D2D1::RectF(keyX, ey, keyX + keyW, ey + eh), S(10), 0xFFFFFF, 0.07f, 1.0f);
        dtext(L"\uE192", f_icon_, D2D1::RectF(keyX + S(8), ey, keyX + S(32), ey + eh), col::accent);
        fill_round(D2D1::RectF(searchX, ey, searchX + searchW, ey + eh), S(10), col::field, 0.9f);
        stroke_round(D2D1::RectF(searchX, ey, searchX + searchW, ey + eh), S(10), 0xFFFFFF, 0.07f, 1.0f);
        dtext(L"\uE721", f_icon_, D2D1::RectF(searchX + S(8), ey, searchX + S(32), ey + eh), col::subtext);
    }

    // tree
    fill_round(D2D1::RectF(r_tree_.left - S(2), r_tree_.top - S(2), r_tree_.right + S(2), r_tree_.bottom + S(2)), S(14), col::panel, 0.4f);
    stroke_round(D2D1::RectF(r_tree_.left - S(2), r_tree_.top - S(2), r_tree_.right + S(2), r_tree_.bottom + S(2)), S(14), 0xFFFFFF, 0.06f, 1.0f);
    dtext(L("col_name"), f_small_, D2D1::RectF(r_tree_.left + S(16), r_tree_head_.top, col_type_x_ - S(8), r_tree_head_.bottom), col::dim);
    dtext(L("col_type"), f_small_, D2D1::RectF(col_type_x_, r_tree_head_.top, col_size_x_ - S(8), r_tree_head_.bottom), col::dim);
    { AlignScope al(f_small_, DWRITE_TEXT_ALIGNMENT_TRAILING);
      dtext(L("col_size"), f_small_, D2D1::RectF(col_size_x_, r_tree_head_.top, col_mod_x_ - S(12), r_tree_head_.bottom), col::dim); }
    dtext(L("col_modified"), f_small_, D2D1::RectF(col_mod_x_, r_tree_head_.top, scrollbar_x_ - S(6), r_tree_head_.bottom), col::dim);
    solid_->SetColor(rgb(0xFFFFFF, 0.06f));
    rt_->DrawLine(D2D1::Point2F(r_tree_.left + S(8), r_tree_head_.bottom), D2D1::Point2F(r_tree_.right - S(8), r_tree_head_.bottom), solid_, 1.0f);

    D2D1_RECT_F listR = D2D1::RectF(r_tree_.left, r_tree_head_.bottom, r_tree_.right, r_tree_.bottom);
    rt_->PushAxisAlignedClip(listR, D2D1_ANTIALIAS_MODE_ALIASED);
    float listTop = listR.top, listH = listR.bottom - listR.top;
    int total = static_cast<int>(rows_.size());
    float contentH = total * row_h_;
    if (scroll_ > contentH - listH) scroll_ = std::max(0.0f, contentH - listH);
    if (scroll_ < 0) scroll_ = 0;
    int first = static_cast<int>(scroll_ / row_h_);
    int last = std::min(total - 1, first + static_cast<int>(listH / row_h_) + 1);
    if (!archive_->is_open()) { AlignScope al(f_title_, DWRITE_TEXT_ALIGNMENT_CENTER); dtext(L("drop_hint"), f_title_, listR, col::dim); }
    for (int i = first; i <= last && i >= 0; ++i) {
        const Row& r = rows_[static_cast<std::size_t>(i)];
        float y = listTop - scroll_ + i * row_h_;
        D2D1_RECT_F rr = D2D1::RectF(listR.left, y, listR.right, y + row_h_);
        if (i == selected_) { solid_->SetColor(rgb(col::accent, 0.14f)); rt_->FillRectangle(rr, solid_);
            if (accent_brush_) { accent_brush_->SetStartPoint(D2D1::Point2F(0, y)); accent_brush_->SetEndPoint(D2D1::Point2F(0, y + row_h_)); rt_->FillRectangle(D2D1::RectF(rr.left, y, rr.left + S(3), y + row_h_), accent_brush_); } }
        else if (i == hover_row_) { solid_->SetColor(rgb(0xFFFFFF, 0.045f)); rt_->FillRectangle(rr, solid_); }
        float indent = r_tree_.left + S(18) + r.depth * S(16);
        if (r.is_folder && search_.empty()) dtext(expanded_.count(r.path) ? L"\uE70D" : L"\uE76C", f_icon_, D2D1::RectF(indent - S(16), y, indent, y + row_h_), col::subtext);
        dtext(r.is_folder ? L"\uE8B7" : L"\uE7C3", f_icon_, D2D1::RectF(indent, y, indent + S(22), y + row_h_), r.type.color);
        dtext(r.name, f_ui_, D2D1::RectF(indent + S(28), y, col_type_x_ - S(8), y + row_h_), col::text, 0.97f);
        if (!r.is_folder) {
            solid_->SetColor(rgb(r.type.color, 0.95f));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(col_type_x_, y + row_h_ / 2 - S(3), col_type_x_ + S(6), y + row_h_ / 2 + S(3)), S(2), S(2)), solid_);
            dtext(L(r.type.name_key), f_small_, D2D1::RectF(col_type_x_ + S(12), y, col_size_x_ - S(8), y + row_h_), col::subtext);
            dtext(fmt_size(r.size), f_ui_right_, D2D1::RectF(col_size_x_, y, col_mod_x_ - S(12), y + row_h_), col::subtext);
            dtext(fmt_time(r.mtime), f_ui_, D2D1::RectF(col_mod_x_, y, scrollbar_x_ - S(6), y + row_h_), col::dim);
        }
    }
    rt_->PopAxisAlignedClip();
    if (contentH > listH) {
        // Track + rounded thumb (wider, hover/drag highlight) — crisp custom scrollbar.
        float tx0 = scrollbar_x_ + S(3), tx1 = scrollbar_x_ + S(11);
        fill_round(D2D1::RectF(tx0, listTop, tx1, listTop + listH), S(4), 0xFFFFFF, 0.05f);
        float thumbH = std::max(S(40), listH * (listH / contentH));
        float thumbY = listTop + (scroll_ / (contentH - listH)) * (listH - thumbH);
        float a = sb_drag_ ? 0.42f : 0.26f;
        fill_round(D2D1::RectF(tx0, thumbY, tx1, thumbY + thumbH), S(4), col::accent, a);
    }

    // preview
    fill_round(r_prev_, S(14), col::panel, 0.4f);
    stroke_round(r_prev_, S(14), 0xFFFFFF, 0.06f, 1.0f);
    const wchar_t* tabic[kTabCount] = {L"\uE809", L"\uE91B", L"\uE8D6", L"\uE8A5", L"\uE945"};
    for (int i = 0; i < kTabCount; ++i) {
        D2D1_RECT_F tr = tab_rect_[i];
        if (i == preview_tab_) { fill_round(tr, S(9), 0xFFFFFF, 0.07f); stroke_round(tr, S(9), col::accent, 0.3f, 1.0f); }
        dtext(tabic[i], f_icon_, D2D1::RectF(tr.left + S(5), tr.top, tr.left + S(27), tr.bottom), i == preview_tab_ ? col::accent : col::dim);
        dtext(L(kTabKeys[i]), f_semibold_, D2D1::RectF(tr.left + S(28), tr.top, tr.right - S(4), tr.bottom), i == preview_tab_ ? col::text : col::subtext);
    }
    rt_->PushAxisAlignedClip(prev_stage_, D2D1_ANTIALIAS_MODE_ALIASED);
    if (preview_tab_ == 1) {
        float cs = S(16);
        for (float yy = prev_stage_.top; yy < prev_stage_.bottom; yy += cs)
            for (float xx = prev_stage_.left; xx < prev_stage_.right; xx += cs) {
                bool dark = (static_cast<int>((xx - prev_stage_.left) / cs) + static_cast<int>((yy - prev_stage_.top) / cs)) % 2;
                solid_->SetColor(rgb(dark ? 0x0C111B : 0x0A0E17)); rt_->FillRectangle(D2D1::RectF(xx, yy, xx + cs, yy + cs), solid_);
            }
        if (tex_bitmap_ && tex_ok_) {
            const float badgeH = S(34);
            const bool replace = can_replace_ddj();
            const bool paged = tex_pages_.size() > 1;
            const float footH = (replace || paged) ? S(48) : 0.0f;
            if (paged) {
                // .2dt tab pages: [<] [>]  "Sekme 2/4 · <caption>"
                const float by0 = prev_stage_.bottom - S(40), by1 = prev_stage_.bottom - S(10);
                btn_page_prev_ = D2D1::RectF(prev_stage_.left + S(10), by0, prev_stage_.left + S(48), by1);
                btn_page_next_ = D2D1::RectF(btn_page_prev_.right + S(6), by0, btn_page_prev_.right + S(44), by1);
                for (const D2D1_RECT_F* b : {&btn_page_prev_, &btn_page_next_}) {
                    fill_round(*b, S(9), col::accent, 0.12f); stroke_round(*b, S(9), col::accent, 0.35f, 1.0f);
                }
                {
                    AlignScope ali(f_icon_, DWRITE_TEXT_ALIGNMENT_CENTER);
                    dtext(L"", f_icon_, btn_page_prev_, 0xCFF0FF);
                    dtext(L"", f_icon_, btn_page_next_, 0xCFF0FF);
                }
                std::wstring lbl = Lf("ui_page_of", tex_page_ + 1, tex_pages_.size());
                if (tex_page_ < tex_page_labels_.size() && !tex_page_labels_[tex_page_].empty()) lbl += L"   ·   " + tex_page_labels_[tex_page_];
                AlignScope all(f_small_, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                dtext(lbl, f_small_, D2D1::RectF(btn_page_next_.right + S(12), by0, prev_stage_.right - (replace ? S(210) : S(10)), by1), col::subtext);
            }
            float availW = prev_stage_.right - prev_stage_.left - S(16);
            float availH = prev_stage_.bottom - prev_stage_.top - S(16) - badgeH - footH;
            if (replace) {
                btn_replace_ = D2D1::RectF(prev_stage_.right - S(200), prev_stage_.bottom - S(40), prev_stage_.right - S(10), prev_stage_.bottom - S(10));
                fill_round(btn_replace_, S(9), col::accent, 0.16f); stroke_round(btn_replace_, S(9), col::accent, 0.4f, 1.0f);
                AlignScope alr(f_semibold_, DWRITE_TEXT_ALIGNMENT_CENTER);
                dtext(L("ui_replace_from_image"), f_semibold_, btn_replace_, 0xCFF0FF);
            }
            float iw = static_cast<float>(tex_img_.width), ih = static_cast<float>(tex_img_.height);
            // Native resolution: never stretch (blurry upscales look broken); only shrink a texture
            // that is larger than the stage, with linear filtering. Tiny pictures (font glyphs,
            // icons up to 64 px) get BAK-2's whole-number nearest-neighbour zoom: crisp pixels.
            float sc = std::min(1.0f, std::min(availW / iw, availH / ih));
            if (iw <= 64 && ih <= 64)
                sc = std::max(1.0f, std::min(8.0f, std::floor(std::min(availW / iw, availH / ih))));
            float dw = std::floor(iw * sc), dh = std::floor(ih * sc);
            float dx = std::floor(prev_stage_.left + (prev_stage_.right - prev_stage_.left - dw) / 2);
            float dy = std::floor(prev_stage_.top + badgeH + (prev_stage_.bottom - prev_stage_.top - badgeH - footH - dh) / 2);
            auto interp = (sc >= 1.0f) ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                       : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
            stroke_round(D2D1::RectF(dx - 1, dy - 1, dx + dw + 1, dy + dh + 1), 0, 0xFFFFFF, 0.10f, 1.0f);
            rt_->DrawBitmap(tex_bitmap_, D2D1::RectF(dx, dy, dx + dw, dy + dh), 1.0f, interp);
            wchar_t b[96];
            if (sc != 1.0f) swprintf(b, 96, L"%u × %u   %d%%", tex_img_.width, tex_img_.height, static_cast<int>(sc * 100 + 0.5f));
            else swprintf(b, 96, L"%u × %u", tex_img_.width, tex_img_.height);
            D2D1_RECT_F badge = D2D1::RectF(prev_stage_.left + S(10), prev_stage_.top + S(8), prev_stage_.right - S(10), prev_stage_.top + S(8) + S(24));
            AlignScope al(f_small_, DWRITE_TEXT_ALIGNMENT_CENTER);
            dtext(b, f_small_, badge, col::subtext);
        } else { AlignScope al(f_ui_, DWRITE_TEXT_ALIGNMENT_CENTER); dtext(L("no_preview"), f_ui_, prev_stage_, col::dim); }
    } else if (preview_tab_ == 3) {
        if (editor_open_) {
            if (editor_mode_ != 1 && code_edit_) {
                // Themed scrollbar beside the editor (same look as the file list).
                RECT er; ::GetWindowRect(code_edit_, &er);
                POINT tl{er.left, er.top}, br{er.right, er.bottom};
                ::ScreenToClient(hwnd_, &tl); ::ScreenToClient(hwnd_, &br);
                ed_bar_ = D2D1::RectF(static_cast<float>(br.x) + S(4), static_cast<float>(tl.y), static_cast<float>(br.x) + S(12), static_cast<float>(br.y));
                RECT cr; ::GetClientRect(code_edit_, &cr);
                int ed_lines = static_cast<int>(::SendMessageW(code_edit_, EM_GETLINECOUNT, 0, 0));
                int ed_vis = std::max(1, static_cast<int>(cr.bottom) / code_line_h_);
                fill_round(ed_bar_, S(4), 0xFFFFFF, 0.05f);
                if (ed_lines > ed_vis) {
                    int ed_first = static_cast<int>(::SendMessageW(code_edit_, EM_GETFIRSTVISIBLELINE, 0, 0));
                    float barH = ed_bar_.bottom - ed_bar_.top;
                    float thumbH = std::max(S(30), barH * static_cast<float>(ed_vis) / static_cast<float>(ed_lines));
                    float thumbY = ed_bar_.top + (barH - thumbH) * std::clamp(static_cast<float>(ed_first) / static_cast<float>(ed_lines - ed_vis), 0.0f, 1.0f);
                    fill_round(D2D1::RectF(ed_bar_.left, thumbY, ed_bar_.right, thumbY + thumbH), S(4), col::accent, ed_sb_drag_ ? 0.42f : 0.26f);
                }
            }
            AlignScope al(f_semibold_, DWRITE_TEXT_ALIGNMENT_CENTER);
            fill_round(btn_editor_save_, S(9), col::green, 0.2f); stroke_round(btn_editor_save_, S(9), col::green, 0.4f, 1.0f);
            dtext(L("save_to_pk2"), f_semibold_, btn_editor_save_, 0xBFF5DB);
            fill_round(btn_editor_cancel_, S(9), 0xFFFFFF, 0.06f); dtext(L("ui_cancel"), f_semibold_, btn_editor_cancel_, col::subtext);
        } else if (text_is_info_) {
            // File description / listing as one block, centered in the stage when it fits,
            // otherwise starting at the top-left (never clipped in the middle).
            D2D1_RECT_F tr = D2D1::RectF(prev_stage_.left + S(16), prev_stage_.top + S(10), prev_stage_.right - S(12), prev_stage_.bottom - S(52));
            UINT32 n = static_cast<UINT32>(std::min<std::size_t>(text_preview_.size(), 20000));
            IDWriteTextLayout* tl = nullptr;
            if (dw_ && f_mono_) dw_->CreateTextLayout(text_preview_.c_str(), n, f_mono_, tr.right - tr.left, tr.bottom - tr.top, &tl);
            if (tl) {
                DWRITE_TEXT_METRICS m{};
                tl->GetMetrics(&m);
                float bw = m.widthIncludingTrailingWhitespace, bh = m.height;
                float ox = tr.left + std::max(0.0f, std::floor((tr.right - tr.left - bw) / 2));
                float oy = tr.top + std::max(0.0f, std::floor((tr.bottom - tr.top - bh) / 2));
                solid_->SetColor(rgb(0xB9C6DB));
                rt_->PushAxisAlignedClip(tr, D2D1_ANTIALIAS_MODE_ALIASED);
                rt_->DrawTextLayout(D2D1::Point2F(ox, oy), tl, solid_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                rt_->PopAxisAlignedClip();
                tl->Release();
            }
            if (info_action_visible()) {
                AlignScope al(f_semibold_, DWRITE_TEXT_ALIGNMENT_CENTER);
                const bool structured = info_edit_ != InfoEdit::None;
                const char* key = info_edit_ == InfoEdit::Version ? "edit_version"
                                : info_edit_ == InfoEdit::UiTable ? "edit_table"
                                : info_edit_ == InfoEdit::Readable ? "open_readable"
                                : structured ? "edit" : "edit_hex";
                fill_round(btn_info_edit_, S(9), col::accent, structured ? 0.16f : 0.10f);
                stroke_round(btn_info_edit_, S(9), col::accent, structured ? 0.4f : 0.3f, 1.0f);
                dtext(L(key), f_semibold_, btn_info_edit_, 0xCFF0FF);
            }
        } else if (!text_preview_.empty()) {
            solid_->SetColor(rgb(0xCBD5E6));
            D2D1_RECT_F tr = D2D1::RectF(prev_stage_.left + S(14), prev_stage_.top + S(10), prev_stage_.right - S(10), prev_stage_.bottom - S(50));
            rt_->DrawTextW(text_preview_.c_str(), static_cast<UINT32>(std::min<std::size_t>(text_preview_.size(), 20000)), f_mono_, tr, solid_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            AlignScope al(f_semibold_, DWRITE_TEXT_ALIGNMENT_CENTER);
            fill_round(btn_edit_, S(9), col::accent, 0.16f); stroke_round(btn_edit_, S(9), col::accent, 0.4f, 1.0f); dtext(L("edit"), f_semibold_, btn_edit_, 0xCFF0FF);
            if (!notepadpp_exe().empty()) { fill_round(btn_npp_, S(9), 0xFFFFFF, 0.06f); dtext(L"Notepad++", f_semibold_, btn_npp_, col::subtext); }
        } else { AlignScope al(f_ui_, DWRITE_TEXT_ALIGNMENT_CENTER); dtext(L("no_preview"), f_ui_, prev_stage_, col::dim); }
    } else if (preview_tab_ == 2) {
        float cx = (prev_stage_.left + prev_stage_.right) / 2;
        float cy = (prev_stage_.top + prev_stage_.bottom) / 2;
        float rad = S(46);
        btn_audio_ = D2D1::RectF(cx - rad, cy - rad, cx + rad, cy + rad);
        bool has_snd = selected_ >= 0 && !rows_[static_cast<std::size_t>(selected_)].is_folder;
        // glow disc
        solid_->SetColor(rgb(col::accent2, 0.14f));
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad + S(10), rad + S(10)), solid_);
        solid_->SetColor(rgb(audio_playing_ ? col::green : col::accent, has_snd ? 0.9f : 0.3f));
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), solid_);
        dtext(audio_playing_ ? L"" /*stop*/ : L"" /*play*/, f_icon_,
              D2D1::RectF(cx - rad, cy - rad, cx + rad, cy + rad), 0x0A0E17);
        AlignScope al1(f_semibold_, DWRITE_TEXT_ALIGNMENT_CENTER);
        AlignScope al2(f_small_, DWRITE_TEXT_ALIGNMENT_CENTER);
        std::wstring nm = has_snd ? base_name(rows_[static_cast<std::size_t>(selected_)].name) : L("sel_file");
        dtext(nm, f_semibold_,
              D2D1::RectF(prev_stage_.left + S(8), cy + rad + S(16), prev_stage_.right - S(8), cy + rad + S(40)), col::text);
        dtext(audio_playing_ ? L("ui_playing") : L("ui_press_to_listen"),
              f_small_, D2D1::RectF(prev_stage_.left + S(8), cy + rad + S(40), prev_stage_.right - S(8), cy + rad + S(60)), col::subtext);
    } else if (preview_tab_ == kTabSkill) {
        // Skill browser: BAK-2 Catalog search + live skillplay in the viewer below.
        fill_round(sk_search_, S(10), col::field, 0.9f);
        stroke_round(sk_search_, S(10), 0xFFFFFF, 0.07f, 1.0f);
        dtext(L"", f_icon_, D2D1::RectF(sk_search_.left + S(8), sk_search_.top, sk_search_.left + S(32), sk_search_.bottom), col::subtext);
        D2D1_RECT_F below = D2D1::RectF(prev_stage_.left, sk_search_.bottom, prev_stage_.right, prev_stage_.bottom);
        if (!skill_db_) {
            AlignScope al(f_ui_, DWRITE_TEXT_ALIGNMENT_CENTER);
            dtext(skill_db_tried_ ? L("ui_skill_no_data") : L("ui_skill_loading"), f_ui_, below, col::dim);
        } else {
            fill_round(sk_list_, S(10), col::panel, 0.55f);
            const float rh = S(28), skListH = sk_list_.bottom - sk_list_.top;
            const float skContentH = static_cast<float>(skill_results_.size()) * rh;
            skill_scroll_ = std::clamp(skill_scroll_, 0.0f, std::max(0.0f, skContentH - skListH));
            rt_->PushAxisAlignedClip(sk_list_, D2D1_ANTIALIAS_MODE_ALIASED);
            int firstRow = static_cast<int>(skill_scroll_ / rh);
            for (int k = firstRow; k < static_cast<int>(skill_results_.size()); ++k) {
                float yy = sk_list_.top - skill_scroll_ + static_cast<float>(k) * rh;
                if (yy > sk_list_.bottom) break;
                D2D1_RECT_F rr = D2D1::RectF(sk_list_.left, yy, sk_list_.right, yy + rh);
                if (k == skill_sel_) { solid_->SetColor(rgb(col::accent, 0.16f)); rt_->FillRectangle(rr, solid_); }
                else if (k == skill_hover_) { solid_->SetColor(rgb(0xFFFFFF, 0.05f)); rt_->FillRectangle(rr, solid_); }
                const bak2::SkillItem& it = skill_results_[static_cast<std::size_t>(k)];
                dtext(it.name, f_ui_, D2D1::RectF(rr.left + S(12), yy, rr.right - S(230), yy + rh), col::text);
                AlignScope ar(f_small_, DWRITE_TEXT_ALIGNMENT_TRAILING);
                dtext(it.code + L"  #" + std::to_wstring(it.id), f_small_, D2D1::RectF(rr.right - S(330), yy, rr.right - S(14), yy + rh), col::dim);
            }
            if (skill_results_.empty()) {
                AlignScope al(f_small_, DWRITE_TEXT_ALIGNMENT_CENTER);
                dtext(L("ui_no_match"), f_small_, sk_list_, col::dim);
            }
            rt_->PopAxisAlignedClip();
            if (skContentH > skListH) {
                float tx0 = sk_list_.right - S(8), tx1 = sk_list_.right - S(3);
                float thumbH = std::max(S(24), skListH * (skListH / skContentH));
                float thumbY = sk_list_.top + (skill_scroll_ / (skContentH - skListH)) * (skListH - thumbH);
                fill_round(D2D1::RectF(tx0, thumbY, tx1, thumbY + thumbH), S(3), col::accent, 0.3f);
            }
            // Control strip.
            AlignScope al(f_semibold_, DWRITE_TEXT_ALIGNMENT_CENTER);
            const bool on = skill_playing_ && fxview_play_;
            fill_round(btn_sk_play_, S(8), on ? col::green : col::accent, on ? 0.22f : 0.16f);
            stroke_round(btn_sk_play_, S(8), on ? col::green : col::accent, 0.45f, 1.0f);
            dtext(on ? L("ui_pause") : L("ui_play"), f_semibold_, btn_sk_play_, on ? 0xBFF5DB : 0xCFF0FF);
            fill_round(btn_sk_loop_, S(8), skill_loop_ ? col::accent2 : 0xFFFFFF, skill_loop_ ? 0.2f : 0.06f);
            dtext(skill_loop_ ? L("ui_loop_on") : L("ui_loop_off"), f_semibold_, btn_sk_loop_, skill_loop_ ? 0xE9DDFF : col::subtext);
            fill_round(btn_sk_reset_, S(8), 0xFFFFFF, 0.06f);
            dtext(L("ui_reset_view"), f_semibold_, btn_sk_reset_, col::subtext);
            if (!skill_playing_) {
                fill_round(sk_view_, S(12), 0x0A0E17, 0.6f);
                dtext(L("ui_skill_pick_hint"), f_semibold_, sk_view_, col::dim);
            }
        }
    } else { // 3B Model
        if (model_has_ || fxview_has_) {
            // Top control strip (the 3D child window sits below it, so these stay visible/clickable).
            float bx = prev_stage_.left + S(10), by = prev_stage_.top + S(6), bw = S(120), bh = S(28);
            btn_rotate_ = D2D1::RectF(bx, by, bx + bw, by + bh);
            btn_resetview_ = D2D1::RectF(bx + bw + S(10), by, bx + bw + S(10) + S(130), by + bh);
            // D3D viewer: spin toggle. BAK-2 viewer: play / pause of the clip or effect.
            bool on = fxview_has_ ? fxview_play_ : model_view_.auto_rotate();
            const std::wstring& label = fxview_has_ ? (on ? L("ui_pause") : L("ui_play")) : (on ? L("ui_stop") : L("ui_rotate"));
            fill_round(btn_rotate_, S(8), on ? col::green : col::accent, on ? 0.22f : 0.16f);
            stroke_round(btn_rotate_, S(8), on ? col::green : col::accent, 0.45f, 1.0f);
            AlignScope al(f_semibold_, DWRITE_TEXT_ALIGNMENT_CENTER);
            dtext(label, f_semibold_, btn_rotate_, on ? 0xBFF5DB : 0xCFF0FF);
            fill_round(btn_resetview_, S(8), 0xFFFFFF, 0.06f);
            dtext(L("ui_reset_view"), f_semibold_, btn_resetview_, col::subtext);
            if (fxview_has_ && !efp_path_.empty()) {
                // .efp: BAK-2 efpwrite editor panel toggle.
                btn_efpedit_ = D2D1::RectF(btn_resetview_.right + S(10), by, btn_resetview_.right + S(10) + S(130), by + bh);
                fill_round(btn_efpedit_, S(8), col::accent2, efp_panel_ ? 0.26f : 0.14f);
                stroke_round(btn_efpedit_, S(8), col::accent2, 0.45f, 1.0f);
                dtext(efp_panel_ ? L("ui_efp_close_edit") : L("ui_efp_edit"), f_semibold_, btn_efpedit_, 0xE9DDFF);
            }
            if (efp_panel_ && efp_ed_ && fxview_has_) {
                D2D1_RECT_F pr = efp_panel_rect_;
                fill_round(pr, S(12), 0x0E1420, 0.96f);
                stroke_round(pr, S(12), col::accent2, 0.3f, 1.0f);
                efp_hits_.clear();
                const float rowH = S(30), colW = (pr.right - pr.left - S(24)) / 2;
                auto stepper = [&](int idx, float x, float y) {
                    const EfpParamDef& d = kEfpParams[idx];
                    double v = efp_p_.*(d.field);
                    dtext(L(d.key), f_small_, D2D1::RectF(x, y, x + S(92), y + rowH), col::subtext);
                    D2D1_RECT_F minus = D2D1::RectF(x + S(94), y + S(3), x + S(120), y + rowH - S(3));
                    D2D1_RECT_F val = D2D1::RectF(minus.right + S(2), y, minus.right + S(74), y + rowH);
                    D2D1_RECT_F plus = D2D1::RectF(val.right + S(2), y + S(3), val.right + S(28), y + rowH - S(3));
                    fill_round(minus, S(6), 0xFFFFFF, 0.08f); fill_round(plus, S(6), 0xFFFFFF, 0.08f);
                    dtext(L"−", f_semibold_, minus, col::text); dtext(L"+", f_semibold_, plus, col::text);
                    wchar_t b[32];
                    if (d.degrees) swprintf(b, 32, L"%+.0f°", v); else swprintf(b, 32, L"×%.2f", v);
                    const bool changed = d.degrees ? (v != 0.0) : (std::fabs(v - 1.0) > 1e-9);
                    dtext(b, f_semibold_, val, changed ? 0xCFF0FF : col::text);
                    efp_hits_.push_back({idx, -1, minus});
                    efp_hits_.push_back({idx, +1, plus});
                };
                float x0 = pr.left + S(12), x1 = pr.left + S(12) + colW, y0 = pr.top + S(8);
                for (int k = 0; k < 3; ++k) stepper(k, x0, y0 + rowH * static_cast<float>(k));
                for (int k = 3; k < 7; ++k) stepper(k, x1, y0 + rowH * static_cast<float>(k - 3));
                // Toggles + actions.
                auto toggle = [&](D2D1_RECT_F r, bool on, const std::wstring& label) {
                    fill_round(r, S(7), on ? col::green : 0xFFFFFF, on ? 0.22f : 0.06f);
                    stroke_round(r, S(7), on ? col::green : 0xFFFFFF, on ? 0.5f : 0.12f, 1.0f);
                    AlignScope ac(f_small_, DWRITE_TEXT_ALIGNMENT_CENTER);
                    dtext(label, f_small_, r, on ? 0xBFF5DB : col::subtext);
                };
                float ty = y0 + rowH * 3 + S(2);
                btn_efp_colorize_ = D2D1::RectF(x0, ty, x0 + S(110), ty + S(26));
                toggle(btn_efp_colorize_, efp_p_.colorize, L("ui_efp_colorize"));
                float ay = pr.bottom - S(40);
                btn_efp_recolor_ = D2D1::RectF(x0, ay, x0 + S(170), ay + S(30));
                toggle(btn_efp_recolor_, efp_p_.recolor_textures, L("ui_efp_recolor"));
                btn_efp_save_ = D2D1::RectF(pr.right - S(12) - S(110), ay, pr.right - S(12), ay + S(30));
                btn_efp_reset_ = D2D1::RectF(btn_efp_save_.left - S(8) - S(100), ay, btn_efp_save_.left - S(8), ay + S(30));
                fill_round(btn_efp_reset_, S(8), 0xFFFFFF, 0.07f); dtext(L("ui_reset"), f_semibold_, btn_efp_reset_, col::subtext);
                const bool can_save = !archive_->read_only() && efp_ed_->modified();
                fill_round(btn_efp_save_, S(8), col::green, can_save ? 0.24f : 0.06f);
                stroke_round(btn_efp_save_, S(8), col::green, can_save ? 0.5f : 0.15f, 1.0f);
                dtext(L("ui_efp_save"), f_semibold_, btn_efp_save_, can_save ? 0xBFF5DB : col::dim);
            }
        } else if (prev3d_bitmap_) {
            // Static 3D render from BAK's preview engine (.nvm / .bsk / .ban / map objects ...).
            D2D1_SIZE_U bs = prev3d_bitmap_->GetPixelSize();
            float availW = prev_stage_.right - prev_stage_.left - S(16), availH = prev_stage_.bottom - prev_stage_.top - S(16);
            float sc = std::min(1.0f, std::min(availW / static_cast<float>(bs.width), availH / static_cast<float>(bs.height)));
            float dw = std::floor(static_cast<float>(bs.width) * sc), dh = std::floor(static_cast<float>(bs.height) * sc);
            float dx = std::floor(prev_stage_.left + (prev_stage_.right - prev_stage_.left - dw) / 2);
            float dy = std::floor(prev_stage_.top + (prev_stage_.bottom - prev_stage_.top - dh) / 2);
            rt_->DrawBitmap(prev3d_bitmap_, D2D1::RectF(dx, dy, dx + dw, dy + dh), 1.0f,
                            sc >= 1.0f ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            AlignScope al(f_ui_, DWRITE_TEXT_ALIGNMENT_CENTER);
            dtext(L("ui_no_3d_model"), f_ui_, prev_stage_, col::dim);
        }
    }
    rt_->PopAxisAlignedClip();
    solid_->SetColor(rgb(0xFFFFFF, 0.06f));
    rt_->DrawLine(D2D1::Point2F(prev_detail_.left + S(12), prev_detail_.top), D2D1::Point2F(prev_detail_.right - S(12), prev_detail_.top), solid_, 1.0f);
    if (has_sel) {
        const Row& r = rows_[static_cast<std::size_t>(selected_)];
        auto kv = [&](const std::wstring& k, const std::wstring& v, float yy) {
            dtext(k, f_small_, D2D1::RectF(prev_detail_.left + S(16), yy, prev_detail_.left + S(130), yy + S(20)), col::subtext);
            AlignScope al(f_semibold_, DWRITE_TEXT_ALIGNMENT_TRAILING);
            dtext(v, f_semibold_, D2D1::RectF(prev_detail_.left + S(130), yy, prev_detail_.right - S(16), yy + S(20)), col::text);
        };
        float yy = prev_detail_.top + S(10);
        kv(L("col_name"), base_name(r.name), yy); yy += S(23);
        kv(L("col_type"), r.is_folder ? L("type_unknown") : L(r.type.name_key), yy); yy += S(23);
        if (!r.is_folder) { kv(L("col_size"), fmt_size(r.size), yy); yy += S(23); kv(L("col_modified"), fmt_time(r.mtime), yy); }
    } else dtext(L("sel_file"), f_small_, D2D1::RectF(prev_detail_.left + S(16), prev_detail_.top + S(10), prev_detail_.right, prev_detail_.top + S(30)), col::dim);

    // activity log
    fill_round(D2D1::RectF(r_log_.left + S(12), r_log_.top + S(2), W - S(14), r_log_.bottom - S(2)), S(12), col::panel, 0.4f);
    dtext(L"\uE81C", f_icon_, D2D1::RectF(r_log_.left + S(22), r_log_.top + S(6), r_log_.left + S(44), r_log_.top + S(26)), col::accent2);
    dtext(L("log"), f_small_, D2D1::RectF(r_log_.left + S(46), r_log_.top + S(6), r_log_.left + S(300), r_log_.top + S(26)), col::subtext);
    {
        float ly = r_log_.top + S(28), lineH = S(16);
        int maxLines = static_cast<int>((r_log_.bottom - S(6) - ly) / lineH);
        int startIdx = std::max(0, static_cast<int>(log_.size()) - maxLines);
        for (int i = startIdx; i < static_cast<int>(log_.size()); ++i) { dtext(log_[static_cast<std::size_t>(i)], f_small_, D2D1::RectF(r_log_.left + S(24), ly, W - S(24), ly + lineH), col::subtext, 0.85f); ly += lineH; }
    }

    // status / progress
    if (busy_) {
        std::uint64_t d = prog_done_.load(), t = prog_total_.load();
        double pct = t ? (100.0 * static_cast<double>(d) / static_cast<double>(t)) : 0.0;
        std::wstring cur; { std::lock_guard<std::mutex> lk(prog_mtx_); cur = prog_current_; }
        double secs = static_cast<double>(::GetTickCount64() - op_start_) / 1000.0;
        double fps = secs > 0.2 ? static_cast<double>(d) / secs : 0.0;
        float px0 = S(16), px1 = W - S(380), py = r_status_.top + (r_status_.bottom - r_status_.top - S(9)) / 2;
        fill_round(D2D1::RectF(px0, py, px1, py + S(9)), S(5), 0xFFFFFF, 0.08f);
        float fillw = (px1 - px0) * static_cast<float>(pct / 100.0);
        if (fillw > S(3) && accent_brush_) {
            accent_brush_->SetStartPoint(D2D1::Point2F(px0, 0)); accent_brush_->SetEndPoint(D2D1::Point2F(px1, 0));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(px0, py, px0 + fillw, py + S(9)), S(5), S(5)), accent_brush_);
            float sh = px0 + fillw * (0.5f + 0.5f * std::sin(anim_phase_ * 0.07f));
            solid_->SetColor(rgb(0xFFFFFF, 0.3f)); rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sh - S(12), py, sh + S(12), py + S(9)), S(5), S(5)), solid_);
        }
        wchar_t buf[320]; swprintf(buf, 320, L"%.1f%%   %llu / %llu   %.0f/s   %.1fs   %s", pct, static_cast<unsigned long long>(d), static_cast<unsigned long long>(t), fps, secs, cur.c_str());
        AlignScope al(f_small_, DWRITE_TEXT_ALIGNMENT_TRAILING);
        dtext(buf, f_small_, D2D1::RectF(px1 + S(10), r_status_.top, W - S(70), r_status_.bottom), col::text);
        dtext(L"Esc", f_small_, D2D1::RectF(W - S(60), r_status_.top, W - S(16), r_status_.bottom), col::dim);
    } else dtext(status_.empty() ? L("ready") : status_, f_small_, D2D1::RectF(S(16), r_status_.top, W - S(16), r_status_.bottom), col::subtext);

    // context menu
    if (ctx_open_ && !ctx_items_.empty()) {
        float mw = S(206), ih = S(34), pad = S(6);
        int visible = 0, seps = 0; for (auto& it : ctx_items_) { if (it.sep) ++seps; else ++visible; }
        float mh = pad * 2 + visible * ih + seps * S(11);
        float mx = std::min(ctx_x_, W - mw - S(8)), my = std::min(ctx_y_, H - mh - S(8));
        D2D1_RECT_F mr = D2D1::RectF(mx, my, mx + mw, my + mh);
        solid_->SetColor(rgb(0x0C1220, 0.98f)); rt_->FillRoundedRectangle(D2D1::RoundedRect(mr, S(12), S(12)), solid_);
        stroke_round(mr, S(12), 0xFFFFFF, 0.12f, 1.0f);
        float yy = my + pad; int idx = 0;
        for (auto& it : ctx_items_) {
            if (it.sep) { solid_->SetColor(rgb(0xFFFFFF, 0.07f)); rt_->FillRectangle(D2D1::RectF(mx + S(10), yy + S(5), mx + mw - S(10), yy + S(6)), solid_); yy += S(11); continue; }
            D2D1_RECT_F ir = D2D1::RectF(mx + S(6), yy, mx + mw - S(6), yy + ih);
            if (idx == ctx_hover_) { solid_->SetColor(rgb(0xFFFFFF, 0.07f)); rt_->FillRoundedRectangle(D2D1::RoundedRect(ir, S(8), S(8)), solid_); }
            dtext(L(it.key), f_ui_, D2D1::RectF(ir.left + S(12), yy, ir.right - S(10), yy + ih), it.danger ? col::red : col::text);
            yy += ih; ++idx;
        }
    }

    // In-app file picker (modal), under a message it may raise.
    if (browser_.is_open()) {
        browser_.layout(D2D1::RectF(0, cap_h_, W, H), scale_);   // the title bar stays usable
        browser_.draw(browser_theme());
        reset_text_formats();
    }

    // In-app dialog (modal): dims the window, centred card, wrapped text, themed buttons.
    if (dlg_.open) {
        solid_->SetColor(rgb(0x000000, 0.55f)); rt_->FillRectangle(D2D1::RectF(0, 0, W, H), solid_);
        float cw = std::min(W - S(40), S(480));
        IDWriteTextLayout* tl = nullptr;
        if (dw_ && f_ui_) dw_->CreateTextLayout(dlg_.text.c_str(), static_cast<UINT32>(dlg_.text.size()), f_ui_, cw - S(48), H, &tl);
        float th = S(20);
        if (tl) {
            tl->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            tl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); th = std::min(m.height, H - S(220));
        }
        float ch = S(60) + th + S(76);
        D2D1_RECT_F card = D2D1::RectF((W - cw) / 2, (H - ch) / 2, (W + cw) / 2, (H + ch) / 2);
        solid_->SetColor(rgb(0x0F1522, 0.99f)); rt_->FillRoundedRectangle(D2D1::RoundedRect(card, S(14), S(14)), solid_);
        stroke_round(card, S(14), dlg_.confirm ? col::accent2 : col::accent, 0.45f, 1.2f);
        dtext(dlg_.confirm ? L"" : L"", f_icon_, D2D1::RectF(card.left + S(18), card.top + S(14), card.left + S(42), card.top + S(44)), dlg_.confirm ? col::accent2 : col::accent);
        dtext(dlg_.title, f_title_, D2D1::RectF(card.left + S(48), card.top + S(14), card.right - S(20), card.top + S(44)), col::text);
        if (tl) {
            solid_->SetColor(rgb(col::text, 0.92f));
            rt_->PushAxisAlignedClip(D2D1::RectF(card.left + S(24), card.top + S(56), card.right - S(24), card.top + S(56) + th), D2D1_ANTIALIAS_MODE_ALIASED);
            rt_->DrawTextLayout(D2D1::Point2F(card.left + S(24), card.top + S(56)), tl, solid_);
            rt_->PopAxisAlignedClip();
            tl->Release();
        }
        float by = card.bottom - S(50), bh = S(34), bw = S(112);
        dlg_.ok = D2D1::RectF(card.right - S(20) - bw, by, card.right - S(20), by + bh);
        dlg_.cancel = dlg_.confirm ? D2D1::RectF(dlg_.ok.left - S(10) - bw, by, dlg_.ok.left - S(10), by + bh) : D2D1::RectF(0, 0, 0, 0);
        AlignScope al(f_semibold_, DWRITE_TEXT_ALIGNMENT_CENTER);
        fill_round(dlg_.ok, S(9), dlg_.confirm ? col::red : col::accent, 0.22f); stroke_round(dlg_.ok, S(9), dlg_.confirm ? col::red : col::accent, 0.5f, 1.0f);
        dtext(dlg_.confirm ? L("ui_yes") : L("ui_ok"), f_semibold_, dlg_.ok, col::text);
        if (dlg_.confirm) { fill_round(dlg_.cancel, S(9), 0xFFFFFF, 0.07f); dtext(L("ui_dlg_cancel"), f_semibold_, dlg_.cancel, col::subtext); }
    }

    // Language picker: a card per language, its flag and its name (in itself, and in the current
    // language below when that differs).
    if (lang_open_) {
        solid_->SetColor(rgb(0x000000, 0.35f)); rt_->FillRectangle(D2D1::RectF(0, cap_h_, W, H), solid_);
        const D2D1_RECT_F& p = lang_panel_;
        solid_->SetColor(rgb(0x000000, 0.35f));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(p.left + S(3), p.top + S(5), p.right + S(3), p.bottom + S(7)), S(16), S(16)), solid_);
        solid_->SetColor(rgb(0x0F1522, 0.99f)); rt_->FillRoundedRectangle(D2D1::RoundedRect(p, S(16), S(16)), solid_);
        stroke_round(p, S(16), col::accent, 0.35f, 1.2f);
        dtext(L"", f_icon_, D2D1::RectF(p.left + S(16), p.top + S(14), p.left + S(40), p.top + S(40)), col::accent);
        dtext(L("lang_title"), f_title_, D2D1::RectF(p.left + S(46), p.top + S(12), p.right - S(18), p.top + S(42)), col::text);
        dtext(L("lang_hint"), f_small_, D2D1::RectF(p.left + S(46), p.top + S(38), p.right - S(18), p.top + S(62)), col::subtext);
        for (int i = 0; i < kLangCount; ++i) {
            const Lang l = kLangs[i];
            const D2D1_RECT_F& c = lang_cards_[i];
            const bool cur = l == current_language(), hov = i == lang_hover_;
            fill_round(c, S(12), cur ? col::accent : 0xFFFFFF, cur ? 0.16f : hov ? 0.09f : 0.04f);
            stroke_round(c, S(12), cur ? col::accent : 0xFFFFFF, cur ? 0.75f : hov ? 0.3f : 0.08f, cur ? 1.6f : 1.0f);
            const float fw = S(72), fh = S(48), fx = (c.left + c.right - fw) / 2, fy = c.top + S(12);
            draw_flag(l, D2D1::RectF(fx, fy, fx + fw, fy + fh));
            if (cur) {
                const D2D1_RECT_F badge = D2D1::RectF(c.right - S(24), c.top + S(6), c.right - S(6), c.top + S(24));
                solid_->SetColor(rgb(col::accent)); rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F((badge.left + badge.right) / 2, (badge.top + badge.bottom) / 2), S(9), S(9)), solid_);
                dtext(L"", f_icon_, badge, 0x0B1019);
            }
            AlignScope a1(f_semibold_, DWRITE_TEXT_ALIGNMENT_CENTER);
            AlignScope a2(f_small_, DWRITE_TEXT_ALIGNMENT_CENTER);
            const std::wstring native = language_name(l);
            const std::wstring here = L(kLangNameKey[i]);
            dtext(native, f_semibold_, D2D1::RectF(c.left + S(4), fy + fh + S(6), c.right - S(4), fy + fh + S(28)), col::text, cur ? 1.0f : 0.92f);
            if (here != native)
                dtext(here, f_small_, D2D1::RectF(c.left + S(4), fy + fh + S(26), c.right - S(4), fy + fh + S(44)), col::subtext);
        }
    }

    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) discard_device_resources();

    if (model_ready_ && model_view_.visible()) model_view_.render();
}

// ---- hit testing -----------------------------------------------------------
int App::row_at(int x, int y) const {
    if (x < r_tree_.left || x > r_tree_.right) return -1; // only inside the tree panel
    if (y < r_tree_head_.bottom || y >= r_tree_.bottom) return -1;
    int idx = static_cast<int>((y - r_tree_head_.bottom + scroll_) / row_h_);
    if (idx < 0 || idx >= static_cast<int>(rows_.size())) return -1;
    return idx;
}
int App::rail_at(int x, int y) const {
    for (int i = 0; i < B_COUNT; ++i) { const auto& r = rail_rect_[i]; if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return i; }
    return -1;
}
int App::tab_at(int x, int y) const {
    for (int i = 0; i < kTabCount; ++i) { const auto& r = tab_rect_[i]; if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return i; }
    return -1;
}
void App::toggle_expand(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const Row& r = rows_[static_cast<std::size_t>(row)];
    if (!r.is_folder) return;
    if (expanded_.count(r.path)) expanded_.erase(r.path); else expanded_.insert(r.path);
    rebuild_rows(); refresh();
}
// One rule for drawing a rail button enabled and for letting a click run it.
bool App::button_enabled(int i) const {
    if (busy_) return false;
    const bool has_open = archive_->is_open();
    const bool has_sel = has_open && selected_ >= 0 && selected_ < static_cast<int>(rows_.size());
    switch (i) {
    case B_OPEN: case B_NEW: return true;
    case B_EXTRACT_SEL: return has_sel;
    case B_DELETE: return has_sel && !archive_->read_only();
    case B_ADD_FILE: case B_ADD_FOLDER: case B_DEFRAG: return has_open && !archive_->read_only();
    default: return has_open;
    }
}

void App::run_button(int id) {
    switch (id) {
    case B_OPEN: do_open(); break; case B_NEW: do_new(); break;
    case B_EXTRACT_ALL: do_extract_all(); break; case B_EXTRACT_SEL: do_extract_sel(); break;
    case B_ADD_FILE: do_add_files(); break; case B_ADD_FOLDER: do_add_folder(); break;
    case B_DELETE: do_delete(); break; case B_DEFRAG: do_defrag(); break;
    default: break;
    }
}

// ---- helpers / logging -----------------------------------------------------
void App::log(const std::wstring& msg) { log_.push_back(now_hhmmss() + L"   " + msg); while (log_.size() > 200) log_.pop_front(); refresh(); }
void App::set_status(const std::wstring& s) { status_ = s; refresh(); }
void App::ensure_timer() {
    bool want = busy_;
    for (int i = 0; i < B_COUNT; ++i) { float t = (i == hover_rail_) ? 1.0f : 0.0f; if (std::fabs(rail_anim_[i] - t) > 0.01f) want = true; }
    if (model_ready_ && model_view_.visible()) want = true;
    if (want && !timer_on_) { ::SetTimer(hwnd_, 1, 16, nullptr); timer_on_ = true; }
    else if (!want && timer_on_) { ::KillTimer(hwnd_, 1); timer_on_ = false; }
}

// ---- Notepad++ / editor ----------------------------------------------------
void App::edit_in_notepadpp(const Row& r, const std::vector<std::uint8_t>* content, bool reencrypt) {
    const Pk2File* f = archive_->find_file(r.path);
    if (!f || notepadpp_exe().empty()) return;
    std::vector<std::uint8_t> bytes;
    if (content) bytes = *content;
    else {
        try { bytes = archive_->read_file(*f); } catch (const std::exception& ex) { show_message(exc_text(ex)); return; }
    }
    if (tmp_dir_.empty()) { wchar_t tp[MAX_PATH]; ::GetTempPathW(MAX_PATH, tp); tmp_dir_ = std::wstring(tp) + L"SroPk2Studio\\"; ::CreateDirectoryW(tmp_dir_.c_str(), nullptr); }
    std::wstring tmp = tmp_dir_ + base_name(r.name);
    HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &w, nullptr); ::CloseHandle(h); }
    Watch wt; wt.temp = tmp; wt.inner = r.path; wt.reencrypt = reencrypt;
    WIN32_FILE_ATTRIBUTE_DATA fa{}; ::GetFileAttributesExW(tmp.c_str(), GetFileExInfoStandard, &fa); wt.t = fa.ftLastWriteTime;
    for (auto it = watches_.begin(); it != watches_.end();) { if (it->inner == wt.inner) it = watches_.erase(it); else ++it; }
    watches_.push_back(wt);
    const std::wstring exe = notepadpp_exe();
    ::ShellExecuteW(hwnd_, L"open", exe.c_str(), (L"\"" + tmp + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    ::SetTimer(hwnd_, 2, 800, nullptr);
    log(L("open_npp") + L": " + base_name(r.name));
}
void App::poll_watches() {
    for (auto& w : watches_) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!::GetFileAttributesExW(w.temp.c_str(), GetFileExInfoStandard, &fa)) continue;
        if (::CompareFileTime(&fa.ftLastWriteTime, &w.t) > 0) {
            w.t = fa.ftLastWriteTime;
            HANDLE h = ::CreateFileW(w.temp.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            LARGE_INTEGER li; ::GetFileSizeEx(h, &li);
            std::vector<std::uint8_t> data(static_cast<std::size_t>(li.QuadPart));
            DWORD got = 0; std::size_t off = 0;
            while (off < data.size()) { DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - off, 0x4000'0000)); if (!::ReadFile(h, data.data() + off, want, &got, nullptr) || got == 0) break; off += got; }
            ::CloseHandle(h);
            if (w.reencrypt) data = bak2::skill_encrypt(data);   // decrypted skill table: cipher back on
            if (archive_->is_open()) { archive_->add_file(w.inner, data); archive_->reload(); rebuild_rows(); if (selected_ >= 0) select_row(selected_); log(L"\u2714 " + Lf("ui_pk2_updated", base_name(w.inner))); }
        }
    }
}
// Opens the editor that fits the file: SV.T version, .2dt widget table, encrypted skill table,
// plain text; the byte editor only for formats without a structured one.
void App::open_inapp_editor(const Row& r) {
    const Pk2File* f = archive_->find_file(r.path);
    if (!f) return;
    std::vector<std::uint8_t> bytes;
    try { bytes = archive_->read_file(*f); } catch (const std::exception& ex) { show_message(exc_text(ex)); return; }
    if (to_lower(base_name(r.name)) == L"sv.t") { open_svt_editor(r); return; }
    if (bakb::ui2dt_is(bytes)) {
        open_editor(r.path, 3, bakb::ui2dt_table(bytes), 0);
        log(L("ui_2dt_hint"));
        return;
    }
    const std::wstring lname = to_lower(base_name(r.name));
    if (lname == L"divisioninfo.txt") {
        if (std::wstring t = division_text(bytes); !t.empty()) {
            open_editor(r.path, 5, t, 0);
            log(L("ui_division_hint"));
            return;
        }
    }
    if (lname == L"gateport.txt") {
        if (std::wstring t = gateport_text(bytes); !t.empty()) { open_editor(r.path, 6, t, 5); return; }
    }
    if (bak2::is_skill_enc(bytes)) {
        std::vector<std::uint8_t> plain;
        if (!bak2::skill_decrypt(bytes, plain)) { show_message(L("ui_skill_decrypt_fail")); return; }
        if (plain.size() > 2'000'000 && !notepadpp_exe().empty()) { edit_in_notepadpp(r, &plain, true); return; }   // big: decrypted copy in Notepad++
        int enc = 0;
        open_editor(r.path, 4, decode_text(plain, enc), 0);
        log(L("ui_skill_enc_hint"));
        return;
    }
    if (bytes.size() > 2'000'000 && !notepadpp_exe().empty()) { edit_in_notepadpp(r); return; }
    std::wstring text = decode_text(bytes, text_enc_);
    const bool blank = text.find_first_not_of(L" \t\r\n﻿") == std::wstring::npos;   // empty text file
    // Binary content (or text with embedded NULs) would be cut / garbled in a text editor: hex instead.
    if (!blank && (!readable_text(text) || text.find(L'\0') != std::wstring::npos)) { open_hex_editor(r); return; }
    open_editor(r.path, 0, text, 0);
}
void App::open_svt_editor(const Row& r) {
    const Pk2File* f = archive_->find_file(r.path);
    if (!f) return;
    auto bytes = archive_->read_file(*f);
    std::wstring ver;
    if (bytes.size() >= 12) {
        std::uint32_t len; std::memcpy(&len, bytes.data(), 4);
        if (len == 8) {
            const std::uint8_t k8[8] = {'S','I','L','K','R','O','A','D'};
            Blowfish bf; bf.initialize(std::span<const std::uint8_t>(k8, 8));
            auto d = bf.decode(std::span<const std::uint8_t>(bytes.data() + 4, 8));
            std::string s; for (auto c : d) if (c >= 0x20 && c <= 0x7E) s.push_back(static_cast<char>(c));
            ver = utf8_to_wide(s);
        }
    }
    open_editor(r.path, 1, ver, 8);
    log(L("ui_svt_hint"));
}

void App::save_inapp_editor() {
    if (!code_edit_ || !archive_->is_open()) return;
    int len = ::GetWindowTextLengthW(code_edit_);
    std::wstring w(static_cast<std::size_t>(len) + 1, L'\0');
    ::GetWindowTextW(code_edit_, w.data(), len + 1);
    w.resize(static_cast<std::size_t>(len));

    if (editor_mode_ == 1) {
        // SV.T: re-encode the typed version into the 8-byte blowfish buffer at offset 4.
        const Pk2File* f = archive_->find_file(editor_inner_);
        if (!f) { close_inapp_editor(); return; }
        auto orig = archive_->read_file(*f);
        std::string ver = wide_to_utf8(w);
        std::uint8_t plain[8] = {0};
        for (std::size_t i = 0; i < ver.size() && i < 8; ++i) plain[i] = static_cast<std::uint8_t>(ver[i]);
        const std::uint8_t k8[8] = {'S','I','L','K','R','O','A','D'};
        Blowfish bf; bf.initialize(std::span<const std::uint8_t>(k8, 8));
        auto ct = bf.encode(std::span<const std::uint8_t>(plain, 8));
        if (orig.size() >= 12 && ct.size() >= 8) {
            std::memcpy(orig.data() + 4, ct.data(), 8);
            try {
                archive_->add_file(editor_inner_, orig);
                archive_->reload(); rebuild_rows();
                log(L"\u2714 " + Lf("ui_svt_saved", w));
            } catch (const std::exception& ex) {
                show_message(exc_text(ex));
            }
        }
        close_inapp_editor();
        if (selected_ >= 0) select_row(selected_);
        return;
    }

    if (editor_mode_ >= 3 && editor_mode_ <= 6) {
        // 3: .2dt widget table written back into the records; 4: skill table encrypted again (BAK-2);
        // 5: DIVISIONINFO.TXT rebuilt from its lines; 6: GATEPORT.TXT port.
        const Pk2File* f = archive_->find_file(editor_inner_);
        if (!f) { close_inapp_editor(); return; }
        std::vector<std::uint8_t> out;
        std::wstring note;
        try {
            if (editor_mode_ == 3) {
                out = archive_->read_file(*f);
                std::size_t changed = 0;
                std::wstring err;
                if (!bakb::ui2dt_apply(out, w, changed, err)) { show_message(err, L("edit_table")); return; }
                if (changed == 0) { close_inapp_editor(); log(L("ui_no_changes")); return; }
                note = Lf("ui_fields_changed", changed);
            } else if (editor_mode_ == 4) {
                out = bak2::skill_encrypt(encode_text(w, kEncUtf16));
                note = L("ui_encrypted");
            } else {
                std::wstring err;
                const bool ok = editor_mode_ == 5 ? division_bytes(w, out, err)
                                                  : gateport_bytes(w, archive_->read_file(*f), out, err);
                if (!ok) { show_message(err, base_name(editor_inner_)); return; }
                note = Lf("ui_n_bytes", out.size());
            }
            archive_->add_file(editor_inner_, out);
            archive_->reload(); rebuild_rows();
            log(L"✔ " + base_name(editor_inner_) + L" → pk2   (" + note + L")");
        } catch (const std::exception& ex) { show_message(exc_text(ex)); return; }
        close_inapp_editor();
        for (std::size_t k = 0; k < rows_.size(); ++k) if (rows_[k].path == editor_inner_) { select_row(static_cast<int>(k)); break; }
        return;
    }

    if (editor_mode_ == 2) {
        std::vector<std::uint8_t> out; std::wstring err;
        if (!parse_hex_editable(w, out, err)) { show_message(err); return; }
        try {
            archive_->add_file(editor_inner_, out);
            archive_->reload(); rebuild_rows();
            log(L"✔ " + base_name(editor_inner_) + L" → pk2   (" + Lf("ui_n_bytes", out.size()) + L", hex)");
        } catch (const std::exception& ex) { show_message(exc_text(ex)); return; }
        close_inapp_editor();
        for (std::size_t k = 0; k < rows_.size(); ++k) if (rows_[k].path == editor_inner_) { select_row(static_cast<int>(k)); break; }
        return;
    }

    auto bytes = encode_text(w, text_enc_);
    try {
        // What changed, in BAK-2's diff format (first line = counts) for the activity log.
        std::wstring before;
        if (const Pk2File* of = archive_->find_file(editor_inner_)) before = decode_text(archive_->read_file(*of), text_enc_);
        std::wstring diff = bak2::text_diff(before, w);
        archive_->add_file(editor_inner_, bytes);
        archive_->reload(); rebuild_rows();
        log(L"\u2714 " + base_name(editor_inner_) + L" \u2192 pk2   " + diff.substr(0, diff.find(L'\n')));
    } catch (const std::exception& ex) {
        show_message(exc_text(ex));
        return;
    }
    close_inapp_editor();
    if (selected_ >= 0) select_row(selected_);
}
void App::close_inapp_editor() { editor_open_ = false; ed_sb_drag_ = false; if (code_edit_) ::ShowWindow(code_edit_, SW_HIDE); refresh(); }

// The EDIT repaints itself on keys / wheel / clicks; the parent must redraw the custom scrollbar.
LRESULT CALLBACK App::code_edit_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CONTEXTMENU) { g_app->edit_context_menu(h, l); return 0; }
    LRESULT r = ::CallWindowProcW(g_app->edit_proc_, h, m, w, l);
    switch (m) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_MOUSEWHEEL: case WM_LBUTTONUP:
    case WM_VSCROLL: case EM_LINESCROLL: case EM_SCROLL: case WM_SETTEXT: case WM_PASTE: case WM_CUT: case WM_UNDO:
        g_app->refresh(); break;
    case WM_MOUSEMOVE: if (w & MK_LBUTTON) g_app->refresh(); break;
    default: break;
    }
    return r;
}

constexpr const wchar_t* kFieldProcProp = L"SroPk2Studio.FieldProc";

LRESULT CALLBACK App::field_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    const auto orig = reinterpret_cast<WNDPROC>(::GetPropW(h, kFieldProcProp));
    if (m == WM_CONTEXTMENU && g_app) { g_app->edit_context_menu(h, l); return 0; }
    if (m == WM_NCDESTROY) ::RemovePropW(h, kFieldProcProp);
    return orig ? ::CallWindowProcW(orig, h, m, w, l) : ::DefWindowProcW(h, m, w, l);
}

void App::subclass_field(HWND h) {
    if (!h || ::GetPropW(h, kFieldProcProp)) return;
    const LONG_PTR orig = ::SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&App::field_proc));
    ::SetPropW(h, kFieldProcProp, reinterpret_cast<HANDLE>(orig));
}

void App::edit_context_menu(HWND edit, LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (pt.x == -1 && pt.y == -1) {   // Shift+F10 / menu key: open at the field
        RECT r; ::GetWindowRect(edit, &r); pt = POINT{r.left + 12, r.bottom};
    }
    ::SetFocus(edit);
    DWORD s = 0, e = 0;
    ::SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&s), reinterpret_cast<LPARAM>(&e));
    const bool sel = e > s;
    const bool ro = (::GetWindowLongW(edit, GWL_STYLE) & ES_READONLY) != 0;
    const bool can_undo = ::SendMessageW(edit, EM_CANUNDO, 0, 0) != 0;
    const bool can_paste = ::IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
    const bool has_text = ::GetWindowTextLengthW(edit) > 0;
    const std::vector<PopupItem> items = {
        {L("edit_undo"), L"Ctrl+Z", 1, can_undo && !ro, false},
        {{}, {}, 0, true, true},
        {L("edit_cut"), L"Ctrl+X", 2, sel && !ro, false},
        {L("edit_copy"), L"Ctrl+C", 3, sel, false},
        {L("edit_paste"), L"Ctrl+V", 4, can_paste && !ro, false},
        {L("edit_delete"), L"Del", 5, sel && !ro, false},
        {{}, {}, 0, true, true},
        {L("edit_select_all"), L"Ctrl+A", 6, has_text, false},
    };
    switch (run_popup_menu(hwnd_, pt, items, scale_, d2d_, dw_)) {
    case 1: ::SendMessageW(edit, WM_UNDO, 0, 0); break;
    case 2: ::SendMessageW(edit, WM_CUT, 0, 0); break;
    case 3: ::SendMessageW(edit, WM_COPY, 0, 0); break;
    case 4: ::SendMessageW(edit, WM_PASTE, 0, 0); break;
    case 5: ::SendMessageW(edit, WM_CLEAR, 0, 0); break;
    case 6: ::SendMessageW(edit, EM_SETSEL, 0, -1); break;
    default: break;
    }
    refresh();
}

// One editor window for every mode; no native scroll bars (the parent draws a themed one).
void App::open_editor(const std::wstring& inner, int mode, const std::wstring& content, std::size_t limit) {
    editor_inner_ = inner;
    editor_mode_ = mode;
    if (!code_edit_) {
        code_edit_ = ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL | ES_WANTRETURN,
                                       0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CODE_EDIT)), hinst_, nullptr);
        edit_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(code_edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&App::code_edit_proc)));
    }
    ::SendMessageW(code_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(code_font_), TRUE);
    ::SendMessageW(code_edit_, EM_SETLIMITTEXT, limit, 0);   // 0 = no practical limit (multiline)
    ::SetWindowTextW(code_edit_, content.c_str());
    {   // line height of the editor font, for the scrollbar maths
        HDC dc = ::GetDC(code_edit_); HGDIOBJ old = ::SelectObject(dc, code_font_);
        TEXTMETRICW tm{}; ::GetTextMetricsW(dc, &tm); ::SelectObject(dc, old); ::ReleaseDC(code_edit_, dc);
        code_line_h_ = std::max<int>(1, tm.tmHeight + tm.tmExternalLeading);
    }
    editor_open_ = true; preview_tab_ = 3;
    ::ShowWindow(code_edit_, dlg_.open ? SW_HIDE : SW_SHOW);
    ::SetFocus(code_edit_);
    layout_controls(); refresh();
}

void App::open_hex_editor(const Row& r) {
    const Pk2File* f = archive_->find_file(r.path);
    if (!f) return;
    if (f->size > 4'000'000) { show_message(Lf("ui_hex_too_big", base_name(r.name))); return; }
    std::vector<std::uint8_t> bytes;
    try { bytes = archive_->read_file(*f); } catch (const std::exception& ex) { show_message(exc_text(ex)); return; }
    open_editor(r.path, 2, make_hex_editable(bytes), 0);
    log(L("ui_hex_hint"));
}

// Scroll the editor so that the scrollbar thumb centre follows y.
void App::editor_scroll_to(float y) {
    if (!code_edit_) return;
    RECT cr; ::GetClientRect(code_edit_, &cr);
    int total = static_cast<int>(::SendMessageW(code_edit_, EM_GETLINECOUNT, 0, 0));
    int vis = std::max(1, static_cast<int>(cr.bottom) / code_line_h_);
    if (total <= vis) return;
    float barH = ed_bar_.bottom - ed_bar_.top;
    float thumbH = std::max(S(30), barH * static_cast<float>(vis) / static_cast<float>(total));
    float frac = std::clamp((y - ed_bar_.top - thumbH / 2) / std::max(1.0f, barH - thumbH), 0.0f, 1.0f);
    int target = static_cast<int>(frac * static_cast<float>(total - vis) + 0.5f);
    int first = static_cast<int>(::SendMessageW(code_edit_, EM_GETFIRSTVISIBLELINE, 0, 0));
    ::SendMessageW(code_edit_, EM_LINESCROLL, 0, target - first);
    refresh();
}

void App::show_message(const std::wstring& text, const std::wstring& title) {
    dlg_.open = true; dlg_.confirm = false;
    dlg_.title = title.empty() ? L("error") : title;
    dlg_.text = text;
    dlg_.on_yes = nullptr;
    if (code_edit_) ::ShowWindow(code_edit_, SW_HIDE);   // child windows would sit above the overlay
    refresh();
}
void App::show_confirm(const std::wstring& title, const std::wstring& text, std::function<void()> on_yes) {
    show_message(text, title);
    dlg_.confirm = true;
    dlg_.on_yes = std::move(on_yes);
}
void App::close_dialog(bool yes) {
    if (!dlg_.open) return;
    dlg_.open = false;
    std::function<void()> act = std::move(dlg_.on_yes);
    dlg_.on_yes = nullptr;
    if (editor_open_ && code_edit_) ::ShowWindow(code_edit_, SW_SHOW);
    refresh();
    if (yes && act) act();
}

// ---- async op --------------------------------------------------------------
void App::start_op(Op op, std::function<void()> work) {
    if (busy_) return;
    cancel_ = false; prog_done_ = 0; prog_total_ = 0;
    { std::lock_guard<std::mutex> lk(prog_mtx_); prog_current_.clear(); }
    result_msg_.clear(); result_ok_ = false;
    cur_op_ = op; op_start_ = ::GetTickCount64(); busy_ = true;
    ensure_timer(); refresh();
    worker_ = std::thread([this, work]() {
        try { work(); result_ok_ = true; }
        catch (const Pk2AuthError&) { result_ok_ = false; std::lock_guard<std::mutex> lk(prog_mtx_); result_msg_ = L("wrong_key"); }
        catch (const std::exception& ex) { result_ok_ = false; std::lock_guard<std::mutex> lk(prog_mtx_); result_msg_ = exc_text(ex); }
        busy_ = false; ::PostMessageW(hwnd_, WM_APP_DONE, 0, 0);
    });
}
void App::finish_op() {
    if (worker_.joinable()) worker_.join();
    if (cur_op_ == Op::OpenDeep) {
        std::wstring err; { std::lock_guard<std::mutex> lk(prog_mtx_); err = result_msg_; }
        busy_ = false; cur_op_ = Op::None;
        if (result_ok_ && pending_store_ && pending_store_->is_open()) {
            if (fxview_has_) { fxview_.clear(); fxview_has_ = false; }
            archive_ = std::move(pending_store_);
            after_open(pending_path_);
        } else {
            pending_store_.reset();
            show_message((err.empty() ? L("ui_key_not_found") : err), L("wrong_key"));
        }
        ensure_timer(); refresh();
        return;
    }
    if (cur_op_ == Op::Recover) {
        std::wstring msg; bool ok; { std::lock_guard<std::mutex> lk(prog_mtx_); msg = result_ok_ ? recover_msg_ : result_msg_; ok = recover_ok_; }
        busy_ = false; cur_op_ = Op::None;
        log(L("recover_key") + (ok ? L": ✔" : L": ✖"));
        show_message(msg.empty() ? (ok ? L("ui_key_recovered") : L("ui_no_encrypted_asset")) : msg, L("recover_key"));
        if (ok && recover_row_ >= 0 && recover_row_ < static_cast<int>(rows_.size())) select_row(recover_row_);
        ensure_timer(); refresh();
        return;
    }
    double secs = static_cast<double>(::GetTickCount64() - op_start_) / 1000.0;
    bool mutating = (cur_op_ == Op::AddFiles || cur_op_ == Op::AddFolder || cur_op_ == Op::Defrag);
    if (mutating && archive_->is_open()) { try { archive_->reload(); } catch (...) {} }
    rebuild_rows();
    std::wstring err; { std::lock_guard<std::mutex> lk(prog_mtx_); err = result_msg_; }
    std::wstring done_line;
    if (!result_ok_) log(L("error") + L": " + err);
    else {
        if (cur_op_ == Op::Defrag) {
            const std::uint64_t after = archive_->is_open() ? archive_->file_size() : 0;
            const std::uint64_t saved = op_size_before_ > after ? op_size_before_ - after : 0;
            done_line = archive_->bak_backed()
                ? Lf("ui_op_done_saved", fmt_size(saved), secs)
                : Lf("compact_result", fmt_size(op_size_before_), fmt_size(after), fmt_size(saved), secs);
        } else done_line = Lf("ui_op_done_items", static_cast<unsigned long long>(prog_done_.load()), secs);
        log(done_line);
        if (archive_->is_open()) status_ = archive_status();
    }
    busy_ = false; cur_op_ = Op::None; ensure_timer(); refresh();
}

// ---- file picker -----------------------------------------------------------
// The app's own picker drawn over the window: child windows (text fields, 3D views) are hidden
// while it is open, the keyboard goes to the main window, and the choice is handed on from the
// message loop once the picker has closed.
void App::browse(ui::BrowseRequest request, Picked done) {
    if (browser_.is_open()) return;
    // First time for a kind: start next to the open archive.
    if (request.start_dir.empty() && archive_->is_open()) {
        const std::wstring& ap = archive_->path();
        const std::size_t cut = ap.find_last_of(L"\\/");
        if (cut != std::wstring::npos) request.start_dir = ap.substr(0, cut);
    }
    ctx_open_ = false;
    if (lang_open_) { lang_open_ = false; lang_hover_ = -1; }
    browse_done_ = std::move(done);
    browser_.open(std::move(request), [this](std::vector<std::wstring> paths) {
        browse_result_ = std::move(paths);
        ::PostMessageW(hwnd_, WM_APP_BROWSED, 0, 0);
    });
    ::SetFocus(hwnd_);
    layout_controls();
    update_model_view();
    refresh();
}

ui::Theme App::browser_theme() const {
    ui::Theme t;
    t.d2d = d2d_; t.dw = dw_; t.rt = rt_; t.brush = solid_;
    t.ui = f_ui_; t.semibold = f_semibold_; t.title = f_title_; t.icon = f_icon_; t.note = f_small_;
    t.round_cap = round_cap_; t.scale = scale_;
    t.panel = col::panel; t.field = col::field; t.text = col::text; t.subtext = col::subtext; t.dim = col::dim;
    t.accent = col::accent; t.accent2 = col::accent2; t.green = col::green; t.red = col::red; t.folder = col::folder;
    return t;
}

bool App::can_replace_ddj() const {
    if (busy_ || !tex_ok_ || archive_->read_only() || selected_ < 0 || selected_ >= static_cast<int>(rows_.size())) return false;
    const Row& r = rows_[static_cast<std::size_t>(selected_)];
    if (r.is_folder) return false;
    std::wstring ln = to_lower(r.name);
    return ln.size() > 4 && (ln.substr(ln.size() - 4) == L".ddj" || ln.substr(ln.size() - 4) == L".dds");
}

// Picture file -> .ddj with BAK-2's encoder (keeps the replaced file's format / mips / header),
// written into the archive (the engine backs the archive up before its first change).
void App::replace_ddj_from_image() {
    const std::wstring inner = rows_[static_cast<std::size_t>(selected_)].path;
    ui::BrowseRequest req;
    req.mode = ui::BrowseMode::OpenFile;
    req.kind = "image";
    req.title = L("ui_replace_from_image");
    req.filters = {{"ui_filter_images", {L"png", L"bmp", L"tga", L"dds", L"ddj", L"jpg", L"jpeg", L"gif", L"tif", L"tiff"}},
                   {"ui_filter_all", {}}};
    browse(std::move(req), [this, inner](std::vector<std::wstring> paths) { replace_ddj_with(inner, paths.front()); });
}
void App::replace_ddj_with(const std::wstring& inner, const std::wstring& src) {
    const Pk2File* f = archive_->find_file(inner);
    if (!f) return;
    Row r;
    r.name = f->name;
    r.path = inner;
    std::vector<std::uint8_t> img;
    {
        HANDLE h = ::CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { show_message(Lf("ui_file_open_fail", src)); return; }
        LARGE_INTEGER sz{}; ::GetFileSizeEx(h, &sz);
        img.resize(static_cast<std::size_t>(sz.QuadPart));
        DWORD got = 0;
        BOOL ok = img.empty() || ::ReadFile(h, img.data(), static_cast<DWORD>(img.size()), &got, nullptr);
        ::CloseHandle(h);
        if (!ok || got != img.size()) { show_message(Lf("ui_file_read_fail", src)); return; }
    }
    try {
        std::vector<std::uint8_t> old = archive_->read_file(*f), out;
        std::wstring err;
        if (!bak2::encode_ddj(img, &old, out, err)) { show_message(err); return; }
        archive_->add_file(r.path, out);
        archive_->reload();
        rebuild_rows();
    } catch (const std::exception& ex) {
        show_message(exc_text(ex));
        return;
    }
    log(L"✔ " + base_name(r.name) + L" ← " + base_name(src) + L" (DDJ)");
    for (std::size_t k = 0; k < rows_.size(); ++k)
        if (rows_[k].path == r.path) { select_row(static_cast<int>(k)); break; }
}

// ---- operations ------------------------------------------------------------
void App::do_open() {
    ui::BrowseRequest req;
    req.mode = ui::BrowseMode::OpenFile;
    req.kind = "pk2";
    req.title = L("open");
    req.filters = {{"ui_filter_sro", {L"pk2", L"pk3", L"lib", L"vp"}}, {"ui_filter_all", {}}};
    browse(std::move(req), [this](std::vector<std::wstring> paths) { open_path(paths.front()); });
}
void App::open_path(const std::wstring& path) {
    wchar_t kb[128]; ::GetWindowTextW(key_edit_, kb, 128); key_ = kb[0] ? kb : L"169841";
    select_row(-1);
    try {
        archive_->open(path, wide_to_utf8(key_));
        after_open(path);
    } catch (const Pk2AuthError&) {
        offer_deep_open(path);
    } catch (const std::exception& ex) {
        // Unknown header / key scheme: BAK's KeyFinder may still find the key.
        std::wstring msg = exc_text(ex);
        show_confirm(L("error"), msg + L"\n\n" + L("ui_deep_search_ask"),
                     [this, path]() { start_deep_open(path); });
    }
    refresh();
}
void App::offer_deep_open(const std::wstring& path) {
    show_confirm(L("error"), L("wrong_key") + L"\n\n" + L("ui_deep_search_ask_slow"),
                 [this, path]() { start_deep_open(path); });
}
void App::start_deep_open(const std::wstring& path) {
    pending_path_ = path;
    pending_store_ = std::make_unique<Store>();
    log(Lf("ui_deep_searching", base_name(path)));
    std::string key = wide_to_utf8(key_);
    start_op(Op::OpenDeep, [this, path, key]() { pending_store_->open(path, key, true); });
}
// Reset the browser state for a freshly opened container and report what was detected.
void App::after_open(const std::wstring& path) {
    // A different client: its skill tables and any running playback belong to the old one.
    if (fxview_has_ || skill_playing_) { fxview_.clear(); fxview_has_ = false; skill_playing_ = false; }
    skill_db_.reset(); skill_db_tried_ = false; skill_results_.clear(); skill_sel_ = -1; skill_scroll_ = 0;
    ::PlaySoundW(nullptr, nullptr, 0);
    if (preview_tab_ == kTabSkill) ensure_skill_db();
    expanded_.clear(); selected_ = -1; scroll_ = 0; search_.clear(); clear_texture(); text_preview_.clear();
    ::SetWindowTextW(search_edit_, L""); rebuild_rows();
    set_status(archive_status());
    log(L("open") + L": " + path + (archive_->bak_backed() ? L"  [" + archive_->kind() + L"]" : L""));
    // First lines of BAK's protection report (container, key scheme, asset cipher) into the log.
    std::wstring rep = archive_->protection_report();
    for (int n = 0; !rep.empty() && n < 4; ++n) {
        std::size_t e = rep.find(L'\n');
        std::wstring line = rep.substr(0, e);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (!line.empty()) log(L"   " + line);
        if (e == std::wstring::npos) break;
        rep.erase(0, e + 1);
    }
}
// ---- Skill tab (BAK-2 Catalog + skillplay) ----------------------------------------------------
void App::ensure_skill_db() {
    if (skill_db_ || skill_db_tried_ || !archive_->is_open()) return;
    skill_db_tried_ = true;
    const std::wstring prev_status = status_;
    set_status(L("ui_skill_db_loading"));
    ::RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    std::wstring err;
    // textdata lives in Media.pk2: read_any falls back to the sibling containers.
    skill_db_ = bak2::SkillDb::load([this](const std::wstring& p, std::vector<std::uint8_t>& out) { return archive_->read_any(p, out); }, err);
    if (skill_db_) {
        log(Lf("ui_skill_records", skill_db_->size()));
        skill_search();
    } else {
        log(L"⚠ Skill: " + err);
    }
    set_status(prev_status);
}

void App::skill_search() {
    skill_results_.clear(); skill_sel_ = -1; skill_hover_ = -1; skill_scroll_ = 0;
    if (!skill_db_) { refresh(); return; }
    wchar_t b[256] = {}; if (skill_edit_) ::GetWindowTextW(skill_edit_, b, 256);
    std::wstring q = b;
    skill_results_ = skill_db_->search(q.empty() ? std::wstring(L"SKILL_") : q, 400);
    refresh();
}

int App::skill_row_at(int x, int y) const {
    if (preview_tab_ != kTabSkill || !skill_db_) return -1;
    if (x < sk_list_.left || x > sk_list_.right - S(10) || y < sk_list_.top || y >= sk_list_.bottom) return -1;
    int k = static_cast<int>((static_cast<float>(y) - sk_list_.top + skill_scroll_) / S(28));
    return (k >= 0 && k < static_cast<int>(skill_results_.size())) ? k : -1;
}

void App::play_skill_row(int k) {
    if (!skill_db_ || !fxview_ready_ || k < 0 || k >= static_cast<int>(skill_results_.size())) return;
    skill_sel_ = k;
    if (fxview_has_) { fxview_.clear(); fxview_has_ = false; efp_panel_ = false; }
    const bak2::SkillItem& it = skill_results_[static_cast<std::size_t>(k)];
    fxview_.set_skill_loop(skill_loop_);
    std::wstring err;
    if (fxview_.play_skill(*skill_db_, it.id, err)) {
        skill_playing_ = true;
        fxview_play_ = true;
        fxview_.set_playing(fxview_vis_);
        // The skill's rows and timeline in plain words for the Metin tab.
        text_preview_ = skill_db_->describe(it.id);
        text_is_info_ = !text_preview_.empty(); info_edit_ = InfoEdit::None;
        log(L"▶ " + it.name + L"  (" + it.code + L")");
    } else {
        skill_playing_ = false;
        show_message(it.name + L"\n\n" + err, L("tab_skill"));
    }
    refresh();
}

// Skill sounds (effectsound / clip events): .wav from the archives, played in-process.
void App::play_skill_sound(const std::wstring& path) {
    if (preview_tab_ != kTabSkill) return;
    std::vector<std::uint8_t> b;
    if (!archive_->read_any(path, b) || b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0) return;
    ::PlaySoundW(nullptr, nullptr, 0);                 // stop the previous one before its buffer goes
    skill_snd_buf_ = std::move(b);
    ::PlaySoundW(reinterpret_cast<LPCWSTR>(skill_snd_buf_.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// ---- .efp editor (BAK-2 efpwrite) ---------------------------------------------------------------
void App::efp_toggle_panel() {
    if (!efp_ed_) {
        std::wstring err;
        efp_ed_ = bak2::EfpEditor::open(efp_bytes_, err, efp_path_,
                                        [this](const std::wstring& p, std::vector<std::uint8_t>& out) { return archive_->read_any(p, out); });
        if (!efp_ed_) { show_message(err.empty() ? L("ui_efp_open_fail") : err); return; }
        efp_p_ = efp_ed_->params();
    }
    efp_panel_ = !efp_panel_;
    if (efp_panel_) { text_preview_ = efp_ed_->summary(); text_is_info_ = !text_preview_.empty(); }
    refresh();
}

// Rebuild the edited effect (always from the original document) and play it live.
void App::efp_apply() {
    if (!efp_ed_) return;
    efp_ed_->set_params(efp_p_);
    std::vector<bak2::EfpEditor::OutFile> files;
    std::wstring err;
    if (!efp_ed_->build_files(files, err) || files.empty()) { log(L"⚠ EFP: " + err); refresh(); return; }
    std::vector<std::pair<std::wstring, std::vector<std::uint8_t>>> extra;
    for (std::size_t i = 1; i < files.size(); ++i) extra.emplace_back(files[i].inner, files[i].data);
    std::wstring e2;
    if (fxview_.load_effect_bytes(files[0].data, efp_path_, extra, e2)) fxview_.set_playing(fxview_vis_ && fxview_play_);
    else log(L"⚠ EFP: " + e2);
    for (const std::wstring& n : efp_ed_->notes()) log(L"   " + n);
    refresh();
}

void App::efp_save() {
    if (!efp_ed_ || archive_->read_only() || !efp_ed_->modified()) return;
    std::vector<bak2::EfpEditor::OutFile> files;
    std::wstring err;
    if (!efp_ed_->build_files(files, err) || files.empty()) { show_message(err); return; }
    const std::wstring path = efp_path_;
    try {
        archive_->add_file(path, files[0].data);
        for (std::size_t i = 1; i < files.size(); ++i) archive_->add_file(files[i].inner, files[i].data);
        archive_->reload();
        rebuild_rows();
    } catch (const std::exception& ex) { show_message(exc_text(ex)); return; }
    log(L"✔ " + base_name(path) + L" → pk2" + (files.size() > 1 ? L"  (" + Lf("ui_plus_textures", files.size() - 1) + L")" : std::wstring()));
    for (std::size_t k = 0; k < rows_.size(); ++k) if (rows_[k].path == path) { select_row(static_cast<int>(k)); break; }
}

// Asset cipher / VP payload key recovery (scans client binaries, dumps, a running client): worker thread.
void App::recover_keys(int sel) {
    if (!archive_->is_open() || busy_) return;
    recover_row_ = sel;
    log(L("recover_key") + L"\u2026");
    start_op(Op::Recover, [this]() {
        std::wstring how, how2;
        bool ok = archive_->recover_asset_cipher(how);
        if (!ok && archive_->bak_backed()) ok = archive_->recover_vp_payload_key(how2);
        std::lock_guard<std::mutex> lk(prog_mtx_);
        recover_msg_ = how + (how2.empty() ? L"" : L"\n" + how2);
        recover_ok_ = ok;
    });
}
void App::do_new() {
    ui::BrowseRequest req;
    req.mode = ui::BrowseMode::SaveFile;
    req.kind = "pk2";
    req.title = L("new_pk2");
    req.filters = {{"type_archive", {L"pk2"}}};
    req.file_name = L"New.pk2";
    req.default_ext = L"pk2";
    browse(std::move(req), [this](std::vector<std::wstring> paths) { create_pk2(paths.front()); });
}
void App::create_pk2(std::wstring path) {
    if (path.size() < 4 || to_lower(path.substr(path.size() - 4)) != L".pk2") path += L".pk2";
    wchar_t kb[128]; ::GetWindowTextW(key_edit_, kb, 128); key_ = kb[0] ? kb : L"169841";
    try { archive_->create(path, wide_to_utf8(key_)); expanded_.clear(); selected_ = -1; rebuild_rows(); set_status(base_name(path)); log(L("new_pk2") + L": " + path); }
    catch (const std::exception& ex) { show_message(exc_text(ex)); }
    refresh();
}
void App::do_extract_all() {
    if (!archive_->is_open()) return;
    ui::BrowseRequest req;
    req.mode = ui::BrowseMode::PickFolder;
    req.kind = "extract";
    req.title = L("extract_all");
    browse(std::move(req), [this](std::vector<std::wstring> paths) { extract_all_to(paths.front()); });
}
void App::extract_all_to(const std::wstring& dest) {
    if (!archive_->is_open() || busy_) return;
    log(L("extracting"));
    start_op(Op::ExtractAll, [this, dest]() { archive_->extract_all(dest,
        [this](const Progress& p) { prog_done_ = p.done; prog_total_ = p.total; std::lock_guard<std::mutex> lk(prog_mtx_); prog_current_ = p.current; }, [this]() { return cancel_.load(); }); });
}
void App::do_extract_sel() {
    if (selected_ < 0 || selected_ >= static_cast<int>(rows_.size())) return;
    Row r = rows_[static_cast<std::size_t>(selected_)];
    ui::BrowseRequest req;
    req.mode = ui::BrowseMode::PickFolder;
    req.kind = "extract";
    req.title = L("extract_sel");
    browse(std::move(req), [this, r](std::vector<std::wstring> paths) { extract_row_to(r, paths.front()); });
}
void App::extract_row_to(const Row& r, const std::wstring& dest) {
    if (!archive_->is_open() || busy_) return;
    log(L("extracting") + L" " + base_name(r.name));
    start_op(Op::ExtractSel, [this, r, dest]() {
        if (r.is_folder) { const Pk2Folder* f = archive_->find_folder(r.path); if (f) archive_->extract_folder(*f, dest + L"\\" + f->name,
            [this](const Progress& p) { prog_done_ = p.done; prog_total_ = p.total; std::lock_guard<std::mutex> lk(prog_mtx_); prog_current_ = p.current; }, [this]() { return cancel_.load(); }); }
        else { const Pk2File* f = archive_->find_file(r.path); if (f) { prog_total_ = 1; archive_->extract_file(*f, dest + L"\\" + f->name); prog_done_ = 1; } }
    });
}
void App::add_paths(const std::vector<std::wstring>& paths, const std::wstring& base) {
    log(L("importing"));
    start_op(Op::AddFiles, [this, paths, base]() {
        const auto progress = [this](const Progress& p) { prog_done_ = p.done; prog_total_ = p.total; std::lock_guard<std::mutex> lk(prog_mtx_); prog_current_ = p.current; };
        const auto cancel = [this]() { return cancel_.load(); };
        // Loose files go in one batch (read ahead on all cores, written in order); each folder
        // through the importer. Replaced files reuse their space: the archive does not grow for them.
        std::vector<DiskFile> files;
        std::uint64_t added = 0;
        for (const std::wstring& full : paths) {
            if (cancel_.load()) break;
            const std::wstring nm = base_name(full);
            const std::wstring inner = base.empty() ? nm : base + L"/" + nm;
            const DWORD attr = ::GetFileAttributesW(full.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) added += archive_->import_folder(full, inner, progress, cancel);
            else files.push_back({full, inner});
        }
        if (!files.empty() && !cancel_.load()) added += archive_->add_disk_files(files, progress, cancel);
        prog_done_ = added;
    });
}
void App::do_add_files() {
    if (!archive_->is_open()) return;
    std::wstring base;
    if (selected_ >= 0 && selected_ < static_cast<int>(rows_.size())) { const Row& r = rows_[static_cast<std::size_t>(selected_)];
        base = r.is_folder ? r.path : (r.path.find_last_of(L'/') == std::wstring::npos ? std::wstring() : r.path.substr(0, r.path.find_last_of(L'/'))); }
    ui::BrowseRequest req;
    req.mode = ui::BrowseMode::OpenFiles;
    req.kind = "import";
    req.title = L("add_file");
    req.filters = {{"ui_filter_all", {}}};
    browse(std::move(req), [this, base](std::vector<std::wstring> paths) {
        if (archive_->is_open() && !busy_) add_paths(paths, base);
    });
}
void App::do_add_folder() {
    if (!archive_->is_open()) return;
    std::wstring base; if (selected_ >= 0 && selected_ < static_cast<int>(rows_.size())) { const Row& r = rows_[static_cast<std::size_t>(selected_)]; if (r.is_folder) base = r.path; }
    ui::BrowseRequest req;
    req.mode = ui::BrowseMode::PickFolder;
    req.kind = "import";
    req.title = L("add_folder");
    browse(std::move(req), [this, base](std::vector<std::wstring> paths) {
        if (archive_->is_open() && !busy_) add_paths(paths, base);
    });
}
void App::do_delete() {
    if (selected_ < 0 || selected_ >= static_cast<int>(rows_.size())) return;
    Row r = rows_[static_cast<std::size_t>(selected_)];
    show_confirm(L("remove"), L("confirm_delete") + L"\n\n" + r.path, [this, r]() {
        try { if (r.is_folder) archive_->remove_folder(r.path); else archive_->remove_file(r.path); archive_->reload(); select_row(-1); rebuild_rows(); status_ = archive_status(); log(L("remove") + L": " + base_name(r.name)); }
        catch (const std::exception& ex) { show_message(exc_text(ex)); }
        refresh();
    });
}
void App::do_defrag() {
    if (!archive_->is_open()) return; op_size_before_ = archive_->file_size(); log(L("defragging"));
    start_op(Op::Defrag, [this]() { archive_->defragment(
        [this](const Progress& p) { prog_done_ = p.done; prog_total_ = p.total; std::lock_guard<std::mutex> lk(prog_mtx_); prog_current_ = p.current; }, [this]() { return cancel_.load(); }); });
}

// ---- window proc -----------------------------------------------------------
LRESULT CALLBACK App::wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) { auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp); ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams)); }
    App* self = reinterpret_cast<App*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(h, m, wp, lp);
    if (m == WM_NCCREATE) self->hwnd_ = h;
    return self->handle(m, wp, lp);
}
LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        scale_ = static_cast<float>(::GetDpiForWindow(hwnd_)) / 96.0f;
        BOOL dark = TRUE; ::DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        int corner = 2; ::DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        MARGINS mg{0, 0, 0, 1}; ::DwmExtendFrameIntoClientArea(hwnd_, &mg);
        ::DragAcceptFiles(hwnd_, TRUE);
        model_ready_ = model_view_.init(hwnd_, hinst_);
        fxview_ready_ = fxview_.create(hwnd_, 0, 0, 16, 16);
        if (fxview_ready_) {
            // Assets the effect / model references: this container first, then its siblings.
            fxview_.set_source([this](const std::wstring& p, std::vector<std::uint8_t>& out) { return archive_->read_any(p, out); });
            fxview_.set_overlay(false);
            fxview_.set_sound_callback([this](const std::wstring& p, int) { play_skill_sound(p); });
        }
        edit_bg_ = ::CreateSolidBrush(RGB(0x0A, 0x0F, 0x19));
        code_bg_ = ::CreateSolidBrush(RGB(0x0A, 0x0E, 0x17));
        edit_font_ = ::CreateFontW(-static_cast<int>(S(15)), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        code_font_ = ::CreateFontW(-static_cast<int>(S(13)), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        key_edit_ = ::CreateWindowExW(0, L"EDIT", L"169841", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_KEY_EDIT)), hinst_, nullptr);
        search_edit_ = ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_EDIT)), hinst_, nullptr);
        ::SendMessageW(key_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), TRUE);
        ::SendMessageW(search_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), TRUE);
        ::SendMessageW(key_edit_, EM_SETLIMITTEXT, 64, 0); // pk2 key up to 56 chars (Silkroad), allow 64
        ::SendMessageW(search_edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L("search").c_str()));
        skill_edit_ = ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SKILL_EDIT)), hinst_, nullptr);
        ::SendMessageW(skill_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), TRUE);
        ::SendMessageW(skill_edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L("skill_search").c_str()));
        subclass_field(key_edit_); subclass_field(search_edit_); subclass_field(skill_edit_);
        layout_controls();
        return 0;
    }
    case WM_NCCALCSIZE:
        if (wp) {
            // Frameless: client == window. But a maximized WS_THICKFRAME window overhangs the
            // monitor by the frame thickness, clipping our edges — inset the client to compensate.
            if (::IsZoomed(hwnd_)) {
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                int fx = ::GetSystemMetrics(SM_CXFRAME) + ::GetSystemMetrics(SM_CXPADDEDBORDER);
                int fy = ::GetSystemMetrics(SM_CYFRAME) + ::GetSystemMetrics(SM_CXPADDEDBORDER);
                p->rgrc[0].left += fx; p->rgrc[0].right -= fx;
                p->rgrc[0].top += fy; p->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; RECT wr; ::GetWindowRect(hwnd_, &wr);
        int x = pt.x - wr.left, y = pt.y - wr.top, w = wr.right - wr.left, h = wr.bottom - wr.top, b = static_cast<int>(S(6));
        if (!::IsZoomed(hwnd_)) { bool l = x < b, r = x >= w - b, t = y < b, bm = y >= h - b;
            if (t && l) return HTTOPLEFT; if (t && r) return HTTOPRIGHT; if (bm && l) return HTBOTTOMLEFT; if (bm && r) return HTBOTTOMRIGHT;
            if (l) return HTLEFT; if (r) return HTRIGHT; if (t) return HTTOP; if (bm) return HTBOTTOM; }
        // Caption: test in CLIENT coords so the maximized inset doesn't shift it. Our buttons are
        // client area (drawn and handled by us), so Windows shows no snap flyout or tooltips.
        POINT cpt{pt.x, pt.y}; ::ScreenToClient(hwnd_, &cpt);
        if (cpt.y >= 0 && cpt.y < static_cast<int>(cap_h_)) {
            auto in = [&](const D2D1_RECT_F& r) { return cpt.x >= r.left && cpt.x <= r.right && cpt.y >= r.top && cpt.y <= r.bottom; };
            if (in(cap_lang_) || in(cap_group_)) return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    // No Windows system menu (right click on the title bar, Alt+Space).
    case WM_NCRBUTTONDOWN: case WM_NCRBUTTONUP: if (wp == HTCAPTION) return 0; break;
    case WM_CONTEXTMENU: return 0;
    case WM_SYSCOMMAND: if ((wp & 0xFFF0) == SC_KEYMENU) return 0; break;
    case WM_GETMINMAXINFO: { auto* mmi = reinterpret_cast<MINMAXINFO*>(lp); mmi->ptMinTrackSize.x = static_cast<LONG>(S(980)); mmi->ptMinTrackSize.y = static_cast<LONG>(S(620));
        HMONITOR mon = ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST); MONITORINFO mi{sizeof(mi)};
        if (::GetMonitorInfoW(mon, &mi)) { mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left; mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top; mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left; mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top; }
        return 0; }
    case WM_SIZE: if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp))); layout_controls(); refresh(); return 0;
    case WM_DPICHANGED: { scale_ = static_cast<float>(HIWORD(wp)) / 96.0f; recreate_text_formats();
        if (edit_font_) ::DeleteObject(edit_font_); edit_font_ = ::CreateFontW(-static_cast<int>(S(15)), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        ::SendMessageW(key_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), TRUE); ::SendMessageW(search_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), TRUE);
        RECT* s = reinterpret_cast<RECT*>(lp); ::SetWindowPos(hwnd_, nullptr, s->left, s->top, s->right - s->left, s->bottom - s->top, SWP_NOZORDER | SWP_NOACTIVATE); layout_controls(); refresh(); return 0; }
    case WM_CTLCOLOREDIT: { HDC dc = reinterpret_cast<HDC>(wp);
        if (reinterpret_cast<HWND>(lp) == code_edit_) { ::SetTextColor(dc, RGB(0xCB, 0xD5, 0xE6)); ::SetBkColor(dc, RGB(0x0A, 0x0E, 0x17)); return reinterpret_cast<LRESULT>(code_bg_); }
        ::SetTextColor(dc, RGB(0xED, 0xF1, 0xF8)); ::SetBkColor(dc, RGB(0x0A, 0x0F, 0x19)); return reinterpret_cast<LRESULT>(edit_bg_); }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_SKILL_EDIT && HIWORD(wp) == EN_CHANGE) { skill_search(); return 0; }
        if (LOWORD(wp) == ID_SEARCH_EDIT && HIWORD(wp) == EN_CHANGE) { wchar_t b[256]; ::GetWindowTextW(search_edit_, b, 256); search_ = b; scroll_ = 0; selected_ = -1; rebuild_rows(); refresh(); }
        return 0;
    case WM_DROPFILES: {
        HDROP hd = reinterpret_cast<HDROP>(wp);
        UINT cnt = ::DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        for (UINT i = 0; i < cnt; ++i) { wchar_t p[MAX_PATH]; ::DragQueryFileW(hd, i, p, MAX_PATH); paths.emplace_back(p); }
        ::DragFinish(hd);
        if (busy_ || paths.empty()) return 0;
        // A single .pk2 dropped while nothing is open opens that archive; otherwise drops are imported.
        auto is_pk2 = [](const std::wstring& p) {
            std::wstring e = to_lower(p.substr(p.find_last_of(L'.') == std::wstring::npos ? p.size() : p.find_last_of(L'.')));
            return e == L".pk2" || e == L".pk3" || e == L".lib" || e == L".vp";
        };
        if (!archive_->is_open() && paths.size() == 1 && is_pk2(paths[0])) { open_path(paths[0]); return 0; }
        if (archive_->is_open()) {
            std::wstring base; if (selected_ >= 0 && selected_ < static_cast<int>(rows_.size())) { const Row& r = rows_[static_cast<std::size_t>(selected_)]; if (r.is_folder) base = r.path; else if (r.path.find_last_of(L'/') != std::wstring::npos) base = r.path.substr(0, r.path.find_last_of(L'/')); }
            add_paths(paths, base);
        }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; ::BeginPaint(hwnd_, &ps); render(); ::EndPaint(hwnd_, &ps); return 0; }
    case WM_MOUSELEAVE: if (lang_btn_hover_ || cap_hover_) { lang_btn_hover_ = false; cap_hover_ = 0; refresh(); } return 0;
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (ed_sb_drag_) { editor_scroll_to(static_cast<float>(y)); return 0; }
        {   // window buttons work even over a dialog
            const int ch = cap_button_at(x, y);
            if (ch != cap_hover_) {
                cap_hover_ = ch; refresh();
                if (ch) { TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd_, 0}; ::TrackMouseEvent(&t); }
            }
            if (ch) { ::SetCursor(::LoadCursorW(nullptr, IDC_HAND)); return 0; }
        }
        if (dlg_.open) return 0;
        if (browser_.is_open()) {
            browser_.on_mouse_move(x, y);
            if (browser_.needs_redraw()) refresh();
            ::SetCursor(::LoadCursorW(nullptr, browser_.hand_cursor() ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        {
            const bool on_btn = x >= cap_lang_.left && x <= cap_lang_.right && y >= cap_lang_.top && y <= cap_lang_.bottom;
            if (on_btn != lang_btn_hover_) {
                lang_btn_hover_ = on_btn; refresh();
                if (on_btn) { TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd_, 0}; ::TrackMouseEvent(&t); }
            }
            if (lang_open_) {
                if (const int c = lang_card_at(x, y); c != lang_hover_) { lang_hover_ = c; refresh(); }
                ::SetCursor(::LoadCursorW(nullptr, (lang_hover_ >= 0 || on_btn) ? IDC_HAND : IDC_ARROW));
                return 0;
            }
            if (on_btn) { ::SetCursor(::LoadCursorW(nullptr, IDC_HAND)); return 0; }
        }
        if (sb_drag_) {
            float listH = r_tree_.bottom - r_tree_head_.bottom, contentH = rows_.size() * row_h_;
            if (contentH > listH) { float thumbH = std::max(S(36), listH * (listH / contentH));
                float frac = (y - r_tree_head_.bottom - thumbH / 2) / (listH - thumbH);
                if (frac < 0) frac = 0; if (frac > 1) frac = 1; scroll_ = frac * (contentH - listH); refresh(); }
            return 0;
        }
        if (int sh = skill_row_at(x, y); sh != skill_hover_) { skill_hover_ = sh; refresh(); }
        int nr = rail_at(x, y), nrow = row_at(x, y);
        bool handcursor = nr >= 0 || tab_at(x, y) >= 0;
        if (nr != hover_rail_ || nrow != hover_row_) { hover_rail_ = nr; hover_row_ = nrow; ensure_timer(); refresh(); }
        if (ctx_open_) {
            int oldh = ctx_hover_; ctx_hover_ = -1;
            float mw = S(206), ih = S(34), pad = S(6); float yy = ctx_y_ + pad; int idx = 0;
            for (auto& it : ctx_items_) { if (it.sep) { yy += S(11); continue; } if (x >= ctx_x_ && x <= ctx_x_ + mw && y >= yy && y <= yy + ih) ctx_hover_ = idx; yy += ih; ++idx; }
            if (oldh != ctx_hover_) refresh();
        }
        ::SetCursor(::LoadCursorW(nullptr, handcursor ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        auto hit = [&](const D2D1_RECT_F& r) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; };
        if (const int cb = cap_button_at(x, y)) {   // acts on release over the same button
            cap_pressed_ = cb; ::SetCapture(hwnd_); refresh();
            return 0;
        }
        if (dlg_.open) {   // modal: only the dialog's buttons react
            if (hit(dlg_.ok)) close_dialog(true);
            else if (dlg_.confirm && hit(dlg_.cancel)) close_dialog(false);
            return 0;
        }
        if (browser_.is_open()) {   // modal picker: drags (scroll bars) keep the mouse
            ::SetCapture(hwnd_);
            browser_.on_mouse_down(x, y, false, (wp & MK_CONTROL) != 0, (wp & MK_SHIFT) != 0);
            if (browser_.needs_redraw()) refresh();
            return 0;
        }
        if (lang_open_) {   // a card picks its language; any other click closes the panel
            const int c = lang_card_at(x, y);
            if (c >= 0) apply_language(kLangs[c]);
            else if (!hit(lang_panel_)) { lang_open_ = false; lang_hover_ = -1; layout_controls(); refresh(); }
            return 0;
        }
        if (hit(cap_lang_)) {
            if (busy_) return 0;
            ctx_open_ = false; lang_open_ = true; lang_hover_ = -1; layout_controls(); ::SetFocus(hwnd_); refresh();
            return 0;
        }
        if (editor_open_ && editor_mode_ != 1 && x >= ed_bar_.left - S(4) && x <= ed_bar_.right + S(4) && y >= ed_bar_.top && y <= ed_bar_.bottom) {
            ed_sb_drag_ = true; ::SetCapture(hwnd_); editor_scroll_to(static_cast<float>(y)); return 0;
        }
        if (ctx_open_) {
            float mw = S(206), ih = S(34), pad = S(6); float yy = ctx_y_ + pad; int chosen = -2;
            for (auto& it : ctx_items_) { if (it.sep) { yy += S(11); continue; } if (x >= ctx_x_ && x <= ctx_x_ + mw && y >= yy && y <= yy + ih) chosen = it.id; yy += ih; }
            ctx_open_ = false; refresh();
            if (chosen == 1000) run_button(B_EXTRACT_SEL);
            else if (chosen == 1001) run_button(B_DELETE);
            else if (chosen == 2000 && selected_ >= 0) edit_in_notepadpp(rows_[static_cast<std::size_t>(selected_)]);
            else if (chosen == 2001 && selected_ >= 0) open_inapp_editor(rows_[static_cast<std::size_t>(selected_)]);
            else if (chosen == 2002 && selected_ >= 0) { preview_tab_ = 2; play_sound(rows_[static_cast<std::size_t>(selected_)]); }
            else if (chosen == 2003 && selected_ >= 0) open_hex_editor(rows_[static_cast<std::size_t>(selected_)]);
            else if (chosen == 2004) {
                std::wstring rep = archive_->protection_report();
                show_message(rep.empty() ? L("ui_no_protection_info") : rep, L("protection"));
            } else if (chosen == 2005) recover_keys(selected_);
            return 0;
        }
        if (editor_open_) {
            if (x >= btn_editor_save_.left && x <= btn_editor_save_.right && y >= btn_editor_save_.top && y <= btn_editor_save_.bottom) { save_inapp_editor(); return 0; }
            if (x >= btn_editor_cancel_.left && x <= btn_editor_cancel_.right && y >= btn_editor_cancel_.top && y <= btn_editor_cancel_.bottom) { close_inapp_editor(); return 0; }
        }
        // scrollbar drag / jump
        {
            float listH = r_tree_.bottom - r_tree_head_.bottom;
            float contentH = rows_.size() * row_h_;
            if (contentH > listH && x >= scrollbar_x_ && x <= scrollbar_x_ + S(14) && y >= r_tree_head_.bottom && y <= r_tree_.bottom) {
                float thumbH = std::max(S(36), listH * (listH / contentH));
                float frac = (y - r_tree_head_.bottom - thumbH / 2) / (listH - thumbH);
                if (frac < 0) frac = 0; if (frac > 1) frac = 1;
                scroll_ = frac * (contentH - listH);
                sb_drag_ = true; ::SetCapture(hwnd_); refresh(); return 0;
            }
        }
        int rb = rail_at(x, y);
        if (rb >= 0) { if (button_enabled(rb)) run_button(rb); return 0; }
        int tb = tab_at(x, y);
        if (tb >= 0) {
            if (editor_open_ && tb != 3) close_inapp_editor();
            if (tb != kTabSkill) ::PlaySoundW(nullptr, nullptr, 0);
            preview_tab_ = tb;
            if (tb == kTabSkill) ensure_skill_db();
            refresh(); return 0;
        }
        if (preview_tab_ == kTabSkill && skill_db_) {
            int k = skill_row_at(x, y);
            if (k >= 0) { play_skill_row(k); return 0; }
            if (hit(btn_sk_play_)) {
                if (skill_playing_) { fxview_play_ = !fxview_play_; fxview_.set_playing(fxview_play_); }
                else if (skill_sel_ >= 0) play_skill_row(skill_sel_);
                refresh(); return 0;
            }
            if (hit(btn_sk_loop_)) { skill_loop_ = !skill_loop_; fxview_.set_skill_loop(skill_loop_); refresh(); return 0; }
            if (hit(btn_sk_reset_)) { if (skill_playing_) fxview_.frame(); return 0; }
        }
        if (preview_tab_ == 0 && fxview_has_ && !efp_path_.empty()) {
            if (hit(btn_efpedit_)) { efp_toggle_panel(); return 0; }
            if (efp_panel_ && efp_ed_ && hit(efp_panel_rect_)) {
                for (const StepHit& s : efp_hits_) {
                    if (!hit(s.r)) continue;
                    const EfpParamDef& d = kEfpParams[s.param];
                    double& v = efp_p_.*(d.field);
                    double lo = d.lo, hi = d.hi;
                    if (d.degrees) { lo = efp_p_.colorize ? 0.0 : -180.0; hi = efp_p_.colorize ? 360.0 : 180.0; }
                    v = std::clamp(std::round((v + d.step * s.dir) / d.step) * d.step, lo, hi);
                    efp_apply(); return 0;
                }
                if (hit(btn_efp_colorize_)) {
                    efp_p_.colorize = !efp_p_.colorize;
                    efp_p_.hue = std::clamp(efp_p_.hue, efp_p_.colorize ? 0.0 : -180.0, efp_p_.colorize ? 360.0 : 180.0);
                    efp_apply(); return 0;
                }
                if (hit(btn_efp_recolor_)) { efp_p_.recolor_textures = !efp_p_.recolor_textures; efp_apply(); return 0; }
                if (hit(btn_efp_reset_)) { efp_p_ = bak2::EfpEditor::Params{}; efp_apply(); return 0; }
                if (hit(btn_efp_save_)) { efp_save(); return 0; }
                return 0;   // clicks inside the panel never reach the list below
            }
        }
        if (preview_tab_ == 2 && x >= btn_audio_.left && x <= btn_audio_.right && y >= btn_audio_.top && y <= btn_audio_.bottom) {
            if (audio_playing_) stop_sound();
            else if (selected_ >= 0 && !rows_[static_cast<std::size_t>(selected_)].is_folder) play_sound(rows_[static_cast<std::size_t>(selected_)]);
            refresh(); return 0;
        }
        if (preview_tab_ == 1 && can_replace_ddj() && x >= btn_replace_.left && x <= btn_replace_.right && y >= btn_replace_.top && y <= btn_replace_.bottom) {
            replace_ddj_from_image(); return 0;
        }
        if (preview_tab_ == 1 && tex_pages_.size() > 1) {
            const std::size_t n = tex_pages_.size();
            if (hit(btn_page_prev_)) { show_tex_page((tex_page_ + n - 1) % n); return 0; }
            if (hit(btn_page_next_)) { show_tex_page((tex_page_ + 1) % n); return 0; }
        }
        if (preview_tab_ == 0 && fxview_has_) {
            if (x >= btn_rotate_.left && x <= btn_rotate_.right && y >= btn_rotate_.top && y <= btn_rotate_.bottom) {
                fxview_play_ = !fxview_play_; fxview_.set_playing(fxview_play_); refresh(); return 0;
            }
            if (x >= btn_resetview_.left && x <= btn_resetview_.right && y >= btn_resetview_.top && y <= btn_resetview_.bottom) {
                fxview_.frame(); refresh(); return 0;
            }
        } else if (preview_tab_ == 0 && model_has_) {
            if (x >= btn_rotate_.left && x <= btn_rotate_.right && y >= btn_rotate_.top && y <= btn_rotate_.bottom) {
                model_view_.toggle_auto_rotate(); ensure_timer(); refresh(); return 0;
            }
            if (x >= btn_resetview_.left && x <= btn_resetview_.right && y >= btn_resetview_.top && y <= btn_resetview_.bottom) {
                model_view_.reset_view(); model_view_.render(); refresh(); return 0;
            }
        }
        if (!editor_open_ && preview_tab_ == 3 && selected_ >= 0 && text_is_info_ && info_action_visible() && hit(btn_info_edit_)) {
            const Row& sr = rows_[static_cast<std::size_t>(selected_)];
            if (info_edit_ == InfoEdit::Readable) reveal_path(std::wstring(goto_path_));
            else if (info_edit_ != InfoEdit::None) open_inapp_editor(sr);
            else open_hex_editor(sr);
            return 0;
        }
        if (!editor_open_ && preview_tab_ == 3 && selected_ >= 0 && !text_preview_.empty() && !text_is_info_) {
            if (x >= btn_edit_.left && x <= btn_edit_.right && y >= btn_edit_.top && y <= btn_edit_.bottom) { open_inapp_editor(rows_[static_cast<std::size_t>(selected_)]); return 0; }
            if (x >= btn_npp_.left && x <= btn_npp_.right && y >= btn_npp_.top && y <= btn_npp_.bottom && !notepadpp_exe().empty()) { edit_in_notepadpp(rows_[static_cast<std::size_t>(selected_)]); return 0; }
        }
        int r = row_at(x, y);
        if (r >= 0) {
            const Row& row = rows_[static_cast<std::size_t>(r)];
            float indent = r_tree_.left + S(18) + row.depth * S(16);
            if (row.is_folder && search_.empty() && x >= indent - S(18) && x <= indent) toggle_expand(r);
            else { if (editor_open_) close_inapp_editor(); select_row(r); }
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (cap_pressed_) {
            const int pressed = cap_pressed_;
            cap_pressed_ = 0; ::ReleaseCapture(); refresh();
            if (cap_button_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) == pressed) {
                if (pressed == 1) ::ShowWindow(hwnd_, SW_MINIMIZE);
                else if (pressed == 2) ::ShowWindow(hwnd_, ::IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
                else ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        if (browser_.is_open()) {
            if (::GetCapture() == hwnd_) ::ReleaseCapture();
            browser_.on_mouse_up(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (browser_.needs_redraw()) refresh();
            return 0;
        }
        if (sb_drag_) { sb_drag_ = false; ::ReleaseCapture(); }
        if (ed_sb_drag_) { ed_sb_drag_ = false; ::ReleaseCapture(); refresh(); }
        return 0;
    case WM_RBUTTONDOWN: {
        if (dlg_.open || browser_.is_open()) return 0;
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int r = row_at(x, y);
        if (r >= 0) {
            select_row(r);
            const Row& row = rows_[static_cast<std::size_t>(r)];
            ctx_items_.clear();
            if (!row.is_folder && row.type.category == FileCategory::Sound) { ctx_items_.push_back({"tab_snd", 2002, false, false}); ctx_items_.push_back({"", 0, false, true}); }
            const std::wstring row_lname = to_lower(base_name(row.name));
            if (!row.is_folder && is_text(row.type.category)) {
                ctx_items_.push_back({"edit", 2001, false, false});
                // DIVISIONINFO.TXT / GATEPORT.TXT are binary records despite the name: no plain-text editor.
                if (row_lname != L"divisioninfo.txt" && row_lname != L"gateport.txt" && !notepadpp_exe().empty()) ctx_items_.push_back({"open_npp", 2000, false, false});
                ctx_items_.push_back({"", 0, false, true});
            }
            else if (!row.is_folder && !archive_->read_only() &&
                     (row.type.category == FileCategory::UI || row_lname == L"sv.t")) {
                // Structured editors: .2dt widget table, SV.T version.
                ctx_items_.push_back({row.type.category == FileCategory::UI ? "edit_table" : "edit_version", 2001, false, false});
                ctx_items_.push_back({"", 0, false, true});
            }
            if (!row.is_folder && !archive_->read_only()) ctx_items_.push_back({"edit_hex", 2003, false, false});
            ctx_items_.push_back({"extract_sel", 1000, false, false});
            ctx_items_.push_back({"", 0, false, true});
            ctx_items_.push_back({"protection", 2004, false, false});
            ctx_items_.push_back({"recover_key", 2005, false, false});
            if (!archive_->read_only()) { ctx_items_.push_back({"", 0, false, true}); ctx_items_.push_back({"remove", 1001, true, false}); }
            ctx_x_ = static_cast<float>(x); ctx_y_ = static_cast<float>(y); ctx_hover_ = -1; ctx_open_ = true; refresh();
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        if (dlg_.open) return 0;
        if (browser_.is_open()) {
            ::SetCapture(hwnd_);
            browser_.on_mouse_down(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true, (wp & MK_CONTROL) != 0, (wp & MK_SHIFT) != 0);
            if (browser_.needs_redraw()) refresh();
            return 0;
        }
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp); int r = row_at(x, y);
        if (r >= 0) { const Row& row = rows_[static_cast<std::size_t>(r)];
            if (row.is_folder) toggle_expand(r);
            else { select_row(r);
                   if (row.type.category == FileCategory::Sound) play_sound(row);
                   else if (is_text(row.type.category)) open_inapp_editor(rows_[static_cast<std::size_t>(r)]); }
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (dlg_.open) return 0;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ::ScreenToClient(hwnd_, &pt);
        const float steps = GET_WHEEL_DELTA_WPARAM(wp) / 120.0f;
        if (browser_.is_open()) {
            browser_.on_mouse_move(pt.x, pt.y);   // the wheel scrolls the pane under the pointer
            browser_.on_wheel(steps);
            if (browser_.needs_redraw()) refresh();
            return 0;
        }
        if (preview_tab_ == kTabSkill && pt.x >= sk_list_.left && pt.x <= sk_list_.right && pt.y >= sk_list_.top && pt.y <= sk_list_.bottom)
            skill_scroll_ -= steps * S(28) * 3.0f;   // clamped when drawn
        else scroll_ -= steps * row_h_ * 3.0f;
        refresh(); return 0;
    }
    case WM_KEYDOWN:
        if (dlg_.open) {
            if (wp == VK_RETURN || wp == VK_SPACE) close_dialog(true);
            else if (wp == VK_ESCAPE) close_dialog(false);
            return 0;
        }
        if (browser_.is_open()) {
            browser_.on_key(static_cast<unsigned>(wp), ::GetKeyState(VK_CONTROL) < 0, ::GetKeyState(VK_SHIFT) < 0);
            if (browser_.needs_redraw()) refresh();
            return 0;
        }
        if (lang_open_) {   // the panel takes the keyboard: Esc closes, arrows + Enter choose
            const int cur = static_cast<int>(std::find(std::begin(kLangs), std::end(kLangs), current_language()) - std::begin(kLangs));
            if (wp == VK_ESCAPE) { lang_open_ = false; lang_hover_ = -1; layout_controls(); refresh(); }
            else if (wp == VK_LEFT || wp == VK_RIGHT) {
                const int from = lang_hover_ >= 0 ? lang_hover_ : cur;
                lang_hover_ = (from + (wp == VK_RIGHT ? 1 : kLangCount - 1)) % kLangCount; refresh();
            } else if (wp == VK_RETURN || wp == VK_SPACE) apply_language(kLangs[lang_hover_ >= 0 ? lang_hover_ : cur]);
            return 0;
        }
        if (wp == VK_ESCAPE) { if (ctx_open_) { ctx_open_ = false; refresh(); } else if (editor_open_) close_inapp_editor(); else if (busy_) cancel_ = true; }
        else if (wp == VK_DELETE && !busy_ && selected_ >= 0) do_delete();
        else if (wp == VK_DOWN && selected_ + 1 < static_cast<int>(rows_.size())) select_row(selected_ + 1);
        else if (wp == VK_UP && selected_ > 0) select_row(selected_ - 1);
        return 0;
    case WM_TIMER:
        if (wp == 1) {
            ++anim_phase_; bool anim = false;
            for (int i = 0; i < B_COUNT; ++i) { float t = (i == hover_rail_) ? 1.0f : 0.0f; rail_anim_[i] += (t - rail_anim_[i]) * 0.25f; if (std::fabs(rail_anim_[i] - t) > 0.01f) anim = true; }
            if (model_ready_ && model_view_.visible()) model_view_.render();
            if (busy_ || anim) refresh();
            ensure_timer();
        } else if (wp == 2) { if (!busy_) poll_watches(); }
        return 0;
    case WM_CHAR:
        if (browser_.is_open() && !dlg_.open) {
            browser_.on_char(static_cast<wchar_t>(wp));
            if (browser_.needs_redraw()) refresh();
        }
        return 0;
    case WM_APP_DONE: finish_op(); return 0;
    case WM_APP_BROWSED: {   // the picker closed: child windows back, then the chosen action
        Picked done = std::move(browse_done_);
        browse_done_ = nullptr;
        std::vector<std::wstring> paths = std::move(browse_result_);
        browse_result_.clear();
        layout_controls(); update_model_view(); refresh();
        if (done && !paths.empty()) done(std::move(paths));
        return 0;
    }
    case WM_DESTROY: stop_sound(); ::PlaySoundW(nullptr, nullptr, 0); if (busy_) { cancel_ = true; if (worker_.joinable()) worker_.join(); } model_view_.destroy(); ::PostQuitMessage(0); return 0;
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

int App::run(HINSTANCE hinst, int nCmdShow) {
    hinst_ = hinst; g_app = this;
    // Command line: [--lang <en|tr|zh|ja|vi>] [archive]. Language: --lang, else the saved choice,
    // else the Windows UI language.
    std::wstring open_arg;
    Lang lang = detect_ui_language();
    {
        int argc = 0;
        if (LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc)) {
            bool from_cmdline = false;
            for (int i = 1; i < argc; ++i) {
                const std::wstring a = argv[i];
                if ((a == L"--lang" || a == L"-lang" || a == L"/lang") && i + 1 < argc) {
                    from_cmdline = parse_language(argv[++i], lang) || from_cmdline;
                } else if (a.starts_with(L"--lang=")) {
                    from_cmdline = parse_language(std::wstring_view(a).substr(7), lang) || from_cmdline;
                } else if (open_arg.empty() && ::GetFileAttributesW(a.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    open_arg = a;
                }
            }
            if (!from_cmdline) { Lang saved = lang; if (saved_language(saved)) lang = saved; }
            ::LocalFree(argv);
        }
    }
    set_language(lang);
    bakb::set_ui_language(language_tag(lang));
    create_device_independent();
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS; wc.lpfnWndProc = &App::wndproc;
    wc.hInstance = hinst; wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = nullptr; wc.lpszClassName = L"SroPk2StudioWnd";
    wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION); ::RegisterClassExW(&wc);
    int sw = ::GetSystemMetrics(SM_CXSCREEN), sh = ::GetSystemMetrics(SM_CYSCREEN), ww = 1680, wh = 940;
    DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN;
    HWND h = ::CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L("app_title").c_str(), style, (sw - ww) / 2, (sh - wh) / 2, ww, wh, nullptr, nullptr, hinst, this);
    if (!h) return 1;
    ::ShowWindow(h, nCmdShow); ::UpdateWindow(h);
    // "Open with" / a .pk2 dropped onto the exe: the archive to open.
    if (!open_arg.empty()) open_path(open_arg);
    MSG msg; while (::GetMessageW(&msg, nullptr, 0, 0)) { ::TranslateMessage(&msg); ::DispatchMessageW(&msg); }
    discard_device_resources();
    safe_release(f_ui_); safe_release(f_ui_right_); safe_release(f_semibold_); safe_release(f_title_); safe_release(f_icon_); safe_release(f_small_); safe_release(f_mono_);
    safe_release(round_cap_); safe_release(dw_); safe_release(d2d_);
    if (edit_font_) ::DeleteObject(edit_font_); if (code_font_) ::DeleteObject(code_font_);
    if (edit_bg_) ::DeleteObject(edit_bg_); if (code_bg_) ::DeleteObject(code_bg_);
    return 0;
}

} // namespace

namespace pk2 {
int run_app(HINSTANCE hinst, int nCmdShow) {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    App app; int rc = app.run(hinst, nCmdShow); ::CoUninitialize();
    return rc;
}
} // namespace pk2
