// SkillEffect Studio v6 - ddj/Dxt.cpp  (M33) - BC1 / BC2 / BC3 block compressors. See Dxt.h.
#include "Dxt.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace se::ddj {
namespace {

inline int Ex5(int v) { return (v << 3) | (v >> 2); }
inline int Ex6(int v) { return (v << 2) | (v >> 4); }
inline int ExC(int k, int v) { return k == 1 ? Ex6(v) : Ex5(v); }
constexpr int kMaxQ[3] = {31, 63, 31};

inline std::uint16_t Pack(const int e[3]) { return static_cast<std::uint16_t>((e[0] << 11) | (e[1] << 5) | e[2]); }
inline void Unpack(std::uint16_t c, int e[3]) { e[0] = (c >> 11) & 31; e[1] = (c >> 5) & 63; e[2] = c & 31; }

// nearest grid value of channel k for an 8-bit-scale value v (decoder expansion, not v * max / 255)
inline int Quant(int k, float v) {
    v = std::clamp(v, 0.0f, 255.0f);
    const int mx = kMaxQ[k];
    int q = static_cast<int>(v * mx / 255.0f + 0.5f);
    q = std::clamp(q, 0, mx);
    int best = q;
    float bd = std::fabs(static_cast<float>(ExC(k, q)) - v);
    for (int d = -1; d <= 1; d += 2) {
        const int t = q + d;
        if (t < 0 || t > mx) continue;
        const float e = std::fabs(static_cast<float>(ExC(k, t)) - v);
        if (e < bd) { bd = e; best = t; }
    }
    return best;
}

// ------------------------------------------------------------------------------------- single colour tables
struct SingleFit { std::uint8_t a = 0, b = 0; };
SingleFit g_s4[2][256], g_s3[2][256];   // [0] 5-bit, [1] 6-bit; s4: index 2 of a 4-colour block, s3: midpoint
std::once_flag g_once;

void InitTables() {
    for (int t = 0; t < 2; ++t) {
        const int mx = t ? 63 : 31;
        for (int v = 0; v < 256; ++v) {
            int b4 = 1 << 30, b3 = 1 << 30;
            for (int a = 0; a <= mx; ++a)
                for (int b = 0; b <= mx; ++b) {
                    const int A = t ? Ex6(a) : Ex5(a), B = t ? Ex6(b) : Ex5(b);
                    const int e4 = std::abs((2 * A + B + 1) / 3 - v), e3 = std::abs((A + B) / 2 - v);
                    if (e4 < b4) { b4 = e4; g_s4[t][v] = {static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b)}; }
                    if (e3 < b3) { b3 = e3; g_s3[t][v] = {static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b)}; }
                }
        }
    }
}

// ------------------------------------------------------------------------------------------- block + eval
struct Block {
    float col[16][3]{};
    float w[16]{};
    bool transp[16]{};
    bool anyTransp = false;
    bool black[16]{};       // near-black texel that may take index 3 of a 3-colour block (threeColorBlack)
    bool blackOk = false;
    float chw[3] = {1, 1, 1};
};

struct Cand {
    std::uint16_t c0 = 0, c1 = 0;
    std::uint32_t idx = 0;
    float err = 1e30f;
    bool four = true;
};

void Palette(std::uint16_t c0, std::uint16_t c1, bool four, int pal[4][3]) {
    int e0[3], e1[3];
    Unpack(c0, e0);
    Unpack(c1, e1);
    for (int k = 0; k < 3; ++k) {
        pal[0][k] = ExC(k, e0[k]);
        pal[1][k] = ExC(k, e1[k]);
        if (four) {
            pal[2][k] = (2 * pal[0][k] + pal[1][k] + 1) / 3;
            pal[3][k] = (pal[0][k] + 2 * pal[1][k] + 1) / 3;
        } else {
            pal[2][k] = (pal[0][k] + pal[1][k]) / 2;
            pal[3][k] = 0;
        }
    }
}

// endpoints in any order; four: c0 > c1 (equal = degenerate, only index 0), three: c0 <= c1 (index 3 = transparent)
void Eval(const Block& b, const int ea[3], const int eb[3], bool four, Cand& out) {
    const std::uint16_t ca = Pack(ea), cb = Pack(eb);
    std::uint16_t c0, c1;
    if (four) { c0 = std::max(ca, cb); c1 = std::min(ca, cb); }
    else { c0 = std::min(ca, cb); c1 = std::max(ca, cb); }
    const bool degenerate = four && c0 == c1;
    int pal[4][3];
    Palette(c0, c1, four && !degenerate, pal);
    const int usable = degenerate ? 1 : (four ? 4 : 3);
    const bool black3 = !four && b.blackOk;
    float err = 0;
    std::uint32_t idx = 0;
    for (int i = 0; i < 16; ++i) {
        if (b.transp[i]) { idx |= 3u << (2 * i); continue; }
        float best = 1e30f;
        int bi = 0;
        for (int s = 0; s < (black3 && b.black[i] ? 4 : usable); ++s) {
            float d = 0;
            for (int k = 0; k < 3; ++k) {
                const float t = static_cast<float>(pal[s][k]) - b.col[i][k];
                d += b.chw[k] * t * t;
            }
            if (d < best) { best = d; bi = s; }
        }
        err += best * b.w[i];
        idx |= static_cast<std::uint32_t>(bi) << (2 * i);
    }
    out.c0 = c0;
    out.c1 = c1;
    out.idx = idx;
    out.err = err;
    out.four = four;
}

void Keep(Cand& best, const Cand& c) {
    if (c.err < best.err) best = c;
}

// ------------------------------------------------------------------------------------------- points
struct Pts {
    int n = 0;
    float p[16][3]{};   // scaled space (x * sqrt(chw))
    float w[16]{};
    float s[3] = {1, 1, 1};
};

void Collect(const Block& b, Pts& P, bool skipBlack = false) {
    for (int k = 0; k < 3; ++k) P.s[k] = std::sqrt(b.chw[k]);
    P.n = 0;
    for (int i = 0; i < 16; ++i) {
        if (b.transp[i] || b.w[i] <= 0 || (skipBlack && b.black[i])) continue;
        float q[3] = {b.col[i][0] * P.s[0], b.col[i][1] * P.s[1], b.col[i][2] * P.s[2]};
        int j = 0;
        for (; j < P.n; ++j)
            if (P.p[j][0] == q[0] && P.p[j][1] == q[1] && P.p[j][2] == q[2]) break;
        if (j < P.n) { P.w[j] += b.w[i]; continue; }
        std::memcpy(P.p[P.n], q, sizeof(q));
        P.w[P.n] = b.w[i];
        ++P.n;
    }
}

void PcaAxis(const Pts& P, float ax[3]) {
    double W = 0, m[3] = {};
    for (int i = 0; i < P.n; ++i) {
        W += P.w[i];
        for (int k = 0; k < 3; ++k) m[k] += P.w[i] * P.p[i][k];
    }
    if (W <= 0) W = 1;
    for (double& v : m) v /= W;
    double c[3][3] = {};
    for (int i = 0; i < P.n; ++i) {
        double d[3] = {P.p[i][0] - m[0], P.p[i][1] - m[1], P.p[i][2] - m[2]};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) c[a][b] += P.w[i] * d[a] * d[b];
    }
    int r = 0;
    for (int a = 1; a < 3; ++a)
        if (c[a][a] > c[r][r]) r = a;
    double v[3] = {c[r][0], c[r][1], c[r][2]};
    if (std::fabs(v[0]) + std::fabs(v[1]) + std::fabs(v[2]) < 1e-9) { v[0] = v[1] = v[2] = 1; }
    for (int it = 0; it < 10; ++it) {
        double n[3] = {};
        for (int a = 0; a < 3; ++a) n[a] = c[a][0] * v[0] + c[a][1] * v[1] + c[a][2] * v[2];
        const double mx = std::max({std::fabs(n[0]), std::fabs(n[1]), std::fabs(n[2])});
        if (mx < 1e-12) break;
        for (int a = 0; a < 3; ++a) v[a] = n[a] / mx;
    }
    const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len < 1e-12) { ax[0] = ax[1] = ax[2] = 0.57735f; return; }
    for (int a = 0; a < 3; ++a) ax[a] = static_cast<float>(v[a] / len);
}

void Order(const Pts& P, const float ax[3], int ord[16]) {
    float d[16];
    for (int i = 0; i < P.n; ++i) {
        d[i] = P.p[i][0] * ax[0] + P.p[i][1] * ax[1] + P.p[i][2] * ax[2];
        ord[i] = i;
    }
    std::stable_sort(ord, ord + P.n, [&](int a, int b) { return d[a] < d[b]; });
}

// scaled value -> grid
inline int QuantS(const Pts& P, int k, float v) { return Quant(k, P.s[k] > 0 ? v / P.s[k] : 0.0f); }
inline float DeqS(const Pts& P, int k, int q) { return static_cast<float>(ExC(k, q)) * P.s[k]; }

void RangeFit(const Block& b, const Pts& P, const float ax[3], bool four, bool three, Cand& best4, Cand& best3) {
    int ord[16];
    Order(P, ax, ord);
    const float* lo = P.p[ord[0]];
    const float* hi = P.p[ord[P.n - 1]];
    int ea[3], eb[3];
    for (int k = 0; k < 3; ++k) { ea[k] = QuantS(P, k, lo[k]); eb[k] = QuantS(P, k, hi[k]); }
    Cand c;
    if (four) { Eval(b, ea, eb, true, c); Keep(best4, c); }
    if (three) { Eval(b, ea, eb, false, c); Keep(best3, c); }
}

// quantisation-aware cluster fit along the ordering of ax. Returns false when nothing was solvable.
bool ClusterFit(const Pts& P, const int ord[16], bool four, int ea[3], int eb[3]) {
    const int n = P.n;
    float W[17] = {}, X[17][3] = {};
    for (int i = 0; i < n; ++i) {
        const int o = ord[i];
        W[i + 1] = W[i] + P.w[o];
        for (int k = 0; k < 3; ++k) X[i + 1][k] = X[i][k] + P.w[o] * P.p[o][k];
    }
    float best = 1e30f;
    bool any = false;
    auto tryPart = [&](float A2, float B2, float AB, const float AX[3], const float BX[3]) {
        const float det = A2 * B2 - AB * AB;
        if (det <= 1e-6f * std::max(1.0f, A2 * B2)) return;
        const float inv = 1.0f / det;
        float e = 0;
        int qa[3], qb[3];
        for (int k = 0; k < 3; ++k) {
            const float a = (AX[k] * B2 - BX[k] * AB) * inv;
            const float b = (BX[k] * A2 - AX[k] * AB) * inv;
            qa[k] = QuantS(P, k, a);
            qb[k] = QuantS(P, k, b);
            const float aq = DeqS(P, k, qa[k]), bq = DeqS(P, k, qb[k]);
            e += aq * aq * A2 + bq * bq * B2 + 2 * aq * bq * AB - 2 * aq * AX[k] - 2 * bq * BX[k];
        }
        if (e < best) {
            best = e;
            any = true;
            std::memcpy(ea, qa, sizeof(qa));
            std::memcpy(eb, qb, sizeof(qb));
        }
    };
    if (four) {
        for (int i = 0; i <= n; ++i)
            for (int j = i; j <= n; ++j)
                for (int k = j; k <= n; ++k) {
                    const float W0 = W[i], W1 = W[j] - W[i], W2 = W[k] - W[j], W3 = W[n] - W[k];
                    const float A2 = W0 + (4.0f / 9) * W1 + (1.0f / 9) * W2;
                    const float B2 = (1.0f / 9) * W1 + (4.0f / 9) * W2 + W3;
                    const float AB = (2.0f / 9) * (W1 + W2);
                    float AX[3], BX[3];
                    for (int c = 0; c < 3; ++c) {
                        const float X0 = X[i][c], X1 = X[j][c] - X[i][c], X2 = X[k][c] - X[j][c], X3 = X[n][c] - X[k][c];
                        AX[c] = X0 + (2.0f / 3) * X1 + (1.0f / 3) * X2;
                        BX[c] = (1.0f / 3) * X1 + (2.0f / 3) * X2 + X3;
                    }
                    tryPart(A2, B2, AB, AX, BX);
                }
    } else {
        for (int i = 0; i <= n; ++i)
            for (int j = i; j <= n; ++j) {
                const float W0 = W[i], W1 = W[j] - W[i], W2 = W[n] - W[j];
                const float A2 = W0 + 0.25f * W1, B2 = 0.25f * W1 + W2, AB = 0.25f * W1;
                float AX[3], BX[3];
                for (int c = 0; c < 3; ++c) {
                    const float X0 = X[i][c], X1 = X[j][c] - X[i][c], X2 = X[n][c] - X[j][c];
                    AX[c] = X0 + 0.5f * X1;
                    BX[c] = 0.5f * X1 + X2;
                }
                tryPart(A2, B2, AB, AX, BX);
            }
    }
    return any;
}

void ClusterSearch(const Block& b, const Pts& P, const float ax0[3], bool four, int iters, Cand& best) {
    float ax[3] = {ax0[0], ax0[1], ax0[2]};
    int prev[16] = {};
    for (int it = 0; it < iters; ++it) {
        int ord[16];
        Order(P, ax, ord);
        if (it > 0 && std::equal(ord, ord + P.n, prev)) break;
        std::memcpy(prev, ord, sizeof(ord));
        int ea[3], eb[3];
        if (!ClusterFit(P, ord, four, ea, eb)) break;
        Cand c;
        Eval(b, ea, eb, four, c);
        Keep(best, c);
        // next axis: from the solved endpoints (scaled space)
        float d[3], len = 0;
        for (int k = 0; k < 3; ++k) {
            d[k] = DeqS(P, k, eb[k]) - DeqS(P, k, ea[k]);
            len += d[k] * d[k];
        }
        if (len < 1e-6f) break;
        len = std::sqrt(len);
        for (int k = 0; k < 3; ++k) ax[k] = d[k] / len;
    }
}

// least squares endpoints for fixed indices, then re-evaluate
void LsRefine(const Block& b, Cand& best, int passes) {
    for (int pass = 0; pass < passes; ++pass) {
        float A2 = 0, B2 = 0, AB = 0, AX[3] = {}, BX[3] = {};
        for (int i = 0; i < 16; ++i) {
            if (b.transp[i] || b.w[i] <= 0) continue;
            const int s = (best.idx >> (2 * i)) & 3;
            float al;
            if (best.four) al = s == 0 ? 1.0f : s == 1 ? 0.0f : s == 2 ? 2.0f / 3 : 1.0f / 3;
            else if (s == 3) continue;   // black / transparent index: not on the line
            else al = s == 0 ? 1.0f : s == 1 ? 0.0f : 0.5f;
            const float be = 1 - al, w = b.w[i];
            A2 += w * al * al;
            B2 += w * be * be;
            AB += w * al * be;
            for (int k = 0; k < 3; ++k) {
                AX[k] += w * al * b.col[i][k];
                BX[k] += w * be * b.col[i][k];
            }
        }
        const float det = A2 * B2 - AB * AB;
        if (det <= 1e-6f) return;
        int ea[3], eb[3];
        for (int k = 0; k < 3; ++k) {
            ea[k] = Quant(k, (AX[k] * B2 - BX[k] * AB) / det);
            eb[k] = Quant(k, (BX[k] * A2 - AX[k] * AB) / det);
        }
        Cand c;
        Eval(b, ea, eb, best.four, c);
        if (!(c.err < best.err)) return;
        best = c;
    }
}

void Climb(const Block& b, Cand& best, int maxSteps, bool diagonal) {
    for (int step = 0; step < maxSteps && best.err > 0; ++step) {
        int e[2][3];
        Unpack(best.c0, e[0]);
        Unpack(best.c1, e[1]);
        Cand bc = best;
        auto tryE = [&](int t0[3], int t1[3]) {
            Cand c;
            Eval(b, t0, t1, best.four, c);
            if (c.err < bc.err) bc = c;
        };
        for (int end = 0; end < 2; ++end)
            for (int k = 0; k < 3; ++k)
                for (int d = -1; d <= 1; d += 2) {
                    int t[2][3];
                    std::memcpy(t, e, sizeof(t));
                    t[end][k] += d;
                    if (t[end][k] < 0 || t[end][k] > kMaxQ[k]) continue;
                    tryE(t[0], t[1]);
                }
        if (diagonal)
            for (int k = 0; k < 3; ++k)
                for (int d0 = -1; d0 <= 1; d0 += 2)
                    for (int d1 = -1; d1 <= 1; d1 += 2) {
                        int t[2][3];
                        std::memcpy(t, e, sizeof(t));
                        t[0][k] += d0;
                        t[1][k] += d1;
                        if (t[0][k] < 0 || t[0][k] > kMaxQ[k] || t[1][k] < 0 || t[1][k] > kMaxQ[k]) continue;
                        tryE(t[0], t[1]);
                    }
        if (!(bc.err < best.err)) break;
        best = bc;
    }
}

// one mode (four = 4-colour, else 3-colour) over the points P: single colour tables, range fit, cluster fit, LS
void FitMode(const Block& b, const Pts& P, bool four, DxtQuality q, Cand& best) {
    Cand cd;
    if (P.n == 0) {   // nothing to fit (all texels transparent or black in a 3-colour block): index 3 everywhere
        int z[3] = {0, 0, 0};
        Eval(b, z, z, four, cd);
        Keep(best, cd);
        return;
    }
    if (P.n == 1) {
        int v[3], ea[3], eb[3];
        for (int k = 0; k < 3; ++k) v[k] = std::clamp(static_cast<int>(P.p[0][k] / (P.s[k] > 0 ? P.s[k] : 1.0f) + 0.5f), 0, 255);
        for (int k = 0; k < 3; ++k) {
            const SingleFit& s = four ? g_s4[k == 1][v[k]] : g_s3[k == 1][v[k]];
            ea[k] = s.a;
            eb[k] = s.b;
        }
        Eval(b, ea, eb, four, cd);
        Keep(best, cd);
        for (int k = 0; k < 3; ++k) ea[k] = eb[k] = Quant(k, static_cast<float>(v[k]));
        Eval(b, ea, eb, four, cd);
        Keep(best, cd);
        return;
    }
    float ax[3];
    PcaAxis(P, ax);
    Cand dummy;
    RangeFit(b, P, ax, four, !four, four ? best : dummy, four ? dummy : best);
    if (q >= DxtQuality::Normal) ClusterSearch(b, P, ax, four, q == DxtQuality::Best ? 6 : 1, best);
    if (best.err < 1e29f) LsRefine(b, best, q == DxtQuality::Fast ? 2 : 1);
}

void Search(const Block& b, bool canFour, bool canThree, DxtQuality q, Cand& out) {
    std::call_once(g_once, InitTables);
    Pts P;
    Collect(b, P);
    Cand b4, b3;
    if (canFour) FitMode(b, P, true, q, b4);
    if (canThree) {
        if (b.blackOk) {   // black texels go to index 3: fit the line through the others only
            Pts P3;
            Collect(b, P3, true);
            FitMode(b, P3, false, q, b3);
        }
        FitMode(b, P, false, q, b3);
    }
    const int steps = q == DxtQuality::Fast ? 0 : q == DxtQuality::Normal ? 8 : 40;
    if (steps > 0) {
        if (q == DxtQuality::Best) {
            if (b4.err < 1e29f) Climb(b, b4, steps, true);
            if (b3.err < 1e29f) Climb(b, b3, steps, true);
        } else if (b4.err <= b3.err) {
            if (b4.err < 1e29f) Climb(b, b4, steps, false);
        } else {
            Climb(b, b3, steps, false);
        }
    }
    out = b4.err <= b3.err ? b4 : b3;
}

void FillBlock(Block& b, const std::uint8_t px[64], const DxtParams& p, int alphaRef) {
    if (p.perceptual) { b.chw[0] = 0.299f * 3; b.chw[1] = 0.587f * 3; b.chw[2] = 0.114f * 3; }
    b.blackOk = p.threeColorBlack && alphaRef < 0;
    bool anyW = false;
    for (int i = 0; i < 16; ++i) {
        const std::uint8_t* t = px + i * 4;
        for (int k = 0; k < 3; ++k) b.col[i][k] = t[k];
        b.transp[i] = alphaRef >= 0 && t[3] < alphaRef;
        b.anyTransp = b.anyTransp || b.transp[i];
        b.black[i] = b.blackOk && std::max({t[0], t[1], t[2]}) <= p.blackLevel;
        b.w[i] = b.transp[i] ? 0.0f : (p.weightByAlpha ? t[3] / 255.0f : 1.0f);
        anyW = anyW || b.w[i] > 0;
    }
    if (!anyW)
        for (int i = 0; i < 16; ++i)
            if (!b.transp[i]) b.w[i] = 1.0f;
}

void WriteColor(const Cand& c, std::uint8_t out[8]) {
    out[0] = static_cast<std::uint8_t>(c.c0 & 0xFF);
    out[1] = static_cast<std::uint8_t>(c.c0 >> 8);
    out[2] = static_cast<std::uint8_t>(c.c1 & 0xFF);
    out[3] = static_cast<std::uint8_t>(c.c1 >> 8);
    for (int i = 0; i < 4; ++i) out[4 + i] = static_cast<std::uint8_t>(c.idx >> (8 * i));
}

void AlphaPalette(int a0, int a1, int pal[8]) {
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1) {
        for (int k = 1; k <= 6; ++k) pal[1 + k] = ((7 - k) * a0 + k * a1 + 3) / 7;
    } else {
        for (int k = 1; k <= 4; ++k) pal[1 + k] = ((5 - k) * a0 + k * a1 + 2) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
}

int AlphaFit(const std::uint8_t a[16], int a0, int a1, std::uint64_t& bits) {
    int pal[8];
    AlphaPalette(a0, a1, pal);
    int err = 0;
    bits = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 1 << 30, bi = 0;
        for (int s = 0; s < 8; ++s) {
            const int d = (pal[s] - a[i]) * (pal[s] - a[i]);
            if (d < best) { best = d; bi = s; }
        }
        err += best;
        bits |= static_cast<std::uint64_t>(bi) << (3 * i);
    }
    return err;
}

}  // namespace

void EncodeBc1(const std::uint8_t px[64], std::uint8_t out[8], const DxtParams& p, bool allowTransparent) {
    Block b;
    FillBlock(b, px, p, allowTransparent ? std::clamp(p.alphaRef, 1, 255) : -1);
    Cand c;
    Search(b, !b.anyTransp, true, p.quality, c);
    WriteColor(c, out);
}

void EncodeColorBc23(const std::uint8_t px[64], std::uint8_t out[8], const DxtParams& p) {
    Block b;
    FillBlock(b, px, p, -1);
    Cand c;
    Search(b, true, false, p.quality, c);
    WriteColor(c, out);
}

void EncodeBc2(const std::uint8_t px[64], std::uint8_t out[16], const DxtParams& p) {
    for (int i = 0; i < 8; ++i) {
        const int lo = (px[(2 * i) * 4 + 3] + 8) / 17, hi = (px[(2 * i + 1) * 4 + 3] + 8) / 17;
        out[i] = static_cast<std::uint8_t>(lo | (hi << 4));
    }
    EncodeColorBc23(px, out + 8, p);
}

void EncodeAlphaBc3(const std::uint8_t a[16], std::uint8_t out[8], DxtQuality q) {
    int mn = 255, mx = 0, mn6 = 255, mx6 = 0;
    bool has6 = false;
    for (int i = 0; i < 16; ++i) {
        mn = std::min(mn, static_cast<int>(a[i]));
        mx = std::max(mx, static_cast<int>(a[i]));
        if (a[i] != 0 && a[i] != 255) {
            has6 = true;
            mn6 = std::min(mn6, static_cast<int>(a[i]));
            mx6 = std::max(mx6, static_cast<int>(a[i]));
        }
    }
    int b0 = mx, b1 = mn;
    std::uint64_t bits = 0;
    int best = 1 << 30;
    auto tryP = [&](int a0, int a1) {
        std::uint64_t bt;
        const int e = AlphaFit(a, a0, a1, bt);
        if (e < best) { best = e; b0 = a0; b1 = a1; bits = bt; }
    };
    if (mn == mx) {
        tryP(mn, mn);
    } else {
        const int dl = q == DxtQuality::Fast ? 0 : q == DxtQuality::Normal ? 3 : 8;
        for (int a0 = std::max(0, mx - dl); a0 <= std::min(255, mx + dl) && best > 0; ++a0)
            for (int a1 = std::max(0, mn - dl); a1 <= std::min(255, mn + dl) && best > 0; ++a1)
                if (a0 > a1) tryP(a0, a1);
        if (has6) {
            for (int a0 = std::max(0, mn6 - dl); a0 <= std::min(255, mn6 + dl) && best > 0; ++a0)
                for (int a1 = std::max(0, mx6 - dl); a1 <= std::min(255, mx6 + dl) && best > 0; ++a1)
                    if (a0 <= a1) tryP(a0, a1);
        } else {
            tryP(0, 255);
        }
    }
    out[0] = static_cast<std::uint8_t>(b0);
    out[1] = static_cast<std::uint8_t>(b1);
    for (int i = 0; i < 6; ++i) out[2 + i] = static_cast<std::uint8_t>(bits >> (8 * i));
}

void EncodeBc3(const std::uint8_t px[64], std::uint8_t out[16], const DxtParams& p) {
    std::uint8_t a[16];
    for (int i = 0; i < 16; ++i) a[i] = px[i * 4 + 3];
    EncodeAlphaBc3(a, out, p.quality);
    EncodeColorBc23(px, out + 8, p);
}

void DecodeColorBlock(const std::uint8_t in[8], std::uint8_t px[64], bool fourAlways) {
    const std::uint16_t c0 = static_cast<std::uint16_t>(in[0] | (in[1] << 8));
    const std::uint16_t c1 = static_cast<std::uint16_t>(in[2] | (in[3] << 8));
    const bool four = c0 > c1 || fourAlways;
    int pal[4][3];
    Palette(c0, c1, four, pal);
    const std::uint32_t idx = static_cast<std::uint32_t>(in[4]) | (static_cast<std::uint32_t>(in[5]) << 8) |
                              (static_cast<std::uint32_t>(in[6]) << 16) | (static_cast<std::uint32_t>(in[7]) << 24);
    for (int i = 0; i < 16; ++i) {
        const int s = (idx >> (2 * i)) & 3;
        for (int k = 0; k < 3; ++k) px[i * 4 + k] = static_cast<std::uint8_t>(pal[s][k]);
        px[i * 4 + 3] = (!four && s == 3) ? 0 : 255;
    }
}

void DecodeAlphaBc3(const std::uint8_t in[8], std::uint8_t a[16]) {
    int pal[8];
    AlphaPalette(in[0], in[1], pal);
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(in[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) a[i] = static_cast<std::uint8_t>(pal[(bits >> (3 * i)) & 7]);
}

}  // namespace se::ddj
