// ============================================================================
//  SRO Asset Studio - formats/TextFile.h
//
//  Text loading with encoding detection, plus tab/comma separated table
//  splitting for the grid view.
//
//  IMPORTANT for Silkroad: most of the game's .txt data files are UTF-16LE
//  WITH a BOM (textdata_*.txt, skilldata_*.txt, ...). Others are plain ANSI in
//  CP1254 (Turkish private servers) or CP949 (Korean originals). Detection
//  order therefore is:
//
//    1. BOM: UTF-16LE, UTF-16BE, UTF-8
//    2. Statistical UTF-16LE sniff (many 0x00 bytes at odd positions)
//    3. Valid UTF-8 sequence check
//    4. Fall back to a code page (default CP1254; CP949 when the byte
//       distribution looks like double-byte Hangul)
//
//  Implementation owner: Agent D (formats).
// ============================================================================
#pragma once

#include "../Common.h"

namespace sro {

enum class TextEncoding : int {
    Unknown = 0,
    Utf8,
    Utf8Bom,
    Utf16Le,
    Utf16LeBom,
    Utf16Be,
    Utf16BeBom,
    Cp1252,
    Cp1254,      ///< Turkish
    Cp949,       ///< Korean
    Ansi         ///< the system code page
};

const wchar_t* TextEncodingName(TextEncoding e);

struct TextDocument {
    std::wstring text;                    ///< decoded, newlines normalised to \n
    TextEncoding encoding = TextEncoding::Unknown;
    bool         hadBom   = false;
    bool         crlf     = false;        ///< original line endings were CRLF
    size_t       lineCount = 0;
    size_t       byteSize  = 0;
    bool         truncated = false;       ///< hit the size cap

    std::wstring summaryTr() const;
};

/// Detect the encoding of a buffer without decoding it.
TextEncoding DetectEncoding(const u8* data, size_t size);

/// Decode a buffer to UTF-16.
/// `maxBytes` caps how much is decoded (0 = no cap); the viewer passes a few
/// MB so a 200 MB text file cannot stall the UI.
Result LoadTextDocument(const u8* data, size_t size, TextDocument& out,
                        size_t maxBytes = 0,
                        TextEncoding forceEncoding = TextEncoding::Unknown);

// ---------------------------------------------------------------------------
// Tabular data
// ---------------------------------------------------------------------------
enum class TableDelimiter : int {
    None = 0,
    Tab,
    Comma,
    Semicolon,
    Pipe
};

struct TableData {
    TableDelimiter delimiter = TableDelimiter::None;
    std::vector<std::wstring>              headers;   ///< empty when the file has none
    std::vector<std::vector<std::wstring>> rows;
    size_t columnCount = 0;
    bool   hasHeaderRow = false;

    std::wstring summaryTr() const;
};

/// Guess the delimiter by consistency of field counts across the first lines.
/// Returns TableDelimiter::None when the file is not tabular.
TableDelimiter DetectDelimiter(const std::wstring& text);

/// Split `text` into a grid. `maxRows` caps the row count (0 = unlimited).
/// Silkroad data files have no header row; `assumeHeaderRow` lets the caller
/// force one when it knows better.
Result ParseTable(const std::wstring& text, TableDelimiter delimiter,
                  bool assumeHeaderRow, size_t maxRows, TableData& out);

} // namespace sro
