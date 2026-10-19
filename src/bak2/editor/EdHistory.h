// SkillEffect Studio - editor/EdHistory.h
// Every file the editor writes goes through EdHistory: the output folder is checked (never a client / game folder),
// an existing target is copied to <out>\_editor_history\backup\ first, the new bytes are written atomically (temp +
// MoveFileEx) and re-read, and one line is appended to <out>\_editor_history\journal.tsv. The backup manager lists
// the records / backups, restores a backup (itself recorded) and prunes old backups. In-memory undo / redo of the
// edits lives in Workspace (EdDoc.h).
#pragma once

#include "EdDoc.h"

#include <string>
#include <vector>

namespace se::ed {

struct WriteRecord {
    std::wstring time;        // local "2026-10-01 23:59:59"
    std::wstring action;      // write / restore / delete-backup
    std::wstring target;      // relative to the output folder
    std::wstring backup;      // relative backup path ("" = the file was new)
    long long sizeBefore = -1, sizeAfter = -1;
    std::wstring hashBefore, hashAfter;  // FNV-1a 64 hex
    std::wstring note;
};

struct BackupInfo {
    std::wstring file;        // relative to the output folder
    std::wstring original;    // relative target it was saved from
    std::wstring time;
    long long size = 0;
};

// refuses client / game folders: a folder (or an ancestor) holding *.pk2 / sro_client.exe / silkroad.exe, a Media /
// server_dep tree of a client, system folders. Creates the folder when it does not exist.
bool IsSafeOutDir(const std::wstring& dir, std::wstring& whyTr, std::wstring& whyEn);

class EdHistory {
public:
    bool Init(const std::wstring& outDir, std::wstring& err);
    const std::wstring& OutDir() const { return m_out; }
    std::wstring HistDir() const;
    // absolute path of a textdata table in the output layout (read back by ClientBase::SetOverlay)
    std::wstring TextdataPath(const std::wstring& tableName) const;
    bool Write(const std::wstring& absPath, const Bytes& data, const std::wstring& note, std::wstring& err);
    std::vector<WriteRecord> Records() const;
    std::vector<BackupInfo> Backups() const;
    bool Restore(const BackupInfo& b, std::wstring& err);   // the current file is backed up first
    bool RevertLast(std::wstring& err);                     // restores the backup of the newest write
    int Prune(int keepPerFile, std::wstring& err);          // keep the newest N backups per original
    std::wstring Rel(const std::wstring& abs) const;

private:
    bool AppendJournal(const WriteRecord& r, std::wstring& err);
    std::wstring m_out;
};

// saves one table of the workspace into the output folder (Media\server_dep\silkroad\textdata\<name>)
bool SaveDoc(Workspace& ws, int doc, EdHistory& h, std::wstring& err, std::wstring* savedPath = nullptr);
// every dirty table; returns the number saved
int SaveAll(Workspace& ws, EdHistory& h, std::wstring& log);

std::wstring HashHex(const Bytes& b);

}  // namespace se::ed
