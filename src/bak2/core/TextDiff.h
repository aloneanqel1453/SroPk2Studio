// SkillEffect Studio - core/TextDiff.h (v6 P3)
// Line diff for the prova ("dry run") window and its tests:
//  * lines are split at '\n' only; the terminator ("\r\n" / "\n" / none at the end) is not part of a line, so a file
//    that only gains a line ending at its end does not count a changed line,
//  * Myers O(ND) diff (linear space, middle snake) over hashed lines, after the common prefix / suffix and the lines
//    that occur on one side only are taken off (both keep the result minimal); a cost cap keeps pathological inputs
//    fast (the script stays valid, `exact` turns false),
//  * DiffCounts: added / changed / removed lines - "changed" = a removed line paired with an added line of the same
//    hunk (min(removed, added) per hunk), the rest of the hunk is added or removed,
//  * client bytes are decoded with DecodeText (UTF-16LE with / without BOM, UTF-8, CP949); a binary file counts as
//    1 changed line when it differs,
//  * StudioWriteFiles: the game files Studio::Write would write for a result (textdata + package resources), each with
//    the client's current bytes - the prova window and its count test use the same list (nothing is written).
#pragma once

#include "Text.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace se {

class Studio;
struct StudioResult;

struct DiffCounts {
    int added = 0, changed = 0, removed = 0;
    bool Any() const { return added || changed || removed; }
    DiffCounts& operator+=(const DiffCounts& o) {
        added += o.added;
        changed += o.changed;
        removed += o.removed;
        return *this;
    }
    bool operator==(const DiffCounts& o) const { return added == o.added && changed == o.changed && removed == o.removed; }
};

enum class DiffKind : std::uint8_t { Same, Del, Add };
struct DiffOp {
    DiffKind kind = DiffKind::Same;
    int a = -1;  // line of "before" (Same / Del)
    int b = -1;  // line of "after" (Same / Add)
};

// lines without their terminator; "" -> no line, "x" / "x\n" / "x\r\n" -> one line "x"
std::vector<std::wstring> SplitLines(std::wstring_view s);
// edit script in order (Same / Del / Add; inside a hunk the removed lines come first). exact = minimal script.
std::vector<DiffOp> DiffLines(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b, bool* exact = nullptr);
DiffCounts CountOps(const std::vector<DiffOp>& ops);

struct LineDiff {
    std::vector<std::wstring> a, b;  // lines of before / after (empty for a binary file)
    std::vector<DiffOp> ops;
    DiffCounts counts;
    bool exact = true;
    bool binary = false;
    bool identical = false;          // the bytes are the same
    TextEnc encA = TextEnc::Utf8, encB = TextEnc::Utf8;
};
LineDiff DiffText(std::wstring_view before, std::wstring_view after);
// isText = false (or bytes that do not look like text): 1 changed when the bytes differ
LineDiff DiffBytes(const Bytes& before, const Bytes& after, bool isText);
DiffCounts DiffBytesCounts(const Bytes& before, const Bytes& after, bool isText);

// text file by its extension (.txt .sql .ini .csv .tsv .xml .htm .html .json .md .log .cfg .bat .ps1)
bool IsTextPath(std::wstring_view path);
// bytes a text decoder can show: a BOM, UTF-16 by DecodeText's rule, or no NUL / few control bytes
bool LooksText(const Bytes& b);

// display: ranges [op0, op1) of ops holding the changes with `context` unchanged lines around them
struct DiffHunk {
    size_t op0 = 0, op1 = 0;
};
std::vector<DiffHunk> DiffHunks(const std::vector<DiffOp>& ops, int context = 3);

// ---- prova of Studio::Write
struct WriteDiffFile {
    std::wstring path;      // relative to the output folder (Media\server_dep\silkroad\textdata\x.txt, Data\res\...)
    Bytes before, after;    // before = the client's current file (empty: the client has none)
    bool isNew = false;     // the client has no such file
    bool isText = true;
    bool clientOwn = false; // the client's own lines change / go (#replace / #remove), or a client resource is replaced
    DiffCounts counts;
};
// the game files Studio::Write(r, ...) writes (r.outputs + r.installs), with the client's current bytes; false + err
// when a package resource cannot be read (Write would fail the same way)
bool StudioWriteFiles(Studio& st, const StudioResult& r, std::vector<WriteDiffFile>& out, std::wstring* err = nullptr);

}  // namespace se
