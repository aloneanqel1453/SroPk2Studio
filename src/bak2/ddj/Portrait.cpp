// SkillEffect Studio v6 - ddj/Portrait.cpp  (M38) - headless portraits + batch gallery. See Portrait.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Portrait.h"
#include "DdjCodec.h"
#include "../render/RScene.h"
#include "../render/SoftRaster.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <thread>

namespace se::ddj {
namespace {

namespace R = se::render;

void Why(std::wstring* w, const std::wstring& s) {
    if (w) *w = s;
}

std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(std::max(0, n)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Html(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        switch (c) {
            case L'&': o += L"&amp;"; break;
            case L'<': o += L"&lt;"; break;
            case L'>': o += L"&gt;"; break;
            case L'"': o += L"&quot;"; break;
            case L'\'': o += L"&#39;"; break;
            default: o += c;
        }
    }
    return o;
}

std::wstring LowerW(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// Black-only probe render: bounding box of non-background pixels (fractions of the probe size)
bool Probe(R::Scene& sc, int P, int threads, double& cx, double& cy, double& ext) {
    sc.camera.W = P;
    sc.camera.H = P;
    R::RFrame f;
    sc.BuildFrame(f);
    f.bgGradient = false;
    f.bg[0] = f.bg[1] = f.bg[2] = 0;
    f.lines.clear();
    f.labels.clear();
    R::SoftRaster sr;
    R::Image img;
    sr.Draw(f, img, threads);
    int x0 = P, y0 = P, x1 = -1, y1 = -1;
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
            const std::uint8_t* p = img.Px(x, y);
            if (p[0] | p[1] | p[2]) {
                x0 = std::min(x0, x); x1 = std::max(x1, x);
                y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
        }
    if (x1 < 0) return false;
    cx = (x0 + x1 + 1) * 0.5 / P;
    cy = (y0 + y1 + 1) * 0.5 / P;
    ext = std::max(x1 - x0 + 1, y1 - y0 + 1) / static_cast<double>(P);
    return true;
}

// pan the camera so the probe box centre is in the middle, then dolly so it fills `target` of the view
void Refit(R::Scene& sc, double cx, double cy, double ext, double target) {
    R::RCamera& c = sc.camera;
    c.Setup();
    const double d = R::Len(c.eye - c.at);
    const double half = std::tan(c.fovDeg * 3.14159265358979 / 360.0);
    const double wx = (cx - 0.5) * 2 * d * half, wy = (cy - 0.5) * 2 * d * half;
    const R::V3 pan = c.x * wx - c.y * wy;
    c.at = c.at + pan;
    c.eye = c.eye + pan;
    const double nd = std::max(0.5, d * std::clamp(ext / target, 0.15, 4.0));
    c.eye = c.at + R::Norm(c.eye - c.at) * nd;
    c.Setup();
}

bool LoadScene(R::Scene& sc, const PortraitJob& job, const PortraitOptions& o, std::wstring* why) {
    if (job.model.empty()) { Why(why, L"modeli yok (istemcide model dosyası tanımlı değil)"); return false; }
    sc.opt.grid = false;
    sc.opt.human = false;
    sc.opt.bones = false;
    sc.opt.effects = o.palette;
    sc.opt.models = true;
    sc.opt.studio = false;
    sc.opt.shadow = o.shadow;
    R::ModelPlace mp;
    mp.scale = job.scale > 0 ? job.scale : 1.0;
    mp.materialSet = job.materialSet;
    mp.idle = true;
    mp.palette = o.palette;
    std::wstring err;
    const int id = sc.AddModel(job.model, mp, &err);
    if (id <= 0) { Why(why, L"model yüklenemedi: " + (err.empty() ? job.model : err)); return false; }
    R::ModelInstance* mi = sc.Model(id);
    if (!mi || !mi->Def() || mi->Def()->parts.empty()) { Why(why, L"modelde görünen parça yok: " + job.model); return false; }
    if (o.poseMs > 0) sc.Advance(o.poseMs);
    return true;
}

}  // namespace

std::wstring SafeFileStem(const std::wstring& code) {
    std::wstring s;
    for (wchar_t c : code) {
        c = static_cast<wchar_t>(std::towlower(c));
        s += (c < 0x80 && (std::iswalnum(c) || c == L'_' || c == L'-')) ? c : L'_';
    }
    if (s.empty()) s = L"obj";
    return s.substr(0, 120);
}

bool PortraitRenderer::Render(const PortraitJob& job, const PortraitOptions& o, Image& out, std::wstring* why) {
    const int size = std::clamp(o.size, 16, 2048);
    const int ss = std::clamp(o.ssaa, 1, 3);
    const int th = std::clamp(o.rasterThreads, 1, 8);
    R::Scene sc(&m_cache);
    if (!LoadScene(sc, job, o, why)) return false;
    sc.camera.W = sc.camera.H = 128;
    sc.FrameModels(o.yawDeg, o.pitchDeg, o.distMul);
    const double target = std::clamp(1.0 - 2.0 * o.margin - 0.04, 0.3, 1.0);
    double cx, cy, ext;
    if (!Probe(sc, 128, th, cx, cy, ext)) { Why(why, L"model görünmüyor (boş görüntü): " + job.model); return false; }
    Refit(sc, cx, cy, ext, target);
    if (Probe(sc, 128, th, cx, cy, ext) && (ext > target + 0.06 || ext < target - 0.2)) Refit(sc, cx, cy, ext, target);
    // final: same frame over black and white -> alpha = 1 - (white - black) / 255, colour = black / alpha
    const int S = size * ss;
    sc.camera.W = S;
    sc.camera.H = S;
    R::RFrame f;
    sc.BuildFrame(f);
    f.bgGradient = false;
    f.lines.clear();
    f.labels.clear();
    R::SoftRaster sr;
    R::Image b, w;
    f.bg[0] = f.bg[1] = f.bg[2] = 0.0;
    sr.Draw(f, b, th);
    f.bg[0] = f.bg[1] = f.bg[2] = 1.0;
    sr.Draw(f, w, th);
    if (b.Empty() || w.w != b.w || w.h != b.h) { Why(why, L"çizim başarısız"); return false; }
    Image hi = MakeImage(b.w, b.h, 0);
    for (std::size_t i = 0; i < hi.rgba.size(); i += 4) {
        float a = 0;
        for (int k = 0; k < 3; ++k) a += 1.0f - (static_cast<float>(w.rgba[i + k]) - b.rgba[i + k]) / 255.0f;
        a = std::clamp(a / 3.0f, 0.0f, 1.0f);
        if (a < 0.5f / 255.0f) continue;
        for (int k = 0; k < 3; ++k) hi.rgba[i + k] = static_cast<std::uint8_t>(std::clamp(b.rgba[i + k] / a, 0.0f, 255.0f) + 0.5f);
        hi.rgba[i + 3] = static_cast<std::uint8_t>(a * 255.0f + 0.5f);
    }
    int x0, y0, x1, y1;
    if (!AlphaBounds(hi, x0, y0, x1, y1, 2)) { Why(why, L"model görünmüyor (boş görüntü): " + job.model); return false; }
    const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    const int side = std::max(1, static_cast<int>(std::ceil(std::max(bw, bh) / std::max(0.3, 1.0 - 2.0 * o.margin))));
    const int ox = x0 + bw / 2 - side / 2, oy = y0 + bh / 2 - side / 2;
    const Image crop = Crop(hi, ox, oy, side, side);
    out = Resample(crop, size, size, side >= size ? Filter::Box : Filter::Mitchell);
    if (!o.transparent) {
        Image bg = MakeImage(size, size, o.background | 0xFF000000u);
        BlitOver(bg, out, 0, 0);
        out = std::move(bg);
    }
    return true;
}

bool PortraitRenderer::RenderGif(const PortraitJob& job, const PortraitOptions& o, const std::wstring& path, int ms, int fps, std::wstring* why) {
    PortraitOptions g = o;
    g.poseMs = 0;
    R::Scene sc(&m_cache);
    if (!LoadScene(sc, job, g, why)) return false;
    sc.opt.studio = true;
    const int size = std::clamp(o.size, 32, 512);
    sc.camera.W = sc.camera.H = 128;
    sc.FrameModels(o.yawDeg, o.pitchDeg, o.distMul);
    double cx, cy, ext;
    if (Probe(sc, 128, 2, cx, cy, ext)) Refit(sc, cx, cy, ext, 0.8);
    const std::wstring tmp = path + L".tmp" + std::to_wstring(GetCurrentThreadId()) + L".gif";
    std::string err;
    if (!sc.RecordGif(tmp, size, size, std::clamp(ms, 100, 10000), std::clamp(fps, 5, 30), err)) {
        DeleteFileW(tmp.c_str());
        Why(why, L"GIF yazılamadı: " + std::wstring(err.begin(), err.end()));
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        Why(why, L"GIF yazılamadı: " + path);
        return false;
    }
    return true;
}

bool WriteGalleryHtml(const std::wstring& path, const std::vector<PortraitResult>& items, const std::wstring& title, bool en, std::wstring* why) {
    int ok = 0;
    for (const auto& r : items) ok += r.ok;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t date[64];
    swprintf_s(date, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    std::wstring h;
    h += L"<!doctype html>\n<html lang=\"" + std::wstring(en ? L"en" : L"tr") + L"\"><head><meta charset=\"utf-8\">"
         L"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>" + Html(title) + L"</title>\n<style>\n"
         L":root{--bg:#16181c;--card:#20232a;--line:#2f333c;--text:#e8e6e1;--muted:#9a9ca3;--accent:#e0a84a}\n"
         L"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.4 'Segoe UI',system-ui,sans-serif}\n"
         L"header{position:sticky;top:0;z-index:1;background:rgba(22,24,28,.96);border-bottom:1px solid var(--line);padding:14px 16px;display:flex;flex-wrap:wrap;gap:12px;align-items:center}\n"
         L"h1{font-size:18px;margin:0;color:var(--accent)}header p{margin:0;color:var(--muted)}\n"
         L"input{flex:1;min-width:180px;max-width:360px;background:var(--card);color:var(--text);border:1px solid var(--line);border-radius:6px;padding:7px 10px;font:inherit}\n"
         L"main{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:12px;padding:16px}\n"
         L"figure{margin:0;background:var(--card);border:1px solid var(--line);border-radius:8px;padding:8px;text-align:center}\n"
         L"figure img{width:100%;height:auto;aspect-ratio:1;background:repeating-conic-gradient(#262a31 0 25%,#1d2026 0 50%) 0 0/16px 16px;border-radius:4px}\n"
         L"figcaption{margin-top:6px;display:flex;flex-direction:column;gap:1px;word-break:break-word}figcaption b{font-weight:600}\n"
         L"figcaption span,figcaption em{color:var(--muted);font-size:12px;font-style:normal}\n"
         L"section{padding:0 16px 24px}h2{font-size:15px;color:var(--muted)}li{color:var(--muted)}li b{color:var(--text)}\n"
         L"</style></head><body>\n<header><h1>" + Html(title) + L"</h1><p>" + std::to_wstring(ok) + (en ? L" portraits · " : L" portre · ") + date +
         L"</p><input id=\"q\" type=\"search\" placeholder=\"" + (en ? L"Search: name, code, kind" : L"Ara: ad, kod, tür") + L"\" aria-label=\"search\"></header>\n<main id=\"g\">\n";
    for (const auto& r : items) {
        if (!r.ok) continue;
        h += L"<figure data-k=\"" + Html(LowerW(r.name + L" " + r.code + L" " + r.kindTr)) + L"\"><img src=\"" + Html(r.file) +
             L"\" loading=\"lazy\" alt=\"" + Html(r.name.empty() ? r.code : r.name) + L"\"><figcaption><b>" + Html(r.name.empty() ? r.code : r.name) +
             L"</b><span>" + Html(r.code) + L"</span><em>" + Html(r.kindTr) + (r.gif.empty() ? L"" : L" · <a href=\"" + Html(r.gif) + L"\">GIF</a>") +
             L"</em></figcaption></figure>\n";
    }
    h += L"</main>\n";
    std::wstring miss;
    int nm = 0;
    for (const auto& r : items)
        if (!r.ok) {
            ++nm;
            miss += L"<li><b>" + Html(r.code) + L"</b> " + Html(r.name) + L" — " + Html(r.whyTr) + L"</li>\n";
        }
    if (nm) h += L"<section><h2>" + std::wstring(en ? L"Without a portrait (" : L"Portresi çıkmayanlar (") + std::to_wstring(nm) + L")</h2><ul>\n" + miss + L"</ul></section>\n";
    h += L"<script>const q=document.getElementById('q'),c=[...document.querySelectorAll('#g figure')];"
         L"q.addEventListener('input',()=>{const t=q.value.trim().toLowerCase();for(const f of c)f.style.display=!t||f.dataset.k.includes(t)?'':'none'});</script>\n"
         L"</body></html>\n";
    const std::string u = U8(h);
    return WriteFileAtomic(path, Bytes(u.begin(), u.end()), why, false);
}

bool RunPortraitBatch(R::AssetCache& cache, const std::vector<PortraitJob>& jobs, const BatchOptions& o, BatchReport& rep, std::wstring* why) {
    rep = BatchReport{};
    const auto t0 = std::chrono::steady_clock::now();
    if (o.outDir.empty()) { Why(why, L"çıktı klasörü seçilmedi"); return false; }
    if (!IsSafeOutputPath(o.outDir + L"\\index.html", why)) return false;
    rep.total = static_cast<int>(jobs.size());
    rep.items.resize(jobs.size());
    // file names (unique, case-insensitive) + model de-duplication
    std::map<std::wstring, int> used;
    std::map<std::wstring, std::size_t> firstOf;   // model|material -> first job index
    std::vector<std::size_t> work;
    std::vector<std::pair<std::size_t, std::size_t>> copies;   // (job, source job)
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const PortraitJob& j = jobs[i];
        PortraitResult& r = rep.items[i];
        r.code = j.code;
        r.name = j.name;
        r.kindTr = j.kindTr;
        r.model = j.model;
        std::wstring stem = SafeFileStem(j.code);
        if (int& n = used[stem]; n++ > 0) stem += L"_" + std::to_wstring(n);
        r.file = stem + L".png";
        if (j.model.empty() || !j.skipTr.empty()) {
            r.whyTr = !j.skipTr.empty() ? j.skipTr : o.english ? L"no model (the client defines no model file)" : L"modeli yok (istemcide model dosyası tanımlı değil)";
            r.model.clear();
            continue;
        }
        if (o.skipExisting && GetFileAttributesW((o.outDir + L"\\" + r.file).c_str()) != INVALID_FILE_ATTRIBUTES) {
            r.ok = true;
            ++rep.skipped;
            continue;
        }
        const std::wstring key = LowerW(j.model) + L"|" + std::to_wstring(j.materialSet);
        auto it = firstOf.find(key);
        if (it == firstOf.end()) {
            firstOf[key] = i;
            work.push_back(i);
        } else {
            copies.push_back({i, it->second});
        }
    }
    std::atomic<std::size_t> next{0};
    std::atomic<int> done{rep.skipped};
    std::mutex mx;
    const int nw = std::clamp(o.workers > 0 ? o.workers : static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 4);
    PortraitOptions po = o.portrait;
    po.rasterThreads = std::max(1, std::min(po.rasterThreads, 8 / nw));
    auto worker = [&] {
        PortraitRenderer pr(cache);
        for (;;) {
            if (o.cancel && o.cancel->load()) return;
            const std::size_t k = next++;
            if (k >= work.size()) return;
            const std::size_t i = work[k];
            PortraitResult& r = rep.items[i];
            const auto s0 = std::chrono::steady_clock::now();
            Image img;
            std::wstring w;
            if (pr.Render(jobs[i], po, img, &w)) {
                Bytes png;
                if (EncodePngRgba(img, png) && WriteFileAtomic(o.outDir + L"\\" + r.file, png, &w, false)) {
                    r.ok = true;
                    std::size_t opaque = 0;
                    for (std::size_t q = 3; q < img.rgba.size(); q += 4) opaque += img.rgba[q] > 127;
                    r.coverage = static_cast<float>(opaque) / (static_cast<float>(img.rgba.size()) / 4);
                } else {
                    r.whyTr = w.empty() ? L"PNG yazılamadı" : w;
                }
                if (r.ok && o.gifMs > 0) {
                    const std::wstring g = r.file.substr(0, r.file.size() - 4) + L".gif";
                    std::wstring gw;
                    if (pr.RenderGif(jobs[i], po, o.outDir + L"\\" + g, o.gifMs, 12, &gw)) r.gif = g;
                }
            } else {
                r.whyTr = w;
            }
            r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
            if (cache.ApproxBytes() > (std::size_t{1536} << 20)) cache.Trim(std::size_t{768} << 20);
            const int d = ++done;
            if (o.progress) {
                std::lock_guard<std::mutex> lk(mx);
                o.progress(d, rep.total, r.code);
            }
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < nw; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    for (const auto& [i, src] : copies) {
        PortraitResult& r = rep.items[i];
        const PortraitResult& s = rep.items[src];
        if (s.ok && !s.file.empty() &&
            CopyFileW((o.outDir + L"\\" + s.file).c_str(), (o.outDir + L"\\" + r.file).c_str(), FALSE)) {
            r.ok = true;
            r.reused = true;
            r.coverage = s.coverage;
        } else {
            r.whyTr = s.whyTr.empty() ? L"aynı modelin portresi çıkmadı" : s.whyTr;
        }
        const int d = ++done;
        if (o.progress) o.progress(d, rep.total, r.code);
    }
    for (auto& r : rep.items) {
        if (r.ok) {
            ++rep.ok;
            rep.reused += r.reused;
        } else {
            if (r.model.empty()) ++rep.noModel;
            else ++rep.failed;
            r.file.clear();
        }
    }
    rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (o.cancel && o.cancel->load()) { Why(why, L"iptal edildi"); }
    if (o.html) {
        const std::wstring title = !o.title.empty() ? o.title : (o.english ? L"Portrait gallery" : L"Portre galerisi");
        rep.indexHtml = o.outDir + L"\\index.html";
        if (!WriteGalleryHtml(rep.indexHtml, rep.items, title, o.english, why)) return false;
        std::wstring t = title + L"\r\n";
        t += (o.english ? L"total " : L"toplam ") + std::to_wstring(rep.total) + L", ok " + std::to_wstring(rep.ok) +
             (o.english ? L" (reused model " : L" (aynı model kopyası ") + std::to_wstring(rep.reused) + L"), " +
             (o.english ? L"no model " : L"modelsiz ") + std::to_wstring(rep.noModel) + L", " + (o.english ? L"failed " : L"başarısız ") +
             std::to_wstring(rep.failed) + L", " + std::to_wstring(static_cast<int>(rep.seconds + 0.5)) + (o.english ? L" s\r\n" : L" sn\r\n");
        for (const auto& r : rep.items)
            if (!r.ok) t += r.code + L"\t" + r.name + L"\t" + r.model + L"\t" + r.whyTr + L"\r\n";
        const std::string u = "\xEF\xBB\xBF" + U8(t);
        rep.reportTxt = o.outDir + L"\\report.txt";
        if (!WriteFileAtomic(rep.reportTxt, Bytes(u.begin(), u.end()), why, false)) return false;
    }
    return !(o.cancel && o.cancel->load());
}

}  // namespace se::ddj
