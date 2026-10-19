// SkillEffect Studio - skillplay/Diagnose.cpp
// "Efekt neden görünmüyor?" doctor (chain diagnosis + row fixes) and the pending-row model check (V13 / V14 / V17).
// Read-only: files come through callbacks, nothing is rendered or written.
#include "Diagnose.h"

#include "../core/SeModel.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace se::sp {

namespace {

// ------------------------------------------------------------------------------------------------ small helpers
std::wstring Join(const std::vector<std::wstring>& c) {
    std::wstring s;
    for (size_t i = 0; i < c.size(); ++i) {
        if (i) s += L'\t';
        s += c[i];
    }
    return s;
}
bool IsNoneTok(const std::wstring& s) { return s.empty() || s == L"none" || s == L"NONE" || s == L"None"; }
bool EffectOnlyKey(const std::wstring& k) {
    return StartsWith(k, L"EFFECT_") || StartsWith(k, L"PARAM_") || StartsWith(k, L"SYSTEM_") || StartsWith(k, L"STATUS_");
}
int SecBit(Sec s) { return 1 << static_cast<int>(s); }

size_t Lev(const std::wstring& a, const std::wstring& b) {
    std::vector<size_t> p(b.size() + 1), q(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) p[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        q[0] = i;
        for (size_t j = 1; j <= b.size(); ++j)
            q[j] = std::min({p[j] + 1, q[j - 1] + 1, p[j - 1] + (a[i - 1] == b[j - 1] ? 0u : 1u)});
        std::swap(p, q);
    }
    return p[b.size()];
}
std::wstring Nearest(const std::wstring& name, const std::vector<std::wstring>& cands) {
    const std::wstring u = ToUpper(name);
    std::wstring best;
    size_t bd = static_cast<size_t>(-1);
    for (const auto& c : cands) {
        if (c.empty() || c == L"*") continue;
        const size_t d = Lev(u, ToUpper(c));
        if (d < bd) {
            bd = d;
            best = c;
        }
    }
    return best;
}

std::wstring SaListText(const std::vector<std::wstring>& cells) {
    return L"#section skillaniset2\r\n" + Join(cells) + L"\r\n";
}
// a whole skilleffectset group of a key (the client's rows of the key are replaced by these)
std::wstring SeGroupText(const std::wstring& key, const std::vector<std::vector<std::wstring>>& rows) {
    std::wstring s = L"#replace " + key + L"\r\n#section skilleffectset\r\n";
    for (const auto& r : rows) s += Join(r) + L"\r\n";
    return s;
}
std::vector<std::vector<std::wstring>> CellsOf(const std::vector<const EffectRow*>& rows) {
    std::vector<std::vector<std::wstring>> out;
    for (const EffectRow* r : rows) out.push_back(r->cells);
    return out;
}
void SetCell(std::vector<std::wstring>& c, size_t i, const std::wstring& v) {
    if (c.size() <= i) c.resize(i + 1, L"none");
    c[i] = v;
}

// ------------------------------------------------------------------------------------------------ byte reader
struct Rd {
    const Bytes& b;
    size_t p = 0;
    bool bad = false;
    explicit Rd(const Bytes& d) : b(d) {}
    bool Need(size_t n) {
        if (bad || p + n > b.size()) bad = true;
        return !bad;
    }
    std::uint32_t U32() {
        if (!Need(4)) return 0;
        const std::uint32_t v = static_cast<std::uint32_t>(b[p]) | (static_cast<std::uint32_t>(b[p + 1]) << 8) |
                                (static_cast<std::uint32_t>(b[p + 2]) << 16) | (static_cast<std::uint32_t>(b[p + 3]) << 24);
        p += 4;
        return v;
    }
    std::uint8_t U8() { return Need(1) ? b[p++] : 0; }
    std::string Str() {
        const std::uint32_t n = U32();
        if (n > 4096 || !Need(n)) {
            bad = true;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(b.data() + p), n);
        p += n;
        return s;
    }
    void Skip(size_t n) {
        if (Need(n)) p += n;
    }
    void Seek(size_t at) {
        if (at > b.size()) bad = true;
        else p = at;
    }
};

// one ModData of a palette set (layout = BsrClips.cpp SkipModData); Sound bodies are counted, the rest skipped
bool ModData(Rd& r, std::uint32_t tag, int animType, const std::wstring& setName, std::vector<SoundSetInfo>& out) {
    r.Skip(28);
    switch (tag) {
        case 0x00000000u: {
            r.U32();
            const std::uint32_t flags = r.U32();
            r.U32();
            const std::uint32_t g = r.U32();
            if (g > 100000) return false;
            r.Skip(static_cast<size_t>(g) * 20);
            if (flags & 4u) {
                const std::uint32_t c = r.U32();
                if (c > 100000) return false;
                r.Skip(static_cast<size_t>(c) * 8);
            }
            r.Skip(36);
            break;
        }
        case 0x00010000u: r.Skip(5 * 4 + 64); break;
        case 0x00010001u:
        case 0x00010002u:
            r.U32();
            r.Str();
            r.U32();
            break;
        case 0x00030000u: {
            const std::uint32_t k = r.U32();
            if (k > 4096) return false;
            for (std::uint32_t i = 0; i < k && !r.bad; ++i) {
                r.U32();
                r.Str();
                r.Str();
                r.Skip(12 + 4 + 3);
                if (r.U8() == 1) r.Skip(12);
            }
            break;
        }
        case 0x00040000u: r.Skip(16); break;
        case 0x00040001u: {
            r.Skip(16 + 24);
            const std::uint32_t k = r.U32();
            if (k > 4096) return false;
            for (std::uint32_t i = 0; i < k && !r.bad; ++i)
                if (r.U8() != 0) r.Str();
            break;
        }
        case 0x00050000u: {
            const std::uint32_t sets = r.U32();
            if (sets == 0 || sets > 0x7FFFFFFFu) break;
            if (sets > 4096) return false;
            r.Skip(44);
            for (std::uint32_t i = 0; i < sets && !r.bad; ++i) {
                SoundSetInfo s;
                s.animType = animType;
                s.set = setName;
                s.name = FromAnsi(r.Str(), 949);
                const std::uint32_t t = r.U32();
                if (t > 8192) return false;
                s.tracks = static_cast<int>(t);
                for (std::uint32_t k = 0; k < t && !r.bad; ++k) {
                    if (r.U32() != 1) continue;
                    r.Str();
                    r.U32();
                    r.Str();
                }
                if (!r.bad) out.push_back(std::move(s));
            }
            break;
        }
        case 0x00060000u:
        case 0x00060001u:
        case 0x00060002u:
        case 0x00070000u: break;
        default: return false;
    }
    return !r.bad;
}

// ------------------------------------------------------------------------------------------------ model checks
struct Clips {
    bool hasSa = false;
    ClipInfo ready, wait;
    bool readyAsked = false, waitAsked = false;
    std::vector<ClipInfo> shots;   // found shot clips
    std::vector<int> shotMissing;  // shot roles the model does not have
};
Clips ClipsOf(const AniSetRow* sa, const IModelInfo* m, const std::wstring& weapon) {
    Clips c;
    if (!sa || !m) return c;
    c.hasSa = true;
    auto get = [&](int role, ClipInfo& ci) { return role >= 0 && m->ClipIn(role, sa->aniGroup, weapon, ci) && ci.found; };
    if (sa->ready >= 0) {
        c.readyAsked = true;
        if (!get(sa->ready, c.ready)) c.ready = ClipInfo{};
    }
    if (sa->wait >= 0) {
        c.waitAsked = true;
        if (!get(sa->wait, c.wait)) c.wait = ClipInfo{};
    }
    for (int role : sa->shot) {
        ClipInfo ci;
        if (get(role, ci)) c.shots.push_back(ci);
        else c.shotMissing.push_back(role);
    }
    return c;
}
// events of the clip a row's StartEvent counts in; -1 = no clip to judge (missing clip: the clip step reports it)
int EventsFor(const EffectRow& r, const Clips& c) {
    if (r.phase == Phase::Ready) return c.ready.found ? static_cast<int>(c.ready.events.size()) : -1;
    if (r.phase == Phase::Wait) return c.wait.found ? static_cast<int>(c.wait.events.size()) : -1;
    if (r.phase == Phase::Shot) {
        if (c.shots.empty()) return -1;
        size_t n = c.shots[0].events.size();
        for (const auto& s : c.shots) n = std::min(n, s.events.size());  // a comma AniShot list: every pick must have it
        return static_cast<int>(n);
    }
    return -1;
}
struct BadEvent {
    int row = -1;
    int k = 0, n = 0;
};
std::vector<BadEvent> BadEvents(const std::vector<const EffectRow*>& rows, const Clips& c, bool pendingOnly) {
    std::vector<BadEvent> out;
    if (!c.hasSa) return out;
    for (size_t i = 0; i < rows.size(); ++i) {
        const EffectRow& r = *rows[i];
        if (pendingOnly && r.origin != Origin::Pending) continue;
        if (r.startEvent <= 0) continue;
        const int n = EventsFor(r, c);
        if (n < 0) continue;
        if (r.startEvent > n) out.push_back({static_cast<int>(i), r.startEvent, n});
    }
    return out;
}
struct BadBone {
    int row = -1;
    size_t cell = 19;  // 19 StartBone, 21 TargetBone
    int actor = 0;
    std::wstring bone;
};
std::vector<BadBone> BadBones(const Timeline& tl, const IModelInfo* caster, const IModelInfo* target, bool pendingOnly) {
    std::vector<BadBone> out;
    std::set<std::pair<int, size_t>> seen;
    auto test = [&](int row, int actor, const std::wstring& bone, size_t cell) {
        if (bone.empty() || bone == L"*") return;
        const IModelInfo* m = actor == 0 ? caster : target;
        if (!m || m->HasBone(bone)) return;
        if (!seen.insert({row, cell}).second) return;
        out.push_back({row, cell, actor, bone});
    };
    for (const FxEvent& e : tl.fx) {
        if (e.copy != 0 || e.row < 0 || static_cast<size_t>(e.row) >= tl.rows.size()) continue;
        const EffectRow& r = *tl.rows[static_cast<size_t>(e.row)];
        if (pendingOnly && r.origin != Origin::Pending) continue;
        if (e.place == Place::Projectile) {
            if (e.fromTargetSpace) continue;  // both ends are offsets around the target
            const bool swapped = r.phase == Phase::SReturn || (r.mov == Mov::Hwan && r.act == Act::Source);
            test(e.row, e.fromActor, e.fromBone, swapped ? 21 : 19);
            if (r.act != Act::MovOption) test(e.row, e.toActor, e.toBone, swapped ? 19 : 21);
        } else if (e.place != Place::DmgPos) {
            test(e.row, e.actor, e.bone, (e.place == Place::Target || e.place == Place::TargetFollow) ? 21 : 19);
        }
    }
    return out;
}
std::vector<std::wstring> BoneNames(const IModelInfo* m, const std::vector<std::wstring>& given) {
    if (!given.empty()) return given;
    std::vector<std::wstring> out;
    if (const auto* b = dynamic_cast<const BsrModel*>(m)) out.assign(b->bones.begin(), b->bones.end());
    return out;
}

// an AniShot role the model has (first attack / skill clip), -1 = none
int SomeShotRole(const IModelInfo* m, const AniSetRow* sa, const std::wstring& weapon) {
    if (!m) return -1;
    const std::wstring group = sa ? sa->aniGroup : std::wstring(L"DEFAULT");
    for (int role = 0; role < 256; ++role) {
        const std::wstring t = AniTokenOf(role);
        if (!StartsWith(t, L"ANI_ATTACK") && !StartsWith(t, L"ANI_SKILL_")) continue;
        ClipInfo ci;
        if (m->ClipIn(role, group, weapon, ci) && ci.found) return role;
    }
    return -1;
}

DiagItem& St(ChainReport& rep, DiagStep s) { return rep.steps[static_cast<size_t>(s)]; }
void Ok(DiagItem& d, const std::wstring& tr, const std::wstring& en) {
    d.state = DiagState::Ok;
    d.sev = Sev::Info;
    d.tr = tr;
    d.en = en;
}
void Fail(DiagItem& d, Sev sev, const wchar_t* rule, const std::wstring& tr, const std::wstring& en) {
    d.state = DiagState::Fail;
    d.sev = sev;
    d.rule = rule;
    d.tr = tr;
    d.en = en;
}
void Skip(DiagItem& d, const std::wstring& tr, const std::wstring& en, DiagState st = DiagState::Skip) {
    d.state = st;
    d.sev = Sev::Info;
    d.tr = tr;
    d.en = en;
}
void SetFix(DiagItem& d, DiagFix f, const std::wstring& key, int secs, const std::wstring& list, const std::wstring& tr,
            const std::wstring& en) {
    d.fix = f;
    d.fixKey = key;
    d.fixSecs = secs;
    d.fixList = list;
    d.fixTr = tr;
    d.fixEn = en;
}
void Note(ChainReport& rep, DiagStep s, Sev sev, const wchar_t* rule, const std::wstring& tr, const std::wstring& en) {
    DiagItem d;
    d.step = s;
    d.state = sev == Sev::Info ? DiagState::Ok : DiagState::Fail;
    d.sev = sev;
    d.rule = rule;
    d.tr = tr;
    d.en = en;
    rep.notes.push_back(std::move(d));
}

std::vector<std::wstring> GenSaCells(const std::wstring& key, int shotRole, bool attack) {
    return {L"1",    key,    key,    L"0",    L"FALSE", L"0",    L"DEFAULT",
            L"none", L"none", shotRole >= 0 ? AniTokenOf(shotRole) : std::wstring(L"none"),
            L"none", L"none", L"none", L"none", L"hiteffect\\hit_3_normal.efp", L"0", L"0,0,0,0", L"INVSRCALPHA",
            L"none", L"none", L"none", L"none", L"none", L"none", L"none", attack ? L"1" : L"0", L"1"};
}

}  // namespace

// ----------------------------------------------------------------------------------------------- hidden rows
bool HiddenRows::Build(const Bytes& raw, std::wstring& err) {
    m_rows.clear();
    m_byKey.clear();
    m_eofLine = 0;
    SeDoc doc;
    if (!doc.Load(raw, err)) return false;
    const size_t n = doc.lines.size();
    size_t eofStart = n;
    if (doc.sim.eofInBlock && n) {
        size_t i = n;
        while (i > 0 && doc.sim.blockAfter[i - 1]) --i;
        eofStart = i;  // the line that opened the unclosed block
        m_eofLine = static_cast<int>(eofStart) + 1;
    }
    for (size_t i = 0; i < n; ++i) {
        if (!doc.sim.dropped[i]) continue;
        auto tok = ClientTokens(doc.Line(i));
        if (tok.empty() || StartsWith(tok[0], L"#") || StartsWith(tok[0], L"/")) continue;
        const Sec s = GuessSection(tok);
        if (s != Sec::CI && s != Sec::SA && s != Sec::SE) continue;
        const size_t kc = static_cast<size_t>(KeyColumn(s));
        if (tok.size() <= kc || tok[kc].empty()) continue;
        HiddenRow h;
        h.sec = s;
        h.line = static_cast<int>(i) + 1;
        h.eofBlock = i >= eofStart;
        h.key = tok[kc];
        h.cells = std::move(tok);
        m_byKey.emplace(h.key, m_rows.size());
        m_rows.push_back(std::move(h));
    }
    return true;
}

std::vector<const HiddenRow*> HiddenRows::Of(const std::wstring& key) const {
    std::vector<const HiddenRow*> out;
    auto [a, b] = m_byKey.equal_range(key);
    for (auto it = a; it != b; ++it) out.push_back(&m_rows[it->second]);
    std::sort(out.begin(), out.end(), [](const HiddenRow* x, const HiddenRow* y) { return x->line < y->line; });
    return out;
}

// --------------------------------------------------------------------------------------------- .bsr sound sets
std::vector<SoundSetInfo> BsrSoundSets(const Bytes& b, bool* ok) {
    std::vector<SoundSetInfo> out;
    if (ok) *ok = false;
    if (b.size() < 44 || std::memcmp(b.data(), "JMXVRES 0109", 12) != 0) return out;
    Rd h(b);
    h.Seek(36);
    const std::uint32_t off = h.U32();
    if (off == 0) {
        if (ok) *ok = true;
        return out;
    }
    if (off >= b.size()) return out;
    Rd r(b);
    r.Seek(off);
    for (int list = 0; list < 2 && !r.bad; ++list) {
        const std::uint32_t n = r.U32();
        if (r.bad || n > 4096) return out;
        for (std::uint32_t i = 0; i < n && !r.bad; ++i) {
            r.U32();  // kind
            const int animType = static_cast<int>(r.U32());
            const std::wstring setName = FromAnsi(r.Str(), 949);
            const std::uint32_t items = r.U32();
            if (r.bad || items > 4096) return out;
            for (std::uint32_t j = 0; j < items && !r.bad; ++j)
                if (!ModData(r, r.U32(), list == 1 ? animType : -1, setName, out)) return out;
        }
    }
    if (ok) *ok = !r.bad;
    return out;
}

// --------------------------------------------------------------------------------------------------- names
const wchar_t* DiagStepId(DiagStep s) {
    static const wchar_t* ids[] = {L"enc",  L"key",   L"comment", L"aniset",  L"order",    L"clip", L"event",
                                   L"bone", L"files", L"efplive", L"charinfo", L"sound",   L"scale"};
    const int i = static_cast<int>(s);
    return i >= 0 && i < static_cast<int>(DiagStep::Count) ? ids[i] : L"?";
}

std::wstring DiagStepName(DiagStep s, bool en) {
    static const wchar_t* tr[] = {L"Skill istemcide tanımlı (skilldata)",
                                  L"Efekt adı doğru (grup / kod)",
                                  L"Satırlar yorumun dışında (/* */)",
                                  L"Animasyon satırı açık (skillaniset2)",
                                  L"Animasyon satırı efektlerden önce",
                                  L"Animasyon modelde var",
                                  L"Başlama anı animasyonda var (StartEvent)",
                                  L"Bağlandığı kemik modelde var",
                                  L"Efekt dosyaları istemcide",
                                  L"Efekt ilk 2 saniyede görünür",
                                  L"Görünüm satırı (characterInfo)",
                                  L"Sesler (en çok 15 iz, dosyalar)",
                                  L"Boyut sıfırdan büyük"};
    static const wchar_t* eng[] = {L"skilldata ENC definition",
                                   L"Presentation key (group / code)",
                                   L"Rows outside /* */",
                                   L"skillaniset2 row, Service 1",
                                   L"skillaniset2 row before the effect rows",
                                   L"Animation clip in the model",
                                   L"StartEvent ≤ events in the clip",
                                   L"Bones in the skeleton",
                                   L"Effect files exist",
                                   L".efp shows something in its first 2 s",
                                   L"characterInfo row",
                                   L"Sound (tracks ≤ 15, files)",
                                   L"Scale > 0"};
    const int i = static_cast<int>(s);
    if (i < 0 || i >= static_cast<int>(DiagStep::Count)) return L"?";
    return en ? eng[i] : tr[i];
}

const wchar_t* DiagFixId(DiagFix f) {
    switch (f) {
        case DiagFix::None: return L"none";
        case DiagFix::ServiceOn: return L"service-on";
        case DiagFix::MoveOutOfComment: return L"move-out";
        case DiagFix::Reorder: return L"reorder";
        case DiagFix::ClampEvent: return L"clamp-event";
        case DiagFix::ReplaceBone: return L"replace-bone";
        case DiagFix::Rekey: return L"rekey";
        case DiagFix::GenSa: return L"gen-sa";
        case DiagFix::PickClip: return L"pick-clip";
        case DiagFix::ReplaceFile: return L"replace-file";
        case DiagFix::AddCharInfo: return L"add-charinfo";
        case DiagFix::FixScale: return L"fix-scale";
        case DiagFix::Manual: return L"manual";
    }
    return L"?";
}

std::wstring ChainReport::Text(bool en) const {
    std::wstring s = (en ? L"key " : L"anahtar ") + key + (code.empty() ? L"" : L" (" + code + L")") + L"\r\n";
    for (const auto& d : steps) {
        const wchar_t* mark = d.state == DiagState::Ok ? L"✓" : d.state == DiagState::Fail ? (d.sev == Sev::Info ? L"i" : L"✗")
                              : d.state == DiagState::Skip ? L"–" : L"·";
        s += Fmt(L"%ls %-8ls %ls", mark, DiagStepId(d.step), DiagStepName(d.step, en).c_str());
        if (!d.Text(en).empty()) s += L": " + d.Text(en);
        if (d.state == DiagState::Fail && d.fix != DiagFix::None)
            s += Fmt(L" [%ls] %ls", DiagFixId(d.fix), (en ? d.fixEn : d.fixTr).c_str());
        s += L"\r\n";
    }
    for (const auto& n : notes) s += Fmt(L"  %ls %ls: %ls\r\n", n.sev == Sev::Info ? L"i" : L"!", DiagStepId(n.step), n.Text(en).c_str());
    if (const DiagItem* f = First())
        s += (en ? L"first broken link: " : L"ilk kırık halka: ") + std::wstring(DiagStepId(f->step)) + L"\r\n";
    else
        s += en ? L"healthy\r\n" : L"sağlam\r\n";
    return s;
}

// --------------------------------------------------------------------------------------------------- doctor
ChainReport Diagnose(const DiagInput& in) {
    ChainReport rep;
    rep.steps.resize(static_cast<size_t>(DiagStep::Count));
    for (size_t i = 0; i < rep.steps.size(); ++i) {
        rep.steps[i].step = static_cast<DiagStep>(i);
        Skip(rep.steps[i], L"denetlenmedi", L"not checked");
    }
    if (!in.db) return rep;
    const FxDb& db = *in.db;
    const SkillInfo* sk = (in.skill && in.skill->valid) ? in.skill : nullptr;
    rep.code = sk ? sk->code : std::wstring();
    const std::wstring expected = sk ? db.PresentationKey(*sk, in.byId) : std::wstring();
    rep.key = !in.key.empty() ? in.key : expected;
    std::wstring rowsKey = rep.key;  // the key whose rows are judged (the code key while a rekey is pending)
    auto hiddenOf = [&](const std::wstring& k) { return in.hidden ? in.hidden->Of(k) : std::vector<const HiddenRow*>{}; };
    auto hasRows = [&](const std::wstring& k) { return db.AniSet(k) || !db.Effects(k).empty() || !hiddenOf(k).empty(); };

    // ------------------------------------------------------------------ enc
    {
        DiagItem& d = St(rep, DiagStep::Enc);
        const SkillInfo* named = (!sk && in.byName && !rep.key.empty()) ? in.byName(rep.key) : nullptr;
        if (sk || (named && named->valid)) {
            const SkillInfo* s = sk ? sk : named;
            Ok(d, Fmt(L"Skill istemcinin skill tablosunda var: %ls (ID %d).", s->code.c_str(), s->id), Fmt(L"In skilldata ENC: %ls (ID %d).", s->code.c_str(), s->id));
        } else if (EffectOnlyKey(rep.key)) {
            Skip(d, L"Efekt anahtarı: skill gerekmez.", L"Effect key: no skill needed.", DiagState::NotApplicable);
        } else if ((in.skill && !in.skill->valid) || in.byName) {
            Fail(d, Sev::Error, L"V8", L"skilldata ENC'de bu anahtarın skilli yok: istemci anahtara sahte kimlik verir, efekt hiç görünmez.",
                 L"No skill for this key in skilldata ENC: the client gives the key a synthetic id, the effect never shows.");
            SetFix(d, DiagFix::Manual, rep.key, 0, L"", L"skilldata (ENC) satırını ekleyin (Aktar / paket).",
                   L"Add the skilldata (ENC) row (Transfer / package).");
        } else {
            Skip(d, L"skilldata verilmedi.", L"No skilldata given.");
        }
    }
    if (rep.key.empty()) {
        Fail(St(rep, DiagStep::Key), Sev::Error, L"V8", L"Anahtar yok.", L"No key.");
        rep.firstBroken = static_cast<int>(DiagStep::Key);
        return rep;
    }

    // ------------------------------------------------------------------ key (group or code)
    {
        DiagItem& d = St(rep, DiagStep::Key);
        if (!sk) {
            if (hasRows(rep.key)) Ok(d, L"Anahtarın satırları var: " + rep.key, L"The key has rows: " + rep.key);
            else Skip(d, L"skill verilmedi: anahtar denetlenmedi.", L"No skill given: key not checked.");
        } else {
            const bool group = !sk->group.empty() && sk->group != L"xxx";
            auto rekey = [&](const std::wstring& from, const std::wstring& to) {
                std::wstring list;
                int secs = 0;
                if (const AniSetRow* sa = db.AniSet(from)) {
                    auto c = sa->cells;
                    SetCell(c, 2, to);
                    list += SaListText(c);
                    secs |= SecBit(Sec::SA);
                }
                auto rows = CellsOf(db.Effects(from));
                for (auto& c : rows) SetCell(c, 1, to);
                if (!rows.empty()) {
                    list += SeGroupText(to, rows);
                    secs |= SecBit(Sec::SE);
                }
                SetFix(d, DiagFix::Rekey, to, secs, list, L"Satırlar " + to + L" anahtarıyla bekleyen listeye kopyalanır.",
                       L"The rows are copied to the pending list under the key " + to + L".");
            };
            // a code key still resolves to this one skill (SKILLEFFECT_LOGIC 3.2 name map 2), but the group key is what
            // binds every skill of the group (levels / chain steps): rows under the code while a group exists = Warn
            auto codeInsteadOfGroup = [&]() {
                Fail(d, Sev::Warn, L"V8",
                     L"Skillin grubu " + sk->group + L", satırlar ise kod anahtarında (" + sk->code +
                         L"): istemci yalnız bu skille bağlar; grubun öbür skilleri (seviyeler / zincir adımları) efektsiz kalır.",
                     L"The skill's group is " + sk->group + L" but the rows are under the code key (" + sk->code +
                         L"): the client binds them to this skill only; the other skills of the group (levels / chain steps) show nothing.");
            };
            if (!in.key.empty() && in.key != expected) {
                if (group && in.key == sk->code) {
                    codeInsteadOfGroup();
                } else {
                    Fail(d, Sev::Error, L"V8",
                         L"Satırlar " + in.key + L" altında, istemci ise " + expected + L" anahtarına bakar: efekt görünmez.",
                         L"The rows are under " + in.key + L" but the client looks up " + expected + L": the effect never shows.");
                }
                if (hasRows(in.key)) rekey(in.key, expected);
            } else if (!hasRows(expected) && group && sk->code != expected && hasRows(sk->code)) {
                codeInsteadOfGroup();
                rekey(sk->code, expected);
                rowsKey = sk->code;
            } else {
                const std::wstring how = expected == (group ? sk->group : sk->code)
                                             ? (group ? std::wstring(L"grup") : std::wstring(L"kod"))
                                             : std::wstring(L"Basic_Original");
                const std::wstring howEn = how == L"grup" ? L"group" : how == L"kod" ? L"code" : L"Basic_Original";
                Ok(d, L"İstemci efekt satırlarını " + expected + L" adıyla arar (" + how + L").", L"The client looks up " + expected + L" (" + howEn + L").");
            }
        }
    }

    const AniSetRow* sa = db.AniSet(rowsKey);
    const std::vector<const EffectRow*> rows = db.Effects(rowsKey);
    PlayOptions po = in.play;
    if (po.casterCi.empty()) po.casterCi = in.casterCi;
    const std::wstring& weapon = po.weaponGroup;
    auto rewriteGroup = [&](DiagItem& d, DiagFix f, std::vector<std::vector<std::wstring>> cells, const std::wstring& tr,
                            const std::wstring& en) {
        SetFix(d, f, rowsKey, SecBit(Sec::SE), SeGroupText(rowsKey, cells), tr, en);
    };
    // a changed skillaniset2 row: a client row is replaced together with the key's effect group ("#replace KEY" covers
    // every section of the key in the merge; the effect rows are carried along unchanged)
    auto saFix = [&](DiagItem& d, DiagFix f, const std::vector<std::wstring>& saCells, const std::wstring& tr, const std::wstring& en) {
        if (sa && sa->origin == Origin::Client && !rows.empty())
            SetFix(d, f, rowsKey, SecBit(Sec::SA) | SecBit(Sec::SE), SaListText(saCells) + SeGroupText(rowsKey, CellsOf(rows)), tr, en);
        else
            SetFix(d, f, rowsKey, SecBit(Sec::SA), SaListText(saCells), tr, en);
    };

    // ------------------------------------------------------------------ comment
    {
        DiagItem& d = St(rep, DiagStep::Comment);
        if (!in.hidden) {
            Skip(d, L"skilleffect.txt ham metni verilmedi.", L"Raw skilleffect.txt not given.");
        } else {
            const auto hid = hiddenOf(rowsKey);
            std::vector<const HiddenRow*> hidSa, uncovered;
            for (const HiddenRow* h : hid) {
                if (h->sec == Sec::SA) hidSa.push_back(h);
                if (h->sec != Sec::SE) continue;
                bool cov = false;
                for (const EffectRow* r : rows) cov = cov || r->cells == h->cells;
                if (!cov) uncovered.push_back(h);
            }
            const bool saLost = !hidSa.empty() && !sa;
            const bool seLost = !uncovered.empty() && rows.empty();
            bool eof = false;
            for (const HiddenRow* h : uncovered) eof = eof || h->eofBlock;
            for (const HiddenRow* h : hidSa) eof = eof || (saLost && h->eofBlock);
            const HiddenRow* first = saLost ? hidSa.front() : (!uncovered.empty() ? uncovered.front() : nullptr);
            if (saLost || seLost || (eof && first)) {
                const int nLost = static_cast<int>((saLost ? 1 : 0) + uncovered.size());
                const std::wstring where = first->eofBlock ? Fmt(L"dosya sonundaki kapanmamış /* bloğunda (satır %d'den beri)", in.hidden->EofBlockLine())
                                                           : std::wstring(L"/* */ yorum bloğunda");
                const std::wstring whereEn = first->eofBlock ? Fmt(L"in the /* block left open at the end of the file (since line %d)", in.hidden->EofBlockLine())
                                                             : std::wstring(L"in a /* */ comment block");
                Fail(d, (saLost || seLost) ? Sev::Error : Sev::Warn, L"V2",
                     Fmt(L"Bu anahtarın %d satırı %ls: istemci bu satırları hiç okumaz.", nLost, where.c_str()),
                     Fmt(L"%d rows of this key are %ls: the client never reads them.", nLost, whereEn.c_str()));
                d.fileLine = first->line;
                d.count = nLost;
                std::wstring list;
                int secs = 0;
                if (saLost) {
                    list += SaListText(hidSa.back()->cells);  // the client would keep the last one
                    secs |= SecBit(Sec::SA);
                }
                if (!uncovered.empty()) {
                    // effective rows + the hidden ones, in file order (pending rows after the client's)
                    std::vector<std::pair<int, std::vector<std::wstring>>> all;
                    for (const EffectRow* r : rows) all.push_back({r->origin == Origin::Client ? r->line : 1000000000 + r->line, r->cells});
                    for (const HiddenRow* h : uncovered) all.push_back({h->line, h->cells});
                    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                    std::vector<std::vector<std::wstring>> cells;
                    for (auto& a : all) cells.push_back(std::move(a.second));
                    list += SeGroupText(rowsKey, cells);
                    secs |= SecBit(Sec::SE);
                }
                SetFix(d, DiagFix::MoveOutOfComment, rowsKey, secs, list,
                       L"Satırlar bekleyen listeye alınır; birleştirme onları dosyanın etkin bölümüne yazar.",
                       L"The rows go to the pending list; the merge writes them into the active part of the file.");
            } else {
                Ok(d, L"Anahtarın satırları istemcinin okuduğu bölümde.", L"The key's rows are in the part the client reads.");
                if (!hid.empty())
                    Note(rep, DiagStep::Comment, Sev::Info, L"V2",
                         Fmt(L"Yorum bloklarında bu anahtarın %zu eski satırı daha var (istemci okumaz, etkin satırlar yeterli).", hid.size()),
                         Fmt(L"Comment blocks hold %zu more old rows of this key (not read; the active rows are enough).", hid.size()));
            }
        }
    }

    // ------------------------------------------------------------------ aniset
    int shotPick = -1;
    {
        DiagItem& d = St(rep, DiagStep::AniSet);
        bool attack = false;
        for (const EffectRow* r : rows) attack = attack || r->dmg == 1;
        if (!sa) {
            if (rows.empty()) {
                Fail(d, Sev::Error, L"V7", L"Bu anahtar için ne skillaniset2 ne skilleffectset satırı var: gösterilecek bir şey yok.",
                     L"The key has neither a skillaniset2 nor a skilleffectset row: nothing to show.");
                SetFix(d, DiagFix::Manual, rowsKey, 0, L"", L"Satırları listeye ekleyin (Aktar / şablon).", L"Add the rows to the list (Transfer / template).");
            } else if (EffectOnlyKey(rowsKey)) {
                Ok(d, L"Efekt anahtarı: skillaniset2 satırı gerekmez.", L"Effect key: no skillaniset2 row needed.");
            } else {
                Fail(d, Sev::Error, L"V7", L"skillaniset2 satırı yok: saldırı animasyonu oynamaz, efektler animasyon olaylarına bağlanamaz.",
                     L"No skillaniset2 row: no attack animation plays, the effects cannot attach to its events.");
                shotPick = SomeShotRole(in.caster, nullptr, weapon);
                SetFix(d, DiagFix::GenSa, rowsKey, SecBit(Sec::SA),
                       SaListText(GenSaCells(rowsKey, shotPick >= 0 ? shotPick : AniRole(L"ANI_ATTACK1"), attack)),
                       L"Asgari bir skillaniset2 satırı bekleyen listeye eklenir (" +
                           (shotPick >= 0 ? AniTokenOf(shotPick) : std::wstring(L"ANI_ATTACK1")) + L").",
                       L"A minimal skillaniset2 row is added to the pending list (" +
                           (shotPick >= 0 ? AniTokenOf(shotPick) : std::wstring(L"ANI_ATTACK1")) + L").");
            }
        } else if (!sa->service) {
            Fail(d, Sev::Error, L"V7", L"Animasyon satırı kapalı (skillaniset2 Service = 0): istemci bu satırı ve skillin efekt satırlarını hiç kullanmaz.",
                 L"skillaniset2 Service = 0: the client ignores the row and drops the key's effect rows.");
            d.fileLine = sa->origin == Origin::Client ? sa->line : -1;
            d.listLine = sa->origin == Origin::Pending ? sa->line : -1;
            auto c = sa->cells;
            SetCell(c, 0, L"1");
            saFix(d, DiagFix::ServiceOn, c, L"Service 1 yapılmış satır bekleyen listeye konur.", L"The row with Service 1 goes to the pending list.");
        } else {
            Ok(d, Fmt(L"Animasyon satırı var ve açık (%ls %d, Service 1).", sa->origin == Origin::Client ? L"satır" : L"liste satırı", sa->line),
               Fmt(L"skillaniset2 row present (%ls %d), Service 1.", sa->origin == Origin::Client ? L"line" : L"list line", sa->line));
        }
    }

    // ------------------------------------------------------------------ order (V9)
    {
        DiagItem& d = St(rep, DiagStep::Order);
        if (!sa || rows.empty()) {
            Skip(d, L"Karşılaştırılacak iki bölüm yok.", L"Nothing to compare.", DiagState::NotApplicable);
        } else {
            int minSe = -1;
            for (const EffectRow* r : rows)
                if (r->origin == Origin::Client && (minSe < 0 || r->line < minSe)) minSe = r->line;
            if (sa->origin == Origin::Client && minSe > 0 && minSe < sa->line) {
                Fail(d, Sev::Error, L"V9",
                     Fmt(L"Efekt satırı (satır %d) skillaniset2 satırından (satır %d) önce: istemcide satırlar sahipsiz kalır, assert verebilir.", minSe, sa->line),
                     Fmt(L"An effect row (line %d) comes before the skillaniset2 row (line %d): the rows are orphaned in the client (assert).", minSe, sa->line));
                d.fileLine = minSe;
                SetFix(d, DiagFix::Reorder, rowsKey, SecBit(Sec::SA) | SecBit(Sec::SE),
                       L"#remove " + rowsKey + L"\r\n" + SaListText(sa->cells) + SeGroupText(rowsKey, CellsOf(rows)),
                       L"Anahtarın satırları kaldırılıp doğru sırayla yeniden yazılır.", L"The key's rows are removed and written again in the right order.");
            } else {
                Ok(d, L"Animasyon satırı efekt satırlarından önce geliyor.", L"The skillaniset2 row comes before the effect rows.");
            }
        }
    }

    // ------------------------------------------------------------------ clip
    const Clips clips = ClipsOf(sa, in.caster, weapon);
    {
        DiagItem& d = St(rep, DiagStep::Clip);
        if (!sa) {
            Skip(d, L"skillaniset2 satırı yok.", L"No skillaniset2 row.", DiagState::NotApplicable);
        } else if (!in.caster) {
            Skip(d, L"Model verilmedi.", L"No model given.");
        } else {
            bool shotRows = false;
            for (const EffectRow* r : rows) shotRows = shotRows || r->phase == Phase::Shot;
            std::vector<std::wstring> missingFiles;
            for (const ClipInfo* c : {&clips.ready, &clips.wait})
                if (c->found && !c->file.empty() && in.fileExists && !in.fileExists(ResArc::Data, c->file)) missingFiles.push_back(c->file);
            for (const auto& c : clips.shots)
                if (!c.file.empty() && in.fileExists && !in.fileExists(ResArc::Data, c.file)) missingFiles.push_back(c.file);
            auto pickFix = [&]() {
                const int role = SomeShotRole(in.caster, sa, weapon);
                if (role < 0) {
                    SetFix(d, DiagFix::Manual, rowsKey, 0, L"", L"Modele saldırı animasyonu ekleyin.", L"Add an attack clip to the model.");
                    return;
                }
                auto c = sa->cells;
                SetCell(c, 9, AniTokenOf(role));
                saFix(d, DiagFix::PickClip, c, L"AniShot modelde olan " + AniTokenOf(role) + L" yapılır.",
                      L"AniShot becomes " + AniTokenOf(role) + L", which the model has.");
            };
            if (sa->shot.empty() && !IsNoneTok(sa->shotTok)) {
                Fail(d, Sev::Error, L"V10", L"AniShot tanınmıyor (" + sa->shotTok + L"): istemci animasyonu bulamaz.",
                     L"AniShot not recognised (" + sa->shotTok + L"): the client finds no animation.");
                pickFix();
            } else if (!sa->shot.empty() && clips.shots.empty()) {
                Fail(d, Sev::Error, L"V13",
                     L"Model " + AniTokenOf(sa->shot[0]) + L" animasyonunu içermiyor (" + sa->aniGroup +
                         L" grubu): saldırı oynamaz, olaylara bağlı efekt ve hasar gelmez.",
                     L"The model has no " + AniTokenOf(sa->shot[0]) + L" clip (" + sa->aniGroup +
                         L" group): no attack plays, effects and damage tied to its events never come.");
                pickFix();
            } else if (!missingFiles.empty()) {
                Fail(d, Sev::Error, L"FILE", L"Animasyon dosyası istemcide yok: " + missingFiles[0], L"Animation file missing in the client: " + missingFiles[0]);
                d.count = static_cast<int>(missingFiles.size());
                SetFix(d, DiagFix::Manual, rowsKey, 0, L"", L"Animasyonu (.ban) Data'ya ekleyin.", L"Add the clip (.ban) to Data.");
            } else if (!clips.shotMissing.empty()) {
                Fail(d, Sev::Warn, L"V13", L"AniShot listesindeki " + AniTokenOf(clips.shotMissing[0]) + L" modelde yok: o seçimde saldırı oynamaz.",
                     L"The AniShot list's " + AniTokenOf(clips.shotMissing[0]) + L" is not in the model: that pick plays no attack.");
            } else if ((clips.readyAsked && !clips.ready.found) || (clips.waitAsked && !clips.wait.found)) {
                const std::wstring tok = clips.readyAsked && !clips.ready.found ? sa->readyTok : sa->waitTok;
                Fail(d, Sev::Warn, L"V13", L"Hazırlık animasyonu " + tok + L" modelde yok: canavar hazırlıkta bekler.",
                     L"The preparation clip " + tok + L" is not in the model: the creature stands during the preparation.");
            } else if (sa->shot.empty() && shotRows) {
                Fail(d, Sev::Warn, L"V13", L"AniShot yok ama SHOT efekt satırları var: saldırı animasyonu olmadan olaylar gelmez.",
                     L"No AniShot but there are SHOT effect rows: without an attack clip no events come.");
                pickFix();
            } else {
                std::wstring what = sa->shot.empty() ? std::wstring(L"(AniShot yok)") : AniTokenOf(sa->shot[0]);
                Ok(d, L"Animasyon modelde var: " + what + (clips.shots.empty() ? L"" : Fmt(L" (%zu vuruş anı)", clips.shots[0].events.size())),
                   L"Clip in the model: " + what + (clips.shots.empty() ? L"" : Fmt(L" (%zu events)", clips.shots[0].events.size())));
            }
        }
    }

    // ------------------------------------------------------------------ event
    {
        DiagItem& d = St(rep, DiagStep::Event);
        if (!sa || rows.empty()) {
            Skip(d, L"Satır yok.", L"No rows.", DiagState::NotApplicable);
        } else if (!in.caster) {
            Skip(d, L"Model verilmedi.", L"No model given.");
        } else {
            const auto bad = BadEvents(rows, clips, false);
            if (!bad.empty()) {
                const EffectRow& r0 = *rows[static_cast<size_t>(bad[0].row)];
                Fail(d, Sev::Error, L"V13",
                     bad[0].n == 0 ? Fmt(L"Satır animasyonun %d. vuruş anını bekliyor ama %ls animasyonunda hiç vuruş anı yok: bu efekt hiç başlamaz (StartEvent).", bad[0].k, PhaseName(r0.phase))
                                   : Fmt(L"Satır animasyonun %d. vuruş anını bekliyor ama %ls animasyonunda yalnız %d vuruş anı var: bu efekt hiç başlamaz (StartEvent).", bad[0].k, PhaseName(r0.phase), bad[0].n),
                     bad[0].n == 0 ? Fmt(L"StartEvent %d but the %ls clip has no type-1 event: the row never fires.", bad[0].k, PhaseName(r0.phase))
                                   : Fmt(L"StartEvent %d but the %ls clip has %d events: the row never fires.", bad[0].k, PhaseName(r0.phase), bad[0].n));
                d.row = bad[0].row;
                d.count = static_cast<int>(bad.size());
                (r0.origin == Origin::Client ? d.fileLine : d.listLine) = r0.line;
                auto cells = CellsOf(rows);
                for (const auto& b : bad) SetCell(cells[static_cast<size_t>(b.row)], 3, std::to_wstring(b.n));
                rewriteGroup(d, DiagFix::ClampEvent, std::move(cells), L"Başlama anı, animasyonun son vuruş anına çekilir.",
                             L"StartEvent is lowered to the clip's event count.");
            } else {
                Ok(d, L"Her efektin başlama anı animasyonda var.", L"Every StartEvent is within the clip's events.");
            }
            int dmgRows = 0;
            for (const EffectRow* r : rows) dmgRows += (r->phase == Phase::Shot && r->dmg == 1) ? 1 : 0;
            const int n = clips.shots.empty() ? -1 : static_cast<int>(clips.shots[0].events.size());
            if (n >= 0 && dmgRows > n)
                Note(rep, DiagStep::Event, Sev::Warn, L"V13", Fmt(L"%d DMG satırı, saldırı klibinde %d olay: fazla vuruşlar aynı anda düşer.", dmgRows, n),
                     Fmt(L"%d DMG rows, %d events in the attack clip: the extra hits land together.", dmgRows, n));
        }
    }

    // timeline (bones, files, sounds): the client's placement rules
    SkillInfo tlSkill = sk ? *sk : SkillInfo{};
    const Timeline tl = BuildTimeline(db, rowsKey, tlSkill, in.caster, in.target, po);

    // ------------------------------------------------------------------ bone
    {
        DiagItem& d = St(rep, DiagStep::Bone);
        if (rows.empty()) {
            Skip(d, L"Satır yok.", L"No rows.", DiagState::NotApplicable);
        } else if (!in.caster && !in.target) {
            Skip(d, L"Model verilmedi.", L"No model given.");
        } else {
            const auto bad = BadBones(tl, in.caster, in.target, false);
            if (!bad.empty()) {
                const EffectRow& r0 = *rows[static_cast<size_t>(bad[0].row)];
                Fail(d, Sev::Warn, L"V14",
                     L"Kemik iskelette yok: " + bad[0].bone + (bad[0].actor == 0 ? L" (kullanan)" : L" (hedef)") + L": efekt modelin kökünde doğar.",
                     L"Bone not in the skeleton: " + bad[0].bone + (bad[0].actor == 0 ? L" (caster)" : L" (target)") + L": the effect starts at the model root.");
                d.row = bad[0].row;
                d.count = static_cast<int>(bad.size());
                (r0.origin == Origin::Client ? d.fileLine : d.listLine) = r0.line;
                auto cells = CellsOf(rows);
                std::wstring firstNew;
                for (const auto& b : bad) {
                    const auto names = BoneNames(b.actor == 0 ? in.caster : in.target, b.actor == 0 ? in.casterBones : std::vector<std::wstring>{});
                    std::wstring nb = Nearest(b.bone, names);
                    if (nb.empty()) nb = L"none";
                    if (firstNew.empty()) firstNew = nb;
                    SetCell(cells[static_cast<size_t>(b.row)], b.cell, nb);
                }
                rewriteGroup(d, DiagFix::ReplaceBone, std::move(cells), L"Kemik iskeletteki en yakın adla değiştirilir (" + firstNew + L").",
                             L"The bone is replaced by the nearest name in the skeleton (" + firstNew + L").");
            } else {
                Ok(d, L"Efektlerin bağlandığı kemikler modelde var.", L"The bones used are in the skeleton.");
            }
        }
    }

    // ------------------------------------------------------------------ files
    std::vector<ResRef> res = tl.Resources();
    std::vector<std::wstring> liveEfps;
    {
        DiagItem& d = St(rep, DiagStep::Files);
        if (!in.fileExists) {
            Skip(d, L"Dosya denetimi verilmedi.", L"No file check given.");
            for (const auto& r : res)
                if (r.what == L"efp") liveEfps.push_back(r.path);
        } else {
            struct Miss {
                ResRef r;
                bool main = false;
            };
            std::vector<Miss> miss;
            int checked = 0;
            for (const auto& r : res) {
                const bool main = r.what == L"efp" || r.what == L"model";
                const bool side = r.what == L"ObjName2" || r.what == L"DamageEfp" || r.what == L"BloodType";
                if (!main && !side) continue;  // clips: clip step, sounds: sound step
                ++checked;
                if (in.fileExists(r.arc, r.path)) {
                    if (r.what == L"efp") liveEfps.push_back(r.path);
                    continue;
                }
                miss.push_back({r, main});
            }
            std::stable_sort(miss.begin(), miss.end(), [](const Miss& a, const Miss& b) { return a.main > b.main; });
            if (!miss.empty()) {
                const Miss& m0 = miss[0];
                Fail(d, m0.main ? Sev::Error : Sev::Warn, L"FILE",
                     L"Dosya istemcide yok: " + m0.r.path + L" (" + m0.r.what + L")" + (m0.main ? L": efekt görünmez." : L": o parça görünmez."),
                     L"File missing in the client: " + m0.r.path + L" (" + m0.r.what + L")" + (m0.main ? L": the effect never shows." : L": that part never shows."));
                d.row = m0.r.row;
                d.count = static_cast<int>(miss.size());
                // replacement: an existing file of the same folder for the row cells (ObjName / ObjName2) and DamageEfp
                auto cells = CellsOf(rows);
                bool rowFix = false;
                std::vector<std::wstring> saCells;
                std::wstring firstNew;
                for (const auto& m : miss) {
                    const std::wstring alt = in.similarFile ? in.similarFile(m.r.arc, m.r.path) : std::wstring();
                    if (alt.empty()) continue;
                    if (firstNew.empty()) firstNew = alt;
                    if ((m.r.what == L"efp" || m.r.what == L"model") && m.r.row >= 0 && static_cast<size_t>(m.r.row) < cells.size()) {
                        SetCell(cells[static_cast<size_t>(m.r.row)], 18, FileNameOf(alt));
                        rowFix = true;
                    } else if (m.r.what == L"ObjName2" && m.r.row >= 0 && static_cast<size_t>(m.r.row) < cells.size()) {
                        auto& rc = cells[static_cast<size_t>(m.r.row)];
                        const std::wstring old = rc.size() > 23 ? rc[23] : std::wstring();
                        const size_t cut = old.find_last_of(L"\\/");
                        SetCell(rc, 23, (cut == std::wstring::npos ? std::wstring() : old.substr(0, cut + 1)) + FileNameOf(alt));
                        rowFix = true;
                    } else if (m.r.what == L"DamageEfp" && sa && saCells.empty()) {
                        saCells = sa->cells;
                        SetCell(saCells, 14, alt);
                    }
                }
                const std::wstring tr = L"Aynı klasördeki benzer dosya kullanılır: " + firstNew, en = L"A similar file of the same folder is used: " + firstNew;
                if (rowFix) {
                    SetFix(d, DiagFix::ReplaceFile, rowsKey, SecBit(Sec::SE) | (saCells.empty() ? 0 : SecBit(Sec::SA)),
                           (saCells.empty() ? std::wstring() : SaListText(saCells)) + SeGroupText(rowsKey, cells), tr, en);
                } else if (!saCells.empty()) {
                    saFix(d, DiagFix::ReplaceFile, saCells, tr, en);
                } else {
                    SetFix(d, DiagFix::Manual, rowsKey, 0, L"", L"Dosyayı istemciye ekleyin (Aktar dosyaları da taşır).",
                           L"Add the file to the client (Transfer copies files too).");
                }
            } else if (checked == 0) {
                Skip(d, L"Satırlar dosya kullanmıyor.", L"The rows use no file.", DiagState::NotApplicable);
            } else {
                Ok(d, Fmt(L"%d dosyanın hepsi istemcide var.", checked), Fmt(L"All %d files are in the client.", checked));
            }
        }
    }

    // ------------------------------------------------------------------ efplive
    {
        DiagItem& d = St(rep, DiagStep::EfpLive);
        std::sort(liveEfps.begin(), liveEfps.end());
        liveEfps.erase(std::unique(liveEfps.begin(), liveEfps.end()), liveEfps.end());
        if (!in.efpProbe) {
            Skip(d, L".efp denetleyici verilmedi.", L"No .efp probe given.");
        } else if (liveEfps.empty()) {
            Skip(d, L"Denetlenecek .efp yok.", L"No .efp to check.", DiagState::NotApplicable);
        } else {
            std::vector<std::wstring> dead;
            int probed = 0;
            for (const auto& p : liveEfps) {
                EfpLive l;
                if (!in.efpProbe(p, in.efpWindowMs, l)) continue;
                ++probed;
                if (l.emitted <= 0) dead.push_back(p);
            }
            if (!dead.empty()) {
                Fail(d, Sev::Warn, L"EFP", Fmt(L"%ls ilk %.0f ms içinde hiçbir şey üretmiyor: efekt görünmez.", dead[0].c_str(), in.efpWindowMs),
                     Fmt(L"%ls emits nothing in its first %.0f ms: the effect does not show.", dead[0].c_str(), in.efpWindowMs));
                d.count = static_cast<int>(dead.size());
                SetFix(d, DiagFix::Manual, rowsKey, 0, L"", L"Başka bir .efp seçin ya da efekti düzenleyin.", L"Pick another .efp or edit the effect.");
            } else if (probed == 0) {
                Skip(d, L".efp benzetilemedi.", L".efp could not be simulated.");
            } else {
                Ok(d, Fmt(L"%d efekt dosyası ilk saniyelerde görüntü üretiyor.", probed), Fmt(L"%d .efp show something early.", probed));
            }
        }
    }

    // ------------------------------------------------------------------ charinfo
    const CharInfoRow* cci = po.casterCi.empty() ? nullptr : db.CharInfo(po.casterCi);
    {
        DiagItem& d = St(rep, DiagStep::CharInfo);
        if (po.casterCi.empty()) {
            Skip(d, L"Canavar kodu verilmedi.", L"No creature code given.");
        } else if (!cci) {
            // not a broken link: the effects still show (thousands of shipped creatures have no row) - Info with a fix
            Fail(d, Sev::Info, L"CI", L"characterInfo satırı yok (" + po.casterCi + L"): efektler görünür ama ölçek 1, ses nesnesi ve kan efekti yok.",
                 L"No characterInfo row (" + po.casterCi + L"): the effects show, but scale 1, no sound object, no blood effect.");
            const std::vector<std::wstring> c = {po.casterCi, po.casterCi, L"2",    L"none", L"none",          L"none", L"none",
                                                 L"none",     L"0,10,0",   L"hit_2_redblood", L"none", L"0", L"0"};
            SetFix(d, DiagFix::AddCharInfo, po.casterCi, SecBit(Sec::CI), L"#section characterInfo\r\n" + Join(c) + L"\r\n",
                   L"Varsayılan bir characterInfo satırı eklenir.", L"A default characterInfo row is added.");
        } else {
            Ok(d, Fmt(L"Görünüm satırı var (boyut %.1f).", cci->size), Fmt(L"characterInfo present (size %.1f).", cci->size));
        }
    }

    // ------------------------------------------------------------------ sound
    {
        DiagItem& d = St(rep, DiagStep::Sound);
        bool any = false;
        if (in.casterBsr) {
            any = true;
            bool ok = false;
            const auto sets = BsrSoundSets(*in.casterBsr, &ok);
            const SoundSetInfo* worst = nullptr;
            for (const auto& s : sets)
                if (!worst || s.tracks > worst->tracks) worst = &s;
            if (worst && worst->tracks > kMaxSoundTrackPerAni) {
                Fail(d, Sev::Error, L"V17",
                     Fmt(L".bsr ses seti '%ls' %d iz taşıyor (en çok 15): istemci assert verir (ÇÖKER).", worst->name.c_str(), worst->tracks),
                     Fmt(L".bsr sound set '%ls' has %d tracks (max 15): the client asserts (CRASH).", worst->name.c_str(), worst->tracks));
                SetFix(d, DiagFix::Manual, rowsKey, 0, L"", L"Ses setini 11 ize indirin; fazlasını SndBegin'e taşıyın.",
                       L"Cut the sound set to 11 tracks; move the rest to SndBegin.");
            } else if (worst && worst->tracks > kShippedMaxSoundTracks) {
                Note(rep, DiagStep::Sound, Sev::Info, L"SND11", Fmt(L"Ses seti '%ls' %d iz (sevkiyat en çok 11).", worst->name.c_str(), worst->tracks),
                     Fmt(L"Sound set '%ls' has %d tracks (shipped max 11).", worst->name.c_str(), worst->tracks));
            }
            if (!ok) Note(rep, DiagStep::Sound, Sev::Info, L"BSR", L".bsr ses paleti tam okunamadı.", L".bsr sound palette not fully readable.");
        }
        if (d.state != DiagState::Fail && in.fileExists) {
            std::vector<std::wstring> miss;
            for (const auto& r : res) {
                if (r.arc != ResArc::Data || r.what == L"model" || StartsWith(r.what, L"clip ")) continue;
                any = true;
                if (!in.fileExists(r.arc, r.path)) miss.push_back(r.path);
            }
            if (!miss.empty()) {
                Fail(d, Sev::Warn, L"FILE", L"Ses dosyası yok: " + miss[0] + L" (efekt sessiz).", L"Sound file missing: " + miss[0] + L" (silent effect).");
                d.count = static_cast<int>(miss.size());
                SetFix(d, DiagFix::Manual, rowsKey, 0, L"", L".wav dosyasını Data'ya ekleyin.", L"Add the .wav to Data.");
            }
        }
        if (d.state != DiagState::Fail) {
            if (any) Ok(d, L"Ses izleri ve dosyaları uygun.", L"Sound tracks and files are fine.");
            else Skip(d, L".bsr / dosya denetimi verilmedi.", L"No .bsr / file check given.");
        }
    }

    // ------------------------------------------------------------------ scale
    {
        DiagItem& d = St(rep, DiagStep::Scale);
        std::vector<int> badScript;
        bool mobBase = false, charBase = false;
        for (size_t i = 0; i < rows.size(); ++i) {
            const EffectRow& r = *rows[i];
            if (r.scale == ScaleBase::Mob) mobBase = true;
            if (r.scale == ScaleBase::Char) charBase = true;
            if ((r.script & kSctEffectScale) && (r.scriptArgs.empty() || r.scriptArgs[0] <= 0)) badScript.push_back(static_cast<int>(i));
        }
        if (mobBase && cci && cci->size <= 0) {
            Fail(d, Sev::Error, L"SCALE", L"characterInfo boyutu 0: MOB_BASE ölçekli efektler 0 boyutla doğar (görünmez).",
                 L"characterInfo size 0: MOB_BASE scaled effects are born at size 0 (invisible).");
            auto c = cci->cells;
            SetCell(c, 2, L"2");
            SetFix(d, DiagFix::FixScale, cci->key, SecBit(Sec::CI), L"#section characterInfo\r\n" + Join(c) + L"\r\n",
                   L"characterInfo boyutu 2 yapılır.", L"The characterInfo size becomes 2.");
        } else if (!badScript.empty()) {
            Fail(d, Sev::Error, L"SCALE", L"SCT_EFFECT_SCALE değeri 0 ya da yok: efekt 0 boyutla doğar (görünmez).",
                 L"SCT_EFFECT_SCALE value 0 or missing: the effect is born at size 0 (invisible).");
            d.row = badScript[0];
            d.count = static_cast<int>(badScript.size());
            auto cells = CellsOf(rows);
            for (int i : badScript) {
                auto& c = cells[static_cast<size_t>(i)];
                if (c.size() <= 25) continue;
                const size_t comma = c[25].find(L',');
                std::wstring rest;
                if (comma != std::wstring::npos) {
                    const size_t next = c[25].find(L',', comma + 1);
                    if (next != std::wstring::npos) rest = c[25].substr(next);
                }
                c[25] = (comma == std::wstring::npos ? c[25] : c[25].substr(0, comma)) + L",1.0" + rest;
            }
            rewriteGroup(d, DiagFix::FixScale, std::move(cells), L"SCT_EFFECT_SCALE değeri 1.0 yapılır.", L"The SCT_EFFECT_SCALE value becomes 1.0.");
        } else if (charBase && po.casterScale <= 0) {
            Fail(d, Sev::Error, L"SCALE", L"CHAR_BASE ölçeği 0: efekt görünmez.", L"CHAR_BASE scale 0: the effect is invisible.");
            SetFix(d, DiagFix::Manual, rowsKey, 0, L"", L"characterdata ölçeğini düzeltin.", L"Fix the characterdata scale.");
        } else if (rows.empty()) {
            Skip(d, L"Satır yok.", L"No rows.", DiagState::NotApplicable);
        } else {
            Ok(d, L"Ölçekler sıfırdan büyük.", L"The scales are above zero.");
        }
    }

    for (size_t i = 0; i < rep.steps.size(); ++i)
        if (rep.steps[i].Broken()) {
            rep.firstBroken = static_cast<int>(i);
            break;
        }
    return rep;
}

ChainReport Diagnose(const FxDb& db, const std::wstring& key, const IModelInfo* model, const FileExistsFn& fileExists,
                     const EfpProbeFn& efpProbe) {
    DiagInput in;
    in.db = &db;
    in.key = key;
    in.caster = model;
    in.fileExists = fileExists;
    in.efpProbe = efpProbe;
    return Diagnose(in);
}

// ------------------------------------------------------------------------------------------------- ApplyFix
std::wstring ApplyFix(const std::wstring& listText, const DiagItem& fix) {
    if (fix.fixList.empty()) return listText;
    std::wstring out;
    Sec cur = Sec::None;
    bool table = false;
    size_t a = 0;
    while (a < listText.size()) {
        size_t b = listText.find(L'\n', a);
        const size_t e = b == std::wstring::npos ? listText.size() : b;
        std::wstring_view line(listText.data() + a, e - a);
        while (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        a = b == std::wstring::npos ? listText.size() : b + 1;
        bool keep = true;
        const std::wstring t = Trim(line);
        if (StartsWith(t, L"#")) {
            const auto tok = ClientTokens(t);
            const std::wstring cmd = tok.empty() ? std::wstring() : tok[0];
            if (cmd == L"#section") {
                table = false;
                cur = tok.size() > 1 ? SecFromName(tok[1]) : Sec::None;
            } else if (cmd == L"#table" || cmd == L"#tablo") {
                table = true;
            } else if (cmd == L"#end-table") {
                table = false;
            }
        } else if (!table && !t.empty() && !StartsWith(t, L"//")) {
            const auto tok = ClientTokens(line);
            const Sec s = cur != Sec::None ? cur : GuessSection(tok);
            if (s != Sec::None && (fix.fixSecs & SecBit(s))) {
                const size_t kc = static_cast<size_t>(KeyColumn(s));
                if (tok.size() > kc && tok[kc] == fix.fixKey) keep = false;
            }
        }
        if (keep) {
            out.append(line);
            out += L"\r\n";
        }
    }
    return out + fix.fixList;
}

// ----------------------------------------------------------------------------------------- pending rows (M6)
std::vector<Issue> CheckPendingRows(const FxDb& db, const PendingCheckOptions& o) {
    std::vector<Issue> out;
    auto add = [&](Sev sev, const wchar_t* rule, int listLine, const std::wstring& key, const std::wstring& tr, const std::wstring& en,
                   const std::wstring& howTr, const std::wstring& howEn) {
        Issue i;
        i.sev = sev;
        i.rule = rule;
        i.listLine = listLine;
        i.key = key;
        i.tr = tr;
        i.en = en;
        i.howTr = howTr;
        i.howEn = howEn;
        i.autoOk = false;
        out.push_back(std::move(i));
    };
    int noModel = 0;
    std::wstring noModelFirst;
    for (const auto& key : db.PendingKeys()) {
        const AniSetRow* sa = db.AniSet(key);
        const auto rows = db.Effects(key);
        const bool saPending = sa && sa->origin == Origin::Pending;
        int firstPend = -1;
        for (const EffectRow* r : rows)
            if (r->origin == Origin::Pending && (firstPend < 0 || r->line < firstPend)) firstPend = r->line;
        if (!saPending && firstPend < 0) continue;  // "#remove KEY" only
        const IModelInfo* m = o.modelOf ? o.modelOf(key) : nullptr;
        if (!m) {
            if (!noModel) noModelFirst = key;
            ++noModel;
            continue;
        }
        const std::wstring weapon = o.weaponOf ? o.weaponOf(key) : std::wstring();
        const Clips clips = ClipsOf(sa, m, weapon);
        const int saLine = saPending ? sa->line : firstPend;
        // V13: roles of the skillaniset2 row in the declared aniGroup
        if (sa && (saPending || firstPend >= 0)) {
            if (!sa->shot.empty() && clips.shots.empty())
                add(Sev::Error, L"V13", saLine, key,
                    L"AniShot " + AniTokenOf(sa->shot[0]) + L" modelin " + sa->aniGroup + L" grubunda yok: saldırı oynamaz, efekt / hasar gelmez.",
                    L"AniShot " + AniTokenOf(sa->shot[0]) + L" is not in the model's " + sa->aniGroup + L" group: no attack, no effect / damage.",
                    L"AniShot'u modelde olan bir animasyonla değiştirin.", L"Use an AniShot clip the model has.");
            else if (!clips.shotMissing.empty())
                add(Sev::Warn, L"V13", saLine, key, L"AniShot listesindeki " + AniTokenOf(clips.shotMissing[0]) + L" modelde yok.",
                    L"The AniShot list's " + AniTokenOf(clips.shotMissing[0]) + L" is not in the model.", L"Listeden çıkarın.", L"Remove it from the list.");
            if (clips.readyAsked && !clips.ready.found)
                add(Sev::Warn, L"V13", saLine, key, L"AniReady " + sa->readyTok + L" modelde yok.", L"AniReady " + sa->readyTok + L" is not in the model.",
                    L"Modelde olan bir hazırlık animasyonu seçin ya da none yazın.", L"Pick a preparation clip the model has, or write none.");
            if (clips.waitAsked && !clips.wait.found)
                add(Sev::Warn, L"V13", saLine, key, L"AniWait " + sa->waitTok + L" modelde yok.", L"AniWait " + sa->waitTok + L" is not in the model.",
                    L"Modelde olan bir bekleme animasyonu seçin ya da none yazın.", L"Pick a wait clip the model has, or write none.");
        }
        // V13: StartEvent of the pending rows
        for (const auto& b : BadEvents(rows, clips, true)) {
            const EffectRow& r = *rows[static_cast<size_t>(b.row)];
            add(Sev::Error, L"V13", r.line, key,
                Fmt(L"Satır animasyonun %d. vuruş anını bekliyor ama %ls animasyonunda yalnız %d vuruş anı var: bu efekt hiç başlamaz (StartEvent).", b.k, PhaseName(r.phase), b.n),
                Fmt(L"StartEvent %d but the %ls clip has %d type-1 events: the row never fires.", b.k, PhaseName(r.phase), b.n),
                Fmt(L"StartEvent hücresine 1 ile %d arasında bir sayı yazın (ya da Nesne sayfasında \"Neden görünmüyor?\" > Düzelt).", b.n),
                Fmt(L"Write a StartEvent from 1 to %d (or Object page \"Why does it not show?\" > Fix).", b.n));
        }
        // V14: bones of the pending rows (caster side)
        PlayOptions po;
        po.weaponGroup = weapon;
        const SkillInfo* si = o.skillOf ? o.skillOf(key) : nullptr;
        const Timeline tl = BuildTimeline(db, key, si ? *si : SkillInfo{}, m, nullptr, po);
        for (const auto& b : BadBones(tl, m, nullptr, true)) {
            const EffectRow& r = *rows[static_cast<size_t>(b.row)];
            add(Sev::Warn, L"V14", r.line, key,
                (b.cell == 19 ? L"StartBone " : L"TargetBone ") + b.bone + L" iskelette yok: efekt modelin kökünde doğar.",
                (b.cell == 19 ? L"StartBone " : L"TargetBone ") + b.bone + L" is not in the skeleton: the effect starts at the model root.",
                L"İskeletteki bir kemik adı yazın (ya da none).", L"Write a bone of the skeleton (or none).");
        }
        // V13: DMG rows vs hits / attack events
        if (firstPend >= 0) {
            int dmgRows = 0;
            for (const EffectRow* r : rows) dmgRows += (r->phase == Phase::Shot && r->dmg == 1) ? 1 : 0;
            const int n = clips.shots.empty() ? -1 : static_cast<int>(clips.shots[0].events.size());
            // (DMG rows vs the skill's Param mc hits is no rule: shipped Roc ATTACK05 has 3 DMG rows for mc 1)
            if (n >= 0 && dmgRows > n && dmgRows > 0)
                add(Sev::Warn, L"V13", firstPend, key, Fmt(L"%d DMG satırı, saldırı klibinde %d olay: fazla vuruşlar aynı anda düşer.", dmgRows, n),
                    Fmt(L"%d DMG rows, %d events in the attack clip: the extra hits land together.", dmgRows, n),
                    L"DMG satırlarını olay sayısına indirin.", L"Cut the DMG rows to the event count.");
        }
    }
    if (noModel)
        add(Sev::Info, L"V13", -1, noModelFirst,
            Fmt(L"%d anahtarın modeli bilinmiyor (ilki %ls): V13 / V14 denetlenmedi.", noModel, noModelFirst.c_str()),
            Fmt(L"%d keys have no known model (first %ls): V13 / V14 not checked.", noModel, noModelFirst.c_str()), L"", L"");
    // V17: .bsr files that come with the package / transfer
    for (const auto& p : o.bsrPaths) {
        Bytes b;
        if (!o.readData || !o.readData(p, b)) continue;
        for (const auto& s : BsrSoundSets(b)) {
            if (s.tracks > kMaxSoundTrackPerAni)
                add(Sev::Error, L"V17", -1, p,
                    Fmt(L"%ls: ses seti '%ls' %d iz taşıyor (en çok 15): istemci assert verir (ÇÖKER).", p.c_str(), s.name.c_str(), s.tracks),
                    Fmt(L"%ls: sound set '%ls' has %d tracks (max 15): the client asserts (CRASH).", p.c_str(), s.name.c_str(), s.tracks),
                    L"Ses setini 11 ize indirin; fazlasını SndBegin'e taşıyın.", L"Cut the set to 11 tracks; move the rest to SndBegin.");
            else if (s.tracks > kShippedMaxSoundTracks)
                add(Sev::Info, L"SND11", -1, p, Fmt(L"%ls: ses seti '%ls' %d iz (sevkiyat en çok 11).", p.c_str(), s.name.c_str(), s.tracks),
                    Fmt(L"%ls: sound set '%ls' has %d tracks (shipped max 11).", p.c_str(), s.name.c_str(), s.tracks), L"", L"");
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Issue& a, const Issue& b) {
        if (a.sev != b.sev) return a.sev < b.sev;
        return a.listLine < b.listLine;
    });
    return out;
}

}  // namespace se::sp
