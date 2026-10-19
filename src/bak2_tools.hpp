#pragma once
// Bridge to the vendored "SkillEffect Studio" (BAK-2) tool modules (src/bak2/ddj,
// src/bak2/core, src/bak2/editor): DDJ / DDS writer and picture readers, skilldata
// ENC cipher, Myers line diff and the client text table editor's validator.
// std types only; no BAK-2 header leaks into app code. Nothing here touches a
// client: every function works on bytes the caller read.
#include <cstdint>
#include <string>
#include <vector>

namespace bak2 {

// ---------------------------------------------------------------- pictures / DDJ
// Any picture BAK-2 reads (PNG, BMP, TGA, DDS, DDJ, then WIC: JPG / GIF / TIFF ...) -> top-down
// B,G,R,A pixels (straight alpha).
bool decode_image_bgra(const std::vector<std::uint8_t>& bytes, int& w, int& h, std::vector<std::uint8_t>& bgra,
                       std::wstring* err = nullptr);

// Picture file -> .ddj ("JMXVDDJ 1000" + DDS). With oldDdj (the file being replaced) the new file keeps
// its layout: format, mip levels, JMXV wrapper and header field style. Without it: DXT1 (no alpha) /
// DXT5 (alpha), no mips, the shipped Joymax header. err (here and in decode_image_bgra): UI language.
bool encode_ddj(const std::vector<std::uint8_t>& imageFile, const std::vector<std::uint8_t>* oldDdj,
                std::vector<std::uint8_t>& outDdj, std::wstring& err);

// ---------------------------------------------------------------- skilldata_*ENC.txt
bool is_skill_enc(const std::vector<std::uint8_t>& bytes);                    // ends with the "//encrypt" marker
bool skill_decrypt(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out);   // false: no marker
std::vector<std::uint8_t> skill_encrypt(const std::vector<std::uint8_t>& plain);           // BOM + UTF-16LE rows

// ---------------------------------------------------------------- text diff
// Readable line diff of two texts (BAK-2 prova format): counts, then hunks with 3 context lines,
// "  " same / "- " removed / "+ " added. Words in the UI language (pk2::L).
std::wstring text_diff(const std::wstring& a, const std::wstring& b);

// ---------------------------------------------------------------- client text tables
struct TableCheck {
    int errors = 0, warnings = 0, infos = 0;
    int rows = 0;                 // data rows read
    bool roundtrip = false;       // the editor writes the bytes back unchanged
    std::wstring report;          // header line + one line per finding
};
// innerPath names the table ("server_dep/silkroad/textdata/skilldata_5000enc.txt" or just the file
// name): the schema is chosen by it. false (out.report = why) when the bytes cannot be read as a table.
// The report is in the UI language (findings: BAK-2's Turkish for Turkish, else its English).
bool table_validate(const std::wstring& innerPath, const std::vector<std::uint8_t>& bytes, TableCheck& out);
// EdDoc load + save is byte-identical (ENC files decrypted and encrypted again).
bool table_roundtrip_ok(const std::vector<std::uint8_t>& bytes, const std::wstring& innerPath = {});

} // namespace bak2
