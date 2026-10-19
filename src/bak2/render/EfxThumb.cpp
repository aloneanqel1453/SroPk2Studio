// SkillEffect Studio v6 - render/EfxThumb.cpp (see EfxThumb.h)
#include "EfxThumb.h"

#include "EfpSim.h"
#include "EfxDraw.h"
#include "SoftRaster.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace se::render {

namespace {

bool Finite(const V3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    for (auto& c : s)
        if (c == L'/') c = L'\\';
    return s;
}

bool FileThere(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void MakeDirs(const std::wstring& dir) {
    for (std::size_t i = 3; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == L'\\' || dir[i] == L'/') CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
}

std::uint64_t Ticks() { return GetTickCount64(); }

}  // namespace

std::uint64_t Fnv64(const void* data, std::size_t n, std::uint64_t h) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::wstring Hex64(std::uint64_t v) {
    wchar_t b[20];
    swprintf_s(b, L"%016llx", static_cast<unsigned long long>(v));
    return b;
}

std::wstring ThumbCacheRoot() {
    wchar_t b[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", b, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    return std::wstring(b) + L"\\SkillEffectStudio\\thumbs";
}

std::wstring ClientFinger(const std::wstring& particlesPath) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    std::uint64_t sz = 0, mt = 0;
    if (GetFileAttributesExW(particlesPath.c_str(), GetFileExInfoStandard, &d)) {
        sz = (static_cast<std::uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
        mt = (static_cast<std::uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    }
    const std::wstring l = Lower(particlesPath);
    std::uint64_t h = Fnv64(l.data(), l.size() * sizeof(wchar_t));
    h = Fnv64(&sz, sizeof sz, h);
    h = Fnv64(&mt, sizeof mt, h);
    return Hex64(h);
}

// ------------------------------------------------------------------ one strip
bool RenderEfxThumb(AssetCache& cache, const std::wstring& path, Image& strip, std::string& err, int size, const std::vector<double>& times) {
    std::shared_ptr<const EfpFile> file = cache.Efp(path, &err);
    if (!file) {
        if (err.empty()) err = "not found";
        return false;
    }
    const V3 place{0, 9, 0};
    // pass 1: the particle bounds over the frame times (same seed as pass 2: identical simulation)
    V3 mn, mx;
    bool any = false;
    {
        EfpEffect fx(file, 1.0, 255, 7);
        int step = 0;
        for (double t : times) {
            const int n = std::max(1, static_cast<int>(std::nearbyint(t * 20)));
            while (step < n) {
                fx.SetWorld(place, nullptr);
                if (fx.Alive()) fx.Update();
                ++step;
            }
            V3 a, b;
            if (fx.Alive() && fx.Bounds(a, b) && Finite(a) && Finite(b)) {
                if (!any) {
                    mn = a;
                    mx = b;
                } else {
                    mn = {std::min(mn.x, a.x), std::min(mn.y, a.y), std::min(mn.z, a.z)};
                    mx = {std::max(mx.x, b.x), std::max(mx.y, b.y), std::max(mx.z, b.z)};
                }
                any = true;
            }
        }
    }
    V3 at = place;
    double radius = 12;
    if (any) {
        at = (mn + mx) * 0.5;
        radius = std::clamp(Len(mx - mn) * 0.5, 2.5, 600.0);
    }
    const V3 dir = Norm(V3{30, 18, -48});   // the RunEffectSheet viewing direction
    const double fov = 50.0;
    double dist = radius / std::sin(fov * 0.5 * kPi / 180) * 0.92;
    const int nf = static_cast<int>(times.size());
    // draws every frame time with one camera (a fresh simulation: same seed = same particles)
    auto drawAll = [&](const V3& camAt, double camDist, std::vector<Image>& out, RCamera* used) {
        out.assign(static_cast<std::size_t>(nf), Image{});
        EfpEffect fx(file, 1.0, 255, 7);
        SoftRaster sr;
        int step = 0;
        for (int fi = 0; fi < nf; ++fi) {
            const int n = std::max(1, static_cast<int>(std::nearbyint(times[static_cast<std::size_t>(fi)] * 20)));
            while (step < n) {
                fx.SetWorld(place, nullptr);
                if (fx.Alive()) fx.Update();
                ++step;
            }
            RFrame f;
            f.cam.W = size;
            f.cam.H = size;
            f.cam.fovDeg = fov;
            f.cam.at = camAt;
            f.cam.eye = camAt + dir * camDist;
            f.cam.Setup();
            if (used) *used = f.cam;
            if (fx.Alive()) DrawEffect(fx, cache, f);
            sr.Draw(f, out[static_cast<std::size_t>(fi)], 1);
        }
    };
    std::vector<Image> imgs;
    RCamera cam;
    drawAll(at, dist, imgs, &cam);
    // particle bounds are loose (faded / tiny far particles): zoom on the lit pixels of all frames
    int x0 = size, y0 = size, x1 = -1, y1 = -1;
    for (const auto& im : imgs) {
        if (im.Empty() || im.w != size || im.h != size) continue;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const std::uint8_t* q = im.Px(x, y);
                if (std::abs(q[0] - 18) + std::abs(q[1] - 20) + std::abs(q[2] - 26) < 36) continue;
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
    }
    if (x1 >= x0 && y1 >= y0) {
        const double k = std::max(x1 - x0 + 1, y1 - y0 + 1) / static_cast<double>(size);
        if (k < 0.70) {
            const double cx = ((x0 + x1) * 0.5 + 0.5) / size * 2 - 1, cy = 1 - ((y0 + y1) * 0.5 + 0.5) / size * 2;
            const double half = dist * std::tan(fov * 0.5 * kPi / 180);
            at = at + cam.x * (cx * half) + cam.y * (cy * half);
            dist *= std::max(k / 0.82, 0.12);
            drawAll(at, dist, imgs, nullptr);
        }
    }
    strip.w = size * nf;
    strip.h = size;
    strip.rgba.assign(static_cast<std::size_t>(strip.w) * strip.h * 4, 255);
    for (int fi = 0; fi < nf; ++fi) {
        const Image& img = imgs[static_cast<std::size_t>(fi)];
        if (img.Empty() || img.w != size || img.h != size) continue;
        for (int y = 0; y < size; ++y)
            std::memcpy(&strip.rgba[(static_cast<std::size_t>(y) * strip.w + static_cast<std::size_t>(fi) * size) * 4], &img.rgba[static_cast<std::size_t>(y) * size * 4],
                        static_cast<std::size_t>(size) * 4);
    }
    return true;
}

void BrokenThumb(Image& strip, int size) {
    strip.w = size * kThumbFrames;
    strip.h = size;
    strip.rgba.assign(static_cast<std::size_t>(strip.w) * strip.h * 4, 255);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < strip.w; ++x) {
            std::uint8_t* p = strip.Px(x, y);
            const int lx = x % size;
            const bool cross = (std::abs(lx - y) <= 2 || std::abs(lx - (size - 1 - y)) <= 2) && lx > size / 4 && lx < size * 3 / 4 && y > size / 4 &&
                               y < size * 3 / 4;
            const bool border = lx < 3 || lx >= size - 3 || y < 3 || y >= size - 3;
            p[0] = cross ? 255 : border ? 248 : 120;
            p[1] = cross ? 255 : border ? 81 : 22;
            p[2] = cross ? 255 : border ? 73 : 28;
            p[3] = 255;
        }
}

// ------------------------------------------------------------------ tags
const std::vector<EfxTag>& EfxTagList() {
    static const std::vector<EfxTag> k = {
        {L"fire", L"ateş", L"fire"},          {L"ice", L"buz", L"ice"},         {L"lightning", L"yıldırım", L"lightning"},
        {L"poison", L"zehir", L"poison"},     {L"light", L"ışık", L"light"},     {L"dark", L"karanlık", L"dark"},
        {L"wind", L"rüzgâr", L"wind"},        {L"earth", L"toprak", L"earth"},   {L"water", L"su", L"water"},
        {L"heal", L"iyileştirme", L"heal"},   {L"hit", L"isabet", L"hit"},       {L"cast", L"hazırlık", L"cast"},
        {L"arrow", L"ok / mermi", L"arrow / missile"}, {L"aura", L"aura / güçlendirme", L"aura / buff"},
    };
    return k;
}

std::vector<int> EfxTagsOf(const std::wstring& path) {
    // keywords per tag (token == keyword, or the token starts with a keyword of 4+ letters)
    static const std::vector<std::vector<const wchar_t*>> kw = {
        {L"fire", L"flame", L"burn", L"blaze", L"hwa", L"explosion", L"explo"},
        {L"ice", L"frost", L"cold", L"snow", L"freeze", L"frozen", L"icicle"},
        {L"lightning", L"thunder", L"electric", L"elec", L"bolt", L"spark"},
        {L"poison", L"venom", L"toxic", L"toxin"},
        {L"light", L"holy", L"shine", L"bless", L"divine", L"sun"},
        {L"dark", L"curse", L"shadow", L"evil", L"death", L"soul", L"ghost"},
        {L"wind", L"storm", L"tornado", L"gale", L"cyclone"},
        {L"earth", L"rock", L"stone", L"quake", L"ground", L"sand"},
        {L"water", L"wave", L"splash", L"bubble", L"aqua"},
        {L"heal", L"cure", L"recovery", L"recover", L"resurrection", L"revive", L"hp", L"mp"},
        {L"hit", L"damage", L"dmg", L"blood", L"impact", L"crash"},
        {L"cast", L"casting", L"ready", L"charge", L"keep"},
        {L"arrow", L"bow", L"missile", L"shot", L"bullet", L"dart"},
        {L"aura", L"buff", L"shield", L"guard", L"barrier", L"protect"},
    };
    std::wstring name = Lower(path);
    const std::size_t slash = name.find_last_of(L'\\');
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, L".efp") == 0) name.resize(name.size() - 4);
    std::vector<std::wstring> tok;
    std::wstring cur;
    for (wchar_t c : name + L"_") {
        if (iswalpha(c)) cur += c;
        else {
            if (!cur.empty()) tok.push_back(cur);
            cur.clear();
        }
    }
    std::vector<int> out;
    for (std::size_t t = 0; t < kw.size(); ++t) {
        bool hit = false;
        for (const auto& w : tok) {
            for (const wchar_t* k : kw[t]) {
                const std::size_t kl = std::wcslen(k);
                const bool eq = w == k;
                const bool pre = kl >= 4 && w.size() > kl && w.compare(0, kl, k) == 0;
                if (!eq && !pre) continue;
                if (EfxTagList()[t].id == std::wstring(L"light") && w.rfind(L"lightn", 0) == 0) continue;   // lightning is not light
                hit = true;
                break;
            }
            if (hit) break;
        }
        if (hit) out.push_back(static_cast<int>(t));
    }
    return out;
}

// ------------------------------------------------------------------ reverse index
void EfxUseIndex::Add(const std::wstring& efpPath, const std::wstring& key) {
    auto& v = m_keys[Lower(efpPath)];
    for (const auto& k : v)
        if (_wcsicmp(k.c_str(), key.c_str()) == 0) return;
    v.push_back(key);
}

const std::vector<std::wstring>* EfxUseIndex::KeysOf(const std::wstring& efpPath) const {
    auto it = m_keys.find(Lower(efpPath));
    return it == m_keys.end() ? nullptr : &it->second;
}

// ------------------------------------------------------------------ background thumbnails
EfxThumbs::~EfxThumbs() { Stop(); }

void EfxThumbs::Stop() {
    m_cancel = true;
    if (m_boss.joinable()) m_boss.join();
    m_running = false;
}

void EfxThumbs::Start(OpenFn open, std::vector<std::wstring> paths, const Options& o) {
    Stop();
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_opt = o;
        m_paths = std::move(paths);
        m_items.assign(m_paths.size(), Item{});
        m_taken.assign(m_paths.size(), 0);
        m_prio.clear();
        m_next = 0;
        m_index.clear();
        for (std::size_t i = 0; i < m_paths.size(); ++i) m_index[Lower(m_paths[i])] = i;
    }
    m_done = m_cached = m_rendered = m_broken = 0;
    m_cancel = false;
    m_running = true;
    m_t0 = Ticks();
    m_t1 = 0;
    if (!m_opt.cacheDir.empty()) MakeDirs(m_opt.cacheDir);
    {   // the PNG writer builds its CRC table lazily: build it here, before any worker thread exists
        Image one;
        one.w = one.h = 1;
        one.rgba.assign(4, 0);
        std::vector<std::uint8_t> png;
        EncodePng(one, png);
    }
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const int th = m_opt.threads > 0 ? m_opt.threads : static_cast<int>(std::min(4u, hw - 1));
    m_boss = std::thread([this, open, th] {
        std::shared_ptr<IAssetSource> src;
        try {
            src = open ? open() : nullptr;
        } catch (...) {
            src.reset();
        }
        if (src && !m_cancel) {
            std::vector<std::thread> ws;
            for (int i = 0; i < th; ++i) ws.emplace_back([this, &src] { Worker(&src); });
            for (auto& w : ws) w.join();
        } else if (!m_cancel) {
            std::lock_guard<std::mutex> lk(m_mx);
            for (auto& it : m_items)
                if (it.state == ThumbState::Pending) {
                    it.state = ThumbState::Missing;
                    it.err = "the client could not be read";
                }
        }
        if (!m_cancel) EnforceCap();
        m_t1 = Ticks();
        m_running = false;
        ++m_gen;
    });
}

bool EfxThumbs::Next(std::size_t& idx) {
    std::lock_guard<std::mutex> lk(m_mx);
    while (!m_prio.empty()) {
        idx = m_prio.back();
        m_prio.pop_back();
        if (idx < m_taken.size() && !m_taken[idx]) {
            m_taken[idx] = 1;
            return true;
        }
    }
    while (m_next < m_paths.size()) {
        idx = m_next++;
        if (!m_taken[idx]) {
            m_taken[idx] = 1;
            return true;
        }
    }
    return false;
}

void EfxThumbs::Prioritize(const std::vector<std::wstring>& paths) {
    std::lock_guard<std::mutex> lk(m_mx);
    m_prio.clear();
    for (auto it = paths.rbegin(); it != paths.rend(); ++it) {   // back() = the first visible cell
        auto f = m_index.find(Lower(*it));
        if (f != m_index.end() && !m_taken[f->second]) m_prio.push_back(f->second);
    }
}

void EfxThumbs::Finish(std::size_t idx, Item&& it) {
    if (it.state == ThumbState::Broken) ++m_broken;
    if (it.fromCache) ++m_cached;
    else if (it.state == ThumbState::Ready || it.state == ThumbState::Broken) ++m_rendered;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (idx < m_items.size()) m_items[idx] = std::move(it);
    }
    ++m_done;
    ++m_gen;
}

void EfxThumbs::Worker(std::shared_ptr<IAssetSource>* srcp) {
    IAssetSource* src = srcp->get();
    AssetCache cache(src);
    std::size_t idx = 0;
    int sinceTrim = 0;
    while (!m_cancel && Next(idx)) {
        const std::wstring path = m_paths[idx];
        Item it;
        std::vector<std::uint8_t> bytes;
        bool read = false;
        try {
            read = src->Read(Arc::Particles, path, bytes);
        } catch (...) {
            read = false;
        }
        if (!read) {
            it.state = ThumbState::Missing;
            it.err = "not found";
            Finish(idx, std::move(it));
            continue;
        }
        const std::wstring lp = Lower(path);
        std::wstring base;
        if (!m_opt.cacheDir.empty())
            base = m_opt.cacheDir + L"\\v2_" + Hex64(Fnv64(lp.data(), lp.size() * sizeof(wchar_t))) + L"_" + Hex64(Fnv64(bytes.data(), bytes.size())) + L"_" +
                   std::to_wstring(bytes.size());
        if (!base.empty() && FileThere(base + L".png")) {
            it.state = ThumbState::Ready;
            it.file = base + L".png";
            it.fromCache = true;
            Finish(idx, std::move(it));
            continue;
        }
        if (!base.empty() && FileThere(base + L"_x.png")) {
            it.state = ThumbState::Broken;
            it.file = base + L"_x.png";
            it.fromCache = true;
            it.err = "the file does not parse";
            Finish(idx, std::move(it));
            continue;
        }
        if (!m_opt.render) {   // lookup only: stays pending
            std::lock_guard<std::mutex> lk(m_mx);
            ++m_done;
            continue;
        }
        Image strip;
        std::string err;
        bool ok = false;
        try {
            ok = RenderEfxThumb(cache, path, strip, err);
        } catch (...) {
            ok = false;
            err = "exception while drawing";
        }
        if (!ok) BrokenThumb(strip);
        it.state = ok ? ThumbState::Ready : ThumbState::Broken;
        it.err = ok ? std::string() : err;
        bool stored = false;
        if (!base.empty()) {
            std::string we;
            const std::wstring f = base + (ok ? L".png" : L"_x.png");
            if (WritePng(f, strip, we, true)) {   // atomic (<f>.tmp + MoveFileEx)
                it.file = f;
                stored = true;
            }
        }
        if (!stored) it.mem = std::move(strip);
        Finish(idx, std::move(it));
        if (++sinceTrim >= 150) {
            cache.Trim(48u << 20);
            sinceTrim = 0;
        }
    }
}

void EfxThumbs::EnforceCap() {
    if (m_opt.cacheDir.empty() || !m_opt.capBytes) return;
    // the cap covers every client's folder under the thumbs root (oldest files go first, down to 80 %)
    std::wstring root = m_opt.cacheDir;
    const std::size_t cut = root.find_last_of(L'\\');
    if (cut != std::wstring::npos) root.resize(cut);
    struct F {
        std::wstring path;
        std::uint64_t size, time;
    };
    std::vector<F> files;
    std::uint64_t total = 0;
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &d);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> dirs;
    do {
        if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && d.cFileName[0] != L'.') dirs.push_back(root + L"\\" + d.cFileName);
    } while (FindNextFileW(h, &d));
    FindClose(h);
    for (const auto& dir : dirs) {
        HANDLE f = FindFirstFileW((dir + L"\\*.png").c_str(), &d);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::uint64_t sz = (static_cast<std::uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
            const std::uint64_t t = (static_cast<std::uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
            files.push_back({dir + L"\\" + d.cFileName, sz, t});
            total += sz;
        } while (FindNextFileW(f, &d));
        FindClose(f);
    }
    if (total <= m_opt.capBytes) return;
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.time < b.time; });
    const std::uint64_t goal = m_opt.capBytes / 10 * 8;
    for (const auto& f : files) {
        if (total <= goal) break;
        if (DeleteFileW(f.path.c_str())) total -= f.size;
    }
}

ThumbState EfxThumbs::State(const std::wstring& path) const {
    std::lock_guard<std::mutex> lk(m_mx);
    auto f = m_index.find(Lower(path));
    return f == m_index.end() ? ThumbState::Missing : m_items[f->second].state;
}

std::string EfxThumbs::Error(const std::wstring& path) const {
    std::lock_guard<std::mutex> lk(m_mx);
    auto f = m_index.find(Lower(path));
    return f == m_index.end() ? std::string() : m_items[f->second].err;
}

bool EfxThumbs::Strip(const std::wstring& path, Image& out) const {
    std::wstring file;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        auto f = m_index.find(Lower(path));
        if (f == m_index.end()) return false;
        const Item& it = m_items[f->second];
        if (it.state != ThumbState::Ready && it.state != ThumbState::Broken) return false;
        if (!it.mem.Empty()) {
            out = it.mem;
            return true;
        }
        file = it.file;
    }
    std::string err;
    return !file.empty() && ReadPng(file, out, err) && !out.Empty();
}

EfxThumbs::Progress EfxThumbs::Prog() const {
    Progress p;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        p.total = static_cast<int>(m_paths.size());
    }
    p.done = m_done;
    p.cached = m_cached;
    p.rendered = m_rendered;
    p.broken = m_broken;
    p.running = m_running;
    const std::uint64_t t0 = m_t0, t1 = m_t1;
    const std::uint64_t end = t1 ? t1 : Ticks();
    p.seconds = t0 ? static_cast<double>(end - t0) / 1000.0 : 0.0;
    return p;
}

}  // namespace se::render
