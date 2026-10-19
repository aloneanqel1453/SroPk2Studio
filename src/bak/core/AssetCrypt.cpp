// ============================================================================
//  SRO Asset Studio - core/AssetCrypt.cpp   (see AssetCrypt.h)
// ============================================================================
#include "AssetCrypt.h"

#include <windows.h>
#include <tlhelp32.h>
#include "KeyFinder.h"
#include "AssetCipherProfiles.h"
#include "AssetCipherSbox.h"
#include "AssetCipherAes.h"
#include "AssetCipherVsro.h"
#include "AssetCipherSailor.h"
#include "AssetCipherGopro.h"
#include "IArchive.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace sro {

namespace {

/// A decrypted asset must carry the JMXV magic AND actually look like game data.
/// Format-agnostic on purpose (.bms/.bsr/.ban have different headers): real SRO
/// assets are ~30% zero bytes, ciphertext is ~0.4%, so the zero density alone
/// separates a correct key from a 12-byte magic collision by a wide margin.
bool PlausibleJmxvBody(const u8* d, size_t n) {
    if (n < 64 || !(d[0] == 'J' && d[1] == 'M' && d[2] == 'X' && d[3] == 'V')) return false;
    for (size_t i = 4; i < 12 && i < n; ++i)
        if (d[i] < 0x20 || d[i] > 0x7E) return false;      // "VBMS 0110" etc. is text
    const size_t look = (std::min)(n, (size_t)8192);
    size_t zeros = 0;
    for (size_t i = 16; i < look; ++i) if (d[i] == 0) ++zeros;
    const size_t span = look - 16;
    return span >= 64 && zeros * 100 >= span * 6;          // >=6% zeros
}

} // namespace

bool LooksEncryptedAsset(const u8* data, size_t size, FormatId format) {
    if (!data || size < 4) return false;
    switch (format) {
        // .2dt is deliberately NOT here: a real one starts with a length-prefixed
        // class name ("\x05\0\0\0CNIFServerEvent..."), never JMXV, so demanding
        // the signature marks every last one of them as encrypted.
        case FormatId::Bsr: case FormatId::Bms: case FormatId::Bmt:
        case FormatId::Bsk: case FormatId::Ban: case FormatId::Cpd:
        case FormatId::Efp: case FormatId::Nvm: case FormatId::Dof:
        case FormatId::Ddj:
            break;
        default:
            return false;      // only formats that MUST carry a JMXV signature
    }
    if (data[0] == 'J' && data[1] == 'M' && data[2] == 'X' && data[3] == 'V') return false;

    // Repacked-image and NIF exceptions apply ONLY to .ddj. That is the one
    // extension shards legitimately fill with a foreign format - Crimson Road
    // ships PNGs as vplus/iconpng/*.ddj, and NIF UI resources appear as .ddj.
    // A .bms/.bsr/.bmt/etc. must start with JMXV; it can never be a BMP, so the
    // weak 2-byte "BM" magic (1-in-65536 on random ciphertext) must NOT excuse
    // one of those from the encrypted tally.
    if (format == FormatId::Ddj) {
        struct Sig { const char* magic; size_t len; };
        static const Sig kOther[] = {
            { "DDS ", 4 }, { "\x89" "PNG", 4 }, { "BM", 2 },
            { "RIFF", 4 }, { "OggS", 4 }, { "\xFF\xD8\xFF", 3 }, { "GIF8", 4 },
        };
        for (const Sig& s : kOther)
            if (size >= s.len && std::memcmp(data, s.magic, s.len) == 0) return false;

        // NIF UI resource: u32 length then the class name ("CNIFEnchantWnd").
        if (size >= 8 && std::memcmp(data + 4, "CNIF", 4) == 0) return false;
    }

    // Pre-JMXV Silkroad assets carry a bare three-letter tag instead:
    // "BAN e...", "BSK e...". They are plaintext and still load, so calling them
    // encrypted is a false alarm. Ciphertext lands on three capitals plus a
    // space about four times in a million, which is a fine trade.
    if (size >= 4 && data[3] == ' ' &&
        data[0] >= 'A' && data[0] <= 'Z' &&
        data[1] >= 'A' && data[1] <= 'Z' &&
        data[2] >= 'A' && data[2] <= 'Z')
        return false;

    return true;
}

// ---------------------------------------------------------------------------
KeystreamVault& KeystreamVault::Instance() {
    static KeystreamVault v;
    return v;
}

size_t KeystreamVault::learn(const u8* cipher, size_t cipherLen,
                             const u8* plain, size_t plainLen) {
    if (!cipher || !plain) return 0;
    const size_t n = (std::min)(cipherLen, plainLen);
    if (n == 0) return 0;

    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_ks.size() < n) { m_ks.resize(n, 0); m_known.resize(n, false); }

    size_t added = 0;
    for (size_t i = 0; i < n; ++i) {
        const u8 k = (u8)(cipher[i] ^ plain[i]);
        if (!m_known[i]) { m_ks[i] = k; m_known[i] = true; ++added; }
        // A conflict means the two samples were not the same asset - trust the
        // first observation and keep going rather than corrupting the stream.
    }
    if (added) extendCoverageLocked();
    return added;
}

/// Advance the cached contiguous run. Scanning `m_known` from zero on every
/// query costs ~10 ms once the vault holds megabytes, and EVERY read of EVERY
/// file asks - which turned a whole-archive pass into half an hour of pure
/// bit-walking. The run only ever grows, so resume from where it stopped.
void KeystreamVault::extendCoverageLocked() {
    while (m_coverage < m_known.size() && m_known[m_coverage]) ++m_coverage;
}

size_t KeystreamVault::coveredPrefix(size_t len) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return (std::min)(len, m_coverage);
}

bool KeystreamVault::decrypt(std::vector<u8>& data) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (data.empty()) return false;
    if (m_coverage < data.size()) return false;
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = (u8)(data[i] ^ m_ks[i]);
    return true;
}

size_t KeystreamVault::coverage() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_coverage;
}

size_t KeystreamVault::knownBytes() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    size_t n = 0;
    for (bool b : m_known) if (b) ++n;
    return n;
}

void KeystreamVault::clear() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_ks.empty()) SecureZeroMemory(m_ks.data(), m_ks.size());
    m_ks.clear();
    m_known.clear();
    m_coverage = 0;
}


// ---------------------------------------------------------------------------
void KeystreamVault::seedFromTables(const std::vector<u8>& A, const std::vector<u8>& B,
                                    u32 start, size_t bytes) {
    if (A.empty() || B.empty() || bytes == 0) return;
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_ks.size() < bytes) { m_ks.resize(bytes, 0); m_known.resize(bytes, false); }
    for (size_t i = 0; i < bytes; ++i) {
        const size_t pos = (size_t)start + i;
        m_ks[i]    = (u8)(A[pos % A.size()] - B[pos % B.size()]);
        m_known[i] = true;
    }
    extendCoverageLocked();
}

// ---------------------------------------------------------------------------
bool DiscoverKeystreamTables(const u8* image, size_t imageSize, u64 imageBase,
                             const u8* cipherSample, size_t sampleLen,
                             const u8* magic, size_t magicLen,
                             KeystreamTables& out) {
    if (!image || imageSize < 0x1000 || !cipherSample || magicLen < 8) return false;

    // The decrypt loop reads both tables through absolute immediates:
    //   0F B6 82 <A32>        movzx eax, byte ptr [edx + A]
    //   2A 83   <B32>         sub    al,  byte ptr [ebx + B]
    // Find that exact pairing; the modulus of each table is the constant the
    // index was reduced by, but we do not need it - we recover the sizes by
    // solving for a start offset that reproduces the known magic.
    for (size_t i = 0; i + 13 <= imageSize; ++i) {
        if (!(image[i] == 0x0F && image[i+1] == 0xB6 && image[i+2] == 0x82)) continue;
        if (!(image[i+7] == 0x2A && image[i+8] == 0x83)) continue;
        u32 va1 = 0, va2 = 0;
        std::memcpy(&va1, image + i + 3,  4);
        std::memcpy(&va2, image + i + 9,  4);
        if (va1 < imageBase || va2 < imageBase) continue;
        const u64 ra = va1 - imageBase, rb = va2 - imageBase;
        if (ra + 8 >= imageSize || rb + 8 >= imageSize) continue;

        // Table sizes come from the two modulus constants in the same loop.
        // Scan a short window backwards for `imul eax, eax, imm32` (69 C0 ..).
        u32 mods[4] = { 0, 0, 0, 0 }; int nm = 0;
        const size_t lo = (i > 0x60) ? i - 0x60 : 0;
        for (size_t j = lo; j + 6 <= i && nm < 4; ++j) {
            if (image[j] == 0x69 && image[j+1] == 0xC0) {
                u32 v = 0; std::memcpy(&v, image + j + 2, 4);
                if (v >= 8 && v <= 65536) mods[nm++] = v;
            }
        }
        if (nm < 2) continue;

        // mods[] arrive in code order: first the %A modulus, then %B.
        for (int p = 0; p < nm; ++p) {
            for (int q = 0; q < nm; ++q) {
                if (p == q) continue;
                const u32 sa = mods[q], sb = mods[p];   // A indexed by 2nd, B by 1st
                if (ra + sa > imageSize || rb + sb > imageSize) continue;
                std::vector<u8> A(image + ra, image + ra + sa);
                std::vector<u8> B(image + rb, image + rb + sb);
                const u64 period = (u64)sa * sb;
                for (u64 st = 0; st < period; ++st) {
                    // cheap gate: the magic must come out byte-exact
                    bool ok = true;
                    for (size_t k = 0; k < magicLen; ++k) {
                        const u64 pos = st + k;
                        if ((u8)(cipherSample[k] ^ (u8)(A[pos % sa] - B[pos % sb])) != magic[k]) {
                            ok = false; break;
                        }
                    }
                    if (!ok) continue;

                    // A 12-byte magic is not proof - two different (table,start)
                    // triples can satisfy it. Decrypt the WHOLE sample and demand
                    // a structurally sane JMXV header before believing it.
                    std::vector<u8> dec(sampleLen);
                    for (size_t k = 0; k < sampleLen; ++k) {
                        const u64 pos = st + k;
                        dec[k] = (u8)(cipherSample[k] ^ (u8)(A[pos % sa] - B[pos % sb]));
                    }
                    if (!PlausibleJmxvBody(dec.data(), dec.size())) continue;

                    out.a = std::move(A); out.b = std::move(B); out.start = (u32)st;
                    return true;
                }
            }
        }
    }
    return false;
}


// ---------------------------------------------------------------------------
bool DiscoverCipherFromProcess(u32 pid,
                               const u8* cipherSample, size_t sampleLen,
                               const u8* magic, size_t magicLen,
                               KeystreamTables& out, std::wstring* whereTr) {
    HANDLE hp = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, (DWORD)pid);
    if (!hp) return false;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, (DWORD)pid);
    if (snap == INVALID_HANDLE_VALUE) { ::CloseHandle(hp); return false; }

    bool found = false;
    MODULEENTRY32W me{}; me.dwSize = sizeof(me);
    if (::Module32FirstW(snap, &me)) {
        do {
            // The OS modules never carry a shard cipher; skip the noise.
            std::wstring path = me.szExePath;
            for (wchar_t& c : path) c = (wchar_t)towlower(c);
            if (path.find(L"\\windows\\") != std::wstring::npos) continue;
            if (me.modBaseSize < 0x2000 || me.modBaseSize > 256u * 1024 * 1024) continue;

            std::vector<u8> img;
            try { img.resize(me.modBaseSize); } catch (...) { continue; }
            SIZE_T got = 0, done = 0; bool any = false;
            while (done < img.size()) {
                const SIZE_T chunk = (std::min)((SIZE_T)65536, img.size() - done);
                if (::ReadProcessMemory(hp, me.modBaseAddr + done, img.data() + done, chunk, &got) && got)
                    any = true;
                done += chunk;
            }
            if (!any) continue;

            if (DiscoverKeystreamTables(img.data(), img.size(), (u64)(uintptr_t)me.modBaseAddr,
                                        cipherSample, sampleLen, magic, magicLen, out)) {
                if (whereTr) *whereTr = me.szModule;
                found = true;
                break;
            }
        } while (::Module32NextW(snap, &me));
    }
    ::CloseHandle(snap); ::CloseHandle(hp);
    return found;
}


// ---------------------------------------------------------------------------
bool RecoverAssetCipher(IArchive& archive, const std::wstring& archiveDir,
                        std::wstring* whereTr, bool* hadEncrypted) {
    if (hadEncrypted) *hadEncrypted = false;
    if (!archive.isOpen()) return false;
    if (KeystreamVault::Instance().coverage() > 0) return true;   // already solved

    // Pick one genuinely encrypted asset - the search needs a ciphertext whose
    // plaintext magic we can predict from the extension.
    std::vector<u8> sample; const char* magic = nullptr;
    ProgressFn none;
    archive.walkAll([&](Pk2Node* n) {
        if (magic || !n->isFile()) return true;
        std::wstring low = n->name;
        for (wchar_t& c : low) c = (wchar_t)towlower(c);
        const size_t dot = low.find_last_of(L'.');
        if (dot == std::wstring::npos) return true;
        const std::wstring ext = low.substr(dot);
        const char* m = (ext == L".bms") ? "JMXVBMS 0110"
                      : (ext == L".bsr") ? "JMXVBSR 0109" : nullptr;
        if (!m) return true;
        std::vector<u8> buf;
        if (!archive.readFile(n, buf).ok() || buf.size() < 256) return true;
        if (buf[0] == 'J' && buf[1] == 'M') return true;       // already plaintext
        sample.swap(buf); magic = m;
        return false;
    }, none);
    if (!magic) return false;                                   // nothing encrypted
    if (hadEncrypted) *hadEncrypted = true;

    // 0) A cipher we already know. Nothing is remembered between runs and no
    //    file name is trusted: each profile must actually decrypt this archive's
    //    own sample to a structurally valid asset before it is accepted, so the
    //    wrong shard's tables are rejected outright.
    {
        size_t nProfiles = 0;
        const assetcipher::Profile* profs = assetcipher::Profiles(nProfiles);
        for (size_t i = 0; i < nProfiles; ++i) {
            const assetcipher::Profile& pr = profs[i];
            const size_t n = (std::min)(sample.size(), (size_t)8192);
            std::vector<u8> dec(n);
            for (size_t k = 0; k < n; ++k) {
                const size_t pos = (size_t)pr.start + k;
                dec[k] = (u8)(sample[k] ^ (u8)(pr.a[pos % pr.aLen] - pr.b[pos % pr.bLen]));
            }
            if (std::memcmp(dec.data(), magic, std::strlen(magic)) != 0) continue;
            if (!PlausibleJmxvBody(dec.data(), dec.size())) continue;
            KeystreamVault::Instance().seedFromTables(
                std::vector<u8>(pr.a, pr.a + pr.aLen),
                std::vector<u8>(pr.b, pr.b + pr.bLen), pr.start, 8u * 1024 * 1024);
            if (whereTr) *whereTr = std::wstring(T(pr.nameTr)) + T(L" (dogrulandi)");
            return true;
        }
    }

    // 0b) HntSRO / SilkroadKit S-box: a per-extension BYTE SUBSTITUTION (not a
    //     keystream), lifted from the client's GFXClientMod hook. The tables are
    //     compiled in, so confirming the archive here is enough - every asset
    //     then decrypts statically in DecryptIfEncrypted with no client at all.
    if (SboxConfirmFromSample(sample.data(), sample.size(), magic, std::strlen(magic))) {
        if (whereTr) *whereTr = T(L"HntSRO S-box (GFXClientMod, uzantiya gore)");
        return true;
    }

    // 0c) Single-byte XOR cipher (ArcaneMax Mystic/Winter Dragon assets with key 0xC3)
    if (sample.size() >= 16 && magic && std::strlen(magic) >= 4) {
        const u8 k = (u8)(sample[0] ^ magic[0]);
        if (k != 0 && (u8)(sample[1] ^ magic[1]) == k &&
            (u8)(sample[2] ^ magic[2]) == k && (u8)(sample[3] ^ magic[3]) == k) {
            std::vector<u8> dec = sample;
            for (size_t i = 0; i < dec.size(); ++i) dec[i] ^= k;
            if (PlausibleJmxvBody(dec.data(), dec.size())) {
                if (whereTr) {
                    wchar_t msg[128];
                    swprintf_s(msg, T(L"Tek bayt XOR (0x%02X - Mystic/Winter Dragon)"), k);
                    *whereTr = msg;
                }
                return true;
            }
        }
    }

    // Preferred: solve it from the archive alone. No client, no dropped DLL,
    // nothing to ship - the construction makes the corpus itself sufficient.
    {
        std::wstring how;
        if (SolveAssetCipherFromArchive(archive, &how)) {
            if (whereTr) *whereTr = how;
            return true;
        }
    }

    // Preferred once a dump exists: read the cipher out of the module images the
    // user already captured for THIS game. No client, and nothing remembered.
    {
        KeystreamTables t; std::wstring where;
        if (DiscoverCipherFromDumps(archiveDir, sample.data(), sample.size(),
                                    (const u8*)magic, std::strlen(magic), t, &where)) {
            KeystreamVault::Instance().seedFromTables(t.a, t.b, t.start, 8u * 1024 * 1024);
            if (whereTr) *whereTr = where + T(L" (dump - istemci gerekmedi)");
            return true;
        }
    }

    // Whole-file AES. Distinct from everything above: those are byte-substitution
    // or keystream ciphers that can be solved from the ciphertext alone, whereas
    // an AES key cannot - it has to come out of a binary. What CAN be done from
    // the archive alone is proving a candidate (the IV-free 96-bit test) and
    // then deriving the IV, so the scan only ever has to supply the key.
    // Nothing here looks at a file NAME: the modules that carry the key are
    // named randomly per client build.
    {
        std::vector<AesSample> aesSamples;
        AesCollectSamples(archive, aesSamples);
        if (!aesSamples.empty()) {
            std::vector<std::wstring> bins;
            AesScanCandidates(archive.path(), bins);
            AesAssetSpec  spec;
            AesFindReport rep;
            LogFn         noLog;
            // HARD DEADLINE. This runs on the UI's single worker during archive
            // open, so an unbounded sweep does not merely feel slow - it makes
            // the next thing the user clicks report "an operation is already
            // running" and do nothing. Measured: an unbounded sweep held the
            // worker for minutes and swallowed an Export the user had asked for.
            // The cheap pass finds a plain-string key almost immediately; if it
            // does not, the user can run the full search deliberately.
            const ULONGLONG deadline = ::GetTickCount64() + 15000;
            ProgressFn budget = [deadline](u64, u64, const wchar_t*) -> bool {
                return ::GetTickCount64() < deadline;
            };
            if (AesFindCipher(aesSamples, bins, spec, rep, budget, noLog)) {
                AesSetSpec(spec);
                // Contentless: the route to a key is as sensitive as the key.
                if (whereTr) *whereTr = T(L"varlık şifresi çözüldü");
                return true;
            }
        }
    }

    // Last resort: lift the tables out of a client that happens to be running.
    const std::vector<DWORD> pids = KeyFinder::FindRunningClientProcesses(archiveDir);
    for (DWORD pid : pids) {
        KeystreamTables t;
        if (!DiscoverCipherFromProcess((u32)pid, sample.data(), sample.size(),
                                       (const u8*)magic, std::strlen(magic), t, whereTr))
            continue;
        // The keystream is periodic in |A|*|B|, so this covers any file size.
        KeystreamVault::Instance().seedFromTables(t.a, t.b, t.start, 8u * 1024 * 1024);
        return true;
    }
    return false;
}


// ---------------------------------------------------------------------------
// Client-free recovery. See the header for why this is tractable.
// ---------------------------------------------------------------------------
namespace {

struct Sample { std::vector<u8> bytes; const char* magic; };

/// Collect encrypted assets whose plaintext magic we can predict.
void CollectEncryptedSamples(IArchive& ar, size_t maxFiles, size_t maxTotal,
                             std::vector<Sample>& out) {
    size_t total = 0;
    ProgressFn none;
    ar.walkAll([&](Pk2Node* n) {
        if (out.size() >= maxFiles || total >= maxTotal) return false;
        if (!n->isFile()) return true;
        std::wstring low = n->name;
        for (wchar_t& c : low) c = (wchar_t)towlower(c);
        const size_t dot = low.find_last_of(L'.');
        if (dot == std::wstring::npos) return true;
        const std::wstring ext = low.substr(dot);
        const char* m = (ext == L".bms") ? "JMXVBMS 0110"
                      : (ext == L".bsr") ? "JMXVBSR 0109" : nullptr;
        if (!m) return true;
        std::vector<u8> buf;
        if (!ar.readFile(n, buf).ok() || buf.size() < 512) return true;
        if (buf[0] == 'J' && buf[1] == 'M') return true;          // not encrypted
        total += buf.size();
        out.push_back(Sample{ std::move(buf), m });
        return true;
    }, none);
}

} // namespace

bool SolveAssetCipherFromArchive(IArchive& archive, std::wstring* howTr) {
    if (KeystreamVault::Instance().coverage() > 0) return true;

    std::vector<Sample> samples;
    CollectEncryptedSamples(archive, 400, 48u * 1024 * 1024, samples);
    if (samples.size() < 8) return false;

    // Filter samples to cluster by common 4-byte crib so mixed archives (e.g.
    // ArcaneMax which has Dragon XOR + block cipher + Coiny custom assets) do
    // not poison the alternating maximum likelihood solver.
    std::vector<Sample> filtered;
    {
        std::map<u32, std::vector<size_t>> clusters;
        for (size_t idx = 0; idx < samples.size(); ++idx) {
            const Sample& s = samples[idx];
            if (s.bytes.size() < 4) continue;
            u32 tag = 0;
            for (int i = 0; i < 4; ++i)
                tag = (tag << 8) | (u8)(s.bytes[i] ^ (u8)s.magic[i]);
            clusters[tag].push_back(idx);
        }
        size_t bestCount = 0;
        u32 bestTag = 0;
        for (const auto& kv : clusters) {
            if (kv.second.size() > bestCount) {
                bestCount = kv.second.size();
                bestTag = kv.first;
            }
        }
        if (bestCount < 8) return false;
        for (size_t idx : clusters[bestTag]) filtered.push_back(std::move(samples[idx]));
    }
    samples = std::move(filtered);

    // The crib: every sample must decrypt to its own magic, so those keystream
    // bytes are known exactly and identically for all of them.
    u8 crib[12] = {}; size_t cribLen = 12;
    for (size_t i = 0; i < cribLen; ++i)
        crib[i] = (u8)(samples[0].bytes[i] ^ (u8)samples[0].magic[i]);

    const size_t LIM = 65536;
    size_t maxLen = 0;
    for (const Sample& s : samples) maxLen = (std::max)(maxLen, s.bytes.size());
    const size_t L = (std::min)(maxLen, LIM);

    // Per-position ciphertext histograms - the raw material for the pooling.
    std::vector<u16> hist;
    try { hist.assign(L * 256, 0); } catch (...) { return false; }
    for (const Sample& s : samples) {
        const size_t n = (std::min)(s.bytes.size(), L);
        for (size_t i = 0; i < n; ++i) {
            u16& c = hist[i * 256 + s.bytes[i]];
            if (c < 0xFFFF) ++c;
        }
    }

    // Candidate moduli. These are algorithm parameters, not secrets; the accept
    // test below rejects a wrong pair outright.
    static const std::pair<int,int> kMods[] = {
        { 211, 493 }, { 493, 211 }, { 251, 499 }, { 257, 509 }, { 127, 251 }
    };

    for (const auto& mn : kMods) {
        const int m = mn.first, n = mn.second;
        if ((size_t)m > L || (size_t)n > L) continue;
        std::vector<int> A(m, 0), B(n, 0);

        // Alternating maximum likelihood. score(v) is a cyclic correlation, so a
        // whole table is refined in one pass over the positions.
        for (int pass = 0; pass < 12; ++pass) {
            std::vector<u32> acc((size_t)m * 256, 0);
            for (size_t i = 0; i < L; ++i) {
                const u16* h = &hist[i * 256];
                const int d = B[i % n];
                u32* a = &acc[(size_t)(i % m) * 256];
                for (int v = 0; v < 256; ++v) a[v] += h[(u8)(v - d)];
            }
            for (int r = 0; r < m; ++r) {
                const u32* a = &acc[(size_t)r * 256];
                int best = 0; u32 bv = 0;
                for (int v = 0; v < 256; ++v) if (a[v] > bv) { bv = a[v]; best = v; }
                A[r] = best;
            }
            std::vector<u32> acc2((size_t)n * 256, 0);
            for (size_t i = 0; i < L; ++i) {
                const u16* h = &hist[i * 256];
                const int d = A[i % m];
                u32* b = &acc2[(size_t)(i % n) * 256];
                for (int v = 0; v < 256; ++v) b[v] += h[(u8)(d - v)];
            }
            for (int r = 0; r < n; ++r) {
                const u32* b = &acc2[(size_t)r * 256];
                int best = 0; u32 bv = 0;
                for (int v = 0; v < 256; ++v) if (b[v] > bv) { bv = b[v]; best = v; }
                B[r] = best;
            }
        }

        // Pin the gauge with the crib, then demand it reproduces ALL of it.
        const int delta = (int)(u8)(crib[0] - (u8)(A[0] - B[0]));
        for (int& x : A) x = (u8)(x + delta);
        bool cribOk = true;
        for (size_t i = 0; i < cribLen; ++i)
            if ((u8)(A[i % m] - B[i % n]) != crib[i]) { cribOk = false; break; }
        if (!cribOk) continue;

        // Accept test: real files must come out structurally valid.
        int good = 0, tried = 0;
        for (const Sample& s : samples) {
            if (tried >= 12) break;
            const size_t nb = (std::min)(s.bytes.size(), L);
            if (nb < 64) continue;
            ++tried;
            std::vector<u8> dec(nb);
            for (size_t i = 0; i < nb; ++i) dec[i] = (u8)(s.bytes[i] ^ (u8)(A[i % m] - B[i % n]));
            if (PlausibleJmxvBody(dec.data(), dec.size())) ++good;
        }
        if (tried < 4 || good * 4 < tried * 3) continue;      // need >=75%

        std::vector<u8> ta(m), tb(n);
        for (int i = 0; i < m; ++i) ta[i] = (u8)A[i];
        for (int i = 0; i < n; ++i) tb[i] = (u8)B[i];
        KeystreamVault::Instance().seedFromTables(ta, tb, 0, 8u * 1024 * 1024);
        if (howTr) {
            wchar_t buf[160];
            swprintf(buf, 160, T(L"arsivin kendisinden cozuldu (%zu ornek, %d/%d dogrulandi)"),
                     samples.size(), good, tried);
            *howTr = buf;
        }
        return true;
    }
    return false;
}



// ---------------------------------------------------------------------------
bool DiscoverCipherFromDumps(const std::wstring& searchDir,
                             const u8* cipherSample, size_t sampleLen,
                             const u8* magic, size_t magicLen,
                             KeystreamTables& out, std::wstring* whereTr) {
    // Look in the game folder and in the usual places a module dump lands.
    std::vector<std::wstring> dirs{ searchDir };
    for (const wchar_t* sub : { L"SaveDlls", L"Dump", L"DUMP", L"dumps", L"modules", L"dump" })
        dirs.push_back(PathJoin(searchDir, sub));

    // Biggest first: the shard's own crypto module is large, the OS stubs are not.
    struct Cand { std::wstring path; u64 size; };
    std::vector<Cand> files;
    for (const std::wstring& d : dirs) {
        WIN32_FIND_DATAW fd;
        HANDLE h = ::FindFirstFileW(PathJoin(d, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const u64 sz = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (sz < 0x2000 || sz > 256ull * 1024 * 1024) continue;
            std::wstring name = fd.cFileName;
            std::wstring low = name;
            for (wchar_t& c : low) c = (wchar_t)towlower(c);
            const bool interesting =
                low.size() > 4 && (low.compare(low.size() - 4, 4, L".dll") == 0 ||
                                   low.compare(low.size() - 4, 4, L".exe") == 0 ||
                                   low.compare(low.size() - 4, 4, L".bin") == 0);
            if (!interesting) continue;
            files.push_back(Cand{ PathJoin(d, name), sz });
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    std::sort(files.begin(), files.end(),
              [](const Cand& a, const Cand& b) { return a.size > b.size; });

    for (const Cand& c : files) {
        std::vector<u8> img;
        if (!ReadEntireFile(c.path, img).ok() || img.size() < 0x2000) continue;

        // A module dumped from memory keeps its load address in the PE header,
        // which is exactly the base the absolute table pointers are relative to.
        u64 base = 0;
        if (img.size() > 0x40 && img[0] == 'M' && img[1] == 'Z') {
            u32 e = 0; std::memcpy(&e, img.data() + 0x3C, 4);
            if (e + 0x38 < img.size() && std::memcmp(img.data() + e, "PE\0\0", 4) == 0) {
                u16 magicOpt = 0; std::memcpy(&magicOpt, img.data() + e + 24, 2);
                if (magicOpt == 0x10B) { u32 b32 = 0; std::memcpy(&b32, img.data() + e + 24 + 28, 4); base = b32; }
                else                   { std::memcpy(&base, img.data() + e + 24 + 24, 8); }
            }
        }
        if (!base) continue;

        if (DiscoverKeystreamTables(img.data(), img.size(), base,
                                    cipherSample, sampleLen, magic, magicLen, out)) {
            if (whereTr) *whereTr = PathFileName(c.path);
            return true;
        }
    }
    return false;
}


// ---------------------------------------------------------------------------
namespace {

/// Rough "does this look like real data" score. Higher is better.
int PlausibilityScore(const u8* d, size_t n) {
    if (n < 8) return 0;
    static const char* kSigs[] = { "JMXV", "DDS ", "RIFF", "OggS", "\x89PNG", "BM" };
    for (const char* sg : kSigs) {
        const size_t L = std::strlen(sg);
        if (n >= L && std::memcmp(d, sg, L) == 0) return 1000;
    }
    const size_t look = (std::min)(n, (size_t)4096);
    size_t zeros = 0, printable = 0;
    for (size_t i = 0; i < look; ++i) {
        if (d[i] == 0) ++zeros;
        else if ((d[i] >= 0x20 && d[i] <= 0x7E) || d[i] == '\r' || d[i] == '\n' || d[i] == '\t')
            ++printable;
    }
    // Ciphertext is ~0.4% zeros and ~37% printable by chance; real files are far
    // off that in one direction or the other.
    const int zpc = (int)(zeros * 100 / look);
    const int ppc = (int)(printable * 100 / look);
    if (ppc >= 92) return 800;               // text file
    return zpc * 4 + (ppc > 60 ? ppc : 0);
}

} // namespace

bool DecryptIfEncrypted(const std::wstring& fileName, std::vector<u8>& buf) {
    if (buf.size() < 16) return false;

    // 0) Sailor Online "Custom Encryption": server-sensitive textdata files
    //    (itemdata_*/skilldata_*/characterdata_*) carry a fixed encrypted-BOM
    //    signature and an additive keystream over a fixed 4096-byte table. Fully
    //    static and self-verifying (accepts only valid UTF-16LE textdata), so it
    //    is a no-op on every other file.
    if (SailorDecryptIfEncrypted(fileName, buf)) return true;

    // 0a) Single-byte XOR (e.g. ArcaneMax Mystic/Winter Dragon assets with key 0xC3)
    if (buf.size() >= 16) {
        const u8 k = (u8)(buf[0] ^ 'J');
        if (k != 0 && (u8)(buf[1] ^ 'M') == k && (u8)(buf[2] ^ 'X') == k && (u8)(buf[3] ^ 'V') == k) {
            for (size_t i = 0; i < buf.size(); ++i) buf[i] ^= k;
            if (PlausibleJmxvBody(buf.data(), buf.size())) return true;
            for (size_t i = 0; i < buf.size(); ++i) buf[i] ^= k; // revert if invalid
        }
    }

    // 0b) FYSRO prefix ("FYSRO_MESH" 10 bytes)
    if (buf.size() > 14 && std::memcmp(buf.data(), "FYSRO_MESH", 10) == 0) {
        buf.erase(buf.begin(), buf.begin() + 10);
        if (buf.size() >= 16) {
            const u8 k = (u8)(buf[0] ^ 'J');
            if (k != 0 && (u8)(buf[1] ^ 'M') == k && (u8)(buf[2] ^ 'X') == k && (u8)(buf[3] ^ 'V') == k) {
                for (size_t i = 0; i < buf.size(); ++i) buf[i] ^= k;
                if (PlausibleJmxvBody(buf.data(), buf.size())) return true;
                for (size_t i = 0; i < buf.size(); ++i) buf[i] ^= k;
            }
        }
        return true;
    }

    // 1) Position-keyed XOR keystream (Kim Hoa family), when the vault covers it.
    if (KeystreamVault::Instance().coverage() >= buf.size()) {
        const FormatId fmt = IdentifyByExtension(fileName);
        const bool magicSaysEncrypted = LooksEncryptedAsset(buf.data(), buf.size(), fmt);

        std::vector<u8> cand = buf;
        if (KeystreamVault::Instance().decrypt(cand)) {
            if (magicSaysEncrypted) {
                // The type demands a JMXV signature - take it only if it now has one.
                if (cand[0] == 'J' && cand[1] == 'M' && cand[2] == 'X' && cand[3] == 'V') {
                    buf.swap(cand);
                    return true;
                }
            } else {
                // Unknown/!JMXV type: accept only a clear improvement, so normal
                // files are never mangled.
                const int before = PlausibilityScore(buf.data(), buf.size());
                const int after  = PlausibilityScore(cand.data(), cand.size());
                if (after >= 800 && after > before * 2) { buf.swap(cand); return true; }
            }
        }
    }

    // 2) HntSRO / SilkroadKit per-extension S-box substitution. Fully static: no
    //    key, no client, no samples - the compiled-in tables ARE the cipher.
    if (SboxDecryptIfEncrypted(fileName, buf)) return true;

    // 3) "vSRO File Protection" (vSroAssetGuard.dll): AES-256-CTR keyed off the
    //    shard's own gateway, nonce per inner path. Armed on archive open when a
    //    divisioninfo.txt is present; a no-op otherwise. Needs the FULL inner
    //    path, which is what callers pass.
    if (VsroDecryptIfEncrypted(fileName, buf)) return true;

    // 4) GoPro / VSROPlus (vsroplus_lib.dll): AES-256-CBC per-asset encryption
    //    (Republic90 and modern VSROPlus shards). Self-verifying via 16-byte
    //    JMXV probe block.
    if (GoproDecryptIfEncrypted(fileName, buf)) return true;

    // 5) Whole-file AES (the Origin Online / Genesis family and anything shaped
    //    like it). The key is NOT compiled in - it is discovered from the
    //    client's own binaries on archive open and lives in memory only, so a
    //    re-keyed client build keeps working instead of silently decrypting to
    //    garbage. Disarmed until AesTryInit proves a key, and it self-verifies
    //    on the JMXV signature, so a foreign file is never mangled.
    if (AesDecryptIfEncrypted(fileName, buf)) return true;

    return false;
}

bool StillLooksEncrypted(const std::wstring& fileName, const u8* data, size_t size) {
    if (!data || size < 64) return false;
    const FormatId fmt = IdentifyByExtension(fileName);
    if (LooksEncryptedAsset(data, size, fmt)) return true;      // needs JMXV, has none
    if (SailorLooksEncrypted(data, size)) return true;          // Sailor encrypted-BOM sig
    if (GoproLooksEncrypted(data, size)) return true;           // GoPro AES-256-CBC sig

    // Only a format with a MANDATORY header can be judged. Scoring entropy
    // instead marks ordinary files as ciphertext - a .txt in a non-ASCII code
    // page and every length-prefixed binary look exactly like noise to a
    // printable-byte count, and crying wolf on those is worse than staying quiet.
    struct Sig { const wchar_t* ext; const char* magic; size_t len; };
    static const Sig kSigs[] = {
        { L".dds", "DDS ",       4 },
        { L".png", "\x89" "PNG", 4 },
        { L".bmp", "BM",         2 },
        { L".ogg", "OggS",       4 },
        { L".wav", "RIFF",       4 },
        { L".jpg", "\xFF\xD8\xFF", 3 },
    };
    const std::wstring ext = PathExtension(fileName);
    for (const Sig& s : kSigs)
        if (ext == s.ext) return std::memcmp(data, s.magic, s.len) != 0;
    return false;
}

} // namespace sro
