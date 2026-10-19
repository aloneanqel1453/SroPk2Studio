// SkillEffect Studio v6 - ddj/IconImport.cpp  (M33) - icon rules + import. See IconImport.h.
#include "IconImport.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>

namespace se::ddj {
namespace {

void Why(std::wstring* w, const std::wstring& s) {
    if (w) *w = s;
}

std::wstring Lower(std::wstring_view s) {
    std::wstring t(s);
    for (auto& c : t) {
        c = static_cast<wchar_t>(std::towlower(c));
        if (c == L'/') c = L'\\';
    }
    return t;
}

std::wstring FmtUpper(DdjFormat f) { return LayoutName(f); }

Image Nearest(const Image& s, int k) {
    Image o = MakeImage(s.w * k, s.h * k, 0);
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x) std::memcpy(o.Px(x, y), s.Px(x / k, y / k), 4);
    return o;
}

void Checker(Image& img, int x, int y, int w, int h) {
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            const bool a = ((i / 6) + (j / 6)) & 1;
            FillRect(img, x + i, y + j, 1, 1, a ? 0xFF4A4A4Au : 0xFF353535u);
        }
}

void Sharpen(Image& img, float amount) {
    if (amount <= 0 || img.w < 3 || img.h < 3) return;
    const Image src = img;
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
            std::uint8_t* o = img.Px(x, y);
            if (o[3] == 0) continue;
            for (int c = 0; c < 3; ++c) {
                float s = 0, wsum = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = std::clamp(x + dx, 0, img.w - 1), yy = std::clamp(y + dy, 0, img.h - 1);
                        const float w = (dx == 0 && dy == 0) ? 4.0f : (dx == 0 || dy == 0) ? 2.0f : 1.0f;
                        s += src.Px(xx, yy)[c] * w;
                        wsum += w;
                    }
                const float blur = s / wsum, v = src.Px(x, y)[c] + amount * (src.Px(x, y)[c] - blur);
                o[c] = static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
            }
        }
}

}  // namespace

// ------------------------------------------------------------------------------------------------ rules
IconRules IconRules::Defaults() {
    IconRules r;
    auto add = [&](const wchar_t* p, int w, int h, DdjFormat f, const wchar_t* alt) {
        IconRule x;
        x.prefix = p;
        x.w = w;
        x.h = h;
        x.format = f;
        x.altTr = alt;
        r.m_rules.push_back(x);
    };
    add(L"icon\\skill\\", 32, 32, DdjFormat::Argb8888, L"A1R5G5B5 32x32 (china)");
    add(L"icon\\item\\", 32, 32, DdjFormat::Argb8888, L"A1R5G5B5 32x32");
    add(L"icon\\item\\etc\\", 32, 32, DdjFormat::Argb1555, L"A8R8G8B8 32x32, kart resimleri 80x120");
    add(L"icon64\\", 64, 64, DdjFormat::Argb8888, L"45x45");
    add(L"icon\\skillgroup\\", 21, 21, DdjFormat::Argb1555, L"");
    add(L"icon\\skillmastery\\", 24, 24, DdjFormat::Argb1555, L"R5G6B5 24x24");
    add(L"icon\\action\\", 32, 32, DdjFormat::Argb1555, L"");
    add(L"icon\\premium\\", 32, 32, DdjFormat::Argb8888, L"");
    add(L"icon\\etc\\", 32, 32, DdjFormat::Argb8888, L"");
    r.m_generic.prefix = L"icon\\";
    r.m_generic.w = r.m_generic.h = 32;
    r.m_generic.format = DdjFormat::Argb8888;
    return r;
}

const IconRule& IconRules::Match(std::wstring_view path) const {
    const std::wstring p = Lower(path);
    const IconRule* best = nullptr;
    for (const auto& r : m_rules)
        if (p.rfind(r.prefix, 0) == 0 && (!best || r.prefix.size() > best->prefix.size())) best = &r;
    return best ? *best : m_generic;
}

std::wstring IconRules::Describe(bool en) const {
    std::wstring s;
    std::vector<const IconRule*> v;
    for (const auto& r : m_rules) v.push_back(&r);
    std::sort(v.begin(), v.end(), [](const IconRule* a, const IconRule* b) { return a->prefix < b->prefix; });
    for (const IconRule* r : v) {
        s += r->prefix + L" -> " + std::to_wstring(r->w) + L"x" + std::to_wstring(r->h) + L" " + FmtUpper(r->format);
        if (r->samples) s += en ? L" (" + std::to_wstring(r->samples) + L" files)" : L" (" + std::to_wstring(r->samples) + L" dosya)";
        if (!r->altTr.empty()) s += (en ? L"; also " : L"; ayrıca ") + r->altTr;
        s += L"\n";
    }
    return s;
}

void IconRuleLearner::Add(std::wstring_view mediaPath, const DdjInfo& info) {
    if (info.format == DdjFormat::Unknown) return;
    const std::wstring p = Lower(mediaPath);
    std::vector<std::wstring> parts;
    std::size_t a = 0;
    for (;;) {
        const auto b = p.find(L'\\', a);
        if (b == std::wstring::npos) break;
        parts.push_back(p.substr(a, b - a));
        a = b + 1;
    }
    if (parts.empty()) return;
    ++m_count;
    std::wstring pre;
    for (std::size_t d = 0; d < parts.size() && d < 3; ++d) {
        pre += parts[d] + L"\\";
        if (d == 0) continue;   // "icon\" alone is the generic rule
        auto it = std::find_if(m_dirs.begin(), m_dirs.end(), [&](const auto& e) { return e.first == pre; });
        if (it == m_dirs.end()) { m_dirs.push_back({pre, {}}); it = m_dirs.end() - 1; }
        const Key k{info.w, info.h, info.format};
        auto jt = std::find_if(it->second.begin(), it->second.end(), [&](const auto& e) { return !(e.first < k) && !(k < e.first); });
        if (jt == it->second.end()) it->second.push_back({k, 1});
        else ++jt->second;
    }
}

IconRules IconRuleLearner::Build(int minSamples) const {
    IconRules r = IconRules::Defaults();
    for (const auto& [dir, keys] : m_dirs) {
        int total = 0;
        for (const auto& k : keys) total += k.second;
        if (total < minSamples) continue;
        auto sorted = keys;
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        // size: the most common size; format: the most common format at that size
        IconRule x;
        x.prefix = dir;
        x.w = sorted[0].first.w;
        x.h = sorted[0].first.h;
        x.format = sorted[0].first.f;
        x.samples = total;
        if (sorted.size() > 1 && sorted[1].second * 5 >= total) {
            const auto& s = sorted[1];
            x.altTr = FmtUpper(s.first.f) + L" " + std::to_wstring(s.first.w) + L"x" + std::to_wstring(s.first.h) + L" %" +
                      std::to_wstring(s.second * 100 / total);
        }
        auto& rules = r.Rules();
        auto it = std::find_if(rules.begin(), rules.end(), [&](const IconRule& e) { return e.prefix == dir; });
        if (it != rules.end()) *it = x;
        else rules.push_back(x);
    }
    r.learned = m_count > 0;
    return r;
}

// ------------------------------------------------------------------------------------------------ paths
bool CheckIconPath(const std::wstring& p, std::wstring* why) {
    if (p.empty()) { Why(why, L"ikon yolu boş"); return false; }
    if (p.size() > 255) { Why(why, L"ikon yolu 255 karakterden uzun"); return false; }
    for (std::size_t i = 0; i < p.size(); ++i) {
        const wchar_t c = p[i];
        if (c >= 0x80 || c < 0x20 || std::wcschr(L":*?\"<>|/", c)) {
            std::wstring s = L"ikon yolunda istemcinin okuyamayacağı karakter var: '";
            s += c;
            s += L"' (" + std::to_wstring(i + 1) + L". karakter). Yalnız İngilizce harf, rakam, _ - . ve \\ kullanın.";
            Why(why, s);
            return false;
        }
    }
    if (p[0] == L'\\' || p.find(L"\\\\") != std::wstring::npos || p.find(L"..") != std::wstring::npos) { Why(why, L"ikon yolu biçimi geçersiz (\\\\, .. ya da başta \\)"); return false; }
    const std::wstring l = Lower(p);
    if (l.size() < 5 || l.compare(l.size() - 4, 4, L".ddj") != 0) { Why(why, L"ikon yolu .ddj ile bitmeli"); return false; }
    return true;
}

std::wstring SuggestIconPath(IconKind kind, const std::wstring& code, bool europe) {
    std::wstring n;
    for (wchar_t c : code) {
        c = static_cast<wchar_t>(std::towlower(c));
        n += (c < 0x80 && (std::iswalnum(c) || c == L'_' || c == L'-')) ? c : L'_';
    }
    if (n.empty()) n = L"icon";
    const wchar_t* dir = L"icon\\etc\\custom\\";
    switch (kind) {
        case IconKind::Skill: dir = europe ? L"icon\\skill\\europe\\custom\\" : L"icon\\skill\\china\\custom\\"; break;
        case IconKind::Item: dir = L"icon\\item\\custom\\"; break;
        case IconKind::Item64: dir = L"icon64\\item\\custom\\"; break;
        case IconKind::SkillGroup: dir = L"icon\\skillgroup\\custom\\"; break;
        case IconKind::SkillMastery: dir = L"icon\\skillmastery\\custom\\"; break;
        case IconKind::Action: dir = L"icon\\action\\"; break;
        default: break;
    }
    return dir + n + L".ddj";
}

std::wstring IconColumnValue(const std::wstring& mediaPath) {
    const std::wstring l = Lower(mediaPath);
    if (l.rfind(L"icon\\", 0) == 0) return mediaPath.substr(5);
    return mediaPath;
}

const wchar_t* IconColumnName(IconKind k) { return k == IconKind::Skill ? L"UI_IconFile" : L"AssocFileIcon128"; }
int IconColumnIndex(IconKind k) { return k == IconKind::Skill ? 61 : 54; }

// ------------------------------------------------------------------------------------------------ preview
Image PreviewSheet(const Image& source, const Image& game, const Image* frame) {
    const int gw = std::max(1, game.w), gh = std::max(1, game.h);
    const int pad = 12, srcBox = 96;
    const int fw = frame && !frame->Empty() ? std::max(frame->w, gw + 6) : gw + 8;
    const int fh = frame && !frame->Empty() ? std::max(frame->h, gh + 6) : gh + 8;
    const int H = pad * 2 + std::max({srcBox, fh, gh * 4});
    const int W = pad + srcBox + pad + fw + pad + gw * 2 + pad + gw * 4 + pad;
    Image s = MakeImage(W, H, 0xFF201C1Au);
    int x = pad;
    Checker(s, x, pad, srcBox, srcBox);
    if (!source.Empty()) {
        const Image f = FitInto(source, srcBox, srcBox, Fit::Contain, Filter::Mitchell);
        BlitOver(s, f, x, pad);
    }
    x += srcBox + pad;
    // slot (game size): client frame or a drawn bevel
    const int fy = pad + (H - 2 * pad - fh) / 2;
    if (frame && !frame->Empty()) {
        BlitOver(s, *frame, x + (fw - frame->w) / 2, fy + (fh - frame->h) / 2);
    } else {
        FillRect(s, x, fy, fw, fh, 0xFF5A5148u);
        FillRect(s, x + 1, fy + 1, fw - 2, fh - 2, 0xFF141210u);
        FillRect(s, x + 3, fy + 3, fw - 6, fh - 6, 0xFF2A2622u);
    }
    if (!game.Empty()) BlitOver(s, game, x + (fw - gw) / 2, fy + (fh - gh) / 2);
    x += fw + pad;
    for (int k : {2, 4}) {
        const int y = pad + (H - 2 * pad - gh * k) / 2;
        Checker(s, x, y, gw * k, gh * k);
        if (!game.Empty()) BlitOver(s, Nearest(game, k), x, y);
        x += gw * k + pad;
    }
    return s;
}

// ------------------------------------------------------------------------------------------------ import
bool ImportIcon(const Image& src, const std::wstring& target, const IconRules& rules, const IconImportOptions& o,
                IconImportResult& out, std::wstring* why, const Image* frame) {
    out = IconImportResult{};
    if (src.Empty()) { Why(why, L"kaynak resim boş"); return false; }
    if (!target.empty() && !CheckIconPath(target, why)) return false;
    out.rule = rules.Match(target);
    out.w = o.w > 0 ? o.w : out.rule.w;
    out.h = o.h > 0 ? o.h : out.rule.h;
    if (out.w > 1024 || out.h > 1024) { Why(why, L"ikon boyutu en çok 1024 x 1024"); return false; }
    out.format = o.format == DdjFormat::Auto ? out.rule.format : o.format;
    out.columnValue = target.empty() ? L"" : IconColumnValue(target);
    Image work = src;
    bool partialAlpha = false;
    {
        bool binary = true;
        const bool alpha = HasAlpha(src, &binary);
        partialAlpha = alpha && !binary;
        int x0, y0, x1, y1;
        if (o.trim && alpha && AlphaBounds(src, x0, y0, x1, y1, 8) && (x1 - x0 + 1 < src.w || y1 - y0 + 1 < src.h))
            work = Crop(src, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    }
    out.fitted = FitInto(work, out.w, out.h, o.fit, o.filter);
    Sharpen(out.fitted, std::clamp(o.sharpen, 0.0f, 1.0f));
    if (!FormatHasAlpha(out.format)) {
        Image bg = MakeImage(out.w, out.h, o.background | 0xFF000000u);
        BlitOver(bg, out.fitted, 0, 0);
        out.fitted = std::move(bg);
        out.notesTr.push_back(L"Biçimde saydamlık yok: saydam yerler arka plan rengiyle dolduruldu.");
    } else if (out.format == DdjFormat::Argb1555 && partialAlpha) {
        out.notesTr.push_back(L"A1R5G5B5 1 bitlik saydamlık taşır: yarı saydam kenarlar tam saydam / tam opak oldu (yumuşak kenar için A8R8G8B8 seçin).");
    }
    DdjOptions dop;
    dop.format = out.format;
    dop.mipLevels = 1;
    dop.jmxv = true;
    dop.dxt.quality = o.quality;
    if (!EncodeDdj(out.fitted, dop, out.ddj, why)) return false;
    std::string e;
    if (!se::render::DecodeDdj(out.ddj.data(), out.ddj.size(), out.game, e)) { Why(why, L"üretilen .ddj geri okunamadı"); return false; }
    out.psnr = Psnr(out.fitted, out.game, 0xF);
    out.preview = PreviewSheet(src, out.game, frame);
    std::wstring rl = L"Kural: " + out.rule.prefix + L" -> " + std::to_wstring(out.rule.w) + L"x" + std::to_wstring(out.rule.h) + L" " + FmtUpper(out.rule.format);
    rl += out.rule.samples ? L" (" + std::to_wstring(out.rule.samples) + L" sevkiyat dosyasından)" : L" (varsayılan)";
    out.notesTr.insert(out.notesTr.begin(), rl);
    out.notesTr.push_back(L"Kaynak " + std::to_wstring(src.w) + L"x" + std::to_wstring(src.h) + L" -> " + std::to_wstring(out.w) + L"x" +
                          std::to_wstring(out.h) + L" " + FmtUpper(out.format) + L", " + std::to_wstring(out.ddj.size()) + L" bayt.");
    if (!out.columnValue.empty()) out.notesTr.push_back(L"Tablo değeri: " + out.columnValue);
    return true;
}

}  // namespace se::ddj
