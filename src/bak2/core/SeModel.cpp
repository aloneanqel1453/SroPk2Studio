// SkillEffect Studio - core/SeModel.cpp
// Client reader model; every rule is SKILLEFFECT_LOGIC.md section 2 (sro_client.exe VAs there).
#include "SeModel.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <map>

namespace se {

const wchar_t* SecName(Sec s) {
    switch (s) {
        case Sec::CI: return L"characterInfo";
        case Sec::Light: return L"light";
        case Sec::SA: return L"skillaniset2";
        case Sec::SE: return L"skilleffectset";
        case Sec::Env: return L"env";
        default: return L"-";
    }
}

Sec SecFromName(std::wstring_view n) {
    if (n == L"characterInfo") return Sec::CI;
    if (n == L"light") return Sec::Light;
    if (n == L"skillaniset" || n == L"skillaniset2") return Sec::SA;
    if (n == L"skilleffectset") return Sec::SE;
    if (n == L"env") return Sec::Env;
    return Sec::None;
}

int ExpectedCells(Sec s) {
    switch (s) {
        case Sec::CI: return 13;
        case Sec::SA: return 27;
        case Sec::SE: return 28;
        default: return 0;
    }
}

int KeyColumn(Sec s) {
    switch (s) {
        case Sec::SA: return 2;
        case Sec::SE: return 1;
        default: return 0;
    }
}

const wchar_t* LineKindName(LineKind k, bool en) {
    switch (k) {
        case LineKind::Blank: return en ? L"blank" : L"boş";
        case LineKind::Active: return en ? L"read" : L"okunur";
        case LineKind::Comment: return en ? L"in a comment (not read)" : L"yorum içinde (okunmaz)";
        case LineKind::Header: return en ? L"section header" : L"bölüm başlığı";
        case LineKind::BadHeader: return en ? L"bad section header (not read, S1)" : L"hatalı bölüm başlığı (okunmaz, S1)";
        case LineKind::OutOfSection: return en ? L"outside any section (not read, S2)" : L"bölüm dışı (okunmaz, S2)";
        case LineKind::Misrouted:
            return en ? L"not read as meant: goes to the previous section (S1)" : L"okunmaz: önceki bölüme gider (S1)";
    }
    return L"?";
}

bool LineKindRead(LineKind k) { return k == LineKind::Active || k == LineKind::Header; }

namespace {
int KindRank(LineKind k) {
    switch (k) {
        case LineKind::Blank: return 0;
        case LineKind::Comment: return 1;
        case LineKind::Active: return 2;
        case LineKind::Header: return 3;
        case LineKind::OutOfSection: return 4;
        case LineKind::Misrouted: return 5;
        case LineKind::BadHeader: return 6;
    }
    return 0;
}
// '#Section', '#SECTION', '#section characterInfo' (space instead of TAB): not a header for the client (R8 is exact)
bool HeaderLookalike(const std::wstring& tok0) {
    static const wchar_t kH[] = L"#section";
    if (tok0 == kH || tok0.size() < 8) return false;
    for (size_t i = 0; i < 8; ++i)
        if (towlower(tok0[i]) != kH[i]) return false;
    return tok0.size() == 8 || tok0[8] == L' ' || tok0[8] == 0x3000;
}
}  // namespace

const std::wstring& Rec::Key() const {
    static const std::wstring empty;
    const size_t k = static_cast<size_t>(KeyColumn(sec));
    return k < cells.size() ? cells[k] : empty;
}

std::vector<PLine> SplitPhysical(std::wstring_view t) {
    std::vector<PLine> v;
    size_t a = 0, i = 0;
    const size_t n = t.size();
    while (i < n) {
        const wchar_t c = t[i];
        if (c == L'\r' || c == L'\n') {
            PLine p;
            p.off = a;
            p.len = i - a;
            if (c == L'\r' && i + 1 < n && t[i + 1] == L'\n') {
                p.term = 1;
                i += 2;
            } else {
                p.term = c == L'\n' ? 2 : 3;
                i += 1;
            }
            v.push_back(p);
            a = i;
        } else {
            ++i;
        }
    }
    if (a < n) v.push_back(PLine{a, n - a, 0});
    return v;
}

SimResult Simulate(std::wstring_view t, const std::vector<PLine>& lines) {
    SimResult r;
    r.recOfLine.assign(lines.size(), -1);
    r.blockAfter.assign(lines.size(), 0);
    r.dropped.assign(lines.size(), 0);
    r.lineKind.assign(lines.size(), static_cast<std::uint8_t>(LineKind::Blank));
    std::vector<size_t> starts(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) starts[i] = lines[i].off;
    auto physOf = [&](size_t off) -> int {
        auto it = std::upper_bound(starts.begin(), starts.end(), off);
        return it == starts.begin() ? 0 : static_cast<int>(it - starts.begin()) - 1;
    };
    std::vector<std::int8_t> stateAt(lines.size(), -1);  // block state after the client line starting there

    bool inBlock = false;
    bool afterBad = false;  // v5 M5: since the last S1 header (until a valid one)
    Sec sec = Sec::None;
    std::wstring cur;
    size_t start = 0;
    const size_t n = t.size();

    auto mark = [&](int p, LineKind k) {
        if (lines.empty()) return;
        auto& v = r.lineKind[static_cast<size_t>(p)];
        if (KindRank(k) > KindRank(static_cast<LineKind>(v))) v = static_cast<std::uint8_t>(k);
    };
    auto handle = [&](const std::wstring& s, size_t startOff) {
        const int p = lines.empty() ? 0 : physOf(startOff);
        std::wstring_view eff;
        bool keep = true;
        if (!inBlock) {
            const size_t j = s.find(L'/');
            if (j != std::wstring::npos && j + 1 < s.size() && s[j + 1] == L'/') {
                eff = std::wstring_view(s).substr(0, j);  // R3 '//'
            } else if (j != std::wstring::npos && j + 1 < s.size() && s[j + 1] == L'*') {
                inBlock = true;  // R3 '/*': cut, block starts, rest of line ignored
                eff = std::wstring_view(s).substr(0, j);
            } else {
                eff = s;
            }
        } else {
            const size_t j = s.find(L'*');  // R4: only the first '*' of the line
            if (j != std::wstring::npos && j + 1 < s.size() && s[j + 1] == L'/') {
                inBlock = false;
                eff = std::wstring_view(s).substr(j + 2);
            } else {
                keep = false;
            }
        }
        if (!lines.empty()) {
            stateAt[static_cast<size_t>(p)] = inBlock ? 1 : 0;
            if (!keep) r.dropped[static_cast<size_t>(p)] = 1;
        }
        if (!keep || IsBlankClient(eff)) {  // R5
            if (!keep || !IsBlankClient(s)) mark(p, LineKind::Comment);
            return;
        }
        auto tok = ClientTokens(eff);
        if (tok.empty()) return;
        if (tok[0] == L"#section") {  // R8
            const Sec ns = tok.size() > 1 ? SecFromName(tok[1]) : Sec::None;
            Rec h;
            if (ns != Sec::None) {
                sec = ns;
                afterBad = false;
                if (r.firstHeaderLine < 0) r.firstHeaderLine = p;
                mark(p, LineKind::Header);
            } else {  // S1: an unknown name keeps the previous section
                h.diag = RecDiag::BadName;
                afterBad = true;
                mark(p, LineKind::BadHeader);
            }
            h.line = p;
            h.sec = sec;
            h.header = true;
            h.cells = std::move(tok);
            r.recs.push_back(std::move(h));
            return;
        }
        const bool lookalike = HeaderLookalike(tok[0]);
        if (lookalike) {
            afterBad = true;
            mark(p, LineKind::BadHeader);
        }
        if (sec == Sec::None) {  // R9 (S2 when it is data)
            if (!lookalike) {
                mark(p, LineKind::OutOfSection);
                r.outOfSection++;
            }
            return;
        }
        Rec d;
        d.line = p;
        d.sec = sec;
        d.diag = lookalike ? RecDiag::BadCase : afterBad ? RecDiag::AfterBad : RecDiag::None;
        mark(p, lookalike ? LineKind::BadHeader : afterBad ? LineKind::Misrouted : LineKind::Active);
        d.cells = std::move(tok);
        if (!lines.empty() && r.recOfLine[static_cast<size_t>(p)] < 0)
            r.recOfLine[static_cast<size_t>(p)] = static_cast<int>(r.recs.size());
        r.counts[static_cast<size_t>(sec)]++;
        r.recs.push_back(std::move(d));
    };

    // R2 line splitter, mode 1
    for (size_t i = 0; i < n; ++i) {
        const wchar_t c = t[i];
        if (c == L'\0') {
            if (!lines.empty()) {
                const int p = physOf(i);
                if (r.nulLines.empty() || r.nulLines.back() != p) r.nulLines.push_back(p);
            }
            handle(cur, start);
            cur.clear();
            start = i + 1;
        } else if (c == L'\r') {
            if (i + 1 < n && t[i + 1] == L'\n') {
                cur.push_back(L'\t');
            } else {
                handle(cur, start);
                cur.clear();
                start = i + 1;
            }
        } else if (c == L'\n') {
            if (i + 1 < n && t[i + 1] == L'\r') {
                cur.push_back(L'\t');
            } else {
                handle(cur, start);
                cur.clear();
                start = i + 1;
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) handle(cur, start);
    r.eofInBlock = inBlock;
    std::uint8_t carry = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        // a line the reader merged into the previous one (blank, R2) inside /* */ is part of the comment
        if (stateAt[i] < 0 && carry && r.lineKind[i] == static_cast<std::uint8_t>(LineKind::Blank))
            r.lineKind[i] = static_cast<std::uint8_t>(LineKind::Comment);
        if (stateAt[i] >= 0) carry = static_cast<std::uint8_t>(stateAt[i]);
        r.blockAfter[i] = carry;
    }
    return r;
}

const wchar_t* SeDoc::TermStr(std::uint8_t t) const {
    switch (t) {
        case 1: return L"\r\n";
        case 2: return L"\n";
        case 3: return L"\r";
        default: return L"";
    }
}

bool SeDoc::Load(const Bytes& raw, std::wstring& err) {
    hasBom = raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE;
    if (!hasBom) {
        // the client reads UTF-16LE only; accept a BOM-less UTF-16LE file (reported as an error by the validator)
        TextEnc enc{};
        bool bom = false;
        DecodeText(raw, &enc, &bom);
        if (enc != TextEnc::Utf16LE) {
            err = std::wstring(L"skilleffect.txt UTF-16LE değil (") + EncName(enc) +
                  L"). İstemci yalnız UTF-16LE okur. / not UTF-16LE, the client reads UTF-16LE only.";
            return false;
        }
    }
    const size_t skip = hasBom ? 2 : 0;
    if ((raw.size() - skip) % 2 != 0) {
        err = L"Tek sayıda bayt: bozuk UTF-16 / odd byte count, broken UTF-16";
        return false;
    }
    text.assign((raw.size() - skip) / 2, L'\0');
    if (!text.empty()) std::memcpy(text.data(), raw.data() + skip, text.size() * 2);
    lines = SplitPhysical(text);
    sim = Simulate(text, lines);
    countCrlf = countLf = countCr = 0;
    for (const auto& l : lines) {
        if (l.term == 1) ++countCrlf;
        else if (l.term == 2) ++countLf;
        else if (l.term == 3) ++countCr;
    }
    mainTerm = 1;
    if (countLf > countCrlf && countLf >= countCr) mainTerm = 2;
    else if (countCr > countCrlf && countCr > countLf) mainTerm = 3;
    finalTerm = !lines.empty() && lines.back().term != 0;
    padTabs.fill(0);
    std::array<std::map<int, int>, kSecCount> hist;
    for (const auto& rc : sim.recs) {
        if (rc.header) continue;
        const auto v = Line(static_cast<size_t>(rc.line));
        const int tabs = static_cast<int>(std::count(v.begin(), v.end(), L'\t'));
        hist[static_cast<size_t>(rc.sec)][tabs]++;
    }
    for (size_t s = 0; s < kSecCount; ++s) {
        int best = 0, bc = 0;
        for (const auto& [k, c] : hist[s])
            if (c > bc) { best = k; bc = c; }
        padTabs[s] = best;
    }
    return true;
}

Bytes SeDoc::Encode(const std::wstring& body) const {
    Bytes b;
    b.reserve(body.size() * 2 + 2);
    if (hasBom) {
        b.push_back(0xFF);
        b.push_back(0xFE);
    }
    const size_t o = b.size();
    b.resize(o + body.size() * 2);
    if (!body.empty()) std::memcpy(b.data() + o, body.data(), body.size() * 2);
    return b;
}

}  // namespace se
