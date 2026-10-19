// SkillEffect Studio - skillplay/Player.cpp
// Runtime driver: plays a Timeline through IPlayHost (any renderer). Every frame: start clips, create effects at
// their time, keep bone-attached effects on their bone (SetWorldMatrix, EFP_RUNTIME §10: with a bone = position only,
// without a bone = the actor's rotation), fly projectiles (local -Z along the flight, homing to the moving target,
// sine arc), spawn ObjName2 + hits + SndEnd + SHAKECAM_MOV at the arrival, SetHold(1) loops until their phase ends,
// fade in/out, release what is left after the skill. RecordingHost logs the calls (tests / CLI / GUI log).
#include "SkillPlay.h"

#include <algorithm>
#include <cmath>

namespace se::sp {

namespace {
constexpr double kPi = 3.14159265358979323846;
enum : char { kPending = 0, kLive = 1, kFailed = 2, kDone = 3 };
}  // namespace

void SkillPlayer::Start(const Timeline& tl, IPlayHost& host) {
    Stop();
    m_tl = &tl;
    m_host = &host;
    m_now = 0;
    m_fxDone.assign(tl.fx.size(), kPending);
    m_hitDone.assign(tl.hits.size(), 0);
    m_sndDone.assign(tl.sounds.size(), 0);
    m_camDone.assign(tl.cams.size(), 0);
    m_matDone.assign(tl.mats.size(), 0);
    m_animDone.assign(tl.anims.size(), 0);
    m_live.clear();
}

void SkillPlayer::Stop() {
    if (m_host)
        for (auto& l : m_live)
            if (l.id) m_host->Release(l.id);
    m_live.clear();
    m_host = nullptr;
}

int SkillPlayer::LiveEffects() const {
    int n = 0;
    for (const auto& l : m_live)
        if (l.id) ++n;
    return n;
}

Mat4 SkillPlayer::Anchor(int actor, const std::wstring& bone, const Vec3& off, bool rotWithBone) const {
    const Mat4 aw = m_host->ActorWorld(actor);
    Mat4 bw;
    const bool hb = !bone.empty() && bone != L"*" && m_host->BoneWorld(actor, bone, bw);
    Mat4 r = hb ? (rotWithBone ? bw.RotOnly() : Mat4{}) : aw.RotOnly();
    r.SetT((hb ? bw.T() : aw.T()) + XfDir(off, aw));
    return r;
}

Vec3 SkillPlayer::ProjectilePos(const Live& l, const FxEvent& e, double u) const {
    const Vec3 d = l.to - l.from;
    Vec3 p = l.from + d * (u * (1 + e.overshoot));
    if (e.arc > 0) {
        const Vec3 dir = d.Norm();
        Vec3 up{0, 1, 0};
        if (e.arcRollDeg != 0 && !dir.Zero()) {  // rotate "up" around the flight axis (Rodrigues)
            const double a = e.arcRollDeg * kPi / 180.0, c = std::cos(a), s = std::sin(a);
            up = up * c + dir.Cross(up) * s + dir * (dir.Dot(up) * (1 - c));
        }
        p = p + up * (std::sin(kPi * std::clamp(u, 0.0, 1.0)) * e.arc * d.Len());
    }
    return p;
}

void SkillPlayer::SpawnEvent(int idx) {
    const FxEvent& e = m_tl->fx[static_cast<size_t>(idx)];
    FxSpawn s;
    s.kind = e.kind;
    s.res = e.res;
    s.scale = e.scale;
    s.hold = e.holdUntil > e.t;
    s.row = e.row;
    const FxId id = m_host->Spawn(s);
    if (!id) {
        m_fxDone[static_cast<size_t>(idx)] = kFailed;
        return;
    }
    m_fxDone[static_cast<size_t>(idx)] = kLive;
    Live l;
    l.ev = idx;
    l.id = id;
    l.born = l.freeAt = m_now;
    if (s.hold) {
        m_host->SetHold(id, true);
        l.holding = true;
    }
    switch (e.place) {
        case Place::Follow: m_host->SetWorld(id, Anchor(e.actor, e.bone, e.offset, false)); break;
        case Place::TargetFollow: m_host->SetWorld(id, Anchor(1, e.bone, e.offset, false)); break;
        case Place::Fixed:
        case Place::Target:
        case Place::DmgPos:
            l.fixed = Anchor(e.actor, e.place == Place::DmgPos ? std::wstring() : e.bone, e.offset, false);
            l.hasFixed = true;
            m_host->SetWorld(id, l.fixed);
            break;
        case Place::Projectile: {
            if (e.fromTargetSpace) {
                l.from = Anchor(1, std::wstring(), e.fromOff, false).T();
                l.to = Anchor(1, std::wstring(), e.toOff, false).T();
            } else {
                l.from = Anchor(e.fromActor, e.fromBone, e.fromOff, false).T();
                Vec3 toOff = e.toOff;
                if (e.toActor == 1 && e.toBone.empty() && m_tl && !m_tl->hits.empty()) toOff = toOff + Vec3{0, m_tl->hits[0].damagePos.y, 0};
                else if (e.toActor == 1 && e.toBone.empty()) toOff = toOff + Vec3{0, 10, 0};
                l.to = Anchor(e.toActor, e.toBone, toOff, false).T();
            }
            const double sp = e.speed > 1 ? e.speed : 200;
            l.flight = std::max(50.0, (l.to - l.from).Len() * (1 + e.overshoot) / sp * 1000.0);
            Mat4 m = Mat4::LookAlongNegZ(l.to - l.from);
            m.SetT(l.from);
            m_host->SetWorld(id, m);
            break;
        }
    }
    if (e.fadeIn > 0) m_host->SetAlpha(id, 0);
    m_live.push_back(l);
}

void SkillPlayer::Arrive(Live& l) {
    l.arrived = true;
    const int ev = l.ev;  // `l` is invalid once m_live grows below
    const FxEvent& e = m_tl->fx[static_cast<size_t>(l.ev)];
    const Vec3 at = ProjectilePos(l, e, 1.0);
    if (!e.arrive.empty()) {
        FxSpawn s;
        s.kind = ObjKind::Efp;
        s.res = e.arrive;
        s.scale = e.scale;
        s.row = e.row;
        if (const FxId id = m_host->Spawn(s)) {
            Live a;
            a.id = id;
            a.born = a.freeAt = m_now;
            a.fixed = m_host->ActorWorld(e.toActor).RotOnly();
            a.fixed.SetT(at);
            a.hasFixed = true;
            m_host->SetWorld(id, a.fixed);
            m_live.push_back(a);  // may reallocate: `l` must not be used after this
        }
    }
    if (releaseProjectileOnArrival) {
        for (auto& x : m_live)
            if (x.ev == ev && x.id) {
                m_host->Release(x.id);
                x.id = 0;
            }
    } else {
        for (auto& x : m_live)
            if (x.ev == ev && x.id && x.holding) {
                m_host->SetHold(x.id, false);
                x.holding = false;
            }
    }
    m_fxDone[static_cast<size_t>(ev)] = kDone;
}

bool SkillPlayer::Update(double now) {
    if (!m_host || !m_tl) return false;
    m_now = now;
    const Timeline& tl = *m_tl;
    for (size_t i = 0; i < tl.anims.size(); ++i)
        if (!m_animDone[i] && now >= tl.anims[i].start) {
            m_animDone[i] = 1;
            const AnimSeg& a = tl.anims[i];
            if (i + 1 < tl.anims.size() && now >= tl.anims[i + 1].start) continue;  // skipped past it
            m_host->PlayClipIn(0, a.found ? a.role : -1, a.group, a.loop, now - a.start);
        }
    for (size_t i = 0; i < tl.fx.size(); ++i)
        if (m_fxDone[i] == kPending && now >= tl.fx[i].t) SpawnEvent(static_cast<int>(i));

    auto doHit = [&](size_t hi) {
        if (m_hitDone[hi]) return;
        m_hitDone[hi] = 1;
        const HitEvent& h = tl.hits[hi];
        for (const std::wstring* res : {&h.damageEfp, &h.bloodEfp}) {
            if (res->empty()) continue;
            FxSpawn s;
            s.res = *res;
            s.row = h.row;
            if (const FxId id = m_host->Spawn(s)) {
                Live l;
                l.id = id;
                l.born = l.freeAt = now;
                l.fixed = Anchor(1, std::wstring(), h.damagePos, false);
                l.hasFixed = true;
                m_host->SetWorld(id, l.fixed);
                m_live.push_back(l);
            }
        }
        m_host->Damage(1, h.index, h.kinds);
    };
    auto byFx = [&](int ev, bool arrivedNow) {  // events tied to a projectile: at its arrival, or at t if it never flew
        for (size_t i = 0; i < tl.hits.size(); ++i)
            if (tl.hits[i].fx == ev && (arrivedNow || now >= tl.hits[i].t)) doHit(i);
        for (size_t i = 0; i < tl.sounds.size(); ++i)
            if (!m_sndDone[i] && tl.sounds[i].fx == ev && (arrivedNow || now >= tl.sounds[i].t)) {
                m_sndDone[i] = 1;
                m_host->Sound(tl.sounds[i].path, tl.sounds[i].volume, tl.sounds[i].actor);
            }
        for (size_t i = 0; i < tl.cams.size(); ++i)
            if (!m_camDone[i] && tl.cams[i].fx == ev && (arrivedNow || now >= tl.cams[i].t)) {
                m_camDone[i] = 1;
                m_host->Shake(tl.cams[i].level, tl.cams[i].moving);
            }
    };

    // live effects (index loop: Arrive / hits may append)
    for (size_t li = 0; li < m_live.size(); ++li) {
        if (!m_live[li].id) continue;
        if (!m_host->Alive(m_live[li].id)) {
            Live& l = m_live[li];
            if (l.ev >= 0 && m_tl->fx[static_cast<size_t>(l.ev)].place == Place::Projectile && !l.arrived) {
                const int ev = l.ev;
                l.id = 0;
                m_fxDone[static_cast<size_t>(ev)] = kDone;
                byFx(ev, true);  // the effect died in flight: its hit still happens
                continue;
            }
            l.id = 0;
            continue;
        }
        if (m_live[li].ev < 0) {  // hit / arrival effects: natural end, capped
            if (now - m_live[li].freeAt > releaseCapMs) {
                m_host->Release(m_live[li].id);
                m_live[li].id = 0;
            }
            continue;
        }
        Live& l = m_live[li];
        const FxEvent& e = tl.fx[static_cast<size_t>(l.ev)];
        if (l.holding && e.holdUntil >= 0 && now >= e.holdUntil) {
            m_host->SetHold(l.id, false);
            l.holding = false;
            l.freeAt = now;
        }
        const double age = now - l.born;
        double alpha = 1;
        if (e.fadeIn > 0) alpha = std::min(alpha, age / e.fadeIn);
        if (e.fadeOut > 0 && e.holdUntil >= 0 && now > e.holdUntil) alpha = std::min(alpha, 1 - (now - e.holdUntil) / e.fadeOut);
        if (e.fadeIn > 0 || e.fadeOut > 0) m_host->SetAlpha(l.id, std::clamp(alpha, 0.0, 1.0));
        switch (e.place) {
            case Place::Follow: m_host->SetWorld(l.id, Anchor(e.actor, e.bone, e.offset, false)); break;
            case Place::TargetFollow: m_host->SetWorld(l.id, Anchor(1, e.bone, e.offset, false)); break;
            case Place::Projectile: {
                if (l.arrived) break;
                if (e.homing) {
                    Vec3 toOff = e.toOff;
                    if (e.toActor == 1 && e.toBone.empty()) toOff = toOff + Vec3{0, tl.hits.empty() ? 10.0 : tl.hits[0].damagePos.y, 0};
                    l.to = Anchor(e.toActor, e.toBone, toOff, false).T();
                }
                const double u = l.flight > 0 ? age / l.flight : 1;
                if (u >= 1) {
                    const int ev = l.ev;
                    Arrive(l);  // may append to m_live
                    byFx(ev, true);
                    break;
                }
                const Vec3 p = ProjectilePos(l, e, u), q = ProjectilePos(l, e, std::min(1.0, u + 0.02));
                Mat4 m = Mat4::LookAlongNegZ(q - p);
                m.SetT(p);
                m_host->SetWorld(l.id, m);
                break;
            }
            default: break;  // fixed placements were set at creation
        }
        Live& l2 = m_live[li];
        if (l2.id && !l2.holding && e.releaseAt >= 0 && now >= e.releaseAt) {
            m_host->Release(l2.id);
            l2.id = 0;
        } else if (l2.id && !l2.holding && e.place != Place::Projectile && now - l2.freeAt > releaseCapMs) {
            m_host->Release(l2.id);
            l2.id = 0;
        }
    }

    for (size_t i = 0; i < tl.hits.size(); ++i) {
        const HitEvent& h = tl.hits[i];
        if (m_hitDone[i] || now < h.t) continue;
        if (h.fx < 0 || m_fxDone[static_cast<size_t>(h.fx)] == kFailed || now > h.t + 3000) doHit(i);
    }
    for (size_t i = 0; i < tl.sounds.size(); ++i) {
        const SoundEvent& s = tl.sounds[i];
        if (m_sndDone[i] || now < s.t) continue;
        if (s.fx < 0 || m_fxDone[static_cast<size_t>(s.fx)] == kFailed || now > s.t + 3000) {
            m_sndDone[i] = 1;
            m_host->Sound(s.path, s.volume, s.actor);
        }
    }
    for (size_t i = 0; i < tl.cams.size(); ++i) {
        const CamEvent& c = tl.cams[i];
        if (m_camDone[i] || now < c.t) continue;
        if (c.fx < 0 || m_fxDone[static_cast<size_t>(c.fx)] == kFailed || now > c.t + 3000) {
            m_camDone[i] = 1;
            m_host->Shake(c.level, c.moving);
        }
    }
    for (size_t i = 0; i < tl.mats.size(); ++i)
        if (!m_matDone[i] && now >= tl.mats[i].t) {
            m_matDone[i] = 1;
            m_host->Tint(tl.mats[i].actor, tl.mats[i].rgb, tl.mats[i].ms);
        }
    // compact
    m_live.erase(std::remove_if(m_live.begin(), m_live.end(), [](const Live& l) { return l.id == 0; }), m_live.end());
    if (now < tl.totalMs) return true;
    for (char d : m_fxDone)
        if (d == kPending) return true;
    return !m_live.empty();
}

// ------------------------------------------------------------------------------------------- RecordingHost
void RecordingHost::PlayClip(int actor, int role, bool loop, double startMs) {
    log.push_back(Fmt(L"%7.0f clip a%d %ls%ls%ls", now, actor, role < 0 ? L"(stand)" : AniTokenOf(role).c_str(), loop ? L" loop" : L"",
                      startMs > 1 ? Fmt(L" +%.0f", startMs).c_str() : L""));
}
Mat4 RecordingHost::ActorWorld(int actor) {
    if (actor == 0) return {};
    Mat4 m = Mat4::RotY(kPi);
    m.SetT({0, 0, -distance});
    return m;
}
bool RecordingHost::BoneWorld(int actor, const std::wstring& bone, Mat4& out) {
    if (missingBones.count(ToUpper(bone))) return false;
    out = ActorWorld(actor);
    out.SetT(XfPoint({0, 10, 0}, ActorWorld(actor)));
    return true;
}
FxId RecordingHost::Spawn(const FxSpawn& s) {
    const FxId id = m_next++;
    Fx f;
    f.s = s;
    f.born = now;
    fx[id] = f;
    ++spawns;
    log.push_back(Fmt(L"%7.0f spawn #%llu %ls %ls s=%.2f row%d", now, static_cast<unsigned long long>(id),
                      s.kind == ObjKind::Bsr ? L"bsr" : L"efp", s.res.c_str(), s.scale, s.row));
    return id;
}
void RecordingHost::SetWorld(FxId id, const Mat4& m) {
    auto it = fx.find(id);
    if (it == fx.end()) return;
    it->second.world = m;
    ++it->second.moves;
}
void RecordingHost::SetHold(FxId id, bool hold) {
    auto it = fx.find(id);
    if (it == fx.end()) return;
    if (!hold && it->second.hold) it->second.born = now;  // the loop ends: natural life from here
    it->second.hold = hold;
    log.push_back(Fmt(L"%7.0f hold #%llu %d", now, static_cast<unsigned long long>(id), hold ? 1 : 0));
}
bool RecordingHost::Alive(FxId id) {
    auto it = fx.find(id);
    if (it == fx.end() || it->second.released) return false;
    return it->second.hold || now - it->second.born < lifeMs;
}
void RecordingHost::Release(FxId id) {
    auto it = fx.find(id);
    if (it == fx.end() || it->second.released) return;
    it->second.released = true;
    ++releases;
    const Vec3 p = it->second.world.T();
    log.push_back(Fmt(L"%7.0f release #%llu at %.0f,%.0f,%.0f", now, static_cast<unsigned long long>(id), p.x, p.y, p.z));
}
void RecordingHost::Sound(const std::wstring& dataPath, int volume, int actor) {
    ++sounds;
    log.push_back(Fmt(L"%7.0f sound a%d %ls v%d", now, actor, dataPath.c_str(), volume));
}
void RecordingHost::Shake(int level, bool moving) {
    ++shakes;
    log.push_back(Fmt(L"%7.0f shake %d%ls", now, level, moving ? L" mov" : L""));
}
void RecordingHost::Tint(int actor, const double rgb[6], double ms) {
    log.push_back(Fmt(L"%7.0f tint a%d %.0f,%.0f,%.0f %.0fms", now, actor, rgb[0], rgb[1], rgb[2], ms));
}
void RecordingHost::Damage(int actor, int hitIndex, int kinds) {
    ++damages;
    log.push_back(Fmt(L"%7.0f damage a%d #%d k%d", now, actor, hitIndex, kinds));
}

}  // namespace se::sp
