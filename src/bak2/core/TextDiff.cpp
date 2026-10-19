// SkillEffect Studio - core/TextDiff.cpp (v6 P3): line diff (Myers, linear space) + prova of Studio::Write
#include "TextDiff.h"

#include "Studio.h"

#include <algorithm>
#include <cwctype>
#include <unordered_map>

namespace se {

namespace {

// d of one middle-snake search; past it the sub-range is marked changed (a valid, not minimal script)
constexpr int kCostCap = 20000;

// Myers 1986, section 4b (linear space): the middle snake splits the problem in two halves whose costs add up to the
// whole, so the recursion gives a minimal script. Flags: ca[i] / cb[j] = 1 when the line is not on the common subsequence.
class Myers {
public:
    Myers(const std::vector<int>& a, const std::vector<int>& b, std::vector<char>& ca, std::vector<char>& cb)
        : A(a), B(b), CA(ca), CB(cb) {}
    bool exact = true;

    void Compare(int a0, int a1, int b0, int b1) {
        for (;;) {
            while (a0 < a1 && b0 < b1 && A[static_cast<size_t>(a0)] == B[static_cast<size_t>(b0)]) {
                ++a0;
                ++b0;
            }
            while (a0 < a1 && b0 < b1 && A[static_cast<size_t>(a1 - 1)] == B[static_cast<size_t>(b1 - 1)]) {
                --a1;
                --b1;
            }
            if (a0 == a1) {
                for (int j = b0; j < b1; ++j) CB[static_cast<size_t>(j)] = 1;
                return;
            }
            if (b0 == b1) {
                for (int i = a0; i < a1; ++i) CA[static_cast<size_t>(i)] = 1;
                return;
            }
            int x = 0, y = 0;
            const int n = a1 - a0, m = b1 - b0;
            const int b = Bisect(a0, n, b0, m, x, y);
            if (b != 1 || (x == 0 && y == 0) || (x == n && y == m)) {
                // 0 = no common line in the sub-range (all changed is the minimal script); cost cap / no usable split:
                // all changed is valid but maybe not minimal
                if (b != 0) exact = false;
                for (int i = a0; i < a1; ++i) CA[static_cast<size_t>(i)] = 1;
                for (int j = b0; j < b1; ++j) CB[static_cast<size_t>(j)] = 1;
                return;
            }
            Compare(a0, a0 + x, b0, b0 + y);
            a0 += x;  // the second half in this loop (no second recursion)
            b0 += y;
        }
    }

private:
    const std::vector<int>& A;
    const std::vector<int>& B;
    std::vector<char>& CA;
    std::vector<char>& CB;
    std::vector<int> v1, v2;

    int Ai(int a0, int i) const { return A[static_cast<size_t>(a0 + i)]; }
    int Bi(int b0, int j) const { return B[static_cast<size_t>(b0 + j)]; }

    // forward / reverse furthest-reaching paths until they overlap; (x, y) = end of the forward snake there.
    // 1 = split found, 0 = the ranges have no common line, -1 = cost cap
    int Bisect(int a0, int n, int b0, int m, int& sx, int& sy) {
        const int maxD = (n + m + 1) / 2;
        const int off = maxD + 1;
        const size_t len = static_cast<size_t>(2 * maxD + 3);
        v1.assign(len, -1);
        v2.assign(len, -1);
        v1[static_cast<size_t>(off + 1)] = 0;
        v2[static_cast<size_t>(off + 1)] = 0;
        const int delta = n - m;
        const bool front = (delta % 2) != 0;
        int k1start = 0, k1end = 0, k2start = 0, k2end = 0;
        auto in = [&](int o) { return o >= 0 && o < static_cast<int>(len); };
        for (int d = 0; d < maxD; ++d) {
            if (d > kCostCap) return -1;
            for (int k1 = -d + k1start; k1 <= d - k1end; k1 += 2) {
                const int ko = off + k1;
                int x1;
                if (k1 == -d || (k1 != d && v1[static_cast<size_t>(ko - 1)] < v1[static_cast<size_t>(ko + 1)])) x1 = v1[static_cast<size_t>(ko + 1)];
                else x1 = v1[static_cast<size_t>(ko - 1)] + 1;
                int y1 = x1 - k1;
                while (x1 < n && y1 < m && Ai(a0, x1) == Bi(b0, y1)) {
                    ++x1;
                    ++y1;
                }
                v1[static_cast<size_t>(ko)] = x1;
                if (x1 > n) k1end += 2;
                else if (y1 > m) k1start += 2;
                else if (front) {
                    const int k2o = off + delta - k1;
                    if (in(k2o) && v2[static_cast<size_t>(k2o)] != -1) {
                        const int x2 = n - v2[static_cast<size_t>(k2o)];
                        if (x1 >= x2) {
                            sx = x1;
                            sy = y1;
                            return 1;
                        }
                    }
                }
            }
            for (int k2 = -d + k2start; k2 <= d - k2end; k2 += 2) {
                const int ko = off + k2;
                int x2;
                if (k2 == -d || (k2 != d && v2[static_cast<size_t>(ko - 1)] < v2[static_cast<size_t>(ko + 1)])) x2 = v2[static_cast<size_t>(ko + 1)];
                else x2 = v2[static_cast<size_t>(ko - 1)] + 1;
                int y2 = x2 - k2;
                while (x2 < n && y2 < m && Ai(a0, n - x2 - 1) == Bi(b0, m - y2 - 1)) {
                    ++x2;
                    ++y2;
                }
                v2[static_cast<size_t>(ko)] = x2;
                if (x2 > n) k2end += 2;
                else if (y2 > m) k2start += 2;
                else if (!front) {
                    const int k1o = off + delta - k2;
                    if (in(k1o) && v1[static_cast<size_t>(k1o)] != -1) {
                        const int x1 = v1[static_cast<size_t>(k1o)];
                        const int y1 = off + x1 - k1o;
                        if (x1 >= n - x2) {
                            sx = x1;
                            sy = y1;
                            return 1;
                        }
                    }
                }
            }
        }
        return 0;
    }
};

const wchar_t* ArcFolder(Arc a) { return a == Arc::Data ? L"Data" : a == Arc::Particles ? L"Particles" : L"Media"; }

}  // namespace

std::vector<std::wstring> SplitLines(std::wstring_view s) {
    std::vector<std::wstring> out;
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find(L'\n', p);
        const size_t next = e == std::wstring_view::npos ? s.size() : e + 1;
        if (e == std::wstring_view::npos) e = s.size();
        size_t body = e;
        if (body > p && s[body - 1] == L'\r' && e < s.size()) --body;  // "\r\n"
        else if (e == s.size() && body > p && s[body - 1] == L'\r') --body;  // a last line ending in a lone '\r'
        out.emplace_back(s.substr(p, body - p));
        p = next;
    }
    return out;
}

std::vector<DiffOp> DiffLines(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b, bool* exact) {
    const int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    // line -> id
    std::unordered_map<std::wstring_view, int> ids;
    ids.reserve(a.size() + b.size());
    std::vector<int> ia(a.size()), ib(b.size());
    for (size_t i = 0; i < a.size(); ++i) ia[i] = ids.emplace(std::wstring_view(a[i]), static_cast<int>(ids.size())).first->second;
    for (size_t j = 0; j < b.size(); ++j) ib[j] = ids.emplace(std::wstring_view(b[j]), static_cast<int>(ids.size())).first->second;
    // common prefix / suffix
    int p = 0;
    while (p < n && p < m && ia[static_cast<size_t>(p)] == ib[static_cast<size_t>(p)]) ++p;
    int s = 0;
    while (s < n - p && s < m - p && ia[static_cast<size_t>(n - 1 - s)] == ib[static_cast<size_t>(m - 1 - s)]) ++s;
    std::vector<char> ca(a.size(), 0), cb(b.size(), 0);
    // lines of the middle that occur on one side only can never match: they are changed (keeps the result minimal)
    std::vector<int> cntA(ids.size(), 0), cntB(ids.size(), 0);
    for (int i = p; i < n - s; ++i) ++cntA[static_cast<size_t>(ia[static_cast<size_t>(i)])];
    for (int j = p; j < m - s; ++j) ++cntB[static_cast<size_t>(ib[static_cast<size_t>(j)])];
    std::vector<int> ra, rb, mapA, mapB;
    for (int i = p; i < n - s; ++i) {
        const int id = ia[static_cast<size_t>(i)];
        if (cntB[static_cast<size_t>(id)]) {
            ra.push_back(id);
            mapA.push_back(i);
        } else {
            ca[static_cast<size_t>(i)] = 1;
        }
    }
    for (int j = p; j < m - s; ++j) {
        const int id = ib[static_cast<size_t>(j)];
        if (cntA[static_cast<size_t>(id)]) {
            rb.push_back(id);
            mapB.push_back(j);
        } else {
            cb[static_cast<size_t>(j)] = 1;
        }
    }
    std::vector<char> rca(ra.size(), 0), rcb(rb.size(), 0);
    Myers my(ra, rb, rca, rcb);
    my.Compare(0, static_cast<int>(ra.size()), 0, static_cast<int>(rb.size()));
    if (exact) *exact = my.exact;
    for (size_t k = 0; k < ra.size(); ++k)
        if (rca[k]) ca[static_cast<size_t>(mapA[k])] = 1;
    for (size_t k = 0; k < rb.size(); ++k)
        if (rcb[k]) cb[static_cast<size_t>(mapB[k])] = 1;
    // edit script: unchanged lines pair up in order; inside a hunk the removed lines come first
    std::vector<DiffOp> ops;
    ops.reserve(a.size() + b.size());
    int i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && !ca[static_cast<size_t>(i)] && !cb[static_cast<size_t>(j)] && ia[static_cast<size_t>(i)] == ib[static_cast<size_t>(j)]) {
            ops.push_back({DiffKind::Same, i++, j++});
            continue;
        }
        const int i0 = i, j0 = j;
        while (i < n && ca[static_cast<size_t>(i)]) ops.push_back({DiffKind::Del, i++, -1});
        while (j < m && cb[static_cast<size_t>(j)]) ops.push_back({DiffKind::Add, -1, j++});
        if (i == i0 && j == j0) {  // never expected (flags out of step): the rest is changed, the script stays valid
            if (exact) *exact = false;
            while (i < n) ops.push_back({DiffKind::Del, i++, -1});
            while (j < m) ops.push_back({DiffKind::Add, -1, j++});
        }
    }
    return ops;
}

DiffCounts CountOps(const std::vector<DiffOp>& ops) {
    DiffCounts c;
    int del = 0, add = 0;
    auto flush = [&] {
        const int ch = std::min(del, add);
        c.changed += ch;
        c.removed += del - ch;
        c.added += add - ch;
        del = add = 0;
    };
    for (const auto& o : ops) {
        if (o.kind == DiffKind::Same) flush();
        else if (o.kind == DiffKind::Del) ++del;
        else ++add;
    }
    flush();
    return c;
}

LineDiff DiffText(std::wstring_view before, std::wstring_view after) {
    LineDiff d;
    d.identical = before == after;
    d.a = SplitLines(before);
    d.b = SplitLines(after);
    d.ops = DiffLines(d.a, d.b, &d.exact);
    d.counts = CountOps(d.ops);
    return d;
}

bool IsTextPath(std::wstring_view path) {
    static const wchar_t* const kExt[] = {L".txt", L".sql", L".ini", L".csv", L".tsv", L".xml", L".htm", L".html",
                                          L".json", L".md", L".log", L".cfg", L".bat", L".ps1"};
    for (const wchar_t* e : kExt)
        if (EndsWithI(path, e)) return true;
    return false;
}

bool LooksText(const Bytes& b) {
    if (b.empty()) return true;
    if (b.size() >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) return true;
    if (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) return true;
    const size_t n = std::min<size_t>(b.size(), 65536);
    size_t zOdd = 0, zEven = 0, ctrl = 0;
    for (size_t i = 0; i < n; ++i) {
        const unsigned c = b[i];
        if (c == 0) (i & 1 ? zOdd : zEven)++;
        else if (c < 0x09 || (c > 0x0D && c < 0x20 && c != 0x1B)) ++ctrl;
    }
    if (b.size() % 2 == 0 && b.size() >= 4) {  // UTF-16 without BOM (DecodeText's rule)
        const size_t m = std::min<size_t>(b.size(), 8192);
        size_t o = 0, e = 0;
        for (size_t i = 0; i < m; ++i)
            if (b[i] == 0) (i & 1 ? o : e)++;
        if ((o * 4 > m / 2 && e * 10 < o) || (e * 4 > m / 2 && o * 10 < e)) return true;
    }
    if (zOdd + zEven) return false;
    return ctrl * 100 <= n;
}

LineDiff DiffBytes(const Bytes& before, const Bytes& after, bool isText) {
    if (!isText || !LooksText(before) || !LooksText(after)) {
        LineDiff d;
        d.binary = true;
        d.identical = before == after;
        d.counts.changed = d.identical ? 0 : 1;
        return d;
    }
    TextEnc ea = TextEnc::Utf8, eb = TextEnc::Utf8;
    const std::wstring wa = before.empty() ? std::wstring() : DecodeText(before, &ea);
    const std::wstring wb = after.empty() ? std::wstring() : DecodeText(after, &eb);
    LineDiff d = DiffText(wa, wb);
    d.identical = before == after;
    d.encA = ea;
    d.encB = eb;
    return d;
}

DiffCounts DiffBytesCounts(const Bytes& before, const Bytes& after, bool isText) { return DiffBytes(before, after, isText).counts; }

std::vector<DiffHunk> DiffHunks(const std::vector<DiffOp>& ops, int context) {
    std::vector<DiffHunk> out;
    const size_t ctx = static_cast<size_t>(std::max(0, context));
    size_t k = 0;
    while (k < ops.size()) {
        if (ops[k].kind == DiffKind::Same) {
            ++k;
            continue;
        }
        DiffHunk h;
        h.op0 = k > ctx ? k - ctx : 0;
        size_t last = k;  // last changed op of the hunk
        size_t q = k;
        while (q < ops.size()) {
            if (ops[q].kind != DiffKind::Same) {
                last = q;
                ++q;
                continue;
            }
            size_t r = q;  // run of unchanged ops
            while (r < ops.size() && ops[r].kind == DiffKind::Same) ++r;
            if (r < ops.size() && r - q <= 2 * ctx) {  // the next change is close: same hunk
                q = r;
                continue;
            }
            break;
        }
        h.op1 = std::min(ops.size(), last + 1 + ctx);
        if (!out.empty() && h.op0 < out.back().op1) h.op0 = out.back().op1;
        out.push_back(h);
        k = h.op1;
    }
    return out;
}

bool StudioWriteFiles(Studio& st, const StudioResult& r, std::vector<WriteDiffFile>& out, std::wstring* err) {
    out.clear();
    for (const auto& o : r.outputs) {
        WriteDiffFile f;
        f.path = std::wstring(L"Media\\") + kTextdataInner + o.name;
        f.after = o.bytes;
        const bool have = !o.created && st.BaseFile(o.name, f.before);
        if (!have) f.before.clear();
        f.isNew = !have;
        f.isText = true;
        f.counts = DiffBytesCounts(f.before, f.after, true);
        f.clientOwn = have && (f.counts.changed + f.counts.removed) > 0;
        out.push_back(std::move(f));
    }
    for (const auto& x : r.installs) {
        WriteDiffFile f;
        f.path = std::wstring(ArcFolder(x.arc)) + L"\\" + x.inner;
        std::wstring e;
        if (!ReadFileBytes(x.src, f.after, e)) {
            if (err) *err = e;
            return false;
        }
        const bool have = x.replaces && st.Base().ReadResource(x.arc, x.inner, f.before);
        if (!have) f.before.clear();
        f.isNew = !have;
        f.isText = IsTextPath(x.inner);
        f.counts = DiffBytesCounts(f.before, f.after, f.isText);
        f.clientOwn = have;
        out.push_back(std::move(f));
    }
    return true;
}

}  // namespace se
