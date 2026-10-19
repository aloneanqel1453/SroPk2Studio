#include "store.hpp"

#include "i18n.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace pk2 {
namespace {

// Directory tree assembled from BAK's flat entry list. std::map keeps child nodes at stable
// addresses while entries arrive in any order; it is converted to Pk2Folder vectors afterwards.
struct Node {
    std::wstring name;                   // display name
    std::map<std::wstring, Node> dirs;   // key: lowercase name
    std::vector<Pk2File> files;
};

std::vector<std::wstring> split_path(std::wstring_view p) {
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : p) {
        if (c == L'\\' || c == L'/') { if (!cur.empty()) parts.push_back(std::move(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(std::move(cur));
    return parts;
}

void to_folder(Node& n, Pk2Folder& out) {
    for (auto& [key, child] : n.dirs) {
        Pk2Folder f;
        f.name = child.name;
        f.path = out.path.empty() ? key : out.path + L"/" + key;
        to_folder(child, f);
        out.folders.push_back(std::move(f));
    }
    out.files = std::move(n.files);
}

void write_bytes(const fs::path& dest, const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    if (dest.has_parent_path()) fs::create_directories(dest.parent_path(), ec);
    std::ofstream o(dest, std::ios::binary | std::ios::trunc);
    if (!o) throw Pk2Error(wide_to_utf8(Lf("ar_st_create", dest.wstring())));
    o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!o) throw Pk2Error(wide_to_utf8(Lf("ar_st_write", dest.wstring())));
}

std::uint64_t count_files(const Pk2Folder& f) {
    std::uint64_t n = f.files.size();
    for (const Pk2Folder& c : f.folders) n += count_files(c);
    return n;
}

} // namespace

std::wstring Store::norm(std::wstring_view p) {
    std::wstring out;
    for (const std::wstring& part : split_path(p)) { if (!out.empty()) out += L'/'; out += part; }
    return to_lower(out);
}

std::wstring Store::bak_path(std::wstring_view inner) {
    std::wstring p(inner);
    std::replace(p.begin(), p.end(), L'/', L'\\');
    while (!p.empty() && p.front() == L'\\') p.erase(p.begin());
    return p;
}

void Store::bak_check(bool ok, const std::wstring& err) const {
    if (!ok) throw Pk2Error(wide_to_utf8(err.empty() ? L("ar_st_failed") : err));
}

void Store::open(const std::wstring& path, std::string_view key, bool deep_key_search) {
    close();
    std::error_code ec;
    const bool is_dir = fs::is_directory(path, ec);
    const std::wstring ext = to_lower(fs::path(path).extension().wstring());
    std::exception_ptr engine_error;
    if (!is_dir && ext != L".vp" && !deep_key_search) {
        try {
            pk2_.open(path, key);
            // Same container through BAK (read-only, lazy): arms the asset ciphers it detects
            // and resolves references into the sibling containers. Optional.
            std::wstring err;
            assets_ = bakb::Archive::open(path, std::string(key), err);
            return;
        } catch (const Pk2Error&) {
            engine_error = std::current_exception();
        }
    }
    // PK2v2 / .vp / folders / SailSafe / other key schemes: BAK's backends, writable where BAK can write.
    std::wstring err;
    bak_ = bakb::Archive::open(path, std::string(key), err, deep_key_search, true);
    if (!bak_) {
        if (engine_error) std::rethrow_exception(engine_error);
        throw Pk2Error(wide_to_utf8(err.empty() ? L("ar_st_open") : err));
    }
    bak_path_ = path;
    build_bak_tree();
}

void Store::create(const std::wstring& path, std::string_view key) {
    close();
    pk2_.create(path, key);
}

void Store::close() {
    assets_.reset();
    bak_.reset();
    bak_path_.clear();
    bak_root_ = Pk2Folder{};
    bak_paths_.clear();
    bak_folders_.clear();
    bak_files_.clear();
    if (pk2_.is_open()) pk2_.close();
}

std::wstring Store::kind() const {
    if (bak_) return bak_->kind();
    return pk2_.is_open() ? L"PK2" : L"";
}

void Store::build_bak_tree() {
    // Rebuilt after every change: drop the old indexes (their pointers die with the old tree).
    bak_paths_.clear();
    bak_folders_.clear();
    bak_files_.clear();
    Node top;
    for (const bakb::Entry& e : bak_->entries()) {
        std::vector<std::wstring> parts = split_path(e.path);
        if (parts.empty()) continue;
        const std::size_t ndirs = e.folder ? parts.size() : parts.size() - 1;
        Node* d = &top;
        for (std::size_t i = 0; i < ndirs; ++i) {
            Node& child = d->dirs[to_lower(parts[i])];
            if (child.name.empty()) child.name = parts[i];
            d = &child;
        }
        if (e.folder) continue;
        Pk2File f;
        f.name = parts.back();
        f.path = norm(e.path);
        f.data_offset = bak_paths_.size();
        f.size = static_cast<std::uint32_t>(std::min<std::uint64_t>(e.size, 0xFFFFFFFFull));
        f.ctime = f.mtime = e.mtime;
        bak_paths_.push_back(e.path);
        d->files.push_back(std::move(f));
    }
    bak_root_ = Pk2Folder{};
    to_folder(top, bak_root_);
    // Index after the tree is final: the vectors no longer move.
    std::vector<const Pk2Folder*> st{&bak_root_};
    while (!st.empty()) {
        const Pk2Folder* f = st.back(); st.pop_back();
        bak_folders_[f->path] = f;
        for (const Pk2File& x : f->files) bak_files_[x.path] = &x;
        for (const Pk2Folder& c : f->folders) st.push_back(&c);
    }
}

const Pk2Folder* Store::find_folder(std::wstring_view path) const {
    if (!bak_) return pk2_.find_folder(path);
    auto it = bak_folders_.find(norm(path));
    return it == bak_folders_.end() ? nullptr : it->second;
}

const Pk2File* Store::find_file(std::wstring_view path) const {
    if (!bak_) return pk2_.find_file(path);
    auto it = bak_files_.find(norm(path));
    return it == bak_files_.end() ? nullptr : it->second;
}

std::vector<std::uint8_t> Store::read_file(const Pk2File& file) const {
    std::vector<std::uint8_t> out;
    if (!bak_) {
        out = pk2_.read_file(file);
        bakb::decrypt_asset(file.name, out);
        return out;
    }
    if (file.data_offset >= bak_paths_.size()) throw Pk2Error(wide_to_utf8(L("ar_st_foreign")));
    std::wstring err;
    if (!bak_->read(bak_paths_[static_cast<std::size_t>(file.data_offset)], out, err))
        throw Pk2Error(wide_to_utf8(err.empty() ? Lf("ar_st_read", file.name) : err));
    return out;
}

bool Store::read_any(const std::wstring& inner_path, std::vector<std::uint8_t>& out) const {
    if (const Pk2File* f = find_file(inner_path)) {
        try { out = read_file(*f); return true; } catch (const Pk2Error&) { return false; }
    }
    const bakb::Archive* a = assets();
    if (!a) return false;
    std::wstring p = inner_path;
    std::replace(p.begin(), p.end(), L'/', L'\\');
    std::wstring err;
    return a->read(p, out, err, true);
}

void Store::require_writable() const {
    if (read_only())
        throw Pk2Error(wide_to_utf8(Lf("ar_st_readonly", bak_->kind())));
}

std::wstring Store::protection_report() const {
    const bakb::Archive* a = assets();
    return a ? a->protection_report() : std::wstring{};
}
bool Store::recover_asset_cipher(std::wstring& how) {
    bakb::Archive* a = bak_ ? bak_.get() : assets_.get();
    if (!a) { how = L("ar_st_nobak"); return false; }
    return a->recover_asset_cipher(how);
}
bool Store::recover_vp_payload_key(std::wstring& how) {
    if (!bak_) { how = L("ar_st_not_vp"); return false; }
    bool ok = bak_->recover_vp_payload_key(how);
    if (ok) build_bak_tree();
    return ok;
}

bool Store::add_file(std::wstring_view inner_path, const std::vector<std::uint8_t>& bytes) {
    require_writable();
    if (!bak_) return pk2_.add_file(inner_path, bytes);
    std::wstring err; bak_check(bak_->add_file(bak_path(inner_path), bytes, err), err);
    return true;
}
bool Store::remove_file(std::wstring_view inner_path) {
    require_writable();
    if (!bak_) return pk2_.remove_file(inner_path);
    std::wstring err; bak_check(bak_->remove(bak_path(inner_path), err), err);
    return true;
}
bool Store::remove_folder(std::wstring_view inner_path) {
    require_writable();
    if (!bak_) return pk2_.remove_folder(inner_path);
    std::wstring err; bak_check(bak_->remove(bak_path(inner_path), err), err);
    return true;
}

std::uint64_t Store::import_folder(const std::wstring& src_dir, std::wstring_view inner_dest,
                                   const ProgressFn& progress, const CancelFn& cancel) {
    require_writable();
    if (!bak_) return pk2_.import_folder(src_dir, inner_dest, progress, cancel);
    // Engine semantics: the folder's contents land under inner_dest (dest/<relative path>).
    const std::wstring dest = bak_path(inner_dest);
    std::uint64_t files = 0;
    std::wstring err;
    bool ok = bak_->import_folder(src_dir, dest, [&](std::uint64_t done, std::uint64_t total, const std::wstring& cur) {
        files = done;
        if (progress) progress(Progress{done, total, cur});
        return !(cancel && cancel());
    }, err);
    bak_check(ok, err);
    return files;
}

std::uint64_t Store::add_disk_files(std::span<const DiskFile> files, const ProgressFn& progress,
                                    const CancelFn& cancel) {
    require_writable();
    if (!bak_) return pk2_.add_disk_files(files, progress, cancel);
    std::uint64_t done = 0;
    for (const DiskFile& f : files) {
        if (cancel && cancel()) break;
        std::ifstream in(fs::path(f.disk), std::ios::binary);
        if (!in) throw Pk2Error(wide_to_utf8(Lf("ar_st_read", f.disk)));
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        add_file(f.inner, bytes);
        if (progress) progress(Progress{++done, files.size(), f.inner});
    }
    return done;
}

std::uint64_t Store::defragment(const ProgressFn& progress, const CancelFn& cancel) {
    require_writable();
    if (bak_) {
        std::uint64_t saved = 0; std::wstring err;
        bool ok = bak_->compact(&saved, [&](std::uint64_t done, std::uint64_t total, const std::wstring& cur) {
            if (progress) progress(Progress{done, total, cur});
            return !(cancel && cancel());
        }, err);
        bak_check(ok, err);
        build_bak_tree();
        return file_size();
    }
    // Defrag replaces the file on disk: drop BAK's handle on it first, reattach afterwards.
    assets_.reset();
    std::uint64_t r = pk2_.defragment(progress, cancel);
    std::wstring err;
    assets_ = bakb::Archive::open(pk2_.path(), pk2_.key(), err);
    return r;
}

void Store::reload() {
    if (!bak_) { pk2_.reload(); return; }
    std::wstring err;
    bak_->flush(err);
    build_bak_tree();
}

void Store::extract_file(const Pk2File& file, const std::wstring& dest_file) const {
    if (!bak_) { pk2_.extract_file(file, dest_file); return; }
    write_bytes(dest_file, read_file(file));
}

void Store::extract_folder(const Pk2Folder& folder, const std::wstring& dest_dir,
                           const ProgressFn& progress, const CancelFn& cancel) const {
    if (!bak_) { pk2_.extract_folder(folder, dest_dir, progress, cancel); return; }
    const std::uint64_t total = count_files(folder);
    std::uint64_t done = 0;
    std::vector<std::pair<const Pk2Folder*, fs::path>> st{{&folder, fs::path(dest_dir)}};
    while (!st.empty()) {
        auto [f, dir] = st.back(); st.pop_back();
        for (const Pk2File& x : f->files) {
            if (cancel && cancel()) return;
            write_bytes(dir / x.name, read_file(x));
            if (progress) progress(Progress{++done, total, x.path});
        }
        for (const Pk2Folder& c : f->folders) st.push_back({&c, dir / c.name});
    }
}

void Store::extract_all(const std::wstring& dest_dir, const ProgressFn& progress, const CancelFn& cancel) const {
    if (!bak_) { pk2_.extract_all(dest_dir, progress, cancel); return; }
    // Same rule as the engine: the root folder is the lowercase stem of the container.
    fs::path src(bak_path_);
    std::wstring stem = to_lower(src.has_stem() && !src.stem().empty() ? src.stem().wstring() : src.filename().wstring());
    extract_folder(bak_root_, (fs::path(dest_dir) / stem).wstring(), progress, cancel);
}

std::uint64_t Store::file_size() const {
    if (!bak_) return pk2_.file_size();
    std::error_code ec;
    if (fs::is_directory(bak_path_, ec)) {
        std::uint64_t n = 0;
        for (const auto& [p, f] : bak_files_) n += f->size;
        return n;
    }
    const auto sz = fs::file_size(bak_path_, ec);
    return ec ? 0 : static_cast<std::uint64_t>(sz);
}

} // namespace pk2
