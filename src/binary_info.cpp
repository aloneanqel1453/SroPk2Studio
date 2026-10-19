#include "binary_info.hpp"

#include "i18n.hpp"
#include "text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace pk2 {
namespace {

std::wstring base_lower(std::wstring_view name) {
    const std::size_t s = name.find_last_of(L"\\/");
    return to_lower(s == std::wstring_view::npos ? name : name.substr(s + 1));
}

std::uint16_t rd16(const std::uint8_t* p) { std::uint16_t v; std::memcpy(&v, p, 2); return v; }
std::uint32_t rd32(const std::uint8_t* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
float rdf(const std::uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; }

std::wstring fmt(const wchar_t* f, auto... a) {
    wchar_t buf[512];
    const int n = swprintf(buf, std::size(buf), f, a...);
    return n > 0 ? std::wstring(buf, static_cast<std::size_t>(n)) : std::wstring();
}

std::wstring size_text(std::size_t n) {
    if (n < 1024) return Lf("bi_bytes", n);
    if (n < 1024 * 1024) return std::format(L"{:.1f} KB ({})", static_cast<double>(n) / 1024.0, Lf("bi_bytes", n));
    return std::format(L"{:.2f} MB ({})", static_cast<double>(n) / (1024.0 * 1024.0), Lf("bi_bytes", n));
}

// Columns `s` takes in a monospace font: CJK and full-width characters take two.
std::size_t columns(std::wstring_view s) {
    std::size_t n = 0;
    for (const wchar_t c : s)
        n += (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) ||
             (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) ||
             (c >= 0xFFE0 && c <= 0xFFE6) ? 2 : 1;
    return n;
}

// A "Label:" from the table, padded with spaces so the values after it start at column `width`.
std::wstring label(std::string_view key, std::size_t width) {
    std::wstring s = L(key);
    const std::size_t c = columns(s);
    s.append(c < width ? width - c : 1, L' ');
    return s;
}

// Closing line of an analysis: no decoder, the bytes are edited with the hex editor.
std::wstring no_decoder() { return L"\n" + Lf("bi_no_decoder", L("edit_hex")); }

std::wstring guid_text(const std::uint8_t* p) {
    return fmt(L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", rd32(p), rd16(p + 4), rd16(p + 6),
               p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
}

// A float that is a real quantity (coordinate, angle, scale), not reinterpreted integer bits.
bool plausible_float(float v) {
    if (!std::isfinite(v)) return false;
    const float a = std::fabs(v);
    return v == 0.0f || (a > 1e-4f && a < 1e6f);
}

std::wstring float_text(float v) {
    std::wstring s = fmt(L"%.4f", static_cast<double>(v));
    while (!s.empty() && s.back() == L'0') s.pop_back();
    if (!s.empty() && s.back() == L'.') s.pop_back();
    return s == L"-0" ? L"0" : s;
}

// Run of plausible float32 values starting at `off`, 4-byte steps.
std::vector<float> float_run(const std::vector<std::uint8_t>& b, std::size_t off, std::size_t max_count) {
    std::vector<float> out;
    for (std::size_t o = off; o + 4 <= b.size() && out.size() < max_count; o += 4) {
        const float v = rdf(b.data() + o);
        if (!plausible_float(v)) break;
        out.push_back(v);
    }
    return out;
}

// Well-known container / file signatures (anything that is not Silkroad's own JMXV family).
struct Magic { const char* sig; std::size_t len; const char* key; };   // key: i18n text of the kind
constexpr Magic kMagics[] = {
    {"MZ", 2, "bi_mg_pe"},
    {"PK\x03\x04", 4, "bi_mg_zip"},
    {"Rar!", 4, "bi_mg_rar"},
    {"7z\xBC\xAF", 4, "bi_mg_7z"},
    {"MSCF", 4, "bi_mg_cab"},
    {"RIFF", 4, "bi_mg_riff"},
    {"OggS", 4, "bi_mg_ogg"},
    {"ID3", 3, "bi_mg_mp3"},
    {"\x89PNG", 4, "bi_mg_png"},
    {"GIF8", 4, "bi_mg_gif"},
    {"\xFF\xD8\xFF", 3, "bi_mg_jpeg"},
    {"DDS ", 4, "bi_mg_dds"},
    {"%PDF", 4, "bi_mg_pdf"},
    {"SQLite format 3", 15, "bi_mg_sqlite"},
    {"\xD0\xCF\x11\xE0", 4, "bi_mg_ole"},
    {"\x1F\x8B", 2, "bi_mg_gzip"},
    {"\x78\x9C", 2, "bi_mg_zlib"},
    {"\x28\xB5\x2F\xFD", 4, "bi_mg_zstd"},
    {"JoyMax File Manager!", 20, "bi_mg_pk2"},
};

// PE header facts: machine, DLL or program, subsystem, link date, section count.
std::wstring describe_pe(const std::vector<std::uint8_t>& b) {
    if (b.size() < 0x40) return {};
    const std::uint32_t pe = rd32(b.data() + 0x3C);
    if (pe + 24 > b.size() || std::memcmp(b.data() + pe, "PE\0\0", 4) != 0) return {};
    const std::uint16_t machine = rd16(b.data() + pe + 4), sections = rd16(b.data() + pe + 6);
    const std::uint32_t stamp = rd32(b.data() + pe + 8);
    const std::uint16_t chars = rd16(b.data() + pe + 22);
    std::wstring m = machine == 0x14C ? L"x86 (32-bit)" : machine == 0x8664 ? L"x64 (64-bit)" :
                     machine == 0xAA64 ? L"ARM64" : fmt(L"%u", machine);
    std::wstring out;
    out += label("bi_kind", 11) + ((chars & 0x2000) ? L("bi_pe_dll") : L("bi_pe_exe")) + L"\n";
    out += label("bi_machine", 11) + m + L"\n";
    out += label("bi_sections", 11) + std::to_wstring(sections) + L"\n";
    if (stamp) {
        const std::time_t t = static_cast<std::time_t>(stamp);
        std::tm tm{};
        if (gmtime_s(&tm, &t) == 0)
            out += label("bi_linked", 11) +
                   fmt(L"%04d-%02d-%02d %02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min) + L"\n";
    }
    return out;
}

struct FoundString { std::size_t off; std::wstring text; };

// Readable runs of at least 4 characters: ASCII, and UTF-16LE with ASCII-range characters.
std::vector<FoundString> embedded_strings(const std::vector<std::uint8_t>& b, std::size_t limit, std::size_t& total) {
    std::vector<FoundString> out;
    total = 0;
    auto printable = [](std::uint32_t c) { return c >= 0x20 && c < 0x7F; };
    const std::size_t n = std::min<std::size_t>(b.size(), 8u << 20);
    for (std::size_t i = 0; i < n;) {
        std::size_t j = i;
        while (j < n && printable(b[j])) ++j;
        if (j - i >= 4) {
            ++total;
            if (out.size() < limit) {
                std::wstring s(b.begin() + static_cast<std::ptrdiff_t>(i), b.begin() + static_cast<std::ptrdiff_t>(std::min(j, i + 96)));
                if (j - i > 96) s += L"…";
                out.push_back({i, std::move(s)});
            }
            i = j;
            continue;
        }
        // UTF-16LE run (character, 0x00, character, 0x00, ...)
        std::size_t k = i;
        std::wstring w;
        while (k + 1 < n && printable(b[k]) && b[k + 1] == 0) { w.push_back(static_cast<wchar_t>(b[k])); k += 2; }
        if (w.size() >= 4) {
            ++total;
            if (out.size() < limit) {
                if (w.size() > 96) { w.resize(96); w += L"…"; }
                out.push_back({i, L"\"" + w + L"\" (UTF-16)"});
            }
            i = k;
            continue;
        }
        i = (j > i) ? j : i + 1;
    }
    return out;
}

// JMXVIMG1 (Media\fonts\*.dat): "JMXVIMG1" + 4-digit version, u16 width, u16 height, then one
// coverage byte per pixel, rows top-down.
bool jmx_glyph(const std::vector<std::uint8_t>& b, std::uint32_t& w, std::uint32_t& h) {
    if (b.size() < 16 || std::memcmp(b.data(), "JMXVIMG1", 8) != 0) return false;
    w = rd16(b.data() + 12);
    h = rd16(b.data() + 14);
    return w && h && w <= 4096 && h <= 4096 && 16 + static_cast<std::size_t>(w) * h <= b.size();
}

bool is_palette(std::wstring_view name, const std::vector<std::uint8_t>& b) {
    const std::wstring base = base_lower(name);
    return b.size() == 768 && (base.ends_with(L".dat") || base.ends_with(L".pal")) && !is_text_bytes(b);
}

} // namespace

bool record_image(std::wstring_view name, const std::vector<std::uint8_t>& b, Image& out) {
    std::uint32_t gw = 0, gh = 0;
    if (jmx_glyph(b, gw, gh)) {
        out.width = gw;
        out.height = gh;
        out.format = "JMXVIMG1";
        out.bgra.assign(static_cast<std::size_t>(gw) * gh * 4, 0xFF);
        for (std::size_t i = 0; i < static_cast<std::size_t>(gw) * gh; ++i) out.bgra[i * 4 + 3] = b[16 + i];   // white, coverage = alpha
        return true;
    }
    if (!is_palette(name, b)) return false;
    constexpr std::uint32_t cell = 16, grid = 16, side = cell * grid;
    out.width = out.height = side;
    out.format = "RGB palette";
    out.bgra.assign(static_cast<std::size_t>(side) * side * 4, 0);
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y / cell) * grid + x / cell;
            const bool edge = (x % cell) == 0 || (y % cell) == 0;   // 1 px grid line between swatches
            std::uint8_t* px = out.bgra.data() + (static_cast<std::size_t>(y) * side + x) * 4;
            px[0] = edge ? 0x10 : b[idx * 3 + 2];
            px[1] = edge ? 0x10 : b[idx * 3 + 1];
            px[2] = edge ? 0x10 : b[idx * 3 + 0];
            px[3] = 0xFF;
        }
    return true;
}

std::wstring describe_client_record(std::wstring_view name, const std::vector<std::uint8_t>& b) {
    const std::wstring base = base_lower(name);

    // plugin.dat: u32 count, then per plugin a GUID, a u16 name length and the DLL name (NUL padded).
    if (base == L"plugin.dat" && b.size() >= 4) {
        const std::uint32_t count = rd32(b.data());
        std::wstring list;
        std::size_t off = 4, parsed = 0;
        for (; parsed < count && parsed < 64 && off + 18 <= b.size(); ++parsed) {
            const std::wstring id = guid_text(b.data() + off);
            const std::uint16_t len = rd16(b.data() + off + 16);
            if (len == 0 || off + 18 + len > b.size()) break;
            const std::string dll(reinterpret_cast<const char*>(b.data() + off + 18), len);
            if (!std::all_of(dll.begin(), dll.end(), [](char c) { return c >= 0x20 && c < 0x7F; })) break;
            list += fmt(L"  %zu. %ls\n     GUID: %ls\n", parsed + 1, acp_to_wide(dll).c_str(), id.c_str());
            off += 18 + len;
            while (off < b.size() && b[off] == 0) ++off;   // name field padding
        }
        if (parsed == count && count > 0)
            return L("bi_plugin_title") + L"\n\n" + Lf("bi_plugin_count", count) + L"\n\n" + list;
    }

    // vssver2.scc: Visual SourceSafe bookkeeping the developers left in the archive.
    if (base == L"vssver2.scc") return L("bi_vss");

    // JMXVCAMR: camera record ("JMXVCAMR" + 4-digit version, then the camera values as float32).
    if (b.size() >= 16 && std::memcmp(b.data(), "JMXVCAMR", 8) == 0) {
        const std::string ver(reinterpret_cast<const char*>(b.data() + 8), 4);
        // The float block follows a short header; take the alignment that yields the longest run.
        std::vector<float> best;
        std::size_t best_off = 0;
        for (std::size_t off = 12; off < 24 && off < b.size(); ++off) {
            auto run = float_run(b, off, 64);
            const bool nonzero = std::any_of(run.begin(), run.end(), [](float v) { return v != 0.0f; });
            if (nonzero && run.size() > best.size()) { best = std::move(run); best_off = off; }
        }
        std::wstring out = Lf("bi_cam_title", acp_to_wide(ver)) + L"\n\n";
        out += label("bi_size", 8) + size_text(b.size()) + L"\n";
        if (best_off > 12) {
            out += label("bi_header", 8);
            for (std::size_t o = 12; o + 2 <= best_off; o += 2) out += std::to_wstring(rd16(b.data() + o)) + L" ";
            out += L"\n";
        }
        if (!best.empty()) {
            out += L"\n" + Lf("bi_cam_values", best.size()) + L"\n";
            for (std::size_t i = 0; i < best.size(); i += 3) {
                out += L"  ";
                for (std::size_t k = i; k < std::min(best.size(), i + 3); ++k) out += float_text(best[k]) + L"   ";
                out += L"\n";
            }
        }
        return out;
    }

    // JMXVIMG1: one glyph of the client's bitmap font (the file name is the character).
    if (std::uint32_t gw = 0, gh = 0; jmx_glyph(b, gw, gh)) {
        const std::string ver(reinterpret_cast<const char*>(b.data() + 8), 4);
        const std::wstring stem = base.substr(0, base.find_last_of(L'.'));
        std::wstring out = Lf("bi_glyph_title", acp_to_wide(ver)) + L"\n\n";
        if (stem.size() == 1) out += label("bi_character", 11) + stem + L"\n";
        out += label("bi_size", 11) + Lf("bi_glyph_size", gw, gh) + L"\n";
        out += L"\n" + Lf("bi_glyph_on_tab", L("tab_tex"));
        return out;
    }

    // 768 bytes of RGB triplets: a 256-colour palette (silk.dat).
    if (is_palette(name, b)) {
        std::wstring out = Lf("bi_palette", L("tab_tex")) + L"\n";
        for (std::size_t i = 0; i < 16; ++i)
            out += fmt(L"  %2zu: R %3u  G %3u  B %3u\n", i, b[i * 3], b[i * 3 + 1], b[i * 3 + 2]);
        return out;
    }
    return {};
}

std::wstring division_text(const std::vector<std::uint8_t>& b) {
    // Validate the whole structure first: every length inside the buffer, the buffer used exactly.
    std::size_t p = 0;
    auto u8 = [&](std::uint8_t& v) { if (p + 1 > b.size()) return false; v = b[p++]; return true; };
    auto str = [&](std::string& s) {
        if (p + 4 > b.size()) return false;
        const std::uint32_t n = rd32(b.data() + p);
        if (n == 0 || n > 256 || p + 4 + n + 1 > b.size() || b[p + 4 + n] != 0) return false;
        s.assign(reinterpret_cast<const char*>(b.data() + p + 4), n);
        p += 4 + n + 1;
        return std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x20 && c < 0x7F; });
    };
    std::uint8_t locale = 0, divisions = 0;
    if (!u8(locale) || !u8(divisions) || divisions == 0) return {};
    // The comment is the only translated line (division_bytes skips it); "locale" is the parsed key.
    std::wstring out = L"# " + L("bi_div_comment") + L"\r\n";
    out += L"locale = " + std::to_wstring(locale) + L"\r\n";
    for (std::uint8_t d = 0; d < divisions; ++d) {
        std::string name;
        std::uint8_t gateways = 0;
        if (!str(name) || !u8(gateways)) return {};
        out += L"\r\n[" + acp_to_wide(name) + L"]\r\n";
        for (std::uint8_t g = 0; g < gateways; ++g) {
            std::string host;
            if (!str(host)) return {};
            out += acp_to_wide(host) + L"\r\n";
        }
    }
    return p == b.size() ? out : std::wstring();
}

bool division_bytes(const std::wstring& text, std::vector<std::uint8_t>& out, std::wstring& err) {
    struct Division { std::string name; std::vector<std::string> hosts; };
    std::vector<Division> divs;
    long locale = -1;
    std::size_t line_no = 0;
    auto ascii = [](const std::wstring& s, std::string& o) {
        o.clear();
        for (wchar_t c : s) { if (c < 0x20 || c >= 0x7F) return false; o.push_back(static_cast<char>(c)); }
        return !o.empty() && o.size() <= 256;
    };
    for (std::size_t pos = 0; pos <= text.size();) {
        std::size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = text.substr(pos, eol - pos);
        pos = eol + 1;
        ++line_no;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t')) line.pop_back();
        const std::size_t first = line.find_first_not_of(L" \t");
        if (first == std::wstring::npos || line[first] == L'#') continue;
        line.erase(0, first);
        const std::wstring where = Lf("bi_line", line_no);
        if (line.rfind(L"locale", 0) == 0) {
            const std::size_t eq = line.find(L'=');
            wchar_t* end = nullptr;
            const std::wstring v = eq == std::wstring::npos ? L"" : line.substr(eq + 1);
            locale = std::wcstol(v.c_str(), &end, 10);
            while (end && *end == L' ') ++end;
            if (eq == std::wstring::npos || !end || *end || locale < 0 || locale > 255) {
                err = where + L("bi_div_locale_bad");
                return false;
            }
        } else if (line.front() == L'[') {
            if (line.back() != L']' || line.size() < 3) { err = where + L("bi_div_name_form"); return false; }
            Division d;
            if (!ascii(line.substr(1, line.size() - 2), d.name)) { err = where + L("bi_div_name_ascii"); return false; }
            divs.push_back(std::move(d));
        } else {
            if (divs.empty()) { err = where + L("bi_div_host_parent"); return false; }
            std::string host;
            if (!ascii(line, host) || host.find(' ') != std::string::npos) { err = where + L("bi_div_host_ascii"); return false; }
            divs.back().hosts.push_back(std::move(host));
        }
    }
    if (locale < 0) { err = L("bi_div_no_locale"); return false; }
    if (divs.empty() || divs.size() > 255) { err = L("bi_div_count"); return false; }
    out.clear();
    out.push_back(static_cast<std::uint8_t>(locale));
    out.push_back(static_cast<std::uint8_t>(divs.size()));
    auto put = [&](const std::string& s) {
        const std::uint32_t n = static_cast<std::uint32_t>(s.size());
        out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&n), reinterpret_cast<const std::uint8_t*>(&n) + 4);
        out.insert(out.end(), s.begin(), s.end());
        out.push_back(0);
    };
    for (const Division& d : divs) {
        if (d.hosts.empty() || d.hosts.size() > 255) { err = Lf("bi_div_hosts", acp_to_wide(d.name)); return false; }
        put(d.name);
        out.push_back(static_cast<std::uint8_t>(d.hosts.size()));
        for (const std::string& h : d.hosts) put(h);
    }
    return true;
}

std::wstring gateport_text(const std::vector<std::uint8_t>& b) {
    std::size_t n = 0;
    while (n < b.size() && b[n] >= '0' && b[n] <= '9') ++n;
    if (n == 0 || n > 5 || !std::all_of(b.begin() + static_cast<std::ptrdiff_t>(n), b.end(), [](std::uint8_t c) { return c == 0 || c == '\r' || c == '\n'; }))
        return {};
    return std::wstring(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n));
}

bool gateport_bytes(const std::wstring& text, const std::vector<std::uint8_t>& orig, std::vector<std::uint8_t>& out,
                    std::wstring& err) {
    std::wstring t = text;
    while (!t.empty() && (t.back() == L'\r' || t.back() == L'\n' || t.back() == L' ')) t.pop_back();
    while (!t.empty() && t.front() == L' ') t.erase(0, 1);
    const bool digits = !t.empty() && t.size() <= 5 && std::all_of(t.begin(), t.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    if (!digits || std::stol(t) < 1 || std::stol(t) > 65535) { err = L("bi_port_bad"); return false; }
    out.clear();
    for (wchar_t ch : t) out.push_back(static_cast<std::uint8_t>(ch));   // ASCII digits (checked above)
    out.resize(std::max(orig.size(), out.size() + 1), 0);              // the client reads a NUL-padded field
    return true;
}

std::wstring describe_binary(std::wstring_view name, const std::vector<std::uint8_t>& b) {
    std::wstring out;
    const std::wstring base = base_lower(name);
    if (b.empty()) return L("bi_empty");

    // Signature
    std::wstring kind;
    for (const Magic& m : kMagics)
        if (b.size() >= m.len && std::memcmp(b.data(), m.sig, m.len) == 0) { kind = L(m.key); break; }
    std::wstring sig;
    if (b.size() >= 12 && std::memcmp(b.data(), "JMXV", 4) == 0) {
        sig.assign(b.begin(), b.begin() + 12);
        kind = L("bi_jmxv_unknown");
    } else {
        std::size_t k = 0;
        while (k < b.size() && k < 16 && ((b[k] >= 'A' && b[k] <= 'Z') || (b[k] >= 'a' && b[k] <= 'z') ||
                                          (b[k] >= '0' && b[k] <= '9') || b[k] == '_' || b[k] == ' ')) ++k;
        if (k >= 3) sig.assign(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(k));
    }
    if (kind.empty()) kind = L("bi_binary_data");
    out += kind + L"\n\n";
    out += label("bi_file", 11) + base + L"\n";
    out += label("bi_size", 11) + size_text(b.size()) + L"\n";
    if (!sig.empty()) out += label("bi_signature", 11) + sig + L"\n";
    if (b.size() >= 2 && b[0] == 'M' && b[1] == 'Z') out += describe_pe(b);

    // Content: entropy (bits per byte), zero / printable share.
    std::array<std::size_t, 256> hist{};
    const std::size_t n = std::min<std::size_t>(b.size(), 16u << 20);
    for (std::size_t i = 0; i < n; ++i) ++hist[b[i]];
    double h = 0;
    std::size_t printable = 0;
    for (std::size_t v = 0; v < 256; ++v) {
        if (hist[v]) { const double p = static_cast<double>(hist[v]) / static_cast<double>(n); h -= p * std::log2(p); }
        if (v >= 0x20 && v < 0x7F) printable += hist[v];
    }
    const double zero_pct = 100.0 * static_cast<double>(hist[0]) / static_cast<double>(n);
    const double print_pct = 100.0 * static_cast<double>(printable) / static_cast<double>(n);
    std::wstring verdict;
    if (std::count(hist.begin(), hist.end(), 0u) == 255)
        verdict = Lf("bi_filler", static_cast<unsigned>(b[0]));
    else if (h >= 7.5 && n >= 64)
        verdict = L("bi_random");
    else if (zero_pct >= 15)
        verdict = L("bi_structured");
    else
        verdict = L("bi_mixed");
    out += L"\n" + label("bi_content", 11) + verdict + L"\n";
    // Repeating aligned 16-byte blocks in near-random data: the layout of a block cipher in ECB mode
    // (equal plaintext blocks give equal ciphertext blocks).
    if (h >= 7.5 && b.size() >= 256 && b.size() % 16 == 0) {
        std::vector<std::array<std::uint8_t, 16>> blocks(b.size() / 16);
        for (std::size_t i = 0; i < blocks.size(); ++i) std::memcpy(blocks[i].data(), b.data() + i * 16, 16);
        std::sort(blocks.begin(), blocks.end());
        std::size_t repeated = 0;
        for (std::size_t i = 1; i < blocks.size(); ++i) repeated += blocks[i] == blocks[i - 1];
        if (repeated * 50 >= blocks.size()) {
            const std::wstring head = label("bi_layout", 11);
            out += head + Lf("bi_ecb_blocks", blocks.size(), repeated) + L"\n" + std::wstring(columns(head), L' ') +
                   L("bi_ecb_note") + L"\n";
        }
    }
    if (h >= 7.5 && n >= 64) {
        // Near-random bytes carry no readable fields or strings: listing chance matches would be noise.
        out += no_decoder();
        return out;
    }
    out += label("bi_entropy", 11) + Lf("bi_entropy_vals", h, zero_pct, print_pct) + L"\n";

    // Leading 32-bit fields (little-endian), past any ASCII signature.
    const std::size_t start = sig.empty() ? 0 : ((sig.size() + 3) & ~std::size_t{3});
    if (b.size() >= start + 8) {
        out += L"\n" + L("bi_leading") + L" ";
        for (std::size_t o = start, k = 0; o + 4 <= b.size() && k < 8; o += 4, ++k) out += std::to_wstring(rd32(b.data() + o)) + L"  ";
        out += L"\n";
    }

    // Embedded strings
    std::size_t total = 0;
    const auto strings = embedded_strings(b, 40, total);
    if (!strings.empty()) {
        out += L"\n" + Lf("bi_embedded", total) + L"\n";
        for (const FoundString& s : strings) out += L"  " + Lf("bi_at", s.off) + L" " + s.text + L"\n";
        if (total > strings.size()) out += L"  " + Lf("bi_more", total - strings.size()) + L"\n";
    }
    out += no_decoder();
    return out;
}

} // namespace pk2
