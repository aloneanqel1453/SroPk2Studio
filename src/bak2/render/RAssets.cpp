// SkillEffect Studio - render/RAssets.cpp
#include "RAssets.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace se::render {

std::wstring Cp949ToW(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(949, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        std::wstring w;
        for (unsigned char c : s) w.push_back(c);
        return w;
    }
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(949, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string WToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string o(static_cast<std::size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), o.data(), n, nullptr, nullptr);
    return o;
}

std::wstring NormPath(const std::wstring& p) {
    std::wstring o;
    o.reserve(p.size());
    for (wchar_t c : p) o.push_back(c == L'/' ? L'\\' : c);
    while (!o.empty() && (o.back() == L' ' || o.back() == L'\t' || o.back() == L'\r' || o.back() == L'\n' || o.back() == L'\0')) o.pop_back();
    std::size_t a = 0;
    while (a < o.size() && (o[a] == L' ' || o[a] == L'\\')) ++a;
    o.erase(0, a);
    // collapse "\\" and resolve ".."
    std::wstring r;
    std::vector<std::wstring> parts;
    std::size_t st = 0;
    while (st <= o.size()) {
        std::size_t e = o.find(L'\\', st);
        if (e == std::wstring::npos) e = o.size();
        std::wstring seg = o.substr(st, e - st);
        if (seg == L"..") { if (!parts.empty()) parts.pop_back(); }
        else if (!seg.empty() && seg != L".") parts.push_back(seg);
        st = e + 1;
    }
    for (std::size_t i = 0; i < parts.size(); ++i) { if (i) r += L'\\'; r += parts[i]; }
    return r;
}

std::wstring LowerW(const std::wstring& s) {
    std::wstring o(s);
    for (auto& c : o) c = static_cast<wchar_t>(std::towlower(c));
    return o;
}

std::wstring DirOf(const std::wstring& p) {
    std::size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring() : p.substr(0, k);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& rel) {
    if (dir.empty()) return NormPath(rel);
    return NormPath(dir + L"\\" + rel);
}

bool AssetCache::Read(Arc arc, const std::wstring& path, std::vector<std::uint8_t>& out) {
    out.clear();
    if (!m_src || path.empty()) return false;
    return m_src->Read(arc, NormPath(path), out);
}

void AssetCache::Warn(const std::wstring& w) {
    if (m_warn.size() < 500) m_warn.push_back(w);
}

std::vector<std::wstring> AssetCache::TakeWarnings() {
    std::lock_guard<std::mutex> lk(m_mx);
    std::vector<std::wstring> w;
    w.swap(m_warn);
    return w;
}

void AssetCache::Clear() {
    std::lock_guard<std::mutex> lk(m_mx);
    m_tex.clear(); m_efp.clear(); m_bms.clear(); m_bmt.clear(); m_bsk.clear(); m_ban.clear(); m_bsr.clear();
}

namespace {
size_t TexBytes(const RTexture& t) { return t.rgba.size() * sizeof(float) + t.img.rgba.size() + 64; }
}  // namespace

size_t AssetCache::ApproxBytes() {
    std::lock_guard<std::mutex> lk(m_mx);
    size_t n = 0;
    for (const auto& [k, t] : m_tex)
        if (t) n += TexBytes(*t);
    return n + (m_bms.size() + m_ban.size() + m_bsr.size() + m_efp.size()) * 64 * 1024;  // rough for the rest
}

size_t AssetCache::Trim(size_t maxBytes) {
    if (ApproxBytes() <= maxBytes) return ApproxBytes();
    {
        std::lock_guard<std::mutex> lk(m_mx);
        auto drop = [](auto& m) {
            for (auto it = m.begin(); it != m.end();) {
                if (!it->second || it->second.use_count() == 1) it = m.erase(it);
                else ++it;
            }
        };
        drop(m_tex);
        drop(m_bms);
        drop(m_bmt);
        drop(m_ban);
        drop(m_efp);
    }
    return ApproxBytes();
}

std::shared_ptr<RTexture> AssetCache::Texture(Arc arc, const std::wstring& path) {
    std::wstring key = std::to_wstring(static_cast<int>(arc)) + L"|" + LowerW(NormPath(path));
    {
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_tex.find(key);
        if (it != m_tex.end()) return it->second;
    }
    std::shared_ptr<RTexture> t;
    std::vector<std::uint8_t> data;
    if (Read(arc, path, data)) {
        Image img;
        std::string err;
        if (DecodeDdj(data.data(), data.size(), img, err)) {
            t = std::make_shared<RTexture>();
            t->w = img.w; t->h = img.h;
            t->path = NormPath(path);
            t->rgba.resize(img.rgba.size());
            std::size_t mid = 0, zero = 0;
            double rgb = 0;
            for (std::size_t i = 0; i < img.rgba.size(); ++i) t->rgba[i] = static_cast<float>(img.rgba[i]) / 255.0f;
            for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
                int a = img.rgba[i + 3];
                if (a > 12 && a < 244) ++mid;
                if (a <= 12) ++zero;
                rgb += (img.rgba[i] + img.rgba[i + 1] + img.rgba[i + 2]) / 3.0;
            }
            std::size_t px = img.rgba.size() / 4;
            if (px) { t->midAlpha = static_cast<float>(mid) / px; t->zeroAlpha = static_cast<float>(zero) / px; t->meanRgb = static_cast<float>(rgb / px); }
            t->anyAlpha = mid + zero > 0;
            t->img = std::move(img);
        } else {
            std::lock_guard<std::mutex> lk(m_mx);
            Warn(L"doku okunamadi: " + path + L" (" + Cp949ToW(err) + L")");
        }
    } else {
        std::lock_guard<std::mutex> lk(m_mx);
        Warn(L"doku yok: " + path);
    }
    std::lock_guard<std::mutex> lk(m_mx);
    m_tex[key] = t;
    return t;
}

template <class T, class F>
std::shared_ptr<const T> AssetCache::Get(std::map<std::wstring, std::shared_ptr<const T>>& m, Arc arc,
                                         const std::wstring& path, F parse) {
    std::wstring key = std::to_wstring(static_cast<int>(arc)) + L"|" + LowerW(NormPath(path));
    {
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m.find(key);
        if (it != m.end()) return it->second;
    }
    std::shared_ptr<const T> res;
    std::vector<std::uint8_t> data;
    std::string err;
    if (Read(arc, path, data)) {
        auto v = std::make_shared<T>();
        if (parse(data.data(), data.size(), *v, err)) res = v;
        else { std::lock_guard<std::mutex> lk(m_mx); Warn(L"okunamadi: " + path + L" (" + Cp949ToW(err) + L")"); }
    } else {
        std::lock_guard<std::mutex> lk(m_mx);
        Warn(L"dosya yok: " + path);
    }
    std::lock_guard<std::mutex> lk(m_mx);
    m[key] = res;
    return res;
}

std::shared_ptr<const EfpFile> AssetCache::Efp(const std::wstring& path, std::string* errOut) {
    std::wstring key = LowerW(NormPath(path));
    {
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_efp.find(key);
        if (it != m_efp.end()) {
            if (!it->second && errOut) *errOut = "missing or unreadable";
            return it->second;
        }
    }
    std::shared_ptr<const EfpFile> res;
    std::vector<std::uint8_t> data;
    std::string err;
    if (Read(Arc::Particles, path, data)) {
        auto f = std::make_shared<EfpFile>();
        if (ParseEfp(data.data(), data.size(), *f, err)) res = f;
    } else err = "missing";
    if (!res && errOut) *errOut = err;
    std::lock_guard<std::mutex> lk(m_mx);
    if (!res) Warn(L"efp: " + path + L" (" + Cp949ToW(err) + L")");
    m_efp[key] = res;
    return res;
}

std::shared_ptr<const BmsFile> AssetCache::Bms(Arc arc, const std::wstring& path) { return Get<BmsFile>(m_bms, arc, path, ParseBms); }
std::shared_ptr<const BmtFile> AssetCache::Bmt(Arc arc, const std::wstring& path) { return Get<BmtFile>(m_bmt, arc, path, ParseBmt); }
std::shared_ptr<const BskFile> AssetCache::Bsk(Arc arc, const std::wstring& path) { return Get<BskFile>(m_bsk, arc, path, ParseBsk); }
std::shared_ptr<const BanFile> AssetCache::Ban(Arc arc, const std::wstring& path) { return Get<BanFile>(m_ban, arc, path, ParseBan); }
std::shared_ptr<const BsrFile> AssetCache::Bsr(Arc arc, const std::wstring& path) { return Get<BsrFile>(m_bsr, arc, path, ParseBsr); }

}  // namespace se::render
