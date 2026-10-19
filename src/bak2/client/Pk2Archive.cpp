// SkillEffect Studio v3 - client/Pk2Archive.cpp
#include "Pk2Archive.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace se::client {

namespace {

inline std::uint32_t Le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::int64_t Le64(const std::uint8_t* p) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(Le32(p)) | (static_cast<std::uint64_t>(Le32(p + 4)) << 32));
}

std::string LowerAscii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// v5 M3: lossless only. A name with a character CP949 does not have (Turkish ş, ğ ...) must not find another file
// through a best-fit byte (test_ş.bsr -> test_s.bsr): it becomes a name no entry can have (a NUL byte), so every lookup
// of it fails.
std::string ToCp949(std::wstring_view w) {
    if (w.empty()) return {};
    std::string s;
    if (!se::NarrowExact(949, w, s)) return std::string("\0?", 2);
    return s;
}

std::wstring FromCp949(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(949, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(949, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

}  // namespace

Pk2Archive::Pk2Archive() = default;
Pk2Archive::~Pk2Archive() { Close(); }

void Pk2Archive::Close() {
    if (m_h) CloseHandle(static_cast<HANDLE>(m_h));
    m_h = nullptr;
    std::lock_guard<std::mutex> lk(m_mx);
    m_folders.clear();
    m_indexed = false;
    m_fileCount = 0;
}

bool Pk2Archive::IsOpen() const { return m_h != nullptr; }

std::vector<std::string> Pk2Archive::SplitInner(std::wstring_view inner) {
    const std::string narrow = LowerAscii(ToCp949(inner));
    std::vector<std::string> parts;
    size_t a = 0;
    while (a <= narrow.size()) {
        size_t b = narrow.find_first_of("\\/", a);
        if (b == std::string::npos) b = narrow.size();
        if (b > a) {
            std::string p = narrow.substr(a, b - a);
            if (p != ".") parts.push_back(std::move(p));
        }
        a = b + 1;
    }
    return parts;
}

bool Pk2Archive::Open(const std::wstring& path, const std::string& keySpec, std::wstring& whyTr, std::wstring& whyEn) {
    Close();
    Pk2Probe pr;
    if (!pr.Load(path)) {
        whyTr = pr.whyTr;
        whyEn = pr.whyEn;
        return false;
    }
    m_encrypted = pr.encrypted;
    if (m_encrypted) {
        std::vector<std::uint8_t> key;
        if (!KeySpecBytes(keySpec, key)) {
            whyTr = L"Pk2 anahtarı geçersiz biçimde (örnek: 169841, ANAHTAR@PARADISE, ANAHTAR@ZERO, raw:A1B2...).";
            whyEn = L"Malformed pk2 key (e.g. 169841, KEY@PARADISE, KEY@ZERO, raw:A1B2...).";
            return false;
        }
        m_bf.Init(key.data(), key.size());
        if (!pr.RootOracle(m_bf)) {
            whyTr = L"Pk2 anahtarı bu arşivi açmıyor (kök dizin çözülemedi): " + FileNameOf(path) +
                    L". Sunucu pk2'leri farklı bir anahtarla şifrelemiş; 'Anahtarı bul' ya da doğru anahtarı yazın.";
            whyEn = L"The pk2 key does not open this archive (root folder does not decrypt): " + FileNameOf(path) +
                    L". Use 'Find key' or type the server's key.";
            return false;
        }
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        whyTr = L"Pk2 açılamadı (kilitli ya da erişim yok): " + path;
        whyEn = L"Cannot open the pk2 (locked or no access): " + path;
        return false;
    }
    m_h = h;
    m_path = path;
    m_key = keySpec;
    m_size = pr.fileSize;
    if (!GetFolder(kPk2RootOffset)) {
        Close();
        whyTr = L"Pk2 kök dizini okunamadı (arşiv bozuk): " + path;
        whyEn = L"Cannot read the pk2 root folder (damaged archive): " + path;
        return false;
    }
    return true;
}

bool Pk2Archive::ReadAt(std::int64_t off, void* dst, std::uint32_t n) const {
    if (!m_h || off < 0) return false;
    auto* p = static_cast<std::uint8_t*>(dst);
    std::uint32_t done = 0;
    while (done < n) {
        OVERLAPPED ov{};
        const std::uint64_t at = static_cast<std::uint64_t>(off) + done;
        ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;
        if (!::ReadFile(static_cast<HANDLE>(m_h), p + done, n - done, &got, &ov) || got == 0) return false;
        done += got;
    }
    return true;
}

const Pk2Archive::Folder* Pk2Archive::GetFolder(std::int64_t off) const {
    {
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_folders.find(off);
        if (it != m_folders.end()) return it->second.get();
    }
    auto f = std::make_unique<Folder>();
    std::int64_t cur = off;
    std::uint8_t blk[kPk2BlockSize];
    std::unordered_set<std::int64_t> seen;
    while (cur != 0 && seen.insert(cur).second && seen.size() < 200000) {
        if (static_cast<std::uint64_t>(cur) + kPk2BlockSize > m_size || !ReadAt(cur, blk, kPk2BlockSize)) {
            if (cur == off) return nullptr;
            break;  // damaged chain: keep what was read
        }
        if (m_encrypted) m_bf.Decode(blk, kPk2BlockSize);
        std::int64_t next = 0;
        for (std::uint32_t k = 0; k < 20; ++k) {
            const std::uint8_t* e = blk + k * kPk2EntrySize;
            if (e[0] == 1 || e[0] == 2) {
                size_t n = 0;
                while (n < 89 && e[1 + n]) ++n;
                std::string raw(reinterpret_cast<const char*>(e + 1), n);
                if (!raw.empty() && raw != "." && raw != "..") {
                    Entry en;
                    en.lname = LowerAscii(raw);
                    en.name = FromCp949(raw);
                    en.folder = e[0] == 1;
                    en.mtime = static_cast<std::uint64_t>(Le64(e + 98));
                    en.off = Le64(e + 106);
                    en.size = Le32(e + 114);
                    if (!f->byName.count(en.lname)) {  // first one wins (client behaviour for duplicate names)
                        f->byName.emplace(en.lname, static_cast<std::uint32_t>(f->items.size()));
                        f->items.push_back(std::move(en));
                    }
                }
            }
            if (k == 19) next = Le64(e + 118);
        }
        cur = next;
    }
    std::lock_guard<std::mutex> lk(m_mx);
    auto [it, inserted] = m_folders.emplace(off, std::move(f));
    return it->second.get();
}

const Pk2Archive::Entry* Pk2Archive::Find(std::wstring_view inner) const {
    const auto parts = SplitInner(inner);
    if (parts.empty()) return nullptr;
    std::int64_t dir = kPk2RootOffset;
    for (size_t i = 0; i < parts.size(); ++i) {
        const Folder* f = GetFolder(dir);
        if (!f) return nullptr;
        auto it = f->byName.find(parts[i]);
        if (it == f->byName.end()) return nullptr;
        const Entry& e = f->items[it->second];
        if (i + 1 == parts.size()) return &e;
        if (!e.folder) return nullptr;
        dir = e.off;
    }
    return nullptr;
}

bool Pk2Archive::Stat(std::wstring_view inner, std::uint64_t* size, std::uint64_t* mtime) const {
    const Entry* e = Find(inner);
    if (!e || e->folder) return false;
    if (size) *size = e->size;
    if (mtime) *mtime = e->mtime;
    return true;
}

bool Pk2Archive::IsFolder(std::wstring_view inner) const {
    if (SplitInner(inner).empty()) return IsOpen();
    const Entry* e = Find(inner);
    return e && e->folder;
}

bool Pk2Archive::Read(std::wstring_view inner, Bytes& out) const {
    out.clear();
    const Entry* e = Find(inner);
    if (!e || e->folder) return false;
    if (static_cast<std::uint64_t>(e->off) + e->size > m_size) return false;
    out.resize(e->size);
    return e->size == 0 || ReadAt(e->off, out.data(), e->size);
}

bool Pk2Archive::ReadHead(std::wstring_view inner, size_t n, Bytes& out) const {
    out.clear();
    const Entry* e = Find(inner);
    if (!e || e->folder) return false;
    const std::uint32_t take = static_cast<std::uint32_t>(std::min<std::uint64_t>(n, e->size));
    if (static_cast<std::uint64_t>(e->off) + take > m_size) return false;
    out.resize(take);
    return take == 0 || ReadAt(e->off, out.data(), take);
}

bool Pk2Archive::List(std::wstring_view dir, std::vector<DirItem>& out) const {
    out.clear();
    std::int64_t off = kPk2RootOffset;
    if (!SplitInner(dir).empty()) {
        const Entry* e = Find(dir);
        if (!e || !e->folder) return false;
        off = e->off;
    }
    const Folder* f = GetFolder(off);
    if (!f) return false;
    out.reserve(f->items.size());
    for (const auto& e : f->items) out.push_back({e.name, e.folder, e.size, e.mtime});
    return true;
}

size_t Pk2Archive::BuildIndex(const std::function<void(int)>& progress, const std::atomic<bool>* cancel) const {
    if (m_indexed) return m_fileCount;
    std::vector<std::int64_t> todo{kPk2RootOffset};
    std::unordered_set<std::int64_t> seen{kPk2RootOffset};
    size_t files = 0, done = 0;
    while (!todo.empty()) {
        if (cancel && cancel->load()) return files;
        const std::int64_t off = todo.back();
        todo.pop_back();
        const Folder* f = GetFolder(off);
        ++done;
        if (!f) continue;
        for (const auto& e : f->items) {
            if (e.folder) {
                if (seen.insert(e.off).second) todo.push_back(e.off);
            } else {
                ++files;
            }
        }
        if (progress && (done % 64) == 0) progress(static_cast<int>(std::min<size_t>(99, done * 100 / (done + todo.size()))));
    }
    m_fileCount = files;
    m_indexed = true;
    if (progress) progress(100);
    return files;
}

void Pk2Archive::ForEachFile(const std::function<bool(const std::wstring& inner, std::uint64_t size)>& fn) const {
    BuildIndex();
    struct Item {
        std::int64_t off;
        std::wstring prefix;
    };
    std::vector<Item> todo{{kPk2RootOffset, L""}};
    std::unordered_set<std::int64_t> seen{kPk2RootOffset};
    while (!todo.empty()) {
        Item it = std::move(todo.back());
        todo.pop_back();
        const Folder* f = GetFolder(it.off);
        if (!f) continue;
        for (const auto& e : f->items) {
            std::wstring p = it.prefix.empty() ? e.name : it.prefix + L"\\" + e.name;
            if (e.folder) {
                if (seen.insert(e.off).second) todo.push_back({e.off, std::move(p)});
            } else if (!fn(p, e.size)) {
                return;
            }
        }
    }
}

size_t Pk2Archive::FileCount() const { return m_fileCount; }

size_t Pk2Archive::FolderCount() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_folders.size();
}

}  // namespace se::client
