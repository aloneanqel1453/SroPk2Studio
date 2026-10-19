#include "pk2_archive.hpp"

#include "i18n.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>

namespace pk2 {
namespace {

// ---- little-endian scalar helpers -----------------------------------------
std::int64_t load_i64(const std::uint8_t* p) noexcept {
    std::int64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
std::uint32_t load_u32(const std::uint8_t* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
void store_i64(std::uint8_t* p, std::int64_t v) noexcept { std::memcpy(p, &v, sizeof(v)); }
void store_u32(std::uint8_t* p, std::uint32_t v) noexcept { std::memcpy(p, &v, sizeof(v)); }

HANDLE H(void* p) noexcept { return static_cast<HANDLE>(p); }

// Errors carry UTF-8 text in the current UI language.
[[noreturn]] void fail(std::string_view key) { throw Pk2Error(wide_to_utf8(L(key))); }
[[noreturn]] void fail_last(std::string_view key) {
    const unsigned long err = ::GetLastError();
    throw Pk2Error(wide_to_utf8(Lf("ar_winerr", L(key), err)));
}
[[noreturn]] void fail_path(std::string_view key, const std::wstring& path) {
    const unsigned long err = ::GetLastError();
    throw Pk2Error(wide_to_utf8(Lf("ar_winerr", Lf(key, path), err)));
}

// Silkroad final-blowfish-key derivation (from File2Pk2 BlowfishHelpers /
// SRO.PK2API): bf_key[x] = ascii_key[x] XOR base_key[x], where base_key is the
// 10-byte Silkroad constant zero-padded to 56. Folder2Pk2 omitted this step;
// official Joymax pk2 files need it.
std::vector<std::uint8_t> derive_blowfish_key(std::string_view ascii) {
    static const std::uint8_t base_key[10] = {0x03, 0xF8, 0xE4, 0x44, 0x88,
                                              0x99, 0x3F, 0x64, 0xFE, 0x35};
    std::size_t len = std::min<std::size_t>(ascii.size(), 56);
    std::vector<std::uint8_t> bf(len);
    for (std::size_t i = 0; i < len; ++i) {
        std::uint8_t b = (i < sizeof(base_key)) ? base_key[i] : 0;
        bf[i] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(ascii[i]) ^ b);
    }
    return bf;
}

std::wstring path_stem(const std::wstring& full) {
    std::size_t slash = full.find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
    std::size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos)
        name = name.substr(0, dot);
    return name;
}

void create_directories(const std::wstring& dir) {
    if (dir.empty())
        return;
    std::wstring cur;
    for (std::size_t i = 0; i < dir.size(); ++i) {
        wchar_t c = dir[i];
        cur += c;
        if (c == L'\\' || c == L'/') {
            if (cur.size() > 3) // skip drive root like "C:\"
                ::CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    ::CreateDirectoryW(dir.c_str(), nullptr);
}

// Unify separators to '/', collapse duplicates, trim leading/trailing — keeps case.
std::wstring unify_trim(std::wstring_view path) {
    std::wstring out;
    bool prev_slash = true;
    for (wchar_t c : path) {
        if (c == L'\\' || c == L'/') {
            if (!prev_slash)
                out += L'/';
            prev_slash = true;
        } else {
            out += c;
            prev_slash = false;
        }
    }
    while (!out.empty() && out.back() == L'/')
        out.pop_back();
    return out;
}

std::vector<std::wstring> split_parts(std::wstring_view norm) {
    std::vector<std::wstring> parts;
    std::size_t start = 0;
    while (start <= norm.size()) {
        std::size_t pos = norm.find(L'/', start);
        if (pos == std::wstring_view::npos) {
            if (start < norm.size())
                parts.emplace_back(norm.substr(start));
            break;
        }
        if (pos > start)
            parts.emplace_back(norm.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

Pk2Archive::Entry parse_entry(const std::uint8_t* p) {
    Pk2Archive::Entry e;
    e.type = static_cast<EntryType>(p[0]);
    std::memcpy(e.name.data(), p + 1, kNameBytes);
    e.ctime = load_i64(p + 90);
    e.mtime = load_i64(p + 98);
    e.offset = load_i64(p + 106);
    e.size = load_u32(p + 114);
    e.next_block = load_i64(p + 118);
    e.pad[0] = p[126];
    e.pad[1] = p[127];
    return e;
}
void serialize_entry(std::uint8_t* p, const Pk2Archive::Entry& e) {
    std::memset(p, 0, kEntrySize);
    p[0] = static_cast<std::uint8_t>(e.type);
    std::memcpy(p + 1, e.name.data(), kNameBytes);
    store_i64(p + 90, e.ctime);
    store_i64(p + 98, e.mtime);
    store_i64(p + 106, e.offset);
    store_u32(p + 114, e.size);
    store_i64(p + 118, e.next_block);
    p[126] = e.pad[0];
    p[127] = e.pad[1];
}

// Empties an entry; the chain link stored in a block's last entry stays.
void clear_entry(Pk2Archive::Entry& e) {
    const std::int64_t next = e.next_block;
    e = Pk2Archive::Entry{};
    e.next_block = next;
}

bool is_dot_entry(const Pk2Archive::Entry& e, const wchar_t* dots) {
    return e.type == EntryType::Folder && decode_pk2_name(e.name) == dots;
}

// ---- Win32 handles and positioned I/O --------------------------------------
struct HandleCloser {
    void operator()(void* h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE)
            ::CloseHandle(static_cast<HANDLE>(h));
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

UniqueHandle open_for_read(const std::wstring& path, DWORD flags = FILE_ATTRIBUTE_NORMAL) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        fail_path("ar_open_file", path);
    return UniqueHandle(h);
}

// Reads exactly `len` bytes at `pos` (thread-safe on a handle of its own per thread).
void read_at(HANDLE h, std::uint64_t pos, std::uint8_t* out, std::size_t len) {
    std::size_t done = 0;
    while (done < len) {
        OVERLAPPED ov{};
        const std::uint64_t at = pos + done;
        ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(len - done, 0x4000'0000));
        if (!::ReadFile(h, out + done, want, &got, &ov))
            fail_last("ar_io_read");
        if (got == 0)
            fail("ar_eof");
        done += got;
    }
}

void write_all(HANDLE h, const std::uint8_t* data, std::size_t len, std::string_view what) {
    std::size_t done = 0;
    while (done < len) {
        DWORD put = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(len - done, 0x4000'0000));
        if (!::WriteFile(h, data + done, want, &put, nullptr) || put == 0)
            fail_last(what);
        done += put;
    }
}

std::vector<std::uint8_t> read_disk_file(const std::wstring& path) {
    UniqueHandle h = open_for_read(path, FILE_FLAG_SEQUENTIAL_SCAN);
    LARGE_INTEGER li{};
    if (!::GetFileSizeEx(h.get(), &li))
        fail_path("ar_read_disk", path);
    if (static_cast<std::uint64_t>(li.QuadPart) > 0xFFFF'FFFFull)
        throw Pk2Error(wide_to_utf8(Lf("ar_too_big", path)));
    std::vector<std::uint8_t> data(static_cast<std::size_t>(li.QuadPart));
    if (!data.empty())
        read_at(h.get(), 0, data.data(), data.size());
    return data;
}

void write_whole_file(const std::wstring& dest, const std::uint8_t* data, std::size_t len) {
    UniqueHandle h(::CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h.get() == INVALID_HANDLE_VALUE)
        fail_path("ar_out_create", dest);
    write_all(h.get(), data, len, "ar_out_write");
}

// ---- parallel helpers ---------------------------------------------------------
unsigned worker_count(std::size_t jobs, unsigned cap) {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return static_cast<unsigned>(std::max<std::size_t>(1, std::min<std::size_t>({jobs, hw, cap})));
}

// fn(begin, end, worker) over [0, n) split into one contiguous range per worker.
template <class Fn>
void parallel_for(std::size_t n, unsigned cap, Fn&& fn) {
    if (n == 0)
        return;
    const unsigned t = worker_count(n, cap);
    if (t == 1) {
        fn(std::size_t{0}, n, 0u);
        return;
    }
    std::exception_ptr err;
    std::mutex m;
    {
        std::vector<std::jthread> pool;
        const std::size_t step = (n + t - 1) / t;
        for (unsigned w = 0; w < t; ++w) {
            const std::size_t b = w * step;
            const std::size_t e = std::min(n, b + step);
            if (b >= e)
                break;
            pool.emplace_back([&, b, e, w] {
                try {
                    fn(b, e, w);
                } catch (...) {
                    std::lock_guard lk(m);
                    if (!err)
                        err = std::current_exception();
                }
            });
        }
    }
    if (err)
        std::rethrow_exception(err);
}

// produce(i, worker) runs on worker threads, at most `window` items ahead of the consumer;
// consume(i, value) runs on the calling thread in index order. Returns false when cancelled.
template <class T, class Produce, class Consume>
bool ordered_pipeline(std::size_t n, unsigned threads, std::size_t window, Produce&& produce,
                      Consume&& consume, const CancelFn& cancel) {
    if (n == 0)
        return true;
    std::vector<std::optional<T>> slot(n);
    std::mutex m;
    std::condition_variable cv;
    std::size_t next = 0, consumed = 0;
    bool stop = false;
    std::exception_ptr err;

    auto worker = [&](unsigned w) {
        for (;;) {
            std::size_t i = 0;
            {
                std::unique_lock lk(m);
                cv.wait(lk, [&] { return stop || next >= n || next < consumed + window; });
                if (stop || next >= n)
                    return;
                i = next++;
            }
            try {
                T v = produce(i, w);
                std::lock_guard lk(m);
                slot[i].emplace(std::move(v));
            } catch (...) {
                std::lock_guard lk(m);
                if (!err)
                    err = std::current_exception();
                stop = true;
            }
            cv.notify_all();
        }
    };

    bool cancelled = false;
    std::vector<std::jthread> pool;
    auto halt = [&] {
        {
            std::lock_guard lk(m);
            stop = true;
        }
        cv.notify_all();
        pool.clear(); // joins
    };
    for (unsigned w = 0; w < threads; ++w)
        pool.emplace_back(worker, w);
    try {
        for (std::size_t i = 0; i < n; ++i) {
            T v;
            {
                std::unique_lock lk(m);
                cv.wait(lk, [&] { return stop || slot[i].has_value(); });
                if (!slot[i])
                    break; // a worker failed
                v = std::move(*slot[i]);
                slot[i].reset();
            }
            consume(i, std::move(v));
            if (cancel && cancel()) {
                cancelled = true;
                break;
            }
            {
                std::lock_guard lk(m);
                consumed = i + 1;
            }
            cv.notify_all();
        }
    } catch (...) {
        halt();
        throw;
    }
    halt();
    if (err)
        std::rethrow_exception(err);
    return !cancelled;
}

} // namespace

// Allow helpers above to name the nested Entry type.
using Entry = Pk2Archive::Entry;

Pk2Archive::~Pk2Archive() { close(); }

std::int64_t Pk2Archive::now_filetime() noexcept {
    FILETIME ft;
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<std::int64_t>(u.QuadPart);
}

// ---- low-level I/O ---------------------------------------------------------
void Pk2Archive::seek(std::uint64_t pos) const {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(pos);
    if (!::SetFilePointerEx(H(file_), li, nullptr, FILE_BEGIN))
        fail_last("ar_io_seek");
}
void Pk2Archive::read_raw(void* buf, std::size_t len) const {
    auto* out = static_cast<std::uint8_t*>(buf);
    std::size_t total = 0;
    while (total < len) {
        DWORD got = 0;
        DWORD want = static_cast<DWORD>(std::min<std::size_t>(len - total, 0x4000'0000));
        if (!::ReadFile(H(file_), out + total, want, &got, nullptr))
            fail_last("ar_io_read");
        if (got == 0)
            fail("ar_eof");
        total += got;
    }
}
void Pk2Archive::write_raw(const void* buf, std::size_t len) const {
    write_all(H(file_), static_cast<const std::uint8_t*>(buf), len, "ar_io_write");
}
void Pk2Archive::write_at(std::uint64_t pos, std::span<const std::uint8_t> bytes) const {
    if (bytes.empty())
        return;
    seek(pos);
    write_raw(bytes.data(), bytes.size());
}
std::uint64_t Pk2Archive::size_on_disk() const {
    LARGE_INTEGER li;
    if (!::GetFileSizeEx(H(file_), &li))
        fail_last("ar_io_size");
    return static_cast<std::uint64_t>(li.QuadPart);
}
std::uint64_t Pk2Archive::file_size() const { return file_ ? size_on_disk() : 0; }

// ---- block load / store ----------------------------------------------------
Pk2Archive::Block Pk2Archive::decode_block(std::span<const std::uint8_t> encoded) const {
    std::vector<std::uint8_t> dec = bf_.decode(encoded);
    Block b;
    for (std::size_t i = 0; i < kEntriesPerBlock; ++i)
        b.entries[i] = parse_entry(dec.data() + i * kEntrySize);
    return b;
}
void Pk2Archive::encode_block(const Block& block, std::uint8_t* out) const {
    std::array<std::uint8_t, kBlockSize> plain{};
    for (std::size_t i = 0; i < kEntriesPerBlock; ++i)
        serialize_entry(plain.data() + i * kEntrySize, block.entries[i]);
    std::vector<std::uint8_t> enc = bf_.encode(plain);
    std::memcpy(out, enc.data(), kBlockSize);
}
Pk2Archive::Block Pk2Archive::load_block(std::uint64_t offset) const {
    seek(offset);
    std::array<std::uint8_t, kBlockSize> enc{};
    read_raw(enc.data(), enc.size());
    return decode_block(enc);
}
void Pk2Archive::store_block(std::uint64_t offset, const Block& block) const {
    std::array<std::uint8_t, kBlockSize> enc{};
    encode_block(block, enc.data());
    seek(offset);
    write_raw(enc.data(), enc.size());
}

// ---- open / create ---------------------------------------------------------
void Pk2Archive::create_base_stream() const {
    // Header (256 bytes, plaintext).
    std::array<std::uint8_t, kHeaderSize> header{};
    const char sig[] = "JoyMax File Manager!\n";
    std::memcpy(header.data(), sig, sizeof(sig) - 1); // no trailing NUL beyond content
    header[30] = 2; header[31] = 0; header[32] = 0; header[33] = 1; // version 2.0.0.1
    header[34] = 1;                                                  // encrypted
    const char tag[] = "Joymax Pack File";
    std::vector<std::uint8_t> cs = bf_.encode(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(tag), sizeof(tag) - 1));
    std::memcpy(header.data() + 35, cs.data(), 3); // first 3 bytes of checksum
    seek(0);
    write_raw(header.data(), header.size());

    // Root block at offset 256 with a "." self-pointer. Nothing else: no padding.
    Block block;
    Entry& e0 = block.entries[0];
    e0.type = EntryType::Folder;
    e0.name = encode_pk2_name(L".");
    e0.ctime = e0.mtime = now_filetime();
    e0.offset = static_cast<std::int64_t>(kHeaderSize);
    store_block(kHeaderSize, block);
}

void Pk2Archive::create(const std::wstring& path, std::string_view key) {
    close();
    key_ = std::string(key);
    bf_.initialize(derive_blowfish_key(key_));
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        fail_path("ar_create", path);
    file_ = h;
    path_ = path;
    create_base_stream();
    backed_up_ = true; // nothing to back up for a fresh file
    read_tree();
}

void Pk2Archive::open(const std::wstring& path, std::string_view key) {
    close();
    key_ = std::string(key);
    bf_.initialize(derive_blowfish_key(key_));
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        fail_path("ar_open_file", path);
    file_ = h;
    path_ = path;
    backed_up_ = false;

    // Validate key via header checksum.
    std::array<std::uint8_t, kHeaderSize> header{};
    if (size_on_disk() < kHeaderSize + kBlockSize) {
        close();
        throw Pk2AuthError(wide_to_utf8(L("wrong_key")));
    }
    seek(0);
    read_raw(header.data(), header.size());
    const char tag[] = "Joymax Pack File";
    std::vector<std::uint8_t> cs = bf_.encode(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(tag), sizeof(tag) - 1));
    std::array<std::uint8_t, 16> comparer{};
    std::memcpy(comparer.data(), cs.data(), 3);
    if (std::memcmp(header.data() + 35, comparer.data(), 16) != 0) {
        close();
        throw Pk2AuthError(wide_to_utf8(L("wrong_key")));
    }

    read_tree();
}

void Pk2Archive::close() {
    if (file_) {
        ::CloseHandle(H(file_));
        file_ = nullptr;
    }
    root_ = Pk2Folder{};
    regions_.clear();
    holes_.clear();
    holes_by_size_.clear();
    end_ = 0;
    folder_blocks_.clear();
    folder_paths_.clear();
    folder_slots_.clear();
    chains_.clear();
    files_.clear();
}

void Pk2Archive::reload() {
    if (!file_)
        return;
    read_tree();
}

// ---- tree read -------------------------------------------------------------
void Pk2Archive::read_tree() {
    root_ = Pk2Folder{};
    root_.block_offset = kHeaderSize;
    regions_.clear();
    holes_.clear();
    holes_by_size_.clear();
    folder_blocks_.clear();
    folder_paths_.clear();
    folder_slots_.clear();
    chains_.clear();
    files_.clear();
    end_ = size_on_disk();

    claim(0, kHeaderSize);
    folder_blocks_[L""] = kHeaderSize;
    folder_paths_[kHeaderSize] = L"";
    read_tree_block(kHeaderSize, root_);
    rebuild_holes();
}

void Pk2Archive::read_tree_block(std::uint64_t first, Pk2Folder& parent) {
    // Walk the block chain iteratively (chains can be very long in big archives); only recurse
    // for subfolders, whose depth is bounded by directory nesting. A block seen before ends the
    // chain, so a damaged archive cannot loop forever.
    std::vector<ChainBlock>& chain = chains_[first]; // node-based map: stays valid while it grows
    std::uint64_t cur = first;
    while (cur != 0 && cur + kBlockSize <= end_ && !regions_.contains(cur)) {
        claim(cur, kBlockSize);
        const Block block = load_block(cur);
        ChainBlock cb{cur, 0};
        for (std::uint32_t i = 0; i < kEntriesPerBlock; ++i) {
            const Entry& e = block.entries[i];
            if (e.type == EntryType::Empty)
                continue;
            ++cb.used;
            if (e.type == EntryType::Folder) {
                std::wstring name = decode_pk2_name(e.name);
                if (name == L"." || name == L"..")
                    continue;
                const auto child_block = static_cast<std::uint64_t>(e.offset);
                if (e.offset <= 0 || child_block + kBlockSize > end_ || regions_.contains(child_block))
                    continue; // points nowhere or back into the tree
                Pk2Folder child;
                child.name = name;
                child.path = parent.path.empty() ? to_lower(name) : parent.path + L"/" + to_lower(name);
                child.block_offset = child_block;
                folder_blocks_[child.path] = child_block;
                folder_paths_[child_block] = child.path;
                folder_slots_[child.path] = Slot{cur, i};
                read_tree_block(child_block, child);
                parent.folders.push_back(std::move(child));
            } else if (e.type == EntryType::File) {
                Pk2File f;
                f.name = decode_pk2_name(e.name);
                f.path = parent.path.empty() ? to_lower(f.name) : parent.path + L"/" + to_lower(f.name);
                f.data_offset = static_cast<std::uint64_t>(e.offset);
                f.size = e.size;
                f.ctime = e.ctime;
                f.mtime = e.mtime;
                if (f.size)
                    claim(f.data_offset, f.size);
                files_[f.path] = FileRef{f.data_offset, f.size, first, Slot{cur, i}};
                parent.files.push_back(std::move(f));
            }
        }
        chain.push_back(cb);
        cur = static_cast<std::uint64_t>(block.next());
    }
}

// ---- lookups ---------------------------------------------------------------
std::wstring Pk2Archive::normalize(std::wstring_view path) {
    std::wstring s(path);
    for (wchar_t& c : s)
        if (c == L'\\')
            c = L'/';
    s = to_lower(s);
    // collapse duplicate and trim leading/trailing slashes
    std::wstring out;
    bool prev_slash = true; // trims leading
    for (wchar_t c : s) {
        if (c == L'/') {
            if (!prev_slash)
                out += c;
            prev_slash = true;
        } else {
            out += c;
            prev_slash = false;
        }
    }
    while (!out.empty() && out.back() == L'/')
        out.pop_back();
    return out;
}

static const Pk2Folder* find_folder_rec(const Pk2Folder& f, std::wstring_view norm) {
    if (f.path == norm)
        return &f;
    for (const Pk2Folder& c : f.folders)
        if (norm == c.path || norm.substr(0, c.path.size() + 1) == c.path + L"/")
            if (const Pk2Folder* r = find_folder_rec(c, norm))
                return r;
    return nullptr;
}

const Pk2Folder* Pk2Archive::find_folder(std::wstring_view path) const {
    std::wstring norm = normalize(path);
    if (norm.empty())
        return &root_;
    return find_folder_rec(root_, norm);
}

const Pk2File* Pk2Archive::find_file(std::wstring_view path) const {
    std::wstring norm = normalize(path);
    std::size_t slash = norm.find_last_of(L'/');
    std::wstring dir = (slash == std::wstring::npos) ? std::wstring() : norm.substr(0, slash);
    const Pk2Folder* folder = find_folder(dir);
    if (!folder)
        return nullptr;
    for (const Pk2File& f : folder->files)
        if (f.path == norm)
            return &f;
    return nullptr;
}

std::vector<std::uint8_t> Pk2Archive::read_file(const Pk2File& file) const {
    std::vector<std::uint8_t> buf(file.size);
    if (file.size) {
        seek(file.data_offset);
        read_raw(buf.data(), buf.size());
    }
    return buf;
}

// ---- space management --------------------------------------------------------
// regions_ holds every live byte range (header, index blocks, file data), holes_ every free one
// below end_. A change frees what it no longer needs (merged with neighbouring holes), takes the
// smallest hole that fits, and only grows the file when no hole fits.
void Pk2Archive::claim(std::uint64_t offset, std::uint64_t size) {
    auto [it, inserted] = regions_.try_emplace(offset, Region{size, 1});
    if (!inserted) {
        it->second.size = std::max(it->second.size, size);
        ++it->second.refs;
    }
}

void Pk2Archive::release(std::uint64_t offset, std::uint64_t size) {
    auto it = regions_.find(offset);
    if (it == regions_.end() || size == 0)
        return;
    if (--it->second.refs > 0)
        return;
    std::uint64_t s = offset, e = offset + it->second.size;
    regions_.erase(it);
    // Never free bytes another live region still covers.
    auto next = regions_.lower_bound(offset);
    if (next != regions_.begin()) {
        auto prev = std::prev(next);
        s = std::max(s, prev->first + prev->second.size);
    }
    if (next != regions_.end())
        e = std::min(e, next->first);
    if (s < e)
        add_hole(s, e - s);
}

void Pk2Archive::add_hole(std::uint64_t offset, std::uint64_t size) {
    if (size == 0)
        return;
    std::uint64_t s = offset, e = offset + size;
    auto next = holes_.lower_bound(s);
    if (next != holes_.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second >= s) {
            s = prev->first;
            e = std::max(e, prev->first + prev->second);
            take_hole(prev);
        }
    }
    next = holes_.lower_bound(s);
    while (next != holes_.end() && next->first <= e) {
        e = std::max(e, next->first + next->second);
        auto victim = next++;
        take_hole(victim);
    }
    holes_.emplace(s, e - s);
    holes_by_size_.emplace(e - s, s);
}

void Pk2Archive::take_hole(std::map<std::uint64_t, std::uint64_t>::iterator it) {
    holes_by_size_.erase({it->second, it->first});
    holes_.erase(it);
}

void Pk2Archive::rebuild_holes() {
    holes_.clear();
    holes_by_size_.clear();
    std::uint64_t cursor = 0;
    for (const auto& [off, r] : regions_) {
        if (off > cursor)
            add_hole(cursor, off - cursor);
        cursor = std::max(cursor, off + r.size);
    }
    if (end_ > cursor)
        add_hole(cursor, end_ - cursor);
}

// Free space at the end of the file is given back to the disk.
void Pk2Archive::trim_tail() {
    if (holes_.empty())
        return;
    auto last = std::prev(holes_.end());
    if (last->first + last->second < end_)
        return;
    const std::uint64_t new_end = last->first;
    take_hole(last);
    seek(new_end);
    if (!::SetEndOfFile(H(file_)))
        fail_last("ar_io_write");
    end_ = new_end;
}

std::uint64_t Pk2Archive::allocate(std::uint64_t size) {
    if (size == 0)
        return end_;
    auto fit = holes_by_size_.lower_bound({size, 0});
    if (fit != holes_by_size_.end()) {
        const std::uint64_t off = fit->second, hole = fit->first;
        take_hole(holes_.find(off));
        if (hole > size)
            add_hole(off + size, hole - size);
        claim(off, size);
        return off;
    }
    // No hole fits: start in the free space at the end of the file (if any) and grow it.
    std::uint64_t off = end_;
    if (!holes_.empty()) {
        auto last = std::prev(holes_.end());
        if (last->first + last->second == end_) {
            off = last->first;
            take_hole(last);
        }
    }
    end_ = off + size;
    claim(off, size);
    return off;
}

// ---- index blocks --------------------------------------------------------------
// A free entry in the folder's chain; the chain gets one more block when all are full.
Pk2Archive::Slot Pk2Archive::free_slot(std::uint64_t first_block, Block& block) {
    std::vector<ChainBlock>& chain = chains_[first_block];
    for (ChainBlock& cb : chain) {
        if (cb.used >= kEntriesPerBlock)
            continue;
        block = load_block(cb.offset);
        for (std::uint32_t i = 0; i < kEntriesPerBlock; ++i)
            if (block.entries[i].type == EntryType::Empty) {
                ++cb.used;
                return Slot{cb.offset, i};
            }
        cb.used = kEntriesPerBlock; // count was stale
    }
    const std::uint64_t last = chain.empty() ? first_block : chain.back().offset;
    const std::uint64_t fresh_off = allocate(kBlockSize);
    Block fresh;
    store_block(fresh_off, fresh);
    Block tail = load_block(last);
    tail.set_next(static_cast<std::int64_t>(fresh_off));
    store_block(last, tail);
    chain.push_back({fresh_off, 1});
    block = fresh;
    return Slot{fresh_off, 0};
}

// Empties an entry; a chain block (not the first) left with no entries is unlinked and freed.
void Pk2Archive::clear_slot(std::uint64_t first_block, Slot slot) {
    Block block = load_block(slot.block);
    clear_entry(block.entries[slot.index]);
    store_block(slot.block, block);
    std::vector<ChainBlock>& chain = chains_[first_block];
    auto it = std::find_if(chain.begin(), chain.end(), [&](const ChainBlock& c) { return c.offset == slot.block; });
    if (it == chain.end())
        return;
    if (it->used > 0)
        --it->used;
    if (it->used > 0 || it == chain.begin())
        return;
    for (const Entry& e : block.entries)
        if (e.type != EntryType::Empty)
            return;
    const std::uint64_t prev_off = std::prev(it)->offset;
    Block prev = load_block(prev_off);
    prev.set_next(block.next());
    store_block(prev_off, prev);
    chain.erase(it);
    release(slot.block, kBlockSize);
}

Pk2Archive::Slot Pk2Archive::find_entry(std::uint64_t first_block, EntryType type, std::uint64_t offset) const {
    auto it = chains_.find(first_block);
    if (it == chains_.end())
        return {};
    for (const ChainBlock& cb : it->second) {
        const Block block = load_block(cb.offset);
        for (std::uint32_t i = 0; i < kEntriesPerBlock; ++i) {
            const Entry& e = block.entries[i];
            if (e.type == type && static_cast<std::uint64_t>(e.offset) == offset && !is_dot_entry(e, L".") &&
                !is_dot_entry(e, L".."))
                return Slot{cb.offset, i};
        }
    }
    return {};
}

// First block of the folder at disp_path (original case), creating missing folders on the way.
std::uint64_t Pk2Archive::ensure_folder(const std::wstring& disp_path) {
    const std::vector<std::wstring> parts = split_parts(unify_trim(disp_path));
    std::uint64_t parent = kHeaderSize;
    std::wstring path;
    for (const std::wstring& part : parts) {
        const std::wstring key = path.empty() ? to_lower(part) : path + L"/" + to_lower(part);
        if (auto it = folder_blocks_.find(key); it != folder_blocks_.end()) {
            parent = it->second;
            path = key;
            continue;
        }
        if (files_.contains(key))
            throw Pk2Error(wide_to_utf8(Lf("ar_name_taken", key)));
        // The new folder's block first, then its entry in the parent: an interrupted write never
        // leaves an entry pointing at garbage.
        const std::uint64_t child = allocate(kBlockSize);
        Block nb;
        nb.entries[0].type = EntryType::Folder;
        nb.entries[0].name = encode_pk2_name(L".");
        nb.entries[0].ctime = nb.entries[0].mtime = now_filetime();
        nb.entries[0].offset = static_cast<std::int64_t>(child);
        nb.entries[1].type = EntryType::Folder;
        nb.entries[1].name = encode_pk2_name(L"..");
        nb.entries[1].ctime = nb.entries[1].mtime = now_filetime();
        nb.entries[1].offset = static_cast<std::int64_t>(parent);
        store_block(child, nb);

        Block pblock;
        const Slot s = free_slot(parent, pblock);
        Entry& e = pblock.entries[s.index];
        e.type = EntryType::Folder;
        e.name = encode_pk2_name(part);
        e.ctime = e.mtime = now_filetime();
        e.offset = static_cast<std::int64_t>(child);
        e.size = 0;
        store_block(s.block, pblock);

        chains_[child] = {ChainBlock{child, 2}};
        folder_blocks_[key] = child;
        folder_paths_[child] = key;
        folder_slots_[key] = s;
        parent = child;
        path = key;
    }
    return parent;
}

// ---- backup ----------------------------------------------------------------
void Pk2Archive::ensure_backup() {
    if (backed_up_)
        return;
    std::wstring bak = path_ + L".bak";
    // Flush our handle's writes to disk before copying.
    ::FlushFileBuffers(H(file_));
    if (!::CopyFileW(path_.c_str(), bak.c_str(), FALSE)) {
        // Non-fatal if the source is brand new/empty, but surface real failures.
        if (::GetLastError() != ERROR_FILE_NOT_FOUND)
            fail_last("ar_backup");
    }
    backed_up_ = true;
}

// ---- folder / file changes -----------------------------------------------------
bool Pk2Archive::add_folder(std::wstring_view inner_path) {
    ensure_backup();
    const std::wstring normc = unify_trim(inner_path); // original case
    const std::wstring norm = to_lower(normc);          // lookup key
    if (norm.empty() || folder_blocks_.contains(norm) || files_.contains(norm))
        return false;
    (void)ensure_folder(normc);
    return true;
}

bool Pk2Archive::add_file(std::wstring_view inner_path, const std::vector<std::uint8_t>& bytes) {
    ensure_backup();
    const std::wstring normc = unify_trim(inner_path); // original case
    const std::wstring norm = to_lower(normc);          // lookup key
    if (norm.empty() || folder_blocks_.contains(norm))
        return false;
    if (bytes.size() > 0xFFFF'FFFFull)
        throw Pk2Error(wide_to_utf8(Lf("ar_too_big", normc)));
    const auto size = static_cast<std::uint32_t>(bytes.size());

    if (auto existing = files_.find(norm); existing != files_.end()) {
        FileRef ref = existing->second;
        Block block = load_block(ref.slot.block);
        const Entry& cur = block.entries[ref.slot.index];
        if (cur.type != EntryType::File || static_cast<std::uint64_t>(cur.offset) != ref.offset) {
            ref.slot = find_entry(ref.parent_block, EntryType::File, ref.offset);
            if (!ref.slot.block)
                return false;
            block = load_block(ref.slot.block);
        }
        const auto region = regions_.find(ref.offset);
        const bool own = ref.size > 0 && region != regions_.end() && region->second.refs == 1;
        std::uint64_t off = ref.offset;
        if (size == 0) {
            release(ref.offset, ref.size);
        } else if (own && size <= ref.size) {
            // Fits where it is: overwrite, give the rest back.
            region->second.size = size;
            add_hole(ref.offset + size, ref.size - size);
        } else {
            // Grows: into the free space right after it if there is enough (or the file's end),
            // otherwise into the smallest hole that fits, freeing the old place.
            const std::uint64_t old_end = ref.offset + ref.size;
            const std::uint64_t need = size - (own ? ref.size : 0);
            auto after = own ? holes_.find(old_end) : holes_.end();
            if (own && after != holes_.end() && after->second >= need) {
                const std::uint64_t hole = after->second;
                take_hole(after);
                if (hole > need)
                    add_hole(old_end + need, hole - need);
                region->second.size = size;
            } else if (own && (old_end == end_ || (after != holes_.end() && after->first + after->second == end_))) {
                if (after != holes_.end())
                    take_hole(after);
                end_ = ref.offset + size;
                region->second.size = size;
            } else {
                release(ref.offset, ref.size);
                off = allocate(size);
            }
        }
        if (size)
            write_at(off, bytes);
        Entry& e = block.entries[ref.slot.index];
        e.mtime = now_filetime();
        e.size = size;
        e.offset = static_cast<std::int64_t>(off);
        store_block(ref.slot.block, block);
        files_[norm] = FileRef{off, size, ref.parent_block, ref.slot};
        trim_tail();
        return true;
    }

    // New file: its folder (created when missing), its data, then its entry.
    const std::size_t slash = normc.find_last_of(L'/');
    const std::wstring dir_disp = (slash == std::wstring::npos) ? std::wstring() : normc.substr(0, slash);
    const std::wstring base = (slash == std::wstring::npos) ? normc : normc.substr(slash + 1);
    const std::uint64_t parent = ensure_folder(dir_disp);
    const std::uint64_t off = allocate(size);
    if (size)
        write_at(off, bytes);
    Block block;
    const Slot s = free_slot(parent, block);
    Entry& e = block.entries[s.index];
    e.type = EntryType::File;
    e.name = encode_pk2_name(base);
    e.ctime = e.mtime = now_filetime();
    e.size = size;
    e.offset = static_cast<std::int64_t>(off);
    store_block(s.block, block);
    files_[norm] = FileRef{off, size, parent, s};
    return true;
}

bool Pk2Archive::remove_file(std::wstring_view inner_path) {
    ensure_backup();
    const std::wstring norm = normalize(inner_path);
    auto it = files_.find(norm);
    if (it == files_.end())
        return false;
    FileRef ref = it->second;
    const Block block = load_block(ref.slot.block);
    const Entry& cur = block.entries[ref.slot.index];
    if (cur.type != EntryType::File || static_cast<std::uint64_t>(cur.offset) != ref.offset) {
        ref.slot = find_entry(ref.parent_block, EntryType::File, ref.offset);
        if (!ref.slot.block)
            return false;
    }
    clear_slot(ref.parent_block, ref.slot);
    release(ref.offset, ref.size);
    files_.erase(it);
    trim_tail();
    return true;
}

bool Pk2Archive::remove_folder(std::wstring_view inner_path) {
    ensure_backup();
    const std::wstring norm = normalize(inner_path);
    if (norm.empty())
        fail("ar_root_remove");
    auto it = folder_blocks_.find(norm);
    if (it == folder_blocks_.end())
        return false;
    const std::uint64_t folder_block = it->second;
    const std::size_t slash = norm.find_last_of(L'/');
    const std::wstring parent_dir = (slash == std::wstring::npos) ? std::wstring() : norm.substr(0, slash);
    auto pit = folder_blocks_.find(parent_dir);
    if (pit == folder_blocks_.end())
        return false;
    const std::uint64_t parent_block = pit->second;

    Slot slot = folder_slots_[norm];
    bool valid = false;
    if (slot.block) {
        const Block b = load_block(slot.block);
        const Entry& e = b.entries[slot.index];
        valid = e.type == EntryType::Folder && static_cast<std::uint64_t>(e.offset) == folder_block;
    }
    if (!valid)
        slot = find_entry(parent_block, EntryType::Folder, folder_block);
    if (!slot.block)
        return false;
    clear_slot(parent_block, slot);

    // Everything below it becomes free space: file data and every block of every chain.
    const std::wstring prefix = norm + L"/";
    for (auto f = files_.lower_bound(prefix); f != files_.end() && f->first.starts_with(prefix);) {
        release(f->second.offset, f->second.size);
        f = files_.erase(f);
    }
    auto drop_folder = [&](const std::wstring& path, std::uint64_t first) {
        if (auto c = chains_.find(first); c != chains_.end()) {
            for (const ChainBlock& cb : c->second)
                release(cb.offset, kBlockSize);
            chains_.erase(c);
        }
        folder_paths_.erase(first);
        folder_slots_.erase(path);
    };
    for (auto g = folder_blocks_.lower_bound(prefix); g != folder_blocks_.end() && g->first.starts_with(prefix);) {
        drop_folder(g->first, g->second);
        g = folder_blocks_.erase(g);
    }
    drop_folder(norm, folder_block);
    folder_blocks_.erase(norm);
    trim_tail();
    return true;
}

// ---- import (disk -> pk2) --------------------------------------------------------
std::uint64_t Pk2Archive::add_disk_files(std::span<const DiskFile> files, const ProgressFn& progress,
                                         const CancelFn& cancel) {
    if (files.empty())
        return 0;
    ensure_backup();
    const unsigned threads = worker_count(files.size(), 8);
    std::uint64_t done = 0;
    ordered_pipeline<std::vector<std::uint8_t>>(
        files.size(), threads, std::size_t{threads} * 4,
        [&](std::size_t i, unsigned) { return read_disk_file(files[i].disk); },
        [&](std::size_t i, std::vector<std::uint8_t>&& data) {
            add_file(files[i].inner, data);
            ++done;
            if (progress)
                progress(Progress{done, files.size(), files[i].inner});
        },
        cancel);
    return done;
}

std::uint64_t Pk2Archive::import_folder(const std::wstring& src_dir, std::wstring_view inner_dest,
                                        const ProgressFn& progress, const CancelFn& cancel) {
    const std::wstring dest = unify_trim(inner_dest); // keep case
    if (!dest.empty())
        add_folder(dest);

    // Every file below src_dir; folders are created too, so empty ones are kept.
    std::vector<DiskFile> files;
    struct Item { std::wstring disk; std::wstring inner; };
    std::vector<Item> stack{{src_dir, dest}};
    while (!stack.empty()) {
        Item cur = std::move(stack.back());
        stack.pop_back();
        if (!cur.inner.empty())
            add_folder(cur.inner);
        WIN32_FIND_DATAW fd;
        HANDLE h = ::FindFirstFileW((cur.disk + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..")
                continue;
            std::wstring inner = cur.inner.empty() ? name : cur.inner + L"/" + name; // keep case
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                stack.push_back({cur.disk + L"\\" + name, std::move(inner)});
            else
                files.push_back({cur.disk + L"\\" + name, std::move(inner)});
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    return add_disk_files(files, progress, cancel);
}

// ---- extract ---------------------------------------------------------------
void Pk2Archive::extract_file(const Pk2File& file, const std::wstring& dest_file) const {
    std::size_t slash = dest_file.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        create_directories(dest_file.substr(0, slash));
    std::vector<std::uint8_t> data = read_file(file);
    write_whole_file(dest_file, data.data(), data.size());
}

void Pk2Archive::extract_folder(const Pk2Folder& folder, const std::wstring& dest_dir,
                                const ProgressFn& progress, const CancelFn& cancel) const {
    // Collect every file job and the set of directories to create (DFS).
    struct Job { std::uint64_t offset; std::uint32_t size; std::wstring dest; };
    std::vector<Job> jobs;
    std::vector<std::wstring> dirs;

    struct Item { const Pk2Folder* node; std::wstring disk; };
    std::vector<Item> stack;
    stack.push_back({&folder, dest_dir});
    while (!stack.empty()) {
        Item cur = stack.back();
        stack.pop_back();
        dirs.push_back(cur.disk);
        for (const Pk2File& f : cur.node->files)
            jobs.push_back({f.data_offset, f.size, cur.disk + L"\\" + f.name});
        for (const Pk2Folder& c : cur.node->folders)
            stack.push_back({&c, cur.disk + L"\\" + c.name});
    }
    // Read in archive order: neighbouring files come from neighbouring bytes.
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.offset < b.offset; });

    // Pre-create directories single-threaded.
    for (const std::wstring& d : dirs)
        create_directories(d);

    const std::uint64_t total = jobs.size();
    std::atomic<std::uint64_t> next{0};
    std::atomic<std::uint64_t> done{0};
    std::atomic<std::uint64_t> failed{0};
    std::atomic<bool> stop{false};
    std::wstring current;
    std::mutex current_mtx;

    const unsigned nthreads = worker_count(jobs.size(), 16);
    auto worker = [&]() {
        UniqueHandle h(::CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (h.get() == INVALID_HANDLE_VALUE) {
            stop.store(true);
            return;
        }
        std::vector<std::uint8_t> buf;
        for (;;) {
            if (stop.load(std::memory_order_relaxed))
                break;
            std::uint64_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= jobs.size())
                break;
            const Job& j = jobs[static_cast<std::size_t>(i)];
            try {
                buf.resize(j.size);
                if (j.size)
                    read_at(h.get(), j.offset, buf.data(), buf.size());
                write_whole_file(j.dest, buf.data(), buf.size());
            } catch (...) {
                failed.fetch_add(1, std::memory_order_relaxed); // reported once all others are written
            }
            {
                std::lock_guard<std::mutex> lk(current_mtx);
                std::size_t slash = j.dest.find_last_of(L'\\');
                current = (slash == std::wstring::npos) ? j.dest : j.dest.substr(slash + 1);
            }
            done.fetch_add(1, std::memory_order_relaxed);
        }
    };

    {
        std::vector<std::jthread> pool;
        for (unsigned t = 0; t < nthreads; ++t)
            pool.emplace_back(worker);

        // Main thread reports progress until all files are done.
        while (done.load(std::memory_order_relaxed) < total && !stop.load(std::memory_order_relaxed)) {
            if (cancel && cancel())
                stop.store(true, std::memory_order_relaxed);
            if (progress) {
                std::wstring cur;
                {
                    std::lock_guard<std::mutex> lk(current_mtx);
                    cur = current;
                }
                progress(Progress{done.load(std::memory_order_relaxed), total, cur});
            }
            ::Sleep(40);
        }
    }
    if (progress)
        progress(Progress{done.load(std::memory_order_relaxed), total, L""});
    if (failed.load())
        throw Pk2Error(wide_to_utf8(Lf("ar_extract_failed", failed.load(), total)));
}

void Pk2Archive::extract_all(const std::wstring& dest_dir, const ProgressFn& progress,
                             const CancelFn& cancel) const {
    // Root extraction folder name is ALWAYS the lowercase stem of the .pk2.
    std::wstring root_name = to_lower(path_stem(path_));
    std::wstring base = dest_dir;
    if (!base.empty() && (base.back() == L'\\' || base.back() == L'/'))
        base.pop_back();
    base += L"\\" + root_name;
    extract_folder(root_, base, progress, cancel);
}

// ---- space report -----------------------------------------------------------------
SpaceInfo Pk2Archive::space() const {
    SpaceInfo s;
    if (!file_)
        return s;
    s.file_size = size_on_disk();
    s.files = files_.size();
    s.folders = folder_blocks_.empty() ? 0 : folder_blocks_.size() - 1;
    std::uint64_t min_blocks = 0;
    for (const auto& [first, chain] : chains_) {
        std::uint64_t used = 0;
        for (const ChainBlock& cb : chain)
            used += cb.used;
        s.blocks += chain.size();
        min_blocks += std::max<std::uint64_t>(1, (used + kEntriesPerBlock - 1) / kEntriesPerBlock);
    }
    std::uint64_t live = 0;
    for (const auto& [off, r] : regions_)
        live += r.size;
    s.index = kHeaderSize + s.blocks * kBlockSize;
    s.data = live > s.index ? live - s.index : 0;
    for (const auto& [off, size] : holes_)
        s.free += size;
    if (s.file_size > end_)
        s.free += s.file_size - end_;
    s.compact_size = kHeaderSize + min_blocks * kBlockSize + s.data;
    return s;
}

// ---- compact ---------------------------------------------------------------------
std::uint64_t Pk2Archive::defragment(const ProgressFn& progress, const CancelFn& cancel) {
    if (!file_)
        return 0;
    reload(); // the index exactly as it is on disk
    ::FlushFileBuffers(H(file_));
    const std::uint64_t source_size = size_on_disk();

    // 1. Every index block, decoded in parallel (one read handle per worker).
    std::vector<std::uint64_t> block_offs;
    for (const auto& [first, chain] : chains_)
        for (const ChainBlock& cb : chain)
            block_offs.push_back(cb.offset);
    std::vector<Block> decoded(block_offs.size());
    {
        std::vector<UniqueHandle> readers(worker_count(block_offs.size(), 16));
        for (UniqueHandle& r : readers)
            r = open_for_read(path_);
        parallel_for(block_offs.size(), 16, [&](std::size_t b, std::size_t e, unsigned w) {
            std::array<std::uint8_t, kBlockSize> enc{};
            for (std::size_t i = b; i < e; ++i) {
                read_at(readers[w].get(), block_offs[i], enc.data(), enc.size());
                decoded[i] = decode_block(enc);
            }
        });
    }
    std::unordered_map<std::uint64_t, std::size_t> block_index;
    block_index.reserve(block_offs.size());
    for (std::size_t i = 0; i < block_offs.size(); ++i)
        block_index.emplace(block_offs[i], i);

    // 2. Folders breadth first from the root, each with its entries in chain order, packed
    //    20 to a block. Entries that point nowhere are dropped.
    struct FolderPlan {
        std::uint64_t old_first = 0;
        std::size_t parent = 0;              // index in plan
        std::vector<Entry> entries;
        std::uint64_t new_first = 0;
        std::uint64_t blocks = 1;
    };
    std::vector<FolderPlan> plan;
    std::unordered_map<std::uint64_t, std::size_t> plan_of;
    plan.push_back({kHeaderSize, 0, {}, 0, 1});
    plan_of.emplace(kHeaderSize, 0);
    std::uint64_t file_entries = 0;
    for (std::size_t q = 0; q < plan.size(); ++q) {
        std::vector<Entry> entries;
        const std::uint64_t own_first = plan[q].old_first;
        for (const ChainBlock& cb : chains_.at(own_first)) {
            for (const Entry& src : decoded[block_index.at(cb.offset)].entries) {
                if (src.type == EntryType::Empty)
                    continue;
                Entry e = src;
                e.next_block = 0;
                if (e.type == EntryType::Folder && !is_dot_entry(e, L".") && !is_dot_entry(e, L"..")) {
                    const auto child = static_cast<std::uint64_t>(e.offset);
                    if (!chains_.contains(child))
                        continue;
                    if (!plan_of.contains(child)) {
                        plan_of.emplace(child, plan.size());
                        plan.push_back({child, q, {}, 0, 1});
                    }
                } else if (e.type == EntryType::File) {
                    if (e.size && static_cast<std::uint64_t>(e.offset) + e.size > source_size)
                        fail("ar_damaged");
                    ++file_entries;
                }
                entries.push_back(e);
            }
        }
        plan[q].entries = std::move(entries);
    }
    std::uint64_t cursor = kHeaderSize;
    for (FolderPlan& p : plan) {
        p.blocks = std::max<std::uint64_t>(1, (p.entries.size() + kEntriesPerBlock - 1) / kEntriesPerBlock);
        p.new_first = cursor;
        cursor += p.blocks * kBlockSize;
    }
    const std::uint64_t data_base = cursor;

    // 3. Data back to back in the order it has now (a near-sequential read of the source);
    //    entries sharing the same bytes keep sharing them.
    struct DataRef { std::uint64_t src; std::uint32_t size; std::size_t folder; std::size_t entry; };
    std::vector<DataRef> refs;
    refs.reserve(static_cast<std::size_t>(file_entries));
    for (std::size_t f = 0; f < plan.size(); ++f)
        for (std::size_t i = 0; i < plan[f].entries.size(); ++i) {
            Entry& e = plan[f].entries[i];
            if (e.type != EntryType::File)
                continue;
            if (e.size == 0)
                e.offset = static_cast<std::int64_t>(data_base);
            else
                refs.push_back({static_cast<std::uint64_t>(e.offset), e.size, f, i});
        }
    std::stable_sort(refs.begin(), refs.end(), [](const DataRef& a, const DataRef& b) { return a.src < b.src; });

    struct Piece { std::uint64_t src; std::uint64_t len; };
    struct Segment { std::vector<Piece> pieces; std::uint64_t bytes = 0; std::uint64_t files = 0; std::wstring last; };
    constexpr std::uint64_t kSegment = 8ull << 20;
    std::vector<Segment> segments(1);
    struct Check { std::uint64_t src; std::uint64_t dst; std::uint32_t size; };
    std::vector<Check> checks;
    checks.reserve(refs.size());
    std::uint64_t copies = 0;
    for (std::size_t r = 0; r < refs.size(); ++r) {
        const DataRef& d = refs[r];
        Entry& e = plan[d.folder].entries[d.entry];
        if (r > 0 && refs[r - 1].src == d.src && refs[r - 1].size == d.size) {
            e.offset = plan[refs[r - 1].folder].entries[refs[r - 1].entry].offset; // shared bytes
            continue;
        }
        e.offset = static_cast<std::int64_t>(cursor);
        checks.push_back({d.src, cursor, d.size});
        cursor += d.size;
        ++copies;
        std::uint64_t src = d.src, left = d.size;
        while (left) {
            Segment* seg = &segments.back();
            if (seg->bytes == kSegment) {
                segments.emplace_back();
                seg = &segments.back();
            }
            const std::uint64_t take = std::min(left, kSegment - seg->bytes);
            if (!seg->pieces.empty() && seg->pieces.back().src + seg->pieces.back().len == src)
                seg->pieces.back().len += take;
            else
                seg->pieces.push_back({src, take});
            seg->bytes += take;
            src += take;
            left -= take;
        }
        segments.back().files += 1;
        segments.back().last = decode_pk2_name(e.name);
    }
    if (segments.back().bytes == 0)
        segments.pop_back();
    const std::uint64_t total_size = cursor;

    // 4. Index blocks with their new links, encoded in parallel.
    const std::uint64_t index_bytes = data_base - kHeaderSize;
    std::vector<std::uint8_t> index(static_cast<std::size_t>(index_bytes));
    parallel_for(plan.size(), 32, [&](std::size_t b, std::size_t e, unsigned) {
        for (std::size_t f = b; f < e; ++f) {
            const FolderPlan& p = plan[f];
            for (std::uint64_t k = 0; k < p.blocks; ++k) {
                Block blk;
                for (std::size_t j = 0; j < kEntriesPerBlock; ++j) {
                    const std::size_t idx = static_cast<std::size_t>(k * kEntriesPerBlock + j);
                    if (idx >= p.entries.size())
                        break;
                    Entry en = p.entries[idx];
                    if (en.type == EntryType::Folder) {
                        if (is_dot_entry(en, L"."))
                            en.offset = static_cast<std::int64_t>(p.new_first);
                        else if (is_dot_entry(en, L".."))
                            en.offset = static_cast<std::int64_t>(plan[p.parent].new_first);
                        else
                            en.offset = static_cast<std::int64_t>(plan[plan_of.at(static_cast<std::uint64_t>(en.offset))].new_first);
                    }
                    blk.entries[j] = en;
                }
                blk.set_next(k + 1 < p.blocks ? static_cast<std::int64_t>(p.new_first + (k + 1) * kBlockSize) : 0);
                encode_block(blk, index.data() + (p.new_first - kHeaderSize) + k * kBlockSize);
            }
        }
    });

    // 5. The new file: header (as it is), index, then the data. Workers read segments ahead;
    //    this thread writes them in order, so the output is one sequential stream.
    const std::wstring tmp = path_ + L".compact.tmp";
    ::DeleteFileW(tmp.c_str());
    bool cancelled = false;
    try {
        UniqueHandle out(::CreateFileW(tmp.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (out.get() == INVALID_HANDLE_VALUE)
            fail_path("ar_create", tmp);
        FILE_ALLOCATION_INFO alloc{};
        alloc.AllocationSize.QuadPart = static_cast<LONGLONG>(total_size);
        ::SetFileInformationByHandle(out.get(), FileAllocationInfo, &alloc, sizeof(alloc)); // a hint only

        std::array<std::uint8_t, kHeaderSize> header{};
        seek(0);
        read_raw(header.data(), header.size());
        write_all(out.get(), header.data(), header.size(), "ar_io_write");
        write_all(out.get(), index.data(), index.size(), "ar_io_write");
        index = {};

        const unsigned threads = worker_count(segments.size(), 8);
        std::vector<UniqueHandle> readers(threads);
        for (UniqueHandle& r : readers)
            r = open_for_read(path_);
        std::uint64_t done = 0;
        const bool finished = ordered_pipeline<std::vector<std::uint8_t>>(
            segments.size(), threads, std::size_t{threads} * 3,
            [&](std::size_t i, unsigned w) {
                const Segment& seg = segments[i];
                std::vector<std::uint8_t> buf(static_cast<std::size_t>(seg.bytes));
                std::size_t pos = 0;
                for (const Piece& p : seg.pieces) {
                    read_at(readers[w].get(), p.src, buf.data() + pos, static_cast<std::size_t>(p.len));
                    pos += static_cast<std::size_t>(p.len);
                }
                return buf;
            },
            [&](std::size_t i, std::vector<std::uint8_t>&& buf) {
                write_all(out.get(), buf.data(), buf.size(), "ar_io_write");
                done += segments[i].files;
                if (progress)
                    progress(Progress{done, copies, segments[i].last});
            },
            cancel);
        if (!finished) {
            cancelled = true;
        } else {
            if (!::FlushFileBuffers(out.get()))
                fail_last("ar_io_write");
            // Spot check: the first and last bytes of every file read back from the new file.
            parallel_for(checks.size(), 16, [&](std::size_t b, std::size_t e, unsigned w) {
                (void)w;
                UniqueHandle src = open_for_read(path_);
                UniqueHandle dst = open_for_read(tmp);
                std::array<std::uint8_t, 64> x{}, y{};
                for (std::size_t i = b; i < e; ++i) {
                    const Check& c = checks[i];
                    const std::size_t n = std::min<std::size_t>(c.size, x.size());
                    for (const std::uint64_t at : {std::uint64_t{0}, std::uint64_t{c.size - n}}) {
                        read_at(src.get(), c.src + at, x.data(), n);
                        read_at(dst.get(), c.dst + at, y.data(), n);
                        if (std::memcmp(x.data(), y.data(), n) != 0)
                            fail("ar_verify");
                    }
                }
            });
        }
    } catch (...) {
        ::DeleteFileW(tmp.c_str());
        throw;
    }
    if (cancelled) {
        ::DeleteFileW(tmp.c_str());
        return size_on_disk();
    }

    // 6. The new file opened on its own: same tree, not one free byte.
    try {
        Pk2Archive check;
        check.open(tmp, key_);
        const SpaceInfo s = check.space();
        std::uint64_t entries = 0;
        for (const FolderPlan& p : plan)
            for (const Entry& e : p.entries)
                entries += e.type == EntryType::File ? 1 : 0;
        std::uint64_t got = 0;
        std::vector<const Pk2Folder*> st{&check.root()};
        while (!st.empty()) {
            const Pk2Folder* f = st.back();
            st.pop_back();
            got += f->files.size();
            for (const Pk2Folder& c : f->folders)
                st.push_back(&c);
        }
        if (got != entries || s.file_size != total_size || s.free != 0 || s.folders + 1 != plan.size())
            fail("ar_verify");
    } catch (...) {
        ::DeleteFileW(tmp.c_str());
        throw;
    }

    // 7. Swap it in. The archive as it was opened becomes <path>.bak (unless this session
    //    already saved one before its first change).
    const std::wstring bak = path_ + L".bak";
    const std::wstring path = path_;
    const std::string key = key_;
    const bool had_backup = backed_up_;
    close();
    BOOL ok;
    if (!had_backup) {
        ::DeleteFileW(bak.c_str());
        ok = ::ReplaceFileW(path.c_str(), tmp.c_str(), bak.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr);
    } else {
        ok = ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    if (!ok) {
        const DWORD err = ::GetLastError();
        if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && !had_backup)
            ::MoveFileExW(bak.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH); // put the original back
        ::DeleteFileW(tmp.c_str());
        open(path, key);
        backed_up_ = had_backup;
        ::SetLastError(err);
        fail_last("ar_replace");
    }
    open(path, key);
    backed_up_ = true;
    return size_on_disk();
}

} // namespace pk2
