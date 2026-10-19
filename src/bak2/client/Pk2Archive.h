// SkillEffect Studio v3 - client/Pk2Archive.h
// Read-only JMXPACK (.pk2 / .lib) archive. Opened with GENERIC_READ only, never written.
//  * names are matched case-insensitively (ASCII folding on the CP949 bytes, like the client), '/' or '\'
//  * folders are decoded lazily and cached; a full index (every file) is built on demand for search / enumeration
//  * Read / Stat / List are thread-safe (positional reads, the folder cache is locked)
#pragma once

#include "../core/Text.h"
#include "Pk2Crypto.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace se::client {

struct DirItem {
    std::wstring name;  // display name (CP949 decoded, original case of the archive / folder)
    bool folder = false;
    std::uint64_t size = 0;
    std::uint64_t mtime = 0;  // FILETIME
};

class Pk2Archive {
public:
    Pk2Archive();
    ~Pk2Archive();
    Pk2Archive(const Pk2Archive&) = delete;
    Pk2Archive& operator=(const Pk2Archive&) = delete;

    // keySpec: see Pk2Crypto.h. Fails with a plain reason (Turkish + English) when the key does not open it.
    bool Open(const std::wstring& path, const std::string& keySpec, std::wstring& whyTr, std::wstring& whyEn);
    void Close();
    bool IsOpen() const;

    bool Read(std::wstring_view inner, Bytes& out) const;
    // first n bytes (header sniffing without reading a whole file)
    bool ReadHead(std::wstring_view inner, size_t n, Bytes& out) const;
    bool Stat(std::wstring_view inner, std::uint64_t* size = nullptr, std::uint64_t* mtime = nullptr) const;
    bool Exists(std::wstring_view inner) const { return Stat(inner); }
    bool IsFolder(std::wstring_view inner) const;
    // entries of a folder ("" = root); false when the folder does not exist
    bool List(std::wstring_view dir, std::vector<DirItem>& out) const;

    // full index (all folders decoded). Returns the file count; cancel stops early (partial index stays usable).
    size_t BuildIndex(const std::function<void(int)>& progress = {}, const std::atomic<bool>* cancel = nullptr) const;
    // every file (inner path with '\', display case); stops when fn returns false. Builds the index first.
    void ForEachFile(const std::function<bool(const std::wstring& inner, std::uint64_t size)>& fn) const;
    size_t FileCount() const;    // 0 until BuildIndex
    size_t FolderCount() const;  // decoded folders so far

    const std::wstring& Path() const { return m_path; }
    const std::string& KeySpec() const { return m_key; }
    bool Encrypted() const { return m_encrypted; }
    std::uint64_t ArchiveSize() const { return m_size; }

    // wide inner path -> lower-case CP949 parts (exposed for tests)
    static std::vector<std::string> SplitInner(std::wstring_view inner);

private:
    struct Entry {
        std::string lname;   // lower-case CP949
        std::wstring name;   // display
        bool folder = false;
        std::int64_t off = 0;
        std::uint32_t size = 0;
        std::uint64_t mtime = 0;
    };
    struct Folder {
        std::vector<Entry> items;  // without "." / ".."
        std::unordered_map<std::string, std::uint32_t> byName;
    };
    const Folder* GetFolder(std::int64_t off) const;  // decoded + cached (locked)
    const Entry* Find(std::wstring_view inner) const;
    bool ReadAt(std::int64_t off, void* dst, std::uint32_t n) const;

    void* m_h = nullptr;  // HANDLE
    std::wstring m_path;
    std::string m_key;
    bool m_encrypted = true;
    std::uint64_t m_size = 0;
    SroBlowfish m_bf;
    mutable std::mutex m_mx;
    mutable std::unordered_map<std::int64_t, std::unique_ptr<Folder>> m_folders;
    mutable std::atomic<bool> m_indexed{false};
    mutable std::atomic<size_t> m_fileCount{0};
};

}  // namespace se::client
