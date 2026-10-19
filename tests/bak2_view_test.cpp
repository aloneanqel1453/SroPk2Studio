// Console check of bak2::View against the real archives (opened READ-ONLY through bakbridge).
//   bak2view_test [efp inner path] [bsr inner path]
// Client: %SRO_CLIENT_DIR% (see test_client.hpp), key 169841; first suitable .efp of Particles.pk2 and
// res\mob\china\tiger.bsr (else the first res\mob .bsr) of Data.pk2. A hidden parent window holds the
// view; frames are captured and written as PNG to %TEMP%\bak2view_test\.
#include "bak2_view.hpp"
#include "i18n.hpp"
#include "test_client.hpp"
#include "bak_bridge.hpp"

#include "bak2/render/RImage.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <print>
#include <string>
#include <vector>

namespace {

const std::wstring kClient = require_client_dir();

std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    std::replace(s.begin(), s.end(), L'\\', L'/');
    return s;
}

bool EndsWith(const std::wstring& s, const wchar_t* e) {
    const std::wstring x(e);
    return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}

void Pump(int ms) {
    const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(ms);
    MSG msg;
    while (GetTickCount64() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

struct Shot {
    std::vector<std::uint8_t> bgra;
    int w = 0, h = 0;
};

bool Capture(bak2::View& v, Shot& s) { return v.capture(s.bgra, s.w, s.h) && s.w > 0 && s.h > 0; }

// Pixels whose colour differs from the empty view (same size) by more than 30 (sum of |dB|+|dG|+|dR|),
// the bottom 24 rows (status line) excluded.
long long NonBackground(const Shot& s, const Shot& empty) {
    if (s.w != empty.w || s.h != empty.h) return -1;
    long long n = 0;
    for (int y = 0; y < s.h - 24; ++y)
        for (int x = 0; x < s.w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * s.w + x) * 4;
            const int d = std::abs(s.bgra[i] - empty.bgra[i]) + std::abs(s.bgra[i + 1] - empty.bgra[i + 1]) +
                          std::abs(s.bgra[i + 2] - empty.bgra[i + 2]);
            if (d > 30) ++n;
        }
    return n;
}

bool SavePng(const std::wstring& path, const Shot& s) {
    se::render::Image img;
    img.w = s.w;
    img.h = s.h;
    img.rgba.resize(s.bgra.size());
    for (std::size_t i = 0; i + 3 < s.bgra.size(); i += 4) {
        img.rgba[i] = s.bgra[i + 2];
        img.rgba[i + 1] = s.bgra[i + 1];
        img.rgba[i + 2] = s.bgra[i];
        img.rgba[i + 3] = 255;
    }
    std::string err;
    if (!se::render::WritePng(path, img, err)) {
        std::println("  PNG FAIL {}: {}", U8(path), err);
        return false;
    }
    return true;
}

void PrintLines(const std::wstring& text, int maxLines) {
    std::size_t a = 0;
    for (int i = 0; i < maxLines && a < text.size(); ++i) {
        std::size_t b = text.find(L'\n', a);
        if (b == std::wstring::npos) b = text.size();
        std::println("    | {}", U8(text.substr(a, b - a)));
        a = b + 1;
    }
}

LRESULT CALLBACK ParentProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring dataPath = std::wstring(kClient) + L"Data.pk2";
    const std::wstring partPath = std::wstring(kClient) + L"Particles.pk2";
    std::wstring err;
    auto data = bakb::Archive::open(dataPath, "169841", err);
    if (!data) { std::println("open Data.pk2 FAIL: {}", U8(err)); return 2; }
    auto part = bakb::Archive::open(partPath, "169841", err);
    if (!part) { std::println("open Particles.pk2 FAIL: {}", U8(err)); return 2; }
    std::println("archives: {} ({}), {} ({}), read-only: {}", U8(dataPath), U8(data->kind()), U8(partPath), U8(part->kind()),
                 !data->can_write() && !part->can_write() ? "yes" : "NO");

    // candidates
    std::vector<std::wstring> efps;
    if (argc > 1) efps.push_back(Lower(argv[1]));
    for (const auto& e : part->entries())
        if (!e.folder && EndsWith(Lower(e.path), L".efp") && Lower(e.path).find(L"skill") != std::wstring::npos) efps.push_back(Lower(e.path));
    std::wstring bsr = argc > 2 ? Lower(argv[2]) : L"";
    if (bsr.empty()) {
        std::wstring firstMob;
        for (const auto& e : data->entries()) {
            const std::wstring p = Lower(e.path);
            if (e.folder || !EndsWith(p, L".bsr")) continue;
            if (p == L"res/mob/china/tiger.bsr") { bsr = p; break; }
            if (firstMob.empty() && p.rfind(L"res/mob/", 0) == 0) firstMob = p;
        }
        if (bsr.empty()) bsr = firstMob;
    }
    std::println("candidates: {} skill .efp, model {}", efps.size(), U8(bsr));

    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring outDir = std::wstring(tmp) + L"bak2view_test\\";
    CreateDirectoryW(outDir.c_str(), nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ParentProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Bak2ViewTestParent";
    RegisterClassExW(&wc);
    HWND parent = CreateWindowExW(0, wc.lpszClassName, L"bak2view_test", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, 680, 520,
                                  nullptr, nullptr, wc.hInstance, nullptr);   // never shown
    if (!parent) { std::println("parent window FAIL"); return 3; }

    bak2::View view;
    if (!view.create(parent, 0, 0, 640, 480)) { std::println("View::create FAIL"); return 3; }
    std::println("view: hwnd {} gl={} visible(before show)={}", static_cast<void*>(view.hwnd()), view.using_gl(),
                 IsWindowVisible(view.hwnd()) != 0);
    view.show(true);   // child shown, parent stays hidden
    view.set_playing(false);
    int fails = 0;

    Shot empty;
    if (!Capture(view, empty)) { std::println("capture (empty) FAIL"); return 4; }

    // --- effect (Particles.pk2 + siblings)
    view.set_source([&part](const std::wstring& p, std::vector<std::uint8_t>& out) {
        std::wstring e;
        return part->read(p, out, e, /*siblings*/ true);
    });
    bool fxOk = false;
    for (std::size_t i = 0; i < efps.size() && i < 400 && !fxOk; ++i) {
        if (!view.load_effect(efps[i], err)) { if (i == 0) std::println("  load_effect {}: {}", U8(efps[i]), U8(err)); continue; }
        for (int k = 0; k < 16; ++k) view.step(50);   // 0.8 s
        Pump(60);
        Shot s;
        if (!Capture(view, s)) continue;
        const long long nb = NonBackground(s, empty);
        if (nb < 1500 && i + 1 < efps.size() && argc <= 1) continue;   // try a more visible one
        view.frame();
        view.step(50);
        Capture(view, s);
        const std::wstring png = outDir + L"effect.png";
        const bool saved = SavePng(png, s);
        std::println("EFFECT {}: frame {}x{}, non-background px {} -> {} {}", U8(efps[i]), s.w, s.h, NonBackground(s, empty),
                     U8(png), saved ? "saved" : "NOT saved");
        PrintLines(view.describe(), 8);
        fxOk = true;
    }
    if (!fxOk) { std::println("EFFECT FAIL: no effect rendered"); ++fails; }

    // --- model (Data.pk2 + siblings)
    view.set_source([&data](const std::wstring& p, std::vector<std::uint8_t>& out) {
        std::wstring e;
        return data->read(p, out, e, /*siblings*/ true);
    });
    if (!view.load_model(bsr, err)) {
        std::println("MODEL FAIL {}: {}", U8(bsr), U8(err));
        ++fails;
    } else {
        view.set_playing(true);
        Pump(700);   // the view's own WM_TIMER advances the idle clip
        view.set_playing(false);
        Shot s;
        if (!Capture(view, s)) { std::println("MODEL capture FAIL"); ++fails; }
        else {
            const std::wstring png = outDir + L"model.png";
            const bool saved = SavePng(png, s);
            const long long nb = NonBackground(s, empty);
            std::println("MODEL {}: frame {}x{}, non-background px {} -> {} {}", U8(bsr), s.w, s.h, nb, U8(png), saved ? "saved" : "NOT saved");
            PrintLines(view.describe(), 8);
            if (nb < 500) { std::println("MODEL FAIL: almost nothing drawn"); ++fails; }
        }
    }

    // --- lone .bms (first mesh of the model's folder family, through the synthesized resource)
    std::wstring bms;
    for (const auto& e : data->entries()) {
        const std::wstring p = Lower(e.path);
        if (!e.folder && p == L"prim/mesh/mob/china/tiger_part1.bms") { bms = p; break; }
        if (!e.folder && bms.empty() && EndsWith(p, L".bms") && p.find(L"tiger") != std::wstring::npos) bms = p;
    }
    if (!bms.empty()) {
        if (!view.load_model(bms, err)) { std::println("BMS FAIL {}: {}", U8(bms), U8(err)); ++fails; }
        else {
            view.step(50);
            Shot s;
            Capture(view, s);
            const std::wstring png = outDir + L"mesh.png";
            const bool saved = SavePng(png, s);
            std::println("BMS {}: frame {}x{}, non-background px {} -> {} {}", U8(bms), s.w, s.h, NonBackground(s, empty), U8(png),
                         saved ? "saved" : "NOT saved");
            PrintLines(view.describe(), 6);
        }
    }

    pk2::set_language(pk2::Lang::tr);   // describe() follows the UI language when it is asked for
    std::println("turkish describe first line: {}", U8(view.describe().substr(0, view.describe().find(L'\n'))));
    view.clear();
    DestroyWindow(parent);
    if (fails) std::println("RESULT: {} FAIL", fails);
    else std::println("RESULT: OK");
    return fails ? 1 : 0;
}
