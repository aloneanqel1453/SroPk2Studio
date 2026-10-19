// SkillEffect Studio - render/RModel.cpp
#include "RModel.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <functional>

namespace se::render {

namespace {

bool IEq(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    return true;
}
bool HasI(const std::wstring& s, const wchar_t* sub) { return LowerW(s).find(sub) != std::wstring::npos; }
std::wstring BaseName(const std::wstring& p) {
    std::size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(k + 1);
}
// "C:\...\Data\prim\mtrl\x.ddj" / "data\prim\..." -> "prim\mtrl\x.ddj"
std::wstring StripDataRoot(const std::wstring& p) {
    std::wstring l = LowerW(NormPath(p));
    std::size_t k = l.rfind(L"data\\");
    if (k != std::wstring::npos && (k == 0 || l[k - 1] == L'\\')) return NormPath(p).substr(k + 5);
    std::size_t c = l.find(L":\\");
    if (c != std::wstring::npos) return NormPath(p).substr(c + 2);
    return NormPath(p);
}

}  // namespace

std::vector<double> ModelClip::EventTimes(std::uint32_t type) const {
    std::vector<double> t;
    for (const auto& e : events)
        if (e.type == type) t.push_back(static_cast<double>(e.keyTime));
    std::sort(t.begin(), t.end());
    return t;
}

double ModelDef::Radius() const {
    V3 e = bbMax - bbMin;
    double r = Len(e) * 0.5;
    return r > 0.1 ? r : 10.0;
}

int ModelDef::Clips() const {
    int n = 0;
    for (const auto& b : bsrs)
        if (b) for (const auto& g : b->aniGroups) n += static_cast<int>(g.entries.size());
    return n;
}

std::vector<std::pair<std::wstring, int>> ModelDef::ClipList() const {
    std::vector<std::pair<std::wstring, int>> v;
    for (const auto& b : bsrs)
        if (b) {
            for (const auto& g : b->aniGroups)
                for (const auto& e : g.entries) v.push_back({g.name, static_cast<int>(e.type)});
            if (!b->aniGroups.empty()) break;
        }
    return v;
}

std::shared_ptr<ModelDef> ModelDef::Load(AssetCache& cache, const std::wstring& path0, int materialSet, std::wstring& err) {
    auto def = std::make_shared<ModelDef>();
    def->path = NormPath(path0);
    std::wstring low = LowerW(def->path);
    if (low.size() > 4 && low.substr(low.size() - 4) == L".cpd") {
        std::vector<std::uint8_t> data;
        CpdFile cpd;
        std::string e;
        if (!cache.Read(Arc::Data, def->path, data) || !ParseCpd(data.data(), data.size(), cpd, e)) {
            err = L"cpd okunamadi: " + def->path;
            return nullptr;
        }
        def->name = cpd.name;
        for (const auto& p : cpd.parts) def->bsrPaths.push_back(NormPath(p));
    } else {
        def->bsrPaths.push_back(def->path);
    }
    for (const auto& p : def->bsrPaths) {
        auto b = cache.Bsr(Arc::Data, p);
        if (!b) { def->warnings.push_back(L"bsr yok: " + p); continue; }
        def->bsrs.push_back(b);
    }
    if (def->bsrs.empty()) { err = L"model okunamadi: " + def->path; return nullptr; }
    if (def->name.empty()) def->name = def->bsrs[0]->name;
    // skeleton: first resource that names one (+ put that resource first so its aniGroups are primary)
    for (std::size_t i = 0; i < def->bsrs.size(); ++i)
        if (def->bsrs[i]->hasSkeleton && !def->bsrs[i]->skeleton.empty()) {
            def->skel = cache.Bsk(Arc::Data, def->bsrs[i]->skeleton);
            if (def->skel) {
                if (i) { std::swap(def->bsrs[0], def->bsrs[i]); std::swap(def->bsrPaths[0], def->bsrPaths[i]); }
                break;
            }
            def->warnings.push_back(L"iskelet yok: " + def->bsrs[i]->skeleton);
        }
    // bind pose
    if (def->skel) {
        const auto& bones = def->skel->bones;
        std::size_t nb = bones.size();
        def->bindWorld.assign(nb, M4());
        def->invBind.assign(nb, M4());
        std::vector<char> done(nb, 0);
        std::function<void(int, int)> bind = [&](int i, int depth) {
            if (done[i] || depth > 256) return;
            M4 local = QuatTrans(bones[i].rotParent, bones[i].transParent);
            int par = bones[i].parentIndex;
            if (par >= 0 && par != i) { bind(par, depth + 1); def->bindWorld[i] = Mul(local, def->bindWorld[par]); }
            else def->bindWorld[i] = local;
            done[i] = 1;
        };
        for (std::size_t i = 0; i < nb; ++i) bind(static_cast<int>(i), 0);
        for (std::size_t i = 0; i < nb; ++i) def->invBind[i] = Inverse(def->bindWorld[i]);
    }
    // meshes + materials
    bool first = true;
    for (std::size_t bi = 0; bi < def->bsrs.size(); ++bi) def->ComposeBsr(cache, bi, materialSet, first);
    if (def->parts.empty()) {   // effect-only resources (sunbeams, lamps) are valid: their mod-palette particles
        bool fx = false;
        for (const auto& b : def->bsrs) fx = fx || !b->particles.empty();
        if (!fx) { err = L"modelin mesh'i yok: " + def->path; return nullptr; }
    }
    for (const auto& w : cache.TakeWarnings()) def->warnings.push_back(w);
    return def;
}

bool ModelDef::Append(AssetCache& cache, const std::wstring& bsrPath, int materialSet, std::wstring& err) {
    auto b = cache.Bsr(Arc::Data, bsrPath);
    if (!b) { err = L"bsr yok: " + bsrPath; return false; }
    bsrs.push_back(b);
    bsrPaths.push_back(NormPath(bsrPath));
    std::size_t before = parts.size();
    bool first = parts.empty();
    ComposeBsr(cache, bsrs.size() - 1, materialSet, first);
    for (const auto& w : cache.TakeWarnings()) warnings.push_back(w);
    if (parts.size() == before) { err = L"parca mesh'i yok: " + bsrPath; return false; }
    return true;
}

void ModelDef::ComposeBsr(AssetCache& cache, std::size_t bi, int materialSet, bool& first) {
    ModelDef* def = this;
    {
        const BsrFile& b = *def->bsrs[bi];
        std::wstring bmtPath;
        if (!b.materialSets.empty()) {
            bmtPath = b.materialSets[0].second;
            bool found = false;
            for (const auto& ms : b.materialSets)
                if (static_cast<int>(ms.first) == materialSet) { bmtPath = ms.second; found = true; break; }
            if (!found && materialSet >= 0 && materialSet < static_cast<int>(b.materialSets.size()))
                bmtPath = b.materialSets[static_cast<std::size_t>(materialSet)].second;
        }
        auto bmt = bmtPath.empty() ? nullptr : cache.Bmt(Arc::Data, bmtPath);
        for (const auto& mp : b.meshes) {
            ModelPart part;
            part.meshPath = NormPath(mp);
            part.bsrIndex = static_cast<int>(bi);
            part.mesh = cache.Bms(Arc::Data, part.meshPath);
            if (!part.mesh) { def->warnings.push_back(L"mesh yok: " + part.meshPath); continue; }
            part.materialName = part.mesh->material;
            if (bmt)
                if (const BmtMaterial* m = bmt->Find(part.materialName)) { part.mat = *m; part.hasMat = true; }
            if (part.hasMat && !part.mat.diffuseMap.empty()) {
                std::vector<std::wstring> cands;
                if (part.mat.absolute) cands.push_back(StripDataRoot(part.mat.diffuseMap));
                cands.push_back(JoinPath(DirOf(bmtPath), part.mat.diffuseMap));
                cands.push_back(StripDataRoot(part.mat.diffuseMap));
                cands.push_back(JoinPath(DirOf(part.meshPath), BaseName(part.mat.diffuseMap)));
                for (const auto& c : cands) {
                    std::vector<std::uint8_t> probe;
                    if (cache.Read(Arc::Data, c, probe)) { part.texturePath = c; break; }
                }
                if (!part.texturePath.empty()) part.tex = cache.Texture(Arc::Data, part.texturePath);
                else def->warnings.push_back(L"doku yok: " + part.mat.diffuseMap);
            }
            // material class (sro-bmt-bumpflag-and-blend-gate, sroviewer-armor-material-opaque, effects-summon-timing)
            const std::wstring& tp = part.texturePath.empty() ? part.meshPath : part.texturePath;
            const bool equip = HasI(tp, L"man_item") || HasI(tp, L"woman_item") || HasI(tp, L"\\avatar\\") ||
                               HasI(tp, L"\\etc\\") || HasI(BaseName(tp), L"avatar_");
            if (part.tex && LowerW(BaseName(part.texturePath)).rfind(L"black", 0) == 0) part.blend = PartBlend::Additive;
            else if (part.tex && part.hasMat && (part.mat.flags & 0x200) && part.tex->anyAlpha && !equip) {
                if (part.tex->midAlpha >= 0.02f && part.tex->midAlpha <= 0.60f && part.tex->zeroAlpha >= 0.02f &&
                    !HasI(tp, L"\\cos\\"))
                    part.blend = PartBlend::Blend;
                else if (part.tex->zeroAlpha >= 0.02f) part.blend = PartBlend::Cutout;
            }
            if (part.hasMat && (part.mat.flags & 0x40)) {   // tint: only a chromatic diffuse, normalised to max
                float r = part.mat.diffuse[0], g = part.mat.diffuse[1], bl = part.mat.diffuse[2];
                float mx = std::max({r, g, bl}), mn = std::min({r, g, bl});
                if (mx > 0.05f && mx - mn > 0.08f) { part.tint[0] = r / mx; part.tint[1] = g / mx; part.tint[2] = bl / mx; }
            }
            // bone map (+ Bip0N -> Bip01 retry)
            if (def->skel && part.mesh->Skinned()) {
                part.boneMap.assign(part.mesh->boneNames.size(), -1);
                for (std::size_t i = 0; i < part.mesh->boneNames.size(); ++i) {
                    const std::wstring& bn = part.mesh->boneNames[i];
                    int j = def->skel->Find(bn);
                    if (j < 0 && bn.size() > 4 && bn.compare(0, 4, L"Bip0") == 0 && bn[4] != L'1') {
                        std::wstring rn = bn;
                        rn[4] = L'1';
                        j = def->skel->Find(rn);
                    }
                    part.boneMap[i] = j;
                }
            }
            // rigid attachment (sro-weapon-attach-conventions): weapon / shield .bsr carry their OWN tiny skeleton
            // (Bone01 ...) and name the character attach bone; raw vertices are in the character's axes with the grip at
            // the origin -> world = raw * inv(R_bind(attach)) * W_pose(attach) (rotation-only inverse bind).
            bool anyMapped = false;
            for (int j : part.boneMap) anyMapped = anyMapped || j >= 0;
            if (def->skel && !anyMapped && !b.attachBone.empty()) part.attachBone = def->skel->Find(b.attachBone);
            for (const auto& v : part.mesh->verts) {
                V3 p{v.pos.x, v.pos.y, v.pos.z};
                if (first) { def->bbMin = def->bbMax = p; first = false; }
                def->bbMin = {std::min(def->bbMin.x, p.x), std::min(def->bbMin.y, p.y), std::min(def->bbMin.z, p.z)};
                def->bbMax = {std::max(def->bbMax.x, p.x), std::max(def->bbMax.y, p.y), std::max(def->bbMax.z, p.z)};
            }
            def->parts.push_back(std::move(part));
        }
    }
}

const ModelClip* ModelDef::Clip(AssetCache& cache, int typeId, const std::wstring& group, bool strict) {
    if (typeId < 0) return nullptr;
    for (const auto& c : m_clips)
        if (c->typeId == typeId && (group.empty() ? c->group.empty() : IEq(c->group, group))) return c.get();
    const BsrFile* b = nullptr;
    for (const auto& x : bsrs)
        if (x && !x->aniGroups.empty()) { b = x.get(); break; }
    if (!b) return nullptr;
    const BsrAniGroup* gFound = nullptr;
    const BsrAniEntry* eFound = nullptr;
    auto search = [&](const BsrAniGroup& g) {
        for (const auto& e : g.entries)
            if (static_cast<int>(e.type) == typeId && e.fileIndex < b->animations.size()) { gFound = &g; eFound = &e; return true; }
        return false;
    };
    if (!group.empty()) {
        for (const auto& g : b->aniGroups)
            if (IEq(g.name, group) && search(g)) break;
        if (!eFound && strict) return nullptr;
    }
    if (!eFound)
        for (const auto& g : b->aniGroups)
            if (IEq(g.name, L"DEFAULT") && search(g)) break;
    if (!eFound)
        for (const auto& g : b->aniGroups)
            if (search(g)) break;
    if (!eFound) return nullptr;
    auto c = std::make_unique<ModelClip>();
    c->typeId = typeId;
    c->group = group;
    c->banPath = NormPath(b->animations[eFound->fileIndex]);
    c->ban = cache.Ban(Arc::Data, c->banPath);
    c->events = eFound->events;
    c->lengthMs = c->ban ? c->ban->LengthMs() : 1000.0;
    (void)gFound;
    m_clips.push_back(std::move(c));
    return m_clips.back().get();
}

const ModelClip* ModelDef::ClipByIndex(AssetCache& cache, int idx) {
    const BsrFile* b = bsrs.empty() ? nullptr : bsrs[0].get();
    if (!b || idx < 0 || idx >= static_cast<int>(b->animations.size())) return nullptr;
    std::wstring p = NormPath(b->animations[static_cast<std::size_t>(idx)]);
    for (const auto& c : m_clips)
        if (c->typeId == -1000 - idx) return c.get();
    auto c = std::make_unique<ModelClip>();
    c->typeId = -1000 - idx;
    c->banPath = p;
    c->ban = cache.Ban(Arc::Data, p);
    for (const auto& g : b->aniGroups)
        for (const auto& e : g.entries)
            if (static_cast<int>(e.fileIndex) == idx && c->events.empty()) c->events = e.events;
    c->lengthMs = c->ban ? c->ban->LengthMs() : 1000.0;
    m_clips.push_back(std::move(c));
    return m_clips.back().get();
}

// ------------------------------------------------------------------------------------------------ instance
ModelInstance::ModelInstance(std::shared_ptr<ModelDef> def) : m_def(std::move(def)) {}

M4 ModelInstance::World() const {
    if (m_useMat) return Mul(Scale3(scale), m_mat);
    return Mul(Mul(Scale3(scale), RotY(yaw)), Translate(pos));
}

void ModelInstance::SetTime(double ms) {
    if (!m_clip) return;
    m_t = std::max(0.0, ms);
    if (m_loop && m_clip->lengthMs > 0) m_t = std::fmod(m_t, m_clip->lengthMs);
    else m_t = std::min(m_t, m_clip->lengthMs);
    m_posed = false;
}

bool ModelInstance::Play(AssetCache& cache, int typeId, bool loop, const std::wstring& group, double speed) {
    const ModelClip* c = m_def->Clip(cache, typeId, group);
    if (!c || !c->ban) return false;
    m_clip = c;
    m_t = 0;
    m_loop = loop;
    m_speed = speed;
    m_posed = false;
    MapClip();
    return true;
}

bool ModelInstance::PlayIndex(AssetCache& cache, int idx, bool loop, double speed) {
    const ModelClip* c = m_def->ClipByIndex(cache, idx);
    if (!c || !c->ban) return false;
    m_clip = c;
    m_t = 0;
    m_loop = loop;
    m_speed = speed;
    m_posed = false;
    MapClip();
    return true;
}

void ModelInstance::MapClip() {
    m_banMap.clear();
    if (!m_clip || !m_clip->ban || !m_def->HasSkeleton()) return;
    const auto& bones = m_def->skel->bones;
    m_banMap.assign(bones.size(), -1);
    for (std::size_t i = 0; i < bones.size(); ++i)
        for (std::size_t j = 0; j < m_clip->ban->bones.size(); ++j)
            if (IEq(m_clip->ban->bones[j].name, bones[i].name)) { m_banMap[i] = static_cast<int>(j); break; }
}

bool ModelInstance::Attach(AssetCache& cache, const std::wstring& bsrPath, std::wstring& err, int materialSet) {
    bool ok = m_def->Append(cache, bsrPath, materialSet, err);
    m_posed = false;
    return ok;
}

void ModelInstance::Stop() { m_clip = nullptr; m_t = 0; m_posed = false; m_banMap.clear(); }

void ModelInstance::Advance(double ms) {
    if (!m_clip) return;
    m_t += ms * m_speed;
    double L = m_clip->lengthMs;
    if (m_loop && L > 0) m_t = std::fmod(m_t, L);
    else if (m_t > L) m_t = L;
    m_posed = false;
}

bool ModelInstance::Finished() const { return m_clip && !m_loop && m_t >= m_clip->lengthMs; }

int ModelInstance::FindBone(const std::wstring& bone) const { return m_def->skel ? m_def->skel->Find(bone) : -1; }

M4 ModelInstance::Local(int i) const {
    const BskBone& b = m_def->skel->bones[static_cast<std::size_t>(i)];
    M4 bindLocal = QuatTrans(b.rotParent, b.transParent);
    if (!m_clip || !m_clip->ban || m_clip->ban->keyTimes.empty()) return bindLocal;
    const BanFile& an = *m_clip->ban;
    int bi = i < static_cast<int>(m_banMap.size()) ? m_banMap[static_cast<std::size_t>(i)] : -1;
    if (bi < 0) return bindLocal;
    const BanBone* ab = &an.bones[static_cast<std::size_t>(bi)];
    if (ab->keys.empty()) return bindLocal;
    const auto& kt = an.keyTimes;
    const double t = m_t;
    std::size_t k = 0;
    while (k + 1 < kt.size() && static_cast<double>(kt[k + 1]) <= t) ++k;
    if (k >= ab->keys.size()) k = ab->keys.size() - 1;
    std::size_t k1 = k + 1 < ab->keys.size() ? k + 1 : k;
    double t0 = static_cast<double>(kt[std::min(k, kt.size() - 1)]);
    double t1 = k1 < kt.size() ? static_cast<double>(kt[k1]) : t0;
    double f = t1 > t0 ? (t - t0) / (t1 - t0) : 0.0;
    f = std::clamp(f, 0.0, 1.0);
    Quat q = Slerp(ab->keys[k].q, ab->keys[k1].q, f);
    V3 tr = ab->keys[k].t * (1 - f) + ab->keys[k1].t * f;
    return QuatTrans(q, tr);
}

void ModelInstance::Pose() {
    if (m_posed) return;
    m_posed = true;
    const auto& def = *m_def;
    if (def.HasSkeleton()) {
        const auto& bones = def.skel->bones;
        std::size_t nb = bones.size();
        m_boneWorld.assign(nb, M4());
        m_skin.assign(nb, M4());
        std::vector<char> done(nb, 0);
        std::function<void(int, int)> go = [&](int i, int depth) {
            if (done[i] || depth > 256) return;
            M4 local = Local(i);
            int par = bones[i].parentIndex;
            if (par >= 0 && par != i) { go(par, depth + 1); m_boneWorld[i] = Mul(local, m_boneWorld[par]); }
            else m_boneWorld[i] = local;
            done[i] = 1;
        };
        for (std::size_t i = 0; i < nb; ++i) go(static_cast<int>(i), 0);
        for (std::size_t i = 0; i < nb; ++i) m_skin[i] = Mul(def.invBind[i], m_boneWorld[i]);
    }
    m_pos.resize(def.parts.size());
    m_nrm.resize(def.parts.size());
    for (std::size_t pi = 0; pi < def.parts.size(); ++pi) {
        const ModelPart& part = def.parts[pi];
        const BmsFile& ms = *part.mesh;
        auto& P = m_pos[pi];
        auto& N = m_nrm[pi];
        P.resize(ms.verts.size());
        N.resize(ms.verts.size());
        const bool rigid = def.HasSkeleton() && part.attachBone >= 0 && part.attachBone < static_cast<int>(m_boneWorld.size());
        const bool skinned = !rigid && def.HasSkeleton() && ms.Skinned() && !part.boneMap.empty();
        M4 att;
        if (rigid) att = Mul(RotPart(def.invBind[static_cast<std::size_t>(part.attachBone)]), m_boneWorld[static_cast<std::size_t>(part.attachBone)]);
        for (std::size_t i = 0; i < ms.verts.size(); ++i) {
            const BmsVertex& v = ms.verts[i];
            V3 p{v.pos.x, v.pos.y, v.pos.z}, n{v.nrm.x, v.nrm.y, v.nrm.z};
            if (skinned) {
                const BmsSkin& w = ms.skin[i];
                int s1 = w.b0 < part.boneMap.size() ? part.boneMap[w.b0] : -1;
                int s2 = w.b1 < part.boneMap.size() ? part.boneMap[w.b1] : -1;
                double tot = static_cast<double>(w.w0) + w.w1;
                double w1 = tot > 0 ? w.w0 / tot : 1.0, w2 = tot > 0 ? w.w1 / tot : 0.0;
                if (s1 >= 0 && s2 >= 0 && w2 > 0) {
                    P[i] = Xf(p, m_skin[s1]) * w1 + Xf(p, m_skin[s2]) * w2;
                    N[i] = Norm(Xf3(n, m_skin[s1]) * w1 + Xf3(n, m_skin[s2]) * w2);
                } else if (s1 >= 0 || s2 >= 0) {
                    int s = s1 >= 0 ? s1 : s2;
                    P[i] = Xf(p, m_skin[s]);
                    N[i] = Norm(Xf3(n, m_skin[s]));
                } else { P[i] = p; N[i] = n; }
            } else if (rigid) {
                P[i] = Xf(p, att);
                N[i] = Norm(Xf3(n, att));
            } else { P[i] = p; N[i] = n; }
        }
    }
}

bool ModelInstance::BoneWorld(int bone, M4& out) {
    if (!m_def->HasSkeleton() || bone < 0 || bone >= static_cast<int>(m_def->skel->bones.size())) return false;
    Pose();
    out = Mul(m_boneWorld[static_cast<std::size_t>(bone)], World());
    return true;
}

bool ModelInstance::BoneWorld(const std::wstring& bone, M4& out) { return BoneWorld(FindBone(bone), out); }

void ModelInstance::Bounds(V3& mn, V3& mx) {
    Pose();
    M4 W = World();
    bool first = true;
    for (const auto& P : m_pos)
        for (const V3& p0 : P) {
            V3 p = Xf(p0, W);
            if (first) { mn = mx = p; first = false; continue; }
            mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
            mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
        }
    if (first) mn = mx = pos;
}

void ModelInstance::Draw(RFrame& f, const ModelLook& look, int passSel) {
    if (!visible) return;
    Pose();
    const M4 W = World();
    const V3 kd = Norm(look.keyDir), rd = Norm(look.rimDir);
    for (int pass = 0; pass < 2; ++pass)
        for (std::size_t pi = 0; (passSel < 0 || passSel == pass) && pi < m_def->parts.size(); ++pi) {
            const ModelPart& part = m_def->parts[pi];
            const bool trans = part.blend == PartBlend::Blend || part.blend == PartBlend::Additive;
            if (part.hidden || (pass == 0) == trans) continue;
            RState st;
            st.lit = true;
            st.cull = 1;
            st.ztest = true;
            st.zwrite = !trans;
            switch (part.blend) {
                case PartBlend::Opaque: st.src = 2; st.dst = 1; break;
                case PartBlend::Cutout: st.src = 2; st.dst = 1; st.alphaRef = 0.5f; break;
                case PartBlend::Blend: st.src = 5; st.dst = 6; st.alphaRef = 0.01f; break;
                case PartBlend::Additive: st.src = 2; st.dst = 2; break;
            }
            RBatch& b = f.Batch(st, part.tex);
            const auto& P = m_pos[pi];
            const auto& N = m_nrm[pi];
            const auto& idx = part.mesh->idx;
            const auto& verts = part.mesh->verts;
            const double g = part.tex ? 1.0 : 0.75;
            for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
                for (int k = 0; k < 3; ++k) {
                    std::uint16_t vi = idx[i + k];
                    RVtx v;
                    v.p = Xf(P[vi], W);
                    V3 n = Norm(Xf3(N[vi], W));
                    if (look.twoSided && Dot(n, f.cam.eye - v.p) < 0) n = -n;
                    double l = look.ambient + look.key * std::max(0.0, Dot(n, kd)) + look.rim * std::max(0.0, Dot(n, rd));
                    l = std::min(l, 1.15);
                    for (int c = 0; c < 3; ++c) v.c[c] = std::min(1.0, l * g * part.tint[c] * tint[c]);
                    v.c[3] = 1.0;
                    v.u = verts[vi].uv.x;
                    v.v = verts[vi].uv.y;
                    b.v.push_back(v);
                }
            }
        }
}

}  // namespace se::render
