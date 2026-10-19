#pragma once
// String conversions between UTF-8 / UTF-16 (wide) / system ANSI (CP_ACP),
// plus pk2 name <-> display helpers. Windows-only.
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pk2 {

constexpr std::size_t kNameBytes = 89; // PackFileEntry name field size

[[nodiscard]] std::wstring utf8_to_wide(std::string_view s);
[[nodiscard]] std::string  wide_to_utf8(std::wstring_view s);
[[nodiscard]] std::wstring acp_to_wide(std::string_view s);
[[nodiscard]] std::string  wide_to_acp(std::wstring_view s);

// Decode a raw 89-byte pk2 name field to a display string.
// Tries UTF-8 first (lossless for ASCII and modern names); falls back to CP_ACP.
[[nodiscard]] std::wstring decode_pk2_name(const std::array<std::uint8_t, kNameBytes>& raw);

// Encode a display name into a raw 89-byte field (CP_ACP, matching the
// reference Folder2Pk2 Ansi marshalling), NUL-terminated and zero-padded.
[[nodiscard]] std::array<std::uint8_t, kNameBytes> encode_pk2_name(std::wstring_view name);

[[nodiscard]] std::wstring to_lower(std::wstring_view s);

// Text encodings reported by decode_text (and expected back by the app's encode_text).
enum TextEncoding : int { kEncAnsi = 0, kEncUtf8 = 1, kEncUtf16 = 2, kEncUtf8Bom = 3, kEncCp949 = 4 };

// Decode a text buffer honoring a BOM (UTF-16LE / UTF-8), then guessing: BOM-less UTF-16LE,
// strict UTF-8, strict CP949 (Korean data tables), ANSI. `enc` receives the TextEncoding used.
[[nodiscard]] std::wstring decode_text(const std::vector<std::uint8_t>& b, int& enc);

// Whether a buffer is human-readable text (BOM, ASCII-ish UTF-16LE, strict UTF-8 or strict CP949
// with almost no control bytes and no NUL).
[[nodiscard]] bool is_text_bytes(const std::vector<std::uint8_t>& b);

// Whether decoded text is something a person can read: letters / digits / punctuation of real
// scripts, almost no control, private-use, replacement or lone-surrogate code units. A buffer
// decoded with the wrong encoding (binary as UTF-16, ...) scatters over random blocks and fails.
[[nodiscard]] bool readable_text(const std::wstring& s);

} // namespace pk2
