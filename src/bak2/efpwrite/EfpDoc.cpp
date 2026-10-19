// SkillEffect Studio - efpwrite/EfpDoc.cpp  (v6 M22) - lossless .efp reader / writer
#include "EfpDoc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace se::efpw {

namespace {

constexpr int kMaxDepth = 64;
constexpr std::uint32_t kMaxName = 4096;
constexpr std::uint32_t kMaxCount = 4096;

const char* const kNoPayload[] = {"NeverExtinct",  "NormalTimeExtinct", "NormalTimeLoop", "ViewNone",
                                  "ViewBillboard", "ViewYBillboard",    "ViewVBillboard", "RenderNone",
                                  "RenderPlate",   "RenderMesh",        "RenderLinkPipe", "RenderLinkDPipe",
                                  "RenderLinkObj", "ProgramUpdate",     "NormalTimeLife", "NormalTimeLoopLife"};
struct FixedCmd { const char* n; int s; };
const FixedCmd kFixed[] = {{"StaticEmit", 20},     {"SetRotationMat", 64},  {"SetRVelocityMat", 64},
                           {"SetRotation", 76},    {"SetRVelocity", 76},    {"SetRotationAxis", 80},
                           {"SetRVelocityAxis", 80}, {"SetShapeRot", 80},   {"SetShapeRotVel", 80},
                           {"SetPosition", 12},    {"SetSpherePos", 12},    {"SetVelocity", 12},
                           {"Force", 12},          {"SetConePos", 24},      {"SetConeVel", 24},
                           {"ConeForce", 24},      {"Attraction", 4},       {"SetGraphRandomScale", 4}};
struct CountedCmd { const char* n; int pre, unit; };
const CountedCmd kCounted[] = {{"TextureSlide", 12, 16}, {"SetGraphScale", 0, 12}, {"SetGraphDiffuse", 0, 4},
                               {"SetBANRot", 4, 64},     {"SetBANPos", 4, 12}};
const char* const kControllers[] = {"NormalTimeLife", "NormalTimeLoopLife", "StaticEmit", "Program", "LinkMode",
                                    "BAN",            "ViewMode",           "Shape",      "ScaleGraph", "DiffuseGraph"};
const char* const kProgram[] = {"SetRotationMat", "SetRVelocityMat", "SetRotation",   "SetRVelocity",
                                "SetRotationAxis", "SetRVelocityAxis", "SetBANRot",   "SetPosition",
                                "SetConePos",     "SetBANPos",       "SetSpherePos",  "SetVelocity",
                                "SetConeVel",     "Force",           "ConeForce",     "Attraction",
                                "TextureSlide",   "SetGraphScale",   "SetGraphRandomScale", "SetGraphDiffuse",
                                "SetShapeRot",    "SetShapeRotVel"};

bool NoPayload(const std::string& c) {
    for (const char* s : kNoPayload)
        if (c == s) return true;
    return false;
}
int FixedSize(const std::string& c) {
    for (const auto& e : kFixed)
        if (c == e.n) return e.s;
    return -1;
}
const CountedCmd* Counted(const std::string& c) {
    for (const auto& e : kCounted)
        if (c == e.n) return &e;
    return nullptr;
}
bool IsControllerName(const char* p, std::size_t n) {
    for (const char* s : kControllers)
        if (std::strlen(s) == n && std::memcmp(s, p, n) == 0) return true;
    return false;
}

std::string Hex(std::size_t v) {
    char b[32];
    sprintf_s(b, "0x%zX", v);
    return b;
}

class Parser {
public:
    Parser(const std::uint8_t* d, std::size_t n, const std::map<std::size_t, std::size_t>* hints)
        : m_d(d), m_n(n), m_hints(hints) {}

    std::string err;
    bool unknownHit = false;
    std::size_t unknownOff = 0;
    std::string unknownName;
    std::vector<std::string> notes;

    std::size_t Pos() const { return m_p; }
    std::size_t Left() const { return m_n - m_p; }

    bool Fail(const std::string& m) {
        if (err.empty()) err = m + " @" + Hex(m_p);
        return false;
    }
    bool U8(std::uint8_t& v) {
        if (Left() < 1) return Fail("eof");
        v = m_d[m_p++];
        return true;
    }
    bool U32(std::uint32_t& v) {
        if (Left() < 4) return Fail("eof");
        std::memcpy(&v, m_d + m_p, 4);
        m_p += 4;
        return true;
    }
    bool Raw(void* dst, std::size_t n) {
        if (Left() < n) return Fail("eof");
        std::memcpy(dst, m_d + m_p, n);
        m_p += n;
        return true;
    }
    bool Skip(std::size_t n) {
        if (Left() < n) return Fail("eof");
        m_p += n;
        return true;
    }
    bool Count(std::uint32_t unit, std::uint32_t& n) {
        if (!U32(n)) return false;
        if (unit && static_cast<std::uint64_t>(n) * unit > Left()) return Fail("bad count " + std::to_string(n));
        return true;
    }
    bool Name(std::string& s) {
        std::uint32_t n;
        if (!U32(n)) return false;
        if (n > kMaxName || n > Left()) return Fail("bad string length " + std::to_string(n));
        s.assign(reinterpret_cast<const char*>(m_d + m_p), n);
        m_p += n;
        return true;
    }
    bool SkipCounted(int pre, int unit) {
        if (!Skip(static_cast<std::size_t>(pre))) return false;
        std::uint32_t n;
        if (!Count(static_cast<std::uint32_t>(unit), n)) return false;
        return Skip(static_cast<std::size_t>(n) * static_cast<std::size_t>(unit));
    }
    bool SkipBlend(int valueBytes) {
        if (!Skip(8)) return false;
        std::uint32_t n;
        if (!Count(static_cast<std::uint32_t>(4 + valueBytes), n)) return false;
        return Skip(static_cast<std::size_t>(n) * static_cast<std::size_t>(4 + valueBytes));
    }
    bool SkipStrList() {
        std::uint32_t n;
        if (!Count(4, n)) return false;
        std::string s;
        for (std::uint32_t i = 0; i < n; ++i)
            if (!Name(s)) return false;
        return true;
    }
    // unknown command / parameter: hint size or stop the parse (driver re-parses with candidates)
    bool Unknown(const std::string& name, bool& unknown) {
        unknown = true;
        if (m_hints) {
            auto it = m_hints->find(m_p);
            if (it != m_hints->end()) {
                notes.push_back("unknown '" + name + "' at " + Hex(m_p) + ": " + std::to_string(it->second) +
                                " bytes kept raw");
                return Skip(it->second);
            }
        }
        unknownHit = true;
        unknownOff = m_p;
        unknownName = name;
        return Fail("unknown name '" + name + "'");
    }
    bool CommandPayload(const std::string& c, bool& unknown) {
        unknown = false;
        if (NoPayload(c)) return true;
        int fs = FixedSize(c);
        if (fs >= 0) return Skip(static_cast<std::size_t>(fs));
        if (const CountedCmd* cc = Counted(c)) return SkipCounted(cc->pre, cc->unit);
        return Unknown(c, unknown);
    }
    bool ReadSource(Source& d) {
        std::uint8_t has;
        if (!U8(has)) return false;
        if (has > 1) return Fail("source flag " + std::to_string(has));
        d = Source();
        d.has = has == 1;
        if (!d.has) return true;
        if (!Name(d.command) || !U8(d.type) || !U8(d.mode) || !Raw(d.timesRaw.data(), 12)) return false;
        std::size_t p0 = m_p;
        if (!CommandPayload(d.command, d.unknown)) return false;
        d.payload.assign(m_d + p0, m_d + m_p);
        return true;
    }
    bool SourceList(std::vector<Source>& v) {
        std::uint32_t n;
        if (!Count(1, n)) return false;
        v.clear();
        v.resize(n);
        for (auto& s : v)
            if (!ReadSource(s)) return false;
        return true;
    }
    bool ReadResource(Resource& res) {
        for (auto& x : res.state)
            if (!U32(x)) return false;
        std::uint32_t n;
        if (!Count(8, n)) return false;
        res.meshes.clear();
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
    std::size_t NextController(std::size_t at, std::size_t blockEnd) const {
        for (std::size_t p = at; p + 4 < blockEnd; ++p) {
            std::uint32_t n;
            std::memcpy(&n, m_d + p, 4);
            if (n >= 3 && n <= 32 && p + 4 + n <= blockEnd && IsControllerName(reinterpret_cast<const char*>(m_d + p + 4), n))
                return p;
        }
        return blockEnd;
    }
    bool ControllerPayload(const std::string& name, std::size_t blockEnd, bool& unknown) {
        unknown = false;
        if (name == "NormalTimeLife" || name == "NormalTimeLoopLife") return true;
        if (name == "StaticEmit") return Skip(20);
        if (name == "Program") { std::vector<Source> tmp; return SourceList(tmp); }
        if (name == "LinkMode") return Skip(16);
        if (name == "BAN") return SkipStrList();
        if (name == "ViewMode") { std::string s; return Name(s); }
        if (name == "Shape") { std::string s; Resource r; return Name(s) && ReadResource(r); }
        if (name == "ScaleGraph") return SkipBlend(4) && SkipBlend(4) && SkipBlend(4) && Skip(8);
        if (name == "DiffuseGraph") return SkipBlend(1) && SkipBlend(4);
        unknown = true;
        std::size_t at = m_p;
        if (m_hints) {
            auto it = m_hints->find(at);
            if (it != m_hints->end()) {
                notes.push_back("unknown controller '" + name + "' at " + Hex(at) + ": " + std::to_string(it->second) + " bytes");
                return Skip(it->second);
            }
        }
        if (blockEnd > m_n || blockEnd < at) {
            bool u;
            return Unknown(name, u);
        }
        std::size_t end = NextController(at, blockEnd);
        notes.push_back("unknown controller '" + name + "' at " + Hex(at) + ": " + std::to_string(end - at) + " bytes");
        m_p = end;
        return true;
    }
    bool GlobalPayload(const std::string& name, bool& unknown) {
        unknown = false;
        if (name == "BlendScaleGraph") return SkipBlend(12);
        if (name == "BlendDiffuseGraph") return SkipBlend(4);
        if (name == "BSAnimation") return SkipStrList();
        return Unknown(name, unknown);
    }
    bool Obj(Node& o, int depth) {
        if (depth > kMaxDepth) return Fail("object tree too deep");
        std::uint32_t skip;
        if (!U32(skip)) return false;
        std::size_t base = m_p;
        if (!Name(o.name)) return false;
        std::uint32_t n;
        if (!U32(n)) return false;
        if (n > kMaxCount) return Fail("bad controller count " + std::to_string(n));
        std::size_t blockEnd = base + skip;
        o.controllers.resize(n);
        for (auto& c : o.controllers) {
            if (!Name(c.name)) return false;
            std::size_t p0 = m_p;
            if (!ControllerPayload(c.name, blockEnd, c.unknown)) return false;
            c.payload.assign(m_d + p0, m_d + m_p);
        }
        o.skipOverride = (m_p - base) != skip;
        o.skipStored = skip;
        if (o.skipOverride) notes.push_back("skipLen override at " + Hex(base - 4) + ": " + std::to_string(skip));
        if (!U32(o.lifeFrames) || !U32(n)) return false;
        if (n > kMaxCount) return Fail("bad parameter count " + std::to_string(n));
        o.globals.resize(n);
        for (auto& g : o.globals) {
            if (!Name(g.name)) return false;
            std::size_t p0 = m_p;
            if (!GlobalPayload(g.name, g.unknown)) return false;
            g.payload.assign(m_d + p0, m_d + m_p);
        }
        if (!SourceList(o.list0) || !SourceList(o.emitList) || !SourceList(o.list2) || !ReadSource(o.life) ||
            !SourceList(o.programList))
            return false;
        if (!Raw(o.scalars.data(), 20)) return false;
        if (!ReadSource(o.view) || !ReadResource(o.res) || !ReadSource(o.render) || !SourceList(o.list3) ||
            !SourceList(o.baked))
            return false;
        std::uint32_t cc;
        if (!Count(4, cc)) return false;
        o.children.clear();
        o.children.resize(cc);
        for (auto& ch : o.children)
            if (!Obj(ch, depth + 1)) return false;
        return true;
    }
    bool File(Doc& d) {
        if (m_n < 12 || std::memcmp(m_d, "JMXVEFF ", 8) != 0) return Fail("not a JMXVEFF file");
        d.version.assign(reinterpret_cast<const char*>(m_d + 8), 4);
        m_p = 12;
        if (d.version == "0000") return Fail("legacy JMXVEFF 0000 layout (no client loader)");
        if (d.version != "0010" && d.version != "0011" && d.version != "0012" && d.version != "0013")
            return Fail("unsupported JMXVEFF version '" + d.version + "'");
        if (d.HasRootScale() && !Raw(d.rootScaleRaw.data(), 4)) return false;
        if (d.version == "0013" && !Raw(d.ints0013.data(), 12)) return false;
        if (!Obj(d.root, 0)) return false;
        d.trailing.assign(m_d + m_p, m_d + m_n);
        m_p = m_n;
        return true;
    }

private:
    const std::uint8_t* m_d;
    std::size_t m_n;
    std::size_t m_p = 0;
    const std::map<std::size_t, std::size_t>* m_hints;
};

std::vector<std::size_t> Candidates(const std::uint8_t* d, std::size_t n, std::size_t at, std::size_t limit) {
    std::vector<std::size_t> out;
    std::set<std::size_t> seen;
    std::size_t left = n - at;
    auto add = [&](std::size_t s) {
        if (s <= left && seen.insert(s).second) out.push_back(s);
    };
    for (std::size_t pre : {0u, 4u, 8u, 12u}) {
        if (at + pre + 4 <= n) {
            std::uint32_t c;
            std::memcpy(&c, d + at + pre, 4);
            for (std::size_t unit : {1u, 4u, 8u, 12u, 16u, 64u})
                if (static_cast<std::uint64_t>(c) * unit <= left) add(pre + 4 + static_cast<std::size_t>(c) * unit);
        }
    }
    for (std::size_t s : {0u, 4u, 8u, 12u, 16u, 20u, 24u, 64u, 76u, 80u}) add(s);
    for (std::size_t s = 0; s <= std::min(left, limit); ++s) add(s);
    return out;
}

void PutU32To(Bytes& w, std::uint32_t v) {
    std::uint8_t b[4];
    std::memcpy(b, &v, 4);
    w.insert(w.end(), b, b + 4);
}
void PutStr(Bytes& w, const std::string& s) {
    PutU32To(w, static_cast<std::uint32_t>(s.size()));
    w.insert(w.end(), s.begin(), s.end());
}
void WriteSource(Bytes& w, const Source& s) {
    w.push_back(s.has ? 1 : 0);
    if (!s.has) return;
    PutStr(w, s.command);
    w.push_back(s.type);
    w.push_back(s.mode);
    w.insert(w.end(), s.timesRaw.begin(), s.timesRaw.end());
    w.insert(w.end(), s.payload.begin(), s.payload.end());
}
void WriteList(Bytes& w, const std::vector<Source>& v) {
    PutU32To(w, static_cast<std::uint32_t>(v.size()));
    for (const auto& s : v) WriteSource(w, s);
}
void WriteResource(Bytes& w, const Resource& r) {
    for (std::uint32_t x : r.state) PutU32To(w, x);
    PutU32To(w, static_cast<std::uint32_t>(r.meshes.size()));
    for (const auto& m : r.meshes) {
        PutStr(w, m.mesh);
        PutU32To(w, static_cast<std::uint32_t>(m.textures.size()));
        for (const auto& t : m.textures) PutStr(w, t);
    }
}
void WriteNode(Bytes& w, const Node& o) {
    Bytes head;
    PutStr(head, o.name);
    PutU32To(head, static_cast<std::uint32_t>(o.controllers.size()));
    for (const auto& c : o.controllers) {
        PutStr(head, c.name);
        head.insert(head.end(), c.payload.begin(), c.payload.end());
    }
    PutU32To(w, o.skipOverride ? o.skipStored : static_cast<std::uint32_t>(head.size()));
    w.insert(w.end(), head.begin(), head.end());
    PutU32To(w, o.lifeFrames);
    PutU32To(w, static_cast<std::uint32_t>(o.globals.size()));
    for (const auto& g : o.globals) {
        PutStr(w, g.name);
        w.insert(w.end(), g.payload.begin(), g.payload.end());
    }
    WriteList(w, o.list0);
    WriteList(w, o.emitList);
    WriteList(w, o.list2);
    WriteSource(w, o.life);
    WriteList(w, o.programList);
    w.insert(w.end(), o.scalars.begin(), o.scalars.end());
    WriteSource(w, o.view);
    WriteResource(w, o.res);
    WriteSource(w, o.render);
    WriteList(w, o.list3);
    WriteList(w, o.baked);
    PutU32To(w, static_cast<std::uint32_t>(o.children.size()));
    for (const auto& c : o.children) WriteNode(w, c);
}

template <class N, class V> void Walk(N& n, V& out) {
    out.push_back(&n);
    for (auto& c : n.children) Walk(c, out);
}
void WalkPaths(const Node& n, const std::string& prefix, std::vector<std::string>& out) {
    std::string p = prefix.empty() ? n.name : prefix + "/" + n.name;
    out.push_back(p);
    for (const auto& c : n.children) WalkPaths(c, p, out);
}

}  // namespace

float Source::Time(int i) const {
    float v;
    std::memcpy(&v, timesRaw.data() + 4 * i, 4);
    return v;
}
void Source::SetTime(int i, float v) { std::memcpy(timesRaw.data() + 4 * i, &v, 4); }

Named* Node::Controller(const char* n) {
    for (auto& c : controllers)
        if (c.name == n) return &c;
    return nullptr;
}
const Named* Node::Controller(const char* n) const {
    for (const auto& c : controllers)
        if (c.name == n) return &c;
    return nullptr;
}
Named* Node::Global(const char* n) {
    for (auto& g : globals)
        if (g.name == n) return &g;
    return nullptr;
}
const Named* Node::Global(const char* n) const {
    for (const auto& g : globals)
        if (g.name == n) return &g;
    return nullptr;
}
int Node::CountNodes() const {
    int n = 1;
    for (const auto& c : children) n += c.CountNodes();
    return n;
}

float Doc::RootScale() const {
    if (!HasRootScale()) return 1.0f;
    float v;
    std::memcpy(&v, rootScaleRaw.data(), 4);
    return v;
}
void Doc::SetRootScale(float v) {
    if (HasRootScale()) std::memcpy(rootScaleRaw.data(), &v, 4);
}

bool Parse(const std::uint8_t* data, std::size_t size, Doc& out, std::string& err, int maxRecover) {
    struct Frame { std::size_t off; std::vector<std::size_t> cands; std::size_t idx; };
    std::map<std::size_t, std::size_t> hints;
    std::vector<Frame> stack;
    int attempts = 0;
    auto nextCandidate = [&]() -> bool {   // advance the innermost guess (pop exhausted ones)
        while (!stack.empty()) {
            Frame& top = stack.back();
            ++top.idx;
            ++attempts;
            if (top.idx < top.cands.size() && attempts <= maxRecover) {
                hints[top.off] = top.cands[top.idx];
                return true;
            }
            hints.erase(top.off);
            stack.pop_back();
        }
        return false;
    };
    for (;;) {
        Doc d;
        Parser p(data, size, &hints);
        bool ok = p.File(d);
        if (!ok && p.unknownHit) {
            if (++attempts > maxRecover) { err = p.err; return false; }
            stack.push_back({p.unknownOff, Candidates(data, size, p.unknownOff, 1024), 0});
            hints[p.unknownOff] = stack.back().cands.empty() ? 0 : stack.back().cands[0];
            if (stack.back().cands.empty()) { err = p.err; return false; }
            continue;
        }
        if (!ok) {
            std::string e = p.err;
            if (stack.empty() || !nextCandidate()) { err = e; return false; }
            continue;
        }
        if (!stack.empty() && !d.trailing.empty()) {   // a recovered size leaving bytes at the end = wrong guess
            if (!nextCandidate()) { err = "unknown name (no payload size fits)"; return false; }
            continue;
        }
        d.notes = std::move(p.notes);
        if (!d.trailing.empty()) d.notes.push_back(std::to_string(d.trailing.size()) + " trailing bytes kept raw");
        out = std::move(d);
        return true;
    }
}

Bytes Serialize(const Doc& d) {
    Bytes w;
    w.reserve(4096);
    const char magic[8] = {'J', 'M', 'X', 'V', 'E', 'F', 'F', ' '};
    w.insert(w.end(), magic, magic + 8);
    std::string v = d.version;
    v.resize(4, '0');
    w.insert(w.end(), v.begin(), v.end());
    if (d.HasRootScale()) w.insert(w.end(), d.rootScaleRaw.begin(), d.rootScaleRaw.end());
    if (d.version == "0013") w.insert(w.end(), d.ints0013.begin(), d.ints0013.end());
    WriteNode(w, d.root);
    w.insert(w.end(), d.trailing.begin(), d.trailing.end());
    return w;
}

std::vector<Node*> NodesPreOrder(Doc& d) {
    std::vector<Node*> v;
    Walk(d.root, v);
    return v;
}
std::vector<const Node*> NodesPreOrder(const Doc& d) {
    std::vector<const Node*> v;
    Walk(d.root, v);
    return v;
}
std::vector<std::string> NodePaths(const Doc& d) {
    std::vector<std::string> v;
    WalkPaths(d.root, "", v);
    return v;
}

bool IsKnownCommand(const std::string& c) { return NoPayload(c) || FixedSize(c) >= 0 || Counted(c) != nullptr; }
bool IsProgramCommand(const std::string& c) {
    for (const char* s : kProgram)
        if (c == s) return true;
    return false;
}

float GetF32(const Bytes& b, std::size_t off) {
    float v = 0;
    if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
    return v;
}
void PutF32(Bytes& b, std::size_t off, float v) {
    if (off + 4 <= b.size()) std::memcpy(b.data() + off, &v, 4);
}
std::uint32_t GetU32(const Bytes& b, std::size_t off) {
    std::uint32_t v = 0;
    if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
    return v;
}
void PutU32(Bytes& b, std::size_t off, std::uint32_t v) {
    if (off + 4 <= b.size()) std::memcpy(b.data() + off, &v, 4);
}

bool DecodeSourceList(const Bytes& payload, std::vector<Source>& out) {
    Parser p(payload.data(), payload.size(), nullptr);
    return p.SourceList(out) && p.Pos() == payload.size();
}
Bytes EncodeSourceList(const std::vector<Source>& v) {
    Bytes w;
    WriteList(w, v);
    return w;
}
bool DecodeShape(const Bytes& payload, std::string& render, Resource& res) {
    Parser p(payload.data(), payload.size(), nullptr);
    return p.Name(render) && p.ReadResource(res) && p.Pos() == payload.size();
}
Bytes EncodeShape(const std::string& render, const Resource& res) {
    Bytes w;
    PutStr(w, render);
    WriteResource(w, res);
    return w;
}

}  // namespace se::efpw
