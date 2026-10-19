// SkillEffect Studio - editor/EdLang.h
// v6 P4 M19: the model of the "Ad / çeviri" (names / translation) page. The client's name tables
// (textdata_object*.txt, textdata_equip&skill*.txt) of an editor Workspace as rows: code | kind | language cells.
// The language columns are found from the CONTENT of every column (Hangul / Turkish letters / ASCII / Arabic / CJK /
// Vietnamese ... statistics), never from a fixed layout; the column map is reported. Filters: translation missing,
// no name, EN = KR. CSV / TSV export and import (UTF-8 BOM, quotes, delimiter / quote / line-break escaping).
// Every edit goes through Workspace::SetCell (one undo step per action), so saving is the normal table save of the
// editor (crash lock + preview); the model never writes a file.
#pragma once

#include "EdDoc.h"

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace se::ed {

enum class LangId : int { KR = 0, EN, TR, AR, ZH, JA, TH, RU, VI, DE, ES, Other, Count };
const wchar_t* LangCode(LangId l);              // "KR", "EN", "TR" ... ("L" for Other)
const wchar_t* LangName(LangId l, bool en);     // "Korece" / "Korean" ...

enum class NameKind : std::uint8_t { Mob = 0, Npc, Item, Skill, Pet, Structure, Quest, Other, Count };
const wchar_t* NameKindText(NameKind k, bool en);
NameKind NameKindOf(const std::wstring& code, bool* isDesc = nullptr);  // from the StrID ("SN_MOB_..." -> Mob)

// one column of one table as found from its content
struct LangCol {
    int col = -1;               // cell index in the row (0 = Service, 1 = StrID)
    LangId lang = LangId::Other;
    int filled = 0;             // rows with text (not empty, not a bare number like "0")
    double share = 0;           // share of the filled cells that look like `lang`
    bool copyOfEn = false;      // mostly the same text as the main EN column (a copy, not a language)
    std::wstring note;          // why ("Hangul %92", "Türkçe harf %40", ...)
};

struct LangTable {
    int doc = -1;               // Workspace doc id
    const EdDoc* ptr = nullptr; // to notice a closed / replaced doc
    std::wstring name;
    int rows = 0;               // name rows (StrID not empty)
    int cellsMax = 0;           // most common cell count of a row
    std::vector<LangCol> cols;  // columns with text (col >= 2), in file order
    std::vector<int> slotCol;   // column of every LangModel slot in this table (-1 = none)
    std::vector<std::uint8_t> slotGuess;  // 1 = the column was not found in this table (taken from the other table /
                                          //     the standard layout): shown as "(tahmini)"
};

// one language column of the page (same order in every table)
struct LangSlot {
    LangId lang = LangId::Other;
    int nth = 0;                // 0 = the main column of the language, 1 = a second one ...
    std::wstring code;          // header token "EN", "EN2", "TR", "L13" (CSV header)
    bool guess = false;         // not found from content in any table (standard layout): "(tahmini)"
};

struct LangRow {
    int table = -1;
    std::uint64_t uid = 0;
    std::wstring code;          // StrID
    int occ = 0;                // 0 = first row of this StrID in its table, 1 = the second ...
    NameKind kind = NameKind::Other;
    bool desc = false;          // a description string (..._TT_DESC / _DESC)
};

enum class LangFilter : int { All = 0, MissingTr, NoName, EnEqKr, Count };

struct LangImport {
    int rows = 0;               // data rows read
    int matched = 0;            // rows whose code was found
    int changedRows = 0;        // rows with at least one changed cell
    int changedCells = 0;
    int unknown = 0;            // codes not in the tables
    int badCells = 0;           // TAB / line break in a value (skipped)
    int noColumn = 0;           // value for a language the row's table has no column for (skipped)
    std::vector<std::wstring> notesTr, notesEn;  // first problems (at most 20)
    std::wstring errTr, errEn;  // fatal (no header / no code column)
};

class LangModel {
public:
    // opens in ws the name tables listed in textdataname.txt (textdata_object* / textdata_equip*), else the two
    // standard file names, and builds the model. false + err when none can be opened.
    bool Build(Workspace& ws, std::wstring& err);
    // the given open docs (tests, synthetic tables)
    bool BuildFrom(Workspace& ws, const std::vector<int>& docs);
    // the workspace is the same and every table is still open (a client change closes them)
    bool Valid(const Workspace& ws) const;

    const std::vector<LangTable>& Tables() const { return m_tables; }
    const std::vector<LangSlot>& Slots() const { return m_slots; }
    const std::vector<LangRow>& Rows() const { return m_rows; }
    int SlotOf(LangId l, int nth = 0) const;   // -1 when absent
    std::wstring SlotLabel(int slot, bool en) const;  // "EN", "TR (tahmini)" ...

    // current text of a cell ("" when the row has no such cell / column); live (follows undo / redo)
    std::wstring Cell(int row, int slot) const;
    static bool HasText(const std::wstring& v);  // not empty, not a bare number ("0" placeholder)
    bool CellEdited(int row, int slot) const;   // differs from the table as loaded (unsaved or saved this session)
    bool Match(int row, LangFilter f) const;
    // rows of a filter + text search (code or any language cell contains text, case-insensitive)
    std::vector<int> Filter(LangFilter f, const std::wstring& text = L"") const;
    int Count(LangFilter f) const;
    // the column map as text (one line per table): "textdata_object.txt (17 sütun): KR=2 EN=9 EN2=8(kopya) TR=12 ..."
    std::wstring Report(bool en) const;

    // edits (one undo step each). false + why when the value has TAB / line break or the table has no such column.
    bool SetText(int row, int slot, const std::wstring& value, std::wstring* whyTr = nullptr, std::wstring* whyEn = nullptr);
    // several cells in one undo step: (row, slot, value); returns the number of cells changed
    int SetTexts(const std::vector<std::tuple<int, int, std::wstring>>& edits, const std::wstring& label);

    // CSV (',' / ';') or TSV: header tablo | kod | tür | <slot codes>; UTF-8 with BOM, quoted when needed
    Bytes Export(const std::vector<int>& rows, bool tsv, bool en) const;
    // reads CSV / TSV (UTF-8 with or without BOM, UTF-16) and applies the changed cells in one undo step
    // (apply = false: only counts). Rows are found by table + code (code alone when the table column is missing).
    LangImport Import(const Bytes& data, bool apply);

    int FindRow(const std::wstring& table, const std::wstring& code, int occ = 0) const;  // -1 when absent

private:
    const EdDoc* DocOf(int table) const;
    void Detect(LangTable& t, const EdDoc& d);
    void MakeSlots();
    Workspace* m_ws = nullptr;
    std::vector<LangTable> m_tables;
    std::vector<LangSlot> m_slots;
    std::vector<LangRow> m_rows;
};

// CSV / TSV helpers (exposed for tests)
std::wstring CsvField(const std::wstring& v, wchar_t delim);
// parses a whole text: quoted fields, doubled quotes, CR / LF / CRLF; delimiter given
std::vector<std::vector<std::wstring>> CsvParse(const std::wstring& text, wchar_t delim);

}  // namespace se::ed
