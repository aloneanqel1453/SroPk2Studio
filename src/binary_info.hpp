#pragma once
// Readable views for client files no format decoder (ours, BAK, BAK-2) knows: small client
// records BAK has no parser for (plugin.dat, JMXVCAMR camera, JMXVIMG1 font glyphs, 256-colour
// palettes, SourceSafe leftovers) and, for anything else, a structural analysis (signature,
// content kind, entropy, leading fields, embedded strings). Never a byte dump: bytes are edited
// in the hex editor.
#include "dds.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pk2 {

// Description of a known small client record, or empty when `b` is none of them. All texts of this
// header are in the current UI language (i18n.hpp).
[[nodiscard]] std::wstring describe_client_record(std::wstring_view name, const std::vector<std::uint8_t>& b);

// The picture of a client record that has one: a font glyph (JMXVIMG1, Media\fonts\*.dat) or a
// 768-byte RGB palette (silk.dat) as a 16x16 grid of colour swatches. False otherwise.
[[nodiscard]] bool record_image(std::wstring_view name, const std::vector<std::uint8_t>& b, Image& out);

// DIVISIONINFO.TXT (binary despite the name): [u8 locale][u8 divisions], per division
// [u32 len][name][NUL][u8 gateways], per gateway [u32 len][host][NUL]. As editable text:
//   locale = 65
//   [DIV1]
//   192.168.1.101
// division_text is empty when `b` is not one; division_bytes rebuilds the file (false + err). Only
// the leading # comment line is translated, so the text of any UI language parses back.
[[nodiscard]] std::wstring division_text(const std::vector<std::uint8_t>& b);
[[nodiscard]] bool division_bytes(const std::wstring& text, std::vector<std::uint8_t>& out, std::wstring& err);

// GATEPORT.TXT: the gateway port as ASCII digits, NUL padded. gateport_bytes keeps the original
// length when the new number fits.
[[nodiscard]] std::wstring gateport_text(const std::vector<std::uint8_t>& b);
[[nodiscard]] bool gateport_bytes(const std::wstring& text, const std::vector<std::uint8_t>& orig,
                                  std::vector<std::uint8_t>& out, std::wstring& err);

// Structural analysis of an unknown binary: kind / signature, size, entropy verdict (compressed /
// encrypted vs structured records), leading 32-bit fields and the embedded ASCII / UTF-16 strings.
[[nodiscard]] std::wstring describe_binary(std::wstring_view name, const std::vector<std::uint8_t>& b);

} // namespace pk2
