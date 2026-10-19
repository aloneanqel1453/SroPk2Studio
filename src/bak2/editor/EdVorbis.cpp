// SkillEffect Studio - editor/EdVorbis.cpp  (Vorbis I decoder, see EdVorbis.h)
#include "EdVorbis.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <map>
#include <random>

namespace se::ed {

namespace {

constexpr double kPi = 3.14159265358979323846;
using cplx = std::complex<double>;

int ILog(std::uint32_t v) {
    int r = 0;
    while (v) {
        ++r;
        v >>= 1;
    }
    return r;
}

float Float32Unpack(std::uint32_t x) {
    const double mant = static_cast<double>(x & 0x1fffff);
    const int ex = static_cast<int>((x & 0x7fe00000u) >> 21);
    const double v = std::ldexp(mant, ex - 788);
    return static_cast<float>((x & 0x80000000u) ? -v : v);
}

std::uint32_t Lookup1Values(std::uint32_t entries, std::uint32_t dims) {
    if (dims == 0) return 0;
    auto pw = [&](std::uint32_t b) {
        double p = 1;
        for (std::uint32_t i = 0; i < dims; ++i) p *= b;
        return p;
    };
    std::uint32_t r = static_cast<std::uint32_t>(std::floor(std::exp(std::log(static_cast<double>(std::max<std::uint32_t>(entries, 1))) / dims)));
    while (pw(r + 1) <= entries) ++r;
    while (r > 0 && pw(r) > entries) --r;
    return r;
}

std::uint32_t BitReverse(std::uint32_t n) {
    n = ((n & 0xAAAAAAAAu) >> 1) | ((n & 0x55555555u) << 1);
    n = ((n & 0xCCCCCCCCu) >> 2) | ((n & 0x33333333u) << 2);
    n = ((n & 0xF0F0F0F0u) >> 4) | ((n & 0x0F0F0F0Fu) << 4);
    n = ((n & 0xFF00FF00u) >> 8) | ((n & 0x00FF00FFu) << 8);
    return (n >> 16) | (n << 16);
}

struct BitReader {
    const std::uint8_t* p;
    size_t n;
    size_t pos = 0;
    bool eop = false;
    BitReader(const std::uint8_t* d, size_t len) : p(d), n(len) {}
    int Bit() {
        const size_t byte = pos >> 3;
        if (byte >= n) {
            eop = true;
            return 0;
        }
        const int b = (p[byte] >> (pos & 7)) & 1;
        ++pos;
        return b;
    }
    std::uint32_t Read(int bits) {
        std::uint32_t v = 0;
        for (int i = 0; i < bits; ++i) v |= static_cast<std::uint32_t>(Bit()) << i;
        return v;
    }
};

struct Codebook {
    std::uint32_t dims = 0, entries = 0;
    std::vector<std::uint8_t> len;
    struct Node {
        int c[2] = {-1, -1};  // -1 empty, >= 0 node, <= -2 leaf (entry = -(v + 2))
    };
    std::vector<Node> nodes;
    int single = -1;  // the only used entry (a one-entry codebook)
    std::vector<float> vq;  // entries * dims (lookup types 1 / 2)

    bool Insert(std::uint32_t code, int l, int entry) {
        int node = 0;
        for (int i = 0; i < l; ++i) {
            const int b = (code >> i) & 1;
            if (i == l - 1) {
                if (nodes[static_cast<size_t>(node)].c[b] != -1) return false;
                nodes[static_cast<size_t>(node)].c[b] = -(entry + 2);
            } else {
                int nx = nodes[static_cast<size_t>(node)].c[b];
                if (nx == -1) {
                    nodes.push_back(Node{});
                    nx = static_cast<int>(nodes.size()) - 1;
                    nodes[static_cast<size_t>(node)].c[b] = nx;
                } else if (nx < 0) {
                    return false;
                }
                node = nx;
            }
        }
        return true;
    }
    bool BuildTree() {
        nodes.assign(1, Node{});
        std::uint32_t available[33] = {};
        int used = 0, k = -1;
        for (std::uint32_t i = 0; i < entries; ++i)
            if (len[i]) {
                ++used;
                if (k < 0) k = static_cast<int>(i);
            }
        if (k < 0) return true;  // no used entry
        if (used == 1) single = k;
        if (!Insert(0, len[static_cast<size_t>(k)], k)) return false;
        for (int i = 1; i <= len[static_cast<size_t>(k)]; ++i) available[i] = 1u << (32 - i);
        for (std::uint32_t i = static_cast<std::uint32_t>(k) + 1; i < entries; ++i) {
            int z = len[i];
            if (!z) continue;
            while (z > 0 && !available[z]) --z;
            if (z == 0) return false;  // over-specified tree
            const std::uint32_t res = available[z];
            available[z] = 0;
            if (!Insert(BitReverse(res), len[i], static_cast<int>(i))) return false;
            if (z != len[i])
                for (int y = len[i]; y > z; --y) available[y] = res + (1u << (32 - y));
        }
        return true;
    }
    int Decode(BitReader& br) const {
        if (nodes.empty()) return -1;
        int node = 0;
        for (int depth = 0; depth < 33; ++depth) {
            const int b = br.Bit();
            if (br.eop) return -1;
            const int v = nodes[static_cast<size_t>(node)].c[b];
            if (v == -1) return single;
            if (v <= -2) return -(v + 2);
            node = v;
        }
        return -1;
    }
};

struct Floor1 {
    int partitions = 0;
    std::vector<int> partClass;
    int classDims[16]{}, classSub[16]{}, classMaster[16]{};
    int subBooks[16][8]{};
    int multiplier = 1, rangebits = 0;
    std::vector<int> X, order, lowN, highN;
};

struct Residue {
    int type = 0;
    std::uint32_t begin = 0, end = 0, partSize = 1;
    int classifications = 1, classbook = 0;
    int books[64][8]{};
};

struct Mapping {
    int submaps = 1;
    std::vector<int> mag, ang, mux;
    int floorOf[16]{}, residueOf[16]{};
};

struct Mode {
    int blockflag = 0, mapping = 0;
};

// IMDCT through DCT-IV of length M = N/2 (one N/4-point complex FFT); y = 2 * M outputs
struct Imdct {
    int N = 0, M = 0, Q = 0;
    std::vector<cplx> tw1, tw2, fftTw;
    std::vector<int> rev;
    explicit Imdct(int n) : N(n), M(n / 2), Q(n / 4) {
        tw1.resize(static_cast<size_t>(Q));
        tw2.resize(static_cast<size_t>(Q));
        for (int k = 0; k < Q; ++k) {
            tw1[static_cast<size_t>(k)] = std::polar(1.0, -kPi * k / M);
            tw2[static_cast<size_t>(k)] = std::polar(1.0, -kPi * (k + 0.25) / M);
        }
        rev.resize(static_cast<size_t>(Q));
        const int bits = ILog(static_cast<std::uint32_t>(Q)) - 1;
        for (int i = 0; i < Q; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[static_cast<size_t>(i)] = r;
        }
        fftTw.resize(static_cast<size_t>(Q / 2 > 0 ? Q / 2 : 1));
        for (int i = 0; i < Q / 2; ++i) fftTw[static_cast<size_t>(i)] = std::polar(1.0, -2 * kPi * i / Q);
    }
    void Fft(std::vector<cplx>& a) const {
        for (int i = 0; i < Q; ++i)
            if (i < rev[static_cast<size_t>(i)]) std::swap(a[static_cast<size_t>(i)], a[static_cast<size_t>(rev[static_cast<size_t>(i)])]);
        for (int len = 2; len <= Q; len <<= 1) {
            const int step = Q / len;
            for (int i = 0; i < Q; i += len)
                for (int j = 0; j < len / 2; ++j) {
                    const cplx w = fftTw[static_cast<size_t>(j * step)];
                    const cplx u = a[static_cast<size_t>(i + j)], v = a[static_cast<size_t>(i + j + len / 2)] * w;
                    a[static_cast<size_t>(i + j)] = u + v;
                    a[static_cast<size_t>(i + j + len / 2)] = u - v;
                }
        }
    }
    void Run(const float* X, float* y) const {
        std::vector<cplx> w(static_cast<size_t>(Q));
        for (int k = 0; k < Q; ++k) w[static_cast<size_t>(k)] = cplx(X[2 * k], X[M - 1 - 2 * k]) * tw1[static_cast<size_t>(k)];
        Fft(w);
        std::vector<double> u(static_cast<size_t>(M));
        for (int n = 0; n < Q; ++n) {
            const cplx c = w[static_cast<size_t>(n)] * tw2[static_cast<size_t>(n)];
            u[static_cast<size_t>(2 * n)] = c.real();
            u[static_cast<size_t>(M - 1 - 2 * n)] = -c.imag();
        }
        for (int i = 0; i < N; ++i) {
            const int m = i + N / 4;
            double v;
            if (m < M) v = u[static_cast<size_t>(m)];
            else if (m < 2 * M) v = -u[static_cast<size_t>(2 * M - 1 - m)];
            else v = -u[static_cast<size_t>(m - 2 * M)];
            y[i] = static_cast<float>(v);
        }
    }
};

struct Decoder {
    int channels = 0, rate = 0, bs[2]{};
    std::vector<Codebook> books;
    std::vector<Floor1> floors;
    std::vector<Residue> residues;
    std::vector<Mapping> maps;
    std::vector<Mode> modes;
    std::map<int, std::unique_ptr<Imdct>> imdct;
    std::map<int, std::vector<float>> windows;  // key = n * 4 + prevLong * 2 + nextLong
    float invdB[256]{};
    std::vector<std::vector<float>> prevRight;
    int prevN = 0;
    bool havePrev = false;
    std::wstring err;

    Decoder() {
        for (int i = 0; i < 256; ++i) invdB[i] = static_cast<float>(1.0649863e-07 * std::pow(1.0 / 1.0649863e-07, i / 255.0));
    }

    bool Ident(const Bytes& p) {
        if (p.size() < 30 || p[0] != 1 || std::memcmp(p.data() + 1, "vorbis", 6) != 0) return false;
        BitReader br(p.data() + 7, p.size() - 7);
        if (br.Read(32) != 0) return false;
        channels = static_cast<int>(br.Read(8));
        rate = static_cast<int>(br.Read(32));
        br.Read(32);
        br.Read(32);
        br.Read(32);
        bs[0] = 1 << br.Read(4);
        bs[1] = 1 << br.Read(4);
        return channels > 0 && channels <= 8 && rate > 0 && bs[0] >= 64 && bs[1] >= bs[0] && bs[1] <= 8192;
    }

    bool Setup(const Bytes& p) {
        if (p.size() < 8 || p[0] != 5 || std::memcmp(p.data() + 1, "vorbis", 6) != 0) return false;
        BitReader br(p.data() + 7, p.size() - 7);
        // codebooks
        const int nb = static_cast<int>(br.Read(8)) + 1;
        books.resize(static_cast<size_t>(nb));
        for (auto& cb : books) {
            if (br.Read(24) != 0x564342) {
                err = L"codebook sync";
                return false;
            }
            cb.dims = br.Read(16);
            cb.entries = br.Read(24);
            cb.len.assign(cb.entries, 0);
            if (br.Read(1) == 0) {
                const bool sparse = br.Read(1) != 0;
                for (std::uint32_t i = 0; i < cb.entries; ++i) {
                    if (sparse && !br.Read(1)) continue;
                    cb.len[i] = static_cast<std::uint8_t>(br.Read(5) + 1);
                }
            } else {
                std::uint32_t cur = 0;
                int l = static_cast<int>(br.Read(5)) + 1;
                while (cur < cb.entries) {
                    const std::uint32_t num = br.Read(ILog(cb.entries - cur));
                    if (num > cb.entries - cur || l > 32) {
                        err = L"codebook lengths";
                        return false;
                    }
                    for (std::uint32_t i = 0; i < num; ++i) cb.len[cur + i] = static_cast<std::uint8_t>(l);
                    cur += num;
                    ++l;
                }
            }
            const int lt = static_cast<int>(br.Read(4));
            if (lt == 1 || lt == 2) {
                const float mn = Float32Unpack(br.Read(32)), delta = Float32Unpack(br.Read(32));
                const int vbits = static_cast<int>(br.Read(4)) + 1;
                const bool seq = br.Read(1) != 0;
                const std::uint32_t lv = lt == 1 ? Lookup1Values(cb.entries, cb.dims) : cb.entries * cb.dims;
                std::vector<std::uint32_t> mult(lv);
                for (auto& m : mult) m = br.Read(vbits);
                cb.vq.assign(static_cast<size_t>(cb.entries) * cb.dims, 0.f);
                for (std::uint32_t e = 0; e < cb.entries; ++e) {
                    float last = 0;
                    std::uint32_t div = 1;
                    for (std::uint32_t i = 0; i < cb.dims; ++i) {
                        const std::uint32_t off = lt == 1 ? (e / div) % std::max<std::uint32_t>(lv, 1) : e * cb.dims + i;
                        const float v = static_cast<float>(mult[off]) * delta + mn + last;
                        if (seq) last = v;
                        cb.vq[static_cast<size_t>(e) * cb.dims + i] = v;
                        if (lt == 1) div *= lv;
                    }
                }
            } else if (lt != 0) {
                err = L"codebook lookup type";
                return false;
            }
            if (!cb.BuildTree()) {
                err = L"codebook tree";
                return false;
            }
            if (br.eop) {
                err = L"setup truncated";
                return false;
            }
        }
        // time domain transforms (placeholders)
        const int nt = static_cast<int>(br.Read(6)) + 1;
        for (int i = 0; i < nt; ++i)
            if (br.Read(16) != 0) {
                err = L"time transform";
                return false;
            }
        // floors
        const int nf = static_cast<int>(br.Read(6)) + 1;
        floors.resize(static_cast<size_t>(nf));
        for (auto& f : floors) {
            const std::uint32_t type = br.Read(16);
            if (type != 1) {
                err = L"floor 0 (eski kodlayıcı) desteklenmiyor / floor 0 unsupported";
                return false;
            }
            f.partitions = static_cast<int>(br.Read(5));
            int maxClass = -1;
            f.partClass.resize(static_cast<size_t>(f.partitions));
            for (auto& c : f.partClass) {
                c = static_cast<int>(br.Read(4));
                maxClass = std::max(maxClass, c);
            }
            for (int c = 0; c <= maxClass; ++c) {
                f.classDims[c] = static_cast<int>(br.Read(3)) + 1;
                f.classSub[c] = static_cast<int>(br.Read(2));
                if (f.classSub[c]) f.classMaster[c] = static_cast<int>(br.Read(8));
                for (int j = 0; j < (1 << f.classSub[c]); ++j) f.subBooks[c][j] = static_cast<int>(br.Read(8)) - 1;
            }
            f.multiplier = static_cast<int>(br.Read(2)) + 1;
            f.rangebits = static_cast<int>(br.Read(4));
            f.X = {0, 1 << f.rangebits};
            for (int pi = 0; pi < f.partitions; ++pi) {
                const int c = f.partClass[static_cast<size_t>(pi)];
                for (int j = 0; j < f.classDims[c]; ++j) f.X.push_back(static_cast<int>(br.Read(f.rangebits)));
            }
            if (f.X.size() > 65) {
                err = L"floor values";
                return false;
            }
            const int nv = static_cast<int>(f.X.size());
            f.order.resize(static_cast<size_t>(nv));
            for (int i = 0; i < nv; ++i) f.order[static_cast<size_t>(i)] = i;
            std::stable_sort(f.order.begin(), f.order.end(), [&](int a, int b) { return f.X[static_cast<size_t>(a)] < f.X[static_cast<size_t>(b)]; });
            f.lowN.assign(static_cast<size_t>(nv), 0);
            f.highN.assign(static_cast<size_t>(nv), 1);
            for (int i = 2; i < nv; ++i) {
                int lo = 0, hi = 1, lx = -1, hx = 1 << 30;
                for (int j = 0; j < i; ++j) {
                    const int x = f.X[static_cast<size_t>(j)];
                    if (x < f.X[static_cast<size_t>(i)] && x > lx) {
                        lx = x;
                        lo = j;
                    }
                    if (x > f.X[static_cast<size_t>(i)] && x < hx) {
                        hx = x;
                        hi = j;
                    }
                }
                f.lowN[static_cast<size_t>(i)] = lo;
                f.highN[static_cast<size_t>(i)] = hi;
            }
        }
        // residues
        const int nr = static_cast<int>(br.Read(6)) + 1;
        residues.resize(static_cast<size_t>(nr));
        for (auto& r : residues) {
            r.type = static_cast<int>(br.Read(16));
            if (r.type > 2) {
                err = L"residue type";
                return false;
            }
            r.begin = br.Read(24);
            r.end = br.Read(24);
            r.partSize = br.Read(24) + 1;
            r.classifications = static_cast<int>(br.Read(6)) + 1;
            r.classbook = static_cast<int>(br.Read(8));
            int cascade[64]{};
            for (int c = 0; c < r.classifications; ++c) {
                int low = static_cast<int>(br.Read(3));
                const int high = br.Read(1) ? static_cast<int>(br.Read(5)) : 0;
                cascade[c] = high * 8 + low;
            }
            for (int c = 0; c < r.classifications; ++c)
                for (int j = 0; j < 8; ++j) r.books[c][j] = (cascade[c] & (1 << j)) ? static_cast<int>(br.Read(8)) : -1;
        }
        // mappings
        const int nm = static_cast<int>(br.Read(6)) + 1;
        maps.resize(static_cast<size_t>(nm));
        for (auto& m : maps) {
            if (br.Read(16) != 0) {
                err = L"mapping type";
                return false;
            }
            m.submaps = br.Read(1) ? static_cast<int>(br.Read(4)) + 1 : 1;
            if (br.Read(1)) {
                const int steps = static_cast<int>(br.Read(8)) + 1;
                const int bits = ILog(static_cast<std::uint32_t>(channels - 1));
                for (int s = 0; s < steps; ++s) {
                    m.mag.push_back(static_cast<int>(br.Read(bits)));
                    m.ang.push_back(static_cast<int>(br.Read(bits)));
                }
            }
            if (br.Read(2) != 0) {
                err = L"mapping reserved";
                return false;
            }
            m.mux.assign(static_cast<size_t>(channels), 0);
            if (m.submaps > 1)
                for (auto& x : m.mux) x = static_cast<int>(br.Read(4));
            for (int s = 0; s < m.submaps; ++s) {
                br.Read(8);
                m.floorOf[s] = static_cast<int>(br.Read(8));
                m.residueOf[s] = static_cast<int>(br.Read(8));
            }
        }
        // modes
        const int nmo = static_cast<int>(br.Read(6)) + 1;
        modes.resize(static_cast<size_t>(nmo));
        for (auto& mo : modes) {
            mo.blockflag = static_cast<int>(br.Read(1));
            br.Read(16);
            br.Read(16);
            mo.mapping = static_cast<int>(br.Read(8));
        }
        if (!br.Read(1) || br.eop) {
            err = L"setup framing";
            return false;
        }
        // bounds of every reference
        for (auto& m : maps)
            for (int s = 0; s < m.submaps; ++s)
                if (m.floorOf[s] >= nf || m.residueOf[s] >= nr) return false;
        for (auto& r : residues)
            if (r.classbook >= nb) return false;
        for (auto& mo : modes)
            if (mo.mapping >= nm) return false;
        return true;
    }

    const std::vector<float>& Window(int n, bool prevLong, bool nextLong) {
        const int key = n * 4 + (prevLong ? 2 : 0) + (nextLong ? 1 : 0);
        auto it = windows.find(key);
        if (it != windows.end()) return it->second;
        std::vector<float> w(static_cast<size_t>(n), 0.f);
        const bool longBlock = n == bs[1] && bs[1] != bs[0];
        int ls, le, ln, rs, re, rn;
        if (longBlock && !prevLong) {
            ls = n / 4 - bs[0] / 4;
            le = n / 4 + bs[0] / 4;
            ln = bs[0] / 2;
        } else {
            ls = 0;
            le = n / 2;
            ln = n / 2;
        }
        if (longBlock && !nextLong) {
            rs = n * 3 / 4 - bs[0] / 4;
            re = n * 3 / 4 + bs[0] / 4;
            rn = bs[0] / 2;
        } else {
            rs = n / 2;
            re = n;
            rn = n / 2;
        }
        for (int i = ls; i < le; ++i) {
            const double s = std::sin((i - ls + 0.5) / ln * kPi / 2);
            w[static_cast<size_t>(i)] = static_cast<float>(std::sin(kPi / 2 * s * s));
        }
        for (int i = le; i < rs; ++i) w[static_cast<size_t>(i)] = 1.f;
        for (int i = rs; i < re; ++i) {
            const double s = std::sin((i - rs + 0.5) / rn * kPi / 2 + kPi / 2);
            w[static_cast<size_t>(i)] = static_cast<float>(std::sin(kPi / 2 * s * s));
        }
        return windows.emplace(key, std::move(w)).first->second;
    }

    static int RenderPoint(int x0, int y0, int x1, int y1, int x) {
        const int dy = y1 - y0, adx = x1 - x0, ady = std::abs(dy);
        const int err2 = ady * (x - x0);
        const int off = adx ? err2 / adx : 0;
        return dy < 0 ? y0 - off : y0 + off;
    }

    void RenderLine(int x0, int y0, int x1, int y1, float* v, int n2) const {
        const int dy = y1 - y0, adx = x1 - x0;
        if (adx <= 0) return;
        int ady = std::abs(dy);
        const int base = dy / adx;
        const int sy = dy < 0 ? base - 1 : base + 1;
        int x = x0, y = y0, e = 0;
        ady -= std::abs(base) * adx;
        if (x < n2) v[x] = invdB[std::clamp(y, 0, 255)];
        for (x = x0 + 1; x < x1; ++x) {
            e += ady;
            if (e >= adx) {
                e -= adx;
                y += sy;
            } else {
                y += base;
            }
            if (x < n2) v[x] = invdB[std::clamp(y, 0, 255)];
        }
    }

    // decode one partition / vector into v (format 0 = interleaved steps, 1 = consecutive)
    bool DecodeVQ(const Codebook& cb, BitReader& br, float* v, int count, int format) const {
        if (cb.vq.empty() || cb.dims == 0) return false;
        const int dims = static_cast<int>(cb.dims);
        if (format == 0) {
            const int step = count / dims;
            for (int i = 0; i < step; ++i) {
                const int e = cb.Decode(br);
                if (e < 0) return false;
                const float* q = &cb.vq[static_cast<size_t>(e) * cb.dims];
                for (int j = 0; j < dims; ++j) v[i + j * step] += q[j];
            }
        } else {
            for (int i = 0; i < count;) {
                const int e = cb.Decode(br);
                if (e < 0) return false;
                const float* q = &cb.vq[static_cast<size_t>(e) * cb.dims];
                for (int j = 0; j < dims && i < count; ++j) v[i++] += q[j];
            }
        }
        return true;
    }

    void DecodeResidue(const Residue& r, BitReader& br, std::vector<float*>& vecs, const std::vector<bool>& dnd, int n2) const {
        const int ch = static_cast<int>(vecs.size());
        const Codebook& cbk = books[static_cast<size_t>(r.classbook)];
        const int cpc = static_cast<int>(cbk.dims);
        if (cpc <= 0) return;
        if (r.type == 2) {
            bool any = false;
            for (int c = 0; c < ch; ++c) any = any || !dnd[static_cast<size_t>(c)];
            if (!any) return;
            std::vector<float> big(static_cast<size_t>(n2) * ch, 0.f);
            std::vector<float*> one{big.data()};
            Residue r1 = r;
            r1.type = 1;
            DecodeResidueCore(r1, br, one, {false}, n2 * ch, cbk, cpc);
            for (int i = 0; i < n2; ++i)
                for (int c = 0; c < ch; ++c) vecs[static_cast<size_t>(c)][i] += big[static_cast<size_t>(i) * ch + c];
            return;
        }
        DecodeResidueCore(r, br, vecs, dnd, n2, cbk, cpc);
    }

    void DecodeResidueCore(const Residue& r, BitReader& br, std::vector<float*>& vecs, const std::vector<bool>& dnd, int size,
                           const Codebook& cbk, int cpc) const {
        const int ch = static_cast<int>(vecs.size());
        const int lb = static_cast<int>(std::min<std::uint32_t>(r.begin, static_cast<std::uint32_t>(size)));
        const int le = static_cast<int>(std::min<std::uint32_t>(r.end, static_cast<std::uint32_t>(size)));
        const int ps = static_cast<int>(r.partSize);
        const int nread = le - lb;
        if (nread <= 0 || ps <= 0) return;
        const int parts = nread / ps;
        std::vector<std::vector<int>> cls(static_cast<size_t>(ch), std::vector<int>(static_cast<size_t>(parts + cpc), 0));
        for (int pass = 0; pass < 8; ++pass) {
            int pc = 0;
            while (pc < parts) {
                if (pass == 0)
                    for (int c = 0; c < ch; ++c) {
                        if (dnd[static_cast<size_t>(c)]) continue;
                        int temp = cbk.Decode(br);
                        if (temp < 0) return;
                        for (int i = cpc - 1; i >= 0; --i) {
                            if (pc + i < parts + cpc) cls[static_cast<size_t>(c)][static_cast<size_t>(pc + i)] = temp % r.classifications;
                            temp /= r.classifications;
                        }
                    }
                for (int i = 0; i < cpc && pc < parts; ++i, ++pc)
                    for (int c = 0; c < ch; ++c) {
                        if (dnd[static_cast<size_t>(c)]) continue;
                        const int vc = cls[static_cast<size_t>(c)][static_cast<size_t>(pc)];
                        const int book = r.books[vc][pass];
                        if (book < 0 || book >= static_cast<int>(books.size())) continue;
                        if (!DecodeVQ(books[static_cast<size_t>(book)], br, vecs[static_cast<size_t>(c)] + lb + pc * ps, ps, r.type == 0 ? 0 : 1))
                            return;  // end of packet: the rest stays zero
                    }
            }
        }
    }

    // one audio packet -> appends finished samples (per channel)
    bool Packet(const Bytes& pk, std::vector<std::vector<float>>& out) {
        BitReader br(pk.data(), pk.size());
        if (br.Read(1) != 0) return true;  // not audio
        const int mi = static_cast<int>(br.Read(ILog(static_cast<std::uint32_t>(modes.size() - 1))));
        if (mi >= static_cast<int>(modes.size())) return false;
        const Mode& mode = modes[static_cast<size_t>(mi)];
        const int n = bs[mode.blockflag], n2 = n / 2;
        bool prevLong = true, nextLong = true;
        if (mode.blockflag) {
            prevLong = br.Read(1) != 0;
            nextLong = br.Read(1) != 0;
        }
        const Mapping& map = maps[static_cast<size_t>(mode.mapping)];
        // floors
        std::vector<std::vector<int>> Y(static_cast<size_t>(channels));
        std::vector<bool> nonzero(static_cast<size_t>(channels), false);
        for (int c = 0; c < channels; ++c) {
            const Floor1& f = floors[static_cast<size_t>(map.floorOf[map.mux[static_cast<size_t>(c)]])];
            if (!br.Read(1)) continue;
            static const int ranges[4] = {256, 128, 86, 64};
            const int range = ranges[f.multiplier - 1];
            auto& y = Y[static_cast<size_t>(c)];
            y.assign(f.X.size(), 0);
            y[0] = static_cast<int>(br.Read(ILog(static_cast<std::uint32_t>(range - 1))));
            y[1] = static_cast<int>(br.Read(ILog(static_cast<std::uint32_t>(range - 1))));
            size_t off = 2;
            bool ok = true;
            for (int pi = 0; pi < f.partitions && ok; ++pi) {
                const int cls = f.partClass[static_cast<size_t>(pi)];
                const int cdim = f.classDims[cls], cbits = f.classSub[cls], csub = (1 << cbits) - 1;
                int cval = 0;
                if (cbits) {
                    cval = books[static_cast<size_t>(f.classMaster[cls])].Decode(br);
                    if (cval < 0) ok = false;
                }
                for (int j = 0; j < cdim && ok; ++j) {
                    const int book = f.subBooks[cls][cval & csub];
                    cval >>= cbits;
                    if (book >= 0) {
                        const int v = books[static_cast<size_t>(book)].Decode(br);
                        if (v < 0) ok = false;
                        else y[off] = v;
                    }
                    ++off;
                }
            }
            nonzero[static_cast<size_t>(c)] = ok && !br.eop;
        }
        // coupled channels: energy in one means both are decoded
        std::vector<bool> nz = nonzero;
        for (size_t s = 0; s < map.mag.size(); ++s)
            if (nz[static_cast<size_t>(map.mag[s])] || nz[static_cast<size_t>(map.ang[s])])
                nz[static_cast<size_t>(map.mag[s])] = nz[static_cast<size_t>(map.ang[s])] = true;
        std::vector<std::vector<float>> res(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(n2), 0.f));
        for (int s = 0; s < map.submaps; ++s) {
            std::vector<float*> vecs;
            std::vector<bool> dnd;
            for (int c = 0; c < channels; ++c)
                if (map.mux[static_cast<size_t>(c)] == s) {
                    vecs.push_back(res[static_cast<size_t>(c)].data());
                    dnd.push_back(!nz[static_cast<size_t>(c)]);
                }
            DecodeResidue(residues[static_cast<size_t>(map.residueOf[s])], br, vecs, dnd, n2);
        }
        // inverse coupling (last step first)
        for (size_t s = map.mag.size(); s-- > 0;) {
            float* M = res[static_cast<size_t>(map.mag[s])].data();
            float* A = res[static_cast<size_t>(map.ang[s])].data();
            for (int j = 0; j < n2; ++j) {
                const float m = M[j], a = A[j];
                float nm, na;
                if (m > 0) {
                    if (a > 0) {
                        nm = m;
                        na = m - a;
                    } else {
                        na = m;
                        nm = m + a;
                    }
                } else {
                    if (a > 0) {
                        nm = m;
                        na = m + a;
                    } else {
                        na = m;
                        nm = m - a;
                    }
                }
                M[j] = nm;
                A[j] = na;
            }
        }
        // floor curves x residue, IMDCT, window
        auto& im = imdct[n];
        if (!im) im = std::make_unique<Imdct>(n);
        const std::vector<float>& win = Window(n, prevLong, nextLong);
        std::vector<std::vector<float>> blk(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(n), 0.f));
        for (int c = 0; c < channels; ++c) {
            std::vector<float> curve(static_cast<size_t>(n2), 0.f);
            if (nonzero[static_cast<size_t>(c)]) {
                const Floor1& f = floors[static_cast<size_t>(map.floorOf[map.mux[static_cast<size_t>(c)]])];
                static const int ranges[4] = {256, 128, 86, 64};
                const int range = ranges[f.multiplier - 1];
                const auto& y = Y[static_cast<size_t>(c)];
                const size_t nv = f.X.size();
                std::vector<int> fy(nv);
                std::vector<bool> step2(nv, false);
                fy[0] = y[0];
                fy[1] = y[1];
                step2[0] = step2[1] = true;
                for (size_t i = 2; i < nv; ++i) {
                    const int lo = f.lowN[i], hi = f.highN[i];
                    const int pred = RenderPoint(f.X[static_cast<size_t>(lo)], fy[static_cast<size_t>(lo)], f.X[static_cast<size_t>(hi)],
                                                 fy[static_cast<size_t>(hi)], f.X[i]);
                    const int val = y[i], highroom = range - pred, lowroom = pred;
                    const int room = (highroom < lowroom ? highroom : lowroom) * 2;
                    if (val) {
                        step2[static_cast<size_t>(lo)] = step2[static_cast<size_t>(hi)] = step2[i] = true;
                        if (val >= room) fy[i] = highroom > lowroom ? val - lowroom + pred : pred - val + highroom - 1;
                        else fy[i] = (val & 1) ? pred - (val + 1) / 2 : pred + val / 2;
                    } else {
                        fy[i] = pred;
                    }
                }
                int lx = 0, ly = fy[static_cast<size_t>(f.order[0])] * f.multiplier, hx = 0, hy = ly;
                for (size_t k = 1; k < nv; ++k) {
                    const int idx = f.order[k];
                    if (!step2[static_cast<size_t>(idx)]) continue;
                    hy = fy[static_cast<size_t>(idx)] * f.multiplier;
                    hx = f.X[static_cast<size_t>(idx)];
                    RenderLine(lx, ly, hx, hy, curve.data(), n2);
                    lx = hx;
                    ly = hy;
                }
                if (hx < n2) RenderLine(hx, hy, n2, hy, curve.data(), n2);
            }
            float* r = res[static_cast<size_t>(c)].data();
            for (int i = 0; i < n2; ++i) r[i] *= curve[static_cast<size_t>(i)];
            im->Run(r, blk[static_cast<size_t>(c)].data());
            for (int i = 0; i < n; ++i) blk[static_cast<size_t>(c)][static_cast<size_t>(i)] *= win[static_cast<size_t>(i)];
        }
        // overlap-add: previous centre .. this centre
        if (havePrev) {
            const int L = prevN / 4 + n / 4;
            for (int c = 0; c < channels; ++c) {
                auto& o = out[static_cast<size_t>(c)];
                const size_t at = o.size();
                o.resize(at + static_cast<size_t>(L), 0.f);
                const auto& pr = prevRight[static_cast<size_t>(c)];
                for (int t = 0; t < L; ++t) {
                    float v = t < static_cast<int>(pr.size()) ? pr[static_cast<size_t>(t)] : 0.f;
                    const int j = t - prevN / 4 + n / 4;
                    if (j >= 0 && j < n2) v += blk[static_cast<size_t>(c)][static_cast<size_t>(j)];
                    o[at + static_cast<size_t>(t)] = v;
                }
            }
        }
        prevRight.assign(static_cast<size_t>(channels), {});
        for (int c = 0; c < channels; ++c)
            prevRight[static_cast<size_t>(c)].assign(blk[static_cast<size_t>(c)].begin() + n2, blk[static_cast<size_t>(c)].end());
        prevN = n;
        havePrev = true;
        return true;
    }
};

// Ogg pages -> packets of the first logical stream; last granule position
bool OggPackets(const Bytes& b, std::vector<Bytes>& packets, unsigned long long& lastGranule) {
    size_t o = 0;
    Bytes cur;
    bool haveSerial = false;
    std::uint32_t serial = 0;
    lastGranule = 0;
    while (o + 27 <= b.size()) {
        if (std::memcmp(b.data() + o, "OggS", 4) != 0) {
            ++o;
            continue;
        }
        const int nseg = b[o + 26];
        if (o + 27 + static_cast<size_t>(nseg) > b.size()) break;
        unsigned long long g = 0;
        for (int k = 0; k < 8; ++k) g |= static_cast<unsigned long long>(b[o + 6 + static_cast<size_t>(k)]) << (8 * k);
        std::uint32_t s = 0;
        for (int k = 0; k < 4; ++k) s |= static_cast<std::uint32_t>(b[o + 14 + static_cast<size_t>(k)]) << (8 * k);
        if (!haveSerial) {
            serial = s;
            haveSerial = true;
        }
        size_t data = o + 27 + static_cast<size_t>(nseg);
        if (s != serial) {  // another logical stream: skip the page
            for (int i = 0; i < nseg; ++i) data += b[o + 27 + static_cast<size_t>(i)];
            o = data;
            continue;
        }
        if (g != ~0ull) lastGranule = g;
        for (int i = 0; i < nseg; ++i) {
            const size_t len = b[o + 27 + static_cast<size_t>(i)];
            if (data + len > b.size()) return !packets.empty();
            cur.insert(cur.end(), b.begin() + static_cast<std::ptrdiff_t>(data), b.begin() + static_cast<std::ptrdiff_t>(data + len));
            data += len;
            if (len < 255) {
                packets.push_back(std::move(cur));
                cur.clear();
            }
        }
        o = data;
    }
    return !packets.empty();
}

}  // namespace

bool DecodeOggVorbis(const Bytes& ogg, std::vector<std::int16_t>& pcm, int& channels, int& rate, std::wstring& err, size_t maxFrames) {
    pcm.clear();
    std::vector<Bytes> packets;
    unsigned long long granule = 0;
    if (!OggPackets(ogg, packets, granule) || packets.size() < 3) {
        err = L"Ogg akışı yok / no Ogg stream";
        return false;
    }
    Decoder d;
    if (!d.Ident(packets[0])) {
        err = L"Vorbis kimlik başlığı yok / no Vorbis identification header";
        return false;
    }
    if (!d.Setup(packets[2])) {
        err = L"Vorbis kurulum başlığı okunamadı / setup header: " + d.err;
        return false;
    }
    std::vector<std::vector<float>> out(static_cast<size_t>(d.channels));
    for (size_t i = 3; i < packets.size(); ++i) {
        if (!d.Packet(packets[i], out)) continue;
        if (maxFrames && out[0].size() >= maxFrames) break;
    }
    size_t frames = out[0].size();
    if (granule && granule < frames) frames = static_cast<size_t>(granule);
    if (maxFrames && frames > maxFrames) frames = maxFrames;
    channels = d.channels;
    rate = d.rate;
    pcm.resize(frames * static_cast<size_t>(channels));
    for (size_t f = 0; f < frames; ++f)
        for (int c = 0; c < channels; ++c) {
            const float v = std::clamp(out[static_cast<size_t>(c)][f], -1.f, 1.f);
            pcm[f * static_cast<size_t>(channels) + static_cast<size_t>(c)] = static_cast<std::int16_t>(std::lrint(v * 32767.f));
        }
    if (frames == 0) {
        err = L"Vorbis ses paketi çözülemedi / no audio decoded";
        return false;
    }
    return true;
}

double VorbisImdctSelfTest(int n) {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> dist(-1.f, 1.f);
    std::vector<float> X(static_cast<size_t>(n / 2)), y(static_cast<size_t>(n));
    for (auto& x : X) x = dist(rng);
    Imdct im(n);
    im.Run(X.data(), y.data());
    double worst = 0;
    for (int i = 0; i < n; ++i) {
        double ref = 0;
        for (int k = 0; k < n / 2; ++k) ref += X[static_cast<size_t>(k)] * std::cos(2 * kPi / n * (i + 0.5 + n / 4.0) * (k + 0.5));
        worst = std::max(worst, std::abs(ref - y[static_cast<size_t>(i)]));
    }
    return worst;
}

}  // namespace se::ed
