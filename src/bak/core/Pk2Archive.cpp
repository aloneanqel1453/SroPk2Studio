// ============================================================================
//  SRO Asset Studio - core/Pk2Archive.cpp
//
//  Read + write access to a JMXPACK archive.
//
//  Everything lives in Pk2Archive::Impl; the public methods are thin locking
//  wrappers. That keeps the private helpers out of the shared header (other
//  agents include it) and lets compact() spin up a second Impl for the
//  rebuilt file without any friend declarations.
//
//  The rules that shape this file:
//    * LAZY. open() touches the 256-byte header and the single root block.
//      A folder's block chain is read the first time somebody asks for its
//      children, so a 6.26 GB archive opens instantly.
//    * 64-bit offsets everywhere. Only Pk2Entry::size is 32-bit.
//    * File payloads are NOT encrypted; only the 2560-byte blocks are.
//    * "." and ".." are self/parent pointers - never listed.
//    * Never truncate the user's file. compact() writes a new file and keeps
//      a .bak.
//    * Every offset out of a block is validated against the real file size
//      before it is used, and every chain walk keeps a visited set, so a
//      hostile or truncated archive fails gracefully instead of looping or
//      reading out of bounds.
// ============================================================================
#include "Pk2Archive.h"
#include "AssetCrypt.h"
#include "../formats/FormatId.h"
#include "KeyRing.h"

#include <map>
#include <set>
#include <unordered_map>

namespace sro {

namespace {

/// Payload copy buffer. Big enough that a 100 MB file is a few dozen writes,
/// small enough to stay off the "huge allocation" path.
constexpr size_t kCopyChunk = 4u * 1024 * 1024;

/// New space at the end of the file is rounded up to this, matching what the
/// reference implementation (and therefore every other pk2 tool) does.
constexpr u64 kTailAlignment = 4096;

/// Entry names are ANSI. Almost every real name is 7-bit ASCII; the rest come
/// from a Korean (CP949) or Turkish (CP1254 == the system ANSI page here)
/// client. Never assume UTF-8.
std::wstring DecodeAnsiName(const std::string& ansi) {
    if (ansi.empty()) return std::wstring();

    bool pureAscii = true;
    for (unsigned char c : ansi) {
        if (c >= 0x80) { pureAscii = false; break; }
    }
    if (pureAscii) return std::wstring(ansi.begin(), ansi.end());

    std::wstring w = MultiByteToWide(ansi, CP_ACP);
    if (w.empty() || w.find(static_cast<wchar_t>(0xFFFD)) != std::wstring::npos) {
        const std::wstring korean = MultiByteToWide(ansi, 949);
        if (!korean.empty() && korean.find(static_cast<wchar_t>(0xFFFD)) == std::wstring::npos)
            return korean;
    }
    if (w.empty()) w = MultiByteToWide(ansi, 1252);
    return w;
}

/// Inverse of DecodeAnsiName for names the user typed.
std::string EncodeAnsiName(const std::wstring& w) {
    if (w.empty()) return std::string();
    bool pureAscii = true;
    for (wchar_t c : w) {
        if (c > 0x7F) { pureAscii = false; break; }
    }
    if (pureAscii) {
        std::string s;
        s.reserve(w.size());
        for (wchar_t c : w) s.push_back(static_cast<char>(c));
        return s;
    }
    std::string s = WideToMultiByte(w, CP_ACP);
    if (s.empty()) s = WideToMultiByte(w, 1252);
    return s;
}

/// A name that would break the archive or the file system.
bool IsUsableName(const std::wstring& name) {
    if (name.empty()) return false;
    if (name == L"." || name == L"..") return false;
    return name.find(L'\\') == std::wstring::npos &&
           name.find(L'/') == std::wstring::npos;
}

} // namespace

// ===========================================================================
//  Impl
// ===========================================================================
struct Pk2Archive::Impl {
    // --- state -------------------------------------------------------------
    std::wstring  path;
    Pk2RawHeader  header{};
    Blowfish      cipher;
    bool          opened          = false;
    bool          readOnlyMode    = true;
    bool          blocksEncrypted = true;
    u64           fileSize        = 0;
    // Offset of the root directory block: 256 for a stock archive, 0x33E for a
    // SailSafe (Sailor) archive. Set per-instance in openFile so every read path
    // (createRootNode, the lazy tree walk) uses the right first block.
    u64           rootBlockOffset = kPk2RootBlockOffset;
    File          file;

    mutable std::recursive_mutex mutex;

    // Node storage. Pointers handed out stay valid until close(); removed
    // nodes are only unlinked, never freed, so the UI can never dangle.
    std::vector<std::unique_ptr<Pk2Node>> nodes;
    Pk2Node* rootNode = nullptr;

    /// lower-case inner path -> node, filled as folders load.
    std::unordered_map<std::wstring, Pk2Node*> pathIndex;

    /// Entries the loader refused because their payload lies outside the file.
    /// A healthy archive has zero; anything else means the file is truncated or
    /// damaged and part of the tree is genuinely unreachable. Kept so the tool
    /// can SAY so instead of quietly showing a short listing.
    u64 skippedEntries = 0;
    std::vector<std::wstring> skippedSamples;

    /// folder node -> the block offsets that make up its chain (in order).
    std::unordered_map<const Pk2Node*, std::vector<i64>> folderChains;

    /// folder node -> index into its chain where the free-slot scan may start.
    /// Without this, adding N files to one folder costs O(N^2) block reads.
    std::unordered_map<const Pk2Node*, size_t> chainScanStart;

    /// offset -> size of every live region. Built on demand (writes only).
    std::map<i64, u64> allocations;
    bool allocationsBuilt = false;

    /// No free gap exists below this offset. Same reason as chainScanStart:
    /// re-scanning the whole allocation map per file is O(N^2). Any erase
    /// resets it to 0, because an erase is exactly what creates a gap.
    i64 allocNoGapBefore = 0;

    ~Impl() { closeAll(); }

    // =======================================================================
    // Lifetime
    // =======================================================================
    void closeAll() {
        readOnlyMode = true;   // never leak a stale writable flag
        file.close();
        nodes.clear();
        rootNode = nullptr;
        pathIndex.clear();
        folderChains.clear();
        chainScanStart.clear();
        allocations.clear();
        allocationsBuilt = false;
        allocNoGapBefore = 0;
        opened = false;
        fileSize = 0;
        rootBlockOffset = kPk2RootBlockOffset;
        skippedEntries = 0;
        skippedSamples.clear();
        path.clear();
        std::memset(&header, 0, sizeof(header));
    }

    Pk2Node* makeNode() {
        nodes.push_back(std::unique_ptr<Pk2Node>(new Pk2Node()));
        return nodes.back().get();
    }

    void createRootNode() {
        rootNode = makeNode();
        rootNode->entry = Pk2Entry();
        rootNode->entry.type    = Pk2EntryType::Folder;
        rootNode->entry.nameAnsi = ".";
        rootNode->entry.name     = L".";
        rootNode->entry.offset  = static_cast<i64>(rootBlockOffset);
        rootNode->parent = nullptr;
        rootNode->loaded = false;
        pathIndex[std::wstring()] = rootNode;
    }

    Result openFile(const std::wstring& p, const Blowfish& bf, bool ro) {
        closeAll();

        if (!FileExists(p))
            return Result::Fail(Status::NotFound, T(L"Dosya bulunamadi: ") + p);

        bool degradedToReadOnly = false;
        if (ro) {
            if (!file.openRead(p))
                return Result::Fail(Status::AccessDenied, T(L"Dosya okumak icin acilamadi: ") + p);
        } else {
            // A Silkroad Media.pk2/Data.pk2 held open by a running client (or a
            // file with the read-only attribute) must still be browsable, so we
            // degrade to read-only instead of refusing to open at all.
            if (!file.openReadWrite(p)) {
                if (!file.openRead(p))
                    return Result::Fail(Status::AccessDenied,
                                        T(L"Dosya acilamadi (baska bir program kullaniyor olabilir): ") + p);
                degradedToReadOnly = true;
            }
        }

        if (!file.readAt(0, &header, sizeof(header))) {
            closeAll();
            return Result::Fail(Status::IoError, T(L"Baslik okunamadi."));
        }

        // SailSafe (Sailor) archives put the root block at 0x33E, not 256, and
        // carry a fixed signature block instead of the JoyMax header. Detect by
        // that constant signature; everything else (cipher, oracle, tree) is
        // identical. This moves ONLY the root-block offset for this instance.
        const bool isSailor =
            std::memcmp(&header, kSailorSignature, sizeof(kSailorSignature)) == 0;
        rootBlockOffset = isSailor ? kSailorRootBlockOffset : kPk2RootBlockOffset;

        fileSize = file.size();
        if (fileSize < rootBlockOffset + kPk2BlockSize) {
            closeAll();
            return Result::Fail(Status::Truncated, T(L"Dosya bir PK2 icin cok kucuk."));
        }

        u8 rawRoot[kPk2BlockSize];
        if (!file.readAt(rootBlockOffset, rawRoot, kPk2BlockSize)) {
            closeAll();
            return Result::Fail(Status::IoError, T(L"Kok blok okunamadi."));
        }

        // SPEC §1: honour header.Encrypted, but also detect plaintext blocks
        // independently - a root block that already begins 01 2E is not encrypted
        // no matter what the flag claims.
        // For a SailSafe archive the JoyMax Encrypted flag does not apply (the
        // header is a different signature block), but the directory IS Blowfish
        // encrypted, so force it on unless the block is already plaintext.
        const bool looksPlain = (rawRoot[0] == 0x01 && rawRoot[1] == 0x2E);
        blocksEncrypted = isSailor ? !looksPlain
                                   : ((header.Encrypted != 0) && !looksPlain);

        if (blocksEncrypted) {
            if (!bf.valid()) {
                closeAll();
                return Result::Fail(Status::WrongKey,
                                    T(L"Arsiv sifreli, gecerli bir key gerekli."));
            }
            // The STRONG oracle decides here, deliberately - not KeyRing::Validate,
            // which is allowed to fall back to the weak header checksum.
            //
            // Observed in the wild (SRO-LIB-2027\Music.pk2): an archive that was
            // re-keyed without its 3 checksum bytes being regenerated. The weak
            // oracle says "correct key", the root block then decrypts to noise and
            // every entry is rejected, and the user gets a silently EMPTY tree
            // instead of an error. Refusing to open is the honest answer.
            if (!KeyRing::RootBlockOracle(bf, rawRoot)) {
                const bool checksumMatches = KeyRing::ChecksumOracle(bf, header);
                closeAll();
                return Result::Fail(Status::WrongKey, checksumMatches
                    ? T(L"Baslik checksum'i eslesiyor ama kok blok bu key ile cozulmuyor "
                      L"- arsiv baska bir key ile yeniden paketlenmis olabilir.")
                    : T(L"Key bu arsivi acmiyor (kok blok dogrulamasi basarisiz)."));
            }
        }

        cipher       = bf;
        path         = p;
        readOnlyMode = ro || degradedToReadOnly;
        opened       = true;
        createRootNode();
        return Result::Ok();
    }

    // =======================================================================
    // Block level
    // =======================================================================
    bool blockOffsetValid(i64 off) const {
        return off >= static_cast<i64>(kPk2HeaderSize) &&
               static_cast<u64>(off) + kPk2BlockSize <= fileSize;
    }

    Result readBlockAt(i64 off, Pk2RawBlock& out) const {
        std::memset(&out, 0, sizeof(out));
        if (!opened) return Result::Fail(Status::InvalidArgument, T(L"Arsiv acik degil."));
        if (!blockOffsetValid(off))
            return Result::Fail(Status::BadFormat,
                                FormatW(T(L"Blok ofseti dosya disinda: %lld"), static_cast<long long>(off)));

        u8* dst = reinterpret_cast<u8*>(&out);
        if (!file.readAt(static_cast<u64>(off), dst, kPk2BlockSize))
            return Result::Fail(Status::IoError, T(L"Blok okunamadi."));
        if (blocksEncrypted) cipher.decodeInPlace(dst, kPk2BlockSize);
        return Result::Ok();
    }

    Result writeBlockAt(i64 off, const Pk2RawBlock& in) {
        if (!opened)       return Result::Fail(Status::InvalidArgument, T(L"Arsiv acik degil."));
        if (readOnlyMode)  return Result::Fail(Status::AccessDenied, T(L"Arsiv salt okunur acildi."));
        if (off < static_cast<i64>(kPk2HeaderSize))
            return Result::Fail(Status::BadFormat, T(L"Gecersiz blok ofseti."));

        u8 buf[kPk2BlockSize];
        std::memcpy(buf, &in, kPk2BlockSize);
        if (blocksEncrypted) cipher.encodeInPlace(buf, kPk2BlockSize);
        if (!file.writeAt(static_cast<u64>(off), buf, kPk2BlockSize))
            return Result::Fail(Status::IoError, T(L"Blok yazilamadi."));
        if (static_cast<u64>(off) + kPk2BlockSize > fileSize)
            fileSize = static_cast<u64>(off) + kPk2BlockSize;
        return Result::Ok();
    }

    // =======================================================================
    // Lazy tree
    // =======================================================================
    static std::wstring JoinPath(const std::wstring& parent, const std::wstring& name) {
        return parent.empty() ? name : (parent + L"\\" + name);
    }

    bool entryPayloadValid(const Pk2Entry& e) const {
        if (e.isFolder()) return blockOffsetValid(e.offset);
        if (e.size == 0)  return e.offset >= 0 && static_cast<u64>(e.offset) <= fileSize;
        return e.offset >= static_cast<i64>(kPk2HeaderSize) &&
               static_cast<u64>(e.offset) + e.size <= fileSize;
    }

    Result loadChildren(Pk2Node* node) {
        if (!node) return Result::Fail(Status::InvalidArgument, T(L"Bos dugum."));
        if (!node->isFolder()) return Result::Ok();
        if (node->loaded) return Result::Ok();
        if (!opened) return Result::Fail(Status::InvalidArgument, T(L"Arsiv acik degil."));

        std::vector<i64>& chain = folderChains[node];
        chain.clear();

        std::set<i64> visited;
        i64 off = node->entry.offset;
        Result last = Result::Ok();

        while (off != 0) {
            if (!blockOffsetValid(off)) {
                last = Result::Fail(Status::BadFormat,
                                    FormatW(T(L"Bozuk blok zinciri: %s"), node->fullPath.c_str()));
                break;
            }
            if (!visited.insert(off).second) {
                // The chain points back at a block we already read - stop rather
                // than spin forever on a corrupt/hostile archive.
                last = Result::Fail(Status::BadFormat,
                                    FormatW(T(L"Dongusel blok zinciri: %s"), node->fullPath.c_str()));
                break;
            }
            chain.push_back(off);

            Pk2RawBlock blk;
            const Result r = readBlockAt(off, blk);
            if (!r.ok()) { last = r; break; }

            for (int slot = 0; slot < static_cast<int>(kPk2EntriesPerBlock); ++slot) {
                Pk2Entry e;
                if (!DecodeEntry(blk.Entries[slot], off, slot, e)) continue;
                if (!e.isListable()) continue;
                if (!entryPayloadValid(e)) {
                    // Skipping is right - the payload is outside the file and
                    // could never be read. Skipping SILENTLY is not: on a
                    // truncated archive whole folders then vanish from the tree,
                    // from search and from extract while every call still
                    // reports success. Count it so the UI can say so.
                    ++skippedEntries;
                    if (skippedSamples.size() < 8)
                        skippedSamples.push_back(JoinPath(node->fullPath, e.name));
                    continue;
                }

                Pk2Node* child = makeNode();
                child->entry    = e;
                child->name     = e.name;
                child->parent   = node;
                child->fullPath = JoinPath(node->fullPath, e.name);
                child->loaded   = false;

                if (e.isFolder()) node->folders.push_back(child);
                else              node->files.push_back(child);

                pathIndex[ToLowerW(child->fullPath)] = child;
            }

            off = blk.Entries[kPk2EntriesPerBlock - 1].NextBlock;
        }

        // Even a partially read folder counts as loaded: retrying would just
        // re-hit the same corruption and duplicate the children we did get.
        node->loaded = true;
        return last;
    }

    Result findNode(const std::wstring& innerPath, Pk2Node** out) {
        if (out) *out = nullptr;
        if (!opened) return Result::Fail(Status::InvalidArgument, T(L"Arsiv acik degil."));

        const std::wstring norm = NormalizeInnerPath(innerPath);
        if (norm.empty()) { if (out) *out = rootNode; return Result::Ok(); }

        const std::wstring key = ToLowerW(norm);
        auto hit = pathIndex.find(key);
        if (hit != pathIndex.end()) { if (out) *out = hit->second; return Result::Ok(); }

        Pk2Node* cur = rootNode;
        const std::vector<std::wstring> parts = SplitW(norm, L'\\');
        for (const std::wstring& part : parts) {
            if (part.empty()) continue;
            if (!cur->isFolder())
                return Result::Fail(Status::NotFound, T(L"Yol bir klasor degil: ") + innerPath);
            loadChildren(cur);

            Pk2Node* next = nullptr;
            for (Pk2Node* f : cur->folders) if (IEqualsW(f->name, part)) { next = f; break; }
            if (!next)
                for (Pk2Node* f : cur->files) if (IEqualsW(f->name, part)) { next = f; break; }
            if (!next)
                return Result::Fail(Status::NotFound, T(L"Bulunamadi: ") + innerPath);
            cur = next;
        }
        if (out) *out = cur;
        return Result::Ok();
    }

    // =======================================================================
    // Reading
    // =======================================================================
    Result readInto(const Pk2Node* node, size_t maxBytes, std::vector<u8>& out) const {
        out.clear();
        if (!opened) return Result::Fail(Status::InvalidArgument, T(L"Arsiv acik degil."));
        if (!node || !node->isFile())
            return Result::Fail(Status::InvalidArgument, T(L"Dosya dugumu degil."));

        const u64 full = node->entry.size;
        const u64 want = (maxBytes == 0) ? full : (std::min)(full, static_cast<u64>(maxBytes));
        if (want == 0) return Result::Ok();

        if (node->entry.offset < 0 ||
            static_cast<u64>(node->entry.offset) + full > fileSize) {
            return Result::Fail(Status::BadFormat,
                                T(L"Dosya verisi arsiv sinirlarinin disinda: ") + node->fullPath);
        }
        try { out.resize(static_cast<size_t>(want)); }
        catch (...) { return Result::Fail(Status::OutOfMemory, T(L"Bellek yetersiz.")); }

        if (!file.readAt(static_cast<u64>(node->entry.offset), out.data(), out.size())) {
            out.clear();
            return Result::Fail(Status::IoError, T(L"Dosya verisi okunamadi: ") + node->fullPath);
        }
        // Some shards additionally XOR-encrypt individual assets. Once the
        // session has learned enough keystream (see core/AssetCrypt.h) they
        // decrypt transparently, so preview and export just work.
        // FULL inner path, not just the name: the vSRO cipher derives its nonce
        // from the whole path (the S-box path only ever looked at the extension,
        // so it is unaffected).
        MaybeDecryptAsset(node->fullPath.empty() ? node->name : node->fullPath, out);
        return Result::Ok();
    }

    /// Decrypt in place when this is an encrypted asset AND the keystream
    /// covers it. A no-op otherwise, so normal files pay one magic compare.
    static void MaybeDecryptAsset(const std::wstring& name, std::vector<u8>& buf) {
        DecryptIfEncrypted(name, buf);        // handles every extension
    }

    Result extractOne(const Pk2Node* node, const std::wstring& outPath) const {
        if (!opened) return Result::Fail(Status::InvalidArgument, T(L"Arsiv acik degil."));
        if (!node || !node->isFile())
            return Result::Fail(Status::InvalidArgument, T(L"Dosya dugumu degil."));

        const std::wstring parent = PathParent(outPath);
        if (!parent.empty() && !EnsureDirectory(parent))
            return Result::Fail(Status::IoError, T(L"Klasor olusturulamadi: ") + parent);

        // Encrypted assets must be written DECRYPTED, so take the buffered path
        // (readInto -> DecryptIfEncrypted) instead of streaming raw bytes. This
        // covers BOTH asset ciphers - the position-keyed keystream vault (Kim Hoa)
        // and the HntSRO per-extension S-box - since readInto applies whichever
        // one fits. Any non-JMXV asset small enough to hold in RAM goes this way;
        // a huge non-JMXV blob (bulk audio) stays on the raw stream so a 500 MB
        // entry never needs 500 MB. A file that is not actually encrypted is a
        // no-op inside DecryptIfEncrypted, so its raw bytes are written unchanged.
        {
            u8 magic[4] = {};
            const bool haveMagic = node->entry.size >= 4 && node->entry.offset >= 0 &&
                file.readAt(static_cast<u64>(node->entry.offset), magic, sizeof(magic));
            const bool notJmxv = haveMagic &&
                !(magic[0] == 'J' && magic[1] == 'M' && magic[2] == 'X' && magic[3] == 'V');
            const bool bufferable = node->entry.size <= (128ull * 1024 * 1024);
            if (notJmxv && bufferable) {
                std::vector<u8> whole;
                const Result rr = readInto(node, 0, whole);
                if (!rr.ok()) return rr;
                return WriteEntireFileAtomic(outPath, whole.data(), whole.size());
            }
        }

        const u64 size = node->entry.size;
        if (size && (node->entry.offset < 0 ||
                     static_cast<u64>(node->entry.offset) + size > fileSize)) {
            return Result::Fail(Status::BadFormat,
                                T(L"Dosya verisi arsiv sinirlarinin disinda: ") + node->fullPath);
        }

        File dst;
        if (!dst.createNew(outPath, true))
            return Result::Fail(Status::IoError, T(L"Cikis dosyasi olusturulamadi: ") + outPath);

        // Streamed, so a 500 MB entry never needs 500 MB of RAM.
        std::vector<u8> buf;
        try { buf.resize(static_cast<size_t>((std::min)(size, static_cast<u64>(kCopyChunk)))); }
        catch (...) { return Result::Fail(Status::OutOfMemory, T(L"Bellek yetersiz.")); }

        u64 left = size;
        u64 src  = static_cast<u64>(node->entry.offset);
        while (left) {
            const size_t chunk = static_cast<size_t>((std::min)(left, static_cast<u64>(buf.size())));
            if (!file.readAt(src, buf.data(), chunk))
                return Result::Fail(Status::IoError, T(L"Okuma hatasi: ") + node->fullPath);
            if (!dst.write(buf.data(), chunk))
                return Result::Fail(Status::IoError, T(L"Yazma hatasi: ") + outPath);
            src  += chunk;
            left -= chunk;
        }
        return Result::Ok();
    }

    // =======================================================================
    // Whole-tree traversal
    // =======================================================================
    /// Depth-first over every node below `start`, loading folders as it goes.
    /// `visit` returns false to stop the walk (reported as Status::Cancelled).
    Result walkFrom(Pk2Node* start,
                    const std::function<bool(Pk2Node*)>& visit,
                    const ProgressFn& progress) {
        if (!opened) return Result::Fail(Status::InvalidArgument, T(L"Arsiv acik degil."));
        if (!start) return Result::Fail(Status::InvalidArgument, T(L"Bos dugum."));

        std::vector<Pk2Node*> stack;
        stack.push_back(start);
        u64 seen = 0;
        Result worst = Result::Ok();

        while (!stack.empty()) {
            Pk2Node* node = stack.back();
            stack.pop_back();

            if (node->isFolder()) {
                const Result r = loadChildren(node);
                if (!r.ok() && worst.ok()) worst = r;   // remember, keep walking
                for (Pk2Node* f : node->files) {
                    ++seen;
                    if (visit && !visit(f)) return Result::Fail(Status::Cancelled);
                }
                // Push in reverse so the visit order matches the listing order.
                for (size_t i = node->folders.size(); i-- > 0; ) {
                    Pk2Node* sub = node->folders[i];
                    ++seen;
                    if (visit && !visit(sub)) return Result::Fail(Status::Cancelled);
                    stack.push_back(sub);
                }
            }
            if ((seen & 0x3FF) == 0 && !ReportProgress(progress, seen, 0, nullptr))
                return Result::Fail(Status::Cancelled);
        }
        return worst;
    }

    /// Total payload bytes / file count under `start` (loads the whole subtree).
    Result measure(Pk2Node* start, u64& bytes, u64& files, const ProgressFn& progress) {
        bytes = 0; files = 0;
        if (start && start->isFile()) { bytes = start->entry.size; files = 1; return Result::Ok(); }
        u64 b = 0, f = 0;
        const Result r = walkFrom(start, [&](Pk2Node* n) {
            if (n->isFile()) { b += n->entry.size; ++f; }
            return true;
        }, progress);
        bytes = b; files = f;
        return r;
    }

    /// Archive stem for the output root, e.g. "OriDB" from "...\OriDB.lib".
    std::wstring archiveBaseName() const {
        std::wstring n = PathFileName(path);
        const size_t d = n.find_last_of(L'.');
        if (d != std::wstring::npos) n = n.substr(0, d);
        if (n.empty()) n = L"arsiv";
        // Silkroad's own layout is lower-case ("media\", "data\"), and the user
        // wants the extracted root to match: Media.pk2 -> media\.
        if (!n.empty()) n[0] = (wchar_t)towlower(n[0]);
        return SanitizeFileName(n);
    }

    /// Sanitize a full inner path ("res\cos\x.bsr") component by component,
    /// dropping empty / "." / ".." so nothing can escape the output directory.
    static std::wstring SanitizeRelPath(const std::wstring& full) {
        std::wstring out;
        size_t i = 0;
        while (i < full.size()) {
            const size_t j = full.find_first_of(L"\\/", i);
            const size_t end = (j == std::wstring::npos) ? full.size() : j;
            std::wstring comp = full.substr(i, end - i);
            i = (j == std::wstring::npos) ? full.size() : j + 1;
            if (comp.empty() || comp == L"." || comp == L"..") continue;
            comp = SanitizeFileName(comp);
            out = out.empty() ? comp : PathJoin(out, comp);
        }
        return out;
    }

    Result extractTree(Pk2Node* node, const std::wstring& outDir, const ProgressFn& progress) {
        if (!opened) return Result::Fail(Status::InvalidArgument, T(L"Arsiv acik degil."));
        if (!node)   return Result::Fail(Status::InvalidArgument, T(L"Bos dugum."));

        // Everything lands under <outDir>\<ArchiveName>\, preserving each file's
        // COMPLETE inner path. Selecting res\cos\cos_p_baby_dino.bsr from OriDB
        // therefore writes  <outDir>\OriDB\res\cos\cos_p_baby_dino.bsr  - the
        // full path, not a bare file name (which is what the user asked for).
        const std::wstring base = PathJoin(outDir, archiveBaseName());
        if (!EnsureDirectory(base))
            return Result::Fail(Status::IoError, T(L"Klasor olusturulamadi: ") + base);

        u64 totalBytes = 0, totalFiles = 0;
        const Result m = measure(node, totalBytes, totalFiles, progress);
        if (m.status == Status::Cancelled) return m;

        u64 done = 0;
        return extractTreeInner(node, base, done, totalBytes, progress);
    }

    Result extractTreeInner(Pk2Node* node, const std::wstring& base,
                            u64& done, u64 total, const ProgressFn& progress) {
        if (node->isFile()) {
            const std::wstring rel = SanitizeRelPath(node->fullPath);
            const std::wstring target = rel.empty()
                                            ? PathJoin(base, SanitizeFileName(node->name))
                                            : PathJoin(base, rel);   // extractOne makes parents
            const Result r = extractOne(node, target);
            done += node->entry.size;
            if (!ReportProgress(progress, done, total, node->fullPath.c_str()))
                return Result::Fail(Status::Cancelled);
            return r;
        }

        // Folder: recurse. Directories are created lazily from each file's full
        // inner path, so the tree above the selected node is preserved too.
        const Result load = loadChildren(node);
        Result worst = load.ok() ? Result::Ok() : load;

        for (Pk2Node* f : node->files) {
            const Result r = extractTreeInner(f, base, done, total, progress);
            if (r.status == Status::Cancelled) return r;
            if (!r.ok() && worst.ok()) worst = r;
        }
        for (Pk2Node* d : node->folders) {
            const Result r = extractTreeInner(d, base, done, total, progress);
            if (r.status == Status::Cancelled) return r;
            if (!r.ok() && worst.ok()) worst = r;
        }
        return worst;
    }

    // =======================================================================
    // Space allocation (write side only)
    // =======================================================================
    /// Builds the offset -> size map of everything currently live. This forces
    /// a full tree load, so it only ever runs on the first write operation.
    Result buildAllocations(const ProgressFn& progress) {
        if (allocationsBuilt) return Result::Ok();
        if (!opened) return Result::Fail(Status::InvalidArgument, T(L"Arsiv acik degil."));

        allocations.clear();
        // Everything before the root block is header. For a stock archive that
        // is the 256-byte JoyMax header; for a SailSafe archive it is the fixed
        // signature block up to 0x33E, which the client memcmp's - reserving
        // only 256 bytes left a 574-byte "gap" that a small added file would
        // overwrite, breaking the archive for the Sailor client.
        allocations[0] = rootBlockOffset;

        const Result r = walkFrom(rootNode, [&](Pk2Node* n) {
            if (n->isFile() && n->entry.size > 0)
                allocations[n->entry.offset] = n->entry.size;
            return true;
        }, progress);
        if (r.status == Status::Cancelled) { allocations.clear(); return r; }

        // Every folder's block chain is live space too.
        for (const auto& kv : folderChains)
            for (i64 off : kv.second) allocations[off] = kPk2BlockSize;

        allocationsBuilt = true;
        return Result::Ok();
    }

    /// First gap large enough, else fresh space at the end of the file.
    /// Returns a negative value on failure.
    i64 allocateSpace(u64 size) {
        if (size == 0) size = 1;

        for (auto it = allocations.begin(); it != allocations.end(); ++it) {
            const i64 end = it->first + static_cast<i64>(it->second);
            if (end < 0) continue;
            auto next = std::next(it);
            const i64 limit = (next == allocations.end())
                                  ? static_cast<i64>(fileSize)
                                  : next->first;
            if (limit > end && static_cast<u64>(limit - end) >= size)
                return end;
        }

        // Grow. SetEndOfFile zero-fills on NTFS, so a 200 MB import does not
        // have to stream 200 MB of zeros first.
        const u64 aligned = ((size + kTailAlignment - 1) / kTailAlignment) * kTailAlignment;
        const i64 pos = static_cast<i64>(fileSize);
        if (!file.setEndOfFile(fileSize + aligned)) return -1;
        fileSize += aligned;
        return pos;
    }

    struct Payload {
        const u8*   mem  = nullptr;   ///< in-memory source
        const File* src  = nullptr;   ///< or another archive to copy from
        u64         srcOffset = 0;
        u64         size = 0;
    };

    Result writePayload(i64 dstOffset, const Payload& p) {
        if (p.size == 0) return Result::Ok();
        if (dstOffset < static_cast<i64>(kPk2HeaderSize))
            return Result::Fail(Status::BadFormat, T(L"Gecersiz veri ofseti."));

        if (p.mem) {
            if (!file.writeAt(static_cast<u64>(dstOffset), p.mem, static_cast<size_t>(p.size)))
                return Result::Fail(Status::IoError, T(L"Veri yazilamadi."));
        } else if (p.src) {
            std::vector<u8> buf;
            try { buf.resize(static_cast<size_t>((std::min)(p.size, static_cast<u64>(kCopyChunk)))); }
            catch (...) { return Result::Fail(Status::OutOfMemory, T(L"Bellek yetersiz.")); }
            u64 left = p.size, from = p.srcOffset, to = static_cast<u64>(dstOffset);
            while (left) {
                const size_t chunk = static_cast<size_t>((std::min)(left, static_cast<u64>(buf.size())));
                if (!p.src->readAt(from, buf.data(), chunk))
                    return Result::Fail(Status::IoError, T(L"Kaynak veri okunamadi."));
                if (!file.writeAt(to, buf.data(), chunk))
                    return Result::Fail(Status::IoError, T(L"Veri yazilamadi."));
                from += chunk; to += chunk; left -= chunk;
            }
        } else {
            return Result::Fail(Status::InvalidArgument, T(L"Veri kaynagi yok."));
        }

        if (static_cast<u64>(dstOffset) + p.size > fileSize)
            fileSize = static_cast<u64>(dstOffset) + p.size;
        return Result::Ok();
    }

    // =======================================================================
    // Block slot management
    // =======================================================================
    /// Appends a fresh, empty block to `folder`'s chain and links it in.
    Result expandChain(Pk2Node* folder, i64& newBlockOffset) {
        newBlockOffset = 0;
        std::vector<i64>& chain = folderChains[folder];
        if (chain.empty())
            return Result::Fail(Status::BadFormat, T(L"Klasorun blok zinciri yok."));

        const i64 off = allocateSpace(kPk2BlockSize);
        if (off < 0) return Result::Fail(Status::IoError, T(L"Yeni blok icin yer ayrilamadi."));

        Pk2RawBlock fresh{};
        std::memset(&fresh, 0, sizeof(fresh));
        Result r = writeBlockAt(off, fresh);
        if (!r.ok()) return r;
        allocations[off] = kPk2BlockSize;

        const i64 lastOff = chain.back();
        Pk2RawBlock last;
        r = readBlockAt(lastOff, last);
        if (!r.ok()) return r;
        last.Entries[kPk2EntriesPerBlock - 1].NextBlock = off;
        r = writeBlockAt(lastOff, last);
        if (!r.ok()) return r;

        chain.push_back(off);
        newBlockOffset = off;
        return Result::Ok();
    }

    /// First Empty slot in `folder`'s chain, expanding the chain if it is full.
    Result findFreeSlot(Pk2Node* folder, i64& blockOffset, int& slot) {
        blockOffset = 0; slot = -1;
        Result r = loadChildren(folder);
        if (!r.ok() && folderChains[folder].empty()) return r;

        std::vector<i64> chain = folderChains[folder];
        for (i64 off : chain) {
            Pk2RawBlock blk;
            r = readBlockAt(off, blk);
            if (!r.ok()) return r;
            for (int i = 0; i < static_cast<int>(kPk2EntriesPerBlock); ++i) {
                if (blk.Entries[i].Type == 0) { blockOffset = off; slot = i; return Result::Ok(); }
            }
        }

        i64 fresh = 0;
        r = expandChain(folder, fresh);
        if (!r.ok()) return r;
        blockOffset = fresh;
        slot = 0;
        return Result::Ok();
    }

    /// Overwrite one slot, preserving the chain link that lives in slot 19.
    Result writeSlot(i64 blockOffset, int slot, const Pk2Entry& e) {
        if (slot < 0 || slot >= static_cast<int>(kPk2EntriesPerBlock))
            return Result::Fail(Status::InvalidArgument, T(L"Gecersiz blok yuvasi."));
        Pk2RawBlock blk;
        Result r = readBlockAt(blockOffset, blk);
        if (!r.ok()) return r;

        const i64 keepNext = blk.Entries[slot].NextBlock;
        Pk2RawEntry raw{};
        if (!EncodeEntry(e, raw))
            return Result::Fail(Status::InvalidArgument, T(L"Isim 88 bayti asiyor."));
        raw.NextBlock = keepNext;               // slot 19 also carries the chain link
        blk.Entries[slot] = raw;
        return writeBlockAt(blockOffset, blk);
    }

    // =======================================================================
    // Mutations
    // =======================================================================
    Result requireWritable() const {
        if (!opened)      return Result::Fail(Status::InvalidArgument, T(L"Arsiv acik degil."));
        if (readOnlyMode) return Result::Fail(Status::AccessDenied, T(L"Arsiv salt okunur acildi."));
        return Result::Ok();
    }

    Pk2Node* findChild(Pk2Node* folder, const std::wstring& name, bool wantFolder) const {
        const std::vector<Pk2Node*>& list = wantFolder ? folder->folders : folder->files;
        for (Pk2Node* n : list) if (IEqualsW(n->name, name)) return n;
        return nullptr;
    }

    Result addPayloadFile(Pk2Node* parent, const std::wstring& name,
                          const Payload& payload, bool overwrite) {
        Result r = requireWritable();
        if (!r.ok()) return r;
        if (!parent || !parent->isFolder())
            return Result::Fail(Status::InvalidArgument, T(L"Hedef bir klasor degil."));
        if (!IsUsableName(name))
            return Result::Fail(Status::InvalidArgument, T(L"Gecersiz dosya adi."));
        if (payload.size > 0xFFFFFFFFull)
            return Result::Fail(Status::InvalidArgument, T(L"Dosya 4 GB sinirini asiyor."));
        const std::string ansi = EncodeAnsiName(name);
        if (ansi.empty() || ansi.size() >= kPk2MaxNameBytes)
            return Result::Fail(Status::InvalidArgument, T(L"Dosya adi cok uzun (88 bayt siniri)."));

        r = loadChildren(parent);
        if (!r.ok()) return r;
        r = buildAllocations(ProgressFn());
        if (!r.ok()) return r;

        if (findChild(parent, name, true))
            return Result::Fail(Status::AlreadyExists, T(L"Ayni isimde bir klasor var."));

        const u64 now = NowFileTime();
        Pk2Node* existing = findChild(parent, name, false);

        if (existing) {
            if (!overwrite) return Result::Fail(Status::AlreadyExists, T(L"Dosya zaten var."));

            // Never overwrite the live payload in place - the old code reused the
            // slot when the new data fit, so a mid-write failure corrupted the
            // existing file (rule 0.5). Always write to a FRESH region; the old
            // one stays reserved in `allocations` so allocateSpace can't hand it
            // back. The commit point is writeSlot: crash before it and the on-disk
            // entry still names the old (intact) region; after it, the new one.
            const i64 oldOffset = existing->entry.offset;
            const u32 oldSize   = existing->entry.size;

            i64 dstOffset = 0;                            // zero-byte file owns no space
            if (payload.size) {
                dstOffset = allocateSpace(payload.size);
                if (dstOffset < 0) return Result::Fail(Status::IoError, T(L"Yer ayrilamadi."));
            }
            r = writePayload(dstOffset, payload);
            if (!r.ok()) {
                if (payload.size) allocations.erase(dstOffset);   // release the fresh region
                return r;                                          // old file untouched
            }
            if (payload.size) allocations[dstOffset] = payload.size;

            Pk2Entry e = existing->entry;
            e.offset     = dstOffset;
            e.size       = static_cast<u32>(payload.size);
            e.modifyTime = now;
            e.nameAnsi   = ansi;
            e.name       = name;
            r = writeSlot(e.blockOffset, e.slot, e);
            if (!r.ok()) {
                if (payload.size) allocations.erase(dstOffset);   // roll back reservation
                return r;                                          // slot still names old region
            }

            // Committed: the slot now points at the new region, so free the old.
            if (oldOffset != 0 && oldSize) allocations.erase(oldOffset);
            existing->entry = e;
            return Result::Ok();
        }

        i64 blockOffset = 0; int slot = -1;
        r = findFreeSlot(parent, blockOffset, slot);
        if (!r.ok()) return r;

        // A zero-byte file owns no space at all, so it gets offset 0.
        const i64 dstOffset = payload.size ? allocateSpace(payload.size) : 0;
        if (dstOffset < 0) return Result::Fail(Status::IoError, T(L"Yer ayrilamadi."));
        r = writePayload(dstOffset, payload);
        if (!r.ok()) return r;
        if (payload.size) allocations[dstOffset] = payload.size;

        Pk2Entry e;
        e.type        = Pk2EntryType::File;
        e.name        = name;
        e.nameAnsi    = ansi;
        e.createTime  = now;
        e.modifyTime  = now;
        e.offset      = dstOffset;
        e.size        = static_cast<u32>(payload.size);
        e.blockOffset = blockOffset;
        e.slot        = slot;
        r = writeSlot(blockOffset, slot, e);
        if (!r.ok()) return r;

        Pk2Node* node = makeNode();
        node->entry    = e;
        node->name     = name;
        node->parent   = parent;
        node->fullPath = JoinPath(parent->fullPath, name);
        node->loaded   = true;
        parent->files.push_back(node);
        pathIndex[ToLowerW(node->fullPath)] = node;
        return Result::Ok();
    }

    Result addFolder(Pk2Node* parent, const std::wstring& name, Pk2Node** out) {
        if (out) *out = nullptr;
        Result r = requireWritable();
        if (!r.ok()) return r;
        if (!parent || !parent->isFolder())
            return Result::Fail(Status::InvalidArgument, T(L"Hedef bir klasor degil."));
        if (!IsUsableName(name))
            return Result::Fail(Status::InvalidArgument, T(L"Gecersiz klasor adi."));
        const std::string ansi = EncodeAnsiName(name);
        if (ansi.empty() || ansi.size() >= kPk2MaxNameBytes)
            return Result::Fail(Status::InvalidArgument, T(L"Klasor adi cok uzun (88 bayt siniri)."));

        r = loadChildren(parent);
        if (!r.ok()) return r;
        r = buildAllocations(ProgressFn());
        if (!r.ok()) return r;

        if (Pk2Node* have = findChild(parent, name, true)) {
            if (out) *out = have;
            return Result::Fail(Status::AlreadyExists, T(L"Klasor zaten var."));
        }
        if (findChild(parent, name, false))
            return Result::Fail(Status::AlreadyExists, T(L"Ayni isimde bir dosya var."));

        const i64 blockOff = allocateSpace(kPk2BlockSize);
        if (blockOff < 0) return Result::Fail(Status::IoError, T(L"Yeni klasor blogu icin yer yok."));

        const u64 now = NowFileTime();

        // A folder block always opens with its own "." and its parent's "..".
        Pk2RawBlock fresh{};
        std::memset(&fresh, 0, sizeof(fresh));
        {
            Pk2Entry self;
            self.type = Pk2EntryType::Folder; self.nameAnsi = "."; self.name = L".";
            self.createTime = now; self.modifyTime = now; self.offset = blockOff;
            Pk2RawEntry raw{};
            EncodeEntry(self, raw);
            fresh.Entries[0] = raw;

            Pk2Entry up;
            up.type = Pk2EntryType::Folder; up.nameAnsi = ".."; up.name = L"..";
            up.createTime = now; up.modifyTime = now; up.offset = parent->entry.offset;
            Pk2RawEntry raw2{};
            EncodeEntry(up, raw2);
            fresh.Entries[1] = raw2;
        }
        r = writeBlockAt(blockOff, fresh);
        if (!r.ok()) return r;
        allocations[blockOff] = kPk2BlockSize;

        i64 slotBlock = 0; int slot = -1;
        r = findFreeSlot(parent, slotBlock, slot);
        if (!r.ok()) return r;

        Pk2Entry e;
        e.type        = Pk2EntryType::Folder;
        e.name        = name;
        e.nameAnsi    = ansi;
        e.createTime  = now;
        e.modifyTime  = now;
        e.offset      = blockOff;
        e.size        = 0;
        e.blockOffset = slotBlock;
        e.slot        = slot;
        r = writeSlot(slotBlock, slot, e);
        if (!r.ok()) return r;

        Pk2Node* node = makeNode();
        node->entry    = e;
        node->name     = name;
        node->parent   = parent;
        node->fullPath = JoinPath(parent->fullPath, name);
        node->loaded   = true;                       // brand new: only . and ..
        folderChains[node] = std::vector<i64>{ blockOff };
        parent->folders.push_back(node);
        pathIndex[ToLowerW(node->fullPath)] = node;
        if (out) *out = node;
        return Result::Ok();
    }

    /// Frees the allocations of everything under `node` (not the entry itself).
    void freeSubtreeSpace(Pk2Node* node) {
        if (node->isFile()) {
            allocations.erase(node->entry.offset);
            return;
        }
        loadChildren(node);
        for (Pk2Node* f : node->files) allocations.erase(f->entry.offset);
        for (Pk2Node* d : node->folders) freeSubtreeSpace(d);
        auto chain = folderChains.find(node);
        if (chain != folderChains.end())
            for (i64 off : chain->second) allocations.erase(off);
    }

    // `dropChains` controls whether the folder block-chain cache is discarded.
    // Removal must drop it (the blocks are freed). Rename must NOT: folderChains
    // is keyed by node POINTER and a rename changes neither the pointers nor any
    // block offset, so the descendants' chains stay valid. Erasing them (the old
    // unconditional behaviour) left buildAllocations blind to those directory
    // blocks, so allocateSpace could hand a live folder block to a later write
    // and destroy every file under a just-renamed subfolder.
    void unlinkSubtreeIndex(Pk2Node* node, bool dropChains = true) {
        pathIndex.erase(ToLowerW(node->fullPath));
        if (node->isFolder()) {
            for (Pk2Node* f : node->files)   unlinkSubtreeIndex(f, dropChains);
            for (Pk2Node* d : node->folders) unlinkSubtreeIndex(d, dropChains);
            if (dropChains) folderChains.erase(node);
        }
    }

    Result removeNode(Pk2Node* node) {
        Result r = requireWritable();
        if (!r.ok()) return r;
        if (!node)          return Result::Fail(Status::InvalidArgument, T(L"Bos dugum."));
        if (node->isRoot()) return Result::Fail(Status::InvalidArgument, T(L"Kok klasor silinemez."));

        r = buildAllocations(ProgressFn());
        if (!r.ok()) return r;

        freeSubtreeSpace(node);

        // Clearing the parent's entry is what actually removes it; the payload
        // bytes stay behind until compact().
        Pk2RawBlock blk;
        r = readBlockAt(node->entry.blockOffset, blk);
        if (!r.ok()) return r;
        const int slot = node->entry.slot;
        if (slot < 0 || slot >= static_cast<int>(kPk2EntriesPerBlock))
            return Result::Fail(Status::BadFormat, T(L"Girdi yuvasi gecersiz."));
        const i64 keepNext = blk.Entries[slot].NextBlock;
        std::memset(&blk.Entries[slot], 0, sizeof(Pk2RawEntry));
        blk.Entries[slot].NextBlock = keepNext;
        r = writeBlockAt(node->entry.blockOffset, blk);
        if (!r.ok()) return r;

        unlinkSubtreeIndex(node);
        Pk2Node* parent = node->parent;
        if (parent) {
            std::vector<Pk2Node*>& list = node->isFolder() ? parent->folders : parent->files;
            list.erase(std::remove(list.begin(), list.end(), node), list.end());
        }
        node->entry.type = Pk2EntryType::Empty;   // the node object itself stays alive
        return Result::Ok();
    }

    void reindexSubtree(Pk2Node* node) {
        pathIndex[ToLowerW(node->fullPath)] = node;
        if (!node->isFolder()) return;
        for (Pk2Node* f : node->files) {
            f->fullPath = JoinPath(node->fullPath, f->name);
            reindexSubtree(f);
        }
        for (Pk2Node* d : node->folders) {
            d->fullPath = JoinPath(node->fullPath, d->name);
            reindexSubtree(d);
        }
    }

    Result renameNode(Pk2Node* node, const std::wstring& newName) {
        Result r = requireWritable();
        if (!r.ok()) return r;
        if (!node)          return Result::Fail(Status::InvalidArgument, T(L"Bos dugum."));
        if (node->isRoot()) return Result::Fail(Status::InvalidArgument, T(L"Kok klasor yeniden adlandirilamaz."));
        if (!IsUsableName(newName))
            return Result::Fail(Status::InvalidArgument, T(L"Gecersiz isim."));
        const std::string ansi = EncodeAnsiName(newName);
        if (ansi.empty() || ansi.size() >= kPk2MaxNameBytes)
            return Result::Fail(Status::InvalidArgument, T(L"Isim cok uzun (88 bayt siniri)."));
        if (node->parent && findChild(node->parent, newName, node->isFolder()))
            return Result::Fail(Status::AlreadyExists, T(L"Bu isim kullaniliyor."));

        Pk2Entry e = node->entry;
        e.name       = newName;
        e.nameAnsi   = ansi;
        e.modifyTime = NowFileTime();
        r = writeSlot(e.blockOffset, e.slot, e);
        if (!r.ok()) return r;

        // Re-key only the path index; keep every folder's chain cache intact
        // (dropChains=false), because a rename moves no blocks.
        unlinkSubtreeIndex(node, /*dropChains=*/false);
        node->entry = e;
        node->name  = newName;
        node->fullPath = JoinPath(node->parent ? node->parent->fullPath : std::wstring(), newName);
        reindexSubtree(node);
        return Result::Ok();
    }

    // =======================================================================
    // Creation
    // =======================================================================
    Result createNewWithCipher(const std::wstring& p, const Blowfish& bf,
                               bool encrypted, const u8* checksumSource,
                               bool overwrite) {
        closeAll();
        if (!overwrite && FileExists(p))
            return Result::Fail(Status::AlreadyExists, T(L"Dosya zaten var: ") + p);
        if (encrypted && !bf.valid())
            return Result::Fail(Status::InvalidArgument, T(L"Sifreli arsiv icin gecerli key gerekli."));

        const std::wstring parent = PathParent(p);
        if (!parent.empty() && !EnsureDirectory(parent))
            return Result::Fail(Status::IoError, T(L"Klasor olusturulamadi: ") + parent);

        if (!file.createNew(p, overwrite))
            return Result::Fail(Status::IoError, T(L"Dosya olusturulamadi: ") + p);

        Pk2RawHeader hdr{};
        std::memset(&hdr, 0, sizeof(hdr));
        auto _sig = Pk2Signature();
        std::memcpy(hdr.Signature, _sig.data(), _sig.size());
        // Verified against real archives: the four version bytes are 02 00 00 01.
        hdr.Version[0] = 0x02; hdr.Version[1] = 0x00;
        hdr.Version[2] = 0x00; hdr.Version[3] = 0x01;
        hdr.Encrypted = encrypted ? 1 : 0;

        if (checksumSource) {
            std::memcpy(hdr.Checksum, checksumSource, sizeof(hdr.Checksum));
        } else if (bf.valid()) {
            std::vector<u8> probe;
            auto _probe = Pk2ChecksumProbe();
            if (bf.encodeString(_probe.c_str(), probe) && probe.size() >= kPk2ChecksumCompareBytes)
                std::memcpy(hdr.Checksum, probe.data(), kPk2ChecksumCompareBytes);
        }

        if (!file.writeAt(0, &hdr, sizeof(hdr))) {
            file.close();
            return Result::Fail(Status::IoError, T(L"Baslik yazilamadi."));
        }

        header          = hdr;
        cipher          = bf;
        path            = p;
        readOnlyMode    = false;
        blocksEncrypted = encrypted;
        opened          = true;
        fileSize        = kPk2HeaderSize;

        const u64 now = NowFileTime();
        Pk2RawBlock root{};
        std::memset(&root, 0, sizeof(root));
        {
            Pk2Entry self;
            self.type = Pk2EntryType::Folder; self.nameAnsi = "."; self.name = L".";
            self.createTime = now; self.modifyTime = now;
            self.offset = static_cast<i64>(kPk2RootBlockOffset);
            Pk2RawEntry raw{};
            EncodeEntry(self, raw);
            root.Entries[0] = raw;
        }
        Result r = writeBlockAt(static_cast<i64>(kPk2RootBlockOffset), root);
        if (!r.ok()) { closeAll(); return r; }

        // Pad out to the 4096-byte chunk every other pk2 tool expects.
        if (fileSize < kTailAlignment) {
            if (!file.setEndOfFile(kTailAlignment)) {
                closeAll();
                return Result::Fail(Status::IoError, T(L"Dosya boyutu ayarlanamadi."));
            }
            fileSize = kTailAlignment;
        }
        file.flush();

        createRootNode();
        rootNode->loaded = true;
        folderChains[rootNode] = std::vector<i64>{ static_cast<i64>(kPk2RootBlockOffset) };

        allocations.clear();
        allocations[0] = kPk2HeaderSize;
        allocations[static_cast<i64>(kPk2RootBlockOffset)] = kPk2BlockSize;
        allocationsBuilt = true;
        return Result::Ok();
    }

    // =======================================================================
    // Compact
    // =======================================================================
    Result copyInto(Impl& dst, Pk2Node* srcFolder, Pk2Node* dstFolder,
                    u64& done, u64 total, const ProgressFn& progress) {
        Result r = loadChildren(srcFolder);
        Result worst = r.ok() ? Result::Ok() : r;

        for (Pk2Node* f : srcFolder->files) {
            Payload p;
            p.src       = &file;
            p.srcOffset = static_cast<u64>(f->entry.offset);
            p.size      = f->entry.size;
            const Result a = dst.addPayloadFile(dstFolder, f->name, p, true);
            if (!a.ok() && worst.ok()) worst = a;
            done += f->entry.size;
            if (!ReportProgress(progress, done, total, f->fullPath.c_str()))
                return Result::Fail(Status::Cancelled);
        }
        for (Pk2Node* d : srcFolder->folders) {
            Pk2Node* newFolder = nullptr;
            const Result a = dst.addFolder(dstFolder, d->name, &newFolder);
            if (!a.ok() && !newFolder) { if (worst.ok()) worst = a; continue; }
            const Result c = copyInto(dst, d, newFolder, done, total, progress);
            if (c.status == Status::Cancelled) return c;
            if (!c.ok() && worst.ok()) worst = c;
        }
        return worst;
    }

    Result compact(u64* savedBytes, const ProgressFn& progress) {
        if (savedBytes) *savedBytes = 0;
        Result r = requireWritable();
        if (!r.ok()) return r;

        const std::wstring original = path;
        const std::wstring newPath  = original + L".new";
        const std::wstring bakPath  = original + L".bak";
        const u64 oldSize = fileSize;

        u64 totalBytes = 0, totalFiles = 0;
        r = measure(rootNode, totalBytes, totalFiles, progress);
        if (r.status == Status::Cancelled) return r;

        u64 newSize = 0;
        {
            Impl dst;
            r = dst.createNewWithCipher(newPath, cipher, blocksEncrypted, header.Checksum, true);
            if (!r.ok()) return r;

            u64 done = 0;
            const Result copied = copyInto(dst, rootNode, dst.rootNode, done, totalBytes, progress);
            dst.file.flush();
            newSize = dst.fileSize;
            dst.closeAll();

            if (copied.status == Status::Cancelled) {
                ::DeleteFileW(newPath.c_str());
                return copied;
            }
            if (!copied.ok()) {
                ::DeleteFileW(newPath.c_str());
                return copied;
            }
        }

        // Swap in the rebuilt file, keeping the original as .bak the whole time.
        const Blowfish keep = cipher;   // closeAll() below must not take it with it
        file.close();
        ::DeleteFileW(bakPath.c_str());
        if (!::MoveFileExW(original.c_str(), bakPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            openFile(original, keep, false);
            ::DeleteFileW(newPath.c_str());
            return Result::Fail(Status::IoError, T(L"Yedek olusturulamadi: ") + bakPath);
        }
        if (!::MoveFileExW(newPath.c_str(), original.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            // Put the original back - the user must never lose the archive.
            ::MoveFileExW(bakPath.c_str(), original.c_str(), MOVEFILE_REPLACE_EXISTING);
            openFile(original, keep, false);
            return Result::Fail(Status::IoError, T(L"Yeni arsiv yerine tasinamadi."));
        }

        r = openFile(original, keep, false);
        if (!r.ok()) return r;

        if (savedBytes) *savedBytes = (oldSize > newSize) ? (oldSize - newSize) : 0;
        return Result::Ok();
    }

    // =======================================================================
    // Diagnostics
    // =======================================================================
    Result diagnose(Pk2Diagnostics& out, const ProgressFn& progress) {
        out = Pk2Diagnostics();
        if (!opened) return Result::Fail(Status::InvalidArgument, T(L"Arsiv acik degil."));

        out.header      = DescribeHeader(header);
        out.archiveSize = fileSize;

        u64 folders = 0, files = 0, payload = 0, largest = 0;
        std::wstring largestPath;

        const Result r = walkFrom(rootNode, [&](Pk2Node* n) {
            if (n->isFile()) {
                ++files;
                payload += n->entry.size;
                if (n->entry.size > largest) { largest = n->entry.size; largestPath = n->fullPath; }
            } else {
                ++folders;
            }
            return true;
        }, progress);
        if (r.status == Status::Cancelled) return r;

        u64 blocks = 0;
        for (const auto& kv : folderChains) blocks += kv.second.size();

        // Empty slots = every slot in every block minus the live entries and
        // the "." / ".." pointers each folder block carries.
        const u64 slots = blocks * kPk2EntriesPerBlock;
        const u64 used  = folders + files + (folders * 2) + 1;
        out.blockCount       = blocks;
        out.folderCount      = folders;
        out.fileCount        = files;
        out.emptyEntryCount  = (slots > used) ? (slots - used) : 0;
        out.totalPayloadBytes = payload;
        out.largestFileSize  = largest;
        out.largestFilePath  = largestPath;

        const u64 live = payload + blocks * kPk2BlockSize + kPk2HeaderSize;
        out.deadBytes = (fileSize > live) ? (fileSize - live) : 0;
        out.fragmentation = fileSize ? (static_cast<double>(out.deadBytes) / static_cast<double>(fileSize)) : 0.0;
        return r;
    }
};

// ===========================================================================
//  Public surface - locking wrappers over Impl
// ===========================================================================
Pk2Archive::Pk2Archive() : m_impl(new Impl()) {}
Pk2Archive::~Pk2Archive() { close(); }

// --- lifetime ---------------------------------------------------------------
Result Pk2Archive::open(const std::wstring& path, const Blowfish& cipher, bool readOnly) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->openFile(path, cipher, readOnly);
}

Result Pk2Archive::ProbeHeader(const std::wstring& path, Pk2RawHeader& out) {
    // Header reading needs no key, so the key search and the diagnostics dialog
    // both depend on it before anything else exists.
    std::memset(&out, 0, sizeof(out));
    File f;
    if (!f.openRead(path))
        return Result::Fail(Status::NotFound, T(L"Dosya acilamadi: ") + path);
    if (f.size() < kPk2HeaderSize + kPk2BlockSize)
        return Result::Fail(Status::Truncated, T(L"Dosya bir PK2 icin cok kucuk."));
    if (!f.readAt(0, &out, sizeof(out)))
        return Result::Fail(Status::IoError, T(L"Baslik okunamadi."));
    return Result::Ok();
}

Pk2HeaderInfo Pk2Archive::DescribeHeader(const Pk2RawHeader& raw) {
    Pk2HeaderInfo info;
    char sig[31] = {};
    std::memcpy(sig, raw.Signature, 30);
    info.signature = MultiByteToWide(std::string(sig), 1252);
    std::memcpy(info.version, raw.Version, 4);
    info.versionText = FormatW(L"%u.%u.%u.%u", raw.Version[3], raw.Version[2],
                               raw.Version[1], raw.Version[0]);
    info.encrypted = raw.Encrypted != 0;
    std::memcpy(info.checksum, raw.Checksum, 16);
    auto _sig = Pk2Signature();
    info.signatureValid = std::memcmp(raw.Signature, _sig.data(),
                                      _sig.size()) == 0;
    return info;
}

bool Pk2Archive::IsSailorArchive(const std::wstring& path) {
    File f;
    if (!f.openRead(path)) return false;
    u8 sig[sizeof(kSailorSignature)] = {};
    if (!f.readAt(0, sig, sizeof(sig))) return false;
    return std::memcmp(sig, kSailorSignature, sizeof(kSailorSignature)) == 0;
}

u64 Pk2Archive::DetectRootBlockOffset(const std::wstring& path) {
    return IsSailorArchive(path) ? kSailorRootBlockOffset : kPk2RootBlockOffset;
}

Result Pk2Archive::ReadRawRootBlock(const std::wstring& path, u8 out[kPk2BlockSize]) {
    // Also real: the strong oracle needs these 2560 encrypted bytes.
    // Auto-detect the root-block offset so a SailSafe archive (root at 0x33E)
    // feeds the oracle the RIGHT block. Every key stage goes through here, so a
    // single detection point covers KeyRing + all KeyFinder stages.
    std::memset(out, 0, kPk2BlockSize);
    const u64 rootOff = DetectRootBlockOffset(path);
    File f;
    if (!f.openRead(path))
        return Result::Fail(Status::NotFound, T(L"Dosya acilamadi: ") + path);
    if (f.size() < rootOff + kPk2BlockSize)
        return Result::Fail(Status::Truncated, T(L"Kok blok icin dosya cok kucuk."));
    if (!f.readAt(rootOff, out, kPk2BlockSize))
        return Result::Fail(Status::IoError, T(L"Kok blok okunamadi."));
    return Result::Ok();
}

Result Pk2Archive::createNew(const std::wstring& path, const Pk2CreateOptions& opt, bool overwrite) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    Blowfish bf;
    if (opt.encrypted) {
        if (!bf.initializeGameKey(opt.asciiKey, opt.baseKey))
            return Result::Fail(Status::InvalidArgument, T(L"Key ile sifre kurulamadi."));
    }
    return m_impl->createNewWithCipher(path, bf, opt.encrypted, nullptr, overwrite);
}

void Pk2Archive::close() {
    if (!m_impl) return;
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->closeAll();
}

bool                Pk2Archive::isOpen()   const { return m_impl->opened; }
bool                Pk2Archive::readOnly() const { return m_impl->readOnlyMode; }
const std::wstring& Pk2Archive::path()     const { return m_impl->path; }
u64                 Pk2Archive::fileSize() const { return m_impl->fileSize; }
const Pk2RawHeader& Pk2Archive::rawHeader() const { return m_impl->header; }
Pk2HeaderInfo       Pk2Archive::headerInfo() const { return DescribeHeader(m_impl->header); }
const Blowfish&     Pk2Archive::cipher()   const { return m_impl->cipher; }

// --- navigation -------------------------------------------------------------
Pk2Node* Pk2Archive::root() const { return m_impl->rootNode; }

Result Pk2Archive::loadChildren(Pk2Node* node) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->loadChildren(node);
}

void Pk2Archive::childrenOf(Pk2Node* node,
                            std::vector<Pk2Node*>& folders,
                            std::vector<Pk2Node*>& files) {
    folders.clear();
    files.clear();
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    if (!node) return;
    folders = node->folders;
    files   = node->files;
}

Result Pk2Archive::findNode(const std::wstring& innerPath, Pk2Node** out) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->findNode(innerPath, out);
}

Result Pk2Archive::findFile(const std::wstring& innerPath, Pk2Node** out) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    Pk2Node* node = nullptr;
    const Result r = m_impl->findNode(innerPath, &node);
    if (!r.ok()) return r;
    if (!node || !node->isFile())
        return Result::Fail(Status::NotFound, T(L"Bu yol bir dosya degil: ") + innerPath);
    if (out) *out = node;
    return Result::Ok();
}

std::wstring Pk2Archive::NodePath(const Pk2Node* node) {
    return node ? node->fullPath : std::wstring();
}

// --- reading ----------------------------------------------------------------
u64 Pk2Archive::skippedEntryCount() const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->skippedEntries;
}

std::vector<std::wstring> Pk2Archive::skippedEntrySamples() const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->skippedSamples;
}

Result Pk2Archive::readFile(const Pk2Node* node, std::vector<u8>& out) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->readInto(node, 0, out);
}

Result Pk2Archive::readFile(const std::wstring& innerPath, std::vector<u8>& out) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    out.clear();
    Pk2Node* node = nullptr;
    const Result r = m_impl->findNode(innerPath, &node);
    if (!r.ok()) return r;
    if (!node || !node->isFile())
        return Result::Fail(Status::NotFound, T(L"Bu yol bir dosya degil: ") + innerPath);
    return m_impl->readInto(node, 0, out);
}

Result Pk2Archive::readFilePrefix(const Pk2Node* node, size_t maxBytes, std::vector<u8>& out) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->readInto(node, maxBytes, out);
}

Result Pk2Archive::readFilePrefix(const std::wstring& innerPath, size_t maxBytes,
                                  std::vector<u8>& out) {
    // Resolve and read under ONE lock so a concurrent close() cannot free the
    // node between find and read (the preview worker calls this off-thread).
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    out.clear();
    Pk2Node* node = nullptr;
    const Result r = m_impl->findNode(innerPath, &node);
    if (!r.ok()) return r;
    if (!node || !node->isFile())
        return Result::Fail(Status::NotFound, T(L"Bu yol bir dosya degil: ") + innerPath);
    return m_impl->readInto(node, maxBytes, out);
}

Result Pk2Archive::extractFile(const Pk2Node* node, const std::wstring& outFilePath) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->extractOne(node, outFilePath);
}

Result Pk2Archive::extractTree(Pk2Node* node, const std::wstring& outDir, const ProgressFn& progress) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->extractTree(node, outDir, progress);
}

Result Pk2Archive::extractAll(const std::wstring& outDir, const ProgressFn& progress) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    if (!m_impl->opened) return Result::Fail(Status::InvalidArgument, T(L"Arsiv acik degil."));
    return m_impl->extractTree(m_impl->rootNode, outDir, progress);
}

// --- searching --------------------------------------------------------------
Result Pk2Archive::walkAll(const std::function<bool(Pk2Node*)>& visit, const ProgressFn& progress) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    if (!m_impl->opened) return Result::Fail(Status::InvalidArgument, T(L"Arsiv acik degil."));
    return m_impl->walkFrom(m_impl->rootNode, visit, progress);
}

Result Pk2Archive::search(const std::wstring& needle, std::vector<Pk2Node*>& results,
                          size_t maxResults, const ProgressFn& progress) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    results.clear();
    if (!m_impl->opened) return Result::Fail(Status::InvalidArgument, T(L"Arsiv acik degil."));
    if (needle.empty()) return Result::Ok();

    bool hitLimit = false;
    const Result r = m_impl->walkFrom(m_impl->rootNode, [&](Pk2Node* n) {
        // Shared rule: plain substring, ".ext", or a glob like "srokim*.bsr".
        if (FilterMatches(n->name, n->fullPath, needle)) {
            results.push_back(n);
            if (maxResults && results.size() >= maxResults) { hitLimit = true; return false; }
        }
        return true;
    }, progress);
    // Stopping because the cap was reached is a success, not a cancellation.
    if (hitLimit && r.status == Status::Cancelled) return Result::Ok();
    return r;
}

Result Pk2Archive::countEntries(bool deep, u64& folders, u64& files, const ProgressFn& progress) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    folders = 0; files = 0;
    if (!m_impl->opened) return Result::Fail(Status::InvalidArgument, T(L"Arsiv acik degil."));

    if (deep) {
        u64 fo = 0, fi = 0;
        const Result r = m_impl->walkFrom(m_impl->rootNode, [&](Pk2Node* n) {
            if (n->isFile()) ++fi; else ++fo;
            return true;
        }, progress);
        folders = fo; files = fi;
        return r;
    }

    // Shallow: whatever has been materialised so far.
    for (const std::unique_ptr<Pk2Node>& n : m_impl->nodes) {
        if (n.get() == m_impl->rootNode) continue;
        if (n->entry.isEmpty()) continue;
        if (n->isFile()) ++files; else ++folders;
    }
    return Result::Ok();
}

// --- writing ----------------------------------------------------------------
Result Pk2Archive::addFile(Pk2Node* parentFolder, const std::wstring& name,
                           const u8* data, size_t size, bool overwrite) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    if (size && !data) return Result::Fail(Status::InvalidArgument, T(L"Veri isaretcisi bos."));
    Impl::Payload p;
    p.mem  = data;
    p.size = size;
    return m_impl->addPayloadFile(parentFolder, name, p, overwrite);
}

Result Pk2Archive::importFile(Pk2Node* parentFolder, const std::wstring& diskPath,
                              const std::wstring& nameOverride, bool overwrite,
                              const ProgressFn& progress) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    if (!FileExists(diskPath))
        return Result::Fail(Status::NotFound, T(L"Dosya bulunamadi: ") + diskPath);

    const u64 size = GetFileSize64(diskPath);
    if (size > 0xFFFFFFFFull)
        return Result::Fail(Status::InvalidArgument, T(L"Dosya 4 GB sinirini asiyor."));

    File src;
    if (!src.openRead(diskPath))
        return Result::Fail(Status::IoError, T(L"Dosya acilamadi: ") + diskPath);

    Impl::Payload p;
    p.src       = &src;
    p.srcOffset = 0;
    p.size      = size;

    const std::wstring name = nameOverride.empty() ? PathFileName(diskPath) : nameOverride;
    const Result r = m_impl->addPayloadFile(parentFolder, name, p, overwrite);
    ReportProgress(progress, size, size, name.c_str());
    return r;
}

Result Pk2Archive::importFolder(Pk2Node* parentFolder, const std::wstring& diskDir,
                                const ProgressFn& progress) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    if (!DirectoryExists(diskDir))
        return Result::Fail(Status::NotFound, T(L"Klasor bulunamadi: ") + diskDir);
    if (!parentFolder || !parentFolder->isFolder())
        return Result::Fail(Status::InvalidArgument, T(L"Hedef bir klasor degil."));

    Result worst = Result::Ok();

    for (const std::wstring& f : ListDirectory(diskDir, L"*", true, false)) {
        File src;
        if (!src.openRead(f)) { if (worst.ok()) worst = Result::Fail(Status::IoError, f); continue; }
        Impl::Payload p;
        p.src       = &src;
        p.srcOffset = 0;
        p.size      = GetFileSize64(f);
        if (p.size > 0xFFFFFFFFull) { if (worst.ok()) worst = Result::Fail(Status::InvalidArgument, f); continue; }
        const Result r = m_impl->addPayloadFile(parentFolder, PathFileName(f), p, true);
        if (!r.ok() && worst.ok()) worst = r;
        if (!ReportProgress(progress, 0, 0, f.c_str())) return Result::Fail(Status::Cancelled);
    }

    for (const std::wstring& d : ListDirectory(diskDir, L"*", false, true)) {
        Pk2Node* sub = nullptr;
        const Result a = m_impl->addFolder(parentFolder, PathFileName(d), &sub);
        if (!sub) { if (worst.ok()) worst = a; continue; }
        // Recurse without re-locking (recursive_mutex makes this safe anyway).
        const Result r = importFolder(sub, d, progress);
        if (r.status == Status::Cancelled) return r;
        if (!r.ok() && worst.ok()) worst = r;
    }
    return worst;
}

Result Pk2Archive::addFolder(Pk2Node* parentFolder, const std::wstring& name, Pk2Node** out) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->addFolder(parentFolder, name, out);
}

Result Pk2Archive::remove(Pk2Node* node) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->removeNode(node);
}

Result Pk2Archive::rename(Pk2Node* node, const std::wstring& newName) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->renameNode(node, newName);
}

Result Pk2Archive::compact(u64* savedBytes, const ProgressFn& progress) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->compact(savedBytes, progress);
}

Result Pk2Archive::flush() {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    if (!m_impl->opened) return Result::Fail(Status::InvalidArgument, T(L"Arsiv acik degil."));
    return m_impl->file.flush() ? Result::Ok()
                                : Result::Fail(Status::IoError, T(L"Disk'e yazilamadi."));
}

// --- diagnostics ------------------------------------------------------------
Result Pk2Archive::diagnose(Pk2Diagnostics& out, const ProgressFn& progress) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->diagnose(out, progress);
}

// --- low level --------------------------------------------------------------
Result Pk2Archive::readBlock(i64 blockOffset, Pk2RawBlock& out) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->readBlockAt(blockOffset, out);
}

Result Pk2Archive::writeBlock(i64 blockOffset, const Pk2RawBlock& in) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->writeBlockAt(blockOffset, in);
}

bool Pk2Archive::DecodeEntry(const Pk2RawEntry& raw, i64 blockOffset, int slot, Pk2Entry& out) {
    out = Pk2Entry();
    if (raw.Type > 2) return false;
    out.type = static_cast<Pk2EntryType>(raw.Type);

    char name[kPk2MaxNameBytes + 1] = {};
    std::memcpy(name, raw.Name, kPk2MaxNameBytes);
    name[kPk2MaxNameBytes] = '\0';
    out.nameAnsi   = name;
    out.name       = DecodeAnsiName(out.nameAnsi);
    out.createTime = raw.CreateTime;
    out.modifyTime = raw.ModifyTime;
    out.offset     = raw.Offset;
    out.size       = raw.Size;
    out.nextBlock  = raw.NextBlock;
    out.blockOffset = blockOffset;
    out.slot        = slot;
    return true;
}

bool Pk2Archive::EncodeEntry(const Pk2Entry& in, Pk2RawEntry& out) {
    std::memset(&out, 0, sizeof(out));
    const std::string ansi = in.nameAnsi.empty() ? EncodeAnsiName(in.name) : in.nameAnsi;
    if (ansi.size() >= kPk2MaxNameBytes) return false;   // must fit with the NUL
    out.Type = static_cast<u8>(in.type);
    if (!ansi.empty()) std::memcpy(out.Name, ansi.data(), ansi.size());
    out.CreateTime = in.createTime;
    out.ModifyTime = in.modifyTime;
    out.Offset     = in.offset;
    out.Size       = in.size;
    out.NextBlock  = in.nextBlock;
    return true;
}

} // namespace sro
