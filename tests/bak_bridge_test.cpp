// Console check of bak_bridge against real archives (opened read-only).
//   bakbridge_test [archive ...] [--key K] [--find substring] [--extra] [--write SRC.pk2]
// --extra also runs avatar_set() and the (slow) recover_asset_cipher().
// --write copies SRC.pk2 to %TEMP%\bakw\ and runs the write tests on that COPY
//   (PK2 add/rename/remove/folder/import/compact/reopen, then FolderArchive on
//   files extracted from the copy). The source is only ever read.
// Defaults: Data.pk2 + Media.pk2 + Particles.pk2 of %SRO_CLIENT_DIR% (see test_client.hpp), key 169841.
#include "bak_bridge.hpp"
#include "test_client.hpp"

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::wstring FirstLine(const std::wstring& s) { return s.substr(0, s.find(L'\n')); }

bool EndsWith(const std::wstring& s, const wchar_t* ext) {
    const std::wstring e(ext);
    if (s.size() < e.size()) return false;
    for (size_t i = 0; i < e.size(); ++i)
        if (towlower(s[s.size() - e.size() + i]) != e[i]) return false;
    return true;
}

// Loads `path`; returns the plate-quad count at t=0.5 (-1 on failure). Prints
// the details only when `print` is set.
int TestEffect(const bakb::Archive& ar, const std::wstring& path, bool print) {
    std::vector<std::uint8_t> bytes;
    std::wstring err;
    if (!ar.read(path, bytes, err)) { std::wprintf(L"  efp read FAIL %ls: %ls\n", path.c_str(), err.c_str()); return -1; }
    auto fx = bakb::Effect::load(bytes, err, &ar, path);
    if (!fx) { if (print) std::wprintf(L"  Effect::load %ls -> %ls\n", path.c_str(), err.c_str()); return -1; }
    std::vector<bakb::FxBatch> batches;
    fx->sample(0.5f, bakb::FxView{}, batches);
    int quads = 0;
    size_t tris = 0;
    for (const bakb::FxBatch& b : batches) {
        if (b.vertices.size() == 4 && b.indices.size() == 6) ++quads;
        tris += b.indices.size() / 3;
    }
    if (!print) return quads;

    const bakb::FxSceneInfo& info = fx->info();
    std::wprintf(L"  Effect %ls: emitters=%zu textures=%zu duration=%.3fs radius=%.2f camDist=%.2f\n",
                 path.c_str(), info.emitters, info.textures.size(), info.duration, info.radius,
                 info.camera_distance);
    size_t readable = 0;
    for (const std::wstring& t : info.textures) {
        std::vector<std::uint8_t> tb;
        if (ar.read(t, tb, err, /*siblings*/ true) && !tb.empty()) ++readable;
    }
    std::wprintf(L"  textures readable (with siblings): %zu/%zu%ls%ls\n", readable, info.textures.size(),
                 info.textures.empty() ? L"" : L"  first=", info.textures.empty() ? L"" : info.textures[0].c_str());
    std::wprintf(L"  sample(t=0.5): batches=%zu quads=%d triangles=%zu\n", batches.size(), quads, tris);
    if (!batches.empty()) {
        const bakb::FxBatch& b = batches.front();
        std::wprintf(L"  batch[0]: v0=(%.2f %.2f %.2f) tex=%d color=(%.2f %.2f %.2f a%.2f) blend=%u->%u\n",
                     b.vertices[0].pos[0], b.vertices[0].pos[1], b.vertices[0].pos[2], b.texture,
                     b.color[0], b.color[1], b.color[2], b.color[3], b.src_blend, b.dst_blend);
    }
    return quads;
}

// build_preview on one asset: format, image size, frames, first 3 summary lines.
int PreviewCheck(const bakb::Archive& ar, const std::wstring& path) {
    std::vector<std::uint8_t> bytes;
    std::wstring err;
    if (!ar.read(path, bytes, err)) { std::wprintf(L"  preview read FAIL %ls: %ls\n", path.c_str(), err.c_str()); return 1; }
    bakb::Preview pv;
    if (!bakb::build_preview(&ar, path, bytes, 640, 480, pv, err)) {
        std::wprintf(L"  build_preview FAIL %ls: %ls\n", path.c_str(), err.c_str());
        return 1;
    }
    // Distinct colours on a sparse grid: a blank / flat render scores 1-2.
    std::vector<std::uint32_t> colours;
    for (std::uint32_t y = 0; y < pv.image.height; y += 8)
        for (std::uint32_t x = 0; x < pv.image.width; x += 8) {
            const std::uint8_t* px = &pv.image.bgra[(static_cast<size_t>(y) * pv.image.width + x) * 4];
            const std::uint32_t c = px[0] | (px[1] << 8) | (px[2] << 16);
            if (std::find(colours.begin(), colours.end(), c) == colours.end()) colours.push_back(c);
        }
    const size_t lines = static_cast<size_t>(std::count(pv.summary.begin(), pv.summary.end(), L'\n'));
    std::wprintf(L"  build_preview(%ls): format='%ls' image=%ux%u colours=%zu is_3d=%d frames=%zu frame_ms=%.0f "
                 L"summary=%zu lines text=%zu chars\n",
                 path.c_str(), pv.format.c_str(), pv.image.width, pv.image.height, colours.size(),
                 pv.is_3d ? 1 : 0, pv.frames.size(), pv.frame_ms, lines, pv.text.size());
    size_t pos = 0;
    for (int line = 0; line < 3 && pos < pv.summary.size();) {
        const size_t nl = pv.summary.find(L'\n', pos);
        const std::wstring s = pv.summary.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = (nl == std::wstring::npos) ? pv.summary.size() : nl + 1;
        if (s.empty()) continue;
        std::wprintf(L"    | %ls\n", s.c_str());
        ++line;
    }
    return 0;
}

int TestArchive(const std::wstring& path, const std::string& key, const std::wstring& find, bool extra) {
    std::wprintf(L"== %ls\n", path.c_str());
    std::wstring err;
    auto ar = bakb::Archive::open(path, key, err);
    if (!ar) { std::wprintf(L"  open FAIL: %ls\n", err.c_str()); return 1; }
    std::wprintf(L"  -- protection_report --\n%ls", ar->protection_report().c_str());

    const auto& entries = ar->entries();
    if (!find.empty()) {   // diagnostic: list paths containing `find` (case-sensitive)
        for (const bakb::Entry& e : entries)
            if (e.path.find(find) != std::wstring::npos) std::wprintf(L"  found %ls\n", e.path.c_str());
        return 0;
    }
    size_t files = 0, folders = 0;
    for (const bakb::Entry& e : entries) (e.folder ? folders : files)++;
    std::wprintf(L"  kind=%ls can_write=%d entries=%zu (files %zu, folders %zu) skipped=%llu\n",
                 ar->kind().c_str(), ar->can_write() ? 1 : 0, entries.size(), files, folders,
                 static_cast<unsigned long long>(ar->skipped_entries()));

    for (const bakb::Entry& e : entries)
        if (!e.folder && e.path.find(L'\\') != std::wstring::npos) {
            std::wprintf(L"  nested sample: %ls (%llu bytes)\n", e.path.c_str(), static_cast<unsigned long long>(e.size));
            break;
        }

    int fails = 0, shown = 0;
    std::wstring bsr, avatar, nvm, ddj;
    std::vector<std::wstring> efps;
    for (const bakb::Entry& e : entries) {
        if (e.folder) continue;
        if (shown < 3) {
            std::vector<std::uint8_t> b;
            if (ar->read(e.path, b, err)) {
                std::wprintf(L"  read %ls -> %zu bytes (entry %llu) [%ls]\n", e.path.c_str(), b.size(),
                             static_cast<unsigned long long>(e.size), FirstLine(bakb::describe(e.path, b)).c_str());
            } else {
                std::wprintf(L"  read FAIL %ls: %ls\n", e.path.c_str(), err.c_str());
                ++fails;
            }
            ++shown;
        }
        if (EndsWith(e.path, L".efp") && efps.size() < 40) efps.push_back(e.path);
        if (bsr.empty() && EndsWith(e.path, L".bsr")) bsr = e.path;
        if (avatar.empty() && EndsWith(e.path, L"_dress.bsr") && e.path.find(L"avatar") != std::wstring::npos)
            avatar = e.path;
        if (nvm.empty() && EndsWith(e.path, L".nvm")) nvm = e.path;
        if (ddj.empty() && EndsWith(e.path, L".ddj")) ddj = e.path;
    }

    // BAK preview engine on one asset of each kind this archive has.
    for (const std::wstring* p : { &bsr, &nvm, &ddj })
        if (!p->empty()) fails += PreviewCheck(*ar, *p);
    if (!efps.empty()) fails += PreviewCheck(*ar, efps.front());

    if (extra) {
        if (!avatar.empty()) {
            std::wstring family;
            const auto pieces = ar->avatar_set(avatar, family);
            std::wprintf(L"  avatar_set(%ls): family='%ls' pieces=%zu%ls%ls\n", avatar.c_str(), family.c_str(),
                         pieces.size(), pieces.empty() ? L"" : L" first=", pieces.empty() ? L"" : pieces[0].c_str());
        }
        std::wstring how;
        const bool ok = ar->recover_asset_cipher(how);
        std::wprintf(L"  recover_asset_cipher: %d (%ls)\n", ok ? 1 : 0, how.c_str());
    }

    if (!bsr.empty()) {
        std::vector<std::uint8_t> b;
        if (ar->read(bsr, b, err)) {
            std::wprintf(L"  describe(%ls):\n%ls\n", bsr.c_str(), bakb::describe(bsr, b).c_str());
            std::vector<std::wstring> missing;
            const auto deps = ar->dependencies(bsr, missing);
            std::wprintf(L"  dependencies: %zu resolved, %zu missing\n", deps.size(), missing.size());
        } else {
            std::wprintf(L"  read FAIL %ls: %ls\n", bsr.c_str(), err.c_str());
            ++fails;
        }
    }
    if (!efps.empty()) {
        std::vector<std::uint8_t> b;
        if (ar->read(efps.front(), b, err))
            std::wprintf(L"  describe(%ls):\n%ls\n", efps.front().c_str(), bakb::describe(efps.front(), b).c_str());
        // First effect that draws billboard plates at t=0.5; else the first that loads.
        std::wstring pick, fallback;
        size_t tried = 0;
        for (const std::wstring& p : efps) {
            ++tried;
            const int q = TestEffect(*ar, p, false);
            if (q < 0) continue;
            if (fallback.empty()) fallback = p;
            if (q > 0) { pick = p; break; }
        }
        if (pick.empty()) pick = fallback;
        std::wprintf(L"  effects tried: %zu\n", tried);
        if (pick.empty() || TestEffect(*ar, pick, true) < 0) ++fails;
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Write tests (on copies only)
// ---------------------------------------------------------------------------
int g_checks = 0, g_failed = 0;

void Check(bool ok, const std::wstring& what, const std::wstring& detail = {}) {
    ++g_checks;
    if (!ok) ++g_failed;
    std::wprintf(L"  [%ls] %ls%ls%ls\n", ok ? L"OK  " : L"FAIL", what.c_str(), detail.empty() ? L"" : L" - ",
                 detail.c_str());
}

std::vector<std::uint8_t> Pattern(size_t n, std::uint8_t seed) {
    std::vector<std::uint8_t> v(n);
    std::uint32_t x = 0x9E3779B9u ^ seed;
    for (auto& b : v) { x = x * 1664525u + 1013904223u; b = static_cast<std::uint8_t>(x >> 24); }
    return v;
}

std::vector<std::uint8_t> Bytes(const char* s) { return { s, s + std::char_traits<char>::length(s) }; }

const bakb::Entry* FindEntry(const bakb::Archive& ar, const std::wstring& path) {
    for (const bakb::Entry& e : ar.entries()) {
        if (e.path.size() != path.size()) continue;
        bool eq = true;
        for (size_t i = 0; i < path.size() && eq; ++i) eq = towlower(e.path[i]) == towlower(path[i]);
        if (eq) return &e;
    }
    return nullptr;
}

bool ReadEquals(const bakb::Archive& ar, const std::wstring& path, const std::vector<std::uint8_t>& want) {
    std::vector<std::uint8_t> got;
    std::wstring err;
    return ar.read(path, got, err) && got == want;
}

void WriteDisk(const fs::path& p, const std::vector<std::uint8_t>& b) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()),
                                             static_cast<std::streamsize>(b.size()));
}

int TestWritePk2(const fs::path& copy, const fs::path& source, const std::string& key) {
    std::wprintf(L"== WRITE PK2 (copy) %ls\n", copy.c_str());
    std::wstring err;
    const auto small = Bytes("bakw small file - fits the SailSafe header gap\r\n");
    const auto big   = Pattern(200000, 7);
    const auto big2  = Pattern(150000, 9);
    const auto imp1  = Pattern(4096, 1);
    const auto imp2  = Pattern(70000, 2);
    std::wstring ddj;                       // an untouched original asset, compared at the end
    std::vector<std::uint8_t> ddjBytes;
    size_t baseEntries = 0;

    // Disk folder for import_folder: imp\a.bin + imp\sub\b.bin
    const fs::path impDir = copy.parent_path() / L"imp";
    fs::remove_all(impDir);
    WriteDisk(impDir / L"a.bin", imp1);
    WriteDisk(impDir / L"sub" / L"b.bin", imp2);

    {
        auto ro = bakb::Archive::open(copy.wstring(), key, err);
        Check(ro != nullptr, L"open copy read-only", err);
        if (!ro) return 1;
        Check(!ro->can_write(), L"read-only open: can_write()==false");
        Check(!ro->add_file(L"x\\y.txt", small, err), L"read-only open refuses add_file", err);
    }

    auto ar = bakb::Archive::open(copy.wstring(), key, err, false, /*writable*/ true);
    Check(ar != nullptr, L"open copy writable", err);
    if (!ar) return 1;
    Check(ar->can_write(), L"can_write()==true");
    std::wprintf(L"  -- protection_report (writable copy) --\n%ls", ar->protection_report().c_str());

    baseEntries = ar->entries().size();
    for (const bakb::Entry& e : ar->entries())
        if (!e.folder && EndsWith(e.path, L".ddj") && e.size > 0) { ddj = e.path; break; }
    if (!ddj.empty()) ar->read(ddj, ddjBytes, err);
    std::wprintf(L"  base entries=%zu sample=%ls (%zu bytes)\n", baseEntries, ddj.c_str(), ddjBytes.size());

    // add (creates bakw_test\sub), read back
    Check(ar->add_file(L"bakw_test/sub/hello.txt", small, err), L"add_file bakw_test/sub/hello.txt", err);
    Check(FindEntry(*ar, L"bakw_test\\sub\\hello.txt") != nullptr, L"entries() lists the new file");
    Check(FindEntry(*ar, L"bakw_test\\sub") && FindEntry(*ar, L"bakw_test\\sub")->folder,
          L"missing parent folders were created");
    Check(ReadEquals(*ar, L"bakw_test\\sub\\hello.txt", small), L"read back equals");

    // rename
    Check(ar->rename(L"bakw_test\\sub\\hello.txt", L"renamed.txt", err), L"rename -> renamed.txt", err);
    Check(!FindEntry(*ar, L"bakw_test\\sub\\hello.txt") && FindEntry(*ar, L"bakw_test\\sub\\renamed.txt"),
          L"entries() reflects the rename");
    Check(ReadEquals(*ar, L"bakw_test\\sub\\renamed.txt", small), L"renamed file reads back");

    // remove
    Check(ar->remove(L"bakw_test\\sub\\renamed.txt", err), L"remove renamed.txt", err);
    Check(!FindEntry(*ar, L"bakw_test\\sub\\renamed.txt"), L"entries() no longer lists it");
    std::vector<std::uint8_t> tmp;
    Check(!ar->read(L"bakw_test\\sub\\renamed.txt", tmp, err), L"read of removed file fails");

    // folder
    Check(ar->add_folder(L"bakw_test\\emptydir", err), L"add_folder bakw_test\\emptydir", err);
    Check(ar->add_folder(L"bakw_test\\emptydir", err), L"add_folder again is idempotent", err);
    Check(FindEntry(*ar, L"bakw_test\\emptydir") && FindEntry(*ar, L"bakw_test\\emptydir")->folder,
          L"entries() lists the folder");

    // root-level small file + overwrite of a big one
    Check(ar->add_file(L"bakw_root.txt", small, err), L"add_file bakw_root.txt (root)", err);
    Check(ar->add_file(L"bakw_test\\big.bin", big, err), L"add_file big.bin (200000 B)", err);
    Check(ar->add_file(L"bakw_test\\big.bin", big2, err), L"overwrite big.bin (150000 B)", err);
    Check(ReadEquals(*ar, L"bakw_test\\big.bin", big2), L"overwritten content reads back");

    // import_folder
    std::uint64_t lastDone = 0, lastTotal = 0;
    Check(ar->import_folder(impDir.wstring(), L"bakw_test\\imported",
                            [&](std::uint64_t d, std::uint64_t t, const std::wstring&) {
                                lastDone = d; lastTotal = t; return true; }, err),
          L"import_folder imp -> bakw_test\\imported", err);
    Check(lastTotal == imp1.size() + imp2.size() && lastDone == lastTotal, L"import progress counts bytes",
          std::to_wstring(lastDone) + L"/" + std::to_wstring(lastTotal));
    Check(ReadEquals(*ar, L"bakw_test\\imported\\a.bin", imp1) &&
          ReadEquals(*ar, L"bakw_test\\imported\\sub\\b.bin", imp2), L"imported files read back");
    Check(!ar->rename(L"bakw_test\\big.bin", L"bad\\name", err), L"rename rejects a separator", err);
    Check(!ar->remove(L"bakw_test\\nope.bin", err), L"remove of a missing path fails", err);
    Check(ar->flush(err), L"flush", err);

    // compact (keeps <copy>.bak)
    std::uint64_t saved = 0, prog = 0;
    Check(ar->compact(&saved, [&](std::uint64_t, std::uint64_t, const std::wstring&) { ++prog; return true; }, err),
          L"compact", err);
    std::wprintf(L"  compact: saved=%llu bytes, progress calls=%llu, .bak exists=%d\n",
                 static_cast<unsigned long long>(saved), static_cast<unsigned long long>(prog),
                 fs::exists(copy.wstring() + L".bak") ? 1 : 0);
    Check(ReadEquals(*ar, L"bakw_test\\big.bin", big2), L"after compact: big.bin still reads (same handle)");
    ar.reset();

    // reopen and verify
    auto re = bakb::Archive::open(copy.wstring(), key, err);
    Check(re != nullptr, L"reopen after compact", err);
    if (!re) return 1;
    Check(ReadEquals(*re, L"bakw_test\\big.bin", big2), L"reopen: big.bin");
    Check(ReadEquals(*re, L"bakw_root.txt", small), L"reopen: bakw_root.txt");
    Check(ReadEquals(*re, L"bakw_test\\imported\\a.bin", imp1) &&
          ReadEquals(*re, L"bakw_test\\imported\\sub\\b.bin", imp2), L"reopen: imported files");
    Check(FindEntry(*re, L"bakw_test\\emptydir") && FindEntry(*re, L"bakw_test\\emptydir")->folder,
          L"reopen: emptydir");
    Check(!FindEntry(*re, L"bakw_test\\sub\\renamed.txt") && !FindEntry(*re, L"bakw_test\\sub\\hello.txt"),
          L"reopen: removed file stays gone");
    // + bakw_test, sub, emptydir, imported, imported\sub (5 folders) + root.txt, big.bin, a.bin, b.bin (4 files)
    Check(re->entries().size() == baseEntries + 9, L"reopen: entry count = base + 9",
          std::to_wstring(re->entries().size()) + L" vs " + std::to_wstring(baseEntries + 9));
    if (!ddj.empty()) Check(ReadEquals(*re, ddj, ddjBytes), L"reopen: untouched original asset identical", ddj);

    // The original source must be unchanged: compare the same asset there.
    if (!ddj.empty()) {
        auto src = bakb::Archive::open(source.wstring(), key, err);
        Check(src && ReadEquals(*src, ddj, ddjBytes), L"source archive asset identical (read-only)");
    }
    return 0;
}

int TestWriteFolder(const fs::path& pk2Copy, const fs::path& folder, const std::string& key) {
    std::wprintf(L"== WRITE Folder %ls\n", folder.c_str());
    std::wstring err;
    fs::remove_all(folder);
    {   // extract a few files from the copy
        auto ar = bakb::Archive::open(pk2Copy.wstring(), key, err);
        Check(ar != nullptr, L"open copy for extraction", err);
        if (!ar) return 1;
        int n = 0;
        for (const bakb::Entry& e : ar->entries()) {
            if (e.folder || e.size == 0 || e.size > 4u << 20) continue;
            std::vector<std::uint8_t> b;
            if (!ar->read(e.path, b, err)) continue;
            WriteDisk(folder / e.path, b);
            if (++n == 6) break;
        }
        Check(n == 6, L"extracted 6 files to the folder", std::to_wstring(n));
    }
    auto fa = bakb::Archive::open(folder.wstring(), key, err, false, /*writable*/ true);
    Check(fa != nullptr, L"open folder writable", err);
    if (!fa) return 1;
    Check(fa->kind() == L"Folder" && fa->can_write(), L"kind Folder, can_write()==true");
    std::wprintf(L"  -- protection_report (folder) --\n%ls", fa->protection_report().c_str());
    const size_t base = fa->entries().size();
    const auto data = Pattern(5000, 3);
    Check(fa->add_file(L"new/deeper/x.bin", data, err), L"add_file new/deeper/x.bin", err);
    Check(fs::exists(folder / L"new" / L"deeper" / L"x.bin") && fs::file_size(folder / L"new" / L"deeper" / L"x.bin") == data.size(),
          L"file is on disk");
    Check(ReadEquals(*fa, L"new\\deeper\\x.bin", data) && FindEntry(*fa, L"new\\deeper\\x.bin"),
          L"read back + entries()");
    Check(fa->rename(L"new\\deeper\\x.bin", L"y.bin", err), L"rename -> y.bin", err);
    Check(fs::exists(folder / L"new" / L"deeper" / L"y.bin") && FindEntry(*fa, L"new\\deeper\\y.bin"),
          L"rename on disk + entries()");
    Check(fa->remove(L"new\\deeper\\y.bin", err), L"remove y.bin", err);
    Check(!fs::exists(folder / L"new" / L"deeper" / L"y.bin") && !FindEntry(*fa, L"new\\deeper\\y.bin"),
          L"removed from disk + entries()");
    Check(fa->remove(L"new", err), L"remove folder new (recursive)", err);
    Check(!fs::exists(folder / L"new") && fa->entries().size() == base, L"folder gone, entry count back to base");
    std::uint64_t saved = 1;
    Check(fa->compact(&saved, {}, err) && saved == 0, L"compact on a folder is a no-op", err);
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (_setmode(_fileno(stdout), _O_U8TEXT) == -1) return 2;
    std::vector<std::wstring> archives;
    std::string key = "169841";
    std::wstring find, writeSrc;
    bool extra = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--extra") {
            extra = true;
        } else if (a == L"--write" && i + 1 < argc) {
            writeSrc = argv[++i];
        } else if (a == L"--find" && i + 1 < argc) {
            find = argv[++i];
        } else if (a == L"--key" && i + 1 < argc) {
            key.clear();
            for (const wchar_t c : std::wstring(argv[++i])) key.push_back(static_cast<char>(c));   // ASCII key
        } else {
            archives.push_back(a);
        }
    }
    if (archives.empty()) {
        // Particles.pk2 holds the .efp effects (textures resolve via the Media sibling).
        const std::wstring dir = require_client_dir();
        archives = { dir + L"Data.pk2", dir + L"Media.pk2", dir + L"Particles.pk2" };
    }
    int fails = 0;
    for (const std::wstring& a : archives) fails += TestArchive(a, key, find, extra);

    if (!writeSrc.empty()) {
        const fs::path src  = fs::absolute(writeSrc);
        const fs::path work = fs::temp_directory_path() / L"bakw";
        const fs::path copy = work / src.filename();
        std::error_code ec;
        if (fs::equivalent(src, copy, ec)) {
            std::wprintf(L"--write: source and copy are the same file, refusing.\n");
            return 3;
        }
        fs::create_directories(work);
        for (const wchar_t* suffix : { L"", L".bak", L".new" }) fs::remove(copy.wstring() + suffix, ec);
        std::wprintf(L"== copying %ls -> %ls\n", src.c_str(), copy.c_str());
        if (!fs::copy_file(src, copy, fs::copy_options::overwrite_existing, ec)) {
            std::wprintf(L"  copy FAIL: %hs\n", ec.message().c_str());
            return 4;
        }
        fails += TestWritePk2(copy, src, key);
        fails += TestWriteFolder(copy, work / L"folder", key);
        std::wprintf(L"WRITE CHECKS: %d run, %d failed\n", g_checks, g_failed);
        fails += g_failed;
    }
    std::wprintf(L"RESULT: %ls (%d failures)\n", fails == 0 ? L"OK" : L"FAIL", fails);
    return fails == 0 ? 0 : 1;
}
