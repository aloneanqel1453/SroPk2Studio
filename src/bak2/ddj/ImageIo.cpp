// SkillEffect Studio v6 - ddj/ImageIo.cpp  (M33 / M38) - picture readers + image helpers. See ImageIo.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include "ImageIo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>

namespace se::ddj {
namespace {

void Why(std::wstring* w, const std::wstring& s) {
    if (w) *w = s;
}

inline std::uint32_t Be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
inline std::uint32_t Le32(const std::uint8_t* p) {
    return p[0] | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint16_t Le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

constexpr std::size_t kMaxPixels = 64u << 20;   // 64 M pixels (8192 x 8192)
bool SizeOk(long long w, long long h) { return w > 0 && h > 0 && w <= 32768 && h <= 32768 && w * h <= static_cast<long long>(kMaxPixels); }

std::wstring LowerExt(const std::wstring& name) {
    const auto d = name.find_last_of(L'.');
    if (d == std::wstring::npos) return L"";
    std::wstring e = name.substr(d + 1);
    for (auto& c : e) c = static_cast<wchar_t>(std::towlower(c));
    return e;
}

// ------------------------------------------------------------------------------------------------ inflate
struct BitIn {
    const std::uint8_t* p = nullptr;
    std::size_t n = 0, pos = 0;
    std::uint64_t buf = 0;
    int cnt = 0;
    bool err = false;
    void Fill(int k) {
        while (cnt < k && pos < n) {
            buf |= static_cast<std::uint64_t>(p[pos++]) << cnt;
            cnt += 8;
        }
    }
    std::uint32_t Bits(int k) {
        if (k == 0) return 0;
        Fill(k);
        if (cnt < k) { err = true; return 0; }
        const std::uint32_t v = static_cast<std::uint32_t>(buf & ((1ull << k) - 1));
        buf >>= k;
        cnt -= k;
        return v;
    }
};

struct Huff {
    short count[16]{};
    short symbol[320]{};
    std::uint16_t fast[512]{};   // (len << 9) | sym for codes of <= 9 bits; 0 = slow path
};

int BuildHuff(Huff& h, const short* len, int n) {
    std::memset(h.count, 0, sizeof(h.count));
    std::memset(h.fast, 0, sizeof(h.fast));
    for (int s = 0; s < n; ++s) h.count[len[s]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l < 16; ++l) {
        left <<= 1;
        left -= h.count[l];
        if (left < 0) return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; ++l) offs[l + 1] = static_cast<short>(offs[l] + h.count[l]);
    for (int s = 0; s < n; ++s)
        if (len[s] != 0) h.symbol[offs[len[s]]++] = static_cast<short>(s);
    int code = 0, idx = 0;
    for (int l = 1; l < 16; ++l) {
        for (int i = 0; i < h.count[l]; ++i) {
            const int sym = h.symbol[idx++];
            if (l <= 9) {
                int r = 0;
                for (int b = 0; b < l; ++b) r |= ((code >> b) & 1) << (l - 1 - b);
                for (int k = r; k < 512; k += (1 << l)) h.fast[k] = static_cast<std::uint16_t>((l << 9) | sym);
            }
            ++code;
        }
        code <<= 1;
    }
    return left;
}

int DecodeSym(BitIn& s, const Huff& h) {
    s.Fill(9);
    if (s.cnt >= 9) {
        const std::uint16_t e = h.fast[s.buf & 511];
        if (e) {
            const int l = e >> 9;
            s.buf >>= l;
            s.cnt -= l;
            return e & 511;
        }
    }
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; ++l) {
        code |= static_cast<int>(s.Bits(1));
        if (s.err) return -1;
        const int count = h.count[l];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

constexpr short kLBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr short kLExt[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr short kDBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr short kDExt[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(BitIn& s, Bytes& out, const Huff& lh, const Huff& dh, std::size_t maxOut) {
    for (;;) {
        int sym = DecodeSym(s, lh);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out.size() >= maxOut) return false;
            out.push_back(static_cast<std::uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            const int len = kLBase[sym] + static_cast<int>(s.Bits(kLExt[sym]));
            const int ds = DecodeSym(s, dh);
            if (ds < 0 || ds >= 30) return false;
            const std::size_t dist = static_cast<std::size_t>(kDBase[ds]) + s.Bits(kDExt[ds]);
            if (s.err || dist > out.size() || out.size() + len > maxOut) return false;
            const std::size_t from = out.size() - dist;
            for (int i = 0; i < len; ++i) out.push_back(out[from + i]);
        }
    }
}

bool InflateRaw(BitIn& s, Bytes& out, std::size_t maxOut, std::wstring* why) {
    struct Fixed {
        Huff L, D;
        Fixed() {
            short l[288];
            for (int i = 0; i < 144; ++i) l[i] = 8;
            for (int i = 144; i < 256; ++i) l[i] = 9;
            for (int i = 256; i < 280; ++i) l[i] = 7;
            for (int i = 280; i < 288; ++i) l[i] = 8;
            BuildHuff(L, l, 288);
            for (int i = 0; i < 30; ++i) l[i] = 5;
            BuildHuff(D, l, 30);
        }
    };
    static const Fixed fx;   // thread-safe init
    const Huff& fixL = fx.L;
    const Huff& fixD = fx.D;
    int last = 0;
    do {
        last = static_cast<int>(s.Bits(1));
        const int type = static_cast<int>(s.Bits(2));
        if (s.err) { Why(why, L"sıkıştırılmış veri kesik"); return false; }
        if (type == 0) {
            s.buf >>= (s.cnt & 7);
            s.cnt -= (s.cnt & 7);
            const std::uint32_t len = s.Bits(16), nlen = s.Bits(16);
            if (s.err || (len ^ 0xFFFF) != nlen) { Why(why, L"sıkıştırılmamış blok bozuk"); return false; }
            if (out.size() + len > maxOut) { Why(why, L"veri beklenenden uzun"); return false; }
            std::uint32_t k = 0;
            while (k < len && s.cnt >= 8) { out.push_back(static_cast<std::uint8_t>(s.Bits(8))); ++k; }
            if (s.pos + (len - k) > s.n) { Why(why, L"sıkıştırılmamış blok kesik"); return false; }
            out.insert(out.end(), s.p + s.pos, s.p + s.pos + (len - k));
            s.pos += len - k;
        } else if (type == 1) {
            if (!Codes(s, out, fixL, fixD, maxOut)) { Why(why, L"sıkıştırılmış veri bozuk (sabit)"); return false; }
        } else if (type == 2) {
            const int nlen = static_cast<int>(s.Bits(5)) + 257, ndist = static_cast<int>(s.Bits(5)) + 1,
                      ncode = static_cast<int>(s.Bits(4)) + 4;
            if (s.err || nlen > 286 || ndist > 30) { Why(why, L"sıkıştırma tablosu bozuk"); return false; }
            static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            short lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<short>(s.Bits(3));
            Huff lencode, distcode;
            if (BuildHuff(lencode, lengths, 19) != 0) { Why(why, L"sıkıştırma tablosu bozuk"); return false; }
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = DecodeSym(s, lencode);
                if (sym < 0) { Why(why, L"sıkıştırma tablosu bozuk"); return false; }
                if (sym < 16) {
                    lengths[idx++] = static_cast<short>(sym);
                } else {
                    short v = 0;
                    int rep;
                    if (sym == 16) {
                        if (idx == 0) { Why(why, L"sıkıştırma tablosu bozuk"); return false; }
                        v = lengths[idx - 1];
                        rep = 3 + static_cast<int>(s.Bits(2));
                    } else if (sym == 17) {
                        rep = 3 + static_cast<int>(s.Bits(3));
                    } else {
                        rep = 11 + static_cast<int>(s.Bits(7));
                    }
                    if (idx + rep > nlen + ndist) { Why(why, L"sıkıştırma tablosu bozuk"); return false; }
                    while (rep--) lengths[idx++] = v;
                }
            }
            if (lengths[256] == 0) { Why(why, L"sıkıştırma tablosu bozuk"); return false; }
            const int el = BuildHuff(lencode, lengths, nlen);
            if (el < 0 || (el > 0 && nlen - lencode.count[0] != 1)) { Why(why, L"sıkıştırma tablosu bozuk"); return false; }
            const int ed = BuildHuff(distcode, lengths + nlen, ndist);
            if (ed < 0 || (ed > 0 && ndist - distcode.count[0] != 1)) { Why(why, L"sıkıştırma tablosu bozuk"); return false; }
            if (!Codes(s, out, lencode, distcode, maxOut)) { Why(why, L"sıkıştırılmış veri bozuk"); return false; }
        } else {
            Why(why, L"bilinmeyen sıkıştırma bloğu");
            return false;
        }
    } while (!last);
    return true;
}

std::uint32_t Adler32(const std::uint8_t* p, std::size_t n) {
    std::uint32_t a = 1, b = 0;
    while (n) {
        const std::size_t k = std::min<std::size_t>(n, 5552);
        for (std::size_t i = 0; i < k; ++i) { a += p[i]; b += a; }
        a %= 65521;
        b %= 65521;
        p += k;
        n -= k;
    }
    return (b << 16) | a;
}

std::uint32_t Crc32(const std::uint8_t* p, std::size_t n) {
    struct Tab {
        std::uint32_t t[256];
        Tab() {
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
        }
    };
    static const Tab tb;   // thread-safe init
    const std::uint32_t* tab = tb.t;
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ------------------------------------------------------------------------------------------------ PNG
std::uint8_t Paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<std::uint8_t>(a);
    if (pb <= pc) return static_cast<std::uint8_t>(b);
    return static_cast<std::uint8_t>(c);
}

bool Unfilter(std::uint8_t* row, const std::uint8_t* prev, std::size_t rb, int bpp, int type) {
    switch (type) {
        case 0: return true;
        case 1:
            for (std::size_t i = bpp; i < rb; ++i) row[i] = static_cast<std::uint8_t>(row[i] + row[i - bpp]);
            return true;
        case 2:
            for (std::size_t i = 0; i < rb; ++i) row[i] = static_cast<std::uint8_t>(row[i] + prev[i]);
            return true;
        case 3:
            for (std::size_t i = 0; i < rb; ++i) {
                const int a = i >= static_cast<std::size_t>(bpp) ? row[i - bpp] : 0;
                row[i] = static_cast<std::uint8_t>(row[i] + ((a + prev[i]) >> 1));
            }
            return true;
        case 4:
            for (std::size_t i = 0; i < rb; ++i) {
                const bool h = i >= static_cast<std::size_t>(bpp);
                row[i] = static_cast<std::uint8_t>(row[i] + Paeth(h ? row[i - bpp] : 0, prev[i], h ? prev[i - bpp] : 0));
            }
            return true;
        default: return false;
    }
}

}  // namespace

bool Inflate(const std::uint8_t* p, std::size_t n, Bytes& out, std::wstring* why, std::size_t expect) {
    out.clear();
    if (n < 2) { Why(why, L"sıkıştırılmış veri boş"); return false; }
    const int cmf = p[0], flg = p[1];
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) { Why(why, L"zlib başlığı bozuk"); return false; }
    BitIn s;
    s.p = p + 2;
    s.n = n - 2;
    out.reserve(expect ? expect : n * 4);
    const std::size_t maxOut = expect ? expect + 65536 : (std::size_t{1} << 30);
    if (!InflateRaw(s, out, maxOut, why)) return false;
    // adler32 after the byte-aligned end (checked when present)
    std::size_t at = s.pos - static_cast<std::size_t>(s.cnt / 8);
    if (at + 4 <= s.n && Be32(s.p + at) != Adler32(out.data(), out.size())) { Why(why, L"veri sağlaması (adler32) tutmuyor"); return false; }
    return true;
}

bool DecodePng(const std::uint8_t* p, std::size_t n, Image& out, std::wstring* why) {
    static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (n < 33 || std::memcmp(p, sig, 8) != 0) { Why(why, L"PNG imzası yok"); return false; }
    std::size_t at = 8;
    std::uint32_t w = 0, h = 0;
    int depth = 0, ct = -1, inter = 0;
    std::uint8_t plte[256][3] = {};
    int nplte = 0;
    std::uint8_t trnsA[256];
    std::memset(trnsA, 255, sizeof(trnsA));
    int trnsKey[3] = {-1, -1, -1};
    Bytes idat;
    bool seenHdr = false, seenEnd = false;
    while (at + 12 <= n) {
        const std::uint32_t len = Be32(p + at);
        const std::uint8_t* type = p + at + 4;
        if (len > n - at - 12) { Why(why, L"PNG kesik (parça boyu dosyayı aşıyor)"); return false; }
        const std::uint8_t* d = p + at + 8;
        if (Crc32(type, len + 4) != Be32(d + len)) { Why(why, L"PNG bozuk (CRC tutmuyor)"); return false; }
        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (len < 13) { Why(why, L"PNG başlığı kısa"); return false; }
            w = Be32(d);
            h = Be32(d + 4);
            depth = d[8];
            ct = d[9];
            inter = d[12];
            if (d[10] != 0 || d[11] != 0 || inter > 1) { Why(why, L"PNG: desteklenmeyen sıkıştırma / süzgeç / geçmeli kip"); return false; }
            const bool okd = (ct == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
                             (ct == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)) ||
                             ((ct == 2 || ct == 4 || ct == 6) && (depth == 8 || depth == 16));
            if (!okd) { Why(why, L"PNG: geçersiz renk türü / bit derinliği"); return false; }
            seenHdr = true;
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            nplte = static_cast<int>(std::min<std::uint32_t>(len / 3, 256));
            for (int i = 0; i < nplte; ++i) std::memcpy(plte[i], d + i * 3, 3);
        } else if (std::memcmp(type, "tRNS", 4) == 0) {
            if (ct == 3) {
                for (std::uint32_t i = 0; i < len && i < 256; ++i) trnsA[i] = d[i];
            } else if (ct == 0 && len >= 2) {
                trnsKey[0] = (d[0] << 8) | d[1];
            } else if (ct == 2 && len >= 6) {
                for (int k = 0; k < 3; ++k) trnsKey[k] = (d[2 * k] << 8) | d[2 * k + 1];
            }
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), d, d + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            seenEnd = true;
            break;
        } else if (!(type[0] & 0x20)) {
            Why(why, L"PNG: bilinmeyen zorunlu parça");
            return false;
        }
        at += 12 + len;
    }
    (void)seenEnd;
    if (!seenHdr) { Why(why, L"PNG başlığı (IHDR) yok"); return false; }
    if (!SizeOk(w, h)) { Why(why, L"PNG boyutu desteklenmiyor (en çok 32768 x 32768, 64 M piksel)"); return false; }
    if (ct == 3 && nplte == 0) { Why(why, L"PNG: palet (PLTE) yok"); return false; }
    const int chans = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : 4;
    const int bitsPx = chans * depth;
    const int bpp = std::max(1, bitsPx / 8);
    static const int ax[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    const int passes = inter ? 7 : 1;
    std::size_t expect = 0;
    for (int ps = 0; ps < passes; ++ps) {
        const int x0 = inter ? ax[ps][0] : 0, y0 = inter ? ax[ps][1] : 0, dx = inter ? ax[ps][2] : 1, dy = inter ? ax[ps][3] : 1;
        const std::size_t pw = (w > static_cast<std::uint32_t>(x0)) ? (w - x0 + dx - 1) / dx : 0;
        const std::size_t ph = (h > static_cast<std::uint32_t>(y0)) ? (h - y0 + dy - 1) / dy : 0;
        if (pw && ph) expect += ph * ((pw * bitsPx + 7) / 8 + 1);
    }
    Bytes raw;
    if (!Inflate(idat.data(), idat.size(), raw, why, expect)) return false;
    if (raw.size() < expect) { Why(why, L"PNG piksel verisi kesik"); return false; }
    out.w = static_cast<int>(w);
    out.h = static_cast<int>(h);
    out.rgba.assign(static_cast<std::size_t>(w) * h * 4, 255);
    std::size_t off = 0;
    for (int ps = 0; ps < passes; ++ps) {
        const int x0 = inter ? ax[ps][0] : 0, y0 = inter ? ax[ps][1] : 0, dx = inter ? ax[ps][2] : 1, dy = inter ? ax[ps][3] : 1;
        const std::size_t pw = (w > static_cast<std::uint32_t>(x0)) ? (w - x0 + dx - 1) / dx : 0;
        const std::size_t ph = (h > static_cast<std::uint32_t>(y0)) ? (h - y0 + dy - 1) / dy : 0;
        if (!pw || !ph) continue;
        const std::size_t rb = (pw * bitsPx + 7) / 8;
        Bytes prev(rb, 0);
        for (std::size_t y = 0; y < ph; ++y) {
            const int ft = raw[off];
            std::uint8_t* row = raw.data() + off + 1;
            if (!Unfilter(row, prev.data(), rb, bpp, ft)) { Why(why, L"PNG: bilinmeyen satır süzgeci"); return false; }
            for (std::size_t x = 0; x < pw; ++x) {
                auto sample = [&](int c) -> int {   // raw sample value (full depth)
                    if (depth == 8) return row[x * chans + c];
                    if (depth == 16) return (row[(x * chans + c) * 2] << 8) | row[(x * chans + c) * 2 + 1];
                    const std::size_t bit = (x * chans + c) * depth;
                    return (row[bit / 8] >> (8 - depth - static_cast<int>(bit % 8))) & ((1 << depth) - 1);
                };
                auto to8 = [&](int v) -> std::uint8_t {
                    if (depth == 16) return static_cast<std::uint8_t>(v >> 8);
                    if (depth == 8) return static_cast<std::uint8_t>(v);
                    return static_cast<std::uint8_t>(v * 255 / ((1 << depth) - 1));
                };
                std::uint8_t* o = out.Px(static_cast<int>(x0 + x * dx), static_cast<int>(y0 + y * dy));
                switch (ct) {
                    case 0: {
                        const int v = sample(0);
                        o[0] = o[1] = o[2] = to8(v);
                        o[3] = (trnsKey[0] >= 0 && v == trnsKey[0]) ? 0 : 255;
                        break;
                    }
                    case 2: {
                        const int r = sample(0), g = sample(1), b = sample(2);
                        o[0] = to8(r); o[1] = to8(g); o[2] = to8(b);
                        o[3] = (trnsKey[0] >= 0 && r == trnsKey[0] && g == trnsKey[1] && b == trnsKey[2]) ? 0 : 255;
                        break;
                    }
                    case 3: {
                        const int i = sample(0);
                        if (i < nplte) { o[0] = plte[i][0]; o[1] = plte[i][1]; o[2] = plte[i][2]; }
                        else { o[0] = o[1] = o[2] = 0; }
                        o[3] = trnsA[i];
                        break;
                    }
                    case 4: o[0] = o[1] = o[2] = to8(sample(0)); o[3] = to8(sample(1)); break;
                    default: o[0] = to8(sample(0)); o[1] = to8(sample(1)); o[2] = to8(sample(2)); o[3] = to8(sample(3)); break;
                }
            }
            std::memcpy(prev.data(), row, rb);
            off += rb + 1;
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ BMP
namespace {
void MaskInfo(std::uint32_t m, int& sh, int& bits) {
    sh = bits = 0;
    if (!m) return;
    while (!((m >> sh) & 1)) ++sh;
    std::uint32_t v = m >> sh;
    while (v & 1) { ++bits; v >>= 1; }
}
std::uint8_t MaskChan(std::uint32_t px, std::uint32_t m, int sh, int bits, std::uint8_t def) {
    if (!m || bits == 0) return def;
    const std::uint64_t mx = bits >= 32 ? 0xFFFFFFFFull : ((1ull << bits) - 1);
    const std::uint64_t v = (px & m) >> sh;
    return static_cast<std::uint8_t>((v * 255 + mx / 2) / mx);
}
}  // namespace

bool DecodeBmp(const std::uint8_t* p, std::size_t n, Image& out, std::wstring* why) {
    if (n < 26 || p[0] != 'B' || p[1] != 'M') { Why(why, L"BMP imzası yok"); return false; }
    const std::uint32_t offBits = Le32(p + 10), hs = Le32(p + 14);
    if (hs < 12 || 14 + static_cast<std::size_t>(hs) > n) { Why(why, L"BMP başlığı bozuk"); return false; }
    long long w, h;
    int bits;
    std::uint32_t comp = 0, clrUsed = 0;
    std::uint32_t masks[4] = {0, 0, 0, 0};
    const std::uint8_t* d = p + 14;
    bool core = hs == 12;
    if (core) {
        w = Le16(d + 4);
        h = static_cast<std::int16_t>(Le16(d + 6));
        bits = Le16(d + 10);
    } else {
        if (hs < 40) { Why(why, L"BMP başlığı bozuk"); return false; }
        w = static_cast<std::int32_t>(Le32(d + 4));
        h = static_cast<std::int32_t>(Le32(d + 8));
        bits = Le16(d + 14);
        comp = Le32(d + 16);
        clrUsed = Le32(d + 32);
        if (hs >= 52) { masks[0] = Le32(d + 40); masks[1] = Le32(d + 44); masks[2] = Le32(d + 48); }
        if (hs >= 56) masks[3] = Le32(d + 52);
    }
    std::size_t palAt = 14 + hs;
    if (!core && hs == 40 && (comp == 3 || comp == 6)) {
        const int nm = comp == 6 ? 4 : 3;
        if (palAt + nm * 4 > n) { Why(why, L"BMP renk maskeleri kesik"); return false; }
        for (int i = 0; i < nm; ++i) masks[i] = Le32(p + palAt + i * 4);
        palAt += nm * 4;
    }
    const bool topDown = h < 0;
    if (h < 0) h = -h;
    if (!SizeOk(w, h)) { Why(why, L"BMP boyutu desteklenmiyor"); return false; }
    if (comp == 4 || comp == 5) { Why(why, L"BMP içinde JPEG / PNG var (desteklenmiyor)"); return false; }
    if (comp != 0 && comp != 1 && comp != 2 && comp != 3 && comp != 6) { Why(why, L"BMP sıkıştırması desteklenmiyor"); return false; }
    if (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32) { Why(why, L"BMP bit derinliği desteklenmiyor"); return false; }
    std::uint8_t pal[256][4] = {};
    if (bits <= 8) {
        const int entry = core ? 3 : 4;
        int np = clrUsed ? static_cast<int>(std::min<std::uint32_t>(clrUsed, 256)) : (1 << bits);
        for (int i = 0; i < np; ++i) {
            const std::size_t at = palAt + static_cast<std::size_t>(i) * entry;
            if (at + 3 > n) break;
            pal[i][0] = p[at + 2]; pal[i][1] = p[at + 1]; pal[i][2] = p[at]; pal[i][3] = 255;
        }
    }
    if (offBits >= n) { Why(why, L"BMP piksel verisi yok"); return false; }
    out.w = static_cast<int>(w);
    out.h = static_cast<int>(h);
    out.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
    const std::uint8_t* px = p + offBits;
    const std::size_t avail = n - offBits;
    auto put = [&](long long x, long long yFile, const std::uint8_t* c) {
        const long long y = topDown ? yFile : (h - 1 - yFile);
        if (x < 0 || x >= w || y < 0 || y >= h) return;
        std::memcpy(out.Px(static_cast<int>(x), static_cast<int>(y)), c, 4);
    };
    if (comp == 1 || comp == 2) {   // RLE8 / RLE4 (bottom-up)
        const bool r4 = comp == 2;
        if ((r4 && bits != 4) || (!r4 && bits != 8)) { Why(why, L"BMP RLE bit derinliği uyumsuz"); return false; }
        long long x = 0, y = 0;
        std::size_t i = 0;
        for (auto& v : out.rgba) v = 0;
        for (long long yy = 0; yy < h; ++yy)
            for (long long xx = 0; xx < w; ++xx) put(xx, yy, pal[0]);
        while (i + 1 < avail) {
            const int a = px[i], b = px[i + 1];
            i += 2;
            if (a > 0) {
                for (int k = 0; k < a; ++k) put(x++, y, pal[r4 ? ((k & 1) ? (b & 15) : (b >> 4)) : b]);
            } else if (b == 0) {
                x = 0; ++y;
            } else if (b == 1) {
                break;
            } else if (b == 2) {
                if (i + 1 >= avail) break;
                x += px[i]; y += px[i + 1]; i += 2;
            } else {
                const std::size_t bytes = r4 ? (static_cast<std::size_t>(b) + 1) / 2 : static_cast<std::size_t>(b);
                if (i + bytes > avail) break;
                for (int k = 0; k < b; ++k) {
                    const int v = r4 ? ((k & 1) ? (px[i + k / 2] & 15) : (px[i + k / 2] >> 4)) : px[i + k];
                    put(x++, y, pal[v]);
                }
                i += (bytes + 1) & ~std::size_t{1};
            }
        }
        return true;
    }
    const std::size_t stride = ((static_cast<std::size_t>(w) * bits + 31) / 32) * 4;
    if (stride * (h - 1) + (static_cast<std::size_t>(w) * bits + 7) / 8 > avail) { Why(why, L"BMP piksel verisi kesik"); return false; }
    bool bitfields = comp == 3 || comp == 6;
    if (bits == 16 && !bitfields) { masks[0] = 0x7C00; masks[1] = 0x03E0; masks[2] = 0x001F; masks[3] = 0; bitfields = true; }
    bool alphaFromX = false;
    if (bits == 32 && !bitfields) {   // BI_RGB 32: B,G,R,X - X used as alpha only when some X is non-zero
        for (long long y = 0; y < h && !alphaFromX; ++y)
            for (long long x = 0; x < w; ++x)
                if (px[y * stride + x * 4 + 3]) { alphaFromX = true; break; }
    }
    int sh[4], nb[4];
    for (int k = 0; k < 4; ++k) MaskInfo(masks[k], sh[k], nb[k]);
    for (long long y = 0; y < h; ++y) {
        const std::uint8_t* r = px + y * stride;
        for (long long x = 0; x < w; ++x) {
            std::uint8_t c[4] = {0, 0, 0, 255};
            if (bits <= 8) {
                const std::size_t bit = static_cast<std::size_t>(x) * bits;
                const int v = (r[bit / 8] >> (8 - bits - static_cast<int>(bit % 8))) & ((1 << bits) - 1);
                std::memcpy(c, pal[v], 4);
            } else if (bits == 24) {
                c[0] = r[x * 3 + 2]; c[1] = r[x * 3 + 1]; c[2] = r[x * 3];
            } else if (bits == 32 && !bitfields) {
                c[0] = r[x * 4 + 2]; c[1] = r[x * 4 + 1]; c[2] = r[x * 4];
                c[3] = alphaFromX ? r[x * 4 + 3] : 255;
            } else {
                const std::uint32_t v = bits == 16 ? Le16(r + x * 2) : Le32(r + x * 4);
                for (int k = 0; k < 3; ++k) c[k] = MaskChan(v, masks[k], sh[k], nb[k], 0);
                c[3] = MaskChan(v, masks[3], sh[3], nb[3], 255);
            }
            put(x, y, c);
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ TGA
bool DecodeTga(const std::uint8_t* p, std::size_t n, Image& out, std::wstring* why) {
    if (n < 18) { Why(why, L"TGA dosyası kısa"); return false; }
    const int idLen = p[0], cmType = p[1], type = p[2];
    const int cmFirst = Le16(p + 3), cmLen = Le16(p + 5), cmBits = p[7];
    const int w = Le16(p + 12), h = Le16(p + 14), bpp = p[16], desc = p[17];
    if (!(type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11) || cmType > 1) { Why(why, L"TGA türü desteklenmiyor"); return false; }
    if (!SizeOk(w, h)) { Why(why, L"TGA boyutu geçersiz"); return false; }
    const bool mapped = type == 1 || type == 9, gray = type == 3 || type == 11, rle = type >= 9;
    if (mapped ? (bpp != 8 && bpp != 16) : gray ? (bpp != 8 && bpp != 16) : (bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32)) {
        Why(why, L"TGA bit derinliği desteklenmiyor");
        return false;
    }
    const int alphaBits = desc & 15;
    std::size_t at = 18 + static_cast<std::size_t>(idLen);
    std::vector<std::uint8_t> cmap;   // RGBA entries
    if (cmType == 1) {
        const int eb = (cmBits + 7) / 8;
        if (eb < 2 || eb > 4 || at + static_cast<std::size_t>(cmLen) * eb > n) { Why(why, L"TGA renk tablosu bozuk"); return false; }
        cmap.resize(static_cast<std::size_t>(cmLen) * 4);
        for (int i = 0; i < cmLen; ++i) {
            const std::uint8_t* e = p + at + static_cast<std::size_t>(i) * eb;
            std::uint8_t* o = &cmap[static_cast<std::size_t>(i) * 4];
            if (eb == 2) {
                const int v = Le16(e);
                o[0] = static_cast<std::uint8_t>(((v >> 10) & 31) * 255 / 31);
                o[1] = static_cast<std::uint8_t>(((v >> 5) & 31) * 255 / 31);
                o[2] = static_cast<std::uint8_t>((v & 31) * 255 / 31);
                o[3] = 255;
            } else {
                o[0] = e[2]; o[1] = e[1]; o[2] = e[0];
                o[3] = eb == 4 ? e[3] : 255;
            }
        }
        at += static_cast<std::size_t>(cmLen) * eb;
    }
    if (mapped && cmap.empty()) { Why(why, L"TGA renk tablosu yok"); return false; }
    const int pb = (bpp + 7) / 8;
    const std::size_t total = static_cast<std::size_t>(w) * h;
    std::vector<std::uint8_t> pix(total * 4);
    std::size_t done = 0;
    bool anyA = false;
    auto conv = [&](const std::uint8_t* s, std::uint8_t* o) {
        if (mapped) {
            const int idx = (pb == 1 ? s[0] : Le16(s)) - cmFirst;
            if (idx >= 0 && idx < cmLen) std::memcpy(o, &cmap[static_cast<std::size_t>(idx) * 4], 4);
            else { o[0] = o[1] = o[2] = 0; o[3] = 255; }
        } else if (gray) {
            o[0] = o[1] = o[2] = s[0];
            o[3] = pb == 2 ? s[1] : 255;
        } else if (pb == 2) {
            const int v = Le16(s);
            o[0] = static_cast<std::uint8_t>(((v >> 10) & 31) * 255 / 31);
            o[1] = static_cast<std::uint8_t>(((v >> 5) & 31) * 255 / 31);
            o[2] = static_cast<std::uint8_t>((v & 31) * 255 / 31);
            o[3] = (bpp == 16 && alphaBits >= 1) ? ((v & 0x8000) ? 255 : 0) : 255;
        } else {
            o[0] = s[2]; o[1] = s[1]; o[2] = s[0];
            o[3] = pb == 4 ? s[3] : 255;
            if (pb == 4 && s[3]) anyA = true;
        }
    };
    if (!rle) {
        if (at + total * pb > n) { Why(why, L"TGA piksel verisi kesik"); return false; }
        for (std::size_t i = 0; i < total; ++i) conv(p + at + i * pb, &pix[i * 4]);
    } else {
        while (done < total) {
            if (at >= n) { Why(why, L"TGA RLE verisi kesik"); return false; }
            const int hd = p[at++];
            const std::size_t cnt = static_cast<std::size_t>(hd & 0x7F) + 1;
            if (hd & 0x80) {
                if (at + pb > n) { Why(why, L"TGA RLE verisi kesik"); return false; }
                std::uint8_t c[4];
                conv(p + at, c);
                at += pb;
                for (std::size_t k = 0; k < cnt && done < total; ++k, ++done) std::memcpy(&pix[done * 4], c, 4);
            } else {
                if (at + cnt * pb > n) { Why(why, L"TGA RLE verisi kesik"); return false; }
                for (std::size_t k = 0; k < cnt && done < total; ++k, ++done) conv(p + at + k * pb, &pix[done * 4]);
                at += cnt * pb;
            }
        }
    }
    if (!mapped && !gray && pb == 4 && alphaBits == 0 && !anyA)   // 32-bit with an unused (all zero) alpha byte
        for (std::size_t i = 0; i < total; ++i) pix[i * 4 + 3] = 255;
    out.w = w;
    out.h = h;
    out.rgba.assign(total * 4, 0);
    const bool top = (desc & 0x20) != 0, right = (desc & 0x10) != 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sy = top ? y : h - 1 - y, sx = right ? w - 1 - x : x;
            std::memcpy(out.Px(x, y), &pix[(static_cast<std::size_t>(sy) * w + sx) * 4], 4);
        }
    return true;
}

// ------------------------------------------------------------------------------------------------ WIC
namespace {
template <class T>
struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};
}  // namespace

bool DecodeWic(const std::uint8_t* p, std::size_t n, Image& out, std::wstring* why) {
    if (n == 0 || n > 0x7FFFFFFF) { Why(why, L"resim boş ya da çok büyük"); return false; }
    const HRESULT hi = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(hi);
    bool ok = false;
    {
        Com<IWICImagingFactory> fac;
        Com<IWICStream> st;
        Com<IWICBitmapDecoder> dec;
        Com<IWICBitmapFrameDecode> fr;
        Com<IWICBitmapSource> conv;
        UINT w = 0, h = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac.p))) &&
            SUCCEEDED(fac->CreateStream(&st)) &&
            SUCCEEDED(st->InitializeFromMemory(const_cast<BYTE*>(p), static_cast<DWORD>(n))) &&
            SUCCEEDED(fac->CreateDecoderFromStream(st.p, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &fr)) &&
            SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, fr.p, &conv)) &&
            SUCCEEDED(conv->GetSize(&w, &h)) && SizeOk(w, h)) {
            out.w = static_cast<int>(w);
            out.h = static_cast<int>(h);
            out.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
            ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.rgba.size()), out.rgba.data()));
        }
    }
    if (uninit) CoUninitialize();
    if (!ok) Why(why, L"resim biçimi tanınmadı (PNG, BMP, TGA, JPG, DDS, DDJ desteklenir)");
    return ok;
}

// ------------------------------------------------------------------------------------------------ dispatch
PicKind SniffPicture(const std::uint8_t* p, std::size_t n, const std::wstring& hint) {
    if (n >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') return PicKind::Png;
    if (n >= 7 && std::memcmp(p, "JMXVDDJ", 7) == 0) return PicKind::Ddj;
    if (n >= 4 && std::memcmp(p, "DDS ", 4) == 0) return PicKind::Dds;
    if (n >= 26 && p[0] == 'B' && p[1] == 'M') return PicKind::Bmp;
    if (n >= 18) {
        const int t = p[2], cm = p[1], bpp = p[16];
        const bool plausible = (t == 1 || t == 2 || t == 3 || t == 9 || t == 10 || t == 11) && cm <= 1 &&
                               (bpp == 8 || bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32) && Le16(p + 12) > 0 && Le16(p + 14) > 0;
        if (plausible && (LowerExt(hint) == L"tga" || LowerExt(hint) == L"tpic" || hint.empty())) return PicKind::Tga;
    }
    return n ? PicKind::Other : PicKind::Unknown;
}

const wchar_t* PicKindName(PicKind k) {
    switch (k) {
        case PicKind::Png: return L"PNG";
        case PicKind::Bmp: return L"BMP";
        case PicKind::Tga: return L"TGA";
        case PicKind::Dds: return L"DDS";
        case PicKind::Ddj: return L"DDJ";
        case PicKind::Other: return L"WIC";
        default: return L"?";
    }
}

bool DecodePicture(const std::uint8_t* p, std::size_t n, Image& out, std::wstring* why, const std::wstring& hint, bool useWic, PicKind* kind) {
    const PicKind k = SniffPicture(p, n, hint);
    if (kind) *kind = k;
    switch (k) {
        case PicKind::Png: return DecodePng(p, n, out, why);
        case PicKind::Bmp: return DecodeBmp(p, n, out, why);
        case PicKind::Tga: return DecodeTga(p, n, out, why);
        case PicKind::Dds:
        case PicKind::Ddj: {
            std::string e;
            if (se::render::DecodeDdj(p, n, out, e)) return true;
            Why(why, L"DDS / DDJ çözülemedi: " + std::wstring(e.begin(), e.end()));
            return false;
        }
        case PicKind::Other:
            if (useWic) return DecodeWic(p, n, out, why);
            Why(why, L"resim biçimi tanınmadı");
            return false;
        default: Why(why, L"dosya boş"); return false;
    }
}

bool ReadFileBytes(const std::wstring& path, Bytes& out, std::wstring* why, std::size_t maxBytes) {
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Why(why, L"dosya açılamadı: " + path); return false; }
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(f, &sz) && static_cast<unsigned long long>(sz.QuadPart) <= maxBytes;
    if (ok) {
        out.resize(static_cast<std::size_t>(sz.QuadPart));
        std::size_t got = 0;
        while (ok && got < out.size()) {
            DWORD r = 0;
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - got, 1u << 30));
            ok = ReadFile(f, out.data() + got, want, &r, nullptr) && r > 0;
            got += r;
        }
    }
    CloseHandle(f);
    if (!ok) { out.clear(); Why(why, L"dosya okunamadı (ya da çok büyük): " + path); }
    return ok;
}

bool LoadPicture(const std::wstring& path, Image& out, std::wstring* why, PicKind* kind) {
    Bytes b;
    if (!ReadFileBytes(path, b, why)) return false;
    return DecodePicture(b.data(), b.size(), out, why, path, true, kind);
}

bool EncodePngRgba(const Image& img, Bytes& out) { return se::render::EncodePng(img, out, false); }

namespace {
bool MakeDirs(const std::wstring& dir) {
    if (dir.empty()) return true;
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const auto s = dir.find_last_of(L"\\/");
    if (s != std::wstring::npos && s > 2 && !MakeDirs(dir.substr(0, s))) return false;
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}
}  // namespace

bool WriteFileAtomic(const std::wstring& path, const Bytes& data, std::wstring* why, bool backup) {
    const auto s = path.find_last_of(L"\\/");
    if (s != std::wstring::npos && !MakeDirs(path.substr(0, s))) { Why(why, L"klasör oluşturulamadı: " + path.substr(0, s)); return false; }
    const std::wstring tmp = path + L".tmp" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetCurrentThreadId());
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Why(why, L"geçici dosya yazılamadı: " + tmp); return false; }
    bool ok = true;
    std::size_t done = 0;
    while (ok && done < data.size()) {
        DWORD wr = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 30));
        ok = WriteFile(f, data.data() + done, want, &wr, nullptr) && wr == want;
        done += wr;
    }
    ok = ok && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok && backup && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) ok = CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE) != 0;
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        DeleteFileW(tmp.c_str());
        Why(why, L"dosya yazılamadı: " + path);
    }
    return ok;
}

// ------------------------------------------------------------------------------------------------ helpers
Image MakeImage(int w, int h, std::uint32_t rgba) {
    Image im;
    im.w = std::max(0, w);
    im.h = std::max(0, h);
    im.rgba.resize(static_cast<std::size_t>(im.w) * im.h * 4);
    for (std::size_t i = 0; i < im.rgba.size(); i += 4) {
        im.rgba[i] = static_cast<std::uint8_t>(rgba & 0xFF);
        im.rgba[i + 1] = static_cast<std::uint8_t>((rgba >> 8) & 0xFF);
        im.rgba[i + 2] = static_cast<std::uint8_t>((rgba >> 16) & 0xFF);
        im.rgba[i + 3] = static_cast<std::uint8_t>(rgba >> 24);
    }
    return im;
}

namespace {
struct Contrib { int first = 0; std::vector<float> w; };

float Kern(Filter f, float x) {
    x = std::fabs(x);
    switch (f) {
        case Filter::Triangle: return x < 1 ? 1 - x : 0;
        case Filter::Mitchell:
            if (x < 1) return (7 * x * x * x - 12 * x * x + 16.0f / 3) / 6;
            if (x < 2) return (-7.0f / 3 * x * x * x + 12 * x * x - 20 * x + 32.0f / 3) / 6;
            return 0;
        case Filter::Lanczos3: {
            if (x < 1e-6f) return 1;
            if (x >= 3) return 0;
            const float a = 3.14159265f * x;
            return 3 * std::sin(a) * std::sin(a / 3) / (a * a);
        }
        default: return x < 0.5f ? 1.0f : 0.0f;
    }
}

std::vector<Contrib> Contribs(int src, int dst, Filter f) {
    std::vector<Contrib> c(dst);
    const double s = static_cast<double>(src) / dst;
    for (int i = 0; i < dst; ++i) {
        Contrib& k = c[i];
        if (f == Filter::Box) {   // exact area coverage
            const double a = i * s, b = (i + 1) * s;
            const int j0 = static_cast<int>(std::floor(a)), j1 = std::min(src - 1, static_cast<int>(std::ceil(b)) - 1);
            k.first = std::max(0, j0);
            for (int j = k.first; j <= std::max(k.first, j1); ++j) {
                const double ov = std::min(b, j + 1.0) - std::max(a, static_cast<double>(j));
                k.w.push_back(static_cast<float>(std::max(0.0, ov)));
            }
        } else {
            const double fs = std::max(1.0, s);
            const double sup = (f == Filter::Triangle ? 1.0 : f == Filter::Mitchell ? 2.0 : 3.0) * fs;
            const double ctr = (i + 0.5) * s - 0.5;
            const int j0 = std::max(0, static_cast<int>(std::floor(ctr - sup))), j1 = std::min(src - 1, static_cast<int>(std::ceil(ctr + sup)));
            k.first = j0;
            for (int j = j0; j <= j1; ++j) k.w.push_back(Kern(f, static_cast<float>((j - ctr) / fs)));
        }
        float sum = 0;
        for (float v : k.w) sum += v;
        if (std::fabs(sum) < 1e-8f) {
            k.first = std::clamp(static_cast<int>((i + 0.5) * s), 0, src - 1);
            k.w.assign(1, 1.0f);
            sum = 1;
        }
        for (float& v : k.w) v /= sum;
    }
    return c;
}
}  // namespace

Image Resample(const Image& src, int w, int h, Filter f) {
    if (src.Empty() || w <= 0 || h <= 0) return MakeImage(std::max(0, w), std::max(0, h));
    if (w == src.w && h == src.h) return src;
    const int sw = src.w, sh = src.h;
    // 7 channels: premultiplied r, g, b, alpha, then plain r, g, b (used where the footprint has no alpha at all,
    // so fully transparent areas keep their colour instead of turning black - matters for alpha used as a mask)
    constexpr int C = 7;
    std::vector<float> pm(static_cast<std::size_t>(sw) * sh * C);
    for (std::size_t i = 0, j = 0; i < src.rgba.size(); i += 4, j += C) {
        const float a = src.rgba[i + 3] / 255.0f;
        for (int ch = 0; ch < 3; ++ch) { pm[j + ch] = src.rgba[i + ch] * a; pm[j + 4 + ch] = src.rgba[i + ch]; }
        pm[j + 3] = a * 255.0f;
    }
    const auto cx = Contribs(sw, w, f), cy = Contribs(sh, h, f);
    std::vector<float> tmp(static_cast<std::size_t>(w) * sh * C, 0.0f);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < w; ++x) {
            const Contrib& c = cx[x];
            float* acc = &tmp[(static_cast<std::size_t>(y) * w + x) * C];
            for (std::size_t k = 0; k < c.w.size(); ++k) {
                const float* s = &pm[(static_cast<std::size_t>(y) * sw + c.first + k) * C];
                for (int ch = 0; ch < C; ++ch) acc[ch] += s[ch] * c.w[k];
            }
        }
    Image out;
    out.w = w;
    out.h = h;
    out.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const Contrib& c = cy[y];
        for (int x = 0; x < w; ++x) {
            float acc[C] = {};
            for (std::size_t k = 0; k < c.w.size(); ++k) {
                const float* s = &tmp[((c.first + k) * w + x) * C];
                for (int ch = 0; ch < C; ++ch) acc[ch] += s[ch] * c.w[k];
            }
            std::uint8_t* o = out.Px(x, y);
            const float a = std::clamp(acc[3], 0.0f, 255.0f);
            o[3] = static_cast<std::uint8_t>(a + 0.5f);
            for (int ch = 0; ch < 3; ++ch) {
                const float v = a > 0.5f ? acc[ch] * 255.0f / a : acc[4 + ch];
                o[ch] = static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
            }
        }
    }
    return out;
}

Image Crop(const Image& img, int x, int y, int w, int h) {
    Image o = MakeImage(w, h, 0);
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            const int sx = x + i, sy = y + j;
            if (sx >= 0 && sy >= 0 && sx < img.w && sy < img.h) std::memcpy(o.Px(i, j), img.Px(sx, sy), 4);
        }
    return o;
}

void BlitOver(Image& dst, const Image& src, int x, int y) {
    for (int j = 0; j < src.h; ++j)
        for (int i = 0; i < src.w; ++i) {
            const int dx = x + i, dy = y + j;
            if (dx < 0 || dy < 0 || dx >= dst.w || dy >= dst.h) continue;
            const std::uint8_t* s = src.Px(i, j);
            std::uint8_t* d = dst.Px(dx, dy);
            const float sa = s[3] / 255.0f, da = d[3] / 255.0f;
            const float oa = sa + da * (1 - sa);
            if (oa <= 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
            for (int c = 0; c < 3; ++c) d[c] = static_cast<std::uint8_t>(std::clamp((s[c] * sa + d[c] * da * (1 - sa)) / oa, 0.0f, 255.0f) + 0.5f);
            d[3] = static_cast<std::uint8_t>(oa * 255.0f + 0.5f);
        }
}

void FillRect(Image& img, int x, int y, int w, int h, std::uint32_t rgba) {
    for (int j = std::max(0, y); j < std::min(img.h, y + h); ++j)
        for (int i = std::max(0, x); i < std::min(img.w, x + w); ++i) {
            std::uint8_t* d = img.Px(i, j);
            d[0] = static_cast<std::uint8_t>(rgba & 0xFF);
            d[1] = static_cast<std::uint8_t>((rgba >> 8) & 0xFF);
            d[2] = static_cast<std::uint8_t>((rgba >> 16) & 0xFF);
            d[3] = static_cast<std::uint8_t>(rgba >> 24);
        }
}

Image FitInto(const Image& src, int w, int h, Fit fit, Filter f) {
    if (src.Empty() || w <= 0 || h <= 0) return MakeImage(std::max(0, w), std::max(0, h));
    if (fit == Fit::Stretch) return Resample(src, w, h, f);
    const double sx = static_cast<double>(w) / src.w, sy = static_cast<double>(h) / src.h;
    const double s = fit == Fit::Contain ? std::min(sx, sy) : std::max(sx, sy);
    const int nw = std::max(1, static_cast<int>(std::lround(src.w * s))), nh = std::max(1, static_cast<int>(std::lround(src.h * s)));
    const Image r = Resample(src, nw, nh, f);
    if (fit == Fit::Cover) return Crop(r, (nw - w) / 2, (nh - h) / 2, w, h);
    Image o = MakeImage(w, h, 0);
    const int ox = (w - nw) / 2, oy = (h - nh) / 2;
    for (int j = 0; j < nh; ++j)
        for (int i = 0; i < nw; ++i)
            if (ox + i >= 0 && oy + j >= 0 && ox + i < w && oy + j < h) std::memcpy(o.Px(ox + i, oy + j), r.Px(i, j), 4);
    return o;
}

bool AlphaBounds(const Image& img, int& x0, int& y0, int& x1, int& y1, int thr) {
    x0 = img.w; y0 = img.h; x1 = -1; y1 = -1;
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
            if (img.Px(x, y)[3] > thr) {
                x0 = std::min(x0, x); x1 = std::max(x1, x);
                y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
    return x1 >= 0;
}

bool HasAlpha(const Image& img, bool* binary) {
    bool any = false, bin = true;
    for (std::size_t i = 3; i < img.rgba.size(); i += 4) {
        const std::uint8_t a = img.rgba[i];
        if (a != 255) any = true;
        if (a != 0 && a != 255) bin = false;
    }
    if (binary) *binary = bin;
    return any;
}

double Psnr(const Image& a, const Image& b, unsigned ch, double* mseOut) {
    if (a.w != b.w || a.h != b.h || a.Empty()) { if (mseOut) *mseOut = -1; return 0; }
    double se = 0;
    std::size_t cnt = 0;
    for (std::size_t i = 0; i < a.rgba.size(); i += 4)
        for (int c = 0; c < 4; ++c) {
            if (!(ch & (1u << c))) continue;
            const double d = static_cast<double>(a.rgba[i + c]) - b.rgba[i + c];
            se += d * d;
            ++cnt;
        }
    const double mse = cnt ? se / cnt : 0;
    if (mseOut) *mseOut = mse;
    if (mse <= 1e-12) return 99.0;
    return std::min(99.0, 10.0 * std::log10(255.0 * 255.0 / mse));
}

}  // namespace se::ddj
