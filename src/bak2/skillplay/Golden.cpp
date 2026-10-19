// SkillEffect Studio v6 P5 (M28) - skillplay/Golden.cpp (see Golden.h)
#include "Golden.h"

#include "../core/SkillCrypt.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace se::sp {
namespace {

std::vector<std::wstring> LinesOf(const std::wstring& t) {
    std::vector<std::wstring> out;
    size_t a = 0;
    while (a < t.size()) {
        size_t b = t.find(L'\n', a);
        if (b == std::wstring::npos) b = t.size();
        std::wstring l = t.substr(a, b - a);
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        out.push_back(std::move(l));
        a = b + 1;
    }
    return out;
}

std::vector<std::wstring> RawCells(const std::wstring& l) {
    std::vector<std::wstring> c;
    for (auto v : SplitView(l, L'\t')) c.emplace_back(v);
    return c;
}

// FNV-1a 32 over UTF-16 code units (stable across runs and machines)
struct Fnv {
    std::uint32_t h = 2166136261u;
    void Add(std::wstring_view s) {
        for (wchar_t c : s) {
            h ^= static_cast<std::uint32_t>(c) & 0xFFFFu;
            h *= 16777619u;
        }
        h ^= 0x1Fu;  // field separator
        h *= 16777619u;
    }
    void Num(double v) { Add(Fmt(L"%.1f", std::round(v * 10.0) / 10.0)); }
    void Int(long long v) { Add(Fmt(L"%lld", v)); }
};

std::wstring Up(std::wstring s) { return ToUpper(s); }

void HashRows(const FxDb& db, const std::wstring& key, Fnv& f, int& service, int& rows) {
    const AniSetRow* sa = db.AniSet(key);
    service = sa ? (db.Disabled(key) ? 0 : 1) : -1;
    f.Int(service);
    if (sa)
        for (const auto& c : sa->cells) f.Add(c);
    const auto se = db.Effects(key);
    rows = static_cast<int>(se.size());
    f.Int(rows);
    for (const EffectRow* r : se) {
        for (const auto& c : r->cells) f.Add(c);
        f.Add(L"|");
    }
}

void HashTimeline(const Timeline& tl, Fnv& f, int& fx, int& snd, int& hit, int& ms) {
    f.Int(tl.ok ? 1 : 0);
    f.Num(tl.totalMs);
    f.Num(tl.shotStart);
    f.Num(tl.shotEnd);
    for (const auto& a : tl.anims) {
        f.Int(static_cast<int>(a.phase));
        f.Int(a.role);
        f.Num(a.start);
        f.Num(a.end);
    }
    for (const auto& e : tl.fx) {
        f.Num(e.t);
        f.Num(e.holdUntil);
        f.Num(e.releaseAt);
        f.Int(static_cast<int>(e.kind));
        f.Add(Up(e.res));
        f.Int(static_cast<int>(e.place));
        f.Int(e.actor);
        f.Add(Up(e.bone));
        f.Num(e.scale);
        f.Int(static_cast<int>(e.mov));
        f.Add(Up(e.arrive));
        f.Int(e.dmg ? 1 : 0);
    }
    for (const auto& s : tl.sounds) {
        f.Num(s.t);
        f.Add(Up(s.path));
        f.Int(s.actor);
    }
    for (const auto& h : tl.hits) {
        f.Num(h.t);
        f.Add(Up(h.damageEfp));
        f.Add(Up(h.bloodEfp));
    }
    for (const auto& c : tl.cams) {
        f.Num(c.t);
        f.Int(c.level);
    }
    fx = static_cast<int>(tl.fx.size());
    snd = static_cast<int>(tl.sounds.size());
    hit = static_cast<int>(tl.hits.size());
    ms = static_cast<int>(std::lround(tl.totalMs));
}

PlayOptions GoldenPlay(std::uint32_t seed) {
    PlayOptions p;
    p.seed = seed;
    p.shotPick = 0;          // first AniShot of a comma list: deterministic
    p.effectSounds = true;
    return p;
}

bool ParseHex32(std::wstring_view s, std::uint32_t& v) {
    if (s.empty() || s.size() > 8) return false;
    v = 0;
    for (wchar_t c : s) {
        v <<= 4;
        if (c >= L'0' && c <= L'9') v |= static_cast<std::uint32_t>(c - L'0');
        else if (c >= L'a' && c <= L'f') v |= static_cast<std::uint32_t>(c - L'a' + 10);
        else if (c >= L'A' && c <= L'F') v |= static_cast<std::uint32_t>(c - L'A' + 10);
        else return false;
    }
    return true;
}

bool LoadSkills(const ClientBase& c, GoldenSource& out, std::wstring& err) {
    Bytes idx;
    std::wstring index = L"skilldataenc.txt";
    if (!c.ReadTextdata(index, idx)) {
        index = L"skilldata.txt";
        if (!c.ReadTextdata(index, idx)) {
            err = L"skilldataenc.txt / skilldata.txt yok / missing";
            return false;
        }
    }
    out.skillIndex = index;
    for (const auto& name : LinesOf(DecodeText(idx))) {
        const std::wstring n = Trim(name);
        if (n.empty() || StartsWith(n, L"//")) continue;
        Bytes b;
        if (!c.ReadTextdata(n, b)) continue;
        ++out.buckets;
        for (const auto& l : LinesOf(SkillDataText(b))) {
            if (l.empty() || StartsWith(l, L"//")) continue;
            SkillInfo s = ParseSkillRow(RawCells(l));
            if (!s.valid) continue;
            auto it = out.byId.find(s.id);
            if (it != out.byId.end()) {
                out.skills[it->second] = std::move(s);
            } else {
                out.byId[s.id] = out.skills.size();
                out.skills.push_back(std::move(s));
            }
        }
    }
    if (out.skills.empty()) {
        err = index + L": skill satırı yok / no skill rows";
        return false;
    }
    return true;
}

}  // namespace

const SkillInfo* GoldenSource::ById(int id) const {
    auto it = byId.find(id);
    return it == byId.end() ? nullptr : &skills[it->second];
}

std::function<const SkillInfo*(int)> GoldenSource::Lookup() const {
    return [this](int id) { return ById(id); };
}

bool LoadGoldenSourceWith(const ClientBase& c, const Bytes* skillEffect, const Bytes* effectSound, GoldenSource& out,
                          std::wstring& err) {
    out = GoldenSource{};
    Bytes se, snd;
    if (skillEffect) se = *skillEffect;
    else if (!c.ReadTextdata(L"skilleffect.txt", se)) {
        err = L"skilleffect.txt yok / missing";
        return false;
    }
    if (!out.db.LoadSkillEffect(se, err)) return false;
    if (effectSound) out.db.LoadEffectSound(*effectSound);
    else if (c.ReadTextdata(L"effectsound.txt", snd)) out.db.LoadEffectSound(snd);
    return LoadSkills(c, out, err);
}

bool LoadGoldenSource(const ClientBase& c, GoldenSource& out, std::wstring& err) {
    return LoadGoldenSourceWith(c, nullptr, nullptr, out, err);
}

std::wstring GoldenLine::Tsv() const {
    return Fmt(L"%d\t%ls\t%ls\t%d\t%d\t%08x\t%08x\t%d\t%d\t%d\t%d", id, code.c_str(), key.c_str(), service, rows, rowHash, tlHash, fx,
               snd, hit, totalMs);
}

GoldenLine GoldenOf(const GoldenSource& src, const SkillInfo& s, const GoldenOptions& o) {
    GoldenLine g;
    g.id = s.id;
    g.code = s.code;
    g.key = src.db.PresentationKey(s, src.Lookup());
    Fnv rows;
    HashRows(src.db, g.key, rows, g.service, g.rows);
    g.rowHash = rows.h;
    Fnv tl;
    tl.Add(Up(g.key));
    const Timeline t = BuildTimeline(src.db, g.key, s, nullptr, nullptr, GoldenPlay(o.seed));
    HashTimeline(t, tl, g.fx, g.snd, g.hit, g.totalMs);
    g.tlHash = tl.h;
    return g;
}

std::uint64_t KeyDigest(const FxDb& db, const std::wstring& key) {
    Fnv rows;
    int svc = 0, n = 0;
    HashRows(db, key, rows, svc, n);
    Fnv tl;
    int fx = 0, snd = 0, hit = 0, ms = 0;
    HashTimeline(BuildTimeline(db, key, SkillInfo{}, nullptr, nullptr, GoldenPlay(1)), tl, fx, snd, hit, ms);
    return (static_cast<std::uint64_t>(rows.h) << 32) | tl.h;
}

std::vector<GoldenLine> GoldenAll(const GoldenSource& src, const GoldenOptions& o) {
    std::vector<GoldenLine> out;
    out.reserve(src.byId.size());
    size_t n = 0;
    int last = -1;
    for (const auto& [id, i] : src.byId) {
        (void)id;
        out.push_back(GoldenOf(src, src.skills[i], o));
        ++n;
        if (o.progress) {
            const int p = static_cast<int>(100 * n / std::max<size_t>(1, src.byId.size()));
            if (p != last) o.progress(last = p);
        }
    }
    return out;
}

std::wstring GoldenText(const std::vector<GoldenLine>& lines, const std::wstring& client) {
    std::wstring t = L"# SkillEffect Studio golden v1\t" + client + Fmt(L"\tskills=%zu\r\n", lines.size());
    t += L"# id\tcode\tkey\tservice\trows\trowhash\ttlhash\tfx\tsnd\thit\tms\r\n";
    for (const auto& l : lines) t += l.Tsv() + L"\r\n";
    return t;
}

bool WriteGolden(const std::wstring& path, const std::vector<GoldenLine>& lines, const std::wstring& client, std::wstring& err) {
    const std::string u = "\xEF\xBB\xBF" + ToUtf8(GoldenText(lines, client));
    return WriteFileAtomic(path, Bytes(u.begin(), u.end()), err);
}

bool ReadGolden(const std::wstring& path, std::vector<GoldenLine>& lines, std::wstring& err) {
    lines.clear();
    Bytes b;
    if (!ReadFileBytes(path, b, err)) return false;
    const auto all = LinesOf(DecodeText(b));
    if (all.empty() || !StartsWith(all[0], L"# SkillEffect Studio golden v1")) {
        err = path + L": altın dosyası değil / not a golden file";
        return false;
    }
    for (size_t i = 1; i < all.size(); ++i) {
        if (all[i].empty() || all[i][0] == L'#') continue;
        const auto c = RawCells(all[i]);
        long long v = 0;
        GoldenLine g;
        if (c.size() < 11 || !ParseInt(c[0], v)) {
            err = Fmt(L"%ls: satır %zu bozuk / line %zu damaged", path.c_str(), i + 1, i + 1);
            return false;
        }
        g.id = static_cast<int>(v);
        g.code = c[1];
        g.key = c[2];
        long long n[4] = {};
        for (int k = 0; k < 2; ++k) ParseInt(c[static_cast<size_t>(3 + k)], n[k]);
        g.service = static_cast<int>(n[0]);
        g.rows = static_cast<int>(n[1]);
        if (!ParseHex32(c[5], g.rowHash) || !ParseHex32(c[6], g.tlHash)) {
            err = Fmt(L"%ls: satır %zu özet bozuk / line %zu bad digest", path.c_str(), i + 1, i + 1);
            return false;
        }
        long long x = 0;
        g.fx = ParseInt(c[7], x) ? static_cast<int>(x) : 0;
        g.snd = ParseInt(c[8], x) ? static_cast<int>(x) : 0;
        g.hit = ParseInt(c[9], x) ? static_cast<int>(x) : 0;
        g.totalMs = ParseInt(c[10], x) ? static_cast<int>(x) : 0;
        lines.push_back(std::move(g));
    }
    return true;
}

GoldenCompare CompareGolden(const std::vector<GoldenLine>& before, const std::vector<GoldenLine>& after) {
    GoldenCompare r;
    std::map<int, const GoldenLine*> a, b;
    for (const auto& l : before) b[l.id] = &l;
    for (const auto& l : after) a[l.id] = &l;
    for (const auto& [id, x] : b) {
        auto it = a.find(id);
        if (it == a.end()) {
            r.diffs.push_back({GoldenDiff::Removed, *x, {}, L"skill"});
            ++r.removed;
            continue;
        }
        const GoldenLine& y = *it->second;
        if (x->SameAs(y)) {
            ++r.same;
            continue;
        }
        std::wstring what;
        auto add = [&](const wchar_t* w) { what += (what.empty() ? L"" : L",") + std::wstring(w); };
        if (x->key != y.key) add(L"key");
        if (x->service != y.service) add(L"service");
        if (x->rows != y.rows || x->rowHash != y.rowHash) add(L"rows");
        if (x->tlHash != y.tlHash) add(L"timeline");
        r.diffs.push_back({GoldenDiff::Changed, *x, y, what});
        ++r.changed;
    }
    for (const auto& [id, y] : a)
        if (!b.count(id)) {
            r.diffs.push_back({GoldenDiff::Added, {}, *y, L"skill"});
            ++r.added;
        }
    return r;
}

std::wstring GoldenCompare::Text(bool en, size_t max) const {
    std::wstring s = Fmt(en ? L"golden: same=%d changed=%d added=%d removed=%d\r\n" : L"altın: aynı=%d değişen=%d eklenen=%d silinen=%d\r\n",
                         same, changed, added, removed);
    size_t n = 0;
    for (const auto& d : diffs) {
        if (n++ >= max) {
            s += Fmt(en ? L"  ... %zu more\r\n" : L"  ... %zu tane daha\r\n", diffs.size() - max);
            break;
        }
        const GoldenLine& l = d.kind == GoldenDiff::Added ? d.after : d.before;
        const wchar_t* k = d.kind == GoldenDiff::Added ? L"+" : d.kind == GoldenDiff::Removed ? L"-" : L"~";
        s += Fmt(L"  %ls %d %ls [%ls] %ls", k, l.id, l.code.c_str(), l.key.c_str(), d.what.c_str());
        if (d.kind == GoldenDiff::Changed && d.before.key != d.after.key) s += L" -> " + d.after.key;
        s += L"\r\n";
    }
    return s;
}

}  // namespace se::sp
