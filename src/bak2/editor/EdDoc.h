// SkillEffect Studio - editor/EdDoc.h
// Editable client text table on top of core TableDoc: every untouched physical line is written back byte for byte,
// an edited row is re-encoded in place (its '//' tail / '*/' head and its line end kept), new rows are spliced in
// before a physical line, deleted rows are dropped. skilldata_*ENC.txt is decrypted for editing and encrypted again
// on save. skilleffect.txt rows carry their #section kind. All edits go through Workspace (undo / redo).
#pragma once

#include "EdSchema.h"

#include "../core/Client.h"
#include "../core/Table.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace se::ed {

struct EdRow {
    std::uint64_t uid = 0;
    int line = -1;                    // physical line of the loaded file (-1 = new row)
    int before = -1;                  // new row: written before this physical line (== line count: end of file)
    TKind kind = TKind::Generic;      // schema of the row (the #section kind in skilleffect.txt)
    std::vector<std::wstring> cells;  // raw TAB split, empty cells kept
    bool deleted = false;
};

struct SecRange {
    TKind kind = TKind::Unknown;
    int header = -1;   // physical line of "#section\t..."
    int endLine = 0;   // next header line or the line count
};

class EdDoc {
public:
    bool Load(const std::wstring& name, const Bytes& raw, std::wstring& err);
    Bytes Save() const;  // full file bytes (encrypted again for ENC)
    bool Dirty() const;                 // differs from the bytes loaded
    bool Unsaved() const { return revision != savedRev; }  // edited since the last save / load
    int Changes(int* added = nullptr, int* edited = nullptr, int* removed = nullptr) const;

    const Schema& SchemaOf(const EdRow& r) const;
    const Schema& SchemaOf(TKind k) const;
    int IndexOf(std::uint64_t uid) const;  // -1 when absent
    EdRow* Row(std::uint64_t uid);
    const EdRow* Row(std::uint64_t uid) const;
    // visible (not deleted) row indexes of one kind (TKind::Unknown = all)
    std::vector<int> Visible(TKind k = TKind::Unknown) const;
    // the cells as loaded (empty for a new row)
    std::vector<std::wstring> OrigCells(const EdRow& r) const;
    bool RowEdited(const EdRow& r) const;
    // rows whose key column equals key (kind Unknown = any)
    std::vector<int> FindKey(const std::wstring& key, TKind k = TKind::Unknown) const;
    // physical line before which a new row goes so that it is read right after `anchor` / at the end of a section
    int InsertPointAfter(const EdRow& anchor) const;
    int InsertPointEnd(TKind k) const;
    // true when a row written before physical line `before` would land inside a /* */ block (client never reads it)
    bool InsideBlock(int before) const;
    std::vector<TKind> Kinds() const;  // row kinds present (sections for skilleffect)
    std::wstring RowText(const EdRow& r) const { return JoinTabs(r.cells); }
    int LineCount() const { return static_cast<int>(base.Count()); }

    std::wstring name;        // textdata file name ("skilleffect.txt", "characterdata_10000.txt" ...)
    std::wstring origin;      // where it was read from (shown in the UI)
    TKind kind = TKind::Unknown;
    bool enc = false;         // skilldata ENC
    TableDoc base;            // plain text as loaded
    Bytes loadedRaw;          // bytes as read (encrypted for ENC)
    Schema generic;           // inferred schema (Generic tables)
    std::vector<EdRow> rows;  // data rows in display order; deleted rows stay flagged
    std::vector<SecRange> sections;
    std::vector<int> rowOfLine;  // physical line -> index (at load) of its data row (-1)
    std::vector<std::uint8_t> blockAt;  // per physical line (+1 for EOF): 1 = the line starts inside /* */
    std::uint64_t revision = 0;
    std::uint64_t savedRev = 0;
    std::uint64_t nextUid = 1;

private:
    friend class Workspace;
    void Reindex() const;
    mutable std::unordered_map<std::uint64_t, int> m_index;
    mutable std::uint64_t m_indexRev = ~0ull;
};

// one primitive edit (undo data included)
struct Op {
    enum class T { Set, Insert, Delete, Undelete } t = T::Set;
    int doc = -1;
    std::uint64_t uid = 0;
    int col = -1;
    std::wstring before, after;  // Set
    int size0 = -1;              // Set: cell count before (a set past the end pads the row)
    EdRow row;                   // Insert (row as inserted)
    int pos = 0;                 // Insert: index in rows
};
struct Change {
    std::wstring label;
    std::vector<Op> ops;
};

// the set of open tables + the undo / redo history of every edit
class Workspace {
public:
    explicit Workspace(const ClientBase* c = nullptr) : client(c) {}
    const ClientBase* client = nullptr;
    std::vector<std::unique_ptr<EdDoc>> docs;

    // textdata file names of the client (+ overlay), sorted case-insensitively
    std::vector<std::wstring> ListTables() const;
    // open (or return the already open) table; -1 + err when it cannot be read
    int Open(const std::wstring& name, std::wstring& err);
    int Find(const std::wstring& name) const;  // case-insensitive, -1 when not open
    int Add(std::unique_ptr<EdDoc> d);
    // every split file listed in an index file ("characterdata.txt" -> characterdata_5000.txt ...); returns doc ids
    std::vector<int> OpenIndexed(const std::wstring& indexName, std::wstring& err);
    EdDoc* Doc(int i) { return i >= 0 && i < static_cast<int>(docs.size()) ? docs[static_cast<size_t>(i)].get() : nullptr; }
    const EdDoc* Doc(int i) const { return i >= 0 && i < static_cast<int>(docs.size()) ? docs[static_cast<size_t>(i)].get() : nullptr; }

    // edits (recorded; inside Begin/End they form one undo step)
    bool SetCell(int doc, std::uint64_t uid, int col, const std::wstring& value);
    std::uint64_t InsertRow(int doc, int pos, EdRow row);  // row.before must be set; returns its uid
    std::uint64_t InsertAfter(int doc, std::uint64_t anchorUid, const std::vector<std::wstring>& cells);
    std::uint64_t InsertAtEnd(int doc, TKind k, const std::vector<std::wstring>& cells);
    bool DeleteRow(int doc, std::uint64_t uid);
    bool RestoreRow(int doc, std::uint64_t uid);
    void Begin(const std::wstring& label);
    void End();
    bool Undo();
    bool Redo();
    bool CanUndo() const { return !undo.empty(); }
    bool CanRedo() const { return !redo.empty(); }
    std::wstring UndoLabel() const { return undo.empty() ? L"" : undo.back().label; }
    std::wstring RedoLabel() const { return redo.empty() ? L"" : redo.back().label; }
    void ClearHistory();
    std::function<void()> onChange;  // UI refresh hook

    std::vector<Change> undo, redo;
    size_t maxUndo = 500;

private:
    void Record(Op op);
    void Apply(const Op& op, bool forward);
    int m_depth = 0;
    Change m_open;
};

// textdata names of a client (Media.pk2 or loose folder, + overlay), case-insensitive unique
std::vector<std::wstring> ListTextdata(const ClientBase& c);
// v5 M9 (crash recovery): the file bytes of a table (as EdDoc::Save wrote them) applied to the open document `doc` as
// edits (changed cells, new rows, deleted rows) in one undo step, so it shows unsaved with its changes marked.
// Returns the number of edits (-1 + err when the bytes cannot be read); err is also set when the result is not
// byte-identical (a row's trailing comment / format), the edits stay.
int ApplyBytesAsEdits(Workspace& ws, int doc, const Bytes& bytes, std::wstring& err);
// the index file names -> the split files they list
std::vector<std::wstring> ReadIndexList(const Bytes& raw);

}  // namespace se::ed
