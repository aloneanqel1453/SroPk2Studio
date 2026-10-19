// SkillEffect Studio - efpwrite/EfpVariant.cpp  (v6 M22)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "EfpVariant.h"

#include "EfpTexture.h"
#include "../render/Efp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace se::efpw {

namespace {

std::wstring Lower(std::wstring s) {
    for (auto& c : s) {
        c = static_cast<wchar_t>(towlower(c));
        if (c == L'/') c = L'\\';
    }
    while (!s.empty() && (s.back() == L'\\' || s.back() == L' ')) s.pop_back();
    while (!s.empty() && (s.front() == L'\\' || s.front() == L' ')) s.erase(s.begin());
    return s;
}
std::wstring Pct(double v) {
    wchar_t b[32];
    swprintf_s(b, L"%d", static_cast<int>(std::lround(v * 100.0)));
    return b;
}
std::wstring Num(double v) {
    wchar_t b[32];
    int i = static_cast<int>(std::lround(v));
    if (i < 0) swprintf_s(b, L"m%d", -i);
    else swprintf_s(b, L"%d", i);
    return b;
}
std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(std::max(0, n)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring FullPath(const std::wstring& p) {
    wchar_t buf[4096];
    DWORD n = GetFullPathNameW(p.c_str(), 4096, buf, nullptr);
    if (n == 0 || n >= 4096) return p;
    return std::wstring(buf, n);
}
bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool MakeDirs(const std::wstring& dir) {
    if (dir.empty()) return true;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::size_t s = dir.find_last_of(L"\\/");
    if (s != std::wstring::npos && s > 2 && !MakeDirs(dir.substr(0, s))) return false;
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}
bool WriteAtomic(const std::wstring& path, const Bytes& data, std::wstring& err) {
    std::wstring tmp = path + L".efpw_tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = L"cannot create " + tmp; return false; }
    DWORD wr = 0;
    BOOL ok = data.empty() || WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wr, nullptr);
    ok = ok && (data.empty() || wr == data.size()) && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); err = L"write failed: " + tmp; return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        err = L"move failed: " + path;
        return false;
    }
    return true;
}
std::wstring StemOf(const std::wstring& inner) {
    std::size_t s = inner.find_last_of(L"\\/");
    std::wstring f = s == std::wstring::npos ? inner : inner.substr(s + 1);
    std::size_t d = f.find_last_of(L'.');
    return d == std::wstring::npos ? f : f.substr(0, d);
}

}  // namespace

std::wstring Cp949ToW(const std::string& s) {
    if (s.empty()) return {};
    for (UINT cp : {949u, static_cast<UINT>(CP_ACP)}) {
        int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (n <= 0) continue;
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }
    std::wstring w;
    for (char c : s) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return w;
}
std::string WToCp949(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(949, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(std::max(0, n)), '\0');
    if (n > 0) WideCharToMultiByte(949, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool SamePath(const std::wstring& a, const std::wstring& b) { return Lower(a) == Lower(b); }

std::wstring AutoTag(const VariantSpec& s) {
    std::vector<std::wstring> p;
    const ColorAdjust& c = s.color;
    if (c.colorize) {
        p.push_back(L"c" + Num(c.hueShift));
        if (c.saturation != 1) p.push_back(L"s" + Pct(c.saturation));
    } else {
        if (c.hueShift != 0) p.push_back(L"h" + Num(c.hueShift));
        if (c.saturation != 1) p.push_back(L"s" + Pct(c.saturation));
    }
    if (c.brightness != 1) p.push_back(L"v" + Pct(c.brightness));
    if (!s.colorKeys.empty()) p.push_back(L"ck");
    if (s.scale != 1) p.push_back(L"x" + Pct(s.scale));
    if (s.particleSize != 1) p.push_back(L"ps" + Pct(s.particleSize));
    if (s.speed != 1) p.push_back(L"sp" + Pct(s.speed));
    if (s.duration != 1) p.push_back(L"du" + Pct(s.duration));
    if (!s.textureSwap.empty()) p.push_back(L"tx");
    if (!s.meshSwap.empty()) p.push_back(L"ms");
    if (p.empty()) return L"copy";
    std::wstring t;
    for (const auto& x : p) t += (t.empty() ? L"" : L"_") + x;
    return t;
}

std::wstring NewInnerName(const std::wstring& inner, const std::wstring& tag) {
    std::size_t s = inner.find_last_of(L"\\/");
    std::size_t d = inner.find_last_of(L'.');
    if (d == std::wstring::npos || (s != std::wstring::npos && d < s)) return inner + L"_" + tag;
    return inner.substr(0, d) + L"_" + tag + inner.substr(d);
}

bool MakeVariant(const std::wstring& efpInner, const VariantSpec& spec, const ReadFn& read, VariantResult& out,
                 std::wstring& err) {
    Bytes b;
    if (!read || !read(efpInner, b)) { err = L"cannot read " + efpInner; return false; }
    return MakeVariantFromBytes(b, efpInner, spec, read, out, err);
}

bool MakeVariantFromBytes(const Bytes& efp, const std::wstring& efpInner, const VariantSpec& spec, const ReadFn& read,
                          VariantResult& out, std::wstring& err) {
    out = VariantResult();
    Doc d;
    std::string e;
    if (!Parse(efp, d, e)) { err = L"parse: " + Cp949ToW(e); return false; }
    std::wstring tag = spec.tag.empty() ? AutoTag(spec) : spec.tag;
    for (wchar_t ch : tag)
        if (ch < 0x20 || ch > 0x7E || wcschr(L"\\/:*?\"<>| ", ch)) { err = L"tag must be plain ASCII without spaces / path characters"; return false; }
    EditReport& rep = out.report;
    const NodeList& nl = spec.nodes;
    int nodeCount = d.root.CountNodes();
    for (int i : nl)
        if (i < 0 || i >= nodeCount) { err = L"node index out of range"; return false; }
    for (const auto& [from, to] : spec.textureSwap) SwapTexture(d, from, to, nl, &rep);
    for (const auto& [from, to] : spec.meshSwap) SwapMesh(d, from, to, nl, &rep);
    if (read) {   // a swap target the client does not have is allowed (packed later) but reported
        Bytes probe;
        for (const auto* list : {&spec.textureSwap, &spec.meshSwap})
            for (const auto& sw : *list)
                if (!read(Cp949ToW(sw.second), probe)) out.notes.push_back("swap target not in the client (pack it too): " + Utf8(Cp949ToW(sw.second)));
    }
    ChangeDuration(d, spec.duration, nl, &rep);
    ChangeSpeed(d, spec.speed, nl, &rep);
    ScaleParticles(d, spec.particleSize, nl, &rep, true);
    if (!ScaleEffect(d, spec.scale, e, true, &rep)) { err = Cp949ToW(e); return false; }
    {
        auto all = NodesPreOrder(d);
        for (const auto& [idx, keys] : spec.colorKeys) {
            if (idx < 0 || idx >= static_cast<int>(all.size())) { err = L"colour-key node out of range"; return false; }
            SetColorKeys(*all[static_cast<std::size_t>(idx)], keys, true, &rep);
        }
    }
    Recolor(d, spec.color, nl, &rep, spec.addMissingColor);
    std::vector<OutFile> tex;
    if (spec.recolorTextures && !spec.color.IsIdentity()) {
        for (const ResUse& u : ListResources(d)) {
            if (u.mesh) continue;
            NodeList use;
            for (int n : u.nodes)
                if (nl.empty() || std::find(nl.begin(), nl.end(), n) != nl.end()) use.push_back(n);
            if (use.empty()) continue;
            std::wstring inner = Cp949ToW(u.path);
            Bytes src, dst;
            if (!read || !read(inner, src)) { out.notes.push_back("texture missing, kept: " + Utf8(inner)); continue; }
            std::string te;
            std::size_t px = 0;
            if (!RecolorTexture(src, spec.color, dst, te, &px)) {
                out.notes.push_back("texture not recoloured (" + te + "): " + Utf8(inner));
                continue;
            }
            if (px == 0) { out.notes.push_back("texture unchanged by the colour edit (grey / black), kept: " + Utf8(inner)); continue; }
            std::wstring ni = NewInnerName(inner, tag);
            std::string niRaw = WToCp949(ni);
            if (Cp949ToW(niRaw) != ni) { out.notes.push_back("texture name not representable in CP949: " + Utf8(ni)); continue; }
            SwapTexture(d, u.path, niRaw, use, &rep);
            tex.push_back({ni, inner, std::move(dst)});
        }
    }
    std::vector<std::string> problems;
    if (!Validate(d, problems)) { err = L"self-check: " + Cp949ToW(problems.front()); return false; }
    Bytes outEfp = Serialize(d);
    {
        se::render::EfpFile rf;
        std::string re;
        if (!se::render::ParseEfp(outEfp.data(), outEfp.size(), rf, re)) { err = L"render reader refuses the result: " + Cp949ToW(re); return false; }
    }
    std::wstring name = spec.newEfpName.empty() ? NewInnerName(efpInner, tag) : spec.newEfpName;
    if (SamePath(name, efpInner)) { err = L"the new .efp needs a new name (same as the source)"; return false; }
    for (const auto& t : tex)
        if (SamePath(t.inner, name)) { err = L"name clash"; return false; }
    out.efpInner = name;
    out.files.push_back({name, efpInner, std::move(outEfp)});
    for (auto& t : tex) out.files.push_back(std::move(t));
    out.manifest = ManifestText(out, spec, efpInner);
    return true;
}

std::string ManifestText(const VariantResult& v, const VariantSpec& spec, const std::wstring& sourceEfp) {
    std::string m = "# SkillEffect Studio - efpwrite (M22) new effect variant (pack the files at these Particles paths)\n";
    m += "source\t" + Utf8(sourceEfp) + "\n";
    for (const auto& f : v.files) {
        std::size_t d = f.inner.find_last_of(L'.');
        std::wstring ext = d == std::wstring::npos ? L"" : f.inner.substr(d + 1);
        m += Utf8(ext) + "\t" + Utf8(f.from) + "\t" + Utf8(f.inner) + "\n";
    }
    char buf[512];
    std::string nodes;
    for (int n : spec.nodes) nodes += (nodes.empty() ? "" : ",") + std::to_string(n);
    sprintf_s(buf, "settings\thue=%g sat=%g val=%g colorize=%d textures=%d scale=%g size=%g speed=%g duration=%g nodes=%s\n",
              spec.color.hueShift, spec.color.saturation, spec.color.brightness, spec.color.colorize ? 1 : 0,
              spec.recolorTextures ? 1 : 0, spec.scale, spec.particleSize, spec.speed, spec.duration,
              nodes.empty() ? "all" : nodes.c_str());
    m += buf;
    for (const auto& n : v.report.notes) m += "note\t" + n + "\n";
    for (const auto& n : v.notes) m += "note\t" + n + "\n";
    return m;
}

bool CheckOutputDir(const std::wstring& outDir, const std::vector<std::wstring>& protectedRoots, std::wstring& err) {
    if (outDir.empty()) { err = L"no output folder"; return false; }
    std::wstring full = Lower(FullPath(outDir));
    for (const auto& r : protectedRoots) {
        if (r.empty()) continue;
        std::wstring pr = Lower(FullPath(r));
        if (full == pr || (full.size() > pr.size() && full.compare(0, pr.size(), pr) == 0 && full[pr.size()] == L'\\')) {
            err = L"output folder is inside a protected folder: " + r;
            return false;
        }
    }
    std::wstring cur = FullPath(outDir);
    while (!cur.empty()) {
        for (const wchar_t* f : {L"sro_client.exe", L"Particles.pk2", L"Media.pk2"})
            if (FileExists(cur + L"\\" + f)) { err = L"output folder is inside a client folder: " + cur; return false; }
        std::size_t s = cur.find_last_of(L'\\');
        if (s == std::wstring::npos || s < 3) break;
        cur.resize(s);
    }
    return true;
}

bool WriteVariant(const VariantResult& v, const std::wstring& outDir, const WriteOptions& opt,
                  std::vector<std::wstring>* written, std::wstring& err) {
    if (!CheckOutputDir(outDir, opt.protectedRoots, err)) return false;
    if (v.files.empty()) { err = L"nothing to write"; return false; }
    std::wstring root = FullPath(outDir);
    std::set<std::wstring> seen;
    for (const auto& f : v.files) {
        if (f.inner.empty() || f.inner.find(L"..") != std::wstring::npos || f.inner.find(L':') != std::wstring::npos) {
            err = L"bad output name: " + f.inner;
            return false;
        }
        if (SamePath(f.inner, f.from)) { err = L"refused: output would keep the source name " + f.from; return false; }
        if (!seen.insert(Lower(f.inner)).second) { err = L"duplicate output " + f.inner; return false; }
        std::wstring target = root + L"\\" + f.inner;
        if (!opt.overwrite && FileExists(target)) { err = L"exists (overwrite off): " + target; return false; }
    }
    std::wstring manifestPath = root + L"\\efpwrite_" + StemOf(v.efpInner) + L".txt";
    if (opt.manifest && !opt.overwrite && FileExists(manifestPath)) { err = L"exists (overwrite off): " + manifestPath; return false; }
    for (const auto& f : v.files) {
        std::wstring target = root + L"\\" + f.inner;
        std::size_t s = target.find_last_of(L'\\');
        if (!MakeDirs(target.substr(0, s))) { err = L"cannot create folder for " + target; return false; }
        if (!WriteAtomic(target, f.data, err)) return false;
        if (written) written->push_back(target);
    }
    if (opt.manifest) {
        Bytes m(v.manifest.begin(), v.manifest.end());
        if (!MakeDirs(root) || !WriteAtomic(manifestPath, m, err)) return false;
        if (written) written->push_back(manifestPath);
    }
    return true;
}

}  // namespace se::efpw
