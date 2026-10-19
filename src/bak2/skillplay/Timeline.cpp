// SkillEffect Studio - skillplay/Timeline.cpp
// One presentation key -> timeline (SRO_SKILLS.md §5.4):
//   |<- PreparingTime: AniReady once, then AniWait looping ->|<- SHOT clip = CastingTime + ActionDuration ->|
//   READY / WAIT / SHOT rows start at the k-th type-1 event of their phase clip (0 = phase start) + MOV delay;
//   DMG Event TRUE rows = hits (projectiles: at the arrival); ACT_S/ACT_L/DEACT = status / buff on the affected actor
//   from the first hit for the buff time; S_RETURN after the outbound projectile; CreateCnt copies; sounds.
#include "SkillPlay.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace se::sp {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    std::uint32_t Next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    double U() { return (Next() & 0xFFFFFF) / 16777216.0; }  // [0,1)
    size_t Pick(size_t n) { return n ? Next() % n : 0; }
};

std::wstring Up(std::wstring_view s) { return ToUpper(s); }

void Note(Timeline& tl, int sev, int row, std::wstring tr, std::wstring en) {
    TlNote n;
    n.sev = sev;
    n.row = row;
    n.tr = std::move(tr);
    n.en = std::move(en);
    tl.notes.push_back(std::move(n));
}

// estimate of a bone position in world space for the preview geometry (caster at the origin facing -Z,
// target at (0,0,-distance) facing +Z). Only used for flight-time estimates; the player uses real bones.
Vec3 EstBone(const IModelInfo* m, const std::wstring& bone, int actor, double distance) {
    Vec3 p;
    if (!bone.empty() && bone != L"*") {
        if (!(m && m->BoneRest(bone, p))) p = {0, (m ? m->Height() : 18) * 0.6, 0};
    }
    if (actor == 1) return Vec3{-p.x, p.y, -distance - p.z};  // turned to face the caster
    return p;
}

}  // namespace

const wchar_t* PlaceName(Place p) {
    switch (p) {
        case Place::Follow: return L"follow";
        case Place::Fixed: return L"fixed";
        case Place::Target: return L"target";
        case Place::TargetFollow: return L"target-follow";
        case Place::DmgPos: return L"dmg-pos";
        case Place::Projectile: return L"projectile";
    }
    return L"?";
}

Timeline BuildTimeline(const FxDb& db, const std::wstring& key, const SkillInfo& skill, const IModelInfo* caster,
                       const IModelInfo* target, const PlayOptions& opt) {
    Timeline tl;
    tl.key = key;
    tl.skill = skill;
    tl.aniSet = db.AniSet(key);
    tl.rows = db.Effects(key);
    tl.rowUsed.assign(tl.rows.size(), false);
    Rng rng(opt.seed);
    const AniSetRow* sa = tl.aniSet;
    if (!sa && tl.rows.empty()) {
        Note(tl, 0, -1, L"Anahtarın skillaniset2 ve skilleffectset satırı yok: oynatılacak bir şey yok.",
             L"The key has no skillaniset2 or skilleffectset row: nothing to play.");
        return tl;
    }
    tl.pendingRows = (sa && sa->origin == Origin::Pending);
    for (const EffectRow* r : tl.rows)
        if (r->origin == Origin::Pending) tl.pendingRows = true;
    if (!sa)
        Note(tl, 1, -1, L"skillaniset2 satırı yok: animasyon oynamaz; efekt satırları adlı nesneye bağlanır (yalnız efekt anahtarları için normal).",
             L"No skillaniset2 row: no animation; the effect rows attach to a named object (normal for effect-only keys only).");
    else if (!sa->service) {
        Note(tl, 1, -1, L"skillaniset2 Service = 0: istemci satırı yok sayar (önizleme yine de oynatır).",
             L"skillaniset2 Service = 0: the client ignores the row (the preview still plays it).");
        if (!opt.playDisabled) return tl;
    }
    if (!skill.valid)
        Note(tl, 2, -1, L"skilldata satırı verilmedi: süreler animasyondan alınır (PreparingTime 0).",
             L"No skilldata row given: timing comes from the clips (PreparingTime 0).");

    const CharInfoRow* cci = opt.casterCi.empty() ? nullptr : db.CharInfo(opt.casterCi);
    const CharInfoRow* tci = opt.targetCi.empty() ? nullptr : db.CharInfo(opt.targetCi);
    if (!opt.casterCi.empty() && !cci)
        Note(tl, 1, -1, L"Canavarın characterInfo satırı yok (" + opt.casterCi + L"): MOB_BASE ölçeği 1, ses nesnesi yok.",
             L"The creature has no characterInfo row (" + opt.casterCi + L"): MOB_BASE scale 1, no sound object.");
    const double casterSize = cci ? cci->size : 1.0;
    const double D = opt.targetDistance > 1 ? opt.targetDistance : 60;
    const double targetH = target ? target->Height() : (caster ? caster->Height() : 18);
    const Vec3 dmgPos = tci && !tci->damagePos.Zero() ? tci->damagePos : Vec3{0, targetH * 0.6, 0};

    // ---------------------------------------------------------------------------------------------- clips
    auto clipOf = [&](int role, const wchar_t* what, ClipInfo& ci) {
        ci = ClipInfo{};
        if (role < 0) return false;
        if (caster && caster->ClipIn(role, sa ? sa->aniGroup : std::wstring(), opt.weaponGroup, ci) && ci.found) return true;
        if (caster)
            Note(tl, 1, -1,
                 Fmt(L"%ls: model bu animasyonu içermiyor (%ls): animasyon oynamaz (oyunda da 63 skillde böyle).", what,
                     AniTokenOf(role).c_str()),
                 Fmt(L"%ls: the model has no %ls clip: no animation plays (63 shipped skills do the same).", what,
                     AniTokenOf(role).c_str()));
        return false;
    };
    ClipInfo cReady, cWait, cShot;
    const bool hasReady = sa && sa->ready >= 0, hasWait = sa && sa->wait >= 0;
    clipOf(hasReady ? sa->ready : -1, L"AniReady", cReady);
    clipOf(hasWait ? sa->wait : -1, L"AniWait", cWait);
    if (sa && !sa->shot.empty()) {
        size_t idx = opt.shotPick >= 0 ? static_cast<size_t>(opt.shotPick) : rng.Pick(sa->shot.size());
        if (idx >= sa->shot.size()) idx = 0;
        tl.shotRole = sa->shot[idx];
        if (sa->shot.size() > 1)
            Note(tl, 2, -1, Fmt(L"AniShot listesi (%zu klip): istemci rastgele seçer; seçilen %ls.", sa->shot.size(), AniTokenOf(tl.shotRole).c_str()),
                 Fmt(L"AniShot list (%zu clips): the client picks at random; chosen %ls.", sa->shot.size(), AniTokenOf(tl.shotRole).c_str()));
    } else if (sa && !sa->shotTok.empty() && sa->shotTok != L"none")
        Note(tl, 1, -1, L"AniShot tanınmıyor: " + sa->shotTok, L"AniShot not recognised: " + sa->shotTok);
    clipOf(tl.shotRole, L"AniShot", cShot);

    const double cast = skill.valid ? skill.castMs : 0, dur = skill.valid ? skill.durMs : 0;
    const double readyMs = cReady.found ? (cReady.durationMs > 0 ? cReady.durationMs : 1000) : (hasReady ? 600 : 0);
    const double waitMs = cWait.found ? (cWait.durationMs > 0 ? cWait.durationMs : 1000) : (hasWait ? 600 : 0);
    double prep = skill.valid ? skill.prepMs : 0;
    if (prep <= 0 && (hasReady || hasWait)) {
        prep = readyMs + waitMs;
        Note(tl, 2, -1, L"PreparingTime 0 ama READY/WAIT klipleri var: her biri bir kez oynatıldı [U].",
             L"PreparingTime 0 but READY/WAIT clips are set: each is played once [U].");
    }
    tl.prepMs = prep;
    const double readyEnd = hasReady ? (hasWait ? std::min(readyMs, prep) : prep) : 0;
    const double waitStart = readyEnd;
    tl.shotStart = prep;
    double shotDur = cShot.found && cShot.durationMs > 0 ? cShot.durationMs : (cast + dur > 0 ? cast + dur : (tl.shotRole >= 0 ? 1000 : 0));
    tl.shotEnd = tl.shotStart + shotDur;
    for (double e : cShot.events) tl.shotEvents.push_back(tl.shotStart + e);
    if (cShot.found && cShot.durationMs > 0 && skill.valid && cast + dur > 0 && std::fabs(cast + dur - cShot.durationMs) > 5)
        Note(tl, 2, -1,
             Fmt(L"Skill süresi (döküm + hareket) %.0f ms, saldırı animasyonu %.0f ms: oyundaki skillerin çoğunda ikisi eşittir; animasyon kendi süresince oynar.",
                 cast + dur, cShot.durationMs),
             Fmt(L"Skill time (cast + action) %.0f ms, attack animation %.0f ms: most shipped skills have them equal; the animation plays its own length.",
                 cast + dur, cShot.durationMs));

    // animation segments
    if (hasReady) {
        AnimSeg s;
        s.phase = Phase::Ready;
        s.role = sa->ready;
        s.token = sa->readyTok;
        s.start = 0;
        s.end = readyEnd;
        s.loop = !hasWait && readyEnd > readyMs + 1;
        s.found = cReady.found;
        s.clipMs = readyMs;
        s.file = cReady.file;
        s.group = cReady.group;
        tl.anims.push_back(s);
    }
    if (hasWait && prep > waitStart) {
        AnimSeg s;
        s.phase = Phase::Wait;
        s.role = sa->wait;
        s.token = sa->waitTok;
        s.start = waitStart;
        s.end = prep;
        s.loop = true;
        s.found = cWait.found;
        s.clipMs = waitMs;
        s.file = cWait.file;
        s.group = cWait.group;
        tl.anims.push_back(s);
    }
    if (!hasReady && !hasWait && prep > 0) {
        AnimSeg s;
        s.phase = Phase::Ready;
        s.token = L"(stand)";
        s.end = prep;
        s.loop = true;
        tl.anims.push_back(s);
        Note(tl, 2, -1, L"PreparingTime var ama AniReady/AniWait yok: canavar hazırlık süresince bekler.",
             L"PreparingTime without AniReady/AniWait: the creature stands during the preparation.");
    }
    if (tl.shotRole >= 0) {
        AnimSeg s;
        s.phase = Phase::Shot;
        s.role = tl.shotRole;
        s.token = AniTokenOf(tl.shotRole);
        s.start = tl.shotStart;
        s.end = tl.shotEnd;
        s.found = cShot.found;
        s.clipMs = shotDur;
        s.file = cShot.file;
        s.group = cShot.group;
        tl.anims.push_back(s);
    }

    // ------------------------------------------------------------------------------------------ row timing
    int kmax[10] = {};
    for (const EffectRow* r : tl.rows)
        if (r->phase >= Phase::Ready && r->phase <= Phase::SReturn)
            kmax[static_cast<int>(r->phase)] = std::max(kmax[static_cast<int>(r->phase)], r->startEvent);
    // ok=false: row skipped
    auto evTime = [&](const EffectRow& r, int ri, bool& ok) -> double {
        ok = true;
        double base = 0;
        const ClipInfo* c = nullptr;
        double clipLen = 0;
        if (r.phase == Phase::Ready) {
            base = 0;
            c = &cReady;
            clipLen = readyMs;
        } else if (r.phase == Phase::Wait) {
            base = waitStart;
            c = &cWait;
            clipLen = waitMs;
        } else {
            base = tl.shotStart;
            c = &cShot;
            clipLen = shotDur;
        }
        const int k = r.startEvent;
        if (k <= 0) return base;
        if (static_cast<int>(c->events.size()) >= k) return base + c->events[static_cast<size_t>(k - 1)];
        if (!c->events.empty()) {
            if (!opt.clampEvents) {
                ok = false;
                Note(tl, 1, ri, Fmt(L"StartEvent %d, klipte %zu olay var: satır oynatılmadı.", k, c->events.size()),
                     Fmt(L"StartEvent %d, the clip has %zu events: row not played.", k, c->events.size()));
                return 0;
            }
            Note(tl, 1, ri, Fmt(L"StartEvent %d, klipte yalnız %zu tip-1 olay var: son olaya kaydırıldı [U].", k, c->events.size()),
                 Fmt(L"StartEvent %d but the clip has only %zu type-1 events: moved to the last one [U].", k, c->events.size()));
            return base + c->events.back();
        }
        // no events at all (clip missing or eventless): spread over the casting time
        const int km = std::max(1, kmax[static_cast<int>(r.phase)]);
        const double span = (r.phase == Phase::Shot && cast > 0) ? cast : clipLen * 0.5;
        Note(tl, 1, ri, Fmt(L"Klipte tip-1 olay yok: StartEvent %d tahmini %.0f ms.", k, span * k / km),
             Fmt(L"The clip has no type-1 event: StartEvent %d estimated at %.0f ms.", k, span * k / km));
        return base + span * k / km;
    };

    // is the skill an attack (status / buff rows then show on the target)?
    bool attack = sa && sa->isAttack == 1;
    for (const EffectRow* r : tl.rows)
        if (r->dmg == 1) attack = true;
    const int buffActor = attack ? 1 : 0;

    auto scaleOf = [&](const EffectRow& r) {
        double s = 1;
        if (r.scale == ScaleBase::Mob) s *= casterSize;
        else if (r.scale == ScaleBase::Char) s *= opt.casterScale;
        if ((r.script & kSctEffectScale) && !r.scriptArgs.empty() && r.scriptArgs[0] > 0) s *= r.scriptArgs[0];
        return s;
    };
    const std::wstring damageEfp = sa ? sa->damageEfp : std::wstring();
    const std::wstring bloodEfp =
        (sa && sa->blood == 1 && tci && !tci->bloodType.empty()) ? L"hiteffect\\" + tci->bloodType + L".efp" : std::wstring();

    auto addHit = [&](double t, int ri, int fxIdx) {
        HitEvent h;
        h.t = t;
        h.index = static_cast<int>(tl.hits.size());
        h.row = ri;
        h.fx = fxIdx;
        h.kinds = opt.hitKind;
        h.damageEfp = damageEfp;
        h.bloodEfp = bloodEfp;
        h.damagePos = dmgPos;
        tl.hits.push_back(h);
    };
    auto addSound = [&](double t, const std::wstring& path, const std::wstring& why, int actor, int fxIdx, bool approx, int vol) {
        SoundEvent s;
        s.t = t;
        s.path = path;
        s.why = why;
        s.actor = actor;
        s.fx = fxIdx;
        s.approx = approx;
        s.volume = vol;
        tl.sounds.push_back(s);
    };

    std::vector<double> arrivals;  // outbound projectile arrival times (S_RETURN)
    double buffStart = -1;

    auto emitRow = [&](int ri, double t0, int actor, double holdUntil) {
        const EffectRow& r = *tl.rows[static_cast<size_t>(ri)];
        if (r.damageTypes != 0 && !(r.damageTypes & opt.hitKind)) {
            Note(tl, 2, ri, L"Satır yalnız başka bir vuruş türünde oynar (DamageType): atlandı.",
                 L"The row plays only for another damage kind (DamageType): skipped.");
            return;
        }
        for (const auto& pn : r.parseNotes) Note(tl, 1, ri, L"Okunamayan hücre: " + pn, L"Unparsed cell: " + pn);
        if (r.act == Act::Bad) return;
        int n = r.createCnt;
        if (n < 1) n = 1;
        if (n > 32) n = 32;
        const double scale = scaleOf(r);
        const bool proj = r.Projectile();
        bool used = false;
        int firstFx = -1;
        double firstArrive = -1;
        for (int c = 0; c < n; ++c) {
            const double tc = t0 + r.movDelay + c * opt.volleyMs;
            if (r.kind == ObjKind::Wav) {
                if (c == 0) addSound(tc, r.ResPath(), Fmt(L"ObjName (.wav) row %d", ri), actor, -1, false, 100);
                used = true;
                continue;
            }
            if (r.kind != ObjKind::Efp && r.kind != ObjKind::Bsr) continue;
            FxEvent e;
            e.t = tc;
            e.row = ri;
            e.copy = c;
            e.kind = r.kind;
            e.res = r.ResPath();
            e.actor = actor;
            e.bone = r.startBone;
            e.offset = r.startOff;
            e.scale = scale;
            e.fadeIn = r.fadeIn;
            e.fadeOut = r.fadeOut;
            e.instId = r.id;
            e.holdUntil = holdUntil;
            e.mov = r.mov;
            e.arrive = r.Res2Path();
            e.dmg = r.dmg == 1 && c == 0;
            const bool movAct = r.act == Act::Mov1Tar || r.act == Act::MovOption || r.act == Act::MovApiece ||
                                r.act == Act::MovSplash || r.act == Act::MovPierce;
            if (proj && (movAct || r.act == Act::Target || r.act == Act::Source || r.act == Act::TargetF || r.act == Act::Stop ||
                         r.act == Act::OneFollow || r.act == Act::DmgPos || r.act == Act::Loop)) {
                e.place = Place::Projectile;
                e.speed = r.speedMin + rng.U() * (r.speedMax - r.speedMin);
                e.fromActor = 0;
                e.toActor = 1;
                e.fromBone = r.startBone;
                e.fromOff = r.startOff;
                e.toBone = r.targetBone;
                e.toOff = r.targetOff;
                e.homing = r.act == Act::Mov1Tar || r.act == Act::MovApiece || r.act == Act::MovSplash;
                if (r.act == Act::Target || r.act == Act::TargetF || r.act == Act::DmgPos) {
                    // falling / rising objects around the target (Demon Shaitan rocks): both ends in target space
                    e.fromTargetSpace = true;
                    e.fromActor = 1;
                    e.homing = false;
                }
                if (r.mov == Mov::Upr || r.mov == Mov::Round || r.mov == Mov::Up) {
                    e.arc = r.param[0] > 0 ? r.param[0] / 1000.0 : 0.25;
                    const double a = r.param[1], b = r.param[2] >= r.param[1] ? r.param[2] : r.param[1];
                    e.arcRollDeg = (a == 0 && b == 0) ? 0 : (a + rng.U() * (b - a)) - 90;
                    if (r.mov == Mov::Up) e.arcRollDeg = 0;
                    e.note = L"arc [U]";
                }
                if (r.act == Act::MovPierce) e.overshoot = 0.8;
                if (r.act == Act::MovOption) {
                    // radial spread: the ActOption angle around the caster, flying for the option time [U]
                    e.angleDeg = r.opt.angle;
                    e.homing = false;
                    e.toActor = 0;
                    e.toBone.clear();
                    const double life = r.opt.ms > 0 ? r.opt.ms : 1000;
                    const double dist = e.speed * life / 1000.0;
                    const double a = r.opt.angle * kPi / 180.0;
                    e.fromOff = e.fromOff + Vec3{std::sin(a), 0, -std::cos(a)} * r.opt.b;
                    e.toOff = e.fromOff + Vec3{std::sin(a), 0, -std::cos(a)} * dist;
                    e.note = L"ActOption radial [U]";
                }
                if (r.phase == Phase::SReturn || (r.mov == Mov::Hwan && r.act == Act::Source)) {
                    std::swap(e.fromActor, e.toActor);
                    std::swap(e.fromBone, e.toBone);
                    std::swap(e.fromOff, e.toOff);
                    e.homing = true;
                }
                // flight estimate in the preview geometry
                const IModelInfo* mf = e.fromActor == 0 ? caster : target;
                const IModelInfo* mt = e.toActor == 0 ? caster : target;
                Vec3 from, to;
                if (e.fromTargetSpace) {
                    from = Vec3{0, 0, -D} + Vec3{-e.fromOff.x, e.fromOff.y, -e.fromOff.z};
                    to = Vec3{0, 0, -D} + Vec3{-e.toOff.x, e.toOff.y, -e.toOff.z};
                } else {
                    const Vec3 fb = EstBone(mf ? mf : caster, e.fromBone, e.fromActor, D);
                    const Vec3 tb = EstBone(mt ? mt : target, e.toBone, e.toActor, D);
                    from = fb + (e.fromActor == 0 ? e.fromOff : Vec3{-e.fromOff.x, e.fromOff.y, -e.fromOff.z});
                    to = (e.toActor == 1 && e.toBone.empty()) ? Vec3{0, dmgPos.y, -D} : tb;
                    to = to + (e.toActor == 0 ? e.toOff : Vec3{-e.toOff.x, e.toOff.y, -e.toOff.z});
                    if (r.act == Act::MovOption) to = e.toOff;
                }
                double dist = (to - from).Len() * (1 + e.overshoot);
                const double sp = e.speed > 1 ? e.speed : 200;
                e.flightMs = std::max(50.0, dist / sp * 1000.0);
            } else {
                switch (r.act) {
                    case Act::OneFollow: e.place = Place::Follow; break;
                    case Act::Loop:
                        e.place = Place::Follow;
                        if (e.holdUntil < 0) e.holdUntil = holdUntil >= 0 ? holdUntil : tl.shotEnd;
                        break;
                    case Act::Stop:
                    case Act::Source: e.place = Place::Fixed; break;
                    case Act::Target:
                        e.place = Place::Target;
                        e.actor = 1;
                        e.bone = r.targetBone;
                        e.offset = r.targetOff;
                        break;
                    case Act::TargetF:
                        e.place = Place::TargetFollow;
                        e.actor = 1;
                        e.bone = r.targetBone;
                        e.offset = r.targetOff;
                        break;
                    case Act::DmgPos:
                        e.place = Place::DmgPos;
                        e.actor = 1;
                        e.bone.clear();
                        e.offset = dmgPos + r.targetOff;
                        break;
                    default:  // AT_MOV_* without a speed: shown at the target
                        e.place = Place::DmgPos;
                        e.actor = 1;
                        e.bone.clear();
                        e.offset = dmgPos + r.targetOff;
                        e.note = L"AT_MOV_* without speed -> at the target [U]";
                        break;
                }
            }
            if (caster && e.actor == 0 && !e.bone.empty() && e.bone != L"*" && !caster->HasBone(e.bone) && c == 0)
                Note(tl, 1, ri, L"Kemik iskelette yok: " + e.bone + L" (efekt kökte doğar).",
                     L"Bone not in the skeleton: " + e.bone + L" (the effect starts at the root).");
            if (e.res.empty()) continue;
            tl.fx.push_back(e);
            const int fi = static_cast<int>(tl.fx.size()) - 1;
            if (c == 0) {
                firstFx = fi;
                if (e.place == Place::Projectile) {
                    firstArrive = e.t + e.flightMs;
                    arrivals.push_back(firstArrive);
                }
            }
            used = true;
            // arrow trail / force of projectiles (skillaniset2 ArrowTail / ArrowForce) [D]
            if (e.place == Place::Projectile && sa && c == 0) {
                if (!sa->arrowTail.empty()) {
                    FxEvent tail = e;
                    tail.res = sa->arrowTail;
                    tail.kind = ObjKind::Efp;
                    tail.instId = 0;
                    tail.arrive.clear();
                    tail.dmg = false;
                    tail.note = L"ArrowTail";
                    tl.fx.push_back(tail);
                }
                if (!sa->arrowForce.empty()) {
                    FxEvent f = e;
                    f.place = Place::Fixed;
                    f.res = sa->arrowForce;
                    f.kind = ObjKind::Efp;
                    f.instId = 0;
                    f.actor = e.fromActor;
                    f.bone = e.fromBone;
                    f.offset = e.fromOff;
                    f.arrive.clear();
                    f.dmg = false;
                    f.note = L"ArrowForce";
                    tl.fx.push_back(f);
                }
            }
        }
        const double t1 = t0 + r.movDelay;
        if (r.trade > 0) tl.kills.push_back(KillEvent{t1, r.trade, ri, true});
        if (r.kill > 0) tl.kills.push_back(KillEvent{t1, r.kill, ri, false});
        if (r.trade > 0 || r.kill > 0) used = true;
        // hit (one per DMG row; projectiles at the arrival)
        if (r.dmg == 1) {
            addHit(firstArrive >= 0 ? firstArrive : t1, ri, firstArrive >= 0 ? firstFx : -1);
            used = true;
        }
        if (!r.sndBegin.empty()) {
            addSound(t1, L"prim\\snd\\" + r.sndBegin, Fmt(L"SndBegin row %d", ri), actor, -1, false, 100);
            used = true;
        }
        if (!r.sndEnd.empty()) {
            addSound(firstArrive >= 0 ? firstArrive : t1, L"prim\\snd\\" + r.sndEnd, Fmt(L"SndEnd row %d", ri),
                     firstArrive >= 0 ? 1 : actor, firstArrive >= 0 ? firstFx : -1, false, 100);
            used = true;
        }
        // camera shake: SHAKECAM_MOVn at the arrival of a moving object, SHAKECAMn at the row time [D]
        static const std::pair<int, int> shakes[] = {{kSctShake0, 0}, {kSctShake1, 1}, {kSctShake2, 2}, {kSctShake3, 3}};
        static const std::pair<int, int> shakesMov[] = {{kSctShakeMov0, 0}, {kSctShakeMov1, 1}, {kSctShakeMov2, 2}, {kSctShakeMov3, 3}};
        for (const auto& [flag, lvl] : shakes)
            if (r.script & flag) {
                tl.cams.push_back(CamEvent{t1, lvl, false, ri, -1});
                used = true;
            }
        for (const auto& [flag, lvl] : shakesMov)
            if (r.script & flag) {
                tl.cams.push_back(CamEvent{firstArrive >= 0 ? firstArrive : t1, lvl, true, ri, firstArrive >= 0 ? firstFx : -1});
                used = true;
            }
        if ((r.script & kSctMat) && r.scriptArgs.size() >= 6) {
            MatEvent m;
            m.t = t1;
            m.actor = actor;
            m.row = ri;
            for (int i = 0; i < 6; ++i) m.rgb[i] = r.scriptArgs[static_cast<size_t>(i)];
            m.ms = r.scriptArgs.size() > 6 ? r.scriptArgs[6] : 500;
            tl.mats.push_back(m);
            used = true;
        }
        if (r.script & (kSctCharScale | kSctRut | kSctMover | kSctArrow))
            Note(tl, 2, ri, L"Betik bayrağı yalnız listelenir: " + r.scriptText, L"Script flag listed only: " + r.scriptText);
        if (used) tl.rowUsed[static_cast<size_t>(ri)] = true;
    };

    // pass 1: READY / WAIT / SHOT
    for (int ri = 0; ri < static_cast<int>(tl.rows.size()); ++ri) {
        const EffectRow& r = *tl.rows[static_cast<size_t>(ri)];
        if (r.phase != Phase::Ready && r.phase != Phase::Wait && r.phase != Phase::Shot) continue;
        bool ok = true;
        const double t = evTime(r, ri, ok);
        if (!ok) continue;
        double hold = -1;
        if (r.act == Act::Loop) hold = (r.phase == Phase::Shot) ? tl.shotEnd : tl.shotStart;
        emitRow(ri, t, 0, hold);
    }
    // buff / status window: from the first hit (or the first SHOT event) for the buff time
    {
        double first = -1;
        for (const auto& h : tl.hits) first = first < 0 ? h.t : std::min(first, h.t);
        if (first < 0) first = tl.shotStart + (!cShot.events.empty() ? cShot.events.front() : cast);
        buffStart = first;
        // a long buff (minutes) is shown shortened: the preview plays its start, a few seconds of its loop and its end
        constexpr double kMaxBuffPreviewMs = 6000;
        const double blReal = skill.buffMs > 0 ? skill.buffMs : opt.buffMs;
        const double bl = std::min(blReal, kMaxBuffPreviewMs);
        if (blReal > kMaxBuffPreviewMs)
            Note(tl, 2, -1, Fmt(L"Güçlendirme süresi %.0f sn: önizlemede kısaltılmış %.0f sn gösterilir (başı, döngüsü, sonu).", blReal / 1000, bl / 1000),
                 Fmt(L"Buff time %.0f s: the preview shows it shortened to %.0f s (start, loop, end).", blReal / 1000, bl / 1000));
        bool any = false;
        for (const EffectRow* r : tl.rows)
            if (r->phase >= Phase::ActOS && r->phase <= Phase::Deact) any = true;
        if (any) {
            tl.buffStart = buffStart;
            tl.buffEnd = buffStart + bl;
            if (skill.buffMs <= 0)
                Note(tl, 2, -1, Fmt(L"Buff/durum süresi bilinmiyor: %.0f ms gösterildi.", bl),
                     Fmt(L"Buff / status duration unknown: %.0f ms shown.", bl));
        }
    }
    // pass 2: ACT_* / DEACT / S_RETURN
    for (int ri = 0; ri < static_cast<int>(tl.rows.size()); ++ri) {
        const EffectRow& r = *tl.rows[static_cast<size_t>(ri)];
        if (r.phase == Phase::Ready || r.phase == Phase::Wait || r.phase == Phase::Shot) continue;
        if (r.phase == Phase::Bad) {
            Note(tl, 1, ri, L"AniType tanınmıyor: satır oynatılmaz.", L"AniType not recognised: the row never plays.");
            continue;
        }
        if (r.dmg == 1 && r.phase != Phase::SReturn)
            Note(tl, 0, ri, L"DMG Event TRUE yalnız READY/WAIT/SHOT/S_RETURN'de olabilir (istemci assert \"i < 3 || i == 9\").",
                 L"DMG Event TRUE only on READY/WAIT/SHOT/S_RETURN (client assert \"i < 3 || i == 9\").");
        int actor = buffActor;
        if (r.phase == Phase::ActOS || r.phase == Phase::ActOL || r.phase == Phase::ActOE) {
            if (!opt.partyOnCaster) continue;
            actor = 0;
            Note(tl, 2, ri, L"ACT_O*: grup üyelerinde oynar; önizlemede canavarın üstünde.",
                 L"ACT_O*: plays on party members; shown on the caster in the preview.");
        }
        if (r.startEvent > 0 && r.phase != Phase::SReturn)
            Note(tl, 2, ri, L"Buff fazında StartEvent kullanılmaz [U].", L"StartEvent is not used in buff phases [U].");
        double t = tl.buffStart >= 0 ? tl.buffStart : buffStart, hold = -1;
        switch (r.phase) {
            case Phase::ActS:
            case Phase::ActOS: hold = r.act == Act::Loop ? tl.buffEnd : -1; break;
            case Phase::ActL:
            case Phase::ActOL: hold = tl.buffEnd; break;
            case Phase::ActOE:
            case Phase::Deact: t = tl.buffEnd >= 0 ? tl.buffEnd : t; break;
            case Phase::SReturn:
                t = arrivals.empty() ? tl.shotEnd : *std::max_element(arrivals.begin(), arrivals.end());
                actor = 1;
                break;
            default: break;
        }
        emitRow(ri, t, actor, hold);
    }

    // projectile details the preview estimates (not verified in the client code): one plain note
    {
        bool proj = false, volley = false, arc = false;
        for (const auto& e : tl.fx) {
            proj = proj || e.place == Place::Projectile;
            volley = volley || e.copy > 0;
            arc = arc || e.note.find(L"arc") != std::wstring::npos;
        }
        if (proj)
            Note(tl, 2, -1,
                 std::wstring(L"Mermi: hız satırdaki aralıktan rastgele, uçuş süresi = önizleme mesafesi / hız") + (volley ? L", kopyalar 100 ms arayla" : L"") +
                     (arc ? L", yay yüksekliği Param ile" : L"") + L" [U].",
                 std::wstring(L"Projectile: speed random within the row's range, flight time = preview distance / speed") +
                     (volley ? L", copies 100 ms apart" : L"") + (arc ? L", arc height from Param" : L"") + L" [U].");
    }

    // ID / Trade / Kill: a later Trade n / Kill n row releases the earlier instances named n [D]
    for (const KillEvent& k : tl.kills) {
        int hit = 0;
        for (auto& e : tl.fx)
            if (e.instId == k.id && e.t <= k.t && e.row != k.row && (e.releaseAt < 0 || e.releaseAt > k.t)) {
                e.releaseAt = k.t;
                if (e.holdUntil > k.t) e.holdUntil = k.t;
                ++hit;
            }
        if (!hit)
            Note(tl, 2, k.row, Fmt(L"%ls %d: bu anahtarda önce ID %d ile yaratılan efekt yok [D].", k.trade ? L"Trade" : L"Kill", k.id, k.id),
                 Fmt(L"%ls %d: no earlier instance with ID %d in this key [D].", k.trade ? L"Trade" : L"Kill", k.id, k.id));
    }

    // no DMG Event TRUE row but the skill deals damage (Param att / mc): the server still sends the damage; how the
    // client shows it then is not verified [U]. The preview shows ONE estimated hit at the attack event (marked).
    if (tl.hits.empty() && skill.valid && (skill.hits > 0 || skill.params.count("att")) && tl.shotRole >= 0) {
        addHit(!tl.shotEvents.empty() ? tl.shotEvents[0] : tl.shotStart + cast, -1, -1);
        tl.hits.back().estimated = true;
        Note(tl, 1, -1, L"Vuruş satırı (DMG Event TRUE) yok ama skill hasar veriyor: önizleme vuruşu saldırı olayında TAHMİNİ gösteriyor [U].",
             L"No hit row (DMG Event TRUE) although the skill deals damage: the preview shows the hit at the attack event as an ESTIMATE [U].");
    }

    // ------------------------------------------------------------------------------------------ effectsound
    const std::wstring sndObj = cci ? cci->soundObject : std::wstring();
    if (opt.effectSounds && !sndObj.empty()) {
        const auto rows = db.SoundsOf(sndObj, key);
        std::map<std::wstring, std::vector<const SoundRow*>> byHandle;
        for (const SoundRow* s : rows) byHandle[Up(s->handle)].push_back(s);
        // the clips' own sound cues (.bsr mod palette, ModDataSound): handle + ms inside READY / WAIT / SHOT; a WAIT cue
        // repeats with the loop; the effectsound row of the handle plays (its variants stay random)
        struct TimedCue {
            std::wstring handle;
            double t;
        };
        std::vector<TimedCue> timed;
        if (caster)
            for (const auto& a : tl.anims) {
                if (a.role < 0 || !a.found) continue;
                if (a.phase != Phase::Ready && a.phase != Phase::Wait && a.phase != Phase::Shot) continue;
                for (const auto& cue : caster->SoundCues(a.role)) {
                    const double len = a.clipMs > 0 ? a.clipMs : 0;
                    double t = a.start + cue.ms;
                    if (t > a.end + 0.5) continue;  // the phase ends before the cue (the clip is cut)
                    do {
                        timed.push_back({Up(cue.handle), t});
                        t += len;
                    } while (a.loop && len > 0 && t <= a.end + 0.5);
                }
            }
        int cueSounds = 0, ruleSounds = 0;
        for (const auto& [h, vars] : byHandle) {
            bool done = false;
            for (const auto& tc : timed)
                if (tc.handle == h) {
                    const SoundRow* v = vars[rng.Pick(vars.size())];
                    addSound(tc.t, v->DataPath(), v->handle + L" (.bsr)", 0, -1, false, v->volume);
                    done = true;
                    ++cueSounds;
                }
            if (done) continue;
            const SoundRow* s = vars[rng.Pick(vars.size())];
            if (h != L"SND_DMG" && h != L"SND_CRIDMG") ++ruleSounds;
            if (h == L"SND_DMG" || h == L"SND_CRIDMG") {
                if ((h == L"SND_CRIDMG") != (opt.hitKind == kCri)) continue;
                for (const auto& hit : tl.hits) addSound(hit.t, vars[rng.Pick(vars.size())]->DataPath(), s->handle, 1, hit.fx, true, s->volume);
                continue;
            }
            double t = tl.shotStart;
            if (StartsWith(h, L"SND_SWING")) {
                // SND_SWINGk / SND_SWING_Sk -> the k-th event of the SHOT clip
                int k = 1;
                if (!h.empty() && iswdigit(h.back())) k = h.back() - L'0';
                if (!tl.shotEvents.empty()) t = tl.shotEvents[static_cast<size_t>(std::min<int>(k, static_cast<int>(tl.shotEvents.size())) - 1)];
                else t = tl.shotStart + cast * 0.8;
            }
            addSound(t, s->DataPath(), s->handle, 0, -1, true, s->volume);
        }
        if (cueSounds)
            Note(tl, 2, -1, Fmt(L"Ses zamanları modelin kendi ses izlerinden (.bsr): %d ses.", cueSounds),
                 Fmt(L"Sound times from the model's own sound tracks (.bsr): %d sounds.", cueSounds));
        if (ruleSounds)
            Note(tl, 2, -1, Fmt(L"%d sesin modelde izi yok: zamanı tahmini (bağırış klip başında, savurma k. olayda) [U].", ruleSounds),
                 Fmt(L"%d sounds have no track in the model: estimated time (shout at the clip start, swing at event k) [U].", ruleSounds));
    }
    // target moan on each hit
    if (opt.effectSounds && tci && !tci->soundObject.empty() && !tl.hits.empty()) {
        std::vector<const SoundRow*> moan;
        for (const SoundRow* s : db.Sounds(tci->soundObject, L"VOC_MOAN", L""))
            if (Up(s->event1) == (opt.hitKind == kCri ? L"CRITYCAL" : L"NORMAL")) moan.push_back(s);
        if (!moan.empty())
            for (const auto& hit : tl.hits) addSound(hit.t, moan[rng.Pick(moan.size())]->DataPath(), L"VOC_MOAN", 1, hit.fx, true, moan[0]->volume);
    }

    // data checks that matter for new / pending rows (SRO_SKILLS §5.4-5.5)
    if (skill.valid) {
        if (skill.hits > 0 && static_cast<int>(tl.hits.size()) != skill.hits)
            Note(tl, 2, -1, Fmt(L"Param mc: %d vuruş, skilleffectset'te %zu DMG Event TRUE satırı (çoğunlukla vuruş başına bir satır).", skill.hits, tl.hits.size()),
                 Fmt(L"Param mc: %d hits, %zu DMG Event TRUE rows in skilleffectset (usually one row per hit).", skill.hits, tl.hits.size()));
        if (!tl.hits.empty() && cast > 0 && tl.hits[0].fx < 0 && std::fabs(tl.hits[0].t - tl.shotStart - cast) > 5)
            Note(tl, 2, tl.hits[0].row,
                 Fmt(L"İlk vuruş klipte %.0f ms, CastingTime %.0f ms (sunucu hasarı CastingTime'da uygular).", tl.hits[0].t - tl.shotStart, cast),
                 Fmt(L"First hit at %.0f ms in the clip, CastingTime %.0f ms (the server applies the damage at CastingTime).", tl.hits[0].t - tl.shotStart, cast));
        if (!skill.chain.empty() && skill.chain != L"0")
            Note(tl, 2, -1, L"Zincir skill: adımlar bu tek klibi paylaşır; satırlar baş anahtarda.",
                 L"Chain skill: the steps share this one clip; the rows live on the head key.");
        if (!skill.paramError.empty()) Note(tl, 0, -1, L"Param akışı: " + skill.paramError, L"Param stream: " + skill.paramError);
    }
    if (sa && sa->trailLen > 0)
        Note(tl, 2, -1, L"Silah izi (mirage) yalnız silahlı oyuncularda: listelenir.", L"Weapon trail (mirage) is for armed players: listed only.");
    if (sa && !sa->defenseEfp.empty())
        Note(tl, 2, -1, L"DefenseEfp yalnız blok olunca oynar: " + sa->defenseEfp, L"DefenseEfp plays only on a block: " + sa->defenseEfp);
    for (size_t i = 0; i < tl.rows.size(); ++i)
        if (!tl.rowUsed[i] && tl.rows[i]->kind == ObjKind::None && tl.rows[i]->dmg != 1 && tl.rows[i]->script == 0 && tl.rows[i]->sndBegin.empty())
            Note(tl, 2, static_cast<int>(i), L"Satır görünür bir şey üretmez (nesne / ses / vuruş yok).",
                 L"The row produces nothing visible (no object / sound / hit).");

    // ------------------------------------------------------------------------------------------ totals
    double end = std::max(tl.shotEnd, tl.prepMs);
    for (const auto& a : tl.anims) end = std::max(end, a.end);
    for (const auto& e : tl.fx) end = std::max({end, e.t + e.flightMs + 300, e.holdUntil});
    for (const auto& h : tl.hits) end = std::max(end, h.t + 300);
    for (const auto& s : tl.sounds) end = std::max(end, s.t);
    if (tl.buffEnd > 0) end = std::max(end, tl.buffEnd + 300);
    tl.totalMs = end + opt.idleTailMs;
    if (tl.anims.empty() || tl.anims.back().end < tl.totalMs) {
        AnimSeg s;
        s.phase = Phase::Shot;
        s.token = L"(stand)";
        s.start = tl.anims.empty() ? 0 : tl.anims.back().end;
        s.end = tl.totalMs;
        s.loop = true;
        tl.anims.push_back(s);
    }
    std::stable_sort(tl.sounds.begin(), tl.sounds.end(), [](const SoundEvent& a, const SoundEvent& b) { return a.t < b.t; });
    tl.ok = !tl.fx.empty() || !tl.hits.empty() || tl.shotRole >= 0 || !tl.sounds.empty();
    return tl;
}

std::wstring Timeline::Dump() const {
    std::wstring o;
    o += Fmt(L"KEY %ls  prep=%.0f shot=%ls [%.0f..%.0f] total=%.0f rows=%zu fx=%zu hits=%zu snd=%zu cam=%zu%ls\n", key.c_str(), prepMs,
             AniTokenOf(shotRole).c_str(), shotStart, shotEnd, totalMs, rows.size(), fx.size(), hits.size(), sounds.size(), cams.size(),
             pendingRows ? L" PENDING" : L"");
    if (!shotEvents.empty()) {
        o += L"events";
        for (double e : shotEvents) o += Fmt(L" %.0f", e);
        o += L"\n";
    }
    for (const auto& a : anims)
        o += Fmt(L"anim %-6ls %-14ls %7.0f..%-7.0f%ls%ls\n", PhaseName(a.phase), a.token.c_str(), a.start, a.end, a.loop ? L" loop" : L"",
                 a.role >= 0 && !a.found ? L" MISSING" : L"");
    for (size_t i = 0; i < fx.size(); ++i) {
        const FxEvent& e = fx[i];
        const EffectRow* r = e.row >= 0 && static_cast<size_t>(e.row) < rows.size() ? rows[static_cast<size_t>(e.row)] : nullptr;
        o += Fmt(L"fx%-3zu t=%7.0f %-6ls L%-5d %-13ls a%d %ls %ls", i, e.t, r ? PhaseName(r->phase) : L"?", r ? r->line : 0,
                 PlaceName(e.place), e.actor, e.bone.empty() ? L"-" : e.bone.c_str(), e.res.c_str());
        if (e.place == Place::Projectile)
            o += Fmt(L" fly a%d>a%d v=%.0f %.0fms%ls", e.fromActor, e.toActor, e.speed, e.flightMs, e.homing ? L" homing" : L"");
        if (e.holdUntil >= 0) o += Fmt(L" hold<%.0f", e.holdUntil);
        if (e.releaseAt >= 0) o += Fmt(L" kill@%.0f", e.releaseAt);
        if (e.instId) o += Fmt(L" id%d", e.instId);
        if (e.scale != 1) o += Fmt(L" s=%.2f", e.scale);
        if (e.copy) o += Fmt(L" copy%d", e.copy);
        if (!e.arrive.empty()) o += L" ->" + e.arrive;
        if (e.dmg) o += L" DMG";
        if (!e.note.empty()) o += L" (" + e.note + L")";
        o += L"\n";
    }
    for (const auto& h : hits)
        o += Fmt(L"hit%-2d t=%7.0f row%d%ls %ls %ls%ls\n", h.index, h.t, h.row, h.fx >= 0 ? Fmt(L" @fx%d", h.fx).c_str() : L"",
                 h.damageEfp.c_str(), h.bloodEfp.c_str(), h.estimated ? L" (estimated)" : L"");
    for (const auto& s : sounds)
        o += Fmt(L"snd   t=%7.0f a%d %ls (%ls%ls)\n", s.t, s.actor, s.path.c_str(), s.why.c_str(), s.approx ? L" ~" : L"");
    for (const auto& c : cams) o += Fmt(L"cam   t=%7.0f shake%d%ls\n", c.t, c.level, c.moving ? L" mov" : L"");
    for (const auto& m : mats) o += Fmt(L"mat   t=%7.0f a%d %.0fms\n", m.t, m.actor, m.ms);
    for (const auto& n : notes) o += Fmt(L"note%c row%d %ls\n", n.sev == 0 ? L'E' : n.sev == 1 ? L'W' : L'I', n.row, n.en.c_str());
    return o;
}

}  // namespace se::sp
