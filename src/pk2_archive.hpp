#pragma once
// Silkroad .pk2 archive reader / writer / compactor.
// Format: 256-byte plaintext header, then chains of 2560-byte Blowfish-ECB-encoded blocks of
// 20 x 128-byte entries. File data is stored raw and packed back to back, like Joymax's own
// archives (no alignment).
//
// Space is managed so the archive never grows needlessly: a change reuses the space it frees
// (in place when the new data fits, otherwise the smallest hole that fits), freed space at the end
// of the file is cut off, and an emptied index block is unlinked and reused. compact() rewrites
// the archive with no dead byte at all: header, minimal index blocks, then the data.
#include "blowfish.hpp"
#include "text.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pk2 {

constexpr std::size_t   kHeaderSize     = 256;
constexpr std::size_t   kBlockSize      = 2560;
constexpr std::size_t   kEntrySize      = 128;
constexpr std::size_t   kEntriesPerBlock = 20;
constexpr std::uint64_t kChunkAlign     = 4096;

enum class EntryType : std::uint8_t { Empty = 0, Folder = 1, File = 2 };

class Pk2Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
class Pk2AuthError : public Pk2Error {
public:
    using Pk2Error::Pk2Error;
};

struct Pk2File {
    std::wstring name;              // display name
    std::wstring path;             // full lowercase display path inside the archive
    std::uint64_t data_offset = 0;
    std::uint32_t size = 0;
    std::int64_t  ctime = 0;       // FILETIME
    std::int64_t  mtime = 0;
};

struct Pk2Folder {
    std::wstring name;              // display name ("" for root)
    std::wstring path;             // full lowercase display path ("" for root)
    std::uint64_t block_offset = 0;
    std::vector<Pk2Folder> folders;
    std::vector<Pk2File>   files;
};

struct Progress {
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    std::wstring  current;
};
using ProgressFn = std::function<void(const Progress&)>;
// Return true from a cancel check to abort a long operation.
using CancelFn = std::function<bool()>;

// Where the bytes of an archive go. compact_size is the size compact() would produce.
struct SpaceInfo {
    std::uint64_t file_size = 0;    // bytes on disk
    std::uint64_t data = 0;         // file contents
    std::uint64_t index = 0;        // header + folder blocks
    std::uint64_t free = 0;         // holes between / after the live regions
    std::uint64_t files = 0;
    std::uint64_t folders = 0;
    std::uint64_t blocks = 0;
    std::uint64_t compact_size = 0;
    [[nodiscard]] std::uint64_t reclaimable() const noexcept {
        return file_size > compact_size ? file_size - compact_size : 0;
    }
};

// One file of an add batch: read from `disk`, stored as `inner` (keeps its case).
struct DiskFile {
    std::wstring disk;
    std::wstring inner;
};

class Pk2Archive {
public:
    Pk2Archive() = default;
    ~Pk2Archive();
    Pk2Archive(const Pk2Archive&) = delete;
    Pk2Archive& operator=(const Pk2Archive&) = delete;

    // Open (and validate the key against the header checksum) an existing .pk2.
    void open(const std::wstring& path, std::string_view key);
    // Create a brand-new empty .pk2.
    void create(const std::wstring& path, std::string_view key);
    void close();
    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }

    [[nodiscard]] const std::wstring& path() const noexcept { return path_; }
    [[nodiscard]] const std::string&  key()  const noexcept { return key_; }
    [[nodiscard]] const Pk2Folder&    root() const noexcept { return root_; }

    // Lookups (path uses '/' or '\\', case-insensitive, relative to root).
    [[nodiscard]] const Pk2Folder* find_folder(std::wstring_view path) const;
    [[nodiscard]] const Pk2File*   find_file(std::wstring_view path) const;

    // Read a file's raw bytes.
    [[nodiscard]] std::vector<std::uint8_t> read_file(const Pk2File& file) const;

    // Mutations. Each backs up the archive once (<path>.bak, on the first change after open).
    bool add_folder(std::wstring_view inner_path);
    bool add_file(std::wstring_view inner_path, const std::vector<std::uint8_t>& bytes);
    bool remove_file(std::wstring_view inner_path);
    bool remove_folder(std::wstring_view inner_path);

    // Add many disk files: worker threads read ahead while the archive is written in order.
    // Returns the number of files added.
    std::uint64_t add_disk_files(std::span<const DiskFile> files,
                                 const ProgressFn& progress = {}, const CancelFn& cancel = {});
    // Import an entire on-disk folder tree under inner_dest (recursive).
    // Returns number of files imported.
    std::uint64_t import_folder(const std::wstring& src_dir, std::wstring_view inner_dest,
                                const ProgressFn& progress = {}, const CancelFn& cancel = {});

    // Extract everything. The destination ROOT folder name is always the
    // lowercase stem of the .pk2 file (e.g. Media.pk2 -> "media").
    void extract_all(const std::wstring& dest_dir,
                     const ProgressFn& progress = {}, const CancelFn& cancel = {}) const;
    // Extract a single folder subtree into dest_dir (keeps its own name).
    void extract_folder(const Pk2Folder& folder, const std::wstring& dest_dir,
                        const ProgressFn& progress = {}, const CancelFn& cancel = {}) const;
    void extract_file(const Pk2File& file, const std::wstring& dest_file) const;

    // Rewrite the archive with no dead space: header, the fewest index blocks, then every file's
    // data back to back in its current order (identical regions stored once). Only this archive
    // is touched. The new file is built next to it, verified, then swapped in; the archive as it
    // was opened is kept as <path>.bak. Returns the new file size (unchanged when cancelled).
    std::uint64_t defragment(const ProgressFn& progress = {}, const CancelFn& cancel = {});

    // Reload the in-memory directory tree from disk.
    void reload();

    [[nodiscard]] std::uint64_t file_size() const;
    [[nodiscard]] SpaceInfo space() const;

    // On-disk entry/block layout (public so free helpers can (de)serialize them).
    struct Entry {
        EntryType type = EntryType::Empty;
        std::array<std::uint8_t, kNameBytes> name{};
        std::int64_t  ctime = 0;
        std::int64_t  mtime = 0;
        std::int64_t  offset = 0;
        std::uint32_t size = 0;
        std::int64_t  next_block = 0;
        std::array<std::uint8_t, 2> pad{};
    };
    struct Block {
        std::array<Entry, kEntriesPerBlock> entries{};
        [[nodiscard]] std::int64_t next() const noexcept {
            return entries[kEntriesPerBlock - 1].next_block;
        }
        void set_next(std::int64_t v) noexcept {
            entries[kEntriesPerBlock - 1].next_block = v;
        }
    };

private:
    // Low-level file I/O (Win32 HANDLE for >4GB support).
    void seek(std::uint64_t pos) const;
    void read_raw(void* buf, std::size_t len) const;
    void write_raw(const void* buf, std::size_t len) const;
    void write_at(std::uint64_t pos, std::span<const std::uint8_t> bytes) const;
    [[nodiscard]] std::uint64_t size_on_disk() const;

    [[nodiscard]] Block load_block(std::uint64_t offset) const;
    void store_block(std::uint64_t offset, const Block& block) const;
    [[nodiscard]] Block decode_block(std::span<const std::uint8_t> encoded) const;
    void encode_block(const Block& block, std::uint8_t* out) const;

    void create_base_stream() const;
    void read_tree();
    void read_tree_block(std::uint64_t offset, Pk2Folder& parent);

    // An entry's place: the block holding it and its index there.
    struct Slot {
        std::uint64_t block = 0;
        std::uint32_t index = 0;
    };
    struct FileRef {
        std::uint64_t offset = 0;
        std::uint32_t size = 0;
        std::uint64_t parent_block = 0; // first block of the owning folder's chain
        Slot slot;
    };
    struct ChainBlock {
        std::uint64_t offset = 0;
        std::uint32_t used = 0;         // non-empty entries
    };
    struct Region {
        std::uint64_t size = 0;
        std::uint32_t refs = 0;         // entries sharing these bytes
    };

    // Space management.
    void claim(std::uint64_t offset, std::uint64_t size);
    void release(std::uint64_t offset, std::uint64_t size);
    void add_hole(std::uint64_t offset, std::uint64_t size);
    void take_hole(std::map<std::uint64_t, std::uint64_t>::iterator it);
    void rebuild_holes();
    void trim_tail();
    [[nodiscard]] std::uint64_t allocate(std::uint64_t size);

    // Index blocks.
    [[nodiscard]] Slot free_slot(std::uint64_t first_block, Block& block);
    void clear_slot(std::uint64_t first_block, Slot slot);
    [[nodiscard]] std::uint64_t ensure_folder(const std::wstring& disp_path);
    [[nodiscard]] Slot find_entry(std::uint64_t first_block, EntryType type, std::uint64_t offset) const;

    void ensure_backup();
    static std::wstring normalize(std::wstring_view path);
    static std::int64_t now_filetime() noexcept;

    void* file_ = nullptr; // HANDLE
    std::wstring path_;
    std::string  key_;
    Blowfish     bf_;
    Pk2Folder    root_;

    std::map<std::uint64_t, Region> regions_;                          // live bytes: offset -> region
    std::map<std::uint64_t, std::uint64_t> holes_;                     // free bytes: offset -> size
    std::set<std::pair<std::uint64_t, std::uint64_t>> holes_by_size_;  // (size, offset), best fit
    std::uint64_t end_ = 0;                                            // file size we manage

    std::map<std::wstring, std::uint64_t> folder_blocks_;              // norm path -> first block
    std::unordered_map<std::uint64_t, std::wstring> folder_paths_;     // first block -> norm path
    std::map<std::wstring, Slot> folder_slots_;                        // norm path -> its entry
    std::unordered_map<std::uint64_t, std::vector<ChainBlock>> chains_; // first block -> chain
    std::map<std::wstring, FileRef> files_;                            // norm path -> file ref
    bool backed_up_ = false;
};

} // namespace pk2
