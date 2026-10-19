// SkillEffect Studio - render/EfxDraw.cpp
#include "EfxDraw.h"

#include <cmath>
#include <unordered_map>

namespace se::render {

namespace {

struct M3 { double m[3][3]; };
M3 Crop(const M4& a) { M3 r; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][j]; return r; }
M3 Mul3(const M3& a, const M3& b) {
    M3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}
V3 Xf33(const V3& v, const M3& M) {
    return {v.x * M.m[0][0] + v.y * M.m[1][0] + v.z * M.m[2][0], v.x * M.m[0][1] + v.y * M.m[1][1] + v.z * M.m[2][1],
            v.x * M.m[0][2] + v.y * M.m[1][2] + v.z * M.m[2][2]};
}

M3 Orient(const EfxNode& nd, const EfxParticle& p, const RCamera& cam) {
    M3 sr = Crop(p.srot);
    M3 inv;
    inv.m[0][0] = cam.x.x; inv.m[0][1] = cam.x.y; inv.m[0][2] = cam.x.z;
    inv.m[1][0] = cam.y.x; inv.m[1][1] = cam.y.y; inv.m[1][2] = cam.y.z;
    inv.m[2][0] = cam.z.x; inv.m[2][1] = cam.z.y; inv.m[2][2] = cam.z.z;
    switch (nd.view) {
        case EfxView::Billboard: return Mul3(sr, inv);
        case EfxView::YBillboard: { M3 B = inv; B.m[1][0] = 0; B.m[1][1] = 1; B.m[1][2] = 0; return Mul3(sr, B); }
        case EfxView::VBillboard: {
            double vl = Len(p.vel);
            V3 z = Cross(p.vel, V3{0, 1, 0});
            double zl = Len(z);
            if (vl < 1e-9 || zl < 1e-9) return sr;
            V3 y = p.vel / vl;
            z = z / zl;
            V3 x = Cross(y, z);
            M3 B;
            B.m[0][0] = x.x; B.m[0][1] = x.y; B.m[0][2] = x.z;
            B.m[1][0] = y.x; B.m[1][1] = y.y; B.m[1][2] = y.z;
            B.m[2][0] = z.x; B.m[2][1] = z.y; B.m[2][2] = z.z;
            return Mul3(sr, B);
        }
        default: return Crop(Mul(p.srot, p.rot));
    }
}

struct NodeRes { std::shared_ptr<RTexture> tex; std::shared_ptr<const BmsFile> mesh; bool loaded = false; };

void Col(const EfxParticle& q, double c[4]) {
    c[0] = q.diffuse[2] / 255.0; c[1] = q.diffuse[1] / 255.0; c[2] = q.diffuse[0] / 255.0; c[3] = q.diffuse[3] / 255.0;
}

void DrawLink(const EfxNode& nd, const EfxGroup& g, const RCamera& cam, const std::shared_ptr<RTexture>& tex, double s,
              RFrame& frame, const RState& st0) {
    std::vector<const EfxParticle*> P;
    for (const EfxParticle* q : g.elems) {
        if (P.empty()) { P.push_back(q); continue; }
        V3 d = q->pos - P.back()->pos;
        if (d.x * d.x + d.y * d.y + d.z * d.z > 1e-12) P.push_back(q);
    }
    if (P.size() < 2) return;
    struct Pt { V3 p; double w; double c[4]; };
    std::vector<Pt> pts;
    for (const EfxParticle* q : P) { Pt t; t.p = q->pos * s; t.w = q->scale.x * s; Col(*q, t.c); pts.push_back(t); }
    if (nd.render != EfxRender::LinkDPipe && pts.size() >= 3) {
        std::vector<Pt> ext;
        ext.push_back(pts.front());
        ext.insert(ext.end(), pts.begin(), pts.end());
        ext.push_back(pts.back());
        std::vector<Pt> sp;
        for (std::size_t i = 0; i + 3 < ext.size(); ++i)
            for (double t : {0.0, 1.0 / 3.0, 2.0 / 3.0}) {
                double b[4] = {std::pow(1 - t, 3), 3 * std::pow(t, 3) - 6 * std::pow(t, 2) + 4,
                               -3 * std::pow(t, 3) + 3 * std::pow(t, 2) + 3 * t + 1, std::pow(t, 3)};
                for (double& x : b) x /= 6;
                Pt o;
                o.p = ext[i].p * b[0] + ext[i + 1].p * b[1] + ext[i + 2].p * b[2] + ext[i + 3].p * b[3];
                o.w = ext[i].w * b[0] + ext[i + 1].w * b[1] + ext[i + 2].w * b[2] + ext[i + 3].w * b[3];
                for (int k = 0; k < 4; ++k) o.c[k] = ext[i].c[k] * b[0] + ext[i + 1].c[k] * b[1] + ext[i + 2].c[k] * b[2] + ext[i + 3].c[k] * b[3];
                sp.push_back(o);
            }
        sp.push_back(pts.back());
        pts.swap(sp);
    }
    const std::size_t n = pts.size();
    const double u0 = P[0]->uv[0], v0 = P[0]->uv[1], du = P[0]->uv[2], dv = P[0]->uv[3];
    const V3 fwd = cam.z;
    RState st = st0;
    st.cull = 1;   // ribbons two-sided
    for (std::size_t i = 0; i + 1 < n; ++i) {
        V3 d = pts[i + 1].p - pts[i].p;
        V3 side = Cross(d, fwd);
        double ln = Len(side);
        if (ln < 1e-9) continue;
        side = side / ln;
        const V3 a = pts[i].p, b = pts[i + 1].p;
        V3 quad[4] = {a + side * pts[i].w / 2, b + side * pts[i + 1].w / 2, b - side * pts[i + 1].w / 2, a - side * pts[i].w / 2};
        double ua = u0 + du * static_cast<double>(i) / static_cast<double>(n - 1);
        double ub = u0 + du * static_cast<double>(i + 1) / static_cast<double>(n - 1);
        double UV[4][2] = {{ua, v0}, {ub, v0}, {ub, v0 + dv}, {ua, v0 + dv}};
        double c[4];
        for (int k = 0; k < 4; ++k) c[k] = (pts[i].c[k] + pts[i + 1].c[k]) / 2;
        RBatch& bt = frame.Batch(st, tex);
        static const int F[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (const auto& f : F)
            for (int k : f) {
                RVtx vx;
                vx.p = quad[k];
                vx.u = UV[k][0];
                vx.v = UV[k][1];
                for (int j = 0; j < 4; ++j) vx.c[j] = c[j];
                bt.v.push_back(vx);
            }
    }
}

}  // namespace

void DrawEffect(EfpEffect& fx, AssetCache& cache, RFrame& frame, const EfxDrawOptions& opt) {
    const double s = fx.S();
    std::vector<const EfxParticle*> order;
    fx.DrawOrder(order);
    std::unordered_map<const EfxNode*, NodeRes> res;
    const RCamera& cam = frame.cam;
    static const V3 kPlate[4] = {{-.5, .5, 0}, {.5, .5, 0}, {.5, -.5, 0}, {-.5, -.5, 0}};
    static const double kPlateUv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (const EfxParticle* p : order) {
        const EfxNode& nd = *p->node;
        if (nd.render == EfxRender::None || nd.hidden) continue;
        NodeRes& r = res[&nd];
        if (!r.loaded) {
            r.loaded = true;
            if (!nd.tex.empty()) r.tex = cache.Texture(Arc::Particles, Cp949ToW(nd.tex));
            if (nd.render == EfxRender::Mesh && !nd.mesh.empty()) r.mesh = cache.Bms(Arc::Particles, Cp949ToW(nd.mesh));
        }
        RState st;
        st.cull = nd.cull;
        st.src = nd.blend[0];
        st.dst = nd.blend[1];
        for (int i = 0; i < 3; ++i) { st.cs[i] = nd.cstage[i]; st.as[i] = nd.astage[i]; }
        st.ztest = opt.ztest;
        if (nd.render == EfxRender::LinkPipe || nd.render == EfxRender::LinkDPipe || nd.render == EfxRender::LinkObj) {
            if (!p->group->elems.empty() && p->group->elems.front() == p) DrawLink(nd, *p->group, cam, r.tex, s, frame, st);
            continue;
        }
        const BmsFile* mesh = r.mesh.get();
        bool plate = nd.render == EfxRender::Plate || (nd.render == EfxRender::Mesh && !mesh && nd.mesh.empty());
        if (!plate && !mesh) continue;
        const M3 R3 = Orient(nd, *p, cam);
        double col[4];
        Col(*p, col);
        const double u0 = p->uv[0], v0 = p->uv[1], du = p->uv[2], dv = p->uv[3];
        RBatch& bt = frame.Batch(st, r.tex);
        auto emit = [&](const V3& loc, double lu, double lv) {
            RVtx vx;
            vx.p = (Xf33(Hadamard(loc, p->scale), R3) + p->pos) * s;
            vx.u = u0 + lu * du;
            vx.v = v0 + lv * dv;
            for (int j = 0; j < 4; ++j) vx.c[j] = col[j];
            bt.v.push_back(vx);
        };
        if (plate) {
            static const int F[2][3] = {{0, 1, 2}, {0, 2, 3}};
            for (const auto& f : F)
                for (int k : f) emit(kPlate[k], kPlateUv[k][0], kPlateUv[k][1]);
        } else {
            for (std::size_t i = 0; i + 2 < mesh->idx.size(); i += 3)
                for (int k = 0; k < 3; ++k) {
                    const BmsVertex& mv = mesh->verts[mesh->idx[i + k]];
                    emit(V3{mv.pos.x, mv.pos.y, mv.pos.z}, mv.uv.x, mv.uv.y);
                }
        }
    }
}

}  // namespace se::render
