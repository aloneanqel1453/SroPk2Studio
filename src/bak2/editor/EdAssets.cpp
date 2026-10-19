// SkillEffect Studio - editor/EdAssets.cpp
#include "EdAssets.h"

#include "EdCompare.h"
#include "EdVorbis.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmsystem.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <cwctype>
#include <mutex>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;

namespace se::ed {

namespace {

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::uint32_t RdU32(const Bytes& b, size_t o) {
    return o + 4 <= b.size() ? static_cast<std::uint32_t>(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24)) : 0;
}
std::uint16_t RdU16(const Bytes& b, size_t o) { return o + 2 <= b.size() ? static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8)) : 0; }

// bounded little-endian reader (strings: u32 length + CP949 bytes)
struct R {
    const Bytes& b;
    size_t p = 0;
    bool bad = false;
    explicit R(const Bytes& d, size_t at = 0) : b(d), p(at) {}
    bool Need(size_t n) {
        if (bad || p + n > b.size()) bad = true;
        return !bad;
    }
    std::uint32_t U32() {
        if (!Need(4)) return 0;
        const std::uint32_t v = RdU32(b, p);
        p += 4;
        return v;
    }
    std::int32_t I32() { return static_cast<std::int32_t>(U32()); }
    std::uint8_t U8() { return Need(1) ? b[p++] : 0; }
    void Skip(size_t n) {
        if (Need(n)) p += n;
    }
    std::wstring Str() {
        const std::uint32_t n = U32();
        if (n > 4096 || !Need(n)) {
            bad = true;
            return L"";
        }
        std::wstring s = FromAnsi(std::string_view(reinterpret_cast<const char*>(b.data() + p), n), 949);
        p += n;
        return s;
    }
};

// ------------------------------------------------------------------ DDS decode
void C565(std::uint16_t c, std::uint8_t* o) {  // -> b,g,r
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, bl = c & 31;
    o[2] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
    o[1] = static_cast<std::uint8_t>((g << 2) | (g >> 4));
    o[0] = static_cast<std::uint8_t>((bl << 3) | (bl >> 2));
}

void ColorBlock(const std::uint8_t* s, std::uint8_t pal[4][4], bool dxt1) {
    const std::uint16_t c0 = static_cast<std::uint16_t>(s[0] | (s[1] << 8)), c1 = static_cast<std::uint16_t>(s[2] | (s[3] << 8));
    C565(c0, pal[0]);
    C565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (c0 > c1 || !dxt1) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<std::uint8_t>((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = static_cast<std::uint8_t>((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) pal[2][k] = static_cast<std::uint8_t>((pal[0][k] + pal[1][k]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
}

bool DecodeDxt(const std::uint8_t* src, size_t avail, int w, int h, int kind, EdImg& out) {
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    const size_t bs = kind == 1 ? 8 : 16;
    if (static_cast<size_t>(bw) * bh * bs > avail) return false;
    out.w = w;
    out.h = h;
    out.bgra.assign(static_cast<size_t>(w) * h * 4, 0);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const std::uint8_t* blk = src + (static_cast<size_t>(by) * bw + bx) * bs;
            std::uint8_t alpha[16];
            std::fill(std::begin(alpha), std::end(alpha), static_cast<std::uint8_t>(255));
            const std::uint8_t* cb = blk;
            if (kind == 3) {
                for (int i = 0; i < 16; ++i) {
                    const int a = (blk[i / 2] >> ((i & 1) * 4)) & 15;
                    alpha[i] = static_cast<std::uint8_t>(a * 17);
                }
                cb = blk + 8;
            } else if (kind == 5) {
                const int a0 = blk[0], a1 = blk[1];
                int pal[8];
                pal[0] = a0;
                pal[1] = a1;
                if (a0 > a1)
                    for (int i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
                else {
                    for (int i = 1; i < 5; ++i) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
                    pal[6] = 0;
                    pal[7] = 255;
                }
                unsigned long long bits = 0;
                for (int i = 0; i < 6; ++i) bits |= static_cast<unsigned long long>(blk[2 + i]) << (8 * i);
                for (int i = 0; i < 16; ++i) alpha[i] = static_cast<std::uint8_t>(pal[(bits >> (3 * i)) & 7]);
                cb = blk + 8;
            }
            std::uint8_t pal[4][4];
            ColorBlock(cb, pal, kind == 1);
            const std::uint32_t idx = static_cast<std::uint32_t>(cb[4] | (cb[5] << 8) | (cb[6] << 16) | (static_cast<std::uint32_t>(cb[7]) << 24));
            for (int i = 0; i < 16; ++i) {
                const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                const int ci = (idx >> (2 * i)) & 3;
                std::uint8_t* o = &out.bgra[(static_cast<size_t>(y) * w + x) * 4];
                o[0] = pal[ci][0];
                o[1] = pal[ci][1];
                o[2] = pal[ci][2];
                o[3] = kind == 1 ? pal[ci][3] : alpha[i];
            }
        }
    return true;
}

std::uint8_t MaskTo8(std::uint32_t px, std::uint32_t mask, std::uint8_t def) {
    if (!mask) return def;
    const int sh = std::countr_zero(mask);
    const int bits = std::popcount(mask);
    const std::uint32_t v = (px & mask) >> sh;
    const std::uint32_t mx = bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    return static_cast<std::uint8_t>(mx ? v * 255u / mx : 0);
}

}  // namespace

bool DecodeDdjImage(const Bytes& b, EdImg& out, std::wstring* fmt) {
    out = EdImg{};
    size_t o = 0;
    if (b.size() > 20 && std::memcmp(b.data(), "JMXVDDJ", 7) == 0) o = 20;
    if (b.size() < o + 128 || std::memcmp(b.data() + o, "DDS ", 4) != 0) return false;
    const int h = static_cast<int>(RdU32(b, o + 12)), w = static_cast<int>(RdU32(b, o + 16));
    const std::uint32_t pfFlags = RdU32(b, o + 80), four = RdU32(b, o + 84), bpp = RdU32(b, o + 88);
    const std::uint32_t mr = RdU32(b, o + 92), mg = RdU32(b, o + 96), mb = RdU32(b, o + 100), ma = RdU32(b, o + 104);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
    const std::uint8_t* src = b.data() + o + 128;
    const size_t avail = b.size() - (o + 128);
    if (pfFlags & 4) {  // fourCC
        char cc[5] = {static_cast<char>(four & 255), static_cast<char>((four >> 8) & 255), static_cast<char>((four >> 16) & 255),
                      static_cast<char>(four >> 24), 0};
        int kind = 0;
        if (!std::strcmp(cc, "DXT1")) kind = 1;
        else if (!std::strcmp(cc, "DXT2") || !std::strcmp(cc, "DXT3")) kind = 3;
        else if (!std::strcmp(cc, "DXT4") || !std::strcmp(cc, "DXT5")) kind = 5;
        if (fmt) *fmt = Fmt(L"%hs %dx%d", cc, w, h);
        return kind && DecodeDxt(src, avail, w, h, kind, out);
    }
    const int bytes = static_cast<int>(bpp / 8);
    if (bytes < 1 || bytes > 4) return false;
    if (static_cast<size_t>(w) * h * bytes > avail) return false;
    out.w = w;
    out.h = h;
    out.bgra.resize(static_cast<size_t>(w) * h * 4);
    const bool lum = (pfFlags & 0x20000) != 0, alphaOnly = (pfFlags & 2) != 0 && !(pfFlags & 0x40);
    const bool hasA = (pfFlags & 1) != 0 || alphaOnly;
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        std::uint32_t px = 0;
        for (int k = 0; k < bytes; ++k) px |= static_cast<std::uint32_t>(src[i * bytes + k]) << (8 * k);
        std::uint8_t* d = &out.bgra[i * 4];
        if (lum) {
            const std::uint8_t l = MaskTo8(px, mr ? mr : 0xFF, 0);
            d[0] = d[1] = d[2] = l;
            d[3] = hasA ? MaskTo8(px, ma, 255) : 255;
        } else if (alphaOnly) {
            d[0] = d[1] = d[2] = 255;
            d[3] = MaskTo8(px, ma ? ma : 0xFF, 255);
        } else {
            d[2] = MaskTo8(px, mr, 0);
            d[1] = MaskTo8(px, mg, 0);
            d[0] = MaskTo8(px, mb, 0);
            d[3] = hasA ? MaskTo8(px, ma, 255) : 255;
        }
    }
    if (fmt) *fmt = Fmt(L"RGB%u %dx%d", bpp, w, h);
    return true;
}

bool SavePng(const std::wstring& path, const EdImg& img, std::wstring& err) {
    if (!img.Valid()) {
        err = L"boş resim / empty image";
        return false;
    }
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(hrInit);
    const std::wstring tmp = path + L".tmp_png";
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> f;
        ComPtr<IWICStream> s;
        ComPtr<IWICBitmapEncoder> e;
        ComPtr<IWICBitmapFrameEncode> fr;
        ComPtr<IPropertyBag2> pb;
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
            SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e)) && SUCCEEDED(e->Initialize(s.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(e->CreateNewFrame(&fr, &pb)) && SUCCEEDED(fr->Initialize(pb.Get())) &&
            SUCCEEDED(fr->SetSize(static_cast<UINT>(img.w), static_cast<UINT>(img.h))) && SUCCEEDED(fr->SetPixelFormat(&pf)) &&
            IsEqualGUID(pf, GUID_WICPixelFormat32bppBGRA) &&
            SUCCEEDED(fr->WritePixels(static_cast<UINT>(img.h), static_cast<UINT>(img.w * 4), static_cast<UINT>(img.bgra.size()),
                                      const_cast<BYTE*>(img.bgra.data()))) &&
            SUCCEEDED(fr->Commit()) && SUCCEEDED(e->Commit()))
            ok = true;
    }
    if (uninit) CoUninitialize();
    if (!ok) {
        DeleteFileW(tmp.c_str());
        err = L"PNG yazılamadı / cannot write PNG: " + path;
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        err = L"PNG taşınamadı / cannot move PNG: " + path;
        return false;
    }
    return true;
}

EdImg ContactSheet(const std::vector<EdImg>& imgs, int cell, int cols) {
    EdImg s;
    if (imgs.empty() || cell <= 0 || cols <= 0) return s;
    const int rows = static_cast<int>((imgs.size() + cols - 1) / cols);
    s.w = cols * (cell + 4) + 4;
    s.h = rows * (cell + 4) + 4;
    s.bgra.assign(static_cast<size_t>(s.w) * s.h * 4, 0);
    for (size_t i = 0; i < s.bgra.size(); i += 4) {  // dark checker so alpha reads
        const size_t px = i / 4;
        const int x = static_cast<int>(px % s.w), y = static_cast<int>(px / s.w);
        const std::uint8_t v = ((x / 8 + y / 8) & 1) ? 48 : 36;
        s.bgra[i] = s.bgra[i + 1] = s.bgra[i + 2] = v;
        s.bgra[i + 3] = 255;
    }
    for (size_t k = 0; k < imgs.size(); ++k) {
        const EdImg& im = imgs[k];
        if (!im.Valid()) continue;
        const int ox = 4 + static_cast<int>(k % cols) * (cell + 4), oy = 4 + static_cast<int>(k / cols) * (cell + 4);
        const double sc = std::min(static_cast<double>(cell) / im.w, static_cast<double>(cell) / im.h);
        const int tw = std::max(1, static_cast<int>(im.w * sc)), th = std::max(1, static_cast<int>(im.h * sc));
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {
                const int sx = std::min(im.w - 1, static_cast<int>(x / sc)), sy = std::min(im.h - 1, static_cast<int>(y / sc));
                const std::uint8_t* p = &im.bgra[(static_cast<size_t>(sy) * im.w + sx) * 4];
                std::uint8_t* d = &s.bgra[(static_cast<size_t>(oy + y) * s.w + ox + x) * 4];
                const int a = p[3];
                for (int c = 0; c < 3; ++c) d[c] = static_cast<std::uint8_t>((p[c] * a + d[c] * (255 - a)) / 255);
                d[3] = 255;
            }
    }
    return s;
}

// ------------------------------------------------------------------ asset lists

std::vector<AssetEntry> ListAssets(const ClientBase& c, Arc a, const std::wstring& dir, const std::wstring& ext, size_t cap,
                                   const std::atomic<bool>* cancel) {
    std::vector<std::wstring> files;
    WalkArchive(c, a, dir, files, ext.empty() ? cap : cap * 8, cancel);
    std::vector<AssetEntry> v;
    const std::wstring dot = L"." + ext;
    for (auto& f : files) {
        if (!ext.empty() && !EndsWithI(f, dot)) continue;
        AssetEntry e;
        e.arc = static_cast<int>(a);
        e.path = std::move(f);
        v.push_back(std::move(e));
        if (v.size() >= cap) break;
    }
    std::sort(v.begin(), v.end(), [](const AssetEntry& x, const AssetEntry& y) { return Lower(x.path) < Lower(y.path); });
    return v;
}

std::unordered_map<std::wstring, std::vector<std::wstring>> EfpUsers(const EdDoc& d, size_t cap) {
    std::unordered_map<std::wstring, std::vector<std::wstring>> m;
    auto add = [&](std::wstring p, const std::wstring& user) {
        if (p.empty() || p == L"none" || p == L"xxx") return;
        for (auto& ch : p)
            if (ch == L'/') ch = L'\\';
        p = Lower(p);
        auto& v = m[p];
        if (v.size() < cap && (v.empty() || v.back() != user)) v.push_back(user);
    };
    for (const auto& r : d.rows) {
        if (r.deleted) continue;
        auto cell = [&](size_t i) -> std::wstring { return i < r.cells.size() ? r.cells[i] : L""; };
        if (r.kind == TKind::SeAniSet) {
            for (size_t c : {13u, 14u, 20u, 21u, 23u})
                if (EndsWithI(cell(c), L".efp")) add(cell(c), cell(2));
        } else if (r.kind == TKind::SeEffectSet) {
            const std::wstring dir = cell(17), name = cell(18);
            if (EndsWithI(name, L".efp") && !StartsWith(ToUpper(dir), L"RES\\")) add((dir == L"none" ? L"" : dir) + name, cell(1));
        } else if (r.kind == TKind::SeCharInfo) {
            const std::wstring bt = cell(9);
            if (!bt.empty() && bt != L"none") add(L"hiteffect\\" + bt + (EndsWithI(bt, L".efp") ? L"" : L".efp"), Trim(cell(0)));
        }
    }
    return m;
}

void AttachUsers(std::vector<AssetEntry>& list, const std::unordered_map<std::wstring, std::vector<std::wstring>>& users) {
    for (auto& e : list) {
        auto it = users.find(Lower(e.path));
        if (it == users.end()) continue;
        e.users = it->second;
        e.uses = static_cast<int>(it->second.size());
    }
}

namespace {
std::mutex g_prevMu;
EfpPreviewFn g_prev;
}  // namespace

void SetEfpPreview(EfpPreviewFn f) {
    std::lock_guard<std::mutex> lk(g_prevMu);
    g_prev = std::move(f);
}
const EfpPreviewFn& GetEfpPreview() { return g_prev; }

ModelPreviewFn g_modelPrev;
void SetModelPreview(ModelPreviewFn f) {
    std::lock_guard<std::mutex> lk(g_prevMu);
    g_modelPrev = std::move(f);
}
const ModelPreviewFn& GetModelPreview() { return g_modelPrev; }

std::wstring EfpSignature(const Bytes& b) {
    if (b.size() < 12 || std::memcmp(b.data(), "JMXVEFF", 7) != 0) return L"";
    return FromAnsi(std::string_view(reinterpret_cast<const char*>(b.data()), 12), 1252);
}

std::vector<std::wstring> EfpResourceRefs(const Bytes& b, const wchar_t* ext) {
    std::vector<std::wstring> v;
    const std::string e = ToUtf8(ext);
    for (size_t i = 0; i + e.size() <= b.size(); ++i) {
        if (_strnicmp(reinterpret_cast<const char*>(b.data() + i), e.c_str(), e.size()) != 0) continue;
        size_t a = i;
        while (a > 0 && b[a - 1] >= 0x20 && b[a - 1] < 0x7F && i - a < 200) --a;
        std::wstring s = FromAnsi(std::string_view(reinterpret_cast<const char*>(b.data() + a), i + e.size() - a), 949);
        if (!s.empty() && std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
        i += e.size() - 1;
    }
    return v;
}

bool EfpFirstTexture(const ClientBase& c, const Bytes& efp, EdImg& out, std::wstring* path) {
    for (const auto& t : EfpResourceRefs(efp, L".ddj")) {
        for (const std::wstring& p : {t, L"textures\\" + FileNameOf(t), std::wstring(L"textures\\") + t}) {
            Bytes b;
            if (c.ReadResource(Arc::Particles, p, b) && DecodeDdjImage(b, out)) {
                if (path) *path = p;
                return true;
            }
        }
    }
    return false;
}

// ------------------------------------------------------------------ audio

AudioInfo ProbeAudio(const Bytes& b) {
    AudioInfo a;
    if (b.size() >= 12 && std::memcmp(b.data(), "RIFF", 4) == 0 && std::memcmp(b.data() + 8, "WAVE", 4) == 0) {
        size_t o = 12;
        std::uint32_t avg = 0, dataLen = 0;
        while (o + 8 <= b.size()) {
            const std::uint32_t len = RdU32(b, o + 4);
            if (std::memcmp(b.data() + o, "fmt ", 4) == 0 && o + 24 <= b.size()) {
                const std::uint16_t tag = RdU16(b, o + 8);
                a.channels = RdU16(b, o + 10);
                a.rate = static_cast<int>(RdU32(b, o + 12));
                avg = RdU32(b, o + 16);
                a.bits = RdU16(b, o + 22);
                switch (tag) {
                    case 1: a.codec = L"PCM"; break;
                    case 2: a.codec = L"MS-ADPCM"; break;
                    case 3: a.codec = L"Float"; break;
                    case 0x11: a.codec = L"IMA-ADPCM"; break;
                    case 0x55: a.codec = L"MP3"; break;
                    default: a.codec = Fmt(L"tag 0x%04X", tag); break;
                }
            } else if (std::memcmp(b.data() + o, "data", 4) == 0) {
                dataLen = len;
            }
            o += 8 + len + (len & 1);
        }
        a.ok = a.channels > 0 && a.rate > 0;
        if (avg) a.seconds = static_cast<double>(std::min<size_t>(dataLen, b.size())) / avg;
        if (!a.ok) a.err = L"WAV başlığı bozuk / bad WAV header";
        return a;
    }
    if (b.size() >= 64 && std::memcmp(b.data(), "OggS", 4) == 0) {
        // first page: identification header "\x01vorbis"
        for (size_t o = 0; o + 30 < std::min<size_t>(b.size(), 512); ++o)
            if (b[o] == 1 && std::memcmp(b.data() + o + 1, "vorbis", 6) == 0) {
                a.codec = L"Vorbis";
                a.channels = b[o + 11];
                a.rate = static_cast<int>(RdU32(b, o + 12));
                a.bits = 16;
                break;
            }
        // duration: granule position of the last page
        for (size_t o = b.size() - 14; o > 0 && a.rate > 0; --o)
            if (std::memcmp(b.data() + o, "OggS", 4) == 0) {
                unsigned long long g = 0;
                for (int k = 0; k < 8; ++k) g |= static_cast<unsigned long long>(b[o + 6 + k]) << (8 * k);
                if (g != ~0ull) a.seconds = static_cast<double>(g) / a.rate;
                break;
            }
        a.ok = a.channels > 0 && a.rate > 0;
        if (!a.ok) a.err = L"Ogg Vorbis başlığı yok / no Vorbis header";
        return a;
    }
    a.err = L"WAV / OGG değil / not WAV / OGG";
    return a;
}

bool DecodeAudio(const Bytes& b, bool ogg, std::vector<std::int16_t>& pcm, int& channels, int& rate, std::wstring& err) {
    pcm.clear();
    channels = rate = 0;
    // Ogg Vorbis: own decoder (Media Foundation has no Vorbis decoder for desktop apps on most PCs)
    if (ogg || (b.size() > 4 && std::memcmp(b.data(), "OggS", 4) == 0)) return DecodeOggVorbis(b, pcm, channels, rate, err);
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co = SUCCEEDED(hrCo);
    bool ok = false;
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        {
            ComPtr<IStream> st;
            st.Attach(SHCreateMemStream(b.data(), static_cast<UINT>(b.size())));
            ComPtr<IMFByteStream> bs;
            ComPtr<IMFSourceReader> rd;
            ComPtr<IMFMediaType> mt, outT;
            if (st && SUCCEEDED(MFCreateMFByteStreamOnStream(st.Get(), &bs))) {
                ComPtr<IMFAttributes> at;
                if (SUCCEEDED(bs.As(&at))) at->SetString(MF_BYTESTREAM_CONTENT_TYPE, ogg ? L"audio/ogg" : L"audio/wav");
                if (SUCCEEDED(MFCreateSourceReaderFromByteStream(bs.Get(), nullptr, &rd)) && SUCCEEDED(MFCreateMediaType(&mt))) {
                    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                    mt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                    mt->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
                    if (SUCCEEDED(rd->SetCurrentMediaType(stream, nullptr, mt.Get())) &&
                        SUCCEEDED(rd->GetCurrentMediaType(stream, &outT))) {
                        channels = static_cast<int>(MFGetAttributeUINT32(outT.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
                        rate = static_cast<int>(MFGetAttributeUINT32(outT.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0));
                        for (;;) {
                            DWORD flags = 0;
                            ComPtr<IMFSample> s;
                            if (FAILED(rd->ReadSample(stream, 0, nullptr, &flags, nullptr, &s))) break;
                            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
                            if (!s) continue;
                            ComPtr<IMFMediaBuffer> mb;
                            if (FAILED(s->ConvertToContiguousBuffer(&mb))) break;
                            BYTE* p = nullptr;
                            DWORD n = 0;
                            if (SUCCEEDED(mb->Lock(&p, nullptr, &n))) {
                                const size_t at0 = pcm.size();
                                pcm.resize(at0 + n / 2);
                                std::memcpy(pcm.data() + at0, p, (n / 2) * 2);
                                mb->Unlock();
                            }
                            if (pcm.size() > 120u * 1024 * 1024) break;  // ~4 min stereo 48k cap
                        }
                        ok = channels > 0 && rate > 0 && !pcm.empty();
                    }
                }
            }
        }
        MFShutdown();
    }
    if (co) CoUninitialize();
    if (!ok)
        err = ogg ? L"OGG çözülemedi: sistemde Vorbis çözücü yok (Windows 'Web Media Extensions'). / cannot decode OGG: no Vorbis decoder."
                  : L"Ses çözülemedi / cannot decode audio";
    return ok;
}

Bytes MakeWav(const std::vector<std::int16_t>& pcm, int ch, int rate) {
    Bytes w(44 + pcm.size() * 2);
    auto put32 = [&](size_t o, std::uint32_t v) {
        for (int k = 0; k < 4; ++k) w[o + k] = static_cast<std::uint8_t>(v >> (8 * k));
    };
    auto put16 = [&](size_t o, std::uint16_t v) {
        w[o] = static_cast<std::uint8_t>(v);
        w[o + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    std::memcpy(w.data(), "RIFF", 4);
    put32(4, static_cast<std::uint32_t>(36 + pcm.size() * 2));
    std::memcpy(w.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, static_cast<std::uint16_t>(ch));
    put32(24, static_cast<std::uint32_t>(rate));
    put32(28, static_cast<std::uint32_t>(rate * ch * 2));
    put16(32, static_cast<std::uint16_t>(ch * 2));
    put16(34, 16);
    std::memcpy(w.data() + 36, "data", 4);
    put32(40, static_cast<std::uint32_t>(pcm.size() * 2));
    if (!pcm.empty()) std::memcpy(w.data() + 44, pcm.data(), pcm.size() * 2);
    return w;
}

AudioPlayer::~AudioPlayer() { Stop(); }

void AudioPlayer::Stop() {
    PlaySoundW(nullptr, nullptr, 0);
    m_buf.clear();
}

bool AudioPlayer::Play(const Bytes& file, bool ogg, std::wstring& err) {
    Stop();
    if (ogg) {
        std::vector<std::int16_t> pcm;
        int ch = 0, rate = 0;
        if (!DecodeAudio(file, true, pcm, ch, rate, err)) return false;
        m_buf = MakeWav(pcm, ch, rate);
    } else {
        m_buf = file;  // PlaySound handles PCM and ACM codecs (ADPCM)
    }
    if (!PlaySoundW(reinterpret_cast<LPCWSTR>(m_buf.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT)) {
        err = L"Çalınamadı / cannot play";
        m_buf.clear();
        return false;
    }
    return true;
}

bool ReadMusic(const ClientBase& c, const std::wstring& inner, Bytes& out) {
    if (c.root.empty()) return false;
    const std::wstring p = FindNameI(c.root, L"Music.pk2", false);
    if (p.empty()) return false;
    Pk2Reader r;
    std::wstring err;
    return r.Open(p, c.key, err) && r.Read(inner, out, err);
}

std::vector<std::wstring> ListMusic(const ClientBase& c, size_t cap) {
    std::vector<std::wstring> v;
    if (c.root.empty()) return v;
    const std::wstring p = FindNameI(c.root, L"Music.pk2", false);
    Pk2Reader r;
    std::wstring err;
    if (p.empty() || !r.Open(p, c.key, err)) return v;
    std::vector<std::wstring> stack{L""};
    while (!stack.empty() && v.size() < cap) {
        const std::wstring d = stack.back();
        stack.pop_back();
        std::vector<std::pair<std::wstring, bool>> e;
        if (!r.List(d, e)) continue;
        for (const auto& [n, dir] : e) {
            if (n == L"." || n == L"..") continue;
            const std::wstring f = d.empty() ? n : d + L"\\" + n;
            if (dir) stack.push_back(f);
            else if (EndsWithI(f, L".ogg") || EndsWithI(f, L".wav")) v.push_back(f);
        }
    }
    std::sort(v.begin(), v.end());
    return v;
}

// ------------------------------------------------------------------ models

namespace {

// byte length of a ModData payload after its tag (sro_bsr._mod_payload_len); sounds/particles collected on the way
bool ModPayload(R& r, std::uint32_t tag, BsrInfo* info, const SndSet* proto) {
    r.Skip(28);  // IModData base
    switch (tag) {
        case 0x00000000: {
            r.U32();
            const std::uint32_t flags = r.U32();
            r.U32();
            const std::int32_t n = r.I32();
            if (n < 0 || n > 100000) return false;
            r.Skip(static_cast<size_t>(20) * n);
            if (flags & 4) {
                const std::int32_t m = r.I32();
                if (m < 0 || m > 100000) return false;
                r.Skip(static_cast<size_t>(8) * m);
            }
            r.Skip(36);
            break;
        }
        case 0x00010000: r.Skip(84); break;
        case 0x00010001:
        case 0x00010002:
            r.U32();
            r.Str();
            r.U32();
            break;
        case 0x00030000: {
            const std::int32_t n = r.I32();
            if (n < 0 || n > 10000) return false;
            for (int i = 0; i < n && !r.bad; ++i) {
                r.I32();
                const std::wstring a = r.Str(), b = r.Str();
                for (const auto& s : {a, b})
                    if (info && EndsWithI(s, L".efp")) info->particles.push_back(s);
                r.Skip(12);
                r.U32();
                r.Skip(3);
                if (r.U8() == 1) r.Skip(12);
            }
            break;
        }
        case 0x00040000: r.Skip(16); break;
        case 0x00040001: {
            r.Skip(40);
            const std::int32_t n = r.I32();
            if (n < 0 || n > 10000) return false;
            for (int i = 0; i < n && !r.bad; ++i)
                if (r.U8()) r.Str();
            break;
        }
        case 0x00050000: {
            const std::int32_t setn = r.I32();
            if (setn < 0 || setn > 1000) return false;
            if (setn > 0) {
                r.Skip(44);
                for (int s = 0; s < setn && !r.bad; ++s) {
                    SndSet ss = proto ? *proto : SndSet{};
                    ss.sndName = r.Str();
                    const std::int32_t n = r.I32();
                    if (n < 0 || n > 10000) return false;
                    ss.tracks = n;
                    for (int t = 0; t < n && !r.bad; ++t)
                        if (r.U32() == 1) {
                            r.Str();
                            r.I32();
                            r.Str();
                            ++ss.used;
                        }
                    if (info) {
                        info->maxTracks = std::max(info->maxTracks, ss.tracks);
                        info->sounds.push_back(std::move(ss));
                    }
                }
            }
            break;
        }
        case 0x00060000:
        case 0x00060001:
        case 0x00060002:
        case 0x00070000: break;
        default: return false;
    }
    return !r.bad;
}

}  // namespace

bool ParseBsrInfo(const Bytes& b, BsrInfo& o) {
    o = BsrInfo{};
    if (b.size() < 120 || std::memcmp(b.data(), "JMXVRES", 7) != 0) {
        o.err = L"JMXVRES değil / not a .bsr";
        return false;
    }
    std::uint32_t off[8];
    for (int i = 0; i < 8; ++i) off[i] = RdU32(b, 12 + 4 * static_cast<size_t>(i));
    const std::uint32_t primFlag = RdU32(b, 44);
    o.objType = RdU16(b, 64);
    {
        R r(b, 68);
        o.name = r.Str();
    }
    // 0 material, 1 mesh, 2 skeleton, 3 animation, 4 meshGroup, 5 aniGroup, 6 modPalette, 7 collision
    {
        R r(b, off[0]);
        const std::uint32_t n = r.U32();
        for (std::uint32_t i = 0; i < n && i < 64 && !r.bad; ++i) {
            r.U32();
            o.materials.push_back(r.Str());
        }
    }
    {
        R r(b, off[1]);
        const std::uint32_t n = r.U32();
        for (std::uint32_t i = 0; i < n && i < 256 && !r.bad; ++i) {
            o.meshes.push_back(r.Str());
            if (primFlag & 1) r.U32();
        }
    }
    {
        R r(b, off[3]);
        r.U32();
        r.U32();
        const std::uint32_t n = r.U32();
        for (std::uint32_t i = 0; i < n && i < 1024 && !r.bad; ++i) o.anis.push_back(r.Str());
    }
    {
        R r(b, off[2]);
        if (r.U32() == 1) {
            o.skeleton = r.Str();
            o.attachBone = r.Str();
        }
    }
    {
        R r(b, off[5]);
        const std::uint32_t ng = r.U32();
        for (std::uint32_t g = 0; g < ng && g < 64 && !r.bad; ++g) {
            const std::wstring gn = r.Str();
            const std::uint32_t k = r.U32();
            for (std::uint32_t i = 0; i < k && i < 1024 && !r.bad; ++i) {
                BsrRole role;
                role.group = gn;
                role.role = r.U32();
                role.roleName = AniRoleName(static_cast<int>(role.role));
                role.file = r.U32();
                const std::uint32_t ne = r.U32();
                for (std::uint32_t e = 0; e < ne && e < 256 && !r.bad; ++e)
                    role.events.push_back({r.U32(), r.U32(), r.U32(), r.U32()});
                const std::uint32_t wn = r.U32();
                r.U32();
                if (wn > 4096) r.bad = true;
                else r.Skip(static_cast<size_t>(wn) * 8);
                o.roles.push_back(std::move(role));
            }
        }
    }
    {
        R r(b, off[6]);
        for (int pal = 0; pal < 2 && !r.bad && off[6] && off[6] < b.size(); ++pal) {
            const std::int32_t ns = r.I32();
            if (ns < 0 || ns > 4096) break;
            for (int s = 0; s < ns && !r.bad; ++s) {
                SndSet proto;
                proto.palette = pal;
                proto.type = r.I32();
                proto.aniType = r.I32();
                proto.setName = r.Str();
                const std::int32_t n = r.I32();
                if (n < 0 || n > 4096) {
                    r.bad = true;
                    break;
                }
                for (int i = 0; i < n && !r.bad; ++i) {
                    const std::uint32_t tag = r.U32();
                    if (!ModPayload(r, tag, &o, &proto)) {
                        r.bad = true;
                        o.err = Fmt(L"ModData 0x%08X okunamadı / unreadable", tag);
                    }
                }
            }
        }
    }
    o.ok = true;
    return true;
}

bool ParseBanInfo(const Bytes& b, BanInfo& o) {
    if (b.size() < 32 || std::memcmp(b.data(), "JMXVBAN", 7) != 0) return false;
    R r(b, 12);
    r.U32();
    r.U32();
    o.name = r.Str();
    o.durationMs = r.U32();
    o.fps = r.U32();
    o.type = r.U32();
    o.keys = r.U32();
    if (o.keys > 100000) return false;
    r.Skip(static_cast<size_t>(o.keys) * 4);
    o.bones = r.U32();
    o.ok = !r.bad;
    return o.ok;
}

BsrInfo ModelAnimations(const ClientBase& c, const std::wstring& bsrInner, std::vector<BanInfo>& bans) {
    BsrInfo info;
    bans.clear();
    Bytes b;
    if (!c.ReadResource(Arc::Data, bsrInner, b)) {
        info.err = L"Data.pk2'de yok / not in Data.pk2: " + bsrInner;
        return info;
    }
    ParseBsrInfo(b, info);
    info.path = bsrInner;
    for (const auto& a : info.anis) {
        BanInfo bi;
        bi.path = a;
        Bytes bb;
        bi.found = c.ReadResource(Arc::Data, a, bb);
        if (bi.found) ParseBanInfo(bb, bi);
        bans.push_back(std::move(bi));
    }
    return info;
}

}  // namespace se::ed
