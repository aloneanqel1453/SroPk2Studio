// SkillEffect Studio - core/Pk2.cpp
#include "Pk2.h"

#include "BlowfishTables.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_set>

namespace se {

namespace {

constexpr std::uint8_t kBaseKey[10] = {0x03, 0xF8, 0xE4, 0x44, 0x88, 0x99, 0x3F, 0x64, 0xFE, 0x35};
constexpr std::uint32_t kBlockSize = 2560;
constexpr std::uint32_t kEntrySize = 128;

inline std::uint32_t Le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline void PutLe32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}
inline std::int64_t Le64(const std::uint8_t* p) {
    return static_cast<std::int64_t>(Le32(p)) | (static_cast<std::int64_t>(Le32(p + 4)) << 32);
}

// SRO Blowfish: textbook key schedule (key bytes folded big-endian), blocks read as two LE words.
class Blowfish {
public:
    void Init(const std::uint8_t* key, size_t len) {
        std::memcpy(P, sro::bftables::kInitP, sizeof(P));
        std::memcpy(S, sro::bftables::kInitS, sizeof(S));
        size_t j = 0;
        for (auto& p : P) {
            const std::uint32_t d = (static_cast<std::uint32_t>(key[j]) << 24) |
                                    (static_cast<std::uint32_t>(key[(j + 1) % len]) << 16) |
                                    (static_cast<std::uint32_t>(key[(j + 2) % len]) << 8) |
                                    static_cast<std::uint32_t>(key[(j + 3) % len]);
            p ^= d;
            j = (j + 4) % len;
        }
        std::uint32_t l = 0, r = 0;
        for (size_t i = 0; i < 18; i += 2) {
            Enc(l, r);
            P[i] = l;
            P[i + 1] = r;
        }
        for (auto& box : S) {
            for (size_t k = 0; k < 256; k += 2) {
                Enc(l, r);
                box[k] = l;
                box[k + 1] = r;
            }
        }
    }
    void EncodeBlock(std::uint8_t* b) const {
        std::uint32_t l = Le32(b), r = Le32(b + 4);
        Enc(l, r);
        PutLe32(b, l);
        PutLe32(b + 4, r);
    }
    void DecodeBlock(std::uint8_t* b) const {
        std::uint32_t l = Le32(b), r = Le32(b + 4);
        Dec(l, r);
        PutLe32(b, l);
        PutLe32(b + 4, r);
    }

private:
    std::uint32_t F(std::uint32_t x) const {
        return ((S[0][(x >> 24) & 0xFF] + S[1][(x >> 16) & 0xFF]) ^ S[2][(x >> 8) & 0xFF]) + S[3][x & 0xFF];
    }
    void Enc(std::uint32_t& xl, std::uint32_t& xr) const {
        std::uint32_t l = xl ^ P[0], r = xr;
        for (int i = 1; i <= 16; i += 2) {
            r ^= F(l) ^ P[i];
            l ^= F(r) ^ P[i + 1];
        }
        r ^= P[17];
        xl = r;
        xr = l;
    }
    void Dec(std::uint32_t& xl, std::uint32_t& xr) const {
        std::uint32_t l = xl ^ P[17], r = xr;
        for (int i = 16; i >= 1; i -= 2) {
            r ^= F(l) ^ P[i];
            l ^= F(r) ^ P[i - 1];
        }
        r ^= P[0];
        xl = r;
        xr = l;
    }
    std::uint32_t P[18]{};
    std::uint32_t S[4][256]{};
};

std::string LowerAscii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// root-block oracle (SROAssetStudio KeyRing): entry 0 of the root block = folder "." and all 20 types <= 2
bool RootOracle(const Blowfish& bf, const std::uint8_t* encRoot) {
    std::uint8_t b[8];
    std::memcpy(b, encRoot, 8);
    bf.DecodeBlock(b);
    if (b[0] != 1 || b[1] != '.' || b[2] != 0) return false;
    std::uint8_t plain[kBlockSize];
    std::memcpy(plain, encRoot, kBlockSize);
    for (std::uint32_t i = 0; i < kBlockSize; i += 8) bf.DecodeBlock(plain + i);
    for (std::uint32_t k = 0; k < 20; ++k)
        if (plain[k * kEntrySize] > 2) return false;
    return true;
}

constexpr std::uint8_t kBaseParadise[10] = {0xA8, 0x19, 0x72, 0xBF, 0x87, 0xD2, 0x0A, 0xFD, 0x37, 0x54};

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool ParseHex(const std::string& h, std::vector<std::uint8_t>& out) {
    out.clear();
    std::string t;
    for (char c : h)
        if (c != ' ' && c != ':' && c != '-') t += c;
    if (t.empty() || t.size() % 2) return false;
    for (size_t i = 0; i < t.size(); i += 2) {
        const int a = HexVal(t[i]), b = HexVal(t[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(static_cast<std::uint8_t>(a * 16 + b));
    }
    return true;
}

}  // namespace

struct Pk2Reader::Impl {
    HANDLE h = INVALID_HANDLE_VALUE;
    Blowfish bf;
    bool encrypted = true;
};

Pk2Reader::Pk2Reader() : m_impl(std::make_unique<Impl>()) {}

Pk2Reader::~Pk2Reader() {
    if (m_impl->h != INVALID_HANDLE_VALUE) CloseHandle(m_impl->h);
}

bool Pk2Reader::ReadAt(std::int64_t off, void* dst, std::uint32_t n) {
    LARGE_INTEGER li{};
    li.QuadPart = off;
    if (!SetFilePointerEx(m_impl->h, li, nullptr, FILE_BEGIN)) return false;
    auto* p = static_cast<std::uint8_t*>(dst);
    std::uint32_t done = 0;
    while (done < n) {
        DWORD got = 0;
        if (!::ReadFile(m_impl->h, p + done, n - done, &got, nullptr) || got == 0) return false;
        done += got;
    }
    return true;
}

bool Pk2Reader::Open(const std::wstring& path, const std::string& asciiKey, std::wstring& err) {
    m_path = path;
    m_dirs.clear();
    if (m_impl->h != INVALID_HANDLE_VALUE) CloseHandle(m_impl->h);
    m_impl->h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_impl->h == INVALID_HANDLE_VALUE) {
        err = L"PK2 açılamadı / cannot open: " + path;
        return false;
    }
    std::uint8_t hdr[256];
    if (!ReadAt(0, hdr, 256) || std::memcmp(hdr, "JoyMax File Manager!", 20) != 0) {
        err = L"JMXPACK imzası yok / not a PK2: " + path;
        return false;
    }
    m_impl->encrypted = hdr[34] != 0;
    std::vector<std::uint8_t> key;
    if (!Pk2KeyBytes(asciiKey, key)) {
        err = L"Blowfish anahtarı geçersiz / invalid key";
        return false;
    }
    m_impl->bf.Init(key.data(), key.size());
    if (m_impl->encrypted) {
        std::uint8_t chk[16];
        std::memcpy(chk, "Joymax Pack File", 16);
        m_impl->bf.EncodeBlock(chk);
        m_impl->bf.EncodeBlock(chk + 8);
        bool okKey = std::memcmp(chk, hdr + 35, 3) == 0;
        if (!okKey) {  // header checksum can be blank / custom: the root block decides
            std::uint8_t blk[kBlockSize];
            okKey = ReadAt(256, blk, kBlockSize) && RootOracle(m_impl->bf, blk);
        }
        if (!okKey) {
            err = L"PK2 anahtarı uymuyor / Blowfish key does not match this archive";
            return false;
        }
    }
    return true;
}

bool Pk2Reader::Block(std::int64_t off, std::vector<Entry>& out, std::wstring& err) {
    out.clear();
    auto it = m_dirs.find(off);
    if (it != m_dirs.end()) {
        out = it->second;
        return true;
    }
    std::int64_t cur = off;
    std::uint8_t blk[kBlockSize];
    for (int guard = 0; cur != 0 && guard < 100000; ++guard) {
        if (!ReadAt(cur, blk, kBlockSize)) {
            err = L"PK2 blok okunamadı / block read failed";
            return false;
        }
        if (m_impl->encrypted)
            for (std::uint32_t i = 0; i < kBlockSize; i += 8) m_impl->bf.DecodeBlock(blk + i);
        std::int64_t next = 0;
        for (std::uint32_t k = 0; k < 20; ++k) {
            const std::uint8_t* e = blk + k * kEntrySize;
            if (e[0] == 1 || e[0] == 2) {
                Entry en;
                en.type = e[0];
                size_t n = 0;
                while (n < 89 && e[1 + n]) ++n;
                en.name = LowerAscii(std::string(reinterpret_cast<const char*>(e + 1), n));
                en.off = Le64(e + 106);
                en.size = Le32(e + 114);
                out.push_back(std::move(en));
            }
            if (k == 19) next = Le64(e + 118);
        }
        cur = next;
    }
    m_dirs[off] = out;
    return true;
}

bool Pk2Reader::Find(const std::wstring& innerPath, Entry& found, std::wstring& err) {
    std::string narrow;
    {
        size_t bad = 0;
        if (!NarrowExact(949, innerPath, narrow, &bad)) {  // v5 M3: a best-fit name could open ANOTHER file
            err = L"ENC2 pk2: " + EncLossText(innerPath, bad, false) + L" / " + EncLossText(innerPath, bad, true);
            return false;
        }
    }
    narrow = LowerAscii(narrow);
    std::vector<std::string> parts;
    size_t a = 0;
    while (a <= narrow.size()) {
        size_t b = narrow.find_first_of("\\/", a);
        if (b == std::string::npos) b = narrow.size();
        if (b > a) parts.push_back(narrow.substr(a, b - a));
        a = b + 1;
    }
    std::int64_t dir = 256;
    std::vector<Entry> entries;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!Block(dir, entries, err)) return false;
        bool hit = false;
        for (const auto& e : entries) {
            if (e.name != parts[i]) continue;
            const bool last = i + 1 == parts.size();
            if (last && e.type == 2) {
                found = e;
                return true;
            }
            if (!last && e.type == 1) {
                dir = e.off;
                hit = true;
                break;
            }
        }
        if (!hit) break;
    }
    err = L"PK2 içinde yok / not in PK2: " + innerPath;
    return false;
}

bool Pk2Reader::Exists(const std::wstring& innerPath) {
    Entry e;
    std::wstring err;
    return Find(innerPath, e, err);
}

bool Pk2Reader::Read(const std::wstring& innerPath, Bytes& out, std::wstring& err) {
    Entry e;
    if (!Find(innerPath, e, err)) return false;
    out.resize(e.size);
    if (e.size && !ReadAt(e.off, out.data(), e.size)) {
        err = L"PK2 veri okunamadı / data read failed: " + innerPath;
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ writer (copy only)
namespace {

// The archive the copy writer works on. dry (v5 M4 plan): the source is opened read-only; written directory entries /
// blocks go to an overlay that later reads see, data writes only move the virtual end.
struct RawFile {
    HANDLE h = INVALID_HANDLE_VALUE;
    bool dry = false;
    std::int64_t realSize = 0, vsize = 0;
    std::unordered_map<std::int64_t, std::array<std::uint8_t, kEntrySize>> ov;
    RawFile() = default;
    RawFile(const RawFile&) = delete;
    RawFile& operator=(const RawFile&) = delete;
    ~RawFile() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    bool Real(std::int64_t off, void* p, std::uint32_t n, bool write) {
        LARGE_INTEGER li{};
        li.QuadPart = off;
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
        DWORD done = 0;
        return write ? (WriteFile(h, p, n, &done, nullptr) && done == n) : (::ReadFile(h, p, n, &done, nullptr) && done == n);
    }
    // header / directory entries / directory blocks
    bool At(std::int64_t off, void* p, std::uint32_t n, bool write) {
        if (!dry) return Real(off, p, n, write);
        auto* b = static_cast<std::uint8_t*>(p);
        if (write) {
            for (std::uint32_t q = 0; q + kEntrySize <= n; q += kEntrySize) std::memcpy(ov[off + q].data(), b + q, kEntrySize);
            vsize = std::max<std::int64_t>(vsize, off + n);
            return true;
        }
        if (n == kEntrySize) {
            auto it = ov.find(off);
            if (it != ov.end()) {
                std::memcpy(b, it->second.data(), kEntrySize);
                return true;
            }
        }
        if (off >= realSize) {
            std::memset(b, 0, n);
            return off + n <= vsize;
        }
        return Real(off, p, n, false);
    }
    // file data (never read back by the writer)
    bool Data(std::int64_t off, const std::uint8_t* p, std::uint32_t n) {
        if (dry) {
            vsize = std::max<std::int64_t>(vsize, off + n);
            return true;
        }
        return n == 0 || Real(off, const_cast<std::uint8_t*>(p), n, true);
    }
    std::int64_t Size() {
        if (dry) return vsize;
        LARGE_INTEGER li{};
        GetFileSizeEx(h, &li);
        return li.QuadPart;
    }
};

// v5 M3: exact CP949 or nothing (every path is checked first: PreCheck / Pk2Reader::List)
std::string Narrow949(const std::wstring& w) {
    std::string s;
    NarrowExact(949, w, s);
    return s;
}

bool SameFile(const std::wstring& a, const std::wstring& b) {
    HANDLE ha = CreateFileW(a.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE hb = CreateFileW(b.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    bool same = false;
    if (ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION ia{}, ib{};
        if (GetFileInformationByHandle(ha, &ia) && GetFileInformationByHandle(hb, &ib))
            same = ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber && ia.nFileIndexHigh == ib.nFileIndexHigh &&
                   ia.nFileIndexLow == ib.nFileIndexLow;
    }
    if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
    return same;
}

struct CopyCtx {
    const std::function<void(int)>* progress;
};

DWORD CALLBACK CopyProgress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE,
                            LPVOID ctx) {
    auto* c = static_cast<CopyCtx*>(ctx);
    if (c && c->progress && *c->progress && total.QuadPart > 0)
        (*c->progress)(static_cast<int>(done.QuadPart * 80 / total.QuadPart));
    return PROGRESS_CONTINUE;
}

void Put64(std::uint8_t* p, std::uint64_t v) {
    PutLe32(p, static_cast<std::uint32_t>(v));
    PutLe32(p + 4, static_cast<std::uint32_t>(v >> 32));
}

std::string EntryName(const std::uint8_t* e) {
    size_t n = 0;
    while (n < 89 && e[1 + n]) ++n;
    return LowerAscii(std::string(reinterpret_cast<const char*>(e + 1), n));
}

std::wstring Num(std::int64_t v) {  // 2 147 483 648
    std::wstring s = std::to_wstring(v < 0 ? -v : v), o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && (s.size() - i) % 3 == 0) o += L' ';
        o += s[i];
    }
    return v < 0 ? L"-" + o : o;
}

// v5 M3: every inner path must be exact CP949 and no data may hold a noted lossy table row (nothing is copied otherwise)
bool PreCheck(const std::vector<Pk2PutFile>& files, std::wstring& err) {
    for (const auto& pf : files) {
        size_t bad = 0;
        if (!IsExactIn(949, pf.inner, &bad)) {
            err = L"ENC2 pk2 yolu / path: " + EncLossText(pf.inner, bad, false) + L" / " + EncLossText(pf.inner, bad, true);
            return false;
        }
        std::wstring tr, en;
        if (PendingEncLosses() && FindEncLossIn(pf.data, &tr, &en) >= 0) {
            err = tr + L" / " + en + L"  (" + pf.inner + L")";
            return false;
        }
    }
    return true;
}

// the directory work shared by Pk2WriteCopy (real copy) and Pk2PlanCopy (dry)
bool WriteFiles(RawFile& f, const std::vector<std::uint8_t>& keyBytes, const std::vector<Pk2PutFile>& files,
                const Pk2CopyOptions& opt, Pk2CopyReport& rep, const std::function<void(int)>& progress, std::wstring& log,
                std::wstring& err) {
    std::uint8_t hdr[256] = {};
    Blowfish bf;
    if (!f.At(0, hdr, 256, false)) return false;
    const bool enc = hdr[34] != 0;
    bf.Init(keyBytes.data(), keyBytes.size());
    auto readEntry = [&](std::int64_t off, std::uint8_t* e) {
        if (!f.At(off, e, kEntrySize, false)) return false;
        if (enc)
            for (std::uint32_t i = 0; i < kEntrySize; i += 8) bf.DecodeBlock(e + i);
        return true;
    };
    auto writeEntry = [&](std::int64_t off, const std::uint8_t* e) {
        std::uint8_t c[kEntrySize];
        std::memcpy(c, e, kEntrySize);
        if (enc)
            for (std::uint32_t i = 0; i < kEntrySize; i += 8) bf.EncodeBlock(c + i);
        return f.At(off, c, kEntrySize, true);
    };
    auto note = [&](std::int64_t off, std::int64_t n) { rep.maxEnd = std::max(rep.maxEnd, off + n); };
    // end of the archive; the test hook pushes appended bytes up to opt.testAppendFloor (sparse copy)
    auto appendPos = [&]() { return std::max<std::int64_t>(f.Size(), opt.testAppendFloor); };
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const std::uint64_t ft64 = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    // every file extent of the archive (built only when a file may go back into its old slot)
    struct Ext {
        std::int64_t entryOff, off;
        std::uint32_t size;
    };
    std::vector<Ext> exts;
    bool extsBuilt = false, extsOk = false;
    auto buildExts = [&]() -> bool {
        if (extsBuilt) return extsOk;
        extsBuilt = true;
        std::vector<std::int64_t> todo{256};
        std::unordered_set<std::int64_t> seen;
        const std::int64_t lim = f.Size();
        while (!todo.empty()) {
            std::int64_t blk = todo.back();
            todo.pop_back();
            while (blk > 0 && blk + kBlockSize <= lim && seen.insert(blk).second) {
                std::int64_t next = 0;
                for (std::uint32_t k = 0; k < 20; ++k) {
                    std::uint8_t e[kEntrySize];
                    if (!readEntry(blk + k * kEntrySize, e)) return false;
                    if (e[0] == 2) {
                        exts.push_back({blk + k * kEntrySize, Le64(e + 106), Le32(e + 114)});
                    } else if (e[0] == 1) {
                        const std::string nm = EntryName(e);
                        if (nm != "." && nm != "..") todo.push_back(Le64(e + 106));
                    }
                    if (k == 19) next = Le64(e + 118);
                }
                blk = next;
            }
        }
        extsOk = true;
        return true;
    };
    auto setExt = [&](std::int64_t entryOff, std::int64_t off, std::uint32_t size) {
        if (!extsBuilt) return;
        for (auto& x : exts)
            if (x.entryOff == entryOff) {
                x.off = off;
                x.size = size;
                return;
            }
        exts.push_back({entryOff, off, size});
    };
    auto sharedSlot = [&](std::int64_t entryOff, std::int64_t off, std::uint32_t size) {
        for (const auto& x : exts)
            if (x.entryOff != entryOff && x.size && x.off < off + size && x.off + x.size > off) return true;
        return false;
    };
    // a free entry slot in the directory chain starting at first (a new chained block when it is full)
    auto freeSlot = [&](std::int64_t first, std::int64_t& slot) -> bool {
        slot = -1;
        std::int64_t lastBlk = 0;
        for (std::int64_t blk = first; blk != 0;) {
            std::int64_t next = 0;
            lastBlk = blk;
            for (std::uint32_t k = 0; k < 20; ++k) {
                std::uint8_t e[kEntrySize];
                if (!readEntry(blk + k * kEntrySize, e)) return false;
                if (e[0] == 0 && slot < 0) slot = blk + k * kEntrySize;
                if (k == 19) next = Le64(e + 118);
            }
            if (slot >= 0) return true;
            blk = next;
        }
        const std::int64_t nb = appendPos();  // chain a new directory block (20 empty entries) at the end
        std::uint8_t blk[kBlockSize] = {};
        if (enc)
            for (std::uint32_t i = 0; i < kBlockSize; i += 8) bf.EncodeBlock(blk + i);
        std::uint8_t last[kEntrySize];
        const std::int64_t lastOff = lastBlk + 19 * kEntrySize;
        if (!f.At(nb, blk, kBlockSize, true) || !readEntry(lastOff, last)) return false;
        note(nb, kBlockSize);
        ++rep.newBlocks;
        Put64(last + 118, static_cast<std::uint64_t>(nb));
        if (!writeEntry(lastOff, last)) return false;
        slot = nb;
        return true;
    };
    auto putEntry = [&](std::int64_t slot, std::uint8_t type, const std::string& cased, std::int64_t off, std::uint32_t size) {
        std::uint8_t e[kEntrySize];
        if (!readEntry(slot, e)) return false;
        const std::uint64_t keepNext = static_cast<std::uint64_t>(Le64(e + 118));
        std::memset(e, 0, kEntrySize);
        e[0] = type;
        std::memcpy(e + 1, cased.c_str(), std::min<size_t>(cased.size(), 88));
        Put64(e + 90, ft64);
        Put64(e + 98, ft64);
        Put64(e + 106, static_cast<std::uint64_t>(off));
        PutLe32(e + 114, size);
        Put64(e + 118, keepNext);
        return writeEntry(slot, e);
    };
    bool ok = true;
    size_t idx = 0;
    for (const auto& pf : files) {
        if (!ok) break;
        if (pf.data.size() > 0xFFFFFFFFull) {
            err = L"Dosya 4 GiB'tan büyük / file larger than 4 GiB: " + pf.inner;
            ok = false;
            break;
        }
        const std::uint32_t size = static_cast<std::uint32_t>(pf.data.size());
        const std::string cased949 = Narrow949(pf.inner);
        const std::string narrow = LowerAscii(cased949);
        std::vector<std::string> parts, casedParts;
        size_t a = 0;
        while (a <= narrow.size()) {
            size_t b = narrow.find_first_of("\\/", a);
            if (b == std::string::npos) b = narrow.size();
            if (b > a) {
                parts.push_back(narrow.substr(a, b - a));
                casedParts.push_back(cased949.substr(a, b - a));
            }
            a = b + 1;
        }
        if (parts.empty()) continue;
        std::int64_t dir = 256;
        for (size_t i = 0; i + 1 < parts.size() && ok; ++i) {  // walk the folders (a missing one is created)
            bool hit = false;
            for (std::int64_t blk = dir; blk != 0 && !hit && ok;) {
                std::int64_t next = 0;
                for (std::uint32_t k = 0; k < 20; ++k) {
                    std::uint8_t e[kEntrySize];
                    if (!readEntry(blk + k * kEntrySize, e)) {
                        ok = false;
                        break;
                    }
                    if (e[0] == 1 && EntryName(e) == parts[i]) {
                        dir = Le64(e + 106);
                        hit = true;
                        break;
                    }
                    if (k == 19) next = Le64(e + 118);
                }
                if (!hit) blk = next;
            }
            if (!ok) break;
            if (hit) continue;
            if (parts[i].size() > 88) {
                err = L"Klasör adı çok uzun / folder name too long: " + pf.inner;
                ok = false;
                break;
            }
            // new folder: a directory block with "." and ".." + a folder entry in the parent
            std::int64_t slot = -1;
            if (!freeSlot(dir, slot)) {
                ok = false;
                break;
            }
            const std::int64_t nb = appendPos();
            std::uint8_t blk[kBlockSize] = {};
            auto self = [&](std::uint8_t* e, const char* nm, std::int64_t off) {
                e[0] = 1;
                std::memcpy(e + 1, nm, std::strlen(nm));
                Put64(e + 90, ft64);
                Put64(e + 98, ft64);
                Put64(e + 106, static_cast<std::uint64_t>(off));
            };
            self(blk, ".", nb);
            self(blk + kEntrySize, "..", dir);
            if (enc)
                for (std::uint32_t q = 0; q < kBlockSize; q += 8) bf.EncodeBlock(blk + q);
            ok = f.At(nb, blk, kBlockSize, true) && putEntry(slot, 1, casedParts[i], nb, 0);
            if (!ok) break;
            note(nb, kBlockSize);
            ++rep.newBlocks;
            log += L"  pk2: " + FromAnsi(casedParts[i], 949) + L"\\  (yeni klasör / new folder)\r\n";
            dir = nb;
        }
        if (!ok) break;
        const std::string& name = parts.back();
        if (name.size() > 88) {
            err = L"Dosya adı çok uzun / name too long: " + pf.inner;
            ok = false;
            break;
        }
        std::int64_t entryOff = -1;
        for (std::int64_t blk = dir; blk != 0 && ok;) {
            std::int64_t next = 0;
            for (std::uint32_t k = 0; k < 20; ++k) {
                std::uint8_t e[kEntrySize];
                if (!readEntry(blk + k * kEntrySize, e)) {
                    ok = false;
                    break;
                }
                if (e[0] == 2 && EntryName(e) == name) entryOff = blk + k * kEntrySize;
                if (k == 19) next = Le64(e + 118);
            }
            if (entryOff >= 0) break;
            blk = next;
        }
        if (!ok) break;
        // v5 M4: a changed file that fits its old slot may go back there (kPk2PreferInPlace, or kPk2InPlaceOver2G when
        // appending it would end past 2 GiB); never when another entry shares that slot
        std::int64_t dataOff = appendPos();
        bool inPlace = false;
        if (entryOff >= 0 && (opt.flags & (kPk2PreferInPlace | kPk2InPlaceOver2G))) {
            std::uint8_t e[kEntrySize];
            if (!readEntry(entryOff, e)) {
                ok = false;
                break;
            }
            const std::int64_t oldOff = Le64(e + 106);
            const std::uint32_t oldSize = Le32(e + 114);
            const bool fits = oldOff >= 256 && size <= oldSize && oldOff + oldSize <= rep.srcSize;
            const bool want = (opt.flags & kPk2PreferInPlace) || dataOff + size > kPk2OffsetLimit;
            if (fits && want) {
                if (buildExts() && !sharedSlot(entryOff, oldOff, oldSize)) {
                    inPlace = true;
                    dataOff = oldOff;
                } else {
                    log += L"  pk2: " + pf.inner + L"  (eski yuva paylaşılıyor, sona eklendi / old slot is shared, appended)\r\n";
                }
            }
        }
        if (!f.Data(dataOff, pf.data.data(), size)) {
            err = L"pk2 kopyasına yazılamadı / write failed";
            ok = false;
            break;
        }
        note(dataOff, size);
        if (dataOff + size > kPk2OffsetLimit) rep.over2G.push_back(pf.inner);
        if (dataOff >= rep.maxDataOff) {
            rep.maxDataOff = dataOff;
            rep.maxInner = pf.inner;
        }
        if (entryOff >= 0) {  // replace: keep name (original case) and ctime, repoint the data
            std::uint8_t e[kEntrySize];
            ok = readEntry(entryOff, e);
            Put64(e + 98, ft64);
            Put64(e + 106, static_cast<std::uint64_t>(dataOff));
            PutLe32(e + 114, size);
            ok = ok && writeEntry(entryOff, e);
            setExt(entryOff, dataOff, size);
            if (inPlace) {
                ++rep.inPlace;
                log += L"  pk2: " + pf.inner + L"  (değişti, eski yerine / replaced in its old slot)\r\n";
            } else {
                ++rep.appended;
                log += L"  pk2: " + pf.inner + L"  (değişti / replaced)\r\n";
            }
        } else {
            std::int64_t slot = -1;
            ok = freeSlot(dir, slot) && putEntry(slot, 2, Narrow949(FileNameOf(pf.inner)), dataOff, size);
            setExt(slot, dataOff, size);
            ++rep.newFiles;
            ++rep.appended;
            log += L"  pk2: " + pf.inner + L"  (yeni / new)\r\n";
        }
        ++idx;
        if (progress) progress(80 + static_cast<int>(idx * 15 / std::max<size_t>(1, files.size())));
    }
    rep.dstSize = f.Size();
    return ok;
}

// largest offset line + the P3 size note (v5 M4; v6 P1: plain information - the vSRO client reads pk2 files of any
// size, files past 2 GiB work (user-confirmed), so P3 never blocks and never asks)
void FinishReport(Pk2CopyReport& rep, std::wstring& log) {
    rep.p3 = rep.maxEnd > kPk2OffsetLimit;
    rep.p3Tr.clear();
    rep.p3En.clear();
    if (rep.maxEnd <= 0) return;
    log += L"  pk2: en büyük yazılan uç / largest written end: " + Num(rep.maxEnd) + L" (veri ofseti / data offset " +
           Num(rep.maxDataOff) + L")" + Fmt(L"; yerinde / in place %d, sona / appended %d\r\n", rep.inPlace, rep.appended);
    if (!rep.p3) return;
    rep.p3Tr = L"Bilgi: pk2 kopyası 2 GB'tan büyük (" + Num(rep.dstSize > rep.maxEnd ? rep.dstSize : rep.maxEnd) +
               Fmt(L" bayt; %zu dosya 2 GB'tan sonra yazıldı). vSRO istemcisi her boyuttaki pk2'yi okur: bir şey yapmanız gerekmez.",
                   rep.over2G.size());
    rep.p3En = L"Info: the pk2 copy is larger than 2 GB (" + Num(rep.dstSize > rep.maxEnd ? rep.dstSize : rep.maxEnd) +
               Fmt(L" bytes; %zu files written after 2 GB). The vSRO client reads pk2 files of any size: nothing to do.",
                   rep.over2G.size());
    log += L"  P3: " + rep.p3Tr + L"\r\n  P3: " + rep.p3En + L"\r\n";
}

bool CheckSrcKey(const std::wstring& src, const std::string& asciiKey, std::vector<std::uint8_t>& keyBytes, std::wstring& err) {
    if (!FileExists(src)) {
        err = L"Kaynak pk2 yok / source pk2 missing: " + src;
        return false;
    }
    if (!Pk2KeyBytes(asciiKey, keyBytes)) {
        err = L"Blowfish anahtarı geçersiz / invalid key";
        return false;
    }
    Pk2Reader r;  // key check on the source (read-only)
    return r.Open(src, asciiKey, err);
}

}  // namespace

bool Pk2WriteCopy(const std::wstring& src, const std::wstring& dst, const std::string& asciiKey,
                  const std::vector<Pk2PutFile>& files, const std::function<void(int)>& progress, std::wstring& log,
                  std::wstring& err, bool backupExisting) {
    Pk2CopyOptions o;
    o.backupExisting = backupExisting;
    return Pk2WriteCopy(src, dst, asciiKey, files, progress, log, err, o, nullptr);
}

bool Pk2PlanCopy(const std::wstring& src, const std::string& asciiKey, const std::vector<Pk2PutFile>& files,
                 const Pk2CopyOptions& opt, Pk2CopyReport& rep, std::wstring& log, std::wstring& err) {
    rep = Pk2CopyReport{};
    std::vector<std::uint8_t> keyBytes;
    if (!CheckSrcKey(src, asciiKey, keyBytes, err) || !PreCheck(files, err)) return false;
    RawFile f;
    f.dry = true;
    f.h = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f.h == INVALID_HANDLE_VALUE) {
        err = L"pk2 açılamadı / cannot open: " + src;
        return false;
    }
    LARGE_INTEGER li{};
    GetFileSizeEx(f.h, &li);
    f.realSize = f.vsize = rep.srcSize = li.QuadPart;
    if (!WriteFiles(f, keyBytes, files, opt, rep, {}, log, err)) {
        if (err.empty()) err = L"pk2 planı çıkarılamadı / pk2 plan failed";
        return false;
    }
    FinishReport(rep, log);
    return true;
}

bool Pk2WriteCopy(const std::wstring& src, const std::wstring& dst, const std::string& asciiKey,
                  const std::vector<Pk2PutFile>& files, const std::function<void(int)>& progress, std::wstring& log,
                  std::wstring& err, const Pk2CopyOptions& opt, Pk2CopyReport* repOut) {
    Pk2CopyReport rep;
    if (!FileExists(src)) {
        err = L"Kaynak pk2 yok / source pk2 missing: " + src;
        return false;
    }
    if (_wcsicmp(src.c_str(), dst.c_str()) == 0 || (FileExists(dst) && SameFile(src, dst))) {
        err = L"Hedef, kaynak pk2'nin kendisi: orijinale asla yazılmaz / target is the source pk2: never written in place";
        return false;
    }
    std::vector<std::uint8_t> keyBytes;
    if (!CheckSrcKey(src, asciiKey, keyBytes, err) || !PreCheck(files, err)) return false;
    const std::wstring tmp = dst + L".tmp~" + std::to_wstring(GetCurrentProcessId());
    CopyCtx cc{&progress};
    if (!CopyFileExW(src.c_str(), tmp.c_str(), CopyProgress, &cc, nullptr, 0)) {
        err = L"pk2 kopyalanamadı / copy failed: " + tmp;
        DeleteFileW(tmp.c_str());
        return false;
    }
    SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
    bool ok = true;
    {
        RawFile f;
        f.h = CreateFileW(tmp.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f.h == INVALID_HANDLE_VALUE) {
            err = L"pk2 kopyası açılamadı / cannot open the copy: " + tmp;
            ok = false;
        }
        if (ok) {
            rep.srcSize = f.Size();
            if (opt.testAppendFloor > 0) {  // test hook: keep the gap below the pushed data sparse
                DWORD br = 0;
                DeviceIoControl(f.h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &br, nullptr);
            }
            ok = WriteFiles(f, keyBytes, files, opt, rep, progress, log, err);
        }
        if (ok) FlushFileBuffers(f.h);
    }
    if (ok) {  // verify every file from the copy with a fresh reader
        Pk2Reader r;
        if (!r.Open(tmp, asciiKey, err)) ok = false;
        for (const auto& pf : files) {
            if (!ok) break;
            Bytes back;
            std::wstring e;
            if (!r.Read(pf.inner, back, e) || back != pf.data) {
                err = L"pk2 kopyası doğrulanamadı / verify failed: " + pf.inner;
                ok = false;
            }
        }
    }
    if (!ok) {
        DeleteFileW(tmp.c_str());
        if (err.empty()) err = L"pk2 kopyası yazılamadı / pk2 copy failed";
        return false;
    }
    FinishReport(rep, log);
    if (FileExists(dst) && !opt.backupExisting) {
        if (!DeleteFileW(dst.c_str())) {
            err = L"Eski pk2 kopyası silinemedi (açık mı?) / cannot replace: " + dst;
            DeleteFileW(tmp.c_str());
            return false;
        }
        log += L"  " + dst + L"  (önceki kopyanın yerine / replaces the previous copy)\r\n";
    }
    if (FileExists(dst)) {
        std::wstring bak = dst + L".bak";
        if (FileExists(bak)) bak = dst + L"." + std::to_wstring(GetTickCount64()) + L".bak";
        if (!MoveFileExW(dst.c_str(), bak.c_str(), MOVEFILE_WRITE_THROUGH)) {
            err = L"Eski pk2 kopyası .bak yapılamadı / cannot back up: " + dst;
            DeleteFileW(tmp.c_str());
            return false;
        }
        log += L"  .bak: " + bak + L"\r\n";
    }
    if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_WRITE_THROUGH)) {
        err = L"pk2 kopyası taşınamadı / move failed: " + dst;
        return false;
    }
    if (repOut) *repOut = rep;
    if (progress) progress(100);
    return true;
}

// ------------------------------------------------------------------------------------------------ key specs + discovery
bool Pk2KeyBytes(const std::string& spec, std::vector<std::uint8_t>& key) {
    key.clear();
    if (spec.rfind("raw:", 0) == 0) return ParseHex(spec.substr(4), key) && key.size() <= 56;
    std::string ascii = spec, base = "STD";
    const size_t at = spec.rfind('@');
    if (at != std::string::npos && at > 0) {
        ascii = spec.substr(0, at);
        base = spec.substr(at + 1);
    }
    if (ascii.empty() || ascii.size() > 56) return false;
    std::vector<std::uint8_t> b;
    if (base == "STD" || base == "std") b.assign(kBaseKey, kBaseKey + 10);
    else if (base == "PARADISE" || base == "paradise") b.assign(kBaseParadise, kBaseParadise + 10);
    else if (base == "ZERO" || base == "zero") b.assign(10, 0);
    else if (!ParseHex(base, b) || b.empty() || b.size() > 56) return false;
    for (size_t i = 0; i < ascii.size(); ++i)
        key.push_back(static_cast<std::uint8_t>(static_cast<std::uint8_t>(ascii[i]) ^ (i < b.size() ? b[i] : 0)));
    return true;
}

namespace {

struct ArcProbe {
    std::uint8_t hdr[256]{};
    std::uint8_t root[kBlockSize]{};
    bool encrypted = true, ok = false;
    bool Load(const std::wstring& path) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD got = 0;
        ok = ::ReadFile(h, hdr, 256, &got, nullptr) && got == 256 && std::memcmp(hdr, "JoyMax File Manager!", 20) == 0 &&
             ::ReadFile(h, root, kBlockSize, &got, nullptr) && got == kBlockSize;
        CloseHandle(h);
        encrypted = hdr[34] != 0;
        return ok;
    }
    bool Test(const std::vector<std::uint8_t>& key) const {
        if (key.empty()) return false;
        Blowfish bf;
        bf.Init(key.data(), key.size());
        return RootOracle(bf, root);
    }
};

std::string HexOf(const std::uint8_t* p, size_t n) {
    static const char* hx = "0123456789abcdef";
    std::string h;
    for (size_t t = 0; t < n; ++t) {
        h += hx[p[t] >> 4];
        h += hx[p[t] & 15];
    }
    return h;
}

}  // namespace

bool Pk2KeyOpens(const std::wstring& pk2, const std::string& spec) {
    Pk2Reader r;
    std::wstring e;
    return r.Open(pk2, spec, e);
}

KeyFindReport FindPk2Key(const std::wstring& pk2, const std::string& typed, const std::vector<std::string>& known,
                         const std::function<void(int)>& progress) {
    KeyFindReport rep;
    ArcProbe ap;
    if (!ap.Load(pk2)) {
        rep.whyTr = L"Dosya pk2 değil ya da okunamıyor: " + pk2;
        rep.whyEn = L"Not a pk2 or unreadable: " + pk2;
        return rep;
    }
    if (!ap.encrypted) {  // unencrypted archive: any key works
        rep.found = true;
        rep.key = typed.empty() ? "169841" : typed;
        rep.howTr = L"arşiv şifresiz";
        rep.howEn = L"archive not encrypted";
        return rep;
    }
    auto tryKey = [&](const std::string& spec) {
        std::vector<std::uint8_t> k;
        ++rep.tests;
        return Pk2KeyBytes(spec, k) && ap.Test(k);
    };
    // stage 1: the typed key, remembered / sibling keys and 169841, each with the three published base keys
    std::vector<std::string> first;
    if (!typed.empty()) first.push_back(typed);
    for (const auto& k : known) first.push_back(k);
    first.push_back("169841");
    for (const auto& k : first) {
        const bool plain = k.find('@') == std::string::npos && k.rfind("raw:", 0) != 0;
        if (tryKey(k)) {
            rep.found = true;
            rep.key = k;
        } else if (plain) {
            for (const char* b : {"PARADISE", "ZERO"})
                if (tryKey(k + "@" + b)) {
                    rep.found = true;
                    rep.key = k + "@" + b;
                    break;
                }
        }
        if (rep.found) {
            rep.howTr = L"bilinen anahtar (" + FromUtf8(rep.key) + L")";
            rep.howEn = L"known key (" + FromUtf8(rep.key) + L")";
            return rep;
        }
    }
    // stage 2: static scan of the exe/dll files next to the archive (read only; nothing is started, no memory is read)
    const std::wstring dir = DirOf(pk2);
    std::vector<std::wstring> bins;
    WIN32_FIND_DATAW fd{};
    HANDLE fh = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring n = fd.cFileName;
            const unsigned long long sz = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            if ((EndsWithI(n, L".exe") || EndsWithI(n, L".dll")) && sz > 0 && sz <= 256ull * 1024 * 1024) bins.push_back(n);
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    auto rank = [](const std::wstring& n) {
        const std::wstring l = ToUpper(n);
        if (l.find(L"GFX") != std::wstring::npos) return 0;
        if (l.find(L"CLIENT") != std::wstring::npos) return 1;
        if (l.find(L"GUARD") != std::wstring::npos || l.find(L"LIB") != std::wstring::npos) return 2;
        return 3;
    };
    std::sort(bins.begin(), bins.end(), [&](const std::wstring& a, const std::wstring& b) {
        return rank(a) < rank(b) || (rank(a) == rank(b) && a < b);
    });
    static const char* kLoc[] = {"Wrong BlowfishKey", "MaxiGuard DLL:", "GFXFileManager.dll", ".?AVCBlowFish@@"};
    const int maxTests = 600000;
    size_t fi = 0;
    for (const auto& bn : bins) {
        if (progress) progress(static_cast<int>(fi++ * 100 / std::max<size_t>(1, bins.size())));
        Bytes b;
        std::wstring e;
        if (!ReadFileBytes(JoinPath(dir, bn), b, e) || b.size() < 16) continue;
        ++rep.filesScanned;
        std::vector<size_t> anchors;
        for (const char* loc : kLoc) {
            const size_t ln = std::strlen(loc);
            size_t p = 0;
            while (p + ln <= b.size() && anchors.size() < 64) {
                const auto it = std::search(b.begin() + static_cast<std::ptrdiff_t>(p), b.end(), loc, loc + ln);
                if (it == b.end()) break;
                anchors.push_back(static_cast<size_t>(it - b.begin()));
                p = static_cast<size_t>(it - b.begin()) + 1;
            }
        }
        auto scanRange = [&](size_t a, size_t z) -> bool {
            size_t i = a;
            while (i < z) {
                size_t j = i;
                while (j < z && b[j] >= 0x21 && b[j] <= 0x7E) ++j;
                const size_t len = j - i;
                if (len >= 6 && len <= 56 && j < b.size() && b[j] == 0) {
                    const std::string cand(reinterpret_cast<const char*>(&b[i]), len);
                    std::vector<std::string> specs = {cand, cand + "@PARADISE", cand + "@ZERO"};
                    size_t q = i;  // base key right before the ascii key (0..8 zero bytes of padding)
                    int pad = 0;
                    while (q > 0 && b[q - 1] == 0 && pad < 8) {
                        --q;
                        ++pad;
                    }
                    if (q >= 10) {
                        bool nz = true;
                        for (size_t t = q - 10; t < q; ++t) nz = nz && b[t] != 0;
                        if (nz) specs.push_back(cand + "@" + HexOf(&b[q - 10], 10));
                    }
                    for (const auto& sp : specs) {
                        if (rep.tests >= maxTests) return false;
                        if (tryKey(sp)) {
                            rep.found = true;
                            rep.key = sp;
                            rep.howTr = L"istemci dosyasından okundu: " + bn;
                            rep.howEn = L"read from the client file " + bn;
                            return true;
                        }
                    }
                }
                i = j + 1;
            }
            return false;
        };
        bool hit = false;
        if (!anchors.empty()) {
            for (size_t a : anchors) {
                const size_t lo = a > 4096 ? a - 4096 : 0, hi = std::min(b.size(), a + 4096);
                hit = scanRange(lo, hi);
                if (hit || rep.tests >= maxTests) break;
            }
        } else if (b.size() <= 32ull * 1024 * 1024) {
            hit = scanRange(0, b.size());
        }
        if (hit) return rep;
        if (rep.tests >= maxTests) break;
    }
    rep.whyTr = Fmt(L"Pk2 anahtarı bulunamadı (%s). Denenen: yazılan anahtar, 169841 (3 taban anahtarıyla) ve klasördeki %d "
                    L"exe/dll dosyasında %d aday. Bu istemcinin anahtarı korumalı (şifreli) bir dosyanın içinde olabilir: "
                    L"anahtarı sunucu yöneticisinden öğrenip 'Pk2 anahtarı' kutusuna yazın (ör. 169841 ya da ANAHTAR@PARADISE). "
                    L"Araç oyunu asla çalıştırmaz, oyun belleğini okumaz.",
                    FileNameOf(pk2).c_str(), rep.filesScanned, rep.tests);
    rep.whyEn = Fmt(L"Pk2 key not found (%s). Tried: the typed key, 169841 (3 base keys) and %d candidates in %d exe/dll files "
                    L"of the folder. The key may be inside a protected (encrypted) file: ask the server admin and type it in "
                    L"the 'Pk2 key' box. The tool never starts the game or reads its memory.",
                    FileNameOf(pk2).c_str(), rep.tests, rep.filesScanned);
    return rep;
}

bool Pk2Reader::List(const std::wstring& innerDir, std::vector<std::pair<std::wstring, bool>>& out) {
    out.clear();
    std::wstring err;
    std::int64_t dir = 256;
    std::vector<Entry> entries;
    std::wstring rest = innerDir;
    for (auto& c : rest)
        if (c == L'/') c = L'\\';
    size_t a = 0;
    while (a < rest.size()) {
        size_t b = rest.find(L'\\', a);
        if (b == std::wstring::npos) b = rest.size();
        if (b > a) {
            if (!IsExactIn(949, rest.substr(a, b - a))) return false;  // v5 M3: no best-fit folder match
            const std::string part = LowerAscii(Narrow949(rest.substr(a, b - a)));
            if (!Block(dir, entries, err)) return false;
            bool hit = false;
            for (const auto& e : entries)
                if (e.type == 1 && e.name == part) {
                    dir = e.off;
                    hit = true;
                    break;
                }
            if (!hit) return false;
        }
        a = b + 1;
    }
    if (!Block(dir, entries, err)) return false;
    for (const auto& e : entries) {
        if (e.name == "." || e.name == "..") continue;
        out.push_back({FromAnsi(e.name, 949), e.type == 1});
    }
    return true;
}

}  // namespace se
