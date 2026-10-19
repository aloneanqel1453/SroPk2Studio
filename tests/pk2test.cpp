// Console test harness for the pk2 core: create, add, list, extract (lowercase
// root), delete, overwrite-grow, defragment, and integrity re-check.
// Optionally opens real .pk2 files passed on the command line.
#include "blowfish.hpp"
#include "dds.hpp"
#include "file_types.hpp"
#include "jmx_parse.hpp"
#include "pk2_archive.hpp"
#include "text.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <print>
#include <span>
#include <string>
#include <vector>

using namespace pk2;

namespace {

int g_pass = 0;
int g_fail = 0;

std::string u8(std::wstring_view w) { return wide_to_utf8(w); }

void check(bool cond, std::string_view name) {
    if (cond) {
        ++g_pass;
        std::println("  [PASS] {}", name);
    } else {
        ++g_fail;
        std::println("  [FAIL] {}", name);
    }
}

std::wstring temp_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = ::GetTempPathW(MAX_PATH, buf);
    std::wstring base(buf, n);
    base += L"srop k2test_";
    base += std::to_wstring(::GetTickCount64());
    ::CreateDirectoryW(base.c_str(), nullptr);
    return base;
}

void write_disk_file(const std::wstring& path, const std::string& content) {
    std::size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring dir = path.substr(0, slash);
        std::wstring cur;
        for (wchar_t c : dir) {
            cur += c;
            if (c == L'\\' || c == L'/')
                ::CreateDirectoryW(cur.c_str(), nullptr);
        }
        ::CreateDirectoryW(dir.c_str(), nullptr);
    }
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD put = 0;
    ::WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &put, nullptr);
    ::CloseHandle(h);
}

std::string read_disk_file(const std::wstring& path) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER li;
    ::GetFileSizeEx(h, &li);
    std::string out(static_cast<std::size_t>(li.QuadPart), '\0');
    DWORD got = 0;
    ::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    ::CloseHandle(h);
    out.resize(got);
    return out;
}

bool path_exists(const std::wstring& p) {
    return ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::uint64_t count_files(const Pk2Folder& f) {
    std::uint64_t n = f.files.size();
    for (const auto& c : f.folders)
        n += count_files(c);
    return n;
}
std::uint64_t count_folders(const Pk2Folder& f) {
    std::uint64_t n = f.folders.size();
    for (const auto& c : f.folders)
        n += count_folders(c);
    return n;
}

void run_synthetic() {
    std::println("== Synthetic pk2 round-trip ==");
    std::wstring dir = temp_dir();
    std::wstring pk2 = dir + L"\\Media.pk2"; // capitalized on purpose
    const std::string key = "169841";

    const std::string c_root = "root-file-content-123";
    const std::string c_sub = "hello from subdir";
    const std::string c_deep = "deep nested data payload";

    // 1) Create + add.
    {
        Pk2Archive a;
        a.create(pk2, key);
        check(a.is_open(), "create() opens archive");
        a.add_file(L"root.txt", {c_root.begin(), c_root.end()});
        a.add_file(L"sub/one.txt", {c_sub.begin(), c_sub.end()});
        a.add_file(L"sub/deep/two.bin", {c_deep.begin(), c_deep.end()});
        a.add_folder(L"empty/folder");
        a.reload();
        check(count_files(a.root()) == 3, "3 files present after add");
        check(a.find_folder(L"empty/folder") != nullptr, "nested empty folder created");
        const Pk2File* f = a.find_file(L"sub/deep/two.bin");
        check(f != nullptr && f->size == c_deep.size(), "deep file size correct");
    }

    // 2) Reopen + key validation.
    {
        Pk2Archive a;
        a.open(pk2, key);
        check(a.is_open(), "reopen with correct key");
        const Pk2File* f = a.find_file(L"root.txt");
        check(f != nullptr, "root.txt found after reopen");
        if (f) {
            auto bytes = a.read_file(*f);
            std::string got(bytes.begin(), bytes.end());
            check(got == c_root, "root.txt content matches");
        }
    }
    {
        Pk2Archive a;
        bool threw = false;
        try {
            a.open(pk2, "000000");
        } catch (const Pk2AuthError&) {
            threw = true;
        }
        check(threw, "wrong key throws Pk2AuthError");
    }

    // 3) Extract all -> root folder MUST be lowercase "media".
    std::wstring outdir = dir + L"\\out";
    {
        Pk2Archive a;
        a.open(pk2, key);
        a.extract_all(outdir);
        check(path_exists(outdir + L"\\media"), "extract root folder exists");
        // Verify the ACTUAL on-disk casing is lowercase (NTFS is case-insensitive,
        // so query the stored name directly).
        WIN32_FIND_DATAW fd{};
        HANDLE fh = ::FindFirstFileW((outdir + L"\\media").c_str(), &fd);
        bool lower = (fh != INVALID_HANDLE_VALUE) && (std::wstring(fd.cFileName) == L"media");
        if (fh != INVALID_HANDLE_VALUE)
            ::FindClose(fh);
        check(lower, "extract root on-disk name is lowercase 'media'");
        check(read_disk_file(outdir + L"\\media\\root.txt") == c_root, "extracted root.txt matches");
        check(read_disk_file(outdir + L"\\media\\sub\\one.txt") == c_sub, "extracted sub/one.txt matches");
        check(read_disk_file(outdir + L"\\media\\sub\\deep\\two.bin") == c_deep,
              "extracted deep file matches");
    }

    // 4) Overwrite-grow + delete.
    {
        Pk2Archive a;
        a.open(pk2, key);
        std::string big(5000, 'X');
        a.add_file(L"root.txt", {big.begin(), big.end()}); // grows beyond original
        a.reload();
        const Pk2File* f = a.find_file(L"root.txt");
        check(f != nullptr && f->size == big.size(), "overwrite-grow updates size");
        auto bytes = a.read_file(*f);
        check(std::string(bytes.begin(), bytes.end()) == big, "overwritten content matches");

        bool removed = a.remove_file(L"sub/one.txt");
        a.reload();
        check(removed, "remove_file returns true");
        check(a.find_file(L"sub/one.txt") == nullptr, "removed file is gone");
        check(a.find_file(L"sub/deep/two.bin") != nullptr, "sibling file survives remove");
    }
    check(path_exists(pk2 + L".bak"), "backup .bak created before mutation");

    // 5) Defragment -> shrink + integrity.
    {
        Pk2Archive a;
        a.open(pk2, key);
        std::uint64_t before = a.file_size();
        std::uint64_t after = a.defragment();
        std::println("  defrag size: {} -> {} bytes", before, after);
        check(after <= before, "defragment does not grow the file");
        // Integrity after defrag.
        check(a.find_file(L"root.txt") != nullptr, "root.txt survives defrag");
        check(a.find_file(L"sub/deep/two.bin") != nullptr, "deep file survives defrag");
        check(a.find_file(L"sub/one.txt") == nullptr, "deleted file stays deleted after defrag");
        const Pk2File* f = a.find_file(L"sub/deep/two.bin");
        if (f) {
            auto bytes = a.read_file(*f);
            check(std::string(bytes.begin(), bytes.end()) == c_deep, "deep content intact after defrag");
        }
    }

    // 6) Import a disk folder tree.
    {
        std::wstring src = dir + L"\\src\\Particles"; // folder name "Particles"
        write_disk_file(src + L"\\a.txt", "aaa");
        write_disk_file(src + L"\\nested\\b.txt", "bbbbb");
        Pk2Archive a;
        a.open(pk2, key);
        std::uint64_t n = a.import_folder(src, L"imported");
        a.reload();
        check(n == 2, "import_folder imported 2 files");
        check(a.find_file(L"imported/a.txt") != nullptr, "imported/a.txt present");
        check(a.find_file(L"imported/nested/b.txt") != nullptr, "imported/nested/b.txt present");
    }

    // 7) No bloat: replacing / deleting reuses space, the file never grows needlessly.
    {
        std::wstring p = dir + L"\\Bloat.pk2";
        Pk2Archive a;
        a.create(p, key);
        check(a.file_size() == kHeaderSize + kBlockSize, "fresh archive is header + one block, no padding");
        auto blob = [](std::size_t n, char c) { return std::vector<std::uint8_t>(n, static_cast<std::uint8_t>(c)); };
        a.add_file(L"big.bin", blob(100'000, 'a'));
        a.add_file(L"tail.bin", blob(10, 't'));
        const std::uint64_t base = a.file_size();
        for (int i = 0; i < 50; ++i)   // shrink / grow back, over and over
            a.add_file(L"big.bin", blob(i % 2 ? 100'000 : 40'000, static_cast<char>('b' + i % 20)));
        check(a.file_size() <= base, "50 replacements of one file do not grow the archive");
        a.add_file(L"big.bin", blob(150'000, 'z'));     // must move: tail.bin sits right after it
        const std::uint64_t grown = a.file_size();
        a.add_file(L"small.bin", blob(90'000, 's'));    // fits the hole big.bin left
        check(a.file_size() == grown, "a new file reuses the hole of a moved one");
        a.reload();
        const Pk2File* big = a.find_file(L"big.bin");
        check(big && big->size == 150'000 && a.read_file(*big) == blob(150'000, 'z'), "moved file content intact");

        // Many files in one folder (several chained blocks), then delete all but a few.
        for (int i = 0; i < 95; ++i)
            a.add_file(L"many/f" + std::to_wstring(i) + L".txt", blob(1000 + i, static_cast<char>('A' + i % 26)));
        const std::uint64_t full = a.file_size();
        for (int i = 0; i < 95; ++i)
            if (i % 19 != 0)
                a.remove_file(L"many/f" + std::to_wstring(i) + L".txt");
        a.reload();
        bool kept = true;
        for (int i = 0; i < 95; i += 19) {
            const Pk2File* f = a.find_file(L"many/f" + std::to_wstring(i) + L".txt");
            kept = kept && f && a.read_file(*f) == blob(1000 + i, static_cast<char>('A' + i % 26));
        }
        check(kept, "files behind emptied index blocks stay reachable");
        a.remove_folder(L"many");
        a.remove_file(L"small.bin");
        check(a.file_size() < full, "deleting gives space back");
        for (int i = 0; i < 95; ++i)
            a.add_file(L"again/f" + std::to_wstring(i) + L".txt", blob(1000, 'q'));
        check(a.file_size() <= full, "re-adding the same amount reuses the freed space");

        // 8) Compaction: not one dead byte, every file intact.
        const SpaceInfo before = a.space();
        const std::uint64_t compacted = a.defragment();
        const SpaceInfo after = a.space();
        std::println("  compact: {} -> {} bytes (predicted {})", before.file_size, compacted, before.compact_size);
        check(compacted == before.compact_size, "compacted size is exactly the predicted size");
        check(after.free == 0 && after.reclaimable() == 0, "compacted archive has no free byte");
        check(compacted == kHeaderSize + after.blocks * kBlockSize + after.data, "size = header + index + data");
        a.reload();
        big = a.find_file(L"big.bin");
        const Pk2File* t = a.find_file(L"tail.bin");
        bool again = true;
        for (int i = 0; i < 95; ++i) {
            const Pk2File* f = a.find_file(L"again/f" + std::to_wstring(i) + L".txt");
            again = again && f && a.read_file(*f) == blob(1000, 'q');
        }
        check(big && a.read_file(*big) == blob(150'000, 'z') && t && a.read_file(*t) == blob(10, 't') && again,
              "every file intact after compaction");
        check(a.find_file(L"small.bin") == nullptr && a.find_folder(L"many") == nullptr, "deleted entries stay deleted");
        a.add_file(L"after.bin", blob(5, 'x'));
        a.reload();
        check(a.find_file(L"after.bin") != nullptr, "compacted archive stays writable");
    }

    // 9) Batch add from disk (parallel read-ahead).
    {
        std::vector<DiskFile> batch;
        for (int i = 0; i < 40; ++i) {
            std::wstring f = dir + L"\\batch\\d" + std::to_wstring(i % 4) + L"\\file" + std::to_wstring(i) + L".dat";
            write_disk_file(f, std::string(static_cast<std::size_t>(100 + i * 37), static_cast<char>('a' + i % 26)));
            batch.push_back({f, L"Batch/D" + std::to_wstring(i % 4) + L"/File" + std::to_wstring(i) + L".dat"});
        }
        Pk2Archive a;
        a.open(pk2, key);
        std::uint64_t n = a.add_disk_files(batch);
        a.reload();
        bool ok = n == batch.size();
        for (int i = 0; i < 40 && ok; ++i) {
            const Pk2File* f = a.find_file(L"batch/d" + std::to_wstring(i % 4) + L"/file" + std::to_wstring(i) + L".dat");
            ok = f && f->name == L"File" + std::to_wstring(i) + L".dat" &&
                 a.read_file(*f) == std::vector<std::uint8_t>(static_cast<std::size_t>(100 + i * 37), static_cast<std::uint8_t>('a' + i % 26));
        }
        check(ok, "40 disk files added in order with their case and content");
    }

    std::println("");
}

// Compacts a COPY of a real archive and compares every file with the original.
//   pk2test --compact <key> <copy.pk2> <original.pk2>
int run_compact(const std::string& key, const std::wstring& copy, const std::wstring& original) {
    try {
        Pk2Archive a;
        a.open(copy, key);
        const SpaceInfo s = a.space();
        std::println("{}: {} bytes, data {}, index {}, free {}, compact -> {} (saves {})", u8(copy), s.file_size, s.data,
                     s.index, s.free, s.compact_size, s.reclaimable());
        const ULONGLONG t0 = ::GetTickCount64();
        const std::uint64_t after = a.defragment();
        const double secs = static_cast<double>(::GetTickCount64() - t0) / 1000.0;
        std::println("compacted in {:.2f}s: {} bytes (free {})", secs, after, a.space().free);
        if (after != s.compact_size || a.space().free != 0) {
            std::println("FAIL: size {} expected {}", after, s.compact_size);
            return 1;
        }
        Pk2Archive o;
        o.open(original, key);
        std::uint64_t files = 0, bad = 0;
        std::vector<const Pk2Folder*> st{&o.root()};
        while (!st.empty()) {
            const Pk2Folder* f = st.back();
            st.pop_back();
            for (const Pk2File& file : f->files) {
                ++files;
                const Pk2File* c = a.find_file(file.path);
                if (!c || c->name != file.name || c->size != file.size || c->mtime != file.mtime ||
                    a.read_file(*c) != o.read_file(file)) {
                    if (++bad <= 5) std::println("  MISMATCH {}", u8(file.path));
                }
            }
            for (const Pk2Folder& c : f->folders)
                st.push_back(&c);
        }
        std::println("{} files compared, {} mismatches: {}", files, bad, bad ? "FAIL" : "OK");
        return bad ? 1 : 0;
    } catch (const std::exception& ex) {
        std::println("compact error: {}", ex.what());
        return 1;
    }
}

void run_real(const std::wstring& path, const std::string& key) {
    std::println("== Real pk2: {} ==", u8(path));
    try {
        Pk2Archive a;
        a.open(path, key);
        std::uint64_t files = count_files(a.root());
        std::uint64_t folders = count_folders(a.root());
        std::println("  opened OK — {} files, {} folders, {} bytes", files, folders, a.file_size());
        // Read one small file to confirm decode works.
        const Pk2File* smallest = nullptr;
        std::vector<const Pk2Folder*> stack{&a.root()};
        while (!stack.empty()) {
            const Pk2Folder* f = stack.back();
            stack.pop_back();
            for (const auto& file : f->files)
                if (file.size > 0 && (!smallest || file.size < smallest->size))
                    smallest = &file;
            for (const auto& c : f->folders)
                stack.push_back(&c);
        }
        if (smallest) {
            auto bytes = a.read_file(*smallest);
            std::println("  read '{}' ({} bytes) OK", u8(smallest->name), bytes.size());
            check(bytes.size() == smallest->size, "real file read size matches entry");
        }
        check(files > 0, "real pk2 has files");
    } catch (const std::exception& ex) {
        std::println("  [FAIL] exception: {}", ex.what());
        ++g_fail;
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // Timed real extract:  pk2test --extract <key> <file.pk2> <destdir>
    if (argc >= 5 && std::wstring(argv[1]) == L"--extract") {
        std::string key = wide_to_utf8(argv[2]);
        std::wstring pk2 = argv[3];
        std::wstring dest = argv[4];
        try {
            Pk2Archive a;
            a.open(pk2, key);
            std::uint64_t files = count_files(a.root());
            std::println("Extracting {} files from {} -> {}", files, u8(pk2), u8(dest));
            ULONGLONG t0 = ::GetTickCount64();
            std::uint64_t last = 0;
            a.extract_all(dest, [&](const Progress& p) {
                if (p.done - last >= 2000 || p.done == p.total) {
                    last = p.done;
                    double pct = p.total ? (100.0 * static_cast<double>(p.done) / static_cast<double>(p.total)) : 100.0;
                    std::println("  {:.1f}%  {}/{}", pct, p.done, p.total);
                }
            });
            double secs = static_cast<double>(::GetTickCount64() - t0) / 1000.0;
            std::println("Extracted in {:.2f}s ({:.0f} files/s)", secs,
                         secs > 0 ? static_cast<double>(files) / secs : 0.0);
        } catch (const std::exception& ex) {
            std::println("extract error: {}", ex.what());
            return 1;
        }
        return 0;
    }

    // Decode-test every/first .ddj texture:  pk2test --ddj <key> <file.pk2> [count]
    if (argc >= 4 && std::wstring(argv[1]) == L"--ddj") {
        std::string key = wide_to_utf8(argv[2]);
        std::wstring pk2 = argv[3];
        int want = (argc >= 5) ? _wtoi(argv[4]) : 5;
        try {
            Pk2Archive a;
            a.open(pk2, key);
            int tested = 0, ok = 0;
            std::vector<const Pk2Folder*> st{&a.root()};
            while (!st.empty() && tested < want) {
                const Pk2Folder* f = st.back(); st.pop_back();
                for (const auto& file : f->files) {
                    if (classify(file.name).category != FileCategory::Texture) continue;
                    auto bytes = a.read_file(file);
                    auto img = decode_dds(bytes.data(), bytes.size());
                    if (img) {
                        std::println("  OK  {}  {}x{}  {}", u8(file.name), img->width, img->height, img->format);
                        ++ok;
                    } else {
                        std::println("  FAIL decode  {} ({} bytes)", u8(file.name), bytes.size());
                    }
                    if (++tested >= want) break;
                }
                for (const auto& c : f->folders) st.push_back(&c);
            }
            std::println("DDJ decode: {}/{} ok", ok, tested);
            return ok == tested && tested > 0 ? 0 : 1;
        } catch (const std::exception& ex) {
            std::println("ddj error: {}", ex.what());
            return 1;
        }
    }

    // Resolve textures for .bsr objects:  pk2test --bsr <key> <file.pk2> [count]
    if (argc >= 4 && std::wstring(argv[1]) == L"--bsr") {
        std::string key = wide_to_utf8(argv[2]);
        std::wstring pk2 = argv[3];
        int want = (argc >= 5) ? _wtoi(argv[4]) : 6;
        try {
            Pk2Archive a; a.open(pk2, key);
            auto resolve = [&](const std::wstring& ref, const std::wstring& dir) -> const Pk2File* {
                std::wstring r = ref; for (auto& c : r) if (c == L'\\') c = L'/';
                while (!r.empty() && r.front() == L'/') r.erase(r.begin());
                if (auto* f = a.find_file(r)) return f;
                if (!dir.empty()) { if (auto* f = a.find_file(dir + L"/" + r)) return f; }
                std::size_t sl = r.find_last_of(L'/'); std::wstring bn = sl == std::wstring::npos ? r : r.substr(sl + 1);
                if (!dir.empty()) { if (auto* f = a.find_file(dir + L"/" + bn)) return f; }
                std::size_t fs = r.find(L'/'); if (fs != std::wstring::npos) { if (auto* f = a.find_file(r.substr(fs + 1))) return f; }
                return nullptr;
            };
            int tested = 0, ok = 0;
            std::vector<const Pk2Folder*> st{&a.root()};
            while (!st.empty() && tested < want) {
                const Pk2Folder* f = st.back(); st.pop_back();
                for (const auto& file : f->files) {
                    if (classify(file.name).category != FileCategory::Resource) continue;
                    ++tested;
                    std::wstring dir = file.path.substr(0, file.path.find_last_of(L'/'));
                    auto bytes = a.read_file(file);
                    JmxBsr bsr = parse_bsr(bytes.data(), bytes.size());
                    bool got = false; std::wstring texname;
                    for (const auto& mp : bsr.material_paths) {
                        const Pk2File* bmtF = resolve(mp, dir); if (!bmtF) continue;
                        std::wstring bdir = bmtF->path.substr(0, bmtF->path.find_last_of(L'/'));
                        auto bb = a.read_file(*bmtF); JmxBmt bmt = parse_bmt(bb.data(), bb.size());
                        for (const auto& m : bmt.materials) { if (m.diffuse.empty()) continue;
                            const Pk2File* tx = resolve(m.diffuse, m.absolute ? std::wstring() : bdir); if (!tx) continue;
                            auto tb = a.read_file(*tx); if (decode_dds(tb.data(), tb.size())) { got = true; texname = tx->name; break; } }
                        if (got) break;
                    }
                    // Skeleton + animation + skinning check.
                    int bones = 0, animBones = 0; std::uint32_t animDur = 0; bool meshSkinned = false;
                    if (!bsr.skeleton_path.empty()) { if (auto* sk = resolve(bsr.skeleton_path, dir)) { auto sb = a.read_file(*sk); auto skel = parse_bsk(sb.data(), sb.size()); bones = static_cast<int>(skel.bones.size()); } }
                    if (!bsr.animation_paths.empty()) { if (auto* an = resolve(bsr.animation_paths[0], dir)) { auto ab = a.read_file(*an); auto anim = parse_ban(ab.data(), ab.size()); animBones = static_cast<int>(anim.bones.size()); animDur = anim.duration; } }
                    for (const auto& mp : bsr.mesh_paths) { if (auto* mf = resolve(mp, dir)) { auto mb = a.read_file(*mf); auto jm = parse_bms(mb.data(), mb.size()); if (jm.skinned) { meshSkinned = true; break; } } }
                    std::string stat = got ? ("tex=" + u8(texname)) : std::string("NO TEXTURE");
                    std::println("  {}  meshes={} mats={} bones={} animBones={} dur={}ms skin={}  {}", u8(file.name), bsr.mesh_paths.size(), bsr.material_paths.size(), bones, animBones, animDur, meshSkinned, stat);
                    if (got) ++ok;
                    if (tested >= want) break;
                }
                for (const auto& c : f->folders) st.push_back(&c);
            }
            std::println("BSR texture resolve: {}/{} ok", ok, tested);
            return 0;
        } catch (const std::exception& ex) { std::println("bsr error: {}", ex.what()); return 1; }
    }

    if (argc >= 5 && std::wstring(argv[1]) == L"--compact")
        return run_compact(wide_to_utf8(argv[2]), argv[3], argv[4]);

    // Data layout of a real archive (read-only):  pk2test --layout <key> <file.pk2>
    if (argc >= 4 && std::wstring(argv[1]) == L"--layout") {
        std::string key = wide_to_utf8(argv[2]);
        Pk2Archive a; a.open(argv[3], key);
        struct R { std::uint64_t off; std::uint32_t size; };
        std::vector<R> rs; std::uint64_t zero = 0, zero_off0 = 0, aligned = 0, folders = 0;
        std::vector<const Pk2Folder*> st{&a.root()};
        while (!st.empty()) { const Pk2Folder* f = st.back(); st.pop_back(); ++folders;
            for (const auto& file : f->files) {
                if (file.size == 0) { ++zero; if (file.data_offset == 0) ++zero_off0; continue; }
                if (file.data_offset % 4096 == 0) ++aligned;
                rs.push_back({file.data_offset, file.size});
            }
            for (const auto& c : f->folders) st.push_back(&c);
        }
        std::sort(rs.begin(), rs.end(), [](const R& x, const R& y) { return x.off < y.off; });
        std::uint64_t live = 0, holes = 0, hole_bytes = 0, shared = 0, overlaps = 0, cursor = 0;
        for (std::size_t i = 0; i < rs.size(); ++i) {
            if (i && rs[i].off == rs[i - 1].off) { ++shared; continue; }
            if (rs[i].off < cursor) ++overlaps;
            else if (rs[i].off > cursor) { ++holes; hole_bytes += rs[i].off - cursor; }
            cursor = std::max<std::uint64_t>(cursor, rs[i].off + rs[i].size);
            live += rs[i].size;
        }
        std::println("{}: size={} files={} folders={} zero-size={} (offset 0: {}) 4k-aligned={} live={} gaps={} gap-bytes={} (incl. index) shared={} overlaps={} tail={}",
                     u8(argv[3]), a.file_size(), rs.size() + zero, folders, zero, zero_off0, aligned, live, holes, hole_bytes,
                     shared, overlaps, a.file_size() > cursor ? a.file_size() - cursor : 0);
        return 0;
    }

    // Probe specific config formats:  pk2test --probe <key> <file.pk2>
    if (argc >= 4 && std::wstring(argv[1]) == L"--probe") {
        std::string key = wide_to_utf8(argv[2]);
        std::wstring pk2 = argv[3];
        Pk2Archive a; a.open(pk2, key);
        const Pk2File* svt = nullptr; const Pk2File* silk = nullptr; const Pk2File* twodt = nullptr;
        std::vector<const Pk2Folder*> st{&a.root()};
        while (!st.empty()) { const Pk2Folder* f = st.back(); st.pop_back();
            for (const auto& file : f->files) {
                std::wstring ln = to_lower(file.name);
                if (ln == L"sv.t" && !svt) svt = &file;
                if (ln == L"silk.dat" && !silk) silk = &file;
                if (ln.size() > 4 && ln.substr(ln.size() - 4) == L".2dt" && !twodt) twodt = &file;
            }
            for (const auto& c : f->folders) st.push_back(&c);
        }
        auto hex = [](const std::vector<std::uint8_t>& b, std::size_t n) {
            std::string s; for (std::size_t i = 0; i < n && i < b.size(); ++i) { char t[4]; std::snprintf(t, 4, "%02X ", b[i]); s += t; } return s; };
        if (svt) {
            auto b = a.read_file(*svt);
            std::uint32_t len = 0; if (b.size() >= 4) std::memcpy(&len, b.data(), 4);
            std::println("SV.T size={} len={} head={}", b.size(), len, hex(b, 16));
            if (b.size() >= 12) {
                auto tryk = [&](const char* label, std::span<const std::uint8_t> key, std::span<const std::uint8_t> ct) {
                    Blowfish bf; bf.initialize(key); auto d = bf.decode(ct);
                    std::string asc; for (auto c : d) asc += (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '.';
                    std::println("  key[{}] -> hex={} ascii=\"{}\"", label, hex(d, d.size()), asc);
                };
                const std::uint8_t k8[8] = {'S','I','L','K','R','O','A','D'};
                const char* k15 = "SILKROADVERSION";
                tryk("SILKROAD", {k8, 8}, {b.data() + 4, 8});
                tryk("SILKROADVERSION", {reinterpret_cast<const std::uint8_t*>(k15), 15}, {b.data() + 4, 8});
            }
        } else std::println("SV.T not found");
        if (silk) { auto b = a.read_file(*silk);
            std::uint32_t v=0,w=0,h=0; if (b.size()>=16){std::memcpy(&v,b.data(),4);std::memcpy(&w,b.data()+8,4);std::memcpy(&h,b.data()+12,4);}
            std::println("silk.dat size={} ver={} w={} h={} head={}", b.size(), v, w, h, hex(b, 32)); }
        else std::println("silk.dat not found");
        if (twodt) { auto b = a.read_file(*twodt);
            std::string head(reinterpret_cast<const char*>(b.data()), std::min<std::size_t>(b.size(), 120));
            std::println(".2dt {} size={} head-ascii=\"{}\"", u8(twodt->name), b.size(), head); }
        else std::println(".2dt not found");
        return 0;
    }

    run_synthetic();

    // Optional: real pk2 files.  Usage: pk2test [key] <file1.pk2> [file2.pk2 ...]
    std::string key = "169841";
    int start = 1;
    if (argc > 1) {
        std::wstring a1 = argv[1];
        if (a1.find(L".pk2") == std::wstring::npos) {
            key = wide_to_utf8(a1);
            start = 2;
        }
    }
    for (int i = start; i < argc; ++i)
        run_real(argv[i], key);

    std::println("==== {} passed, {} failed ====", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
