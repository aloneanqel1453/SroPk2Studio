// ============================================================================
//  SRO Asset Studio - core/VpArchive.cpp   (see VpArchive.h)
// ============================================================================
#include "VpArchive.h"
#include "VpCrypto.h"
#include "VpFormat.h"
#include "L33tPayloadKey.h"
#include "Obfuscate.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <array>

namespace sro {

// ---------------------------------------------------------------------------
// small local helpers
// ---------------------------------------------------------------------------
namespace {

// The container family ships under more than one magic string. The layout and
// the whole crypto chain are identical (see [[electus-vp-container]] and the
// L33T variant); only the signature text differs. We accept every known magic.
// The magic texts are XOR-obfuscated (Obfuscate.h) so neither signature sits in
// the exe as plaintext; they are rebuilt to their exact original value at run
// time and compared as before.
inline std::string vpMagic()     { return OBF("VSROPLUS TOOLS ENCRYPTED CONTAINER").str(); }
inline std::string vpMagicL33T() { return OBF("L33T ENCRYPTED CONTAINER").str(); }

// Return the magic length if `buf` (>= got bytes) starts with a known container
// signature (Int16 len + text), else 0. Also reports which family matched.
inline size_t matchContainerMagic(const u8* buf, size_t got) {
    if (got < 2) return 0;
    u16 idlen; std::memcpy(&idlen, buf, 2);
    if (got < (size_t)2 + idlen) return 0;
    const std::string m1 = vpMagic();
    if (idlen == (u16)m1.size() &&
        std::memcmp(buf + 2, m1.data(), idlen) == 0) return idlen;
    const std::string m2 = vpMagicL33T();
    if (idlen == (u16)m2.size() &&
        std::memcmp(buf + 2, m2.data(), idlen) == 0) return idlen;
    return 0;
}

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}
std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::wstring dirOf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"." : p.substr(0, i);
}
std::wstring baseNoExt(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/"); std::wstring b = (s == std::wstring::npos) ? p : p.substr(s + 1);
    size_t d = b.find_last_of(L'.');
    return d == std::wstring::npos ? b : b.substr(0, d);
}
std::wstring fileNameOf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

/// Shared per-license key, recovered once per session so containers 2..5 open
/// instantly. Memory only - never written to disk.
std::mutex        g_plkMtx;
std::vector<u8>   g_plk;

/// Session-only vault of payload keystream keys, keyed by containerId
/// ("MAP_CONTAINER" -> 32-byte xorKey). L33T uses a distinct xorKey per container
/// that cannot be derived from the archive; once one is observed (from a running
/// client) it is cached here for the rest of the session. RULE 1: never persisted.
std::mutex g_xorMtx;
std::map<std::string, std::array<u8,32>> g_xorVault;

bool xorVaultGet(const std::string& cid, u8 out[32]) {
    std::lock_guard<std::mutex> g(g_xorMtx);
    auto it = g_xorVault.find(cid);
    if (it == g_xorVault.end()) return false;
    std::memcpy(out, it->second.data(), 32);
    return true;
}
void xorVaultPut(const std::string& cid, const u8 key[32]) {
    std::lock_guard<std::mutex> g(g_xorMtx);
    std::array<u8,32> a; std::memcpy(a.data(), key, 32);
    g_xorVault[cid] = a;
}

bool readAllH(HANDLE h, void* buf, DWORD n) {
    DWORD got = 0; return ReadFile(h, buf, n, &got, nullptr) && got == n;
}

/// RAII for a Win32 HANDLE so no error path can leak it.
struct HandleGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit HandleGuard(HANDLE v) : h(v) {}
    ~HandleGuard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    HANDLE release() { HANDLE t = h; h = INVALID_HANDLE_VALUE; return t; }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
};

i64 nowTicks() {
    FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    // FILETIME epoch 1601-01-01 == .NET ticks epoch 0001-01-01 + 504911232000000000
    return (i64)u.QuadPart + 504911232000000000ll;
}

} // namespace

// ---------------------------------------------------------------------------
std::wstring IArchive::NodePath(const Pk2Node* node) {
    return node ? node->fullPath : std::wstring();
}

// ---------------------------------------------------------------------------
VpArchive::VpArchive() = default;
VpArchive::~VpArchive() { close(); }

bool VpArchive::IsVpContainer(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    HandleGuard hg(h);
    u8 buf[64] = {}; DWORD got = 0;
    if (!ReadFile(h, buf, sizeof(buf), &got, nullptr)) return false;
    return matchContainerMagic(buf, got) != 0;
}

// ---------------------------------------------------------------------------
// Gather candidate client binaries around the archive (for RSA-key harvest).
// ---------------------------------------------------------------------------
static void collectBinaries(const std::wstring& dir, int depth,
                            std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0) collectBinaries(full, depth - 1, out);
        } else {
            std::wstring lo = lower(name);
            bool bin = lo.size() > 4 &&
                       (lo.compare(lo.size() - 4, 4, L".dll") == 0 ||
                        lo.compare(lo.size() - 4, 4, L".exe") == 0);
            u64 sz = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (bin && sz > 0 && sz < 80ull * 1024 * 1024) out.push_back(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int keyBinScore(const std::wstring& p) {
    std::wstring lo = lower(p);
    int s = 0;
    if (lo.find(L"\\dump\\") != std::wstring::npos)        s += 40;
    if (lo.find(L"gfxfilemanager") != std::wstring::npos)  s += 30;
    if (lo.find(L"electus") != std::wstring::npos)         s += 20;
    if (lo.find(L"silkroad") != std::wstring::npos)        s += 10;
    if (lo.find(L"sro_client") != std::wstring::npos)      s += 8;
    return s;
}

// ---------------------------------------------------------------------------
Result VpArchive::open(const std::wstring& path, const LogFn& log, bool readOnly) {
    close();
    std::lock_guard<std::recursive_mutex> lk(m_mtx);

    // Prefer read/write so import/delete/compact work; fall back to read-only
    // when the file is locked or marked read-only on disk.
    bool ro = readOnly;
    HANDLE raw = INVALID_HANDLE_VALUE;
    if (!ro) {
        raw = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (raw == INVALID_HANDLE_VALUE) {
        raw = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (raw == INVALID_HANDLE_VALUE)
            return Result::Fail(Status::IoError, T(L"Dosya acilamadi: ") + path);
        ro = true;
    }
    HandleGuard hg(raw);

    LARGE_INTEGER li{};
    if (!GetFileSizeEx(raw, &li)) return Result::Fail(Status::IoError, T(L"Boyut okunamadi."));
    const u64 fileSize = (u64)li.QuadPart;

    // --- header (every read bounds-checked against what we actually read) ---
    u8 hdr[700] = {}; DWORD got = 0;
    if (!ReadFile(raw, hdr, sizeof(hdr), &got, nullptr) || got < 64)
        return Result::Fail(Status::Truncated, T(L"Baslik cok kisa."));
    const size_t lim = got;
    size_t o = 0;
    auto need = [&](size_t k) { return o + k <= lim; };

    if (!need(2)) return Result::Fail(Status::BadFormat, T(L"Bozuk baslik."));
    const size_t magicLen = matchContainerMagic(hdr, lim);
    if (magicLen == 0)
        return Result::Fail(Status::BadFormat, T(L"Konteyner imzasi yok."));
    u16 idlen; std::memcpy(&idlen, hdr + o, 2); o += 2;
    o += idlen;  // magic already validated by matchContainerMagic

    if (!need(2)) return Result::Fail(Status::BadFormat, T(L"Bozuk surum alani."));
    u16 vlen; std::memcpy(&vlen, hdr + o, 2); o += 2;
    if (vlen > 64 || !need(vlen)) return Result::Fail(Status::BadFormat, T(L"Bozuk surum alani."));
    m_version = fromUtf8(std::string((const char*)hdr + o, vlen)); o += vlen;

    if (!need(4)) return Result::Fail(Status::BadFormat, T(L"Bozuk RSA blob1."));
    int l1; std::memcpy(&l1, hdr + o, 4); o += 4;
    if (l1 <= 0 || l1 > 512 || !need((size_t)l1))
        return Result::Fail(Status::BadFormat, T(L"Bozuk RSA blob1."));
    std::vector<u8> blob1(hdr + o, hdr + o + l1); o += l1;

    if (!need(4)) return Result::Fail(Status::BadFormat, T(L"Bozuk RSA blob2."));
    int l2; std::memcpy(&l2, hdr + o, 4); o += 4;
    if (l2 < 0 || l2 > 512 || !need((size_t)l2))
        return Result::Fail(Status::BadFormat, T(L"Bozuk RSA blob2."));
    o += (size_t)l2;                       // password blob - unused for v2.4

    if (!need(1 + 8 + 4 + 4)) return Result::Fail(Status::BadFormat, T(L"Bozuk baslik alanlari."));
    const bool isFullEnc = hdr[o] != 0;     // container-wide payload encryption
    o += 1;
    m_metaFieldPos = (i64)o;
    i64 metaOffset; std::memcpy(&metaOffset, hdr + o, 8); o += 8;
    int metaSize;   std::memcpy(&metaSize,   hdr + o, 4); o += 4;
    int metaReserved; std::memcpy(&metaReserved, hdr + o, 4); o += 4;

    const i64 headerLen = m_metaFieldPos + 16;
    if (metaSize < 28 || metaOffset < headerLen ||
        (u64)metaOffset + 4 + 32 + (u64)metaSize > fileSize)
        return Result::Fail(Status::BadFormat, T(L"Metadata konumu gecersiz."));

    // --- container id (the KDF context) ------------------------------------
    // It is NOT stored in the file: the client passes "<NAME>_CONTAINER". The
    // file name is the natural guess, but a renamed/copied container must still
    // open, so we keep the standard set as fallbacks. Every candidate is proven
    // by the AES-GCM tag, so a wrong one can never be accepted.
    std::wstring up = baseNoExt(path);
    for (wchar_t& c : up) c = (wchar_t)towupper(c);
    const std::string kContainerSfx = OBF("_CONTAINER").str();
    std::vector<std::string> idCandidates{ toUtf8(up) + kContainerSfx };
    for (const std::string& n : { OBF("MEDIA").str(), OBF("DATA").str(), OBF("MAP").str(),
                                  OBF("MUSIC").str(), OBF("PARTICLES").str(), OBF("SETTINGS").str() }) {
        std::string cand = n + kContainerSfx;
        if (cand != idCandidates[0]) idCandidates.push_back(std::move(cand));
    }

    // --- read signature + encrypted metadata --------------------------------
    li.QuadPart = metaOffset;
    if (!SetFilePointerEx(raw, li, nullptr, FILE_BEGIN))
        return Result::Fail(Status::IoError, T(L"Konumlandirma hatasi."));
    int sigLen = 0;
    if (!readAllH(raw, &sigLen, 4) || sigLen < 0 || sigLen > 4096)
        return Result::Fail(Status::BadFormat, T(L"Bozuk metadata imzasi."));
    std::vector<u8> sig((size_t)sigLen);
    if (sigLen && !readAllH(raw, sig.data(), (DWORD)sigLen))
        return Result::Fail(Status::Truncated, T(L"Metadata imzasi eksik."));
    std::vector<u8> enc((size_t)metaSize);
    if (!readAllH(raw, enc.data(), (DWORD)metaSize))
        return Result::Fail(Status::Truncated, T(L"Metadata eksik."));

    const u8* nonce = enc.data();
    const u8* ct    = enc.data() + 12;
    const size_t ctLen = (size_t)metaSize - 12 - 16;
    const u8* tag   = enc.data() + 12 + ctLen;

    // --- discover the key (the GCM tag self-validates every candidate) ------
    u8 metaKey[32] = {};
    auto tryKey = [&](const std::vector<u8>& plk, std::vector<u8>& plainOut) -> bool {
        if (plk.size() != 32) return false;
        for (const std::string& cid : idCandidates) {
            u8 k[32];
            if (!vp::HmacSha256(plk.data(), plk.size(),
                                (const u8*)cid.data(), cid.size(), k)) continue;
            if (!vp::AesGcmDecrypt(k, nonce, 12, ct, ctLen, tag, 16, plainOut)) continue;
            std::memcpy(metaKey, k, 32);
            m_containerId = cid;
            return true;
        }
        return false;
    };

    std::vector<u8> plain;
    bool solved = false;
    std::vector<u8> binWithPem;         ///< kept so we can also learn the signing context

    {
        std::lock_guard<std::mutex> g(g_plkMtx);
        if (!g_plk.empty() && tryKey(g_plk, plain)) solved = true;
    }

    // The toolkit's own RSA key is the reliable path (protected clients carry no
    // plaintext PEM), but anything discovered from THIS game's binaries wins, so
    // a re-keyed toolkit still opens.
    if (!solved) {
        std::vector<std::wstring> bins;
        collectBinaries(dirOf(path), 2, bins);
        std::stable_sort(bins.begin(), bins.end(),
                         [](const std::wstring& a, const std::wstring& b){ return keyBinScore(a) > keyBinScore(b); });
        for (const std::wstring& bp : bins) {
            HANDLE bh = CreateFileW(bp.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (bh == INVALID_HANDLE_VALUE) continue;
            HandleGuard bg(bh);
            LARGE_INTEGER bsz{};
            if (!GetFileSizeEx(bh, &bsz) || bsz.QuadPart <= 0) continue;
            std::vector<u8> data;
            try { data.resize((size_t)bsz.QuadPart); } catch (...) { continue; }
            DWORD rd = 0; bool okRead = true; size_t done = 0;
            while (done < data.size()) {
                DWORD want = (DWORD)(std::min)((size_t)(1u << 20), data.size() - done);
                if (!ReadFile(bh, data.data() + done, want, &rd, nullptr) || rd == 0) { okRead = false; break; }
                done += rd;
            }
            if (!okRead) continue;

            std::string pem;
            if (!vp::FindRsaPem(data.data(), data.size(), pem)) continue;
            std::vector<u8> plk;
            if (!vp::RsaPkcs1Decrypt(pem, blob1.data(), blob1.size(), plk)) continue;
            if (tryKey(plk, plain)) {
                solved = true;
                binWithPem.swap(data);
                { std::lock_guard<std::mutex> g(g_plkMtx); g_plk = plk; }
                break;
            }
        }
    }

    // Format-level fallbacks (see VpFormat.h): the vsroplus toolkit key, then the
    // L33T variant key. Each is only accepted if the AES-GCM tag validates.
    if (!solved) {
        for (const std::string& pem : { vp::ToolkitRsaPem(), vp::L33tRsaPem() }) {
            std::vector<u8> plk;
            if (vp::RsaPkcs1Decrypt(pem, blob1.data(), blob1.size(), plk) &&
                tryKey(plk, plain)) {
                solved = true;
                std::lock_guard<std::mutex> g(g_plkMtx); g_plk = plk;
                break;
            }
        }
    }

    if (!solved)
        return Result::Fail(Status::WrongKey,
            T(L"Anahtar bulunamadi. Ayni klasorde istemci ikilileri gerekli "
            L"(GFXFileManager*.dll / Electus*.dll veya bir DUMP\\ klasoru)."));

    std::memcpy(m_metaKey, metaKey, 32);

    // --- establish the SIGNING key, verified against the stored signature ---
    // Only when we can reproduce the container's own signature do we allow
    // writing; otherwise the game would reject anything we saved.
    m_canSign = false;
    auto trySignCtx = [&](const char* ctx, size_t ctxLen) -> bool {
        u8 sk[32], mac[32];
        if (!vp::HmacSha256(m_metaKey, 32, (const u8*)ctx, ctxLen, sk)) return false;
        if (!vp::HmacSha256(sk, 32, plain.data(), plain.size(), mac)) return false;
        if (sig.size() != 32 || std::memcmp(mac, sig.data(), 32) != 0) return false;
        std::memcpy(m_signKey, sk, 32);
        return true;
    };
    // L33T signing: the file-signature key is mid = HMAC_SHA256(perLicenseKey,
    // "L33T-SIGN"), and the metadata signature is HMAC_SHA256(mid, plaintext)
    // (recovered from GFXFileManager.dll, see [[l33t-container-solved]]). Try that
    // first so L33T containers verify (and stay fully writable) without a warning.
    auto tryL33tSign = [&]() -> bool {
        std::vector<u8> plk;
        { std::lock_guard<std::mutex> g(g_plkMtx); plk = g_plk; }
        if (plk.size() != 32) return false;
        u8 mid[32], mac[32];
        const std::string kSign = OBF("L33T-SIGN").str();
        if (!vp::HmacSha256(plk.data(), plk.size(), (const u8*)kSign.data(), kSign.size(), mid)) return false;
        if (!vp::HmacSha256(mid, 32, plain.data(), plain.size(), mac)) return false;
        if (sig.size() != 32 || std::memcmp(mac, sig.data(), 32) != 0) return false;
        std::memcpy(m_signKey, mid, 32);
        return true;
    };
    if (tryL33tSign()) {
        m_canSign = true;
    } else {
        const std::string signCtx = vp::SignContext();
        if (trySignCtx(signCtx.data(), signCtx.size())) m_canSign = true;
    }
    if (!m_canSign && !binWithPem.empty()) {
        // Generic fallback: any 32-char printable-ASCII run in the client binary.
        const u8* d = binWithPem.data(); const size_t n = binWithPem.size();
        size_t runStart = 0; bool inRun = false;
        for (size_t i = 0; i <= n && !m_canSign; ++i) {
            const bool pr = (i < n) && d[i] >= 0x20 && d[i] <= 0x7E;
            if (pr && !inRun) { inRun = true; runStart = i; }
            else if (!pr && inRun) {
                inRun = false;
                for (size_t s = runStart; s + 32 <= i; ++s)
                    if (trySignCtx((const char*)d + s, 32)) { m_canSign = true; break; }
            }
        }
    }

    // --- parse the metadata plaintext --------------------------------------
    m_recs.clear(); m_chunks.clear();
    const u8* p = plain.data();
    const size_t n = plain.size();
    size_t q = 0;
    auto have = [&](size_t k) { return q + k <= n; };
    auto r32 = [&]() { int v = 0; std::memcpy(&v, p + q, 4); q += 4; return v; };
    auto r16 = [&]() { short v = 0; std::memcpy(&v, p + q, 2); q += 2; return v; };
    auto r64 = [&]() { i64 v = 0; std::memcpy(&v, p + q, 8); q += 8; return v; };
    auto r8  = [&]() { return p[q++]; };

    if (!have(4)) return Result::Fail(Status::BadFormat, T(L"Bos metadata."));
    int marker = r32();
    int count = marker;
    m_writeHashes = false;
    if (marker == -1) {
        if (!have(8)) return Result::Fail(Status::BadFormat, T(L"Bozuk metadata basligi."));
        r32(); m_writeHashes = true; count = r32();
    }
    if (count < 0 || count > 5'000'000)
        return Result::Fail(Status::BadFormat, T(L"Gecersiz dosya sayisi."));

    m_recs.reserve((size_t)count);
    for (int i = 0; i < count; ++i) {
        if (!have(6)) break;
        Rec rec;
        rec.id = r32();
        short pl = r16();
        if (pl < 0 || !have((size_t)pl + 8 * 4 + 2)) break;
        std::string rel((const char*)p + q, (size_t)pl); q += pl;
        rec.size        = r64();
        rec.created     = r64();
        rec.modified    = r64();
        rec.chunkOffset = r64();
        const u8 isFolder  = r8();
        rec.encrypted      = r8() != 0;
        if (m_writeHashes) {
            if (!have(1)) break;
            const u8 hl = r8();
            if (hl == 32) { if (!have(32)) break; std::memcpy(rec.hash, p + q, 32); q += 32; rec.hasHash = true; }
        }
        if (isFolder) continue;
        rec.path = fromUtf8(rel);
        for (wchar_t& c : rec.path) if (c == L'/') c = L'\\';
        if (rec.path.empty() || rec.size < 0 || rec.chunkOffset < 0) continue;
        m_recs.push_back(std::move(rec));
    }
    if (have(4)) {
        int nch = r32();
        if (nch > 0 && nch < 1'000'000) {
            for (int i = 0; i < nch && have(17); ++i) {
                Chunk c; c.offset = r64(); c.size = r64(); c.free = r8() != 0;
                if (c.offset >= 0 && c.size > 0) m_chunks.push_back(c);
            }
        }
    }

    // --- commit -------------------------------------------------------------
    m_handle       = hg.release();
    m_path         = path;
    m_fileSize     = fileSize;
    // Writable whenever the file itself is writable. (The signing context is a
    // fixed constant of the format and verifies on every real container; if it
    // ever did not we still write - we just say so in the log.)
    m_readOnly     = ro;
    m_fullEncrypted = isFullEnc;
    m_metaOffset   = metaOffset;
    m_metaSize     = metaSize;
    m_metaReserved = (metaReserved > 0) ? metaReserved : (4 + 32 + metaSize);
    rebuildTree();

    // --- payload keystream key ---------------------------------------------
    // The directory is now readable. Establishing the key that decrypts the FILE
    // BYTES is a separate step because container families differ here:
    //   * classic VSROPLUS/Electus: a single format-constant xorKey.
    //   * L33T: a per-container xorKey that is NOT derivable from blob1/blob2 -
    //     it is produced by the client at run time. We resolve it via the
    //     session vault, then (if missing) by observing a running client, and
    //     never persist it (see [[sro-asset-studio-key-privacy]], RULE 1).
    // resolvePayloadKey() picks whichever key actually decrypts this container's
    // own files, proven by a JMXV/known-signature trial-decrypt.
    resolvePayloadKey();

    if (log) {
        wchar_t buf[200];
        swprintf(buf, 200, L"%s acildi: %llu dosya, %llu klasor.%s",
                 up.c_str(), (unsigned long long)m_fileCount,
                 (unsigned long long)m_folderCount,
                 m_readOnly ? L"  (salt okunur - dosya kilitli)" : L"");
        log(buf);
        if (!m_canSign && !m_readOnly)
            log(L"Not: imza bagalami dogrulanamadi; yazma acik ama kaydedilen "
                L"dizini oyun kabul etmeyebilir.");
    }
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// Tree
// ---------------------------------------------------------------------------
void VpArchive::rebuildTree() {
    m_nodes.clear(); m_byPath.clear(); m_fileRec.clear();
    m_folderCount = 0; m_fileCount = 0;

    m_nodes.emplace_back();
    Pk2Node& rootN = m_nodes.front();
    rootN.entry.type = Pk2EntryType::Folder;
    rootN.loaded = true;
    m_byPath[L""] = &rootN;

    for (const std::wstring& f : m_emptyFolders) intern(f);

    for (size_t i = 0; i < m_recs.size(); ++i) {
        const Rec& rec = m_recs[i];
        const size_t slash = rec.path.find_last_of(L'\\');
        std::wstring folderPath = (slash == std::wstring::npos) ? L"" : rec.path.substr(0, slash);
        std::wstring leaf       = (slash == std::wstring::npos) ? rec.path : rec.path.substr(slash + 1);
        if (leaf.empty()) continue;

        Pk2Node* parent = intern(folderPath);
        m_nodes.emplace_back();
        Pk2Node* fn = &m_nodes.back();
        fn->parent   = parent;
        fn->name     = leaf;
        fn->fullPath = rec.path;
        fn->loaded   = true;
        fn->entry.type     = Pk2EntryType::File;
        fn->entry.name     = leaf;
        fn->entry.nameAnsi = toUtf8(leaf);
        fn->entry.offset   = rec.chunkOffset;
        fn->entry.size     = (u32)(std::min<i64>)(rec.size, 0xFFFFFFFFll);
        parent->files.push_back(fn);
        m_byPath[lower(rec.path)] = fn;
        m_fileRec[fn] = i;
        ++m_fileCount;
    }

    auto byName = [](const Pk2Node* a, const Pk2Node* b) { return lower(a->name) < lower(b->name); };
    for (Pk2Node& nd : m_nodes) {
        std::sort(nd.folders.begin(), nd.folders.end(), byName);
        std::sort(nd.files.begin(),   nd.files.end(),   byName);
    }
}

Pk2Node* VpArchive::intern(const std::wstring& folderPath) {
    if (folderPath.empty()) return &m_nodes.front();
    const std::wstring key = lower(folderPath);
    auto it = m_byPath.find(key);
    if (it != m_byPath.end()) return it->second;

    const size_t slash = folderPath.find_last_of(L'\\');
    std::wstring parentPath = (slash == std::wstring::npos) ? L"" : folderPath.substr(0, slash);
    std::wstring leaf       = (slash == std::wstring::npos) ? folderPath : folderPath.substr(slash + 1);
    Pk2Node* parent = intern(parentPath);

    m_nodes.emplace_back();
    Pk2Node* fn = &m_nodes.back();
    fn->parent   = parent;
    fn->name     = leaf;
    fn->fullPath = folderPath;
    fn->loaded   = true;
    fn->entry.type     = Pk2EntryType::Folder;
    fn->entry.name     = leaf;
    fn->entry.nameAnsi = toUtf8(leaf);
    parent->folders.push_back(fn);
    m_byPath[key] = fn;
    ++m_folderCount;
    return fn;
}

// ---------------------------------------------------------------------------
// Info
// ---------------------------------------------------------------------------
bool VpArchive::isOpen()   const { return m_handle != nullptr; }
bool VpArchive::readOnly() const { return m_readOnly; }
bool VpArchive::canWrite() const { return m_handle != nullptr && !m_readOnly; }
const std::wstring& VpArchive::path() const { return m_path; }
u64  VpArchive::fileSize() const { return m_fileSize; }

Pk2HeaderInfo VpArchive::headerInfo() const {
    Pk2HeaderInfo hi;
    hi.signature      = fromUtf8(vpMagic());   // obfuscated at rest; exact value at run time
    hi.versionText    = m_version;
    hi.encrypted      = true;
    hi.signatureValid = m_canSign;
    return hi;
}

Pk2Node* VpArchive::root() const {
    return m_nodes.empty() ? nullptr : const_cast<Pk2Node*>(&m_nodes.front());
}

Result VpArchive::loadChildren(Pk2Node*) { return Result::Ok(); }   // eager

void VpArchive::childrenOf(Pk2Node* node,
                           std::vector<Pk2Node*>& folders,
                           std::vector<Pk2Node*>& files) {
    folders.clear();
    files.clear();
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!node) return;
    folders = node->folders;
    files   = node->files;
}

Result VpArchive::findNode(const std::wstring& innerPath, Pk2Node** out) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (out) *out = nullptr;
    std::wstring key = innerPath;
    for (wchar_t& c : key) if (c == L'/') c = L'\\';
    while (!key.empty() && key.front() == L'\\') key.erase(0, 1);
    while (!key.empty() && key.back()  == L'\\') key.pop_back();
    auto it = m_byPath.find(lower(key));
    if (it == m_byPath.end()) return Result::Fail(Status::NotFound);
    if (out) *out = it->second;
    return Result::Ok();
}

Result VpArchive::findFile(const std::wstring& innerPath, Pk2Node** out) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Pk2Node* n = nullptr;
    Result r = findNode(innerPath, &n);
    if (!r.ok()) return r;
    if (!n || !n->isFile()) return Result::Fail(Status::NotFound);
    if (out) *out = n;
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// Raw IO
// ---------------------------------------------------------------------------
// NOTE: readRaw does NOT take m_mtx - callers do. (compact()/addFile() already
// hold it, so locking here would self-deadlock on a non-recursive mutex.)
Result VpArchive::readRaw(i64 off, i64 size, std::vector<u8>& out) const {
    if (!m_handle) return Result::Fail(Status::NotFound, T(L"Arsiv kapali."));
    if (size < 0 || off < 0) return Result::Fail(Status::InvalidArgument);
    try { out.resize((size_t)size); }
    catch (...) { return Result::Fail(Status::OutOfMemory, T(L"Bellek yetersiz.")); }
    if (size == 0) return Result::Ok();
    LARGE_INTEGER li; li.QuadPart = off;
    if (!SetFilePointerEx((HANDLE)m_handle, li, nullptr, FILE_BEGIN))
        return Result::Fail(Status::IoError, T(L"Konumlandirma hatasi."));
    size_t done = 0;
    while (done < (size_t)size) {
        DWORD want = (DWORD)(std::min<i64>)(1 << 20, size - (i64)done), gotN = 0;
        if (!ReadFile((HANDLE)m_handle, out.data() + done, want, &gotN, nullptr) || gotN == 0)
            return Result::Fail(Status::IoError, T(L"Okuma hatasi."));
        done += gotN;
    }
    return Result::Ok();
}

Result VpArchive::writeRaw(i64 off, const u8* data, i64 size) {
    if (!m_handle) return Result::Fail(Status::NotFound, T(L"Arsiv kapali."));
    if (size < 0 || off < 0) return Result::Fail(Status::InvalidArgument);
    LARGE_INTEGER li; li.QuadPart = off;
    if (!SetFilePointerEx((HANDLE)m_handle, li, nullptr, FILE_BEGIN))
        return Result::Fail(Status::IoError, T(L"Konumlandirma hatasi."));
    size_t done = 0;
    while (done < (size_t)size) {
        DWORD want = (DWORD)(std::min<i64>)(1 << 20, size - (i64)done), put = 0;
        if (!WriteFile((HANDLE)m_handle, data + done, want, &put, nullptr) || put == 0)
            return Result::Fail(Status::IoError, T(L"Yazma hatasi."));
        done += put;
    }
    if ((u64)(off + size) > m_fileSize) m_fileSize = (u64)(off + size);
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// Payload keystream key resolution
// ---------------------------------------------------------------------------
namespace {
/// Is `buf` a plausible decrypted Silkroad asset / plain file? Format-agnostic,
/// wide margins (same spirit as the PK3 asset-cipher validator): a JMXV/known
/// signature, or mostly-printable text, or a healthy zero-byte fraction typical
/// of real binary assets (ciphertext sits near 0.4% zeros).
bool looksDecrypted(const u8* b, size_t n) {
    if (n < 8) return true;                       // too small to judge - don't block
    static const char* sig[] = { "JMXV", "OggS", "JMXVBMS", "JMXVRES", "JMXVBMT",
                                 "JMXVDDJ", "JMXVBAN", "JMXVEFP", "JMXVCPD", "DDS ",
                                 "BM", "\x89PNG", "RIFF" };
    for (const char* s : sig) { size_t L = std::strlen(s); if (n >= L && std::memcmp(b, s, L) == 0) return true; }
    size_t printable = 0, zero = 0, cap = n < 256 ? n : 256;
    for (size_t i = 0; i < cap; ++i) { u8 c = b[i]; if (c == 0) ++zero; if ((c >= 0x20 && c < 0x7f) || c == '\r' || c == '\n' || c == '\t') ++printable; }
    if (printable * 100 >= cap * 92) return true;  // text file
    if (zero * 100 >= cap * 6)       return true;  // real binary asset (~30% zeros); ciphertext ~0.4%
    return false;
}
} // namespace

bool VpArchive::payloadKeyDecrypts(const u8 key[32]) const {
    // Try a few of this container's own files: read raw, decrypt with `key`, judge.
    int tried = 0;
    for (const Rec& f : m_recs) {
        if (f.size < 16 || f.size > (1 << 20)) continue;
        std::vector<u8> raw;
        if (!readRaw(f.chunkOffset, (f.size < 4096 ? f.size : 4096), raw).ok() || raw.empty()) continue;
        std::vector<u8> dec;
        if (!vp::AesEcbKeystreamXor(key, vp::FileNonce(toUtf8(f.path)),
                                    raw.data(), raw.size(), dec)) continue;
        if (looksDecrypted(dec.data(), dec.size())) return true;
        if (++tried >= 6) break;   // a handful is enough; wrong key ~never passes
    }
    return false;
}

void VpArchive::resolvePayloadKey() {
    m_payloadKeyReady = false;
    // Default to the toolkit constant so classic containers with only per-file
    // IsEncrypted (m_fullEncrypted == false) keep decrypting exactly as before.
    { const std::string tk = vp::ToolkitXorKey(); std::memcpy(m_xorKey, tk.data(), 32); }
    if (!m_fullEncrypted) { m_payloadKeyReady = true; return; }  // no whole-file layer

    u8 cand[32];

    // 0) L33T derivation - the payload xorKey is HMAC-SHA256(perLicenseKey,
    //    "L33T-FILESTREAM"). Recovered from GFXFileManager.dll and verified on all
    //    six DESTEK containers: the whole key schedule is HMAC over the RSA-unwrapped
    //    perLicenseKey with three fixed messages - metadataKey uses "<NAME>_CONTAINER",
    //    the file signature key uses "L33T-SIGN", and the payload key uses
    //    "L33T-FILESTREAM". This is FULLY OFFLINE - no client, no RAM, any server
    //    version - so it is tried first. (Classic VSROPLUS/Electus fall through to
    //    the toolkit constant below.)
    {
        std::vector<u8> plk;
        { std::lock_guard<std::mutex> g(g_plkMtx); plk = g_plk; }
        if (plk.size() == 32) {
            const std::string kFileStream = OBF("L33T-FILESTREAM").str();
            if (vp::HmacSha256(plk.data(), plk.size(),
                               (const u8*)kFileStream.data(), kFileStream.size(), cand) &&
                payloadKeyDecrypts(cand)) {
                std::memcpy(m_xorKey, cand, 32); m_payloadKeyReady = true; return;
            }
        }
    }

    // 1) classic format constant (VSROPLUS/Electus containers).
    { const std::string tk = vp::ToolkitXorKey(); std::memcpy(cand, tk.data(), 32); }
    if (payloadKeyDecrypts(cand)) { std::memcpy(m_xorKey, cand, 32); m_payloadKeyReady = true; return; }

    // 2) session vault (a key observed earlier this run for this container id).
    if (!m_containerId.empty() && xorVaultGet(m_containerId, cand) && payloadKeyDecrypts(cand)) {
        std::memcpy(m_xorKey, cand, 32); m_payloadKeyReady = true; return;
    }
    // Not resolved yet: the directory is fully usable; file bytes stay encrypted
    // until installPayloadKey() supplies a validated key (see key discovery).
}

bool VpArchive::installPayloadKey(const u8 key[32]) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!payloadKeyDecrypts(key)) return false;
    std::memcpy(m_xorKey, key, 32);
    m_payloadKeyReady = true;
    if (!m_containerId.empty()) xorVaultPut(m_containerId, key);
    return true;
}

bool VpArchive::metadataKey(u8 out[32]) const {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    // m_metaKey is zero only before a successful open.
    static const u8 zero[32] = {};
    if (std::memcmp(m_metaKey, zero, 32) == 0) return false;
    std::memcpy(out, m_metaKey, 32);
    return true;
}

bool VpArchive::buildPayloadCribs(std::vector<PayloadCrib>& out) const {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    out.clear();
    for (const Rec& f : m_recs) {
        if (out.size() >= 12) break;
        if (f.size < 16) continue;
        u8 known[16]; int knownLen = 0;
        if (!L33tKnownPlain(fileNameOf(f.path), known, knownLen)) continue;  // no signature
        if (knownLen < 4) continue;
        std::vector<u8> raw;
        if (!readRaw(f.chunkOffset, 16, raw).ok() || raw.size() < (size_t)knownLen) continue;
        PayloadCrib c;
        c.path  = toUtf8(f.path);
        c.nonce = vp::FileNonce(c.path);
        std::memcpy(c.cipher, raw.data(), 16);
        c.ksLen = knownLen < 16 ? knownLen : 16;
        for (int i = 0; i < c.ksLen; ++i) c.ks[i] = (u8)(raw[i] ^ known[i]);
        out.push_back(std::move(c));
    }
    return !out.empty();
}

Result VpArchive::requireWritable() const {
    if (!m_handle) return Result::Fail(Status::NotFound, T(L"Arsiv kapali."));
    if (m_readOnly)
        return Result::Fail(Status::AccessDenied,
            T(L"Arsiv salt okunur acildi - dosya baska bir program tarafindan "
            L"kullaniliyor veya salt-okunur isaretli."));
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------
Result VpArchive::readFile(const Pk2Node* node, std::vector<u8>& out) const {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!node || !node->isFile()) return Result::Fail(Status::InvalidArgument);
    auto it = m_fileRec.find(node);
    if (it == m_fileRec.end() || it->second >= m_recs.size()) return Result::Fail(Status::NotFound);
    const Rec& f = m_recs[it->second];
    Result r = readRaw(f.chunkOffset, f.size, out);
    if (!r.ok()) return r;
    if ((m_fullEncrypted || f.encrypted) && !out.empty()) {
        std::vector<u8> dec;
        if (!vp::AesEcbKeystreamXor(m_xorKey, vp::FileNonce(toUtf8(f.path)),
                                    out.data(), out.size(), dec))
            return Result::Fail(Status::Unknown, T(L"Dosya cozulemedi."));
        out.swap(dec);
    }
    return Result::Ok();
}

Result VpArchive::readFile(const std::wstring& innerPath, std::vector<u8>& out) {
    // One lock across find + read: the recursive mutex lets the inner
    // readFile(node) re-lock, and close() can no longer free the node in a gap.
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Pk2Node* n = nullptr;
    Result r = findFile(innerPath, &n);
    if (!r.ok()) return r;
    return readFile(n, out);
}

Result VpArchive::readFilePrefix(const std::wstring& innerPath, size_t maxBytes,
                                 std::vector<u8>& out) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Pk2Node* n = nullptr;
    Result r = findFile(innerPath, &n);
    if (!r.ok()) return r;
    return readFilePrefix(n, maxBytes, out);
}

Result VpArchive::readFilePrefix(const Pk2Node* node, size_t maxBytes, std::vector<u8>& out) const {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!node || !node->isFile()) return Result::Fail(Status::InvalidArgument);
    auto it = m_fileRec.find(node);
    if (it == m_fileRec.end() || it->second >= m_recs.size()) return Result::Fail(Status::NotFound);
    const Rec& f = m_recs[it->second];
    if (m_fullEncrypted || f.encrypted) {
        // Path-keyed keystream: decrypt from offset 0, then trim.
        Result r = readRaw(f.chunkOffset, f.size, out);
        if (!r.ok()) return r;
        if (!out.empty()) {
            std::vector<u8> dec;
            if (!vp::AesEcbKeystreamXor(m_xorKey, vp::FileNonce(toUtf8(f.path)),
                                        out.data(), out.size(), dec))
                return Result::Fail(Status::Unknown, T(L"Dosya cozulemedi."));
            out.swap(dec);
        }
        if (out.size() > maxBytes) out.resize(maxBytes);
        return Result::Ok();
    }
    return readRaw(f.chunkOffset, (std::min<i64>)(f.size, (i64)maxBytes), out);
}

// ---------------------------------------------------------------------------
static void ensureDirs(const std::wstring& fileOrDir) {
    std::wstring dir = fileOrDir;
    const size_t i = dir.find_last_of(L"\\/");
    if (i == std::wstring::npos) return;
    dir = dir.substr(0, i);
    std::wstring cur;
    for (size_t k = 0; k <= dir.size(); ++k) {
        if (k == dir.size() || dir[k] == L'\\' || dir[k] == L'/') {
            if (!cur.empty() && !(cur.size() == 2 && cur[1] == L':')) CreateDirectoryW(cur.c_str(), nullptr);
        }
        if (k < dir.size()) cur.push_back(dir[k]);
    }
}

static Result writeDiskFile(const std::wstring& outFilePath, const std::vector<u8>& data) {
    ensureDirs(outFilePath);
    HANDLE h = CreateFileW(outFilePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Result::Fail(Status::IoError, T(L"Yazilamadi: ") + outFilePath);
    HandleGuard hg(h);
    size_t done = 0;
    while (done < data.size()) {
        DWORD want = (DWORD)(std::min)((size_t)(1u << 20), data.size() - done), put = 0;
        if (!WriteFile(h, data.data() + done, want, &put, nullptr) || put == 0)
            return Result::Fail(Status::IoError, T(L"Yazma hatasi: ") + outFilePath);
        done += put;
    }
    return Result::Ok();
}

Result VpArchive::extractFile(const Pk2Node* node, const std::wstring& outFilePath) const {
    std::vector<u8> data;
    Result r = readFile(node, data);
    if (!r.ok()) return r;
    return writeDiskFile(outFilePath, data);
}

Result VpArchive::extractTree(Pk2Node* node, const std::wstring& outDir, const ProgressFn& progress) {
    if (!node) return Result::Fail(Status::InvalidArgument);
    std::vector<Pk2Node*> stack{ node }, files;
    while (!stack.empty()) {
        Pk2Node* cur = stack.back(); stack.pop_back();
        if (cur->isFile()) { files.push_back(cur); continue; }
        for (Pk2Node* f : cur->folders) stack.push_back(f);
        for (Pk2Node* f : cur->files)   files.push_back(f);
    }

    // Same rule as Pk2Archive: everything lands under <outDir>\<ArchiveName>\
    // keeping each file's COMPLETE inner path. Exporting the folder
    // "server_dep" from Media.vp therefore writes
    //     <outDir>\Media\server_dep\silkroad\...
    // instead of dumping its children straight into <outDir>.
    std::wstring stem = fileNameOf(m_path);
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem = stem.substr(0, dot);
    if (stem.empty()) stem = L"arsiv";
    // Same rule as Pk2Archive: the extracted root is lower-cased (media\, data\)
    // to match Silkroad's own layout.
    stem[0] = (wchar_t)towlower(stem[0]);
    const std::wstring base = outDir + L"\\" + SanitizeFileName(stem);

    u64 i = 0;
    for (Pk2Node* f : files) {
        // Sanitize component by component so a hostile path cannot escape.
        std::wstring rel;
        std::wstring comp;
        const std::wstring& fp = f->fullPath;
        for (size_t k = 0; k <= fp.size(); ++k) {
            if (k == fp.size() || fp[k] == L'\\' || fp[k] == L'/') {
                if (!comp.empty()) {
                    if (!rel.empty()) rel += L"\\";
                    rel += SanitizeFileName(comp);
                }
                comp.clear();
            } else comp.push_back(fp[k]);
        }
        if (rel.empty()) rel = SanitizeFileName(f->name);

        Result r = extractFile(f, base + L"\\" + rel);
        if (!r.ok()) return r;
        if (!ReportProgress(progress, ++i, files.size(), f->name.c_str()))
            return Result::Fail(Status::Cancelled);
    }
    return Result::Ok();
}

Result VpArchive::extractAll(const std::wstring& outDir, const ProgressFn& progress) {
    return extractTree(root(), outDir, progress);
}

// ---------------------------------------------------------------------------
// Search / count
// ---------------------------------------------------------------------------
Result VpArchive::walkAll(const std::function<bool(Pk2Node*)>& visit, const ProgressFn& progress) {
    u64 i = 0; const u64 total = m_nodes.size();
    for (Pk2Node& nd : m_nodes) {
        if (nd.isRoot()) continue;
        if (visit && !visit(&nd)) return Result::Fail(Status::Cancelled);
        if (!ReportProgress(progress, ++i, total, nullptr)) return Result::Fail(Status::Cancelled);
    }
    return Result::Ok();
}

Result VpArchive::search(const std::wstring& needle, std::vector<Pk2Node*>& results,
                         size_t maxResults, const ProgressFn& progress) {
    results.clear();
    u64 i = 0; const u64 total = m_nodes.size();
    for (Pk2Node& nd : m_nodes) {
        if (nd.isRoot()) continue;
        // Shared rule: plain substring, ".ext", or a glob like "srokim*.bsr".
        if (FilterMatches(nd.name, nd.fullPath, needle)) {
            results.push_back(&nd);
            if (results.size() >= maxResults) break;
        }
        if (!ReportProgress(progress, ++i, total, nullptr)) return Result::Fail(Status::Cancelled);
    }
    return Result::Ok();
}

Result VpArchive::countEntries(bool, u64& folders, u64& files, const ProgressFn&) {
    folders = m_folderCount; files = m_fileCount;
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------
void VpArchive::defragment() {
    std::sort(m_chunks.begin(), m_chunks.end(),
              [](const Chunk& a, const Chunk& b) { return a.offset < b.offset; });
    std::vector<Chunk> merged;
    for (const Chunk& c : m_chunks) {
        if (!c.free || c.size <= 0) continue;
        if (!merged.empty() && merged.back().offset + merged.back().size == c.offset)
            merged.back().size += c.size;
        else merged.push_back(c);
    }
    m_chunks.swap(merged);
}

i64 VpArchive::allocate(i64 size) {
    if (size <= 0) size = 1;
    defragment();
    size_t best = (size_t)-1;
    for (size_t i = 0; i < m_chunks.size(); ++i)
        if (m_chunks[i].free && m_chunks[i].size >= size &&
            (best == (size_t)-1 || m_chunks[i].size < m_chunks[best].size))
            best = i;
    if (best != (size_t)-1) {
        const i64 off = m_chunks[best].offset;
        const i64 rem = m_chunks[best].size - size;
        if (rem >= 4096) { m_chunks[best].offset += size; m_chunks[best].size = rem; }
        else m_chunks.erase(m_chunks.begin() + (ptrdiff_t)best);
        return off;
    }
    i64 off = (i64)((m_fileSize + 15ull) & ~15ull);   // 16-byte aligned append
    m_fileSize = (u64)(off + size);
    return off;
}

void VpArchive::release(i64 offset, i64 size) {
    if (size <= 0 || offset < 0) return;
    m_chunks.push_back(Chunk{ offset, size, true });
}

// ---------------------------------------------------------------------------
// Metadata save
// ---------------------------------------------------------------------------
int VpArchive::nextId() const {
    int mx = 0;
    for (const Rec& r : m_recs) mx = (std::max)(mx, r.id);
    return mx + 1;
}

VpArchive::Rec* VpArchive::recForPath(const std::wstring& innerPath) {
    const std::wstring key = lower(innerPath);
    for (Rec& r : m_recs) if (lower(r.path) == key) return &r;
    return nullptr;
}

void VpArchive::serializeMetadata(std::vector<u8>& out) const {
    out.clear();
    auto w32 = [&](int v)  { const u8* b = (const u8*)&v; out.insert(out.end(), b, b + 4); };
    auto w16 = [&](short v){ const u8* b = (const u8*)&v; out.insert(out.end(), b, b + 2); };
    auto w64 = [&](i64 v)  { const u8* b = (const u8*)&v; out.insert(out.end(), b, b + 8); };
    auto w8  = [&](u8 v)   { out.push_back(v); };

    if (m_writeHashes) { w32(-1); w32(1); }
    w32((int)m_recs.size());
    for (const Rec& r : m_recs) {
        const std::string p = toUtf8(r.path);
        w32(r.id);
        w16((short)p.size());
        out.insert(out.end(), p.begin(), p.end());
        w64(r.size); w64(r.created); w64(r.modified); w64(r.chunkOffset);
        w8(0);                       // isFolder
        w8(r.encrypted ? 1 : 0);
        if (m_writeHashes) {
            if (r.hasHash) { w8(32); out.insert(out.end(), r.hash, r.hash + 32); }
            else           { w8(0); }
        }
    }
    int nfree = 0;
    for (const Chunk& c : m_chunks) if (c.free && c.size > 0) ++nfree;
    w32(nfree);
    for (const Chunk& c : m_chunks) {
        if (!c.free || c.size <= 0) continue;
        w64(c.offset); w64(c.size); w8(1);
    }
}

Result VpArchive::saveMetadata() {
    Result rw = requireWritable();
    if (!rw.ok()) return rw;

    std::vector<u8> pt;
    serializeMetadata(pt);

    std::vector<u8> enc;
    if (!vp::AesGcmEncrypt(m_metaKey, pt.data(), pt.size(), enc))
        return Result::Fail(Status::Unknown, T(L"Metadata sifrelenemedi."));
    u8 sig[32];
    if (!vp::HmacSha256(m_signKey, 32, pt.data(), pt.size(), sig))
        return Result::Fail(Status::Unknown, T(L"Metadata imzalanamadi."));

    // TRANSACTIONAL SAVE. The new directory always goes to a FRESH region; the
    // container keeps pointing at the old one until we have written the new one,
    // read it back and re-verified it. The 16-byte header patch is the single
    // commit point, so an error (or a crash) at any earlier step leaves the
    // container exactly as it was.
    const i64 region   = 4 + 32 + (i64)enc.size();
    const i64 reserved = (std::max<i64>)(region + region / 2, 1 << 20);
    const i64 oldOff = m_metaOffset, oldRes = m_metaReserved, oldSize = m_metaSize;
    const i64 newOff = allocate(reserved);

    std::vector<u8> blob;
    blob.reserve((size_t)region);
    const int sl = 32;
    blob.insert(blob.end(), (const u8*)&sl, (const u8*)&sl + 4);
    blob.insert(blob.end(), sig, sig + 32);
    blob.insert(blob.end(), enc.begin(), enc.end());

    Result r = writeRaw(newOff, blob.data(), (i64)blob.size());
    if (!r.ok()) { release(newOff, reserved); return r; }
    FlushFileBuffers((HANDLE)m_handle);

    // Read it back and prove it decrypts + authenticates before committing.
    {
        std::vector<u8> check;
        r = readRaw(newOff, (i64)blob.size(), check);
        if (!r.ok() || check.size() != blob.size()) {
            release(newOff, reserved);
            return Result::Fail(Status::IoError, T(L"Yazilan dizin geri okunamadi."));
        }
        const u8* e  = check.data() + 4 + 32;
        const size_t elen = (size_t)enc.size();
        std::vector<u8> back;
        const bool gcmOk = elen > 28 &&
            vp::AesGcmDecrypt(m_metaKey, e, 12, e + 12, elen - 28, e + elen - 16, 16, back);
        u8 vmac[32];
        const bool sigOk = gcmOk && back.size() == pt.size() &&
            std::memcmp(back.data(), pt.data(), pt.size()) == 0 &&
            vp::HmacSha256(m_signKey, 32, back.data(), back.size(), vmac) &&
            std::memcmp(vmac, check.data() + 4, 32) == 0;
        if (!sigOk) {
            release(newOff, reserved);
            return Result::Fail(Status::Unknown,
                T(L"Yazilan dizin dogrulanamadi; arsiv degistirilmedi."));
        }
    }

    // COMMIT: repoint the header at the verified region.
    u8 hf[16];
    const int ms = (int)enc.size(), mr = (int)reserved;
    std::memcpy(hf,      &newOff, 8);
    std::memcpy(hf + 8,  &ms, 4);
    std::memcpy(hf + 12, &mr, 4);
    r = writeRaw(m_metaFieldPos, hf, 16);
    if (!r.ok()) { release(newOff, reserved); return r; }
    FlushFileBuffers((HANDLE)m_handle);

    m_metaOffset   = newOff;
    m_metaSize     = ms;
    m_metaReserved = reserved;
    if (oldOff > 0 && oldRes > 0) release(oldOff, oldRes);   // recycle the old region
    (void)oldSize;
    return Result::Ok();
}

Result VpArchive::flush() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_handle) return Result::Fail(Status::NotFound, T(L"Arsiv kapali."));
    if (m_readOnly) return Result::Ok();
    FlushFileBuffers((HANDLE)m_handle);
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------
Result VpArchive::addFile(Pk2Node* parentFolder, const std::wstring& name,
                          const u8* data, size_t size, bool overwrite) {
    // Resolve to a PATH before mutating: any write rebuilds the tree, which
    // invalidates every Pk2Node*, so node pointers must not survive the call.
    std::wstring folder;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        if (parentFolder) folder = parentFolder->fullPath;
    }
    return addFileAtPath(folder, name, data, size, overwrite);
}

Result VpArchive::addFileAtPath(const std::wstring& folder, const std::wstring& name,
                                const u8* data, size_t size, bool overwrite) {
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        Result rw = requireWritable();
        if (!rw.ok()) return rw;
        if (name.empty()) return Result::Fail(Status::InvalidArgument, T(L"Ad bos."));

        std::wstring full = folder.empty() ? name : (folder + L"\\" + name);

        Rec* existing = recForPath(full);
        if (existing && !overwrite)
            return Result::Fail(Status::AlreadyExists, T(L"Zaten var: ") + full);

        const i64 need = (i64)size;
        if (existing) {
            // Never overwrite the live payload in place: a mid-write I/O error
            // would corrupt the existing file with no backup (rule 0.5). Write
            // the new bytes to a FRESH region - the old one stays allocated so
            // allocate() cannot hand it back - and only swap the record over
            // once the write has fully succeeded, then release the old region.
            // A failure anywhere leaves the old payload and metadata intact.
            const i64 oldOffset = existing->chunkOffset;
            const i64 oldSize   = existing->size;
            const i64 newOffset = allocate(need);
            if (size) {
                std::vector<u8> enc;
                const u8* src = data;
                if (m_fullEncrypted || existing->encrypted) {
                    if (!vp::AesEcbKeystreamXor(m_xorKey, vp::FileNonce(toUtf8(full)),
                                                data, size, enc)) {
                        release(newOffset, need);
                        return Result::Fail(Status::Unknown, T(L"Dosya sifrelenemedi."));
                    }
                    src = enc.data();
                }
                Result r = writeRaw(newOffset, src, need);
                if (!r.ok()) { release(newOffset, need); return r; }
            }
            existing->chunkOffset = newOffset;
            existing->size        = need;
            existing->modified    = nowTicks();
            existing->hasHash     = false;
            if (oldOffset > 0 && oldSize > 0) release(oldOffset, oldSize);
        } else {
            Rec rec;
            rec.id          = nextId();
            rec.path        = full;
            rec.size        = need;
            rec.created     = nowTicks();
            rec.modified    = rec.created;
            rec.chunkOffset = allocate(need);
            rec.encrypted   = false;   // container flag governs when fullEncrypted
            if (size) {
                std::vector<u8> enc;
                const u8* src = data;
                if (m_fullEncrypted) {
                    if (!vp::AesEcbKeystreamXor(m_xorKey, vp::FileNonce(toUtf8(full)),
                                                data, size, enc))
                        return Result::Fail(Status::Unknown, T(L"Dosya sifrelenemedi."));
                    src = enc.data();
                }
                Result r = writeRaw(rec.chunkOffset, src, need);
                if (!r.ok()) return r;
            }
            m_recs.push_back(std::move(rec));
        }

        Result r = saveMetadata();
        if (!r.ok()) return r;
        rebuildTree();
    }
    return Result::Ok();
}

Result VpArchive::importFile(Pk2Node* parentFolder, const std::wstring& diskPath,
                             const std::wstring& nameOverride, bool overwrite,
                             const ProgressFn& progress) {
    HANDLE h = CreateFileW(diskPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return Result::Fail(Status::NotFound, T(L"Okunamadi: ") + diskPath);
    HandleGuard hg(h);
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) return Result::Fail(Status::IoError, T(L"Boyut okunamadi."));
    if (sz.QuadPart > (i64)0x7FFFFFFF)
        return Result::Fail(Status::Unsupported, T(L"Dosya cok buyuk (2 GB siniri)."));

    std::vector<u8> data;
    try { data.resize((size_t)sz.QuadPart); }
    catch (...) { return Result::Fail(Status::OutOfMemory, T(L"Bellek yetersiz.")); }
    size_t done = 0; DWORD rd = 0;
    while (done < data.size()) {
        DWORD want = (DWORD)(std::min)((size_t)(1u << 20), data.size() - done);
        if (!ReadFile(h, data.data() + done, want, &rd, nullptr) || rd == 0)
            return Result::Fail(Status::IoError, T(L"Okuma hatasi: ") + diskPath);
        done += rd;
        if (!ReportProgress(progress, done, data.size(), nullptr))
            return Result::Fail(Status::Cancelled);
    }
    const std::wstring name = nameOverride.empty() ? fileNameOf(diskPath) : nameOverride;
    std::wstring folder;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        if (parentFolder) folder = parentFolder->fullPath;
    }
    return addFileAtPath(folder, name, data.data(), data.size(), overwrite);
}

Result VpArchive::importFileToPath(const std::wstring& folder, const std::wstring& diskPath,
                                   const std::wstring& name, bool overwrite) {
    HANDLE h = CreateFileW(diskPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return Result::Fail(Status::NotFound, T(L"Okunamadi: ") + diskPath);
    HandleGuard hg(h);
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) return Result::Fail(Status::IoError, T(L"Boyut okunamadi."));
    if (sz.QuadPart > (i64)0x7FFFFFFF)
        return Result::Fail(Status::Unsupported, T(L"Dosya cok buyuk (2 GB siniri)."));
    std::vector<u8> data;
    try { data.resize((size_t)sz.QuadPart); }
    catch (...) { return Result::Fail(Status::OutOfMemory, T(L"Bellek yetersiz.")); }
    size_t done = 0; DWORD rd = 0;
    while (done < data.size()) {
        DWORD want = (DWORD)(std::min)((size_t)(1u << 20), data.size() - done);
        if (!ReadFile(h, data.data() + done, want, &rd, nullptr) || rd == 0)
            return Result::Fail(Status::IoError, T(L"Okuma hatasi: ") + diskPath);
        done += rd;
    }
    return addFileAtPath(folder, name, data.data(), data.size(), overwrite);
}

Result VpArchive::importFolder(Pk2Node* parentFolder, const std::wstring& diskDir,
                               const ProgressFn& progress) {
    const std::wstring baseInner = parentFolder ? parentFolder->fullPath : std::wstring();
    const std::wstring rootName  = fileNameOf(diskDir);

    struct Item { std::wstring disk; std::wstring inner; };
    std::vector<Item> items;
    std::vector<std::pair<std::wstring, std::wstring>> stack{
        { diskDir, baseInner.empty() ? rootName : (baseInner + L"\\" + rootName) } };

    while (!stack.empty()) {
        auto [dd, ii] = stack.back(); stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE fh = FindFirstFileW((dd + L"\\*").c_str(), &fd);
        if (fh == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring nm = fd.cFileName;
            if (nm == L"." || nm == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                stack.push_back({ dd + L"\\" + nm, ii + L"\\" + nm });
            else
                items.push_back({ dd + L"\\" + nm, ii + L"\\" + nm });
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }

    u64 i = 0;
    for (const Item& it : items) {
        const size_t slash = it.inner.find_last_of(L'\\');
        const std::wstring folder = (slash == std::wstring::npos) ? L"" : it.inner.substr(0, slash);
        const std::wstring leaf   = (slash == std::wstring::npos) ? it.inner : it.inner.substr(slash + 1);
        // Paths only - each write rebuilds the tree and invalidates node pointers.
        Result r = importFileToPath(folder, it.disk, leaf, true);
        if (!r.ok()) return r;
        if (!ReportProgress(progress, ++i, items.size(), leaf.c_str()))
            return Result::Fail(Status::Cancelled);
    }
    return Result::Ok();
}

Result VpArchive::addFolder(Pk2Node* parentFolder, const std::wstring& name, Pk2Node** out) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Result rw = requireWritable();
    if (!rw.ok()) return rw;
    if (name.empty()) return Result::Fail(Status::InvalidArgument, T(L"Ad bos."));

    const std::wstring folder = parentFolder ? parentFolder->fullPath : std::wstring();
    const std::wstring full   = folder.empty() ? name : (folder + L"\\" + name);
    if (m_byPath.count(lower(full))) return Result::Fail(Status::AlreadyExists, T(L"Zaten var."));

    // The format stores no standalone folder rows (the reader drops them), so an
    // empty folder lives in the session tree until it holds a file.
    m_emptyFolders.push_back(full);
    Pk2Node* n = intern(full);
    if (out) *out = n;
    return Result::Ok();
}

Result VpArchive::remove(Pk2Node* node) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Result rw = requireWritable();
    if (!rw.ok()) return rw;
    if (!node || node->isRoot()) return Result::Fail(Status::InvalidArgument);

    const std::wstring target = lower(node->fullPath);
    const std::wstring prefix = target + L"\\";
    size_t removed = 0;
    for (size_t i = m_recs.size(); i-- > 0; ) {
        const std::wstring rp = lower(m_recs[i].path);
        if (rp == target || rp.compare(0, prefix.size(), prefix) == 0) {
            release(m_recs[i].chunkOffset, m_recs[i].size);
            m_recs.erase(m_recs.begin() + (ptrdiff_t)i);
            ++removed;
        }
    }
    for (size_t i = m_emptyFolders.size(); i-- > 0; ) {
        const std::wstring fp = lower(m_emptyFolders[i]);
        if (fp == target || fp.compare(0, prefix.size(), prefix) == 0)
            m_emptyFolders.erase(m_emptyFolders.begin() + (ptrdiff_t)i);
    }
    if (removed == 0 && !node->isFolder()) return Result::Fail(Status::NotFound);

    Result r = saveMetadata();
    if (!r.ok()) return r;
    rebuildTree();
    return Result::Ok();
}

Result VpArchive::rename(Pk2Node* node, const std::wstring& newName) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Result rw = requireWritable();
    if (!rw.ok()) return rw;
    if (!node || node->isRoot() || newName.empty()) return Result::Fail(Status::InvalidArgument);
    if (newName.find_first_of(L"\\/") != std::wstring::npos)
        return Result::Fail(Status::InvalidArgument, T(L"Ad ayrac icermemeli."));

    const std::wstring oldPath = node->fullPath;
    const size_t slash = oldPath.find_last_of(L'\\');
    const std::wstring parent = (slash == std::wstring::npos) ? L"" : oldPath.substr(0, slash);
    const std::wstring newPath = parent.empty() ? newName : (parent + L"\\" + newName);
    if (m_byPath.count(lower(newPath))) return Result::Fail(Status::AlreadyExists, T(L"Bu ad kullaniliyor."));

    const std::wstring oldLow = lower(oldPath);
    const std::wstring pfx    = oldLow + L"\\";

    // The payload keystream is path-keyed, so every affected encrypted record
    // must be re-encrypted under its new path or it becomes unreadable. That is
    // a WRITE to live data, and rule 0.5 says never leave a user file half-done.
    // So: (1) resolve the new path of every record and precompute all the
    // re-encrypted bytes in memory - if any read/re-encrypt fails, NOTHING has
    // been written and the file is untouched; (2) only then write, keeping the
    // original bytes so a mid-loop I/O error can be rolled back.
    struct PendingWrite { i64 off; std::vector<u8> enc, orig; };
    std::vector<PendingWrite>          writes;
    std::vector<std::pair<size_t, std::wstring>> pathUpdates;   // rec index -> new path

    for (size_t i = 0; i < m_recs.size(); ++i) {
        Rec& r = m_recs[i];
        const std::wstring rl = lower(r.path);
        std::wstring np;
        if (rl == oldLow) np = newPath;
        else if (rl.compare(0, pfx.size(), pfx) == 0) np = newPath + r.path.substr(oldPath.size());
        else continue;

        if ((m_fullEncrypted || r.encrypted) && r.size > 0) {
            std::vector<u8> raw, plain, enc;
            Result rr = readRaw(r.chunkOffset, r.size, raw);
            if (!rr.ok()) return rr;                       // nothing written yet
            if (!vp::AesEcbKeystreamXor(m_xorKey, vp::FileNonce(toUtf8(r.path)),
                                        raw.data(), raw.size(), plain) ||
                !vp::AesEcbKeystreamXor(m_xorKey, vp::FileNonce(toUtf8(np)),
                                        plain.data(), plain.size(), enc))
                return Result::Fail(Status::Unknown, T(L"Yeniden sifreleme basarisiz."));
            writes.push_back({ r.chunkOffset, std::move(enc), std::move(raw) });
        }
        pathUpdates.emplace_back(i, np);
    }

    // Write phase. A keystream XOR preserves length, so each write is in place;
    // if one fails, roll the earlier ones back to their original bytes so the
    // container stays exactly as it was before the rename.
    for (size_t w = 0; w < writes.size(); ++w) {
        Result rr = writeRaw(writes[w].off, writes[w].enc.data(), (i64)writes[w].enc.size());
        if (!rr.ok()) {
            for (size_t b = 0; b < w; ++b)
                writeRaw(writes[b].off, writes[b].orig.data(), (i64)writes[b].orig.size());
            return rr;
        }
    }
    for (const auto& pu : pathUpdates) m_recs[pu.first].path = pu.second;
    for (std::wstring& f : m_emptyFolders) {
        const std::wstring fl = lower(f);
        if (fl == oldLow) f = newPath;
        else if (fl.compare(0, pfx.size(), pfx) == 0) f = newPath + f.substr(oldPath.size());
    }

    Result r = saveMetadata();
    if (!r.ok()) return r;
    rebuildTree();
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// Compact - rewrite the container without dead space (keeps a .bak)
// ---------------------------------------------------------------------------
Result VpArchive::compact(u64* savedBytes, const ProgressFn& progress) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Result rw = requireWritable();
    if (!rw.ok()) return rw;

    const u64 before = m_fileSize;
    const std::wstring newPath = m_path + L".new";
    const std::wstring bakPath = m_path + L".bak";

    // 1) copy the fixed header prefix verbatim
    const i64 headerLen = m_metaFieldPos + 16;
    std::vector<u8> header;
    Result r = readRaw(0, headerLen, header);
    if (!r.ok()) return r;

    HANDLE nh = CreateFileW(newPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nh == INVALID_HANDLE_VALUE)
        return Result::Fail(Status::IoError, T(L"Gecici dosya olusturulamadi."));
    HandleGuard ng(nh);

    auto writeAt = [&](i64 off, const u8* d, i64 sz) -> bool {
        LARGE_INTEGER li; li.QuadPart = off;
        if (!SetFilePointerEx(nh, li, nullptr, FILE_BEGIN)) return false;
        size_t done = 0;
        while (done < (size_t)sz) {
            DWORD want = (DWORD)(std::min<i64>)(1 << 20, sz - (i64)done), put = 0;
            if (!WriteFile(nh, d + done, want, &put, nullptr) || put == 0) return false;
            done += put;
        }
        return true;
    };
    if (!writeAt(0, header.data(), (i64)header.size()))
        return Result::Fail(Status::IoError, T(L"Baslik yazilamadi."));

    // 2) payloads, packed back to back
    std::vector<Rec> newRecs = m_recs;
    i64 cursor = headerLen;
    std::vector<u8> buf;
    for (size_t i = 0; i < newRecs.size(); ++i) {
        Rec& rec = newRecs[i];
        r = readRaw(rec.chunkOffset, rec.size, buf);
        if (!r.ok()) return r;
        if (!buf.empty() && !writeAt(cursor, buf.data(), (i64)buf.size()))
            return Result::Fail(Status::IoError, T(L"Veri yazilamadi."));
        rec.chunkOffset = cursor;
        cursor += rec.size;
        if (!ReportProgress(progress, i + 1, newRecs.size(), rec.path.c_str()))
            return Result::Fail(Status::Cancelled);
    }

    // 3) fresh metadata at the end, no free chunks left
    std::vector<Rec>   savedRecs   = m_recs;
    std::vector<Chunk> savedChunks = m_chunks;
    const i64 savedOff = m_metaOffset, savedRes = m_metaReserved, savedSize = m_metaSize;
    const u64 savedFileSize = m_fileSize;

    m_recs = newRecs;
    m_chunks.clear();
    std::vector<u8> pt; serializeMetadata(pt);
    std::vector<u8> enc; u8 sig[32];
    const bool cryptoOk = vp::AesGcmEncrypt(m_metaKey, pt.data(), pt.size(), enc) &&
                          vp::HmacSha256(m_signKey, 32, pt.data(), pt.size(), sig);
    if (!cryptoOk) {
        m_recs = savedRecs; m_chunks = savedChunks;
        return Result::Fail(Status::Unknown, T(L"Metadata yazilamadi."));
    }

    const i64 metaOff  = (cursor + 15) & ~15ll;
    const i64 region   = 4 + 32 + (i64)enc.size();
    std::vector<u8> blob; blob.reserve((size_t)region);
    const int sl = 32;
    blob.insert(blob.end(), (const u8*)&sl, (const u8*)&sl + 4);
    blob.insert(blob.end(), sig, sig + 32);
    blob.insert(blob.end(), enc.begin(), enc.end());
    if (!writeAt(metaOff, blob.data(), (i64)blob.size())) {
        m_recs = savedRecs; m_chunks = savedChunks;
        return Result::Fail(Status::IoError, T(L"Metadata yazilamadi."));
    }
    u8 hf[16];
    const int ms = (int)enc.size(), mr = (int)region;
    std::memcpy(hf, &metaOff, 8); std::memcpy(hf + 8, &ms, 4); std::memcpy(hf + 12, &mr, 4);
    if (!writeAt(m_metaFieldPos, hf, 16)) {
        m_recs = savedRecs; m_chunks = savedChunks;
        return Result::Fail(Status::IoError, T(L"Baslik guncellenemedi."));
    }
    FlushFileBuffers(nh);
    LARGE_INTEGER endLi; endLi.QuadPart = metaOff + region;
    SetFilePointerEx(nh, endLi, nullptr, FILE_BEGIN); SetEndOfFile(nh);
    const u64 after = (u64)(metaOff + region);
    ng.release(); CloseHandle(nh);

    // 4) swap the files
    CloseHandle((HANDLE)m_handle); m_handle = nullptr;
    DeleteFileW(bakPath.c_str());
    if (!MoveFileW(m_path.c_str(), bakPath.c_str())) {
        m_recs = savedRecs; m_chunks = savedChunks;
        m_metaOffset = savedOff; m_metaReserved = savedRes; m_metaSize = savedSize;
        m_fileSize = savedFileSize;
        m_handle = CreateFileW(m_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) m_handle = nullptr;
        DeleteFileW(newPath.c_str());
        return Result::Fail(Status::IoError, T(L"Dosya degistirilemedi."));
    }
    if (!MoveFileW(newPath.c_str(), m_path.c_str())) {
        MoveFileW(bakPath.c_str(), m_path.c_str());
        m_recs = savedRecs; m_chunks = savedChunks;
        m_handle = CreateFileW(m_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) m_handle = nullptr;
        return Result::Fail(Status::IoError, T(L"Yeni dosya yerine konamadi."));
    }

    m_handle = CreateFileW(m_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_handle == INVALID_HANDLE_VALUE) { m_handle = nullptr; return Result::Fail(Status::IoError, T(L"Yeniden acilamadi.")); }

    m_metaOffset   = metaOff;
    m_metaSize     = ms;
    m_metaReserved = region;
    m_fileSize     = after;
    rebuildTree();
    if (savedBytes) *savedBytes = (before > after) ? (before - after) : 0;
    return Result::Ok();
}

// ---------------------------------------------------------------------------
void VpArchive::close() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (m_handle) { CloseHandle((HANDLE)m_handle); m_handle = nullptr; }
    m_nodes.clear(); m_byPath.clear(); m_fileRec.clear();
    m_recs.clear(); m_chunks.clear(); m_emptyFolders.clear();
    m_path.clear(); m_fileSize = 0; m_folderCount = m_fileCount = 0;
    m_readOnly = true; m_canSign = false;
    m_metaOffset = m_metaSize = m_metaReserved = m_metaFieldPos = 0;
    SecureZeroMemory(m_metaKey, sizeof(m_metaKey));
    SecureZeroMemory(m_signKey, sizeof(m_signKey));
}

} // namespace sro
