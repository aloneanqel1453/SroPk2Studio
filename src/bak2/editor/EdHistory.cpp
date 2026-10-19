// SkillEffect Studio - editor/EdHistory.cpp
#include "EdHistory.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <map>

namespace se::ed {

namespace {

constexpr const wchar_t* kHist = L"_editor_history";

std::wstring Now(bool fileSafe) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    return fileSafe ? Fmt(L"%04u%02u%02u_%02u%02u%02u_%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds)
                    : Fmt(L"%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

bool MakeDirs(const std::wstring& d) {
    if (d.empty() || DirExists(d)) return true;
    const std::wstring up = DirOf(d);
    if (!up.empty() && up != d && !MakeDirs(up)) return false;
    return CreateDirectoryW(d.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool HasMatch(const std::wstring& dir, const wchar_t* pattern) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool any = false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        // exact extension (FindFirstFile "*.pk2" also matches 8.3 names like "x.pk2x")
        const std::wstring n = fd.cFileName;
        const std::wstring p = pattern;
        if (p.size() > 1 && p[0] == L'*' && !EndsWithI(n, p.substr(1))) continue;
        any = true;
        break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return any;
}

std::wstring FullPath(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetFullPathNameW(p.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
    std::wstring r = n && n < std::size(buf) ? std::wstring(buf, n) : p;
    while (r.size() > 3 && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    return r;
}

std::vector<std::wstring> SplitTsv(const std::wstring& line) {
    std::vector<std::wstring> v;
    for (auto c : SplitView(line, L'\t')) v.emplace_back(c);
    return v;
}

std::wstring Esc(std::wstring s) {
    for (auto& c : s)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return s;
}

}  // namespace

std::wstring HashHex(const Bytes& b) {
    unsigned long long h = 1469598103934665603ULL;
    for (auto c : b) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return Fmt(L"%016llx", h);
}

bool IsSafeOutDir(const std::wstring& dirIn, std::wstring& tr, std::wstring& en) {
    if (dirIn.empty()) {
        tr = L"Çıktı klasörü seçilmedi.";
        en = L"No output folder.";
        return false;
    }
    const std::wstring dir = FullPath(dirIn);
    const std::wstring up = ToUpper(dir);
    for (const wchar_t* sys : {L"C:\\WINDOWS", L"C:\\PROGRAM FILES", L"C:\\PROGRAMDATA"})
        if (StartsWith(up, sys)) {
            tr = L"Sistem klasörüne yazılmaz: " + dir;
            en = L"Never writes into a system folder: " + dir;
            return false;
        }
    if (up.size() <= 3) {
        tr = L"Sürücü köküne yazılmaz.";
        en = L"Never writes into a drive root.";
        return false;
    }
    // the folder itself and every ancestor: no client archives / executables
    std::wstring d = dir;
    for (int i = 0; i < 12 && d.size() > 3; ++i) {
        if (DirExists(d)) {
            for (const wchar_t* p : {L"*.pk2", L"sro_client.exe", L"silkroad.exe", L"*.exe"}) {
                if (i > 0 && std::wstring(p) == L"*.exe") continue;  // an ancestor may hold tools; only the folder itself is strict
                if (HasMatch(d, p)) {
                    tr = L"Bu klasör (ya da üstü) bir oyun/istemci klasörü (" + std::wstring(p) + L" var): " + d +
                         L". Oyun klasörlerine ASLA yazılmaz; ayrı bir çıktı klasörü seçin.";
                    en = L"This folder (or a parent) is a game / client folder (" + std::wstring(p) + L"): " + d +
                         L". The editor never writes into game folders; pick a separate output folder.";
                    return false;
                }
            }
        }
        const std::wstring u = DirOf(d);
        if (u == d || u.empty()) break;
        d = u;
    }
    if (up.find(L"\\SERVER_DEP\\SILKROAD") != std::wstring::npos) {
        tr = L"İstemci textdata klasörüne yazılmaz; çıktı klasörü Media\\server_dep\\... düzenini kendisi kurar.";
        en = L"Never writes into a client textdata folder; the output folder builds Media\\server_dep\\... itself.";
        return false;
    }
    if (!MakeDirs(dir)) {
        tr = L"Klasör oluşturulamadı: " + dir;
        en = L"Cannot create the folder: " + dir;
        return false;
    }
    return true;
}

bool EdHistory::Init(const std::wstring& outDir, std::wstring& err) {
    std::wstring tr, en;
    if (!IsSafeOutDir(outDir, tr, en)) {
        err = tr + L"\n" + en;
        return false;
    }
    m_out = FullPath(outDir);
    if (!MakeDirs(JoinPath(HistDir(), L"backup"))) {
        err = L"Geçmiş klasörü oluşturulamadı / cannot create history folder";
        return false;
    }
    return true;
}

std::wstring EdHistory::HistDir() const { return JoinPath(m_out, kHist); }

std::wstring EdHistory::TextdataPath(const std::wstring& n) const {
    return JoinPath(JoinPath(m_out, L"Media\\server_dep\\silkroad\\textdata"), n);
}

std::wstring EdHistory::Rel(const std::wstring& abs) const {
    if (abs.size() > m_out.size() && _wcsnicmp(abs.c_str(), m_out.c_str(), m_out.size()) == 0 && abs[m_out.size()] == L'\\')
        return abs.substr(m_out.size() + 1);
    return abs;
}

bool EdHistory::AppendJournal(const WriteRecord& r, std::wstring& err) {
    const std::wstring jp = JoinPath(HistDir(), L"journal.tsv");
    Bytes old;
    std::wstring e;
    if (FileExists(jp) && !ReadFileBytes(jp, old, e)) {
        err = e;
        return false;
    }
    std::string line = ToUtf8(Esc(r.time) + L"\t" + Esc(r.action) + L"\t" + Esc(r.target) + L"\t" + Esc(r.backup) + L"\t" +
                              std::to_wstring(r.sizeBefore) + L"\t" + std::to_wstring(r.sizeAfter) + L"\t" + r.hashBefore + L"\t" +
                              r.hashAfter + L"\t" + Esc(r.note)) +
                       "\r\n";
    if (old.empty()) {
        const std::string head = "time\taction\ttarget\tbackup\tsizeBefore\tsizeAfter\thashBefore\thashAfter\tnote\r\n";
        old.insert(old.end(), head.begin(), head.end());
    }
    old.insert(old.end(), line.begin(), line.end());
    return WriteFileAtomic(jp, old, err);
}

bool EdHistory::Write(const std::wstring& absPath, const Bytes& data, const std::wstring& note, std::wstring& err) {
    if (m_out.empty()) {
        err = L"Çıktı klasörü yok / no output folder";
        return false;
    }
    const std::wstring full = FullPath(absPath);
    if (Rel(full) == full) {
        err = L"Hedef çıktı klasörünün dışında / target outside the output folder: " + full;
        return false;
    }
    if (!MakeDirs(DirOf(full))) {
        err = L"Klasör oluşturulamadı / cannot create: " + DirOf(full);
        return false;
    }
    WriteRecord r;
    r.time = Now(false);
    r.action = L"write";
    r.target = Rel(full);
    r.note = note;
    r.sizeAfter = static_cast<long long>(data.size());
    r.hashAfter = HashHex(data);
    if (FileExists(full)) {
        Bytes old;
        if (!ReadFileBytes(full, old, err)) return false;
        r.sizeBefore = static_cast<long long>(old.size());
        r.hashBefore = HashHex(old);
        if (r.hashBefore == r.hashAfter && old == data) return true;  // nothing to do
        std::wstring flat = r.target;
        for (auto& c : flat)
            if (c == L'\\' || c == L'/') c = L'~';
        const std::wstring bk = JoinPath(JoinPath(HistDir(), L"backup"), Now(true) + L"__" + flat);
        if (!WriteFileAtomic(bk, old, err)) return false;
        r.backup = Rel(bk);
    }
    if (!WriteFileAtomic(full, data, err)) return false;
    Bytes back;
    if (!ReadFileBytes(full, back, err) || back != data) {
        err = L"Yazılan dosya geri okunamadı / verify failed: " + full;
        return false;
    }
    return AppendJournal(r, err);
}

std::vector<WriteRecord> EdHistory::Records() const {
    std::vector<WriteRecord> v;
    Bytes b;
    std::wstring e;
    if (!ReadFileBytes(JoinPath(HistDir(), L"journal.tsv"), b, e)) return v;
    const std::wstring t = FromUtf8(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
    bool head = true;
    for (auto line : SplitView(t, L'\n')) {
        std::wstring s(line);
        if (!s.empty() && s.back() == L'\r') s.pop_back();
        if (s.empty()) continue;
        if (head) {
            head = false;
            continue;
        }
        const auto c = SplitTsv(s);
        if (c.size() < 9) continue;
        WriteRecord r;
        r.time = c[0];
        r.action = c[1];
        r.target = c[2];
        r.backup = c[3];
        long long x = 0;
        if (ParseInt(c[4], x)) r.sizeBefore = x;
        if (ParseInt(c[5], x)) r.sizeAfter = x;
        r.hashBefore = c[6];
        r.hashAfter = c[7];
        r.note = c[8];
        v.push_back(std::move(r));
    }
    return v;
}

std::vector<BackupInfo> EdHistory::Backups() const {
    std::vector<BackupInfo> v;
    const std::wstring dir = JoinPath(HistDir(), L"backup");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring n = fd.cFileName;
        const size_t sep = n.find(L"__");
        if (sep == std::wstring::npos) continue;
        BackupInfo b;
        b.file = Rel(JoinPath(dir, n));
        b.original = n.substr(sep + 2);
        for (auto& c : b.original)
            if (c == L'~') c = L'\\';
        const std::wstring t = n.substr(0, sep);  // yyyymmdd_hhmmss_mmm
        if (t.size() >= 15)
            b.time = t.substr(0, 4) + L"-" + t.substr(4, 2) + L"-" + t.substr(6, 2) + L" " + t.substr(9, 2) + L":" + t.substr(11, 2) + L":" +
                     t.substr(13, 2);
        b.size = (static_cast<long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        v.push_back(std::move(b));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(v.begin(), v.end(), [](const BackupInfo& a, const BackupInfo& b) { return a.file > b.file; });  // newest first
    return v;
}

bool EdHistory::Restore(const BackupInfo& b, std::wstring& err) {
    Bytes data;
    if (!ReadFileBytes(JoinPath(m_out, b.file), data, err)) return false;
    return Write(JoinPath(m_out, b.original), data, L"restore " + b.file, err);
}

bool EdHistory::RevertLast(std::wstring& err) {
    const auto recs = Records();
    for (auto it = recs.rbegin(); it != recs.rend(); ++it) {
        if (it->action != L"write") continue;
        if (it->backup.empty()) {
            err = L"Son yazılan dosya yeniydi (yedeği yok): " + it->target + L" — kaldırmak için dosyayı elle silin.\n"
                  L"The newest written file was new (no backup): " + it->target;
            return false;
        }
        BackupInfo b;
        b.file = it->backup;
        b.original = it->target;
        return Restore(b, err);
    }
    err = L"Geri alınacak yazma yok / nothing to revert";
    return false;
}

int EdHistory::Prune(int keep, std::wstring& err) {
    std::map<std::wstring, std::vector<BackupInfo>> by;
    for (auto& b : Backups()) by[ToUpper(b.original)].push_back(b);
    int removed = 0;
    for (auto& [k, v] : by) {
        (void)k;
        for (size_t i = static_cast<size_t>(std::max(keep, 0)); i < v.size(); ++i) {
            const std::wstring p = JoinPath(m_out, v[i].file);
            if (DeleteFileW(p.c_str())) {
                ++removed;
                WriteRecord r;
                r.time = Now(false);
                r.action = L"delete-backup";
                r.target = v[i].file;
                r.sizeBefore = v[i].size;
                AppendJournal(r, err);
            }
        }
    }
    return removed;
}

bool SaveDoc(Workspace& ws, int doc, EdHistory& h, std::wstring& err, std::wstring* savedPath) {
    EdDoc* d = ws.Doc(doc);
    if (!d) {
        err = L"tablo yok / no table";
        return false;
    }
    const Bytes data = d->Save();
    const std::wstring p = h.TextdataPath(d->name);
    int a = 0, e = 0, r = 0;
    d->Changes(&a, &e, &r);
    if (!h.Write(p, data, Fmt(L"%ls: +%d ~%d -%d", d->name.c_str(), a, e, r), err)) return false;
    d->savedRev = d->revision;
    if (savedPath) *savedPath = p;
    return true;
}

int SaveAll(Workspace& ws, EdHistory& h, std::wstring& log) {
    int n = 0;
    for (size_t i = 0; i < ws.docs.size(); ++i) {
        EdDoc* d = ws.docs[i].get();
        if (!d->Unsaved() && !d->Dirty()) continue;
        if (!d->Unsaved()) continue;
        std::wstring err, p;
        if (SaveDoc(ws, static_cast<int>(i), h, err, &p)) {
            ++n;
            log += L"kaydedildi / saved: " + p + L"\n";
        } else {
            log += L"HATA / ERROR " + d->name + L": " + err + L"\n";
        }
    }
    return n;
}

}  // namespace se::ed
