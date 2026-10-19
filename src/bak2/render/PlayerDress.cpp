// SkillEffect Studio v5/v6 - render/PlayerDress.cpp (see PlayerDress.h)
#include "PlayerDress.h"

#include "../skillplay/SkillPlay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <map>

namespace se::render {

namespace {

std::wstring Up(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = static_cast<wchar_t>(std::towupper(c));
    return r;
}

bool HasL(const std::wstring& lowHay, const wchar_t* lowNeedle) { return lowHay.find(lowNeedle) != std::wstring::npos; }

std::vector<std::wstring> Tokens(const std::wstring& s) {
    std::vector<std::wstring> t;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == L'_') { t.push_back(cur); cur.clear(); }
        else cur += c;
    }
    t.push_back(cur);
    return t;
}

bool AllDigits(const std::wstring& s) {
    if (s.empty() || s.size() > 2) return false;
    for (wchar_t c : s)
        if (c < L'0' || c > L'9') return false;
    return true;
}

// first one / two digit token of a code: ITEM_CH_M_HEAVY_08_BA_A -> 8, ITEM_EU_SHIELD_11_A_RARE -> 11
int DegreeOf(const std::vector<std::wstring>& tok) {
    for (const auto& t : tok)
        if (AllDigits(t)) return std::stoi(t);
    return 0;
}

int SlotOfToken(const std::wstring& t) {
    static const wchar_t* const kSlots[6] = {L"HA", L"SA", L"BA", L"LA", L"AA", L"FA"};
    for (int i = 0; i < 6; ++i)
        if (t == kSlots[i]) return i + 1;
    return 0;
}

std::wstring BaseNoExt(const std::wstring& path) {
    std::size_t s = path.find_last_of(L"\\/");
    std::wstring b = s == std::wstring::npos ? path : path.substr(s + 1);
    std::size_t d = b.find_last_of(L'.');
    if (d != std::wstring::npos) b.resize(d);
    return LowerW(b);
}

// look tag of a worn model: heavy_08_ba -> "", heavy_08_ba_set_a -> "_set_a", heavy_07_ba_super -> "_super"
std::wstring VariantTag(const std::wstring& model) {
    const std::wstring b = BaseNoExt(model);
    static const wchar_t* const kTok[6] = {L"_ha", L"_sa", L"_ba", L"_la", L"_aa", L"_fa"};
    for (const wchar_t* t : kTok) {
        for (std::size_t p = b.find(t); p != std::wstring::npos; p = b.find(t, p + 1))
            if (p + 3 == b.size() || b[p + 3] == L'_') return b.substr(p + 3);
    }
    return L"";
}

bool IsGmLook(const std::wstring& lowText) {
    return HasL(lowText, L"super") || HasL(lowText, L"_gm") || HasL(lowText, L"gacha") || HasL(lowText, L"event");
}

// plain shop item first: service, not rare / event / mall
int CodeScore(const DressItem& it) {
    const std::wstring u = Up(it.code);
    int s = it.service ? 4 : 0;
    if (u.find(L"RARE") == std::wstring::npos) s += 2;
    if (u.find(L"EVENT") == std::wstring::npos && u.find(L"MALL") == std::wstring::npos && u.find(L"_GM") == std::wstring::npos) s += 3;
    if (u.size() > 2 && u.compare(u.size() - 2, 2, L"_A") == 0) s += 1;
    return s;
}

struct ExistCache {
    const DressExistsFn* fn;
    std::map<std::wstring, bool> memo;
    bool operator()(const std::wstring& p) {
        if (p.empty()) return false;
        if (!fn || !*fn) return true;
        const std::wstring k = LowerW(p);
        auto it = memo.find(k);
        if (it != memo.end()) return it->second;
        return memo[k] = (*fn)(p);
    }
};

struct Weapon {
    int type;
    const wchar_t* group;
    bool melee;
};
const Weapon kWeaponGroups[] = {
    {2, L"sword", true},          {3, L"sword", true},           {4, L"spear", true},          {5, L"spear", true},
    {6, L"bow", false},           {7, L"onehand_sword", true},   {8, L"twohand_sword", true},  {9, L"dual_axe", true},
    {10, L"onehand_staff", false}, {11, L"twohand_staff", false}, {12, L"bow", false},         {13, L"dagger", true},
    {14, L"harf", false},          {15, L"onehand_staff", false},
};

const Weapon* WeaponOf(int t) {
    for (const auto& w : kWeaponGroups)
        if (w.type == t) return &w;
    return nullptr;
}

}  // namespace

// ------------------------------------------------------------------ body / rules
int BodyRace(const std::wstring& code, const std::wstring& model) {
    const std::wstring u = Up(code);
    if (u.rfind(L"CHAR_EU", 0) == 0 || u.find(L"_EU_") != std::wstring::npos) return 1;
    if (u.rfind(L"CHAR_CH", 0) == 0 || u.find(L"_CH_") != std::wstring::npos) return 0;
    const std::wstring l = LowerW(model);
    return HasL(l, L"europe") ? 1 : 0;
}

int BodyGender(const std::wstring& code, const std::wstring& model) {
    const std::wstring u = Up(code);
    if (u.find(L"_WOMAN") != std::wstring::npos || u.find(L"_W_") != std::wstring::npos) return 1;
    if (u.find(L"_MAN") != std::wstring::npos || u.find(L"_M_") != std::wstring::npos) return 0;
    const std::wstring l = LowerW(model);
    return HasL(l, L"woman") ? 1 : 0;
}

// the game's one-hand rule: a shield goes with CH sword / blade and EU one-hand sword / dark staff / cleric rod.
// Two-hand weapons (spear, glaive, two-hand sword, wizard staff, harp), bows, crossbows and the dual pairs
// (axes, daggers) leave no free off hand.
bool ShieldAllowed(int race, int weaponType) {
    if (race == 0) return weaponType == 2 || weaponType == 3;
    return weaponType == 7 || weaponType == 10 || weaponType == 15;
}

ArmorClass ArmorClassFor(int race, int weaponType, int mastery) {
    switch (mastery) {  // the usual build of a mastery
        case 257: case 258: return ArmorClass::Armor;           // CH sword / spear
        case 259: return ArmorClass::Protector;                 // CH bow
        case 273: case 274: case 275: case 276: return ArmorClass::Garment;   // CH elements / force
        case 513: return ArmorClass::Armor;                     // EU warrior
        case 515: return ArmorClass::Protector;                 // EU rogue
        case 514: case 516: case 517: case 518: return ArmorClass::Garment;   // EU wizard / warlock / bard / cleric
        default: break;
    }
    switch (weaponType) {
        case 2: case 3: case 4: case 5: case 7: case 8: case 9: return ArmorClass::Armor;
        case 6: case 12: case 13: return ArmorClass::Protector;
        case 10: case 11: case 14: case 15: return ArmorClass::Garment;
        default: (void)race; return ArmorClass::Armor;
    }
}

const wchar_t* WeaponAniGroup(int weaponType) {
    const Weapon* w = WeaponOf(weaponType);
    return w ? w->group : L"";
}

const wchar_t* ArmorClassName(int race, ArmorClass c) {
    switch (c) {
        case ArmorClass::Garment: return race ? L"robe" : L"garment";
        case ArmorClass::Protector: return race ? L"light" : L"protector";
        case ArmorClass::Armor: return race ? L"heavy" : L"armor";
        default: return L"none";
    }
}

double PlayerTargetDistance(int weaponType, double casterRadius, double targetRadius) {
    const Weapon* w = WeaponOf(weaponType);
    const double r = casterRadius + targetRadius;
    if (w && w->melee) {
        const bool polearm = weaponType == 4 || weaponType == 5;   // spear / glaive reach further
        return std::clamp(r * 0.55 + (polearm ? 11.0 : 8.0), 20.0, 70.0);
    }
    return std::clamp(r * 0.55 + 18.0, 30.0, 600.0);   // ranged / magic: close enough that the hit stays in view
}

// ------------------------------------------------------------------ index
void DressIndex::Clear() {
    m_armor.clear();
    m_shields.clear();
    m_weapons.clear();
}

void DressIndex::Add(const DressItem& it) {
    if (it.type[0] != 3 || it.type[1] != 1 || it.model.empty()) return;
    const std::wstring u = Up(it.code);
    const auto tok = Tokens(u);
    Entry e;
    e.it = it;
    e.degree = DegreeOf(tok);
    if (tok.size() > 1) e.race = tok[1] == L"CH" ? 0 : tok[1] == L"EU" ? 1 : -1;
    const std::wstring lm = LowerW(it.model);
    const int t3 = it.type[2];
    if ((t3 >= 1 && t3 <= 3) || (t3 >= 9 && t3 <= 11)) {   // armour
        if (e.race < 0) e.race = t3 >= 9 ? 1 : 0;
        e.cls = static_cast<ArmorClass>(t3 >= 9 ? t3 - 8 : t3);
        if (tok.size() > 2 && (tok[2] == L"M" || tok[2] == L"W")) e.gender = tok[2] == L"W" ? 1 : 0;
        else e.gender = HasL(lm, L"woman_item") ? 1 : HasL(lm, L"man_item") ? 0 : -1;
        e.slot = it.type[3] >= 1 && it.type[3] <= 6 ? it.type[3] : 0;
        for (const auto& t : tok)   // the code's slot token wins (rows with an odd TypeID4)
            if (int s = SlotOfToken(t)) { e.slot = s; break; }
        if (e.slot <= 0 || e.gender < 0) return;
        m_armor.push_back(std::move(e));
    } else if (t3 == 4) {   // shield: TypeID4 1 Chinese, 2 European
        if (e.race < 0) e.race = it.type[3] == 2 ? 1 : 0;
        e.slot = 7;
        m_shields.push_back(std::move(e));
    } else if (t3 == 6 && WeaponOf(it.type[3])) {
        if (e.race < 0) e.race = it.type[3] >= 7 ? 1 : 0;
        e.slot = 8;
        m_weapons.push_back(std::move(e));
    }
}

std::vector<int> DressIndex::Degrees(int race, int gender, ArmorClass c, const DressExistsFn& exists) const {
    ExistCache ex{&exists, {}};
    std::vector<int> d;
    for (const auto& e : m_armor)
        if (e.race == race && e.gender == gender && e.cls == c && e.degree > 0 && ex(e.it.model) &&
            std::find(d.begin(), d.end(), e.degree) == d.end())
            d.push_back(e.degree);
    std::sort(d.begin(), d.end());
    return d;
}

namespace {

struct Cand {
    const DressItem* it = nullptr;
    std::wstring model, tag;
    bool exists = false;
    int score = 0;
};

struct SetPick {
    std::array<const Cand*, 7> slot{};
    std::wstring tag;
    int have = 0;
};

// resolveArmorSets (sroviewer-armor-set-resolver): PASS 1 the best existing candidate per slot (+100 _set_ look,
// -60 GM / event look, +2 item\common\); PASS 2 one look tag for the whole set (max slot coverage, then _set_,
// never GM) and every slot re-aligned to an existing candidate of that tag.
SetPick ResolveSet(const std::array<std::vector<Cand>, 7>& c) {
    SetPick r;
    for (int s = 1; s <= 6; ++s)
        for (const auto& x : c[static_cast<std::size_t>(s)])
            if (x.exists && (!r.slot[static_cast<std::size_t>(s)] || x.score > r.slot[static_cast<std::size_t>(s)]->score)) r.slot[static_cast<std::size_t>(s)] = &x;
    std::map<std::wstring, int> chosenTags;
    for (int s = 1; s <= 6; ++s)
        if (const Cand* x = r.slot[static_cast<std::size_t>(s)]) ++chosenTags[x->tag];
    if (chosenTags.size() > 1) {
        std::map<std::wstring, int> cover;
        for (int s = 1; s <= 6; ++s) {
            std::map<std::wstring, bool> seen;
            for (const auto& x : c[static_cast<std::size_t>(s)])
                if (x.exists && !seen[x.tag]) { seen[x.tag] = true; ++cover[x.tag]; }
        }
        std::wstring best;
        int bestKey = -1000000;
        for (const auto& [tag, n] : cover) {
            int key = n * 1000 + (HasL(tag, L"set_") ? 100 : 0) - (IsGmLook(tag) ? 600 : 0) + (tag.empty() ? 10 : 0);
            if (key > bestKey) { bestKey = key; best = tag; }
        }
        for (int s = 1; s <= 6; ++s) {
            const Cand* pick = nullptr;
            for (const auto& x : c[static_cast<std::size_t>(s)])
                if (x.exists && x.tag == best && (!pick || x.score > pick->score)) pick = &x;
            if (pick) r.slot[static_cast<std::size_t>(s)] = pick;
        }
        r.tag = best;
    } else if (!chosenTags.empty()) {
        r.tag = chosenTags.begin()->first;
    }
    for (int s = 1; s <= 6; ++s) r.have += r.slot[static_cast<std::size_t>(s)] != nullptr;
    return r;
}

}  // namespace

DressPlan DressIndex::Plan(const DressRequest& rq, const DressExistsFn& exists) const {
    DressPlan p;
    ExistCache ex{&exists, {}};
    p.race = BodyRace(rq.body, rq.bodyModel);
    p.gender = BodyGender(rq.body, rq.bodyModel);
    p.cls = rq.cls == ArmorClass::Auto ? ArmorClassFor(p.race, rq.weaponType, rq.mastery) : rq.cls;

    // candidates per degree and slot (unique model files; the best code names each file)
    std::map<int, std::array<std::vector<Cand>, 7>> byDeg;
    if (p.cls != ArmorClass::None)
        for (const auto& e : m_armor) {
            if (e.race != p.race || e.gender != p.gender || e.cls != p.cls || e.degree <= 0) continue;
            auto& v = byDeg[e.degree][static_cast<std::size_t>(e.slot)];
            const std::wstring key = LowerW(NormPath(e.it.model));
            auto f = std::find_if(v.begin(), v.end(), [&](const Cand& c) { return LowerW(NormPath(c.model)) == key; });
            if (f != v.end()) {
                if (CodeScore(e.it) > CodeScore(*f->it)) f->it = &e.it;
                continue;
            }
            Cand c;
            c.it = &e.it;
            c.model = e.it.model;
            c.tag = VariantTag(e.it.model);
            v.push_back(std::move(c));
        }
    // degrees in the order they are tried: the wanted one, then nearest (lower first on a tie)
    const int want = rq.degree > 0 ? rq.degree : kPreferDegree;
    std::vector<int> order;
    for (const auto& kv : byDeg) order.push_back(kv.first);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const int da = std::abs(a - want), db = std::abs(b - want);
        return da != db ? da < db : a < b;
    });
    int bestDeg = 0, bestHave = -1;
    SetPick best;
    for (int d : order) {
        auto& slots = byDeg[d];
        for (auto& v : slots)
            for (auto& c : v) {
                c.exists = ex(c.model);
                const std::wstring lm = LowerW(c.model);
                c.score = (HasL(c.tag, L"set_") ? 100 : 0) - (IsGmLook(c.tag) || IsGmLook(lm) ? 60 : 0) +
                          (HasL(lm, L"\\common\\") ? 2 : 0) + CodeScore(*c.it);
            }
        SetPick sp = ResolveSet(slots);
        if (sp.have > bestHave) { bestHave = sp.have; bestDeg = d; best = sp; }
        if (sp.have == 6 && (rq.degree <= 0 || d == rq.degree)) break;   // complete: the nearest complete set wins
        if (rq.degree > 0 && d == rq.degree && sp.have > 0) break;      // an explicit degree is kept when it has pieces
    }
    p.degree = bestDeg;
    p.variant = best.tag;
    for (int s = 1; s <= 6; ++s) {
        const Cand* c = best.slot[static_cast<std::size_t>(s)];
        if (!c) { ++p.missingSlots; continue; }
        DressPiece dp;
        dp.slot = s;
        dp.code = c->it->code;
        dp.model = c->model;
        p.armor.push_back(std::move(dp));
    }
    const int wantDeg = p.degree > 0 ? p.degree : want;

    // nearest-degree pick among held items of the race (same degree, else lower, else higher)
    auto pickHeld = [&](const std::vector<Entry>& pool, auto&& accept, DressPiece& out) {
        const Entry* bestE = nullptr;
        long long bestK = 0;
        std::wstring m1, m2;
        for (const auto& e : pool) {
            if (e.race != p.race || !accept(e)) continue;
            std::wstring a = e.it.model, b;
            if (!ex(a)) {   // daggers / axes: _r + _l files
                std::wstring stem = a;
                if (stem.size() > 4 && LowerW(stem.substr(stem.size() - 4)) == L".bsr") stem.resize(stem.size() - 4);
                if (!ex(stem + L"_r.bsr")) continue;
                a = stem + L"_r.bsr";
                b = ex(stem + L"_l.bsr") ? stem + L"_l.bsr" : std::wstring();
            }
            const int dd = e.degree - wantDeg;
            const long long k = (dd == 0 ? 0LL : dd < 0 ? -dd * 10LL : dd * 10LL + 5) * 100 - CodeScore(e.it) * 10 + (IsGmLook(LowerW(e.it.code)) ? 500 : 0);
            if (!bestE || k < bestK || (k == bestK && e.it.id < bestE->it.id)) { bestE = &e; bestK = k; m1 = a; m2 = b; }
        }
        if (!bestE) return;
        out.slot = bestE->slot;
        out.code = bestE->it.code;
        out.model = m1;
        out.model2 = m2;
    };
    p.shieldAllowed = ShieldAllowed(p.race, rq.weaponType);
    if (p.shieldAllowed && rq.shield) pickHeld(m_shields, [&](const Entry& e) { return e.it.type[3] == (p.race ? 2 : 1); }, p.shield);
    if (rq.weaponType > 0) pickHeld(m_weapons, [&](const Entry& e) { return e.it.type[3] == rq.weaponType; }, p.weapon);

    p.note = std::wstring(p.race ? L"EU " : L"CH ") + (p.gender ? L"woman " : L"man ") + ArmorClassName(p.race, p.cls) +
             L" dg" + std::to_wstring(p.degree) + L" " + std::to_wstring(p.armor.size()) + L"/6" +
             (p.variant.empty() ? L"" : L" look " + p.variant) +
             L" | shield " + (p.shield.model.empty() ? (p.shieldAllowed ? L"(no model)" : L"(not allowed)") : p.shield.code) +
             L" | weapon " + (p.weapon.model.empty() ? L"-" : p.weapon.code);
    return p;
}

// ------------------------------------------------------------------ held-item materials
namespace {

// connected-component backdrop cut (sroviewer-weapon-opaque): returns a copy, cut = texels made transparent
std::shared_ptr<RTexture> BackdropCut(const RTexture& t, std::size_t& cut) {
    cut = 0;
    const int w = t.img.w, h = t.img.h;
    if (t.img.Empty()) return nullptr;
    const std::size_t tot = static_cast<std::size_t>(w) * h;
    std::vector<std::uint8_t> inv(tot, 0);
    for (std::size_t i = 0; i < tot; ++i) {
        const std::uint8_t* p = &t.img.rgba[i * 4];
        const int a = p[3], rgb = p[0] + p[1] + p[2];
        inv[i] = a <= 12 || (a < 128 && rgb < 48);
    }
    std::vector<int> label(tot, -1);
    std::vector<std::uint8_t> cutBlob;
    std::vector<std::size_t> stack;
    for (std::size_t s = 0; s < tot; ++s) {
        if (!inv[s] || label[s] >= 0) continue;
        const int id = static_cast<int>(cutBlob.size());
        std::size_t size = 0;
        bool border = false;
        stack.assign(1, s);
        label[s] = id;
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            ++size;
            const int x = static_cast<int>(i % static_cast<std::size_t>(w)), y = static_cast<int>(i / static_cast<std::size_t>(w));
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) border = true;
            const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
            for (int k = 0; k < 4; ++k) {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
                const std::size_t j = static_cast<std::size_t>(ny[k]) * w + nx[k];
                if (inv[j] && label[j] < 0) { label[j] = id; stack.push_back(j); }
            }
        }
        cutBlob.push_back(border || size > tot / 110 ? 1 : 0);
    }
    auto out = std::make_shared<RTexture>(t);
    const bool floats = out->rgba.size() >= tot * 4;
    std::size_t zero = 0;
    for (std::size_t i = 0; i < tot; ++i) {
        const bool c = label[i] >= 0 && cutBlob[static_cast<std::size_t>(label[i])];
        out->img.rgba[i * 4 + 3] = c ? 0 : 255;
        if (floats) out->rgba[i * 4 + 3] = c ? 0.f : 1.f;
        if (c) ++zero;
    }
    cut = zero;
    out->zeroAlpha = static_cast<float>(static_cast<double>(zero) / static_cast<double>(tot));
    out->midAlpha = 0;
    out->anyAlpha = zero > 0;
    return out;
}

}  // namespace

int FixHeldParts(ModelInstance& m, std::size_t firstPart) {
    auto def = m.Def();
    if (!def) return 0;
    std::map<const RTexture*, std::pair<std::shared_ptr<RTexture>, std::size_t>> done;
    int changed = 0;
    for (std::size_t i = firstPart; i < def->parts.size(); ++i) {
        ModelPart& part = def->parts[i];
        // rigid on the .bsr attach bone when the mesh only matched non-biped bones of the body: woman / robe skeletons
        // carry Bone01.. (skirt, hair) whose names collide with the weapon / shield skeleton -> the item followed a
        // skirt bone and lay at the feet. Biped matches (Bip02 -> Bip01 avatar pieces) keep their skinning.
        if (part.attachBone < 0 && def->skel && part.mesh && part.bsrIndex >= 0 && static_cast<std::size_t>(part.bsrIndex) < def->bsrs.size() &&
            def->bsrs[static_cast<std::size_t>(part.bsrIndex)] && !def->bsrs[static_cast<std::size_t>(part.bsrIndex)]->attachBone.empty()) {
            bool any = false, biped = false;
            for (std::size_t k = 0; k < part.boneMap.size() && k < part.mesh->boneNames.size(); ++k)
                if (part.boneMap[k] >= 0) {
                    any = true;
                    biped = biped || LowerW(part.mesh->boneNames[k]).rfind(L"bip0", 0) == 0;
                }
            const int ab = def->skel->Find(def->bsrs[static_cast<std::size_t>(part.bsrIndex)]->attachBone);
            if (any && !biped && ab >= 0) {
                part.attachBone = ab;
                ++changed;
            }
        }
        if (!part.tex || (part.blend != PartBlend::Cutout && part.blend != PartBlend::Blend)) continue;
        const std::wstring lp = LowerW(part.texturePath.empty() ? part.meshPath : part.texturePath);
        if (!HasL(lp, L"item\\") || (!HasL(lp, L"weapon") && !HasL(lp, L"shield"))) continue;
        auto it = done.find(part.tex.get());
        if (it == done.end()) {
            std::size_t cut = 0;
            auto t = BackdropCut(*part.tex, cut);
            it = done.emplace(part.tex.get(), std::make_pair(t, cut)).first;
        }
        if (!it->second.first) continue;
        part.tex = it->second.first;
        part.blend = it->second.second ? PartBlend::Cutout : PartBlend::Opaque;
        ++changed;
    }
    return changed;
}

// ------------------------------------------------------------------ dressing
int ApplyDress(ModelInstance& m, AssetCache& cache, const DressPlan& p, std::wstring* log, bool withWeapon) {
    int n = 0;
    auto one = [&](const std::wstring& path) {
        if (path.empty()) return;
        std::wstring err;
        if (m.Attach(cache, path, err)) ++n;
        else if (log) *log += path + L": " + err + L"\n";
    };
    for (const auto& a : p.armor) one(a.model);
    const std::size_t held = m.Def() ? m.Def()->parts.size() : 0;
    one(p.shield.model);
    if (withWeapon) {
        one(p.weapon.model);
        one(p.weapon.model2);
    }
    if (FixHeldParts(m, held) > 0) m.Advance(0);   // re-pose with the corrected parts
    return n;
}

// ------------------------------------------------------------------ camera
namespace {

struct Box {
    V3 mn, mx;
    std::array<V3, 8> Corners() const {
        std::array<V3, 8> c;
        for (int i = 0; i < 8; ++i) c[static_cast<std::size_t>(i)] = {i & 1 ? mx.x : mn.x, i & 2 ? mx.y : mn.y, i & 4 ? mx.z : mn.z};
        return c;
    }
    V3 Center() const { return (mn + mx) * 0.5; }
};

// the body alone (skeleton extent: feet .. head top), without held weapons; false = no skeleton
bool BodyBox(ModelInstance& m, Box& b) {
    auto def = m.Def();
    if (!def || !def->HasSkeleton()) return false;
    int n = 0;
    for (int i = 0; i < static_cast<int>(def->skel->bones.size()); ++i) {
        M4 B;
        if (!m.BoneWorld(i, B)) continue;
        const V3 p{B.m[3][0], B.m[3][1], B.m[3][2]};
        if (!n++) { b.mn = b.mx = p; continue; }
        b.mn = {std::min(b.mn.x, p.x), std::min(b.mn.y, p.y), std::min(b.mn.z, p.z)};
        b.mx = {std::max(b.mx.x, p.x), std::max(b.mx.y, p.y), std::max(b.mx.z, p.z)};
    }
    if (n < 2) return false;
    const double h = b.mx.y - b.mn.y;
    if (h < 1) return false;
    b.mx.y += h * 0.10;   // head top above the head bone
    b.mn.y = std::min(b.mn.y, m.pos.y);
    b.mn = b.mn - V3{h * 0.08, 0, h * 0.08};
    b.mx = b.mx + V3{h * 0.08, 0, h * 0.08};
    return true;
}

bool BoxOf(Scene& sc, int id, Box& b) {
    ModelInstance* m = id > 0 ? sc.Model(id) : nullptr;
    if (!m || !m->visible) return false;
    m->Bounds(b.mn, b.mx);
    return Len(b.mx - b.mn) > 1e-6;
}

}  // namespace

namespace {

// local x side of the shield (0 = none) and of the held weapon (-1 = right hand of a character facing -z)
void HeldSides(ModelInstance* m, int& shieldSide, int& weaponSide) {
    shieldSide = 0;
    weaponSide = -1;
    if (!m || !m->Def()) return;
    const M4 inv = Inverse(m->World());
    V3 shield, weapon;
    int ns = 0, nw = 0;
    for (const auto& part : m->Def()->parts) {
        if (part.attachBone < 0) continue;
        M4 B;
        if (!m->BoneWorld(part.attachBone, B)) continue;
        const V3 local = Xf(V3{B.m[3][0], B.m[3][1], B.m[3][2]}, inv);
        const std::wstring lp = LowerW(part.texturePath.empty() ? part.meshPath : part.texturePath);
        if (HasL(lp, L"shield")) { shield += local; ++ns; }
        else { weapon += local; ++nw; }
    }
    if (ns) shieldSide = shield.x >= 0 ? 1 : -1;
    if (nw) weaponSide = weapon.x / nw >= 0.5 ? 1 : -1;
}

// a camera yaw given in the caster frame (180 = straight in front) -> world orbit yaw (degrees)
double WorldYaw(ModelInstance* m, double localDeg) {
    const double a = localDeg * kPi / 180;
    const V3 world = Xf3(V3{std::sin(a), 0, std::cos(a)}, RotY(m ? m->yaw : 0));
    double deg = std::atan2(world.x, world.z) * 180 / kPi;
    return deg < 0 ? deg + 360 : deg;
}

}  // namespace

double AutoPlayerYaw(Scene& sc, int caster) {
    ModelInstance* m = caster > 0 ? sc.Model(caster) : nullptr;
    int ss = 0, ws = -1;
    HeldSides(m, ss, ws);
    return WorldYaw(m, ws > 0 ? 145.0 : 215.0);
}

namespace {

void Grow(Box& a, const Box& b) {
    a.mn = {std::min(a.mn.x, b.mn.x), std::min(a.mn.y, b.mn.y), std::min(a.mn.z, b.mn.z)};
    a.mx = {std::max(a.mx.x, b.mx.x), std::max(a.mx.y, b.mx.y), std::max(a.mx.z, b.mx.z)};
}

// pelvis height above the model's feet (0 = no pelvis bone)
double PelvisY(ModelInstance& m) {
    int b = m.FindBone(L"Bip01 Pelvis");
    if (b < 0) b = m.FindBone(L"Bip01");
    M4 B;
    if (b < 0 || !m.BoneWorld(b, B)) return 0;
    return B.m[3][1] - m.pos.y;
}

struct PoseBox {
    Box body, all;
    bool okBody = false, okAll = false;
    double pelvisY = 0;
};

// poses of the caster spread over the clips, sampled on a COPY of the instance (the scene keeps its clip and time)
std::vector<PoseBox> SamplePoses(Scene& sc, int caster, const std::vector<FrameClip>& clips, int samples) {
    std::vector<PoseBox> out;
    ModelInstance* m = caster > 0 ? sc.Model(caster) : nullptr;
    if (!m || !m->Def() || clips.empty() || samples <= 0 || !sc.Cache()) return out;
    AssetCache& cache = *sc.Cache();
    ModelInstance cp = *m;
    struct Seg {
        const FrameClip* c;
        double len;
    };
    std::vector<Seg> segs;
    double total = 0;
    for (const auto& c : clips) {
        if (c.role < 0 || !cp.Play(cache, c.role, false, c.group)) continue;
        const ModelClip* mc = cp.Current();
        double len = mc ? mc->lengthMs : 0;
        if (c.ms > 0 && (len <= 0 || c.ms < len)) len = c.ms;
        if (len <= 0) len = 1;
        segs.push_back({&c, len});
        total += len;
    }
    if (segs.empty()) return out;
    for (int i = 0; i < samples; ++i) {
        double t = samples == 1 ? total * 0.5 : total * i / (samples - 1);
        std::size_t k = 0;
        while (k + 1 < segs.size() && t > segs[k].len) {
            t -= segs[k].len;
            ++k;
        }
        t = std::clamp(t, 0.0, std::max(0.0, segs[k].len - 1.0));
        cp.Play(cache, segs[k].c->role, false, segs[k].c->group);
        cp.SetTime(t);
        cp.Pose();
        PoseBox pb;
        pb.okBody = BodyBox(cp, pb.body);
        cp.Bounds(pb.all.mn, pb.all.mx);
        pb.okAll = Len(pb.all.mx - pb.all.mn) > 1e-6;
        pb.pelvisY = PelvisY(cp);
        out.push_back(pb);
    }
    return out;
}

void SetupCam(RCamera& cam, const PlayerFrameOptions& o) {
    cam.fovDeg = o.fovDeg;
    cam.H = 1000;
    cam.W = std::max(1, static_cast<int>(1000 * o.aspect));
}

bool Ndc(const RCamera& cam, const V3& P, double& x, double& y) {
    const V3 v = cam.ToView(P);
    if (v.z < 0.5) return false;
    x = v.x / v.z * cam.f * cam.H / cam.W;
    y = v.y / v.z * cam.f;
    return true;
}

}  // namespace

bool FitTargetFor(int weaponType, int mastery) {
    switch (mastery) {   // casters hit from afar whatever they hold (CH elements / force, EU wizard / warlock / bard / cleric)
        case 273: case 274: case 275: case 276: case 514: case 516: case 517: case 518: return true;
        default: break;
    }
    const Weapon* w = WeaponOf(weaponType);
    return w && !w->melee;
}

std::vector<FrameClip> FrameClipsOf(const se::sp::Timeline& tl) {
    std::vector<FrameClip> v;
    for (const auto& a : tl.anims) {
        if (a.phase != se::sp::Phase::Ready && a.phase != se::sp::Phase::Wait && a.phase != se::sp::Phase::Shot) continue;
        if (a.role < 0 || !a.found) continue;
        FrameClip c;
        c.role = a.role;
        c.group = a.group;
        double len = a.end - a.start;
        if (a.clipMs > 0 && (a.loop || len <= 0 || len > a.clipMs)) len = a.clipMs;
        c.ms = len > 0 ? len : 0;
        v.push_back(c);
    }
    return v;
}

std::wstring OtherGenderCode(const std::wstring& code) {
    const std::wstring u = Up(code);
    for (const wchar_t* race : {L"CHAR_CH_", L"CHAR_EU_"}) {
        if (u.rfind(race, 0) != 0) continue;
        const std::size_t n = std::wcslen(race);
        const bool lowerCase = code.size() > n && std::iswlower(code[n]);
        if (u.compare(n, 6, L"WOMAN_") == 0 || u.substr(n) == L"WOMAN")
            return code.substr(0, n) + (lowerCase ? L"man" : L"MAN") + code.substr(n + 5);
        if (u.compare(n, 4, L"MAN_") == 0 || u.substr(n) == L"MAN")
            return code.substr(0, n) + (lowerCase ? L"woman" : L"WOMAN") + code.substr(n + 3);
    }
    return {};
}

ShotFrame FramePlayer(Scene& sc, int caster, int target, const PlayerFrameOptions& o) {
    ShotFrame best;
    best.pitchDeg = o.pitchDeg;
    ModelInstance* cm = caster > 0 ? sc.Model(caster) : nullptr;
    Box cb, tb;
    if (!BoxOf(sc, caster, cb)) {
        cb.mn = {-5, 0, -5};
        cb.mx = {5, 18, 5};
    }
    Box body = cb;   // the caster height on screen is measured on the body, not on a long spear / staff
    if (cm) BodyBox(*cm, body);
    const double casterH = std::max(body.mx.y - body.mn.y, 4.0);
    // what must stay in frame: the body + fx pad, and the held weapon up to weaponKeep of its reach beyond the body
    // (a 36-unit spear may lose its far tip so the character keeps its size)
    const double reach = casterH * o.weaponReachMax;
    auto keep = [&](const Box& all, const Box& bd) {
        Box p = all;
        for (int a = 0; a < 3; ++a) {
            if (all.mn[a] < bd.mn[a]) p.mn[a] = bd.mn[a] - std::min((bd.mn[a] - all.mn[a]) * o.weaponKeep, reach);
            if (all.mx[a] > bd.mx[a]) p.mx[a] = bd.mx[a] + std::min((all.mx[a] - bd.mx[a]) * o.weaponKeep, reach);
        }
        return p;
    };
    Box pad = keep(cb, body);
    pad.mn = pad.mn - V3{o.fxPad, 0, o.fxPad};
    pad.mx = pad.mx + V3{o.fxPad, o.fxPad * 0.5, o.fxPad};
    // v6: every pose of the skill (READY + WAIT + SHOT samples) stays in frame: the union with each sample's body +
    // kept weapon (the fx pad only around the start pose: trails / auras are drawn by the effects, not the body)
    Box cbU = cb, bodyU = body;
    for (const auto& pb : SamplePoses(sc, caster, o.clips, o.samples)) {
        if (!pb.okBody) continue;
        Grow(bodyU, pb.body);
        Grow(cbU, pb.okAll ? pb.all : pb.body);
        Grow(pad, keep(pb.okAll ? pb.all : pb.body, pb.body));
        Grow(pad, pb.body);
    }
    const bool hasT = BoxOf(sc, target, tb);
    const bool fitT = hasT && (o.fitTarget > 0 || (o.fitTarget < 0 && FitTargetFor(o.weaponType, o.mastery)));
    const double minFrac = fitT ? o.minCasterFracFit : o.minCasterFrac;
    const auto cc = pad.Corners();
    const auto tc = tb.Corners();
    const auto bc = body.Corners();
    const V3 cCenter = cbU.Center(), bCenter = bodyU.Center();
    const V3 tCenter = hasT ? tb.Center() : cCenter;
    const V3 facing = Xf3(V3{0, 0, -1}, RotY(cm ? cm->yaw : 0));   // characters face -z in their own frame
    const bool autoYaw = o.yawDeg < 0;

    RCamera cam;
    SetupCam(cam, o);
    const double lim = 1.0 - o.margin, tlim = 1.0 - o.targetMargin;
    auto ndc = [&](const V3& P, double& x, double& y) { return Ndc(cam, P, x, y); };
    auto rectOf = [&](const std::array<V3, 8>& pts, double r[4]) {   // xmin ymin xmax ymax; false = behind the camera
        r[0] = r[1] = 1e9;
        r[2] = r[3] = -1e9;
        for (const auto& P : pts) {
            double x, y;
            if (!ndc(P, x, y)) return false;
            r[0] = std::min(r[0], x);
            r[1] = std::min(r[1], y);
            r[2] = std::max(r[2], x);
            r[3] = std::max(r[3], y);
        }
        return true;
    };
    double bestKey = -1e18;
    bool found = false;
    auto fit = [&](double yawDeg, double bonus) {
        const double yw = yawDeg * kPi / 180, pt = o.pitchDeg * kPi / 180;
        const V3 dir{std::sin(yw) * std::cos(pt), std::sin(pt), std::cos(yw) * std::cos(pt)};
        // a camera in front of the caster scores a little (face and both hands); bonus = weapon-hand side
        const double front = bonus + 2.0 * Dot(Norm(V3{dir.x, 0, dir.z}), Norm(V3{facing.x, 0, facing.z}));
        const int kSteps = hasT ? (fitT ? 21 : 15) : 1;
        for (int ki = 0; ki < kSteps * 2; ++ki) {
            const double k = (ki % kSteps) * 0.05;   // look-at slides from the caster towards the target (0 .. 0.7 / 1.0)
            const V3 base = ki < kSteps ? cCenter : bCenter;   // whole outfit or the body alone as the centre
            const V3 at = base + (tCenter - base) * k;
            for (double d = casterH * 0.6; d < 900; d *= 1.03) {
                cam.at = at;
                cam.eye = at + dir * d;
                cam.Setup();
                bool inside = true;
                for (const auto& P : cc) {
                    double x, y;
                    if (!ndc(P, x, y) || std::fabs(x) > lim || std::fabs(y) > lim) { inside = false; break; }
                }
                if (!inside) continue;
                double br[4];
                rectOf(bc, br);
                const double frac = (br[3] - br[1]) * 0.5;   // the body height on screen (no pad, no weapon)
                if (frac > o.maxCasterFrac) continue;
                double tScore = 0, cover = 0, near = 0, huge = 0;
                bool tAll = false;
                if (hasT) {
                    int in = 0, inM = 0;
                    for (const auto& P : tc) {
                        double x, y;
                        if (!ndc(P, x, y)) continue;
                        if (std::fabs(x) <= 1.0 && std::fabs(y) <= 1.0) ++in;
                        if (std::fabs(x) <= tlim && std::fabs(y) <= tlim) ++inM;
                    }
                    tAll = inM == 8;
                    double x, y;
                    const bool cIn = ndc(tCenter, x, y) && std::fabs(x) <= 0.92 && std::fabs(y) <= 0.92;
                    tScore = in / 8.0 + (cIn ? 0.75 : 0.0);
                    double tr[4];   // a target between the camera and the caster hides the body
                    const double dT = Len(tCenter - cam.eye), dC = Len(bCenter - cam.eye);
                    if (dT < dC) near = 1.0 - dT / dC;   // a foreground target (and its weapon) crowds the frame
                    if (dT < dC && rectOf(tc, tr)) {
                        const double ix = std::max(0.0, std::min(tr[2], br[2]) - std::max(tr[0], br[0]));
                        const double iy = std::max(0.0, std::min(tr[3], br[3]) - std::max(tr[1], br[1]));
                        const double ba = std::max(1e-6, (br[2] - br[0]) * (br[3] - br[1]));
                        cover = ix * iy / ba;
                        // a target that looks much bigger than the caster steals the frame
                        huge = std::max(0.0, (tr[3] - tr[1]) * 0.5 / std::max(frac, 1e-3) - 1.3);
                    }
                }
                const bool bigEnough = frac >= minFrac;
                double key;
                if (fitT) {   // v6: the whole target first, then the biggest caster (>= minCasterFracFit), no hiding
                    key = (tAll ? 5000.0 : 0.0) + (bigEnough ? 1000.0 + frac * 300.0 : frac * 1000.0) + tScore * 10.0 - cover * 60.0 - near * 30.0 -
                          huge * 10.0 + front;
                } else {
                    // big caster first (weapon readable), then as much of the target / hit area as fits without hiding
                    // the body, then the front, then closer
                    key = (bigEnough ? 1000.0 : frac * 1000.0) + tScore * 10.0 - cover * 25.0 - near * 20.0 - huge * 10.0 + front +
                          (hasT ? frac : -std::fabs(frac - (o.minCasterFrac + o.maxCasterFrac) * 0.5));
                }
                if (key > bestKey) {
                    bestKey = key;
                    best.at = at;
                    best.dist = d;
                    best.yawDeg = yawDeg;
                    found = true;
                }
            }
        }
    };
    if (!autoYaw) fit(o.yawDeg, 0);
    else {   // front quarters of the weapon hand (preferred) and of the shield arm
        int ss = 0, ws = -1;
        HeldSides(cm, ss, ws);
        if (fitT) {   // caster and target side by side need the side views too
            for (double local : {150.0, 135.0, 120.0, 105.0, 90.0, 75.0}) {
                fit(WorldYaw(cm, ws > 0 ? local : 360.0 - local), 2.0);
                fit(WorldYaw(cm, ws > 0 ? 360.0 - local : local), 0.0);
            }
        } else {
            for (double local : {150.0, 130.0, 115.0}) fit(WorldYaw(cm, ws > 0 ? local : 360.0 - local), 2.0);
            if (!o.clips.empty()) fit(WorldYaw(cm, ws > 0 ? 165.0 : 195.0), 2.0);   // a lunge reads shorter from the front
            if (ss && ss != ws)
                for (double local : {150.0, 130.0}) fit(WorldYaw(cm, ss > 0 ? local : 360.0 - local), 0.0);
        }
    }
    if (!found) {
        best.yawDeg = autoYaw ? AutoPlayerYaw(sc, caster) : o.yawDeg;
        best.at = cCenter;
        best.dist = casterH * 2.6;
    }
    return best;
}

FrameReport MeasurePlayerFrame(Scene& sc, int caster, int target, const ShotFrame& f, const PlayerFrameOptions& o) {
    FrameReport r;
    RCamera cam;
    SetupCam(cam, o);
    const double yw = f.yawDeg * kPi / 180, pt = f.pitchDeg * kPi / 180;
    const V3 dir{std::sin(yw) * std::cos(pt), std::sin(pt), std::cos(yw) * std::cos(pt)};
    cam.at = f.at;
    cam.eye = f.at + dir * f.dist;
    cam.Setup();
    auto inside = [&](const Box& b) {
        int n = 0;
        for (const auto& P : b.Corners()) {
            double x, y;
            if (Ndc(cam, P, x, y) && std::fabs(x) <= 1.0 && std::fabs(y) <= 1.0) ++n;
        }
        return n / 8.0;
    };
    ModelInstance* cm = caster > 0 ? sc.Model(caster) : nullptr;
    Box body;
    if (cm && BodyBox(*cm, body)) {
        double lo = 1e9, hi = -1e9;
        for (const auto& P : body.Corners()) {
            double x, y;
            if (!Ndc(cam, P, x, y)) continue;
            lo = std::min(lo, y);
            hi = std::max(hi, y);
        }
        if (hi > lo) r.bodyFrac = (hi - lo) * 0.5;
    }
    if (cm) r.standPelvisY = PelvisY(*cm);
    Box tb;
    if (BoxOf(sc, target, tb)) {
        r.hasTarget = true;
        r.targetIn = inside(tb);
    }
    for (const auto& pb : SamplePoses(sc, caster, o.clips, o.samples)) {
        r.sampleBodyIn.push_back(pb.okBody ? inside(pb.body) : 0.0);
        r.sampleAllIn.push_back(pb.okAll ? inside(pb.all) : 0.0);
        r.samplePelvisY.push_back(pb.pelvisY);
    }
    return r;
}

void ApplyFrame(Scene& sc, const ShotFrame& f) {
    const double yw = f.yawDeg * kPi / 180, pt = f.pitchDeg * kPi / 180;
    const V3 dir{std::sin(yw) * std::cos(pt), std::sin(pt), std::cos(yw) * std::cos(pt)};
    sc.camera.at = f.at;
    sc.camera.eye = f.at + dir * f.dist;
    sc.camera.Setup();
}

}  // namespace se::render
