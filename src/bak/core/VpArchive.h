// ============================================================================
//  SRO Asset Studio - core/VpArchive.h
//
//  READ + WRITE backend for the Electus / "VSROPLUS TOOLS ENCRYPTED CONTAINER"
//  (.vp) format, version 2.4.  Feature parity with Pk2Archive: browse, extract,
//  import, delete, rename and compact all work the same way.
//
//  KEY DISCOVERY IS AUTOMATIC AND HARDCODES NOTHING.  Like the PK2/LIB search:
//  the RSA private key is harvested from the client binaries next to the
//  archive; the container's own header blob is RSA-unwrapped to the per-license
//  key; the directory key is HMAC-SHA256(perLicenseKey, "<NAME>_CONTAINER");
//  and the AES-256-GCM tag is the self-validating oracle - a wrong key can
//  never authenticate.
//
//  WRITING keeps the container valid: after any mutation the directory is
//  re-serialised, encrypted with AES-256-GCM under the same key, and re-signed
//  with HMAC-SHA256.  The signing key is itself *verified against the
//  container's existing signature* at open time, so we only enable writing when
//  we can reproduce a signature the game will accept.
//
//  Layout:
//    header : i16 len + id("VSROPLUS TOOLS ENCRYPTED CONTAINER"),
//             i16 len + version, i32 256 + RSA(aesKey), i32 256 + RSA(pwd),
//             u8 isFullEncrypted, i64 metaOffset, i32 metaSize, i32 reservedSize
//    meta   : i32 sigLen(=32), sig[32], AES-GCM( i12 nonce | ct | i16 tag )
//    plain  : i32 marker(=-1 -> i32=1, i32 count | else count), then per file
//             i32 id, i16 pathLen, path, i64 size, i64 c, i64 m, i64 chunkOff,
//             u8 folder, u8 encrypted, [hash]; then a ChunkMap.
//    data   : raw bytes at chunkOffset..+size.
// ============================================================================
#pragma once

#include "IArchive.h"
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sro {

class VpArchive : public IArchive {
public:
    VpArchive();
    ~VpArchive() override;

    VpArchive(const VpArchive&) = delete;
    VpArchive& operator=(const VpArchive&) = delete;

    /// Sniff the first bytes of a file: true when it is a VSROPLUS container.
    static bool IsVpContainer(const std::wstring& path);

    /// Open the container, auto-discovering the key.  `log` (may be empty)
    /// receives Turkish progress lines.  When `readOnly` is false the file is
    /// opened for writing; if that is impossible (or the signature could not be
    /// reproduced) the archive silently falls back to read-only.
    Result open(const std::wstring& path, const LogFn& log, bool readOnly = false);

    // --- IArchive ----------------------------------------------------------
    ArchiveKind kind()     const override { return ArchiveKind::Vp; }
    bool        canWrite() const override;

    bool                isOpen()   const override;
    bool                readOnly() const override;
    const std::wstring& path()     const override;
    u64                 fileSize() const override;
    Pk2HeaderInfo       headerInfo() const override;

    Pk2Node* root() const override;
    Result   loadChildren(Pk2Node* node) override;              // eager: no-op
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

    // --- writing -----------------------------------------------------------
    Result addFile(Pk2Node* parentFolder, const std::wstring& name,
                   const u8* data, size_t size, bool overwrite) override;
    Result importFile(Pk2Node* parentFolder, const std::wstring& diskPath,
                      const std::wstring& nameOverride, bool overwrite,
                      const ProgressFn& progress) override;
    Result importFolder(Pk2Node* parentFolder, const std::wstring& diskDir,
                        const ProgressFn& progress) override;
    Result addFolder(Pk2Node* parentFolder, const std::wstring& name, Pk2Node** out) override;
    Result remove(Pk2Node* node) override;
    Result rename(Pk2Node* node, const std::wstring& newName) override;
    Result compact(u64* savedBytes, const ProgressFn& progress) override;
    Result flush() override;

    void close() override;

private:
    /// One directory record, exactly what the metadata stores per file.
    struct Rec {
        int          id = 0;
        std::wstring path;          ///< inner path, backslash separated
        i64          size = 0;
        i64          created = 0;   ///< .NET ticks
        i64          modified = 0;
        i64          chunkOffset = 0;
        bool         encrypted = false;
        bool         hasHash = false;
        u8           hash[32] = {};
    };
    struct Chunk { i64 offset = 0; i64 size = 0; bool free = false; };

    // tree ------------------------------------------------------------------
    void     rebuildTree();
    Pk2Node* intern(const std::wstring& folderPath);

    // Path-based write helpers. Every mutation rebuilds the tree and therefore
    // invalidates all Pk2Node*, so internal call chains must pass paths.
    Result addFileAtPath(const std::wstring& folder, const std::wstring& name,
                         const u8* data, size_t size, bool overwrite);
    Result importFileToPath(const std::wstring& folder, const std::wstring& diskPath,
                            const std::wstring& name, bool overwrite);

    // io ---------------------------------------------------------------------
    Result readRaw(i64 off, i64 size, std::vector<u8>& out) const;
    Result writeRaw(i64 off, const u8* data, i64 size);

    // allocation --------------------------------------------------------------
    i64  allocate(i64 size);            ///< reuse a free chunk or append at EOF
    void release(i64 offset, i64 size); ///< give a chunk back
    void defragment();

    // metadata ---------------------------------------------------------------
    void   serializeMetadata(std::vector<u8>& out) const;
    Result saveMetadata();              ///< encrypt + sign + write + patch header
    Result requireWritable() const;

    // payload key ------------------------------------------------------------
    /// Decide which keystream key decrypts THIS container's file bytes, proven by
    /// trial-decrypting one file to a known signature. Tries the toolkit constant
    /// then the session vault. Sets m_xorKey + m_payloadKeyReady.
    void resolvePayloadKey();
    /// Does `key` decrypt a sample file of this container to a plausible plaintext?
    bool payloadKeyDecrypts(const u8 key[32]) const;

public:
    /// Install a payload keystream key discovered externally (e.g. observed from a
    /// running client). Cached in the session vault under this container's id and
    /// applied immediately if it validates. Returns true when accepted.
    bool installPayloadKey(const u8 key[32]);
    /// True once file bytes can be decrypted (directory always opens regardless).
    bool payloadKeyReady() const { return m_payloadKeyReady; }
    /// This container's KDF context id ("MAP_CONTAINER" etc.), for key discovery.
    const std::string& containerId() const { return m_containerId; }
    /// Whether payloads are encrypted at all (IsFullEncrypted or any per-file flag).
    bool payloadEncrypted() const { return m_fullEncrypted; }
    /// Collect payload cribs (known-plaintext keystream prefixes) from files whose
    /// extension has a known signature. Used by L33T payload-key recovery.
    bool buildPayloadCribs(std::vector<struct PayloadCrib>& out) const;
    /// This container's 32-byte metadataKey (HMAC-SHA256(perLicenseKey, containerId)),
    /// computed offline at open. In the client's security-context object the payload
    /// xorKey sits exactly 64 bytes after this key, so finding the metadataKey in RAM
    /// and reading +64 recovers the xorKey with no crib/signature guess (works for
    /// every container, incl. Settings). Returns false if not resolved.
    bool metadataKey(u8 out[32]) const;
private:

    int  nextId() const;
    Rec* recForPath(const std::wstring& innerPath);

    // Recursive so the by-path readers can hold ONE lock across findNode +
    // record lookup + readRaw. A plain mutex forced find and read into separate
    // critical sections, letting close() free the node in the gap (data race +
    // use-after-free on the preview worker thread).
    mutable std::recursive_mutex m_mtx;
    std::wstring       m_path;
    void*              m_handle = nullptr;    ///< HANDLE
    u64                m_fileSize = 0;
    bool               m_readOnly = true;
    std::wstring       m_version;
    std::string        m_containerId;

    u8   m_metaKey[32] = {};
    u8   m_signKey[32] = {};
    u8   m_xorKey[32]  = {};                  ///< payload keystream key
    bool m_payloadKeyReady = false;           ///< true once m_xorKey actually decrypts payloads
    bool m_fullEncrypted = false;             ///< container-wide payload encryption
    bool m_canSign = false;                   ///< signature reproducible -> writable
    i64  m_metaFieldPos = 0;                  ///< file offset of the i64 metaOffset field
    i64  m_metaOffset = 0;
    i64  m_metaSize = 0;
    i64  m_metaReserved = 0;
    bool m_writeHashes = false;               ///< container stores per-file SHA-256

    std::vector<Rec>   m_recs;
    std::vector<Chunk> m_chunks;

    std::deque<Pk2Node> m_nodes;              ///< stable storage; [0] is the root
    std::unordered_map<std::wstring, Pk2Node*> m_byPath;   ///< lower(fullPath) -> node
    std::unordered_map<const Pk2Node*, size_t> m_fileRec;  ///< node -> index into m_recs
    std::vector<std::wstring> m_emptyFolders; ///< folders with no files yet (session only)
    u64 m_folderCount = 0;
    u64 m_fileCount   = 0;
};

} // namespace sro
