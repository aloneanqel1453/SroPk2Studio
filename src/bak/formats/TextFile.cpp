// ============================================================================
//  SRO Asset Studio - formats/TextFile.cpp
//
//  Text loading with encoding detection + tab/semicolon separated table
//  splitting.
//
//  Why the detection order matters for Silkroad specifically:
//    * textdata_*.txt, skilldata_*.txt, itemdata_*.txt and most of the client
//      data tables are UTF-16LE WITH a BOM. Reading them as ANSI produces a
//      string full of NULs and a totally wrong column count.
//    * Some private servers re-export those tables WITHOUT the BOM. The only
//      reliable tell is then the density of NUL bytes at odd offsets, which is
//      what SniffUtf16 measures.
//    * Loose ANSI files are CP1254 on Turkish shards and CP949 on the Korean
//      originals; CP949 is only chosen when the byte pattern actually looks
//      like double-byte Hangul, otherwise a Turkish 'ð' would be swallowed.
// ============================================================================
#include "TextFile.h"
#include "../core/I18n.h"   // UI strings follow the chosen language

namespace sro {

namespace {

/// How many bytes the statistical sniffs look at.
constexpr size_t kSniffBytes = 4096;

// ---------------------------------------------------------------------------
// BOM detection
// ---------------------------------------------------------------------------
bool HasUtf8Bom(const u8* d, size_t n)    { return n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF; }
bool HasUtf16LeBom(const u8* d, size_t n) { return n >= 2 && d[0] == 0xFF && d[1] == 0xFE; }
bool HasUtf16BeBom(const u8* d, size_t n) { return n >= 2 && d[0] == 0xFE && d[1] == 0xFF; }

// ---------------------------------------------------------------------------
// UTF-16 sniff without a BOM.
//
// Latin/Turkish/Korean text encoded as UTF-16LE puts a 0x00 in every SECOND
// byte (the high byte of a BMP code point below 0x100). So:
//   * NULs at odd offsets  -> UTF-16LE
//   * NULs at even offsets -> UTF-16BE
// A real 8-bit text file has no NULs at all, and a binary file scatters them.
// ---------------------------------------------------------------------------
bool SniffUtf16(const u8* d, size_t n, bool& bigEndian) {
    bigEndian = false;
    if (n < 16) return false;
    const size_t look = (n < kSniffBytes) ? (n & ~size_t(1)) : kSniffBytes;
    if (look < 16) return false;

    size_t oddNul = 0, evenNul = 0;
    for (size_t i = 0; i + 1 < look; i += 2) {
        if (d[i] == 0)     ++evenNul;
        if (d[i + 1] == 0) ++oddNul;
    }
    const size_t pairs = look / 2;
    // Require a strong majority so a binary blob with a run of zeros does not
    // get mistaken for text.
    if (oddNul * 10 >= pairs * 8 && evenNul * 4 < pairs) { bigEndian = false; return true; }
    if (evenNul * 10 >= pairs * 8 && oddNul * 4 < pairs) { bigEndian = true;  return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Strict UTF-8 validation over the sniff window. Returns false on any invalid
// sequence; `sawMultiByte` tells the caller whether it saw anything above
// ASCII (pure ASCII is valid UTF-8 but also valid in every code page, so it
// must not by itself decide the encoding).
// ---------------------------------------------------------------------------
bool LooksLikeUtf8(const u8* d, size_t n, bool& sawMultiByte) {
    sawMultiByte = false;
    const size_t look = (n < kSniffBytes) ? n : kSniffBytes;
    size_t i = 0;
    while (i < look) {
        const u8 c = d[i];
        if (c < 0x80) { ++i; continue; }

        int extra;
        if      ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return false;                       // 0x80..0xBF lead or 0xF8+

        // A truncated sequence at the very end of the window is not an error.
        if (i + static_cast<size_t>(extra) >= look) {
            if (look < n) return true;           // window edge, not file edge
            return false;
        }
        for (int k = 1; k <= extra; ++k)
            if ((d[i + static_cast<size_t>(k)] & 0xC0) != 0x80) return false;

        sawMultiByte = true;
        i += static_cast<size_t>(extra) + 1;
    }
    return true;
}

// ---------------------------------------------------------------------------
// CP949 (Korean) sniff: EUC-KR lead bytes are 0x81..0xFE and the trail byte is
// 0x41..0xFE. Turkish CP1254 text mixes high bytes with ASCII freely, so a
// high proportion of well formed double-byte PAIRS is the discriminator.
// ---------------------------------------------------------------------------
bool LooksLikeCp949(const u8* d, size_t n) {
    const size_t look = (n < kSniffBytes) ? n : kSniffBytes;
    size_t pairs = 0, loneHigh = 0;
    for (size_t i = 0; i < look; ) {
        const u8 c = d[i];
        if (c < 0x80) { ++i; continue; }
        if (c >= 0x81 && c <= 0xFE && i + 1 < look) {
            const u8 t = d[i + 1];
            if (t >= 0x41 && t <= 0xFE) { ++pairs; i += 2; continue; }
        }
        ++loneHigh;
        ++i;
    }
    return pairs >= 4 && pairs > loneHigh * 3;
}

/// Windows code page number for a TextEncoding.
UINT CodePageFor(TextEncoding e) {
    switch (e) {
        case TextEncoding::Utf8:
        case TextEncoding::Utf8Bom: return CP_UTF8;
        case TextEncoding::Cp1252:  return 1252;
        case TextEncoding::Cp1254:  return 1254;
        case TextEncoding::Cp949:   return 949;
        case TextEncoding::Ansi:    return CP_ACP;
        default:                    return 1252;
    }
}

/// Byte-swap a UTF-16BE buffer in place into host order.
void SwapUtf16(std::wstring& s) {
    for (wchar_t& c : s) {
        const u16 v = static_cast<u16>(c);
        c = static_cast<wchar_t>(static_cast<u16>((v >> 8) | (v << 8)));
    }
}

/// Normalise CRLF/CR to LF, report whether the source used CRLF, and drop a
/// leading U+FEFF that survived a code page conversion.
void NormaliseNewlines(std::wstring& s, bool& crlf) {
    crlf = false;
    if (!s.empty() && s[0] == 0xFEFF) s.erase(s.begin());

    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c == L'\r') {
            crlf = true;
            if (i + 1 < s.size() && s[i + 1] == L'\n') ++i;
            out.push_back(L'\n');
        } else {
            out.push_back(c);
        }
    }
    s.swap(out);
}

/// Field counts per line for one candidate delimiter, used to score it.
void ScoreDelimiter(const std::vector<std::wstring>& lines, wchar_t sep,
                    size_t& outMedianFields, size_t& outConsistentLines) {
    size_t counts[9] = {};   // histogram of "field count" clamped to 8+
    outMedianFields = 0;
    outConsistentLines = 0;

    std::vector<size_t> perLine;
    perLine.reserve(lines.size());
    for (const std::wstring& l : lines) {
        size_t fields = 1;
        for (wchar_t c : l) if (c == sep) ++fields;
        perLine.push_back(fields);
        counts[(fields > 8) ? 8 : fields] += 1;
    }
    if (perLine.empty()) return;

    // The winning field count is the most common one above 1.
    size_t best = 0, bestCount = 0;
    for (size_t f = 2; f < 9; ++f)
        if (counts[f] > bestCount) { bestCount = counts[f]; best = f; }
    if (best == 0) return;

    // For the "8+" bucket, find the true modal value.
    if (best == 8) {
        std::vector<size_t> big;
        for (size_t v : perLine) if (v >= 8) big.push_back(v);
        std::sort(big.begin(), big.end());
        size_t run = 0, bestRun = 0, bestVal = 8;
        for (size_t i = 0; i < big.size(); ++i) {
            run = (i && big[i] == big[i - 1]) ? run + 1 : 1;
            if (run > bestRun) { bestRun = run; bestVal = big[i]; }
        }
        best = bestVal;
    }

    size_t consistent = 0;
    for (size_t v : perLine) if (v == best) ++consistent;

    outMedianFields = best;
    outConsistentLines = consistent;
}

} // namespace

// ---------------------------------------------------------------------------
// TextEncodingName
// ---------------------------------------------------------------------------
const wchar_t* TextEncodingName(TextEncoding e) {
    switch (e) {
        case TextEncoding::Utf8:       return L"UTF-8";
        case TextEncoding::Utf8Bom:    return L"UTF-8 (BOM)";
        case TextEncoding::Utf16Le:    return L"UTF-16LE";
        case TextEncoding::Utf16LeBom: return L"UTF-16LE (BOM)";
        case TextEncoding::Utf16Be:    return L"UTF-16BE";
        case TextEncoding::Utf16BeBom: return L"UTF-16BE (BOM)";
        case TextEncoding::Cp1252:     return L"CP1252";
        case TextEncoding::Cp1254:     return T(L"CP1254 (Türkçe)");
        case TextEncoding::Cp949:      return T(L"CP949 (Korece)");
        case TextEncoding::Ansi:       return L"ANSI";
        default:                       return T(L"Bilinmeyen");
    }
}

// ---------------------------------------------------------------------------
// DetectEncoding
// ---------------------------------------------------------------------------
TextEncoding DetectEncoding(const u8* data, size_t size) {
    if (!data || size == 0) return TextEncoding::Unknown;

    // 1. BOM - conclusive.
    if (HasUtf8Bom(data, size))    return TextEncoding::Utf8Bom;
    if (HasUtf16LeBom(data, size)) return TextEncoding::Utf16LeBom;
    if (HasUtf16BeBom(data, size)) return TextEncoding::Utf16BeBom;

    // 2. BOM-less UTF-16 (very common in re-exported Silkroad tables).
    bool be = false;
    if (SniffUtf16(data, size, be))
        return be ? TextEncoding::Utf16Be : TextEncoding::Utf16Le;

    // 3. Valid multi-byte UTF-8.
    bool sawMultiByte = false;
    if (LooksLikeUtf8(data, size, sawMultiByte) && sawMultiByte)
        return TextEncoding::Utf8;

    // 4. Code page fallback.
    if (LooksLikeCp949(data, size)) return TextEncoding::Cp949;

    // Pure ASCII decodes identically under every candidate; CP1254 is the
    // project default and a superset of ASCII, so it is the safe answer.
    return TextEncoding::Cp1254;
}

// ---------------------------------------------------------------------------
// LoadTextDocument
// ---------------------------------------------------------------------------
Result LoadTextDocument(const u8* data, size_t size, TextDocument& out,
                        size_t maxBytes, TextEncoding forceEncoding) {
    out = TextDocument();
    if (!data || size == 0) {
        out.encoding = TextEncoding::Unknown;
        return Result::Ok();          // an empty file is not an error
    }

    out.byteSize = size;

    TextEncoding enc = (forceEncoding != TextEncoding::Unknown)
                     ? forceEncoding
                     : DetectEncoding(data, size);
    out.encoding = enc;

    // Skip the BOM before decoding.
    size_t offset = 0;
    switch (enc) {
        case TextEncoding::Utf8Bom:    offset = 3; out.hadBom = true; break;
        case TextEncoding::Utf16LeBom: offset = 2; out.hadBom = true; break;
        case TextEncoding::Utf16BeBom: offset = 2; out.hadBom = true; break;
        default: break;
    }
    if (offset > size) offset = size;

    size_t avail = size - offset;
    if (maxBytes && avail > maxBytes) { avail = maxBytes; out.truncated = true; }

    const u8* p = data + offset;

    const bool utf16 = (enc == TextEncoding::Utf16Le || enc == TextEncoding::Utf16LeBom ||
                        enc == TextEncoding::Utf16Be || enc == TextEncoding::Utf16BeBom);

    if (utf16) {
        const size_t units = avail / 2;   // an odd trailing byte is dropped
        try { out.text.assign(reinterpret_cast<const wchar_t*>(p), units); }
        catch (...) { return Result::Fail(Status::OutOfMemory, T(L"Metin belleğe sığmadı.")); }
        if (enc == TextEncoding::Utf16Be || enc == TextEncoding::Utf16BeBom)
            SwapUtf16(out.text);
    } else {
        const UINT cp = CodePageFor(enc);
        try { out.text = MultiByteToWide(reinterpret_cast<const char*>(p), avail, cp); }
        catch (...) { return Result::Fail(Status::OutOfMemory, T(L"Metin belleğe sığmadı.")); }

        // MultiByteToWideChar returns nothing when the buffer is not valid in
        // the chosen code page; fall back to CP1252, which maps every byte.
        if (out.text.empty() && avail > 0) {
            out.text = MultiByteToWide(reinterpret_cast<const char*>(p), avail, 1252);
            if (!out.text.empty()) out.encoding = TextEncoding::Cp1252;
        }
    }

    // Drop an embedded NUL tail (some exporters pad the file).
    const size_t nul = out.text.find(L'\0');
    if (nul != std::wstring::npos) out.text.resize(nul);

    NormaliseNewlines(out.text, out.crlf);

    out.lineCount = out.text.empty() ? 0 : 1;
    for (wchar_t c : out.text) if (c == L'\n') ++out.lineCount;
    // A trailing newline does not start a new line.
    if (!out.text.empty() && out.text.back() == L'\n' && out.lineCount > 1) --out.lineCount;

    return Result::Ok();
}

// ---------------------------------------------------------------------------
// TextDocument::summaryTr
// ---------------------------------------------------------------------------
std::wstring TextDocument::summaryTr() const {
    std::wstring s;
    s += T(L"Kodlama       : ") + std::wstring(TextEncodingName(encoding));
    if (hadBom) s += T(L"  [BOM var]");
    s += L"\n";
    s += T(L"Satır sonu    : ") + std::wstring(crlf ? T(L"CRLF (Windows)") : L"LF") + L"\n";
    s += T(L"Satır sayısı  : ") + FormatCountTr(lineCount) + L"\n";
    s += T(L"Karakter      : ") + FormatCountTr(text.size()) + L"\n";
    s += T(L"Dosya boyutu  : ") + FormatBytesTr(byteSize) + L"\n";
    if (truncated)
        s += T(L"UYARI         : Dosya çok büyük, yalnızca başı gösteriliyor.\n");
    return s;
}

// ---------------------------------------------------------------------------
// DetectDelimiter
// ---------------------------------------------------------------------------
TableDelimiter DetectDelimiter(const std::wstring& text) {
    if (text.empty()) return TableDelimiter::None;

    // Sample the first non-empty lines (skip comment lines starting with //).
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start < text.size() && lines.size() < 64) {
        size_t nl = text.find(L'\n', start);
        if (nl == std::wstring::npos) nl = text.size();
        std::wstring line = text.substr(start, nl - start);
        start = nl + 1;
        const std::wstring trimmed = TrimW(line);
        if (trimmed.empty()) continue;
        if (trimmed.size() >= 2 && trimmed[0] == L'/' && trimmed[1] == L'/') continue;
        lines.push_back(std::move(line));
    }
    if (lines.size() < 2) return TableDelimiter::None;

    struct Candidate { wchar_t ch; TableDelimiter id; };
    static const Candidate kCandidates[] = {
        { L'\t', TableDelimiter::Tab },
        { L';',  TableDelimiter::Semicolon },
        { L',',  TableDelimiter::Comma },
        { L'|',  TableDelimiter::Pipe },
    };

    TableDelimiter best = TableDelimiter::None;
    double bestScore = 0.0;

    for (const Candidate& c : kCandidates) {
        size_t fields = 0, consistent = 0;
        ScoreDelimiter(lines, c.ch, fields, consistent);
        if (fields < 2) continue;
        // Reward both consistency and column count; a 40-column tab file must
        // beat a 2-column accidental comma split.
        const double ratio = static_cast<double>(consistent) / static_cast<double>(lines.size());
        if (ratio < 0.6) continue;
        const double score = ratio * (1.0 + static_cast<double>(fields));
        if (score > bestScore) { bestScore = score; best = c.id; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// ParseTable
// ---------------------------------------------------------------------------
Result ParseTable(const std::wstring& text, TableDelimiter delimiter,
                  bool assumeHeaderRow, size_t maxRows, TableData& out) {
    out = TableData();

    if (delimiter == TableDelimiter::None)
        delimiter = DetectDelimiter(text);
    out.delimiter = delimiter;

    if (delimiter == TableDelimiter::None)
        return Result::Fail(Status::Unsupported, T(L"Tablo: ayırıcı bulunamadı."));

    wchar_t sep = L'\t';
    switch (delimiter) {
        case TableDelimiter::Tab:       sep = L'\t'; break;
        case TableDelimiter::Comma:     sep = L',';  break;
        case TableDelimiter::Semicolon: sep = L';';  break;
        case TableDelimiter::Pipe:      sep = L'|';  break;
        default: break;
    }

    size_t start = 0;
    bool   first = true;
    while (start <= text.size()) {
        if (maxRows && out.rows.size() >= maxRows) break;

        size_t nl = text.find(L'\n', start);
        const bool last = (nl == std::wstring::npos);
        if (last) nl = text.size();
        if (start >= text.size() && !first) break;

        std::wstring line = text.substr(start, nl - start);
        start = nl + 1;

        // Keep blank lines out of the grid; Silkroad tables end with several.
        if (TrimW(line).empty()) {
            if (last) break;
            continue;
        }

        std::vector<std::wstring> fields = SplitW(line, sep);
        for (std::wstring& f : fields) {
            f = TrimW(f);
            // Strip the quotes Silkroad wraps its string columns in.
            if (f.size() >= 2 && f.front() == L'"' && f.back() == L'"')
                f = f.substr(1, f.size() - 2);
        }

        if (fields.size() > out.columnCount) out.columnCount = fields.size();

        if (first && assumeHeaderRow) {
            out.headers = std::move(fields);
            out.hasHeaderRow = true;
        } else {
            out.rows.push_back(std::move(fields));
        }
        first = false;

        if (last) break;
    }

    if (!out.hasHeaderRow) {
        // Silkroad data files carry no header row; number the columns so the
        // grid still has something to label with.
        out.headers.reserve(out.columnCount);
        for (size_t i = 0; i < out.columnCount; ++i)
            out.headers.push_back(FormatW(T(L"Sütun %zu"), i + 1));
    }

    // Pad short rows so the grid never indexes past a row's end.
    for (std::vector<std::wstring>& row : out.rows)
        if (row.size() < out.columnCount) row.resize(out.columnCount);

    if (out.rows.empty())
        return Result::Fail(Status::BadFormat, T(L"Tablo: veri satırı yok."));
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// TableData::summaryTr
// ---------------------------------------------------------------------------
std::wstring TableData::summaryTr() const {
    const wchar_t* sepName = T(L"yok");
    switch (delimiter) {
        case TableDelimiter::Tab:       sepName = T(L"sekme (TAB)"); break;
        case TableDelimiter::Comma:     sepName = T(L"virgül");      break;
        case TableDelimiter::Semicolon: sepName = T(L"noktalı virgül"); break;
        case TableDelimiter::Pipe:      sepName = T(L"dikey çizgi"); break;
        default: break;
    }
    std::wstring s;
    s += T(L"Ayırıcı       : ") + std::wstring(sepName) + L"\n";
    s += T(L"Sütun sayısı  : ") + FormatCountTr(columnCount) + L"\n";
    s += T(L"Satır sayısı  : ") + FormatCountTr(rows.size()) + L"\n";
    s += T(L"Başlık satırı : ") + std::wstring(hasHeaderRow ? T(L"var") : T(L"yok")) + L"\n";
    return s;
}

} // namespace sro
