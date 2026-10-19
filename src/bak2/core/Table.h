// SkillEffect Studio - core/Table.h
// Any client text table other than skilleffect.txt (characterdata*, itemdata*, skilldata*ENC, textdata_*,
// effectsound, index files ...) as byte-exact physical lines + the client comment rules (R3/R4: '//' cuts,
// '/*' opens a block that only a '*/' closes). New rows are spliced in as bytes: every existing byte stays.
#pragma once

#include "Text.h"

#include <string>
#include <utility>
#include <vector>

namespace se {

struct TLine {
    size_t off = 0;   // byte offset of the content
    size_t len = 0;   // content bytes (no terminator)
    size_t term = 0;  // terminator bytes (CRLF = 4 in UTF-16, 2 in CP949; 0 on an unterminated last line)
};

class TableDoc {
public:
    bool Load(const Bytes& raw, std::wstring& err);
    size_t Count() const { return lines.size(); }
    bool IsData(size_t i) const { return state[i] == 0; }
    // raw TAB split of a data line's effective text (empty cells kept)
    std::vector<std::wstring> Cells(size_t i) const;
    // physical line before which new rows must go to be read: right after the last data row, never inside /* */
    int EndInsert() const;
    // byte-exact splice; each pair = (insert before physical line, rows without line ends); before == Count() = EOF
    // drop: optional per physical line flag (1 = line removed, "Var olanı değiştir")
    Bytes Splice(std::vector<std::pair<int, std::vector<std::wstring>>> ins, const std::vector<std::uint8_t>* drop = nullptr) const;
    Bytes EncodeRow(const std::wstring& row) const;  // row + CRLF in the file's encoding

    Bytes raw;
    bool utf16 = true, bom = false, finalTerm = true, lfOnly = false;
    std::vector<TLine> lines;
    std::vector<std::wstring> text;     // decoded content per physical line
    std::vector<std::wstring> eff;      // text the client reads (comments cut)
    std::vector<std::uint8_t> state;    // 0 data, 1 blank/comment, 2 inside /* */ (dropped)
    bool eofInBlock = false;
    int openBlockLine = -1;             // line that opened the block still open at EOF
    int dataRows = 0, hiddenRows = 0;   // data-looking rows inside /* */ blocks
    int dominantCells = 0;              // most common raw cell count of data rows
};

// raw TAB split keeping empty cells
std::vector<std::wstring> SplitTabs(std::wstring_view s);
std::wstring JoinTabs(const std::vector<std::wstring>& c);

}  // namespace se
