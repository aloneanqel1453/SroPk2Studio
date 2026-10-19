// SkillEffect Studio - render/Efp.cpp
#include "Efp.h"

#include "RBytes.h"

#include <cstring>

namespace se::render {

namespace {

constexpr int kMaxDepth = 64;
constexpr std::uint32_t kMaxName = 4096;

bool IsNoPayload(const std::string& c) {
    static const char* k[] = {"NeverExtinct", "NormalTimeExtinct", "NormalTimeLoop", "ViewNone", "ViewBillboard",
                              "ViewYBillboard", "ViewVBillboard", "RenderNone", "RenderPlate", "RenderMesh",
                              "RenderLinkPipe", "RenderLinkDPipe", "RenderLinkObj", "ProgramUpdate", "NormalTimeLife",
                              "NormalTimeLoopLife"};
    for (const char* s : k)
        if (c == s) return true;
    return false;
}
int FixedSize(const std::string& c) {
    struct E { const char* n; int s; };
    static const E k[] = {{"StaticEmit", 20},     {"SetRotationMat", 64}, {"SetRVelocityMat", 64}, {"SetRotation", 76},
                          {"SetRVelocity", 76},   {"SetRotationAxis", 80}, {"SetRVelocityAxis", 80}, {"SetShapeRot", 80},
                          {"SetShapeRotVel", 80}, {"SetPosition", 12},    {"SetSpherePos", 12},    {"SetVelocity", 12},
                          {"Force", 12},          {"SetConePos", 24},     {"SetConeVel", 24},      {"ConeForce", 24},
                          {"Attraction", 4},      {"SetGraphRandomScale", 4}};
    for (const auto& e : k)
        if (c == e.n) return e.s;
    return -1;
}
bool Counted(const std::string& c, int& prefix, int& unit) {
    struct E { const char* n; int p, u; };
    static const E k[] = {{"TextureSlide", 12, 16}, {"SetGraphScale", 0, 12}, {"SetGraphDiffuse", 0, 4},
                          {"SetBANRot", 4, 64},     {"SetBANPos", 4, 12}};
    for (const auto& e : k)
        if (c == e.n) { prefix = e.p; unit = e.u; return true; }
    return false;
}

class Parser {
public:
    Parser(const std::uint8_t* d, std::size_t n) : r(d, n), data(d) {}
    RReader r;
    const std::uint8_t* data;
    std::string err;

    bool Fail(const std::string& m) {
        if (err.empty()) err = m + " @0x" + Hex(r.Pos());
        return false;
    }
    static std::string Hex(std::size_t v) {
        char b[32];
        sprintf_s(b, "%zX", v);
        return b;
    }
    bool Count(std::uint32_t unit, std::uint32_t& n) {
        n = r.U32();
        if (r.Fail()) return Fail("eof");
        if (unit && static_cast<std::uint64_t>(n) * unit > r.Left()) return Fail("bad count");
        return true;
    }
    bool Name(std::string& s) {
        std::uint32_t n = r.U32();
        if (r.Fail() || n > kMaxName || n > r.Left()) return Fail("bad string");
        s.assign(reinterpret_cast<const char*>(r.Ptr()), n);
        r.Skip(n);
        return true;
    }
    bool SkipCounted(int prefix, int unit) {
        if (!r.Skip(static_cast<std::size_t>(prefix))) return Fail("eof");
        std::uint32_t n;
        if (!Count(static_cast<std::uint32_t>(unit), n)) return false;
        return r.Skip(static_cast<std::size_t>(n) * unit) || Fail("eof");
    }
    bool SkipBlend(int valueBytes) {
        if (!r.Skip(8)) return Fail("eof");
        std::uint32_t n;
        if (!Count(static_cast<std::uint32_t>(4 + valueBytes), n)) return false;
        return r.Skip(static_cast<std::size_t>(n) * (4 + valueBytes)) || Fail("eof");
    }
    bool SkipStrList() {
        std::uint32_t n;
        if (!Count(4, n)) return false;
        std::string s;
        for (std::uint32_t i = 0; i < n; ++i)
            if (!Name(s)) return false;
        return true;
    }
    bool CommandPayload(const std::string& c) {
        if (IsNoPayload(c)) return true;
        int fs = FixedSize(c);
        if (fs >= 0) return r.Skip(static_cast<std::size_t>(fs)) || Fail("eof");
        int p, u;
        if (Counted(c, p, u)) return SkipCounted(p, u);
        return Fail("unknown command '" + c + "'");
    }
    bool Source(EfpSource& d) {
        std::uint8_t has = r.U8();
        if (r.Fail()) return Fail("eof");
        if (has > 1) return Fail("source flag");
        d.has = has == 1;
        if (!d.has) return true;
        if (!Name(d.command)) return false;
        d.sourceType = r.U8();
        d.sourceByte = r.U8();
        for (float& t : d.times) t = r.F32();
        if (r.Fail()) return Fail("eof");
        std::size_t p0 = r.Pos();
        if (!CommandPayload(d.command)) return false;
        d.payload.assign(data + p0, data + r.Pos());
        return true;
    }
    bool SourceList(std::vector<EfpSource>& v) {
        std::uint32_t n;
        if (!Count(1, n)) return false;
        v.resize(n);
        for (auto& s : v)
            if (!Source(s)) return false;
        return true;
    }
    bool Resource(EfpResource& res) {
        std::uint32_t v[9];
        for (auto& x : v) x = r.U32();
        if (r.Fail()) return Fail("eof");
        res.cull = v[0]; res.src = v[1]; res.dst = v[2];
        for (int i = 0; i < 3; ++i) { res.stage1[i] = v[3 + i]; res.stage2[i] = v[6 + i]; }
        std::uint32_t n;
        if (!Count(8, n)) return false;
        res.meshes.resize(n);
        for (auto& m : res.meshes) {
            if (!Name(m.mesh)) return false;
            std::uint32_t tc;
            if (!Count(4, tc)) return false;
            m.textures.resize(tc);
            for (auto& t : m.textures)
                if (!Name(t)) return false;
        }
        return true;
    }
    bool ControllerPayload(const std::string& name, std::size_t blockEnd) {
        if (name == "NormalTimeLife" || name == "NormalTimeLoopLife") return true;
        if (name == "StaticEmit") return r.Skip(20) || Fail("eof");
        if (name == "Program") { std::vector<EfpSource> tmp; return SourceList(tmp); }
        if (name == "LinkMode") return r.Skip(16) || Fail("eof");
        if (name == "BAN") return SkipStrList();
        if (name == "ViewMode") { std::string s; return Name(s); }
        if (name == "Shape") { std::string s; EfpResource res; return Name(s) && Resource(res); }
        if (name == "ScaleGraph") return SkipBlend(4) && SkipBlend(4) && SkipBlend(4) && (r.Skip(8) || Fail("eof"));
        if (name == "DiffuseGraph") return SkipBlend(1) && SkipBlend(4);
        // unknown controller: bounded by skipLen
        if (blockEnd > r.Size() || blockEnd < r.Pos()) return Fail("unknown controller '" + name + "'");
        return r.Seek(blockEnd) || Fail("eof");
    }
    bool GlobalPayload(const std::string& name) {
        if (name == "BlendScaleGraph") return SkipBlend(12);
        if (name == "BlendDiffuseGraph") return SkipBlend(4);
        if (name == "BSAnimation") return SkipStrList();
        return Fail("unknown parameter '" + name + "'");
    }
    bool Obj(EfpObject& o, int depth) {
        if (depth > kMaxDepth) return Fail("tree too deep");
        std::uint32_t skip = r.U32();
        std::size_t base = r.Pos();
        if (!Name(o.name)) return false;
        std::uint32_t n = r.U32();
        if (r.Fail() || n > 4096) return Fail("bad controller count");
        std::size_t blockEnd = base + skip;
        o.controllers.resize(n);
        for (auto& c : o.controllers) {
            if (!Name(c.name)) return false;
            std::size_t p0 = r.Pos();
            if (!ControllerPayload(c.name, blockEnd)) return false;
            c.payload.assign(data + p0, data + r.Pos());
        }
        o.lifeFrames = r.U32();
        n = r.U32();
        if (r.Fail() || n > 4096) return Fail("bad parameter count");
        o.globals.resize(n);
        for (auto& g : o.globals) {
            if (!Name(g.name)) return false;
            std::size_t p0 = r.Pos();
            if (!GlobalPayload(g.name)) return false;
            g.payload.assign(data + p0, data + r.Pos());
        }
        if (!SourceList(o.list0) || !SourceList(o.emitList) || !SourceList(o.list2) || !Source(o.life) ||
            !SourceList(o.programList))
            return false;
        if (!r.Raw(o.scalars.data(), 20)) return Fail("eof");
        if (!Source(o.view) || !Resource(o.res) || !Source(o.render) || !SourceList(o.list3) || !SourceList(o.baked))
            return false;
        std::uint32_t cc;
        if (!Count(4, cc)) return false;
        o.children.reserve(cc);
        for (std::uint32_t i = 0; i < cc; ++i) {
            auto ch = std::make_unique<EfpObject>();
            if (!Obj(*ch, depth + 1)) return false;
            o.children.push_back(std::move(ch));
        }
        return true;
    }
};

}  // namespace

std::string EfpResource::Texture() const {
    for (const auto& m : meshes)
        if (!m.textures.empty()) return m.textures[0];
    return {};
}
std::string EfpResource::Mesh() const {
    for (const auto& m : meshes)
        if (!m.mesh.empty()) return m.mesh;
    return {};
}

const EfpNamed* EfpObject::Global(const char* n) const {
    for (const auto& g : globals)
        if (g.name == n) return &g;
    return nullptr;
}

int EfpObject::CountNodes() const {
    int n = 1;
    for (const auto& c : children) n += c->CountNodes();
    return n;
}

bool ParseEfp(const std::uint8_t* data, std::size_t size, EfpFile& out, std::string& err) {
    out = EfpFile();
    if (size < 12 || std::memcmp(data, "JMXVEFF ", 8) != 0) { err = "not a JMXVEFF file"; return false; }
    out.version.assign(reinterpret_cast<const char*>(data + 8), 4);
    if (out.version == "0000") { err = "legacy JMXVEFF 0000 layout (not loaded by the client)"; return false; }
    if (out.version != "0010" && out.version != "0011" && out.version != "0012" && out.version != "0013") {
        err = "unsupported JMXVEFF version " + out.version;
        return false;
    }
    Parser p(data, size);
    p.r.Seek(12);
    if (out.version == "0012" || out.version == "0013") {
        out.hasRootScale = true;
        out.rootScale = p.r.F32();
    }
    if (out.version == "0013")
        for (auto& v : out.ints0013) v = p.r.I32();
    if (p.r.Fail()) { err = "short header"; return false; }
    out.root = std::make_unique<EfpObject>();
    if (!p.Obj(*out.root, 0)) { err = p.err; return false; }
    return true;
}

std::vector<EfpBlendV3Key> DecodeBlendScaleGraph(const std::vector<std::uint8_t>& payload) {
    std::vector<EfpBlendV3Key> keys;
    RReader r(payload.data(), payload.size());
    r.Skip(8);
    std::uint32_t n = r.U32();
    if (r.Fail() || static_cast<std::uint64_t>(n) * 16 > r.Left()) return keys;
    keys.resize(n);
    for (auto& k : keys) { k.t = r.F32(); for (float& v : k.v) v = r.F32(); }
    return keys;
}

bool DecodeStaticEmit(const std::vector<std::uint8_t>& p, EfpStaticEmit& e) {
    if (p.size() < 20) return false;
    std::memcpy(&e.start, &p[0], 4);
    std::memcpy(&e.duration, &p[4], 4);
    std::memcpy(&e.interval, &p[8], 4);
    std::memcpy(&e.maxAlive, &p[12], 4);
    std::memcpy(&e.perEvent, &p[16], 4);
    return true;
}

std::vector<std::string> DecodeStrList(const std::vector<std::uint8_t>& payload) {
    std::vector<std::string> v;
    RReader r(payload.data(), payload.size());
    std::uint32_t n = r.U32();
    for (std::uint32_t i = 0; i < n && r.Ok(); ++i) {
        std::string s = r.Str(4096);
        if (r.Ok()) v.push_back(s);
    }
    return v;
}

}  // namespace se::render
