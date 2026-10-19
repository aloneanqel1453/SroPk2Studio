// Console check of the bak2 tools bridge against the real Media.pk2 (opened READ-ONLY through bakbridge).
//   bak2tools_test [media.pk2]
// .ddj decode -> PNG -> encode_ddj (old file's layout) -> decode again; skilldata_*enc.txt decrypt / encrypt
// round trip; table_validate + byte-exact rewrite of textdata tables; text_diff of an edited copy.
// Output files go to %TEMP%\bak2tools_test\ (never into the client).
#include "bak2_tools.hpp"
#include "test_client.hpp"
#include "bak_bridge.hpp"

#include "bak2/core/Text.h"
#include "bak2/ddj/ImageIo.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <print>
#include <string>
#include <vector>

namespace {

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

void PrintLines(const std::wstring& text, int maxLines, std::size_t maxChars = 140) {
    std::size_t a = 0;
    for (int i = 0; i < maxLines && a < text.size(); ++i) {
        std::size_t b = text.find(L'\n', a);
        if (b == std::wstring::npos) b = text.size();
        std::wstring line = text.substr(a, std::min(b - a, maxChars));
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::println("    | {}", U8(line));
        a = b + 1;
    }
}

bool SaveFile(const std::wstring& path, const std::vector<std::uint8_t>& b) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

// BGRA (decode_image_bgra) -> PNG with BAK-2's own writer (ddj/ImageIo EncodePngRgba).
std::vector<std::uint8_t> ToPng(const std::vector<std::uint8_t>& bgra, int w, int h) {
    se::ddj::Image img;
    img.w = w;
    img.h = h;
    img.rgba.resize(bgra.size());
    for (std::size_t i = 0; i + 3 < bgra.size(); i += 4) {
        img.rgba[i] = bgra[i + 2];
        img.rgba[i + 1] = bgra[i + 1];
        img.rgba[i + 2] = bgra[i];
        img.rgba[i + 3] = bgra[i + 3];
    }
    std::vector<std::uint8_t> png;
    se::ddj::EncodePngRgba(img, png);
    return png;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring mediaPath, find;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--find" && i + 1 < argc) find = Lower(argv[++i]);
        else mediaPath = argv[i];
    }
    if (mediaPath.empty()) mediaPath = require_client_dir() + L"Media.pk2";
    std::wstring err;
    auto media = bakb::Archive::open(mediaPath, "169841", err);
    if (!media) { std::println("open Media.pk2 FAIL: {}", U8(err)); return 2; }
    if (!find.empty()) {   // --find substring: list matching entries (path, size) and stop
        for (const auto& e : media->entries())
            if (!e.folder && Lower(e.path).find(find) != std::wstring::npos) std::println("  {}  {} B", U8(e.path), e.size);
        return 0;
    }
    std::println("archive: {} ({}), read-only: {}", U8(mediaPath), U8(media->kind()), media->can_write() ? "NO" : "yes");
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring outDir = std::wstring(tmp) + L"bak2tools_test\\";
    CreateDirectoryW(outDir.c_str(), nullptr);
    int fails = 0;

    std::wstring ddj, enc, skilldata, skilleffect, chardata;
    for (const auto& e : media->entries()) {
        if (e.folder) continue;
        const std::wstring p = Lower(e.path);
        if (ddj.empty() && EndsWith(p, L".ddj") && p.rfind(L"icon/", 0) == 0) ddj = p;
        const bool sd = p.find(L"textdata/skilldata_") != std::wstring::npos && EndsWith(p, L".txt");
        if (enc.empty() && sd && EndsWith(p, L"enc.txt")) enc = p;
        if (skilldata.empty() && sd && !EndsWith(p, L"enc.txt")) skilldata = p;
        if (skilleffect.empty() && EndsWith(p, L"textdata/skilleffect.txt") && e.size > 16) skilleffect = p;
        if (chardata.empty() && p.find(L"textdata/characterdata_") != std::wstring::npos && EndsWith(p, L".txt")) chardata = p;
    }
    auto read = [&](const std::wstring& p, std::vector<std::uint8_t>& b) {
        std::wstring e;
        if (media->read(p, b, e)) return true;
        std::println("  read FAIL {}: {}", U8(p), U8(e));
        return false;
    };

    // --- DDJ: decode -> PNG -> encode_ddj like the old file -> decode again
    std::vector<std::uint8_t> old;
    if (ddj.empty() || !read(ddj, old)) { std::println("DDJ FAIL: no icon .ddj"); ++fails; }
    else {
        int w = 0, h = 0;
        std::vector<std::uint8_t> px;
        if (!bak2::decode_image_bgra(old, w, h, px, &err)) { std::println("DDJ decode FAIL {}: {}", U8(ddj), U8(err)); ++fails; }
        else {
            const std::vector<std::uint8_t> png = ToPng(px, w, h);
            SaveFile(outDir + L"icon.png", png);
            std::vector<std::uint8_t> again;
            if (!bak2::encode_ddj(png, &old, again, err)) { std::println("encode_ddj FAIL: {}", U8(err)); ++fails; }
            else {
                int w2 = 0, h2 = 0;
                std::vector<std::uint8_t> px2;
                bak2::decode_image_bgra(again, w2, h2, px2);
                int maxDiff = 0;
                if (w2 == w && h2 == h)
                    for (std::size_t i = 0; i < px.size(); ++i) maxDiff = std::max(maxDiff, std::abs(px[i] - px2[i]));
                const std::size_t hdr = std::min<std::size_t>(148, std::min(old.size(), again.size()));
                const bool sameHeader = std::equal(old.begin(), old.begin() + static_cast<std::ptrdiff_t>(hdr), again.begin());
                SaveFile(outDir + L"icon_again.ddj", again);
                std::println("DDJ {}: {}x{}, {} B -> PNG {} B -> ddj {} B, header(148) same: {}, max pixel diff {}, bytes identical: {}",
                             U8(ddj), w, h, old.size(), png.size(), again.size(), sameHeader, maxDiff, again == old);
                std::vector<std::uint8_t> fresh;
                bak2::encode_ddj(png, nullptr, fresh, err);
                std::println("  new-file layout (no old): {} B, starts {}", fresh.size(),
                             fresh.size() >= 12 ? std::string(fresh.begin(), fresh.begin() + 12) : std::string("?"));
                if (w2 != w || h2 != h || !sameHeader) ++fails;
            }
        }
    }

    // --- skilldata ENC: a shipped ENC file, else a plain skilldata table encrypted here (in memory) and back
    std::vector<std::uint8_t> encBytes;
    std::wstring encName = enc;
    if (enc.empty()) {
        std::vector<std::uint8_t> plainShipped;
        if (!skilldata.empty() && read(skilldata, plainShipped)) {
            encBytes = bak2::skill_encrypt(plainShipped);
            encName = skilldata.substr(0, skilldata.size() - 4) + L"enc.txt";
            std::println("SKILL ENC: no skilldata_*enc.txt in this Media.pk2; {} encrypted in memory ({} B -> {} B, is_skill_enc {} / plain {})",
                         U8(skilldata), plainShipped.size(), encBytes.size(), bak2::is_skill_enc(encBytes), bak2::is_skill_enc(plainShipped));
            std::vector<std::uint8_t> back;
            if (!bak2::skill_decrypt(encBytes, back) || back != plainShipped) { std::println("  decrypt(encrypt(x)) != x FAIL"); ++fails; }
        }
    } else {
        read(enc, encBytes);
    }
    if (!encBytes.empty()) {
        std::vector<std::uint8_t> plain;
        if (!bak2::skill_decrypt(encBytes, plain)) { std::println("SKILL ENC {}: decrypt FAIL", U8(encName)); ++fails; }
        else {
            const bool back = bak2::skill_encrypt(plain) == encBytes;
            std::println("SKILL ENC {}: {} B -> {} B plain, encrypt again byte-identical: {}; first lines:", U8(encName), encBytes.size(),
                         plain.size(), back);
            PrintLines(se::DecodeText(plain), 3);
            if (!back) ++fails;
        }
    }

    // --- tables (+ the ENC form, validated through EdDoc's decrypt / re-encrypt)
    auto checkTable = [&](const std::wstring& name, const std::vector<std::uint8_t>& b) {
        bak2::TableCheck tc;
        const bool ok = bak2::table_validate(name, b, tc);
        std::println("TABLE {}: validate={} errors={} warnings={} infos={} rows={} roundtrip={} (table_roundtrip_ok={})", U8(name), ok,
                     tc.errors, tc.warnings, tc.infos, tc.rows, tc.roundtrip, bak2::table_roundtrip_ok(b, name));
        PrintLines(tc.report, 4);
        if (!ok || !tc.roundtrip) ++fails;
    };
    for (const std::wstring& t : {skilleffect, chardata, skilldata}) {
        std::vector<std::uint8_t> b;
        if (!t.empty() && read(t, b)) checkTable(t, b);
    }
    if (!encBytes.empty()) checkTable(encName, encBytes);

    // --- text diff: a table's text with one line added and one removed
    const std::wstring diffSrc = !skilleffect.empty() ? skilleffect : chardata;
    if (!diffSrc.empty()) {
        std::vector<std::uint8_t> b;
        if (read(diffSrc, b)) {
            const std::wstring a = se::DecodeText(b);
            std::wstring m = a;
            const std::size_t l1 = m.find(L'\n', m.size() / 3);
            if (l1 != std::wstring::npos) m.insert(l1 + 1, L"#bak2tools_test added row\r\n");
            const std::size_t l2 = m.find(L'\n', m.size() / 2);
            if (l2 != std::wstring::npos) {
                const std::size_t e2 = m.find(L'\n', l2 + 1);
                if (e2 != std::wstring::npos) m.erase(l2 + 1, e2 - l2);
            }
            const std::wstring d = bak2::text_diff(a, m);
            std::println("TEXT DIFF {} ({} chars, one row added, one removed):", U8(diffSrc), a.size());
            PrintLines(d, 12, 110);
            if (d.find(L"#bak2tools_test added row") == std::wstring::npos) ++fails;
        }
    }
    if (fails) std::println("RESULT: {} FAIL", fails);
    else std::println("RESULT: OK");
    return fails ? 1 : 0;
}
