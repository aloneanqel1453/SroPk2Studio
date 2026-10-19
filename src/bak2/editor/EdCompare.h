// SkillEffect Studio - editor/EdCompare.h
// Client compare (A = reference, B = the other): every textdata table (rows added / changed / removed, matched by
// the table key; skilldata ENC compared decrypted; byte-identical tables skipped fast), tables only in one client,
// and optionally the archive file trees (Data / Particles / Media: files only in A / only in B). Read-only.
#pragma once

#include "EdSchema.h"

#include "../core/Client.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace se::ed {

struct RowDiff {
    wchar_t op = L'~';              // '+' only in B, '-' only in A, '~' changed
    std::wstring key;
    TKind kind = TKind::Unknown;    // row kind (skilleffect section)
    int lineA = -1, lineB = -1;     // physical lines (0-based)
    std::vector<int> cols;          // changed columns ('~')
    std::wstring a, b;              // row text (TAB separated)
};

struct TableDiff {
    enum class St { Same, Changed, OnlyA, OnlyB, Error } st = St::Same;
    std::wstring name;
    TKind kind = TKind::Unknown;
    int added = 0, removed = 0, changed = 0, same = 0;
    std::vector<RowDiff> rows;      // capped by CompareOpts::maxRows
    bool truncated = false;
    bool byteSame = false;
    int colsA = 0, colsB = 0;       // most common cell count (a different layout makes every row 'changed')
    std::wstring err;
};

struct FileDiff {
    int arc = 0;                    // 0 Media, 1 Data, 2 Particles
    std::wstring path;
    wchar_t op = L'+';              // '+' only in B, '-' only in A
};

struct CompareOpts {
    bool tables = true;
    bool files = false;             // walk the archive trees (presence only)
    std::vector<std::wstring> only; // table names (empty = all)
    size_t maxRows = 3000;          // per table
    size_t maxFiles = 200000;
    std::function<void(int percent, const std::wstring& what)> progress;
    const std::atomic<bool>* cancel = nullptr;
};

struct ClientDiff {
    std::wstring descA, descB;
    std::vector<TableDiff> tables;
    std::vector<FileDiff> files;
    bool filesTruncated = false;
    int tablesSame = 0, tablesChanged = 0, onlyA = 0, onlyB = 0;
    int rowsAdded = 0, rowsRemoved = 0, rowsChanged = 0;
    double seconds = 0;
    std::wstring Report(bool en, size_t rowsPerTable = 40) const;
};

TableDiff CompareTable(const std::wstring& name, const Bytes& a, const Bytes& b, size_t maxRows);
ClientDiff CompareClients(const ClientBase& A, const ClientBase& B, const CompareOpts& o);
// every file under an archive folder (recursive, inner paths)
void WalkArchive(const ClientBase& c, Arc a, const std::wstring& dir, std::vector<std::wstring>& out, size_t cap,
                 const std::atomic<bool>* cancel = nullptr);

}  // namespace se::ed
