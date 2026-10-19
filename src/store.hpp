#pragma once
// One archive surface for the app. Normal JMXPACK .pk2 files go through our read/write engine
// (pk2::Pk2Archive). Everything it cannot open — PK2v2, VSROPLUS .vp, other Blowfish key schemes,
// an extracted client folder — is opened read-only through the vendored BAK backends and shown
// through the same Pk2Folder / Pk2File tree, so the UI needs no per-format code.
#include "bak_bridge.hpp"
#include "pk2_archive.hpp"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pk2 {

class Store {
public:
    Store() = default;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // deep_key_search: BAK's KeyFinder (all stages; slow) when no standard key opens the container.
    void open(const std::wstring& path, std::string_view key, bool deep_key_search = false);
    void create(const std::wstring& path, std::string_view key);
    void close();

    [[nodiscard]] bool is_open() const noexcept { return pk2_.is_open() || bak_ != nullptr; }
    [[nodiscard]] bool read_only() const noexcept { return bak_ != nullptr && !bak_->can_write(); }
    [[nodiscard]] bool bak_backed() const noexcept { return bak_ != nullptr; }
    // Detected container / key scheme / asset ciphers / VP signature state (BAK), readable text.
    [[nodiscard]] std::wstring protection_report() const;
    // Explicit recovery actions (may scan the client binaries / a running client / dumps).
    bool recover_asset_cipher(std::wstring& how);
    bool recover_vp_payload_key(std::wstring& how);
    [[nodiscard]] std::wstring kind() const;           // "PK2", "PK2v2", "VP", "Folder"
    [[nodiscard]] const std::wstring& path() const noexcept { return bak_ ? bak_path_ : pk2_.path(); }
    [[nodiscard]] const Pk2Folder& root() const noexcept { return bak_ ? bak_root_ : pk2_.root(); }

    [[nodiscard]] const Pk2Folder* find_folder(std::wstring_view path) const;
    [[nodiscard]] const Pk2File*   find_file(std::wstring_view path) const;

    // File bytes, asset-decrypted when a known asset cipher (MaxiGuard / VsroPlus / GoPro / ...) applies.
    [[nodiscard]] std::vector<std::uint8_t> read_file(const Pk2File& file) const;
    // Any inner path, also from the sibling containers next to this one (Data/Media/Particles/Map).
    bool read_any(const std::wstring& inner_path, std::vector<std::uint8_t>& out) const;
    // BAK view of the open container (dependency/sibling resolution for previews); may be null.
    [[nodiscard]] const bakb::Archive* assets() const noexcept { return bak_ ? bak_.get() : assets_.get(); }

    // Mutations: our engine for JMXPACK, BAK's writer for the other writable containers;
    // a read-only container (PK2v2, unsigned VP, locked file) throws Pk2Error.
    bool add_file(std::wstring_view inner_path, const std::vector<std::uint8_t>& bytes);
    bool remove_file(std::wstring_view inner_path);
    bool remove_folder(std::wstring_view inner_path);
    std::uint64_t import_folder(const std::wstring& src_dir, std::wstring_view inner_dest,
                                const ProgressFn& progress = {}, const CancelFn& cancel = {});
    // Disk files read ahead in parallel, written in order (the engine reuses freed space).
    std::uint64_t add_disk_files(std::span<const DiskFile> files,
                                 const ProgressFn& progress = {}, const CancelFn& cancel = {});
    // Compacts only this container (never its siblings).
    std::uint64_t defragment(const ProgressFn& progress = {}, const CancelFn& cancel = {});
    void reload();

    void extract_all(const std::wstring& dest_dir, const ProgressFn& progress = {}, const CancelFn& cancel = {}) const;
    void extract_folder(const Pk2Folder& folder, const std::wstring& dest_dir,
                        const ProgressFn& progress = {}, const CancelFn& cancel = {}) const;
    void extract_file(const Pk2File& file, const std::wstring& dest_file) const;

    [[nodiscard]] std::uint64_t file_size() const;
    // Space report of an engine-opened .pk2 (all zero for BAK containers).
    [[nodiscard]] SpaceInfo space() const { return bak_ ? SpaceInfo{} : pk2_.space(); }

private:
    void build_bak_tree();
    void require_writable() const;
    void bak_check(bool ok, const std::wstring& err) const;   // throws Pk2Error(err) when !ok
    static std::wstring bak_path(std::wstring_view inner);    // inner path with '\\' separators
    static std::wstring norm(std::wstring_view p);

    Pk2Archive pk2_;
    std::unique_ptr<bakb::Archive> bak_;     // read-only backend when pk2_ cannot open the container
    std::unique_ptr<bakb::Archive> assets_;  // BAK view of a pk2_-opened container: ciphers + siblings
    std::wstring bak_path_;
    Pk2Folder bak_root_;
    std::vector<std::wstring> bak_paths_;    // Pk2File::data_offset of a BAK file indexes this
    std::map<std::wstring, const Pk2Folder*> bak_folders_;
    std::map<std::wstring, const Pk2File*>   bak_files_;
};

} // namespace pk2
