// SkillEffect Studio - core/SeModel.h
// skilleffect.txt as physical lines (byte-exact) + an exact model of how sro_client.exe
// (vSRO 1.188) reads it (SKILLEFFECT_LOGIC.md R1..R9).
#pragma once

#include "Text.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace se {

enum class Sec : int { None = 0, CI = 1, Light = 2, SA = 3, SE = 4, Env = 6 };
constexpr int kSecCount = 7;

const wchar_t* SecName(Sec s);         // characterInfo / skillaniset2 / skilleffectset ...
Sec SecFromName(std::wstring_view n);  // client mapping (R8); None when unknown
int ExpectedCells(Sec s);              // 13 / 27 / 28
int KeyColumn(Sec s);                  // 0 / 2 / 1

struct PLine {
    size_t off = 0;           // offset in wchar units into SeDoc::text
    size_t len = 0;           // content length (no terminator)
    std::uint8_t term = 0;    // 0 none, 1 CRLF, 2 LF, 3 CR, 4 NUL
};

// v5 M5: section header traps (diagnostics only, the client model is unchanged)
enum class RecDiag : std::uint8_t {
    None = 0,
    BadName = 1,    // S1: '#section <unknown / wrong-case / missing name>': the client keeps the previous section
    BadCase = 2,    // S1: first token '#Section' / '#SECTION' ...: not a header for the client, read as a data row
    AfterBad = 3,   // a data row after an S1 header: read by the PREVIOUS section's handler (not where it was meant)
};

// what the client does with each physical line (the tables grid strip): LineKindName gives the TR / EN label
enum class LineKind : std::uint8_t {
    Blank = 0,         // empty / whitespace only
    Active = 1,        // read as a data row of its section
    Comment = 2,       // '//' comment, inside /* */, or only a comment opener / closer
    Header = 3,        // valid '#section'
    BadHeader = 4,     // S1
    OutOfSection = 5,  // S2: data before the first valid '#section' (ignored, R9)
    Misrouted = 6,     // data after an S1 header: read into the previous section
};
const wchar_t* LineKindName(LineKind k, bool english);
bool LineKindRead(LineKind k);  // true only for Active (and Header)

struct Rec {
    int line = 0;  // physical line index (0-based) where the client line starts
    Sec sec = Sec::None;
    bool header = false;
    RecDiag diag = RecDiag::None;  // v5 M5
    std::vector<std::wstring> cells;
    const std::wstring& Key() const;
};

struct SimResult {
    std::vector<Rec> recs;               // headers + data records in client order
    std::vector<int> recOfLine;          // physical line -> data record index or -1
    std::vector<std::uint8_t> blockAfter;  // physical line -> inside /* */ after this line
    std::vector<std::uint8_t> dropped;     // physical line -> dropped by the comment reader
    bool eofInBlock = false;
    std::array<int, kSecCount> counts{};   // data records per section
    // v5 M5 diagnostics
    std::vector<std::uint8_t> lineKind;    // physical line -> LineKind
    std::vector<int> nulLines;             // physical lines holding a NUL (the client ends a line there, S3)
    int firstHeaderLine = -1;              // first VALID '#section' (physical, 0-based)
    int outOfSection = 0;                  // S2: data rows before it
};

SimResult Simulate(std::wstring_view text, const std::vector<PLine>& lines);
std::vector<PLine> SplitPhysical(std::wstring_view text);

class SeDoc {
public:
    bool Load(const Bytes& raw, std::wstring& err);
    std::wstring_view Line(size_t i) const { return std::wstring_view(text).substr(lines[i].off, lines[i].len); }
    const wchar_t* TermStr(std::uint8_t t) const;
    Bytes Encode(const std::wstring& body) const;  // BOM (if the original had one) + UTF-16LE

    bool hasBom = false;
    std::wstring text;
    std::vector<PLine> lines;
    SimResult sim;
    // style of the original, reused for new lines
    std::uint8_t mainTerm = 1;
    bool finalTerm = true;
    std::array<int, kSecCount> padTabs{};  // dominant TAB count per line in each section (0 = none)
    int countCrlf = 0, countLf = 0, countCr = 0;
};

}  // namespace se
