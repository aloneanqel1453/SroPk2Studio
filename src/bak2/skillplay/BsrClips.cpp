// SkillEffect Studio - skillplay/BsrClips.cpp
// Minimal read-only .bsr / .ban / .bsk readers for the timeline (clip list, aniGroup entries with their type-1
// events, clip durations, bone names + bind pose). Layout = JMXVRES 0109 as core/Studio.cpp BsrRoles and the user's
// SroEffectViewer Formats.h read it. Every read is bounds-checked; a damaged file gives "not found", never a crash.
#include "SkillPlay.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace se::sp {

namespace {

struct R {
    const Bytes& b;
    size_t p = 0;
    bool bad = false;
    explicit R(const Bytes& d) : b(d) {}
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
    float F32() {
        const std::uint32_t u = U32();
        float f = 0;
        std::memcpy(&f, &u, 4);
        return f;
    }
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

std::wstring W(const std::string& s) { return FromAnsi(s, 949); }
std::wstring Low(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}
std::wstring NormP(std::wstring s) {
    for (auto& c : s)
        if (c == L'/') c = L'\\';
    while (!s.empty() && s.front() == L'\\') s.erase(s.begin());
    return s;
}

Mat4 QuatTrans(double x, double y, double z, double w, const Vec3& t) {  // D3DXMatrixRotationQuaternion + row 3
    Mat4 m;
    m.m[0][0] = 1 - 2 * (y * y + z * z);
    m.m[0][1] = 2 * (x * y + z * w);
    m.m[0][2] = 2 * (x * z - y * w);
    m.m[1][0] = 2 * (x * y - z * w);
    m.m[1][1] = 1 - 2 * (x * x + z * z);
    m.m[1][2] = 2 * (y * z + x * w);
    m.m[2][0] = 2 * (x * z + y * w);
    m.m[2][1] = 2 * (y * z - x * w);
    m.m[2][2] = 1 - 2 * (x * x + y * y);
    m.SetT(t);
    return m;
}

struct BskBone {
    std::wstring name, parent;
    Mat4 local;
};
std::vector<BskBone> ParseBskBones(const Bytes& b) {
    std::vector<BskBone> out;
    if (b.size() < 16 || std::memcmp(b.data(), "JMXVBSK", 7) != 0) return out;
    R r(b);
    r.Skip(12);
    const std::uint32_t n = r.U32();
    if (n > 4096) return out;
    for (std::uint32_t i = 0; i < n && !r.bad; ++i) {
        BskBone bone;
        r.U8();
        bone.name = W(r.Str());
        bone.parent = W(r.Str());
        const double qx = r.F32(), qy = r.F32(), qz = r.F32(), qw = r.F32();
        const double tx = r.F32(), ty = r.F32(), tz = r.F32();
        bone.local = QuatTrans(qx, qy, qz, qw, {tx, ty, tz});
        r.Skip(7 * 4 * 2);  // to-origin, to-local
        const std::uint32_t kids = r.U32();
        if (kids > 4096) return {};
        for (std::uint32_t k = 0; k < kids && !r.bad; ++k) r.Str();
        if (!r.bad) out.push_back(std::move(bone));
    }
    return r.bad ? std::vector<BskBone>{} : out;
}

}  // namespace

double BanDurationMs(const Bytes& b) {
    if (b.size() < 32 || std::memcmp(b.data(), "JMXVBAN", 7) != 0) return 0;
    R r(b);
    r.Skip(12);
    r.U32();
    r.U32();
    r.Str();
    const std::uint32_t d = r.U32();
    return r.bad ? 0 : static_cast<double>(d);
}

std::vector<std::wstring> BskBoneNames(const Bytes& bsk) {
    std::vector<std::wstring> out;
    for (const auto& b : ParseBskBones(bsk)) out.push_back(b.name);
    return out;
}

// mod palette (JMXVRES 0109 section 6): two lists (system, animation) of sets {kind, anim_type, name, ModData...};
// every ModData = tag + 28-byte base + a tag body. Only the Sound bodies (tag 0x00050000) are kept, with the set's
// anim_type: the clip's own sound cues (handle + ms). Layout = render/RFormats ReadModData + efsanesro bsr.py.
static bool SkipModData(R& r, std::uint32_t tag, int animType, std::vector<BsrModel::Cue>& cues) {
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
                r.Str();
                const std::uint32_t t = r.U32();
                if (t > 8192) return false;
                for (std::uint32_t k = 0; k < t && !r.bad; ++k) {
                    if (r.U32() != 1) continue;
                    BsrModel::Cue c;
                    c.role = animType;
                    c.cue.wav = NormP(W(r.Str()));
                    c.cue.ms = static_cast<double>(r.U32());
                    c.cue.handle = W(r.Str());
                    if (!r.bad && !c.cue.handle.empty() && animType >= 0) cues.push_back(std::move(c));
                }
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

static void ReadPaletteCues(const Bytes& b, std::uint32_t off, std::vector<BsrModel::Cue>& cues) {
    if (off == 0 || off >= b.size()) return;
    R r(b);
    r.Seek(off);
    for (int list = 0; list < 2 && !r.bad; ++list) {
        const std::uint32_t n = r.U32();
        if (r.bad || n > 4096) return;
        for (std::uint32_t i = 0; i < n && !r.bad; ++i) {
            r.U32();  // kind
            const int animType = static_cast<int>(r.U32());
            r.Str();
            const std::uint32_t items = r.U32();
            if (r.bad || items > 4096) return;
            for (std::uint32_t j = 0; j < items && !r.bad; ++j)
                if (!SkipModData(r, r.U32(), list == 1 ? animType : -1, cues)) return;  // unknown tag: stop (cues so far kept)
        }
    }
}

bool BsrModel::Load(const Bytes& b, const Reader& read, std::wstring& err) {
    anis.clear();
    aniMs.clear();
    entries.clear();
    cues.clear();
    bones.clear();
    skeleton.clear();
    m_rest.clear();
    if (b.size() < 44 || std::memcmp(b.data(), "JMXVRES 0109", 12) != 0) {
        err = L"JMXVRES 0109 değil / not a JMXVRES 0109 .bsr";
        return false;
    }
    R r(b);
    r.Seek(12);
    const std::uint32_t offMat = r.U32(), offMesh = r.U32(), offSkel = r.U32(), offAni = r.U32();
    r.U32();  // mesh group
    const std::uint32_t offGroup = r.U32();
    const std::uint32_t offMod = r.U32();
    (void)offMat;
    (void)offMesh;
    ReadPaletteCues(b, offMod, cues);
    // animation files
    r.Seek(offAni);
    r.U32();
    r.U32();
    const std::uint32_t na = r.U32();
    if (na > 4096) r.bad = true;
    for (std::uint32_t i = 0; i < na && !r.bad; ++i) anis.push_back(NormP(W(r.Str())));
    if (r.bad) {
        err = L".bsr animasyon listesi okunamadı / animation list unreadable";
        return false;
    }
    // skeleton
    r.Seek(offSkel);
    if (r.U32() == 1) skeleton = NormP(W(r.Str()));
    r.bad = false;
    // aniGroup entries
    r.Seek(offGroup);
    const std::uint32_t groups = r.U32();
    if (groups > 64) r.bad = true;
    for (std::uint32_t g = 0; g < groups && !r.bad; ++g) {
        const std::wstring gname = Low(W(r.Str()));
        const std::uint32_t ne = r.U32();
        if (ne > 4096) {
            r.bad = true;
            break;
        }
        for (std::uint32_t e = 0; e < ne && !r.bad; ++e) {
            Entry en;
            en.group = gname;
            en.role = static_cast<int>(r.U32());
            en.file = static_cast<int>(r.U32());
            const std::uint32_t ev = r.U32();
            if (ev > 4096) {
                r.bad = true;
                break;
            }
            for (std::uint32_t k = 0; k < ev && !r.bad; ++k) {
                const std::uint32_t key = r.U32(), type = r.U32();
                r.U32();
                r.U32();
                ++en.eventsAll;
                if (type == 1) en.events.push_back(static_cast<double>(key));
            }
            std::sort(en.events.begin(), en.events.end());
            const std::uint32_t wn = r.U32();
            if (wn > 100000) {
                r.bad = true;
                break;
            }
            r.F32();
            r.Skip(static_cast<size_t>(wn) * 8);
            if (!r.bad) entries.push_back(std::move(en));
        }
    }
    if (r.bad && entries.empty()) {
        err = L".bsr aniGroup okunamadı / aniGroup unreadable";
        return false;
    }
    // clip durations (only the files an entry uses)
    aniMs.assign(anis.size(), 0);
    if (read) {
        std::vector<char> want(anis.size(), 0);
        for (const auto& e : entries)
            if (e.file >= 0 && static_cast<size_t>(e.file) < anis.size()) want[static_cast<size_t>(e.file)] = 1;
        for (size_t i = 0; i < anis.size(); ++i) {
            if (!want[i]) continue;
            Bytes ban;
            if (read(anis[i], ban)) aniMs[i] = BanDurationMs(ban);
        }
        Bytes bsk;
        if (!skeleton.empty() && read(skeleton, bsk)) {
            const auto bb = ParseBskBones(bsk);
            std::vector<Mat4> world(bb.size());
            double top = 0;
            for (size_t i = 0; i < bb.size(); ++i) {
                bones.insert(ToUpper(bb[i].name));
                world[i] = bb[i].local;
                for (size_t j = 0; j < i; ++j)
                    if (bb[j].name == bb[i].parent) {
                        world[i] = bb[i].local * world[j];
                        break;
                    }
                const Vec3 p = world[i].T();
                m_rest[ToUpper(bb[i].name)] = p;
                top = std::max(top, p.y);
            }
            if (top > 1 && top < 2000) m_height = top;
        }
    }
    return true;
}

bool BsrModel::Clip(int role, ClipInfo& out) const {
    out = ClipInfo{};
    const Entry* best = nullptr;
    for (const auto& e : entries) {
        if (e.role != role) continue;
        if (!best || (e.group == L"default" && best->group != L"default")) best = &e;
    }
    if (!best) return false;
    out.found = true;
    out.role = role;
    out.events = best->events;
    if (best->file >= 0 && static_cast<size_t>(best->file) < anis.size()) {
        out.file = anis[static_cast<size_t>(best->file)];
        out.durationMs = aniMs.size() > static_cast<size_t>(best->file) ? aniMs[static_cast<size_t>(best->file)] : 0;
    }
    return true;
}

bool BsrModel::ClipIn(int role, const std::wstring& declared, const std::wstring& weapon, ClipInfo& out) const {
    out = ClipInfo{};
    auto find = [&](const std::wstring& g) -> const Entry* {
        for (const auto& e : entries)
            if (e.role == role && e.group == g) return &e;
        return nullptr;
    };
    const std::wstring dg = Low(declared), wg = Low(weapon);
    const Entry* e = nullptr;
    if (!dg.empty() && dg != L"none") e = find(dg);              // 1. the declared group (DEFAULT included)
    if (!e && (dg.empty() || dg == L"default") && !wg.empty()) e = find(wg);  // 2. a DEFAULT cast held with a weapon
    if (!e) e = find(L"default");                                // 3. default, then any group
    if (!e)
        for (const auto& x : entries)
            if (x.role == role) {
                e = &x;
                break;
            }
    if (!e) return false;
    out.found = true;
    out.role = role;
    out.group = e->group;
    out.events = e->events;
    if (e->file >= 0 && static_cast<size_t>(e->file) < anis.size()) {
        out.file = anis[static_cast<size_t>(e->file)];
        out.durationMs = aniMs.size() > static_cast<size_t>(e->file) ? aniMs[static_cast<size_t>(e->file)] : 0;
    }
    return true;
}

std::vector<SoundCue> BsrModel::SoundCues(int role) const {
    std::vector<SoundCue> out;
    for (const auto& c : cues)
        if (c.role == role) out.push_back(c.cue);
    std::sort(out.begin(), out.end(), [](const SoundCue& a, const SoundCue& b) { return a.ms < b.ms; });
    return out;
}

bool BsrModel::HasBone(const std::wstring& bone) const {
    if (bones.empty()) return true;  // skeleton not read: cannot tell
    return bones.count(ToUpper(bone)) != 0;
}

bool BsrModel::BoneRest(const std::wstring& bone, Vec3& pos) const {
    auto it = m_rest.find(ToUpper(bone));
    if (it == m_rest.end()) return false;
    pos = it->second;
    return true;
}

}  // namespace se::sp
