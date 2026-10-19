// SkillEffect Studio - core/Table.cpp
#include "Table.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>

namespace se {

std::vector<std::wstring> SplitTabs(std::wstring_view s) {
    std::vector<std::wstring> v;
    size_t a = 0;
    for (;;) {
        const size_t b = s.find(L'\t', a);
        if (b == std::wstring_view::npos) {
            v.emplace_back(s.substr(a));
            break;
        }
        v.emplace_back(s.substr(a, b - a));
        a = b + 1;
    }
    return v;
}

std::wstring JoinTabs(const std::vector<std::wstring>& c) {
    std::wstring s;
    for (size_t i = 0; i < c.size(); ++i) {
        if (i) s += L'\t';
        s += c[i];
    }
    return s;
}

static bool LooksUtf16NoBom(const Bytes& b) {
    if (b.size() < 4 || b.size() % 2) return false;
    size_t zOdd = 0;
    const size_t n = std::min<size_t>(b.size(), 4096);
    for (size_t i = 1; i < n; i += 2) zOdd += b[i] == 0;
    return zOdd * 3 > n / 2;  // > ~66% of the high bytes are zero
}

bool TableDoc::Load(const Bytes& r, std::wstring& err) {
    raw = r;
    lines.clear();
    text.clear();
    eff.clear();
    state.clear();
    bom = raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE;
    utf16 = bom || LooksUtf16NoBom(raw);
    if (!utf16 && raw.size() >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        err = L"UTF-16BE tablo desteklenmiyor / UTF-16BE table not supported";
        return false;
    }
    const size_t start = bom ? 2 : 0, unit = utf16 ? 2 : 1;
    auto at = [&](size_t i) -> unsigned {
        return utf16 ? static_cast<unsigned>(raw[i] | (raw[i + 1] << 8)) : static_cast<unsigned>(raw[i]);
    };
    // CRLF tables (textdata cells may hold a bare LF: it is NOT a line end there); LF-only files split on LF
    size_t crlf = 0, lf = 0;
    for (size_t i = start; i + unit <= raw.size(); i += unit) {
        if (at(i) == 0x0A) {
            ++lf;
            if (i >= start + unit && at(i - unit) == 0x0D) ++crlf;
        }
    }
    lfOnly = crlf == 0 && lf > 0;
    size_t a = start;
    for (size_t i = start; i + unit <= raw.size(); i += unit) {
        if (at(i) != 0x0A) continue;
        const bool cr = i >= a + unit && at(i - unit) == 0x0D;
        if (!lfOnly && !cr) continue;
        TLine L;
        L.off = a;
        L.len = (cr ? i - unit : i) - a;
        L.term = (cr ? 2 : 1) * unit;
        lines.push_back(L);
        a = i + unit;
    }
    finalTerm = true;
    if (a < raw.size()) {
        TLine L;
        L.off = a;
        L.len = raw.size() - a;
        L.term = 0;
        lines.push_back(L);
        finalTerm = false;
    }
    text.reserve(lines.size());
    for (const auto& L : lines) {
        if (utf16) {
            std::wstring w(L.len / 2, L'\0');
            if (!w.empty()) std::memcpy(w.data(), raw.data() + L.off, w.size() * 2);
            text.push_back(std::move(w));
        } else {
            text.push_back(FromAnsi(std::string_view(reinterpret_cast<const char*>(raw.data() + L.off), L.len), 949));
        }
    }
    // client comment rules R3/R4 (first '/' outside a block, first '*' inside one)
    bool inBlock = false;
    eofInBlock = false;
    openBlockLine = -1;
    dataRows = hiddenRows = 0;
    std::map<int, int> hist;
    state.assign(lines.size(), 1);
    eff.assign(lines.size(), std::wstring());
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::wstring& s = text[i];
        std::wstring_view e;
        if (!inBlock) {
            const size_t j = s.find(L'/');
            if (j != std::wstring::npos && j + 1 < s.size() && (s[j + 1] == L'/' || s[j + 1] == L'*')) {
                if (s[j + 1] == L'*') {
                    inBlock = true;
                    openBlockLine = static_cast<int>(i);
                }
                e = std::wstring_view(s).substr(0, j);
            } else {
                e = s;
            }
        } else {
            const size_t j = s.find(L'*');
            if (j != std::wstring::npos && j + 1 < s.size() && s[j + 1] == L'/') {
                inBlock = false;
                e = std::wstring_view(s).substr(j + 2);
            } else {
                state[i] = 2;
                if (ClientTokens(s).size() >= 3) ++hiddenRows;
                continue;
            }
        }
        if (!IsBlankClient(e)) {
            state[i] = 0;
            eff[i] = std::wstring(e);
            ++dataRows;
            hist[static_cast<int>(std::count(e.begin(), e.end(), L'\t')) + 1]++;
        }
    }
    eofInBlock = inBlock;
    if (!inBlock) openBlockLine = -1;
    int best = 0, bc = 0;
    for (const auto& [k, c] : hist)
        if (c > bc) {
            best = k;
            bc = c;
        }
    dominantCells = best;
    return true;
}

std::vector<std::wstring> TableDoc::Cells(size_t i) const { return SplitTabs(eff[i]); }

int TableDoc::EndInsert() const {
    int last = -1;
    for (size_t i = 0; i < lines.size(); ++i)
        if (state[i] == 0) last = static_cast<int>(i);
    if (last >= 0) return last + 1;
    if (eofInBlock && openBlockLine >= 0) return openBlockLine;
    return static_cast<int>(lines.size());
}

Bytes TableDoc::EncodeRow(const std::wstring& row) const {
    Bytes b;
    if (utf16) {
        b.resize((row.size() + 2) * 2);
        std::memcpy(b.data(), row.data(), row.size() * 2);
        b[row.size() * 2] = 0x0D;
        b[row.size() * 2 + 1] = 0;
        b[row.size() * 2 + 2] = 0x0A;
        b[row.size() * 2 + 3] = 0;
        if (lfOnly) b.erase(b.begin() + static_cast<std::ptrdiff_t>(row.size() * 2), b.begin() + static_cast<std::ptrdiff_t>(row.size() * 2 + 2));
        return b;
    }
    std::string s;
    size_t bad = 0;
    const bool exact = row.empty() || NarrowExact(949, row, s, &bad);  // v5 M3
    if (!exact) {
        // the old (lossy) bytes stay the result so nothing else changes, but the loss is noted: the writers
        // (WriteFileAtomic / Pk2WriteCopy) refuse data holding this row (ENC2)
        const int n = WideCharToMultiByte(949, 0, row.data(), static_cast<int>(row.size()), nullptr, 0, nullptr, nullptr);
        s.assign(static_cast<size_t>(n > 0 ? n : 0), '\0');
        if (n > 0) WideCharToMultiByte(949, 0, row.data(), static_cast<int>(row.size()), s.data(), n, nullptr, nullptr);
    }
    b.assign(s.begin(), s.end());
    if (!lfOnly) b.push_back(0x0D);
    b.push_back(0x0A);
    if (!exact) {
        EncLoss l;
        l.where = L"CP949 tablo satırı / table row";
        l.text = row;
        l.pos = bad;
        l.cell = static_cast<int>(std::count(row.begin(), row.begin() + static_cast<std::ptrdiff_t>(std::min(bad, row.size())), L'\t'));
        l.lossy = b;
        NoteEncLoss(std::move(l));
    }
    return b;
}

Bytes TableDoc::Splice(std::vector<std::pair<int, std::vector<std::wstring>>> ins, const std::vector<std::uint8_t>* drop) const {
    std::stable_sort(ins.begin(), ins.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    Bytes out;
    out.reserve(raw.size() + 4096);
    const size_t start = bom ? 2 : 0;
    out.insert(out.end(), raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(start));
    if (!bom && utf16 && lines.empty() && raw.empty()) {
        out.push_back(0xFF);  // a brand-new UTF-16 table gets the BOM the client skips blindly
        out.push_back(0xFE);
    }
    size_t k = 0;
    auto emit = [&](int before) {
        while (k < ins.size() && ins[k].first == before) {
            for (const auto& r : ins[k].second) {
                const Bytes b = EncodeRow(r);
                out.insert(out.end(), b.begin(), b.end());
            }
            ++k;
        }
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        emit(static_cast<int>(i));
        if (drop && i < drop->size() && (*drop)[i]) continue;
        const auto& L = lines[i];
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(L.off),
                   raw.begin() + static_cast<std::ptrdiff_t>(L.off + L.len + L.term));
    }
    if (k < ins.size() && !finalTerm) {  // EOF insert after an unterminated last line: end that line first
        const Bytes nl = EncodeRow(L"");
        out.insert(out.end(), nl.begin(), nl.end());
    }
    while (k < ins.size()) {
        for (const auto& r : ins[k].second) {
            const Bytes b = EncodeRow(r);
            out.insert(out.end(), b.begin(), b.end());
        }
        ++k;
    }
    return out;
}

}  // namespace se
