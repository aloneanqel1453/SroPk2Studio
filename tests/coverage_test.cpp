// Preview coverage over real client archives (read-only): for every extension, which view the
// application's select_row() picks — texture, 3D, text, readable info, sound — and which files
// get no view. Mirrors App::select_row's branch order.
//
//   coverage_test [archive ...] [--per N] [--only ext]   (default: the archives in %SRO_CLIENT_DIR%, 60 per extension)
// Exit code 1 when any sampled file shows nothing, cannot be read, or its .2dt table does not round-trip.
#include "bak2_tools.hpp"
#include "bak_bridge.hpp"
#include "binary_info.hpp"
#include "test_client.hpp"
#include "dds.hpp"
#include "file_types.hpp"
#include "i18n.hpp"
#include "text.hpp"

#include <fcntl.h>
#include <io.h>


#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace pk2;

namespace {

std::wstring ext_of(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring base = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const std::size_t dot = base.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 == base.size()) return L"(yok)";
    return to_lower(base.substr(dot + 1));
}

std::wstring first_line(const std::wstring& s) {
    std::wstring l = s.substr(0, s.find(L'\n'));
    if (l.size() > 90) l = l.substr(0, 90) + L"…";
    return l;
}

bool image_ok(const std::vector<std::uint8_t>& b) {
    const std::uint8_t* p = b.data();
    std::size_t n = b.size();
    if (n >= 9236 && std::memcmp(p, "JMXVMAPT", 8) == 0) { p += 9236; n -= 9236; }
    return decode_image(p, n).has_value();
}

struct Outcome { std::wstring kind; std::wstring note; };

Outcome classify_view(const bakb::Archive& ar, const std::wstring& path, std::uint64_t size) {
    const FileCategory c = classify(path).category;
    if (size == 0) return {L"boş-dosya", {}};
    if (c == FileCategory::Sound) return {L"ses", {}};
    if (size > 16'000'000 && !(is_text(c) && size <= 128'000'000)) return {L"bilgi(büyük)", {}};
    std::vector<std::uint8_t> bytes;
    std::wstring err;
    if (!ar.read(path, bytes, err, true)) return {L"OKUNAMADI", err};
    // A view that would be the structural analysis (describe_binary) is reported as "analiz".
    bool generic = false;
    auto bak = [&](bool& has_text, bool& has_info, bool& has_3d, bool& has_img, std::wstring& note) {
        bakb::Preview pv;
        std::wstring e;
        if (!bytes.empty() && bakb::build_preview(&ar, path, bytes, 320, 240, pv, e)) {
            has_text = !pv.text.empty() && readable_text(pv.text) && !(pv.text_guess && !is_text_bytes(bytes));
            generic = !has_text && (pv.generic || pv.text_guess);
            has_info = !pv.summary.empty();
            has_3d = pv.image.width && pv.is_3d;
            has_img = pv.image.width && !pv.is_3d;
            note = first_line(has_text ? pv.text : pv.summary);
        } else note = e;
    };
    bool t = false, i = false, d3 = false, im = false;
    std::wstring note;
    switch (c) {
    case FileCategory::Texture: case FileCategory::Image:
        if (image_ok(bytes)) return {L"doku", {}};
        bak(t, i, d3, im, note);
        return im ? Outcome{L"doku(bak)", {}} : (t || i) ? Outcome{L"bilgi", note} : Outcome{L"BOŞ", note};
    case FileCategory::Resource: case FileCategory::Compound: case FileCategory::Mesh:
        bak(t, i, d3, im, note);
        if (d3) return {L"3B", {}};
        return i || t ? Outcome{L"bilgi", note} : Outcome{L"BOŞ", note};
    case FileCategory::Effect: case FileCategory::Material: {
        const std::wstring d = bakb::describe(path, bytes);
        return d.empty() ? Outcome{L"BOŞ", {}} : Outcome{L"bilgi", first_line(d)};
    }
    default: break;
    }
    if (image_ok(bytes)) return {L"doku", {}};
    const std::wstring lpath = to_lower(path);
    if (lpath.ends_with(L"sv.t")) return {L"sürüm", {}};
    if (lpath.ends_with(L"divisioninfo.txt") && !division_text(bytes).empty()) {
        std::vector<std::uint8_t> back;
        std::wstring e;
        if (!division_bytes(division_text(bytes), back, e) || back != bytes) return {L"DIVISION-HATA", e};
        return {L"bölgeler", {}};
    }
    if (lpath.ends_with(L"gateport.txt") && !gateport_text(bytes).empty()) {
        std::vector<std::uint8_t> back;
        std::wstring e;
        if (!gateport_bytes(gateport_text(bytes), bytes, back, e) || back != bytes) return {L"GATEPORT-HATA", e};
        return {L"port", {}};
    }
    if (bytes.size() <= 4096 && (is_text(c) || is_text_bytes(bytes))) {
        int enc = 0;
        if (decode_text(bytes, enc).find_first_not_of(L" \t\r\n﻿") == std::wstring::npos) return {L"boş-metin", {}};
    }
    if (bak2::is_skill_enc(bytes)) {
        // BAK-2's SkillCrypt: decrypt + encrypt must give the file back byte for byte.
        std::vector<std::uint8_t> plain;
        if (!bak2::skill_decrypt(bytes, plain) || plain.size() < 2 || plain[0] != 0xFF || plain[1] != 0xFE)
            return {L"ENC-HATA", L"çözülen metin BOM ile başlamıyor"};
        if (bak2::skill_encrypt(plain) != bytes) return {L"ENC-HATA", L"yeniden şifreleme farklı"};
        return {L"metin(ENC)", {}};
    }
    if (is_text(c) || is_text_bytes(bytes)) {
        std::vector<std::uint8_t> tb = bytes;
        if (tb.size() > 400'000) tb.resize(400'000);
        int enc = 0;
        if (readable_text(decode_text(tb, enc))) return {L"metin", {}};
    }
    if (!describe_client_record(path, bytes).empty()) return {L"kayıt", {}};
    bak(t, i, d3, im, note);
    if (bakb::ui2dt_is(bytes)) {
        // The editable table must reproduce the file byte for byte when nothing is changed.
        std::vector<std::uint8_t> copy = bytes;
        std::size_t changed = 0;
        std::wstring e;
        const bool ok = bakb::ui2dt_apply(copy, bakb::ui2dt_table(bytes), changed, e);
        if (!ok || changed != 0 || copy != bytes) return {L"2DT-TABLO-HATA", ok ? L"değişiklik olmadan fark" : e};
        return {im ? L"2dt(tablo+görsel)" : L"2dt(tablo)", {}};
    }
    if (d3) return {L"3B(bak)", note};
    if (t) return {L"metin(bak)", note};
    if (im) return {L"doku(bak)", note};
    if (generic) return {L"analiz", first_line(describe_binary(path, bytes))};
    if (i) return {L"bilgi", note};
    return {L"analiz", first_line(describe_binary(path, bytes))};
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    // --show <archive> <inner>: the full preview BAK builds for one file (summary, text head, image).
    if (argc == 4 && std::wstring(argv[1]) == L"--show") {
        std::wstring err;
        auto ar = bakb::Archive::open(argv[2], "169841", err);
        std::vector<std::uint8_t> bytes;
        if (!ar || !ar->read(argv[3], bytes, err, true)) { std::wprintf(L"!! %ls\n", err.c_str()); return 1; }
        bakb::Preview pv;
        const bool ok = bakb::build_preview(ar.get(), argv[3], bytes, 320, 240, pv, err);
        std::wprintf(L"bytes=%zu ok=%d format='%ls' image=%ux%u is_3d=%d err='%ls'\n--- summary\n%ls\n--- text (%zu)\n%ls\n",
                     bytes.size(), ok, pv.format.c_str(), pv.image.width, pv.image.height, pv.is_3d, err.c_str(),
                     pv.summary.c_str(), pv.text.size(), pv.text.substr(0, 1500).c_str());
        std::wprintf(L"--- head\n");
        for (std::size_t k = 0; k < std::min<std::size_t>(bytes.size(), 64); ++k) std::wprintf(L"%02X%ls", bytes[k], k % 16 == 15 ? L"\n" : L" ");
        std::wprintf(L"\n");
        return ok ? 0 : 1;
    }
    // --edit-selftest <Media.pk2>: the structured editors' write paths on real files, in memory only:
    // one .2dt field change touches exactly its 4 bytes; DIVISIONINFO / GATEPORT edits read back.
    if (argc == 3 && std::wstring(argv[1]) == L"--edit-selftest") {
        std::wstring err;
        auto ar = bakb::Archive::open(argv[2], "169841", err);
        if (!ar) { std::wprintf(L"!! %ls\n", err.c_str()); return 1; }
        int fails = 0;
        auto check = [&](bool ok, const wchar_t* what) { std::wprintf(L"  [%ls] %ls\n", ok ? L"PASS" : L"FAIL", what); fails += !ok; };
        for (const bakb::Entry& e : ar->entries()) {
            if (e.folder || !to_lower(e.path).ends_with(L".2dt")) continue;
            std::vector<std::uint8_t> b;
            if (!ar->read(e.path, b, err, false) || !bakb::ui2dt_is(b)) continue;
            std::wstring table = bakb::ui2dt_table(b);
            // Row 1, column X (index 5 after "#"): replace the value with 777.
            const std::size_t row1 = table.find(L"\r\n") + 2;
            std::size_t c = row1;
            for (int k = 0; k < 5; ++k) c = table.find(L'\t', c) + 1;
            const std::size_t end = table.find(L'\t', c);
            table.replace(c, end - c, L"777");
            std::vector<std::uint8_t> out = b;
            std::size_t changed = 0;
            const bool ok = bakb::ui2dt_apply(out, table, changed, err);
            std::size_t diff = 0, at = 0;
            for (std::size_t i = 0; i < b.size(); ++i) if (b[i] != out[i]) { ++diff; at = i; }
            std::int32_t x = 0;
            std::memcpy(&x, out.data() + 4 + 860, 4);
            check(ok && changed == 1 && x == 777 && diff <= 4 && (diff == 0 || (at >= 864 - 4 && at < 868)), L".2dt: X of row 1 -> 777, only those bytes change");
            std::wstring bad = table;
            bad.replace(c, 3, L"abc");
            std::vector<std::uint8_t> untouched = b;
            check(!bakb::ui2dt_apply(untouched, bad, changed, err) && untouched == b, L".2dt: a non-number is refused, file untouched");
            break;
        }
        std::vector<std::uint8_t> div;
        if (ar->read(L"DIVISIONINFO.TXT", div, err, false)) {
            // The editable text of every UI language (only its # comment line is translated) writes
            // the file back byte for byte.
            bool same = true;
            for (const Lang l : kLangs) {
                set_language(l);
                std::vector<std::uint8_t> rb;
                const std::wstring lt = division_text(div);
                same = same && !lt.empty() && division_bytes(lt, rb, err) && rb == div;
            }
            set_language(Lang::en);
            check(same, L"DIVISIONINFO: the text in all five UI languages writes the file back unchanged");
            std::wstring t = division_text(div);
            t += L"\r\n[DIV2]\r\ngw.example.org\r\n10.0.0.2\r\n";
            std::vector<std::uint8_t> nb;
            const bool ok = division_bytes(t, nb, err);
            const std::wstring back = division_text(nb);
            check(ok && back.find(L"[DIV2]") != std::wstring::npos && back.find(L"10.0.0.2") != std::wstring::npos, L"DIVISIONINFO: a second division with two gateways reads back");
        }
        std::vector<std::uint8_t> gp, out;
        if (ar->read(L"GATEPORT.TXT", gp, err, false)) {
            check(gateport_bytes(L"15880", gp, out, err) && gateport_text(out) == L"15880" && out.size() == gp.size(), L"GATEPORT: 15880 written in the same field size");
            check(!gateport_bytes(L"70000", gp, out, err), L"GATEPORT: 70000 refused");
        }
        std::wprintf(L"RESULT: %ls\n", fails ? L"FAIL" : L"OK");
        return fails ? 1 : 0;
    }
    // --list <archive> <path-substring>: inner paths and sizes of the matching files.
    if (argc == 4 && std::wstring(argv[1]) == L"--list") {
        std::wstring err;
        auto ar = bakb::Archive::open(argv[2], "169841", err);
        if (!ar) { std::wprintf(L"!! %ls\n", err.c_str()); return 1; }
        const std::wstring needle = to_lower(argv[3]);
        for (const bakb::Entry& e : ar->entries())
            if (!e.folder && to_lower(e.path).find(needle) != std::wstring::npos) std::wprintf(L"%10llu  %ls\n", e.size, e.path.c_str());
        return 0;
    }
    // --dump <archive> <inner> <file>: the payload as the archive hands it out (asset ciphers applied).
    if (argc == 5 && std::wstring(argv[1]) == L"--dump") {
        std::wstring err;
        auto ar = bakb::Archive::open(argv[2], "169841", err);
        std::vector<std::uint8_t> bytes;
        if (!ar || !ar->read(argv[3], bytes, err, true)) { std::wprintf(L"!! %ls\n", err.c_str()); return 1; }
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, argv[4], L"wb") != 0 || !fp) return 1;
        std::fwrite(bytes.data(), 1, bytes.size(), fp);
        std::fclose(fp);
        std::wprintf(L"%zu bytes\n", bytes.size());
        return 0;
    }
    std::vector<std::wstring> archives;
    std::size_t per = 60;
    std::wstring only;   // --only <ext>: just that extension
    for (int a = 1; a < argc; ++a) {
        if (std::wstring(argv[a]) == L"--per" && a + 1 < argc) per = static_cast<std::size_t>(_wtoi(argv[++a]));
        else if (std::wstring(argv[a]) == L"--only" && a + 1 < argc) only = to_lower(argv[++a]);
        else archives.emplace_back(argv[a]);
    }
    if (archives.empty())
        for (const wchar_t* n : {L"Media.pk2", L"Map.pk2", L"Particles.pk2", L"Music.pk2", L"Data.pk2"})
            archives.push_back(require_client_dir() + n);

    std::size_t bad = 0;
    for (const std::wstring& path : archives) {
        std::wstring err;
        auto ar = bakb::Archive::open(path, "169841", err);
        if (!ar) { std::wprintf(L"!! %ls: %ls\n", path.c_str(), err.c_str()); ++bad; continue; }
        std::map<std::wstring, std::vector<const bakb::Entry*>> by_ext;
        for (const bakb::Entry& e : ar->entries())
            if (!e.folder) by_ext[ext_of(e.path)].push_back(&e);
        std::wprintf(L"\n=== %ls (%zu uzantı)\n", path.c_str(), by_ext.size());
        for (auto& [ext, files] : by_ext) {
            if (!only.empty() && ext != only) continue;
            std::map<std::wstring, std::size_t> hist;
            std::map<std::wstring, std::vector<std::wstring>> samples;
            const std::size_t n = std::min(per, files.size());
            for (std::size_t k = 0; k < n; ++k) {
                const bakb::Entry* e = files[k * files.size() / n];   // spread over the archive
                const Outcome o = classify_view(*ar, e->path, e->size);
                ++hist[o.kind];
                auto& s = samples[o.kind];
                if (s.size() < (only.empty() ? 2u : 100000u)) s.push_back(e->path + (o.note.empty() ? L"" : L"  — " + o.note));
            }
            std::wstring line;
            bool flagged = false;
            for (auto& [kind, cnt] : hist) {
                line += L"  " + kind + L"=" + std::to_wstring(cnt);
                if (kind == L"BOŞ" || kind == L"OKUNAMADI" || kind == L"2DT-TABLO-HATA" || kind == L"DIVISION-HATA" || kind == L"GATEPORT-HATA" || kind == L"ENC-HATA") flagged = true;
            }
            std::wprintf(L"%-8ls %6zu |%ls\n", ext.c_str(), files.size(), line.c_str());
            for (auto& [kind, s] : samples) {
                const bool note_all = kind == L"BOŞ" || kind == L"OKUNAMADI" || kind == L"2DT-TABLO-HATA" || kind == L"bilgi" || kind == L"analiz";
                for (std::size_t k = 0; k < (note_all ? s.size() : 1); ++k)
                    std::wprintf(L"           %ls: %ls\n", kind.c_str(), s[k].c_str());
            }
            if (flagged) ++bad;
        }
    }
    std::wprintf(L"\nRESULT: %ls (%zu uzantıda boş / okunamayan / hatalı görünüm)\n", bad ? L"FAIL" : L"OK", bad);
    return bad ? 1 : 0;
}
