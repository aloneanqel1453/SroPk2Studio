// SkillEffect Studio - core/Text.h
// Small text/file helpers shared by the GUI and the CLI. UTF-16 code units are copied
// verbatim (no conversion) so an unchanged line is always byte-identical on output.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace se {

using Bytes = std::vector<std::uint8_t>;

enum class TextEnc { Utf16LE, Utf16BE, Utf8, Cp949 };

bool ReadFileBytes(const std::wstring& path, Bytes& out, std::wstring& err);
// temp file in the same folder + MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH)
bool WriteFileAtomic(const std::wstring& path, const Bytes& data, std::wstring& err);
bool FileExists(const std::wstring& path);
bool DirExists(const std::wstring& path);

// Auto-detects BOM / UTF-16 without BOM / UTF-8 / CP949 (lists, fragments, clipboard files).
std::wstring DecodeText(const Bytes& b, TextEnc* encOut = nullptr, bool* hadBom = nullptr);
const wchar_t* EncName(TextEnc e);

std::string ToUtf8(std::wstring_view w);
std::wstring FromUtf8(std::string_view s);
std::wstring FromAnsi(std::string_view s, unsigned codePage);

// ---- v5 M3: lossless narrow conversion ("ENC2")
// Converts text to code page cp (949 = the client's pk2 names and CP949 tables) only when nothing is lost:
// WC_NO_BEST_FIT_CHARS + lpUsedDefaultChar + a round trip back to UTF-16 that must be identical (CP_UTF8: invalid
// surrogates fail). Pure ASCII is copied byte for byte. On loss: returns false, out is empty and *badPos = UTF-16 index
// of the first character that cannot be written (Turkish ş/ğ/ç/İ in CP949, ...).
bool NarrowExact(unsigned cp, std::wstring_view text, std::string& out, size_t* badPos = nullptr);
bool IsExactIn(unsigned cp, std::wstring_view text, size_t* badPos = nullptr);
// "'ş' (U+015F, karakter 14) CP949'da yok: res\mob\test_ş.bsr" / "... is not in CP949: ..."
std::wstring EncLossText(std::wstring_view text, size_t pos, bool english, unsigned cp = 949);

// Losses noticed where no error can be returned (TableDoc::EncodeRow). Process-wide list (thread-safe, at most 256).
// A loss also guards the writers: WriteFileAtomic and Pk2WriteCopy refuse (ENC2) data that holds the lossy bytes of a
// noted row, so a lossy row is never written even when the caller does not check. TakeEncLosses() returns + clears.
struct EncLoss {
    std::wstring where;  // "table row", pk2 path ...
    std::wstring text;   // the whole row / path (UTF-16)
    size_t pos = 0;      // first character that cannot be written
    int cell = -1;       // TAB cell index of pos (rows), -1 = not a row
    unsigned cp = 949;
    Bytes lossy;         // what the old lossy conversion would have written (the write guard looks for it)
};
void NoteEncLoss(EncLoss e);
std::vector<EncLoss> TakeEncLosses();
size_t PendingEncLosses();
// index of a pending loss whose lossy bytes (>= 8 bytes) occur in data, -1 = none; *msg = ENC2 text (TR / EN)
int FindEncLossIn(const Bytes& data, std::wstring* msgTr = nullptr, std::wstring* msgEn = nullptr);

// client whitespace (SKILLEFFECT_LOGIC R6): U+0001..U+0010 and space
inline bool IsClientWs(wchar_t c) { return (c >= 1 && c <= 0x10) || c == L' '; }
bool IsBlankClient(std::wstring_view s);
// client tokenizer (R7): leading whitespace skipped, cell ends at TAB -> empty cells vanish
std::vector<std::wstring> ClientTokens(std::wstring_view line);
// raw split on TAB (keeps empty cells)
std::vector<std::wstring_view> SplitView(std::wstring_view s, wchar_t sep);

bool ParseInt(std::wstring_view s, long long& v);
bool ParseFloat(std::wstring_view s, double& v);
std::wstring Trim(std::wstring_view s);
bool StartsWith(std::wstring_view s, std::wstring_view p);
bool EndsWithI(std::wstring_view s, std::wstring_view p);
std::wstring ToUpper(std::wstring_view s);
std::wstring Fmt(const wchar_t* fmt, ...);
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
std::wstring DirOf(const std::wstring& p);
std::wstring FileNameOf(const std::wstring& p);

}  // namespace se
