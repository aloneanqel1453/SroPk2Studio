// Console check of the BAK-2 EFP editor, skill database / playback and the "ultra" render path against the
// real archives (opened READ-ONLY through bakbridge).
//   bak2more_test [efp inner path] [skill id or code]
// Client: %SRO_CLIENT_DIR% (see test_client.hpp), key 169841. PNGs go to %TEMP%\bak2_more_test\.
#include "bak2_edit.hpp"
#include "i18n.hpp"
#include "test_client.hpp"
#include "bak2_skill.hpp"
#include "bak2_view.hpp"
#include "bak_bridge.hpp"

#include "bak2/render/RImage.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <format>
#include <map>
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

long long NonBackground(const Shot& s, const Shot& empty) {
    if (s.w != empty.w || s.h != empty.h) return -1;
    long long n = 0;
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * s.w + x) * 4;
            const int d = std::abs(s.bgra[i] - empty.bgra[i]) + std::abs(s.bgra[i + 1] - empty.bgra[i + 1]) +
                          std::abs(s.bgra[i + 2] - empty.bgra[i + 2]);
            if (d > 30) ++n;
        }
    return n;
}

// mean B,G,R of the pixels that differ from the empty view (the effect's colour)
void MeanColour(const Shot& s, const Shot& empty, double bgr[3]) {
    bgr[0] = bgr[1] = bgr[2] = 0;
    long long n = 0;
    for (std::size_t i = 0; i + 3 < s.bgra.size() && i + 3 < empty.bgra.size(); i += 4) {
        const int d = std::abs(s.bgra[i] - empty.bgra[i]) + std::abs(s.bgra[i + 1] - empty.bgra[i + 1]) + std::abs(s.bgra[i + 2] - empty.bgra[i + 2]);
        if (d <= 30) continue;
        for (int c = 0; c < 3; ++c) bgr[c] += s.bgra[i + static_cast<std::size_t>(c)];
        ++n;
    }
    if (n)
        for (int c = 0; c < 3; ++c) bgr[c] /= static_cast<double>(n);
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

void PrintLines(const std::wstring& text, int maxLines, const char* pre = "    | ") {
    std::size_t a = 0;
    for (int i = 0; i < maxLines && a < text.size(); ++i) {
        std::size_t b = text.find(L'\n', a);
        if (b == std::wstring::npos) b = text.size();
        std::println("{}{}", pre, U8(text.substr(a, b - a)));
        a = b + 1;
    }
}

LRESULT CALLBACK ParentProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

double Ms(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring err;
    auto data = bakb::Archive::open(std::wstring(kClient) + L"Data.pk2", "169841", err);
    if (!data) { std::println("open Data.pk2 FAIL: {}", U8(err)); return 2; }
    auto media = bakb::Archive::open(std::wstring(kClient) + L"Media.pk2", "169841", err);
    if (!media) { std::println("open Media.pk2 FAIL: {}", U8(err)); return 2; }
    auto part = bakb::Archive::open(std::wstring(kClient) + L"Particles.pk2", "169841", err);
    if (!part) { std::println("open Particles.pk2 FAIL: {}", U8(err)); return 2; }
    std::println("archives read-only: {}", !data->can_write() && !media->can_write() && !part->can_write() ? "yes" : "NO");
    // one reader for everything: Data first, then its siblings (Media / Particles / Map)
    const bak2::ReadFn read = [&data](const std::wstring& p, std::vector<std::uint8_t>& out) {
        std::wstring e;
        return data->read(p, out, e, /*siblings*/ true);
    };
    if (argc > 2 && std::wstring(argv[1]) == L"--dump") {   // bak2more_test --dump <inner path> [bytes]: raw bytes to stdout
        std::vector<std::uint8_t> b;
        if (!read(Lower(argv[2]), b)) { std::println("missing"); return 1; }
        const std::size_t n = std::min<std::size_t>(b.size(), argc > 3 ? static_cast<std::size_t>(_wtoi(argv[3])) : 600);
        std::fwrite(b.data(), 1, n, stdout);
        return 0;
    }

    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring outDir = std::wstring(tmp) + L"bak2_more_test\\";
    CreateDirectoryW(outDir.c_str(), nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ParentProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Bak2MoreTestParent";
    RegisterClassExW(&wc);
    HWND parent = CreateWindowExW(0, wc.lpszClassName, L"bak2more_test", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, 840, 640, nullptr,
                                  nullptr, wc.hInstance, nullptr);   // never shown
    if (!parent) { std::println("parent window FAIL"); return 3; }
    bak2::View view;
    if (!view.create(parent, 0, 0, 800, 600)) { std::println("View::create FAIL"); return 3; }
    view.show(true);
    view.set_playing(false);
    view.set_source(read);
    std::println("QUALITY gl={} samples={} anisotropy={:.0f} hw mipmaps={} (CPU fallback: 2x SSAA)", view.using_gl(), view.gl_samples(),
                 view.gl_anisotropy(), view.gl_hw_mipmaps());
    int fails = 0;
    Shot empty;
    if (!Capture(view, empty)) { std::println("capture (empty) FAIL"); return 4; }

    // ================================================================== 1. EFP editor
    {
        std::vector<std::wstring> efps;
        if (argc > 1) efps.push_back(Lower(argv[1]));
        for (const auto& e : part->entries())
            if (!e.folder && EndsWith(Lower(e.path), L".efp") && Lower(e.path).find(L"skill") != std::wstring::npos) efps.push_back(Lower(e.path));
        // lossless round trip over many effects
        int same = 0, opened = 0, diff = 0;
        std::wstring firstDiff;
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < efps.size() && i < 600; ++i) {
            std::vector<std::uint8_t> in, out;
            if (!read(efps[i], in)) continue;
            auto ed = bak2::EfpEditor::open(in, err, efps[i]);
            if (!ed) continue;
            ++opened;
            if (ed->build(out, err) && out == in) ++same;
            else { ++diff; if (firstDiff.empty()) firstDiff = efps[i] + L" " + err; }
        }
        std::println("EFP round trip: {} opened, {} byte-identical, {} differ ({:.0f} ms){}{}", opened, same, diff, Ms(t0),
                     firstDiff.empty() ? "" : " first: ", U8(firstDiff));
        if (opened == 0 || diff != 0) ++fails;

        // pick a visible effect with a texture, edit it, preview before / after
        bool done = false;
        for (std::size_t i = 0; i < efps.size() && i < 400 && !done; ++i) {
            std::vector<std::uint8_t> in;
            if (!read(efps[i], in)) continue;
            auto ed = bak2::EfpEditor::open(in, err, efps[i], read);
            if (!ed || ed->textures().empty()) continue;
            if (!view.load_effect_bytes(in, efps[i], err)) continue;
            for (int k = 0; k < 16; ++k) view.step(50);
            Shot before;
            if (!Capture(view, before) || NonBackground(before, empty) < 4000) continue;
            std::vector<std::uint8_t> same0;
            const bool unchangedOk = ed->build(same0, err) && same0 == in;
            bak2::EfpEditor::Params p;
            p.hue = 120;
            p.saturation = 1.4;
            p.scale = 1.6;
            p.add_missing_color = true;
            p.recolor_textures = true;
            ed->set_params(p);
            std::vector<bak2::EfpEditor::OutFile> files;
            if (!ed->build_files(files, err)) { std::println("EFP build FAIL {}: {}", U8(efps[i]), U8(err)); ++fails; break; }
            std::vector<std::pair<std::wstring, std::vector<std::uint8_t>>> extra;
            for (std::size_t f = 1; f < files.size(); ++f) extra.emplace_back(files[f].inner, files[f].data);
            if (!view.load_effect_bytes(files[0].data, efps[i], extra, err)) { std::println("load_effect_bytes FAIL: {}", U8(err)); ++fails; break; }
            for (int k = 0; k < 16; ++k) view.step(50);
            Shot after;
            Capture(view, after);
            double c0[3], c1[3];
            MeanColour(before, empty, c0);
            MeanColour(after, empty, c1);
            const bool s0 = SavePng(outDir + L"efp_before.png", before), s1 = SavePng(outDir + L"efp_after.png", after);
            std::println("EFP EDIT {}: {} bytes, {} nodes, textures {}, meshes {}, unchanged build identical: {}", U8(efps[i]), in.size(),
                         ed->node_count(), ed->textures().size(), ed->meshes().size(), unchangedOk ? "yes" : "NO");
            std::println("  edited: {} bytes + {} recoloured textures; px before {} after {}; mean BGR before ({:.0f},{:.0f},{:.0f}) after ({:.0f},{:.0f},{:.0f}); PNG {} {}",
                         files[0].data.size(), files.size() - 1, NonBackground(before, empty), NonBackground(after, empty), c0[0], c0[1], c0[2], c1[0],
                         c1[1], c1[2], s0 && s1 ? "saved" : "NOT saved", U8(outDir));
            PrintLines(ed->summary(), 14);
            // texture swap: the first texture replaced by the second distinct one of another effect
            ed->set_params({});
            std::wstring swapTo;
            for (std::size_t j = i + 1; j < efps.size() && j < i + 50 && swapTo.empty(); ++j) {
                std::vector<std::uint8_t> other;
                if (!read(efps[j], other)) continue;
                if (auto oe = bak2::EfpEditor::open(other, err))
                    for (const auto& t : oe->textures())
                        if (Lower(t) != Lower(ed->textures()[0])) { swapTo = t; break; }
            }
            if (!swapTo.empty()) {
                std::vector<std::uint8_t> sw;
                const bool ok = ed->replace_texture(0, swapTo, err) && ed->build(sw, err) && view.load_effect_bytes(sw, efps[i], err);
                std::println("  texture swap -> {}: {} ({} bytes, modified {})", U8(swapTo), ok ? "ok" : U8(L"FAIL " + err), sw.size(), ed->modified());
                if (!ok) ++fails;
            }
            if (!unchangedOk) ++fails;
            done = true;
        }
        if (!done) { std::println("EFP EDIT FAIL: no visible effect"); ++fails; }
    }

    // ================================================================== 2. skill database
    const auto t1 = std::chrono::steady_clock::now();
    auto db = bak2::SkillDb::load(read, err);
    if (!db) {
        std::println("SkillDb FAIL: {}", U8(err));
        ++fails;
    } else {
        pk2::set_language(pk2::Lang::en);   // names / describe() in English
        std::println("SKILLDB {} skills in {:.0f} ms: {}", db->size(), Ms(t1), U8(db->summary()));
        for (const auto& p : db->problems()) std::println("  problem: {}", U8(p));
        for (const wchar_t* q : {L"fire", L"sword"}) {
            const auto hits = db->search(q, 5);
            std::println("  search \"{}\": {} hit(s)", U8(q), hits.size());
            for (const auto& h : hits) std::println("    {} {} | {} | {}", h.id, U8(h.code), U8(h.name), U8(h.group));
        }
        // skill to play: argument, else a sword skill with presentation rows, else the first search hit
        int sid = 0;
        if (argc > 2) {
            sid = _wtoi(argv[2]);
            if (!sid)
                for (const auto& h : db->search(argv[2], 1)) sid = h.id;
        }
        if (!sid)
            for (const wchar_t* q : {L"SKILL_CH_SWORD_SMASH_A", L"SKILL_CH_SWORD", L"SKILL_CH"})
                for (const auto& h : db->search(q, 40)) {
                    const std::wstring d = db->describe(h.id);
                    if (!sid && d.find(L"skilleffectset: 0") == std::wstring::npos) sid = h.id;
                }
        std::println("  describe {}:", sid);
        PrintLines(db->describe(sid), 14);

        // ============================================================== 3. playback
        std::map<std::wstring, int> sounds;
        int soundCalls = 0;
        view.set_sound_callback([&](const std::wstring& p, int vol) {
            ++soundCalls;
            ++sounds[p + L" @" + std::to_wstring(vol)];
        });
        auto play = [&](int id, const wchar_t* tag) {
            const auto t2 = std::chrono::steady_clock::now();
            if (!view.play_skill(*db, id, err)) {
                std::println("PLAY {} FAIL: {}", id, U8(err));
                ++fails;
                return;
            }
            view.set_playing(false);
            std::println("PLAY {} ({}) started in {:.0f} ms", id, U8(tag), Ms(t2));
            long long bestPx = -1;
            for (int k = 1; k <= 40; ++k) {   // 2 s in 50 ms steps (the effects' 20 Hz clock)
                view.step(50);
                if (k == 12 || k == 24 || k == 40) {
                    Pump(20);
                    Shot s;
                    if (!Capture(view, s)) { std::println("  capture FAIL"); ++fails; continue; }
                    const std::wstring png = outDir + std::format(L"skill_{}_{}ms.png", tag, k * 50);
                    const long long nb = NonBackground(s, empty);
                    bestPx = std::max(bestPx, nb);
                    std::println("  t={} ms non-background px {} -> {} {}", k * 50, nb, U8(png), SavePng(png, s) ? "saved" : "NOT saved");
                }
            }
            PrintLines(view.describe(), 16);
            if (bestPx < 2000) { std::println("  FAIL: almost nothing drawn"); ++fails; }
        };
        play(sid, L"player");
        // a monster skill too (caster = the monster that uses it)
        int mid = 0;
        for (const auto& h : db->search(L"MSKILL_CH_TIGERWOMAN", 20))
            if (!mid && db->describe(h.id).find(L"skilleffectset: 0") == std::wstring::npos) mid = h.id;
        if (!mid)
            for (const auto& h : db->search(L"MSKILL_", 60))
                if (!mid && db->describe(h.id).find(L"skilleffectset: 0") == std::wstring::npos) mid = h.id;
        if (mid) play(mid, L"monster");
        std::println("SOUND callbacks: {} calls, {} distinct", soundCalls, sounds.size());
        for (const auto& [k, n] : sounds) std::println("    {} x{}", U8(k), n);
        view.set_overlay(true);   // overlay switch: the BAK-2 text comes back on demand
        view.step(50);
        Shot o;
        if (Capture(view, o)) SavePng(outDir + L"skill_overlay_on.png", o);
        view.set_overlay(false);
    }

    view.clear();
    DestroyWindow(parent);
    if (fails) std::println("RESULT: {} FAIL", fails);
    else std::println("RESULT: OK");
    return fails ? 1 : 0;
}
