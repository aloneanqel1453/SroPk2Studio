#pragma once
// In-app file / folder picker drawn with Direct2D in the app's own style (no Windows dialog).
// Modal overlay: while open it takes every mouse and keyboard event of the main window.
//
// Modes: open one file, open several files, pick a folder, save a file (name field + overwrite
// confirmation). Places on the left (Desktop, Documents, Downloads, recent folders, drives), a
// clickable path bar, folders first then files of the active filter, keyboard navigation, a text
// field for the name / a typed path. Remembers the last folder per request kind.
#include <d2d1.h>
#include <dwrite.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pk2::ui {

// The app's drawing resources and palette, lent for one frame.
struct Theme {
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dw = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    IDWriteTextFormat* ui = nullptr;        // Segoe UI 14.5, single line, vertically centred
    IDWriteTextFormat* semibold = nullptr;  // Segoe UI Semibold 13.5
    IDWriteTextFormat* title = nullptr;     // Segoe UI Semibold 16.5
    IDWriteTextFormat* icon = nullptr;      // Segoe Fluent Icons 15, centred
    IDWriteTextFormat* note = nullptr;      // Segoe UI 12 ("small" is a Windows macro)
    ID2D1StrokeStyle* round_cap = nullptr;
    float scale = 1.0f;                     // DPI scale (1.0 = 96 dpi)
    std::uint32_t panel = 0x121A28, field = 0x0A0F19, text = 0xEDF1F8, subtext = 0x9BA7BC, dim = 0x5C687E,
                  accent = 0x5AD1FF, accent2 = 0x9B6BFF, green = 0x45E0A0, red = 0xFF6585, folder = 0x6FD0FF;
};

enum class BrowseMode { OpenFile, OpenFiles, PickFolder, SaveFile };

struct BrowseFilter {
    std::string label_key;              // i18n key of the filter's name
    std::vector<std::wstring> exts;     // lowercase, without the dot; empty = every file
};

struct BrowseRequest {
    BrowseMode mode = BrowseMode::OpenFile;
    std::string kind;                   // remembers the last folder per kind ("pk2", "image", "extract", ...)
    std::wstring title;                 // already translated
    std::vector<BrowseFilter> filters;  // first = active; ignored for PickFolder
    std::wstring start_dir;             // used when nothing is remembered for `kind`
    std::wstring file_name;             // SaveFile: proposed name
    std::wstring default_ext;           // SaveFile: added when the typed name has none (without dot)
};

class FileBrowser {
public:
    // Chosen full paths; empty when cancelled.
    using Done = std::function<void(std::vector<std::wstring> paths)>;

    FileBrowser();
    ~FileBrowser();
    FileBrowser(const FileBrowser&) = delete;
    FileBrowser& operator=(const FileBrowser&) = delete;

    void open(BrowseRequest request, Done done);
    void cancel();                                  // closes and reports "cancelled"
    [[nodiscard]] bool is_open() const noexcept;

    // Lays out inside the client area (call before draw and before hit tests).
    void layout(D2D1_RECT_F client, float scale);
    void draw(const Theme& theme);

    // Main-window input in client pixels. Each returns true when the picker used the event
    // (always true while open: it is modal). `needs_redraw()` tells whether to repaint.
    bool on_mouse_move(int x, int y);
    bool on_mouse_down(int x, int y, bool double_click, bool ctrl, bool shift);
    bool on_mouse_up(int x, int y);
    bool on_wheel(float notches);
    bool on_key(unsigned vk, bool ctrl, bool shift);
    bool on_char(wchar_t c);
    [[nodiscard]] bool needs_redraw() noexcept;     // true once after a change
    [[nodiscard]] bool hand_cursor() const noexcept; // pointer is over something clickable

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pk2::ui
