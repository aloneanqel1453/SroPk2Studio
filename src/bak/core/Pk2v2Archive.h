// ============================================================================
//  SRO Asset Studio - core/Pk2v2Archive.h
//
//  READ backend for the Sailor "Arabian Dawn" PK2v2 container (the new format
//  that replaced the old SailSafe "DJ&-" / Blowfish@0x33E wrapper on the
//  Sept-30-2026 client). It is a THIRD archive variant alongside JMXPACK
//  (Pk2Archive) and VSROPLUS (VpArchive) and materialises the SAME Pk2Node tree,
//  so the UI / preview / extract paths need no per-format code.
//
//  FORMAT (all reverse-engineered from SRO_Client.exe and validated):
//    header 0x80 : "PK2v2\0\0\0", ver=2, flags (bit0 ChaCha, bit1 ZSTD, bit2
//                  verify), dirOffset(0x18), dirCipherLen(0x20), dirPlain(0x28),
//                  q16 salt(0x40), verifyTag(0x50), nonceWord1(0x64).
//    directory   : ChaCha20(key,nonce).decrypt(file[dirOff:dirOff+ctLen]) then
//                  ZSTD decompress -> a "2LBT" blob:
//                    u32 magic, u32 count, u32 poolSize, reserved; then
//                    count x 0x40-byte entry records; then a string pool.
//                  entry: +0x08 u64 offset, +0x10 u64 size, +0x30 i32 parentIdx
//                  (-1=root), +0x34 u32 nameOff, +0x38 u16 nameLen, +0x3a u8 type
//                  (1=folder,2=file), +0x3b u8 flag.
//    key         : PBKDF2-HMAC-SHA256(password, q16||"PK2v2-key"||0*7, 4096, 32)
//                  verified by HMAC(key,"PK2v2-check")[:12] == header verifyTag.
//
//  The per-client PASSWORD is a RUNTIME secret (proven not recoverable from the
//  static client files - it reaches the client's own open routine as an argument
//  through a vtable call, not as an embedded string). It is supplied to open():
//  via the explicit argument, PK2V2_PASSWORD, or the obfuscated candidate table
//  in Pk2v2KeyTable.h (empty until a value is known). File PAYLOADS are not
//  directory-encrypted; any per-asset cipher is handled by AssetCrypt as usual.
// ============================================================================
#pragma once

#include "IArchive.h"
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sro {

class Pk2v2Archive : public IArchive {
public:
    Pk2v2Archive();
    ~Pk2v2Archive() override;
    Pk2v2Archive(const Pk2v2Archive&) = delete;
    Pk2v2Archive& operator=(const Pk2v2Archive&) = delete;

    /// Sniff the first 8 bytes: true when the file begins "PK2v2\0\0\0".
    static bool IsPk2v2(const std::wstring& path);

    /// Open read-only, decrypting + parsing the directory. `password` may be
    /// empty: open() then tries PK2V2_PASSWORD, then the Pk2v2KeyTable candidates,
    /// and fails with Status::WrongKey (a clear message) if none verifies. `log`
    /// may be empty; it receives Turkish progress lines.
    Result open(const std::wstring& path, const std::string& password,
                const LogFn& log);

    // --- IArchive ----------------------------------------------------------
    ArchiveKind kind()     const override { return ArchiveKind::Pk2; } // shares the Pk2 node model
    bool        canWrite() const override { return false; }            // read-only backend

    bool                isOpen()   const override;
    bool                readOnly() const override { return true; }
    const std::wstring& path()     const override;
    u64                 fileSize() const override;
    Pk2HeaderInfo       headerInfo() const override;

    Pk2Node* root() const override;
    Result   loadChildren(Pk2Node* node) override;          // eager: no-op
    void     childrenOf(Pk2Node* node, std::vector<Pk2Node*>& folders,
                        std::vector<Pk2Node*>& files) override;
    Result   findNode(const std::wstring& innerPath, Pk2Node** out) override;
    Result   findFile(const std::wstring& innerPath, Pk2Node** out) override;

    Result readFile(const Pk2Node* node, std::vector<u8>& out) const override;
    Result readFile(const std::wstring& innerPath, std::vector<u8>& out) override;
    Result readFilePrefix(const Pk2Node* node, size_t maxBytes, std::vector<u8>& out) const override;
    Result readFilePrefix(const std::wstring& innerPath, size_t maxBytes, std::vector<u8>& out) override;
    Result extractFile(const Pk2Node* node, const std::wstring& outFilePath) const override;
    Result extractTree(Pk2Node* node, const std::wstring& outDir, const ProgressFn& progress) override;
    Result extractAll(const std::wstring& outDir, const ProgressFn& progress) override;

    Result walkAll(const std::function<bool(Pk2Node*)>& visit, const ProgressFn& progress) override;
    Result search(const std::wstring& needle, std::vector<Pk2Node*>& results,
                  size_t maxResults, const ProgressFn& progress) override;
    Result countEntries(bool deep, u64& folders, u64& files, const ProgressFn& progress) override;

    // --- writing: not supported (read-only backend) ------------------------
    Result addFile(Pk2Node*, const std::wstring&, const u8*, size_t, bool) override;
    Result importFile(Pk2Node*, const std::wstring&, const std::wstring&, bool, const ProgressFn&) override;
    Result importFolder(Pk2Node*, const std::wstring&, const ProgressFn&) override;
    Result addFolder(Pk2Node*, const std::wstring&, Pk2Node**) override;
    Result remove(Pk2Node*) override;
    Result rename(Pk2Node*, const std::wstring&) override;
    Result compact(u64*, const ProgressFn&) override;
    Result flush() override;

    void close() override;

private:
    struct Rec {
        std::wstring path;
        u64 offset = 0;
        u64 compSize = 0;
        u64 plainSize = 0;
        u64 nonce = 0;
        u8 flags = 0;
        bool folder = false;
    };

    void     rebuildTree();
    Pk2Node* intern(const std::wstring& folderPath);
    Result   readRaw(u64 off, u64 size, std::vector<u8>& out) const;

    mutable std::recursive_mutex m_mtx;
    std::wstring m_path;
    void*        m_handle = nullptr;   // HANDLE
    u64          m_fileSize = 0;
    bool         m_open = false;

    u8           m_key[32] = {};
    std::vector<Rec> m_recs;

    std::deque<Pk2Node> m_nodes;       // [0] is root
    std::unordered_map<std::wstring, Pk2Node*> m_byPath;
    std::unordered_map<const Pk2Node*, size_t> m_fileRec;
    u64 m_folderCount = 0;
    u64 m_fileCount   = 0;
};

} // namespace sro
