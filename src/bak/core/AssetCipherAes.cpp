// ============================================================================
//  SRO Asset Studio - core/AssetCipherAes.cpp   (see AssetCipherAes.h)
// ============================================================================
#include "AssetCipherAes.h"

#include "AesCore.h"
#include "AssetCrypt.h"
#include "IArchive.h"
#include "I18n.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace sro {

namespace {

// ---------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------
std::mutex   g_specMx;
AesAssetSpec g_spec;
bool         g_armed = false;

inline bool IsKeyByte(u8 c) { return c >= 0x21 && c <= 0x7E; }

inline u32 ReadU32LE(const u8* p) {
    return  static_cast<u32>(p[0])        | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

/// Extension -> the 8-byte tag its plaintext must start with. Same table the
/// asset dumper uses; kept local so this file has no dependency on KeyFinder.
std::string SigForExtension(const std::wstring& fileName) {
    std::wstring e = PathExtension(fileName);
    for (wchar_t& c : e) c = static_cast<wchar_t>(towlower(c));
    if (e == L".bsr") return "JMXVRES ";
    if (e == L".bms") return "JMXVBMS ";
    if (e == L".bmt") return "JMXVBMT ";
    if (e == L".bsk") return "JMXVBSK ";
    if (e == L".ban") return "JMXVBAN ";
    if (e == L".ddj") return "JMXVDDJ ";
    if (e == L".cpd") return "JMXVCPD ";
    if (e == L".efp") return "JMXVEFF ";
    if (e == L".nvm") return "JMXVNVM ";
    if (e == L".dof") return "JMXVDOF ";
    return std::string();
}

// ---------------------------------------------------------------------------
// Candidate harvesting
//
// Two shapes, because two shapes occur in the wild:
//   stride 1  - the key sitting in .rdata as a plain 16-byte string
//   stride s  - one byte per instruction, e.g. the 16 `mov byte ptr
//               [esp+disp32], imm8` stores that build an IV on the stack, whose
//               immediates land exactly 8 bytes apart.
// Requiring all 16 bytes printable is what keeps this tractable: on random code
// that is 0.37^16 per position, so the strided passes contribute a few hundred
// windows on a 28 MB module rather than hundreds of millions.
// ---------------------------------------------------------------------------
using Window = std::array<u8, 32>;

struct Harvest {
    std::vector<std::array<u8, 16>> values;   ///< unique 16-byte windows
    std::vector<u64>                offsets;  ///< where each was found
    std::vector<int>                strides;
    u64                             capped = 0;
};

struct ArrHash {
    size_t operator()(const std::array<u8, 16>& a) const noexcept {
        size_t h = 1469598103934665603ull;
        for (u8 b : a) { h ^= b; h *= 1099511628211ull; }
        return h;
    }
};

constexpr size_t kMaxCandidates = 6u * 1000u * 1000u;
/// Total bytes the sweep may read across ALL binaries. A client is a few
/// hundred MB at most; anything beyond this means the scope is wrong.
constexpr u64 kScanBudgetBytes = 512ull * 1024 * 1024;

void HarvestWindows(const u8* buf, size_t size, Harvest& out,
                    int strideMin = 1, int strideMax = 16) {
    if (size < 16) return;
    std::unordered_set<std::array<u8, 16>, ArrHash> seen;
    seen.reserve(1 << 16);

    auto emit = [&](size_t start, int stride) {
        std::array<u8, 16> v{};
        for (int k = 0; k < 16; ++k) v[static_cast<size_t>(k)] = buf[start + static_cast<size_t>(k) * static_cast<size_t>(stride)];
        if (!seen.insert(v).second) return;
        if (out.values.size() >= kMaxCandidates) { ++out.capped; return; }
        out.values.push_back(v);
        out.offsets.push_back(start);
        out.strides.push_back(stride);
    };

    // Contiguous first: the key itself is normally a plain string, so proving it
    // early means the strided passes usually only have to supply the IV.
    for (int stride = strideMin; stride <= strideMax; ++stride) {
        const size_t span = static_cast<size_t>(stride) * 15;
        if (span >= size) break;
        for (int r = 0; r < stride; ++r) {
            size_t run = 0;
            for (size_t i = static_cast<size_t>(r); i < size; i += static_cast<size_t>(stride)) {
                if (IsKeyByte(buf[i])) {
                    ++run;
                    if (run >= 16) emit(i - span, stride);
                } else {
                    run = 0;
                }
            }
        }
        if (out.values.size() >= kMaxCandidates) break;
    }
}

/// Whole printable tokens, not every window inside them. See fastpass note in
/// the header: a stored key is a complete NUL-delimited string, so its window
/// starts where a run starts.
void HarvestDelimited(const u8* buf, size_t size, Harvest& out) {
    if (size < 16) return;
    std::unordered_set<std::array<u8, 16>, ArrHash> seen;
    seen.reserve(1 << 14);

    auto emit = [&](size_t start) {
        if (start + 16 > size) return;
        std::array<u8, 16> v{};
        std::memcpy(v.data(), buf + start, 16);
        if (!seen.insert(v).second) return;
        if (out.values.size() >= kMaxCandidates) { ++out.capped; return; }
        out.values.push_back(v);
        out.offsets.push_back(start);
        out.strides.push_back(1);
    };

    size_t i = 0;
    while (i < size) {
        if (!IsKeyByte(buf[i])) { ++i; continue; }
        const size_t begin = i;
        while (i < size && IsKeyByte(buf[i])) ++i;
        const size_t len = i - begin;
        if (len < 16) continue;
        emit(begin);                    // the token, or its head
        emit(i - 16);                   // its tail
    }
}

/// Later pass, only when the cheap ones proved nothing: every 16-byte
/// window regardless of content. Expensive but bounded, and it is the honest
/// fallback for a key that is raw bytes rather than a passphrase.
void HarvestRaw(const u8* buf, size_t size, size_t step, Harvest& out) {
    if (size < 16) return;
    std::unordered_set<std::array<u8, 16>, ArrHash> seen;
    seen.reserve(1 << 18);
    for (size_t i = 0; i + 16 <= size; i += step) {
        std::array<u8, 16> v{};
        std::memcpy(v.data(), buf + i, 16);
        if (!seen.insert(v).second) continue;
        if (out.values.size() >= kMaxCandidates) { ++out.capped; return; }
        out.values.push_back(v);
        out.offsets.push_back(i);
        out.strides.push_back(0);
    }
}

// ---------------------------------------------------------------------------
// Sample groups
// ---------------------------------------------------------------------------
struct Group {
    std::string             sig8;
    std::string             knownVersion;   ///< "0109" from a plaintext sibling
    std::vector<const AesSample*> items;    ///< distinct first blocks only
};

std::vector<Group> BuildGroups(const std::vector<AesSample>& samples) {
    std::vector<Group> groups;
    for (const AesSample& s : samples) {
        if (s.sig8.size() != 8 || s.data.size() < 32 || (s.data.size() % 16) != 0) continue;
        Group* g = nullptr;
        for (Group& c : groups) if (c.sig8 == s.sig8) { g = &c; break; }
        if (!g) { groups.push_back(Group{ s.sig8, std::string(), {} }); g = &groups.back(); }
        if (g->knownVersion.empty() && s.knownVersion.size() == 4) g->knownVersion = s.knownVersion;

        // Two files with an identical first ciphertext block prove nothing about
        // the key - the oracle would pass for every candidate.
        bool dup = false;
        for (const AesSample* o : g->items)
            if (std::memcmp(o->data.data(), s.data.data(), 16) == 0) { dup = true; break; }
        if (!dup) g->items.push_back(&s);
    }
    // Strongest group first: more distinct blocks = a stronger consistency test.
    std::stable_sort(groups.begin(), groups.end(),
                     [](const Group& a, const Group& b) { return a.items.size() > b.items.size(); });
    groups.erase(std::remove_if(groups.begin(), groups.end(),
                                [](const Group& g) { return g.items.size() < 2; }),
                 groups.end());
    return groups;
}

/// The 96-bit test described in the header, plus the cross-extension check.
/// `ivHead` comes back with IV[0..7] when it passes.
bool KeyPasses(const aes::Key& k, const std::vector<Group>& groups, u8 ivHead[8]) {
    if (groups.empty()) return false;

    // Only the STRONGEST group decides. Its 12-byte agreement across distinct
    // ciphertext blocks is a 96-bit test - 2^-96 against the ~10^8 candidates a
    // real client yields, so nothing further is needed to be certain.
    //
    // Every other extension is deliberately NOT allowed to veto. An archive can
    // mix ciphers, and "does not start with JMXV" does not prove a file is AES;
    // one such group rejecting the right key is exactly what happened when the
    // collector was widened from 4 extensions to 10.
    // ANY group may be the one this cipher owns - not necessarily the largest.
    // Insisting on groups[0] alone was just the veto bug from the other side:
    // when the strongest extension turned out not to be AES at all, the correct
    // key could never pass. One group agreeing internally on 12 bytes across
    // distinct ciphertext blocks is already 2^-96, so "any" is safe.
    for (const Group& g : groups) {
        u8 first[16];
        aes::DecryptBlock(k, g.items[0]->data.data(), first);

        const size_t n = std::min<size_t>(g.items.size(), 4);
        bool ok = true;
        for (size_t i = 1; i < n; ++i) {
            u8 d[16];
            aes::DecryptBlock(k, g.items[i]->data.data(), d);
            if (std::memcmp(d, first, 12) != 0) { ok = false; break; }
        }
        if (!ok) continue;
        for (int i = 0; i < 8; ++i)
            ivHead[i] = static_cast<u8>(first[i] ^ static_cast<u8>(g.sig8[static_cast<size_t>(i)]));
        return true;
    }
    return false;
}

/// Groups that actually belong to this cipher: same IV head as the strongest
/// one. Used downstream so a foreign group cannot poison the IV solve either.
std::vector<Group> AgreeingGroups(const aes::Key& k, const std::vector<Group>& groups,
                                  const u8 ivHead[8]) {
    std::vector<Group> keep;
    for (const Group& g : groups) {
        u8 d[16];
        aes::DecryptBlock(k, g.items[0]->data.data(), d);
        bool ok = true;
        for (int i = 0; i < 8; ++i)
            if (static_cast<u8>(d[i] ^ static_cast<u8>(g.sig8[static_cast<size_t>(i)])) != ivHead[i]) { ok = false; break; }
        // A matching head on the FIRST file is not enough - the .bsk group on a
        // live client matched there and still disagreed with itself, which is
        // the signature of files that belong to another cipher entirely.
        if (ok) {
            const size_t n = std::min<size_t>(g.items.size(), 4);
            for (size_t i = 1; i < n && ok; ++i) {
                u8 e[16];
                aes::DecryptBlock(k, g.items[i]->data.data(), e);
                if (std::memcmp(e, d, 12) != 0) ok = false;
            }
        }
        if (ok) keep.push_back(g);
    }
    return keep;
}

/// IV[8..11]: the four ASCII version digits. Per byte, keep every value that
/// makes EVERY extension's digit legal.
///
/// Demanding a UNIQUE survivor per byte was too strict: with two extensions the
/// expected number of accidental extras is 10*10/256 = 0.39, so a second
/// candidate turns up often enough to matter, and the whole cryptanalytic path
/// was being abandoned for it. Each extra extension divides that by ~25, so the
/// caller enumerates the (small) product instead and lets full validation pick.
void VersionByteCandidates(const aes::Key& k, const std::vector<Group>& groups,
                           std::vector<u8> cands[4]) {
    std::vector<std::array<u8, 16>> heads;
    heads.reserve(groups.size());
    for (const Group& g : groups) {
        std::array<u8, 16> d{};
        aes::DecryptBlock(k, g.items[0]->data.data(), d.data());
        heads.push_back(d);
    }
    for (int p = 8; p < 12; ++p) {
        for (int v = 0; v < 256; ++v) {
            bool ok = true;
            for (const std::array<u8, 16>& d : heads) {
                const u8 ch = static_cast<u8>(d[static_cast<size_t>(p)] ^ static_cast<u8>(v));
                if (ch < '0' || ch > '9') { ok = false; break; }
            }
            if (ok) cands[p - 8].push_back(static_cast<u8>(v));
        }
    }
}

/// IV[12..15] from a .ddj: plaintext[12..15] is texSize and 20 + texSize is the
/// decrypted length, which is the ciphertext length minus a 0..15 byte pad.
bool SolveDdjTail(const aes::Key& k, const std::vector<Group>& groups, u8 iv[16]) {
    const Group* ddj = nullptr;
    for (const Group& g : groups) if (g.sig8 == "JMXVDDJ ") { ddj = &g; break; }
    if (!ddj) return false;

    // Vote rather than intersect: a file whose plaintext length happened to be a
    // block multiple carries no pad, and its real last byte can imitate one. One
    // such file must not veto the answer, so the tail every other sample agrees
    // on wins.
    std::vector<std::pair<u32, int>> votes;

    for (const AesSample* s : ddj->items) {
        const size_t n = s->data.size();
        if (n < 32) continue;

        // Last block, IV-free: P[last] = D_k(C[last]) XOR C[last-1].
        u8 tail[16];
        aes::DecryptBlock(k, s->data.data() + n - 16, tail);
        for (int i = 0; i < 16; ++i) tail[i] ^= s->data[n - 32 + static_cast<size_t>(i)];

        u32 pad = 0;
        const u8 v = tail[15];
        if (v >= 1 && v <= 15) {
            bool all = true;
            for (int i = 16 - static_cast<int>(v); i < 16; ++i) if (tail[i] != v) { all = false; break; }
            if (all) pad = v;
        }
        if (n < pad + 20) continue;

        u8 head[16];
        aes::DecryptBlock(k, s->data.data(), head);
        const u32 texSize = static_cast<u32>(n - pad - 20);
        const u32 cand    = ReadU32LE(head + 12) ^ texSize;

        bool seen = false;
        for (std::pair<u32, int>& p : votes) if (p.first == cand) { ++p.second; seen = true; break; }
        if (!seen) votes.push_back({ cand, 1 });
    }

    int best = 0, bestCount = 0, runnerUp = 0;
    for (const std::pair<u32, int>& p : votes) {
        if (p.second > bestCount) { runnerUp = bestCount; bestCount = p.second; best = static_cast<int>(p.first); }
        else if (p.second > runnerUp) { runnerUp = p.second; }
    }
    if (bestCount < 2 || bestCount == runnerUp) return false;   // no clear winner

    const u32 t = static_cast<u32>(best);
    iv[12] = static_cast<u8>(t & 0xFF);
    iv[13] = static_cast<u8>((t >> 8) & 0xFF);
    iv[14] = static_cast<u8>((t >> 16) & 0xFF);
    iv[15] = static_cast<u8>((t >> 24) & 0xFF);
    return true;
}

/// Final proof: decrypt the first block of every sample and demand the exact
/// signature its extension requires, plus four digits. Nothing is armed until
/// this passes, so a near-miss never reaches the preview path.
/// COUNTS rather than demands. Requiring every sample to validate assumes the
/// whole archive uses one cipher, and a real one does not: measured on a live
/// client, a .bsk group was internally inconsistent (those files are not this
/// cipher at all) and vetoed the CORRECT key and IV. Each passing sample is an
/// 8-byte signature plus four digits - 96 bits - so a handful is already proof
/// beyond doubt, and the foreign group simply does not count towards it.
bool FullyValidates(const AesAssetSpec& spec, const std::vector<Group>& groups) {
    aes::Key k;
    if (!aes::Expand(spec.key, spec.keyLen, k)) return false;
    int good = 0;
    for (const Group& g : groups) {
        for (const AesSample* s : g.items) {
            u8 p[16];
            aes::DecryptBlock(k, s->data.data(), p);
            if (spec.hasIv) for (int i = 0; i < 16; ++i) p[i] ^= spec.iv[i];
            if (std::memcmp(p, g.sig8.data(), 8) != 0) continue;
            bool digits = true;
            for (int i = 8; i < 12; ++i) if (p[i] < '0' || p[i] > '9') { digits = false; break; }
            if (digits) ++good;
        }
    }
    return good >= 4;
}

/// Assemble and validate a spec from a proven key. Tries the cryptanalytic IV
/// first, then the harvested windows, then the two degenerate conventions.
bool BuildSpec(const u8* key, int keyLen, const std::vector<Group>& groupsIn,
               const u8 ivHead[8], const Harvest* harvest,
               AesAssetSpec& out, bool& ivFromArchive) {
    aes::Key k;
    if (!aes::Expand(key, keyLen, k)) return false;

    // Downstream reasoning uses only the groups this cipher actually owns.
    const std::vector<Group> mine = AgreeingGroups(k, groupsIn, ivHead);
    const std::vector<Group>& groups = mine.empty() ? groupsIn : mine;

    AesAssetSpec spec;
    spec.keyLen = keyLen;
    std::memcpy(spec.key, key, static_cast<size_t>(keyLen));
    spec.hasIv = true;
    std::memcpy(spec.iv, ivHead, 8);

    // 1. Straight out of the archive: IV[0..7] exact, IV[12..15] from the .ddj
    //    size relation, IV[8..11] enumerated over the surviving digit values.
    //    Accepted ONLY when exactly one combination validates - two survivors
    //    means the header cannot tell them apart and guessing would corrupt
    //    four bytes of every file's first block, silently.
    if (SolveDdjTail(k, groups, spec.iv)) {
        bool solved = false;

        // (a) A plaintext file of one of these extensions was seen in the
        //     archive, so its version string is known outright. Exact, and the
        //     common case: shards usually leave part of the client unencrypted.
        for (const Group& g : groups) {
            if (g.knownVersion.size() != 4) continue;
            u8 d[16];
            aes::DecryptBlock(k, g.items[0]->data.data(), d);
            for (int p = 8; p < 12; ++p)
                spec.iv[p] = static_cast<u8>(d[p] ^ static_cast<u8>(g.knownVersion[static_cast<size_t>(p - 8)]));
            solved = true;
            break;
        }

        // (b) Nothing plaintext to lean on: fall back to the digit constraint.
        //     Accepted ONLY when every byte has exactly one surviving value.
        //     Enumerating several and picking one that "validates" would be
        //     self-deception - validation checks the very digit property the
        //     candidates were generated from, so it cannot separate them, and a
        //     wrong pick silently corrupts four bytes of every first block.
        if (!solved) {
            std::vector<u8> cands[4];
            VersionByteCandidates(k, groups, cands);
            solved = true;
            for (int p = 8; p < 12; ++p) {
                if (cands[p - 8].size() != 1) { solved = false; break; }
                spec.iv[p] = cands[p - 8][0];
            }
        }

        if (solved && FullyValidates(spec, groups)) {
            ivFromArchive = true;
            out = spec;
            return true;
        }
    }

    // 2. A harvested window whose first 8 bytes are the IV head we derived.
    if (harvest) {
        for (const std::array<u8, 16>& v : harvest->values) {
            if (std::memcmp(v.data(), ivHead, 8) != 0) continue;
            AesAssetSpec t = spec;
            std::memcpy(t.iv, v.data(), 16);
            if (FullyValidates(t, groups)) { ivFromArchive = false; out = t; return true; }
        }
    }

    // 3. Conventions that cost nothing to rule out.
    {
        AesAssetSpec t = spec;
        std::memcpy(t.iv, key, 16);
        if (keyLen >= 16 && FullyValidates(t, groups)) { out = t; return true; }
    }
    {
        AesAssetSpec t = spec;
        std::memset(t.iv, 0, 16);
        if (FullyValidates(t, groups)) { out = t; return true; }
    }
    {
        AesAssetSpec t = spec;
        t.hasIv = false;                       // ECB
        if (FullyValidates(t, groups)) { out = t; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Whole-file mapping (a 28 MB module must not be copied to test it)
// ---------------------------------------------------------------------------
class MappedFile {
public:
    ~MappedFile() { close(); }
    bool open(const std::wstring& path, u64 maxBytes) {
        close();
        m_file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_file == INVALID_HANDLE_VALUE) { m_file = nullptr; return false; }
        LARGE_INTEGER li{};
        if (!::GetFileSizeEx(m_file, &li) || li.QuadPart <= 0) { close(); return false; }
        if (static_cast<u64>(li.QuadPart) > maxBytes) { close(); return false; }
        m_size = static_cast<size_t>(li.QuadPart);
        m_map = ::CreateFileMappingW(m_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!m_map) { close(); return false; }
        m_view = static_cast<const u8*>(::MapViewOfFile(m_map, FILE_MAP_READ, 0, 0, 0));
        if (!m_view) { close(); return false; }
        return true;
    }
    void close() {
        if (m_view) { ::UnmapViewOfFile(m_view); m_view = nullptr; }
        if (m_map)  { ::CloseHandle(m_map);  m_map = nullptr; }
        if (m_file) { ::CloseHandle(m_file); m_file = nullptr; }
        m_size = 0;
    }
    const u8* data() const { return m_view; }
    size_t    size() const { return m_size; }

private:
    HANDLE    m_file = nullptr;
    HANDLE    m_map  = nullptr;
    const u8* m_view = nullptr;
    size_t    m_size = 0;
};

/// Run the oracle over a harvest, in parallel. Returns the winning index.
long long TestHarvest(const Harvest& h, const std::vector<Group>& groups,
                      int keyLen, u8 ivHeadOut[8], std::atomic<u64>& tested,
                      const ProgressFn& progress) {
    const size_t n = h.values.size();
    if (n == 0) return -1;

    unsigned threads = std::thread::hardware_concurrency();
    if (threads == 0) threads = 4;
    threads = std::min<unsigned>(threads, 64);
    // Spawning a pool per file per key length costs far more than the work when
    // the cheap pass hands over a few thousand candidates: measured 228 live
    // threads at 16% of a 32-core machine. Below this, run inline.
    if (n < 8192) threads = 1;
    else if (n / 256 < threads) threads = std::max<unsigned>(1u, static_cast<unsigned>(n / 256));

    std::atomic<long long> hit{ -1 };
    std::atomic<size_t>    next{ 0 };
    std::mutex             headMx;
    u8                     head[8] = {};

    auto worker = [&]() {
        u8 localHead[8];
        for (;;) {
            const size_t i = next.fetch_add(256);
            if (i >= n || hit.load(std::memory_order_relaxed) >= 0) return;
            const size_t end = std::min(n, i + 256);
            for (size_t j = i; j < end; ++j) {
                aes::Key k;
                if (!aes::Expand(h.values[j].data(), keyLen, k)) continue;
                tested.fetch_add(1, std::memory_order_relaxed);
                if (!KeyPasses(k, groups, localHead)) continue;
                long long expect = -1;
                if (hit.compare_exchange_strong(expect, static_cast<long long>(j))) {
                    std::lock_guard<std::mutex> lock(headMx);
                    std::memcpy(head, localHead, 8);
                }
                return;
            }
            if (!ReportProgress(progress, i, n, nullptr)) return;
        }
    };

    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (std::thread& t : pool) t.join();

    const long long idx = hit.load();
    if (idx >= 0) std::memcpy(ivHeadOut, head, 8);
    return idx;
}

} // namespace

// ---------------------------------------------------------------------------
bool AesFindCipherInBuffer(const std::vector<AesSample>& samples,
                           const u8* buf, size_t size,
                           AesAssetSpec& out,
                           AesFindReport& report,
                           const ProgressFn& progress,
                           int depth) {
    const std::vector<Group> groups = BuildGroups(samples);
    report.sampleGroups = static_cast<int>(groups.size());
    if (groups.empty() || !buf || size < 16) return false;

    // Depth 0 tries CONTIGUOUS windows only. The strided ones exist to catch an
    // IV built one byte per instruction, and an IV is never a key candidate -
    // it is matched against the derived IV head once a key is already proven.
    // Testing them as keys was 15/16 of all the work for nothing.
    //   0 = whole printable tokens          (thousands of candidates)
    //   1 = every contiguous window           (millions)
    //   2 = strided windows + 192/256-bit keys
    //   3 = every raw 16-byte window
    Harvest h;
    if (depth == 0) HarvestDelimited(buf, size, h);
    else            HarvestWindows(buf, size, h, 1, (depth >= 2) ? 16 : 1);
    report.candidates += h.values.size();

    std::atomic<u64> tested{ 0 };
    const std::vector<int> lens = (depth >= 2) ? std::vector<int>{ 16, 32, 24 }
                                               : std::vector<int>{ 16 };
    for (int keyLen : lens) {
        u8 ivHead[8];
        const long long idx = TestHarvest(h, groups, keyLen, ivHead, tested, progress);
        report.tested += tested.exchange(0);
        if (idx < 0) continue;

        // A key is proven. NOW collect the strided windows, so the IV can be
        // found even when it is not a contiguous string anywhere.
        Harvest ivPool;
        if (depth < 2) {
            HarvestWindows(buf, size, ivPool, 2, 16);
            for (size_t q = 0; q < h.values.size(); ++q) {
                ivPool.values.push_back(h.values[q]);
                ivPool.offsets.push_back(h.offsets[q]);
                ivPool.strides.push_back(h.strides[q]);
            }
        }
        const Harvest& pool = (depth < 2) ? ivPool : h;

        bool ivFromArchive = false;
        if (BuildSpec(h.values[static_cast<size_t>(idx)].data(), keyLen, groups,
                      ivHead, &pool, out, ivFromArchive)) {
            report.found         = true;
            report.sourceOffset  = h.offsets[static_cast<size_t>(idx)];
            report.ivFromArchive = ivFromArchive;
            report.howTr = FormatW(L"adim %d, ofset 0x%llx",
                                   h.strides[static_cast<size_t>(idx)],
                                   static_cast<unsigned long long>(report.sourceOffset));
            return true;
        }
    }

    // Nothing printable worked: fall back to every raw window. Only worth it on
    // a module small enough to sweep, and only for AES-128 - a raw 24/32-byte
    // key with no printable form has never been seen on a Silkroad shard and
    // tripling the sweep for it is not honest work.
    if (depth >= 3 && size <= 64ull * 1024 * 1024) {
        Harvest raw;
        HarvestRaw(buf, size, 1, raw);
        report.candidates += raw.values.size();
        u8 ivHead[8];
        const long long idx = TestHarvest(raw, groups, 16, ivHead, tested, progress);
        report.tested += tested.exchange(0);
        if (idx >= 0) {
            bool ivFromArchive = false;
            if (BuildSpec(raw.values[static_cast<size_t>(idx)].data(), 16, groups,
                          ivHead, &raw, out, ivFromArchive)) {
                report.found         = true;
                report.sourceOffset  = raw.offsets[static_cast<size_t>(idx)];
                report.ivFromArchive = ivFromArchive;
                report.howTr = FormatW(L"ham pencere, ofset 0x%llx",
                                       static_cast<unsigned long long>(report.sourceOffset));
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
bool AesFindCipher(const std::vector<AesSample>& samples,
                   const std::vector<std::wstring>& scanFiles,
                   AesAssetSpec& out,
                   AesFindReport& report,
                   const ProgressFn& progress,
                   const LogFn& log) {
    report = AesFindReport();
    const std::vector<Group> groups = BuildGroups(samples);
    report.sampleGroups = static_cast<int>(groups.size());
    if (groups.empty()) {
        ReportLog(log, T(L"Şifreli varlık örneği yok; anahtar araması yapılmadı."));
        return false;
    }

    // Cheapest pass over EVERY file before any expensive pass over one of them.
    // The key is a plain string in almost every client, so depth 0 normally ends
    // the search in the time it takes to read the modules.
    for (int depth = 0; depth <= 3; ++depth) {
        u64 done = 0;
        for (const std::wstring& path : scanFiles) {
            MappedFile mf;
            if (!mf.open(path, 512ull * 1024 * 1024)) continue;
            if (AesFindCipherInBuffer(samples, mf.data(), mf.size(), out, report,
                                      progress, depth)) {
                report.sourceFile = path;
                return true;
            }
            if (!ReportProgress(progress, ++done, scanFiles.size(),
                                L"İstemci ikilileri taranıyor..."))
                return false;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
std::wstring AesDiagnoseKey(const std::vector<AesSample>& samples,
                            const u8* key, int keyLen) {
    std::wstring out;
    aes::Key k;
    if (!aes::Expand(key, keyLen, k)) return L"key genisletilemedi";

    const std::vector<Group> groups = BuildGroups(samples);
    out += FormatW(L"grup sayisi: %zu (ornek %zu)\n", groups.size(), samples.size());
    for (const Group& g : groups) {
        u8 first[16];
        aes::DecryptBlock(k, g.items[0]->data.data(), first);
        const size_t n = std::min<size_t>(g.items.size(), 4);
        bool agree = true;
        for (size_t i = 1; i < n; ++i) {
            u8 d[16];
            aes::DecryptBlock(k, g.items[i]->data.data(), d);
            if (std::memcmp(d, first, 12) != 0) { agree = false; break; }
        }
        u8 head[8];
        for (int i = 0; i < 8; ++i)
            head[i] = static_cast<u8>(first[i] ^ static_cast<u8>(g.sig8[static_cast<size_t>(i)]));
        out += FormatW(L"  %S  ornek=%zu  12-bayt-uyum=%s  ivHead=%s\n",
                       g.sig8.c_str(), g.items.size(), agree ? L"EVET" : L"hayir",
                       BytesToHexW(head, 8).c_str());
    }

    u8 ivHead[8];
    if (!KeyPasses(k, groups, ivHead)) { out += L"KeyPasses: RED\n"; return out; }
    out += L"KeyPasses: GECTI, ivHead=" + BytesToHexW(ivHead, 8) + L"\n";

    const std::vector<Group> mine = AgreeingGroups(k, groups, ivHead);
    out += FormatW(L"uyumlu grup: %zu\n", mine.size());

    u8 iv[16] = {};
    std::memcpy(iv, ivHead, 8);
    out += FormatW(L"SolveDdjTail: %s\n",
                   SolveDdjTail(k, mine.empty() ? groups : mine, iv) ? L"GECTI" : L"BASARISIZ");

    std::vector<u8> cands[4];
    VersionByteCandidates(k, mine.empty() ? groups : mine, cands);
    out += FormatW(L"surum adaylari: %zu/%zu/%zu/%zu\n",
                   cands[0].size(), cands[1].size(), cands[2].size(), cands[3].size());
    return out;
}

// ---------------------------------------------------------------------------
void AesCollectSamples(IArchive& archive, std::vector<AesSample>& out,
                       size_t perGroup, size_t maxGroups) {
    if (!archive.isOpen()) return;

    // A single extension can never yield the version digits, and without a
    //  .ddj there is no length relation to pin the IV tail, so cast wide.
    struct Bucket { std::string sig; size_t taken = 0; };
    std::vector<Bucket> buckets;
    struct Ver { std::string sig; std::string version; };
    std::vector<Ver> versions;

    archive.walkAll([&](Pk2Node* n) -> bool {
        if (!n || n->isFolder()) return true;
        const std::string sig = SigForExtension(n->name);
        if (sig.empty()) return true;

        const u64 sz = n->entry.size;
        if (sz < 32 || (sz % 16) != 0 || sz > 4ull * 1024 * 1024) return true;

        Bucket* b = nullptr;
        for (Bucket& c : buckets) if (c.sig == sig) { b = &c; break; }
        if (!b) {
            if (buckets.size() >= maxGroups) return true;
            buckets.push_back(Bucket{ sig, 0 });
            b = &buckets.back();
        }
        if (b->taken >= perGroup) return true;

        std::vector<u8> bytes;
        if (!archive.readFile(n, bytes).ok() || bytes.size() < 32) return true;
        // Already plaintext: not a sample, but its version string is a gift -
        // it pins IV[8..11] exactly for every ENCRYPTED file of this extension.
        if (std::memcmp(bytes.data(), "JMXV", 4) == 0) {
            if (std::memcmp(bytes.data(), sig.data(), 8) == 0) {
                bool digits = true;
                for (int i = 8; i < 12; ++i)
                    if (bytes[static_cast<size_t>(i)] < '0' || bytes[static_cast<size_t>(i)] > '9') digits = false;
                if (digits) {
                    std::string ver(reinterpret_cast<const char*>(bytes.data()) + 8, 4);
                    bool have = false;
                    for (const Ver& v : versions) if (v.sig == sig) { have = true; break; }
                    if (!have) versions.push_back(Ver{ sig, ver });
                }
            }
            return true;
        }
        if ((bytes.size() % 16) != 0) return true;

        AesSample s;
        s.sig8   = sig;
        s.nameTr = n->fullPath;
        s.data.swap(bytes);
        out.push_back(std::move(s));
        ++b->taken;
        return true;
    }, nullptr);

    // A plaintext file may well turn up after the encrypted ones, so the
    // versions are stamped on at the end rather than during the walk.
    for (AesSample& s : out)
        for (const Ver& v : versions)
            if (v.sig == s.sig8) { s.knownVersion = v.version; break; }
}

// ---------------------------------------------------------------------------
void AesScanCandidates(const std::wstring& archivePath,
                       std::vector<std::wstring>& out,
                       u64 maxBytesEach) {
    struct Bin { std::wstring path; u64 size; };
    std::vector<Bin> bins;
    u64 budget = kScanBudgetBytes;

    const std::wstring dir    = PathParent(archivePath);
    const std::wstring parent = PathParent(dir);

    auto sweep = [&](const std::wstring& d) {
        if (d.empty() || !DirectoryExists(d) || budget == 0) return;
        for (const wchar_t* pat : { L"*.dll", L"*.exe", L"*.bin" }) {
            for (const std::wstring& p : ListDirectory(d, pat, true, false)) {
                if (budget == 0) return;
                // A cloud placeholder is not a local file: opening one makes
                // Windows DOWNLOAD it, which is why a sweep that wandered into
                // the Downloads tree sat for 19 minutes on 3 seconds of CPU.
                const DWORD at = ::GetFileAttributesW(p.c_str());
                if (at != INVALID_FILE_ATTRIBUTES &&
                    (at & (FILE_ATTRIBUTE_OFFLINE | 0x00400000 /*RECALL_ON_DATA_ACCESS*/ |
                           0x00040000 /*RECALL_ON_OPEN*/ | FILE_ATTRIBUTE_REPARSE_POINT)))
                    continue;
                File f;
                if (!f.openRead(p)) continue;
                const u64 sz = f.size();
                f.close();
                if (sz < 4096 || sz > maxBytesEach) continue;
                bool dup = false;
                for (const Bin& b : bins) if (IEqualsW(b.path, p)) { dup = true; break; }
                if (dup) continue;
                bins.push_back({ p, sz });
                budget = (sz >= budget) ? 0 : budget - sz;
            }
        }
    };

    // SCOPE. The archive's own folder and ONE level of subfolders under it -
    // that is where a client keeps its modules. The parent folder's own files
    // are included too (archives often live in a `data\` subfolder), but NOT
    // the parent's other subfolders: with the archive in
    // Downloads\SomeClient\, that meant sweeping every other download the user
    // ever made - measured 1154 binaries instead of 79, and a hang on the first
    // cloud-backed file it touched.
    sweep(dir);
    for (const std::wstring& sub : ListDirectory(dir, L"*", false, true)) sweep(sub);
    if (parent != dir) sweep(parent);

    // Largest first. NOT by name: the modules this was built against are named
    // randomly per build, so any name heuristic would be a coin flip that also
    // hides the answer when it loses.
    std::stable_sort(bins.begin(), bins.end(),
                     [](const Bin& a, const Bin& b) { return a.size > b.size; });
    for (const Bin& b : bins) out.push_back(b.path);
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------
void AesSetSpec(const AesAssetSpec& spec) {
    std::lock_guard<std::mutex> lock(g_specMx);
    g_spec  = spec;
    g_armed = spec.valid();
}

bool AesActive() {
    std::lock_guard<std::mutex> lock(g_specMx);
    return g_armed;
}

void AesClearSpec() {
    std::lock_guard<std::mutex> lock(g_specMx);
    ::SecureZeroMemory(g_spec.key, sizeof(g_spec.key));
    ::SecureZeroMemory(g_spec.iv,  sizeof(g_spec.iv));
    g_spec  = AesAssetSpec();
    g_armed = false;
}

std::wstring AesStatusTr() {
    return AesActive() ? T(L"varlık şifresi çözüldü") : T(L"varlık şifresi yok");
}

std::wstring AesStatusSensitive() {
    std::lock_guard<std::mutex> lock(g_specMx);
    if (!g_armed) return L"(yok)";
    std::wstring s = FormatW(L"AES-%d-%s key=", g_spec.keyLen * 8, g_spec.hasIv ? L"CBC" : L"ECB");
    s += BytesToHexW(g_spec.key, static_cast<size_t>(g_spec.keyLen));
    if (g_spec.hasIv) s += L" iv=" + BytesToHexW(g_spec.iv, 16);
    return s;
}

// ---------------------------------------------------------------------------
bool AesDecryptIfEncrypted(const std::wstring& innerPath, std::vector<u8>& buf) {
    AesAssetSpec spec;
    {
        std::lock_guard<std::mutex> lock(g_specMx);
        if (!g_armed) return false;
        spec = g_spec;
    }
    if (buf.size() < 16 || (buf.size() % 16) != 0) return false;
    if (std::memcmp(buf.data(), "JMXV", 4) == 0) return false;   // already plain

    std::vector<u8> plain(buf.size());
    const bool ok = spec.hasIv
        ? aes::CbcDecrypt(spec.key, spec.keyLen, spec.iv, buf.data(), buf.size(), plain.data())
        : aes::EcbDecrypt(spec.key, spec.keyLen, buf.data(), buf.size(), plain.data());
    if (!ok) return false;

    // Structural accept test. A file this cipher does not own comes out as noise
    // and is left completely untouched.
    if (std::memcmp(plain.data(), "JMXV", 4) != 0) return false;
    const std::string sig = SigForExtension(innerPath);
    if (!sig.empty() && std::memcmp(plain.data(), sig.data(), 8) != 0) return false;

    // Strip the pad. A length that was already a block multiple carries NO pad
    // block, so a trailing 0x10 is real data and never padding.
    const u8 last = plain.back();
    if (last >= 1 && last <= 15 && plain.size() > last) {
        bool pad = true;
        for (size_t i = plain.size() - last; i < plain.size(); ++i)
            if (plain[i] != last) { pad = false; break; }
        if (pad) plain.resize(plain.size() - last);
    }
    buf.swap(plain);
    return true;
}

// ---------------------------------------------------------------------------
bool AesTryInit(IArchive& archive, const LogFn& log) {
    if (!archive.isOpen()) return false;
    if (AesActive()) return true;              // one key opens a whole client

    std::vector<AesSample> samples;
    AesCollectSamples(archive, samples);
    if (samples.empty()) return false;         // nothing encrypted - normal

    std::vector<std::wstring> bins;
    AesScanCandidates(archive.path(), bins);
    if (bins.empty()) {
        ReportLog(log, T(L"Şifreli varlıklar var ama taranacak istemci dosyası bulunamadı."));
        return false;
    }

    ReportLog(log, T(L"Şifreli varlıklar bulundu, anahtar aranıyor..."));

    AesAssetSpec  spec;
    AesFindReport rep;
    if (!AesFindCipher(samples, bins, spec, rep, nullptr, log)) {
        ReportLog(log, T(L"Varlık şifresi çözülemedi. Uydurma bir anahtar kullanılmadı."));
        return false;
    }
    AesSetSpec(spec);
    // Contentless on purpose: neither the key nor where it came from.
    ReportLog(log, T(L"Varlık şifresi çözüldü."));
    return true;
}

} // namespace sro
