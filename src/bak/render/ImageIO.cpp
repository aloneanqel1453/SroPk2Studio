// ============================================================================
//  SRO Asset Studio - render/ImageIO.cpp
//
//  Image encode / decode through WIC (windowscodecs.lib), plus a hand written
//  TGA decoder because WIC does not ship one.
//
//  Everything converts to / from the RGBA8 Image type.
//
//  Threading: WIC needs COM. Startup()/Shutdown() are per thread and
//  reference counted, and every public entry point wraps itself in a
//  TransientScope so it also works when the caller forgot to call Startup().
//
//  Channel order: our Image is R,G,B,A. WIC's 32bppBGRA is B,G,R,A. Every
//  conversion in this file goes through SwapRB(), so nothing here can leave
//  red and blue transposed.
// ============================================================================
#include "ImageIO.h"
#include "../formats/Ddj.h"
#include "../formats/DdsDecode.h"

#include <wincodec.h>
#include <objbase.h>

namespace sro {
namespace ImageIO {

namespace {

// ---------------------------------------------------------------------------
// Tiny COM pointer. No ATL, no third party code.
// ---------------------------------------------------------------------------
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : m_p(o.m_p) { o.m_p = nullptr; }

    T** operator&()  { reset(); return &m_p; }
    T*  operator->() const { return m_p; }
    T*  get() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }
    void reset() { if (m_p) { m_p->Release(); m_p = nullptr; } }
private:
    T* m_p = nullptr;
};

// ---------------------------------------------------------------------------
// Per thread WIC state
// ---------------------------------------------------------------------------
struct WicThreadState {
    IWICImagingFactory* factory = nullptr;
    int  refCount = 0;
    bool ownsCom  = false;   ///< true when we must CoUninitialize on the way out
};

thread_local WicThreadState t_wic;

Result Fail(Status s, const wchar_t* msg) { return Result::Fail(s, std::wstring(msg)); }

/// Ensures WIC is up for the duration of a call, even if the caller never
/// called Startup() itself.
class TransientScope {
public:
    TransientScope() {
        if (t_wic.refCount > 0 && t_wic.factory) { m_owned = false; m_ok = true; return; }
        m_owned = true;
        m_ok = Startup().ok();
    }
    ~TransientScope() { if (m_owned && m_ok) Shutdown(); }
    bool ok() const { return m_ok && t_wic.factory != nullptr; }
    IWICImagingFactory* factory() const { return t_wic.factory; }
private:
    bool m_owned = false;
    bool m_ok    = false;
};

// ---------------------------------------------------------------------------
// Pixel helpers
// ---------------------------------------------------------------------------
/// In-place R <-> B swap over a tightly packed 4 byte per pixel buffer.
inline void SwapRB(u8* p, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i) std::swap(p[i * 4 + 0], p[i * 4 + 2]);
}

/// Human name of a WIC container GUID, for the readout line.
const wchar_t* ContainerName(const GUID& g) {
    if (g == GUID_ContainerFormatPng)  return L"PNG";
    if (g == GUID_ContainerFormatJpeg) return L"JPEG";
    if (g == GUID_ContainerFormatBmp)  return L"BMP";
    if (g == GUID_ContainerFormatGif)  return L"GIF";
    if (g == GUID_ContainerFormatTiff) return L"TIFF";
    if (g == GUID_ContainerFormatIco)  return L"ICO";
    if (g == GUID_ContainerFormatWmp)  return L"HD Photo";
    return L"WIC";
}

// ---------------------------------------------------------------------------
// Encode helper shared by PNG and JPEG
// ---------------------------------------------------------------------------
Result EncodeWith(const Image& img, const GUID& container, int jpegQuality,
                  std::vector<u8>& out) {
    out.clear();
    if (!img.valid()) return Fail(Status::InvalidArgument, L"Bos goruntu kodlanamaz.");

    TransientScope wic;
    if (!wic.ok()) return Fail(Status::Unsupported, L"WIC baslatilamadi.");

    // Our RGBA -> WIC's BGRA.
    std::vector<u8> bgra(img.storage());
    SwapRB(bgra.data(), static_cast<size_t>(img.width()) * img.height());

    ComPtr<IWICBitmap> bitmap;
    HRESULT hr = wic.factory()->CreateBitmapFromMemory(
        static_cast<UINT>(img.width()), static_cast<UINT>(img.height()),
        GUID_WICPixelFormat32bppBGRA,
        static_cast<UINT>(img.stride()),
        static_cast<UINT>(bgra.size()), bgra.data(), &bitmap);
    if (FAILED(hr) || !bitmap) return Fail(Status::Unsupported, L"WIC bitmap olusturulamadi.");

    ComPtr<IStream> stream;
    hr = ::CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr) || !stream) return Fail(Status::OutOfMemory, L"Bellek akisi olusturulamadi.");

    ComPtr<IWICBitmapEncoder> encoder;
    hr = wic.factory()->CreateEncoder(container, nullptr, &encoder);
    if (FAILED(hr) || !encoder) return Fail(Status::Unsupported, L"WIC kodlayici bulunamadi.");
    hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return Fail(Status::Unsupported, L"WIC kodlayici baslatilamadi.");

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> bag;
    hr = encoder->CreateNewFrame(&frame, &bag);
    if (FAILED(hr) || !frame) return Fail(Status::Unsupported, L"WIC kare olusturulamadi.");

    if (jpegQuality > 0 && bag) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v{};
        ::VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = static_cast<float>((std::min)(100, (std::max)(1, jpegQuality))) / 100.0f;
        bag->Write(1, &opt, &v);
        ::VariantClear(&v);
    }

    hr = frame->Initialize(bag.get());
    if (FAILED(hr)) return Fail(Status::Unsupported, L"WIC kare baslatilamadi.");
    hr = frame->SetSize(static_cast<UINT>(img.width()), static_cast<UINT>(img.height()));
    if (FAILED(hr)) return Fail(Status::Unsupported, L"WIC kare boyutu ayarlanamadi.");

    // Ask for BGRA; the codec may negotiate something else (JPEG will pick
    // 24bppBGR). WICConvertBitmapSource then does the conversion for us.
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);

    ComPtr<IWICBitmapSource> converted;
    hr = ::WICConvertBitmapSource(fmt, bitmap.get(), &converted);
    if (FAILED(hr) || !converted) return Fail(Status::Unsupported, L"Piksel formati donusturulemedi.");

    hr = frame->WriteSource(converted.get(), nullptr);
    if (FAILED(hr)) return Fail(Status::IoError, L"WIC kare yazilamadi.");
    hr = frame->Commit();
    if (FAILED(hr)) return Fail(Status::IoError, L"WIC kare kaydedilemedi.");
    hr = encoder->Commit();
    if (FAILED(hr)) return Fail(Status::IoError, L"WIC kodlama tamamlanamadi.");

    HGLOBAL hg = nullptr;
    hr = ::GetHGlobalFromStream(stream.get(), &hg);
    if (FAILED(hr) || !hg) return Fail(Status::IoError, L"Kodlanmis veri alinamadi.");
    const SIZE_T bytes = ::GlobalSize(hg);
    void* mem = ::GlobalLock(hg);
    if (!mem || bytes == 0) { if (mem) ::GlobalUnlock(hg); return Fail(Status::IoError, L"Kodlanmis veri bos."); }
    try { out.assign(static_cast<const u8*>(mem), static_cast<const u8*>(mem) + bytes); }
    catch (...) { ::GlobalUnlock(hg); return Fail(Status::OutOfMemory, L"Bellek yetersiz."); }
    ::GlobalUnlock(hg);
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// TGA
// ---------------------------------------------------------------------------
struct TgaHeader {
    u8  idLength;
    u8  colorMapType;
    u8  imageType;
    u16 cmFirst;
    u16 cmLength;
    u8  cmEntrySize;
    u16 xOrigin;
    u16 yOrigin;
    u16 width;
    u16 height;
    u8  pixelDepth;
    u8  descriptor;
};

/// Expand one raw TGA pixel to RGBA8.
inline void TgaPixel(const u8* s, int depth, const u8* palette, u32 paletteCount,
                     int paletteDepth, u8* d) {
    switch (depth) {
        case 8: {
            if (palette && s[0] < paletteCount) {
                const u8* e = palette + static_cast<size_t>(s[0]) * ((paletteDepth + 7) / 8);
                switch (paletteDepth) {
                    case 15: case 16: {
                        const u16 v = static_cast<u16>(e[0] | (e[1] << 8));
                        const u32 r5 = (v >> 10) & 0x1F, g5 = (v >> 5) & 0x1F, b5 = v & 0x1F;
                        d[0] = static_cast<u8>((r5 << 3) | (r5 >> 2));
                        d[1] = static_cast<u8>((g5 << 3) | (g5 >> 2));
                        d[2] = static_cast<u8>((b5 << 3) | (b5 >> 2));
                        d[3] = (paletteDepth == 16 && !(v & 0x8000)) ? 0 : 255;
                        break;
                    }
                    case 32: d[0] = e[2]; d[1] = e[1]; d[2] = e[0]; d[3] = e[3]; break;
                    default: d[0] = e[2]; d[1] = e[1]; d[2] = e[0]; d[3] = 255;  break;
                }
            } else {
                d[0] = d[1] = d[2] = s[0];   // grey scale
                d[3] = 255;
            }
            break;
        }
        case 15:
        case 16: {
            const u16 v = static_cast<u16>(s[0] | (s[1] << 8));
            const u32 r5 = (v >> 10) & 0x1F, g5 = (v >> 5) & 0x1F, b5 = v & 0x1F;
            d[0] = static_cast<u8>((r5 << 3) | (r5 >> 2));
            d[1] = static_cast<u8>((g5 << 3) | (g5 >> 2));
            d[2] = static_cast<u8>((b5 << 3) | (b5 >> 2));
            d[3] = (depth == 16 && !(v & 0x8000)) ? 0 : 255;
            break;
        }
        case 24: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;  break;
        default: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; break;   // 32
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Startup / Shutdown
// ---------------------------------------------------------------------------
Result Startup() {
    if (t_wic.refCount > 0 && t_wic.factory) { ++t_wic.refCount; return Result::Ok(); }

    bool ownsCom = false;
    const HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (hrCo == RPC_E_CHANGED_MODE) {
        // The thread is already COM initialised in another apartment model.
        // That is fine for WIC; we simply must not uninitialise it.
        ownsCom = false;
    } else if (FAILED(hrCo)) {
        return Fail(Status::Unsupported, T(L"COM baslatilamadi."));
    } else {
        ownsCom = true;   // S_OK and S_FALSE both need a matching CoUninitialize
    }

    IWICImagingFactory* factory = nullptr;
    const HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                          CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        if (ownsCom) ::CoUninitialize();
        return Fail(Status::Unsupported, T(L"WIC fabrikasi olusturulamadi."));
    }

    t_wic.factory  = factory;
    t_wic.ownsCom  = ownsCom;
    t_wic.refCount = 1;
    return Result::Ok();
}

void Shutdown() {
    if (t_wic.refCount <= 0) return;
    if (--t_wic.refCount > 0) return;
    if (t_wic.factory) { t_wic.factory->Release(); t_wic.factory = nullptr; }
    if (t_wic.ownsCom) { ::CoUninitialize(); t_wic.ownsCom = false; }
}

// ---------------------------------------------------------------------------
// DecodeWic
// ---------------------------------------------------------------------------
Result DecodeWic(const u8* data, size_t size, Image& out, ImageMeta* meta) {
    out.clear();
    if (meta) *meta = ImageMeta();
    if (!data || size == 0) return Fail(Status::InvalidArgument, T(L"Bos goruntu arabellegi."));
    if (size > 0xFFFFFFFFull) return Fail(Status::InvalidArgument, T(L"Goruntu cok buyuk."));

    TransientScope wic;
    if (!wic.ok()) return Fail(Status::Unsupported, T(L"WIC baslatilamadi."));

    ComPtr<IWICStream> stream;
    HRESULT hr = wic.factory()->CreateStream(&stream);
    if (FAILED(hr) || !stream) return Fail(Status::Unsupported, T(L"WIC akisi olusturulamadi."));
    hr = stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size));
    if (FAILED(hr)) return Fail(Status::Unsupported, T(L"WIC akisi baglanamadi."));

    ComPtr<IWICBitmapDecoder> decoder;
    hr = wic.factory()->CreateDecoderFromStream(stream.get(), nullptr,
                                                WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr) || !decoder)
        return Fail(Status::BadFormat, T(L"Goruntu formati taninmadi (WIC)."));

    UINT frameCount = 0;
    decoder->GetFrameCount(&frameCount);

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) return Fail(Status::BadFormat, T(L"Goruntu karesi okunamadi."));

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) return Fail(Status::BadFormat, T(L"Goruntu boyutu gecersiz."));

    WICPixelFormatGUID srcFmt = { 0 };
    frame->GetPixelFormat(&srcFmt);
    const bool srcHasAlpha =
        (srcFmt == GUID_WICPixelFormat32bppBGRA) || (srcFmt == GUID_WICPixelFormat32bppRGBA) ||
        (srcFmt == GUID_WICPixelFormat32bppPBGRA) || (srcFmt == GUID_WICPixelFormat64bppRGBA) ||
        (srcFmt == GUID_WICPixelFormat64bppPRGBA)  || (srcFmt == GUID_WICPixelFormat32bppPRGBA);

    ComPtr<IWICFormatConverter> conv;
    hr = wic.factory()->CreateFormatConverter(&conv);
    if (FAILED(hr) || !conv) return Fail(Status::Unsupported, T(L"WIC donusturucu olusturulamadi."));
    hr = conv->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
                          WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return Fail(Status::Unsupported, T(L"Piksel formati donusturulemedi."));

    if (!out.resize(static_cast<int>(w), static_cast<int>(h)))
        return Fail(Status::OutOfMemory, T(L"Goruntu icin bellek ayrilamadi."));

    const UINT stride = static_cast<UINT>(out.stride());
    hr = conv->CopyPixels(nullptr, stride, static_cast<UINT>(out.sizeBytes()), out.data());
    if (FAILED(hr)) { out.clear(); return Fail(Status::IoError, T(L"Piksel verisi kopyalanamadi.")); }

    SwapRB(out.data(), static_cast<size_t>(w) * h);   // BGRA -> RGBA

    if (meta) {
        GUID container = { 0 };
        decoder->GetContainerFormat(&container);
        meta->formatName = ContainerName(container);
        meta->width      = static_cast<int>(w);
        meta->height     = static_cast<int>(h);
        meta->mipCount   = 1;
        meta->faceCount  = (frameCount > 1) ? static_cast<int>(frameCount) : 1;
        meta->dataSize   = static_cast<u64>(size);
        meta->hasAlpha   = srcHasAlpha && out.hasTransparency();
    }
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// DecodeTga
// ---------------------------------------------------------------------------
Result DecodeTga(const u8* data, size_t size, Image& out, ImageMeta* meta) {
    out.clear();
    if (meta) *meta = ImageMeta();

    ByteReader r(data, size);
    TgaHeader h{};
    h.idLength     = r.readU8();
    h.colorMapType = r.readU8();
    h.imageType    = r.readU8();
    h.cmFirst      = r.readU16();
    h.cmLength     = r.readU16();
    h.cmEntrySize  = r.readU8();
    h.xOrigin      = r.readU16();
    h.yOrigin      = r.readU16();
    h.width        = r.readU16();
    h.height       = r.readU16();
    h.pixelDepth   = r.readU8();
    h.descriptor   = r.readU8();
    if (r.failed()) return Fail(Status::Truncated, T(L"TGA basligi eksik."));

    if (h.width == 0 || h.height == 0)
        return Fail(Status::BadFormat, T(L"TGA boyutu gecersiz."));
    if (h.pixelDepth != 8 && h.pixelDepth != 15 && h.pixelDepth != 16 &&
        h.pixelDepth != 24 && h.pixelDepth != 32)
        return Fail(Status::Unsupported, T(L"Desteklenmeyen TGA piksel derinligi."));

    const bool rle = (h.imageType == 9 || h.imageType == 10 || h.imageType == 11);
    const u8   base = static_cast<u8>(h.imageType & 0x07);
    if (base != 1 && base != 2 && base != 3)
        return Fail(Status::Unsupported, T(L"Desteklenmeyen TGA goruntu tipi."));

    if (!r.skip(h.idLength)) return Fail(Status::Truncated, T(L"TGA kimlik alani eksik."));

    // Colour map
    const u8* palette = nullptr;
    u32 paletteCount = 0;
    int paletteDepth = 0;
    if (h.colorMapType == 1 && h.cmLength > 0) {
        paletteDepth = h.cmEntrySize ? h.cmEntrySize : 24;
        const size_t entryBytes = static_cast<size_t>((paletteDepth + 7) / 8);
        const size_t mapBytes   = entryBytes * h.cmLength;
        palette = r.readPtr(mapBytes);
        if (!palette) return Fail(Status::Truncated, T(L"TGA renk paleti eksik."));
        paletteCount = h.cmLength;
    }

    const int  depth = h.pixelDepth;
    const size_t bpp = static_cast<size_t>((depth + 7) / 8);
    const int  w = h.width, ht = h.height;
    if (!out.resize(w, ht)) return Fail(Status::OutOfMemory, T(L"TGA icin bellek ayrilamadi."));

    const size_t total = static_cast<size_t>(w) * static_cast<size_t>(ht);
    u8* dst = out.data();

    if (!rle) {
        const u8* src = r.readPtr(total * bpp);
        if (!src) { out.clear(); return Fail(Status::Truncated, T(L"TGA piksel verisi eksik.")); }
        for (size_t i = 0; i < total; ++i)
            TgaPixel(src + i * bpp, depth, palette, paletteCount, paletteDepth, dst + i * 4);
    } else {
        size_t written = 0;
        while (written < total) {
            const u8 packet = r.readU8();
            if (r.failed()) { out.clear(); return Fail(Status::Truncated, T(L"TGA RLE verisi eksik.")); }
            const size_t count = static_cast<size_t>(packet & 0x7F) + 1;
            if (written + count > total) { out.clear(); return Fail(Status::BadFormat, T(L"TGA RLE tasmasi.")); }
            if (packet & 0x80) {
                const u8* src = r.readPtr(bpp);
                if (!src) { out.clear(); return Fail(Status::Truncated, T(L"TGA RLE verisi eksik.")); }
                u8 px[4];
                TgaPixel(src, depth, palette, paletteCount, paletteDepth, px);
                for (size_t i = 0; i < count; ++i) std::memcpy(dst + (written + i) * 4, px, 4);
            } else {
                const u8* src = r.readPtr(count * bpp);
                if (!src) { out.clear(); return Fail(Status::Truncated, T(L"TGA verisi eksik.")); }
                for (size_t i = 0; i < count; ++i)
                    TgaPixel(src + i * bpp, depth, palette, paletteCount, paletteDepth,
                             dst + (written + i) * 4);
            }
            written += count;
        }
    }

    // Origin bits: bit 5 set means the first row in the file is the TOP row.
    // TGA's default is bottom-up, which is the opposite of our layout.
    if ((h.descriptor & 0x20) == 0) out.flipVertical();
    if (h.descriptor & 0x10)        out.flipHorizontal();

    if (meta) {
        meta->formatName = FormatW(L"TGA %d bit%s", depth, rle ? L" RLE" : L"");
        meta->width      = w;
        meta->height     = ht;
        meta->mipCount   = 1;
        meta->faceCount  = 1;
        meta->dataSize   = static_cast<u64>(size);
        meta->hasAlpha   = (depth == 32) || (depth == 16);
    }
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// DecodeAny
// ---------------------------------------------------------------------------
Result DecodeAny(const u8* data, size_t size, Image& out, ImageMeta* meta) {
    out.clear();
    if (meta) *meta = ImageMeta();
    if (!data || size == 0) return Fail(Status::InvalidArgument, T(L"Bos goruntu arabellegi."));

    std::wstring err;

    if (IsDdj(data, size)) {
        if (DecodeDdj(data, size, 0, 0, out, meta, &err)) return Result::Ok();
        return Result::Fail(Status::BadFormat,
                            err.empty() ? std::wstring(T(L"DDJ cozulemedi.")) : err);
    }
    if (size >= 4 && std::memcmp(data, "DDS ", 4) == 0) {
        if (DecodeDds(data, size, out, meta, &err)) return Result::Ok();
        return Result::Fail(Status::BadFormat,
                            err.empty() ? std::wstring(T(L"DDS cozulemedi.")) : err);
    }

    // TGA has no magic at the front. Only try it when the 18 byte header is
    // structurally plausible, or when the v2 footer signature is present.
    bool looksTga = false;
    if (size >= 18 + 26) {
        const char* footer = reinterpret_cast<const char*>(data + size - 18);
        if (std::memcmp(footer, "TRUEVISION-XFILE", 16) == 0) looksTga = true;
    }
    if (!looksTga && size >= 18) {
        const u8 cmType = data[1];
        const u8 imType = data[2];
        const u8 depth  = data[16];
        const u16 w = static_cast<u16>(data[12] | (data[13] << 8));
        const u16 h = static_cast<u16>(data[14] | (data[15] << 8));
        const bool typeOk  = (imType == 1 || imType == 2 || imType == 3 ||
                              imType == 9 || imType == 10 || imType == 11);
        const bool depthOk = (depth == 8 || depth == 15 || depth == 16 || depth == 24 || depth == 32);
        looksTga = (cmType <= 1) && typeOk && depthOk && w > 0 && h > 0;
    }
    if (looksTga) {
        Image tga;
        ImageMeta tgaMeta;
        const Result r = DecodeTga(data, size, tga, &tgaMeta);
        if (r.ok()) { out = std::move(tga); if (meta) *meta = tgaMeta; return Result::Ok(); }
        // fall through to WIC - the heuristic may have misfired
    }

    return DecodeWic(data, size, out, meta);
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------
Result EncodePng(const Image& img, std::vector<u8>& out) {
    return EncodeWith(img, GUID_ContainerFormatPng, 0, out);
}

Result SavePng(const Image& img, const std::wstring& path) {
    std::vector<u8> bytes;
    const Result r = EncodePng(img, bytes);
    if (!r.ok()) return r;
    return WriteEntireFile(path, bytes.data(), bytes.size());
}

Result EncodeJpeg(const Image& img, int quality, std::vector<u8>& out) {
    return EncodeWith(img, GUID_ContainerFormatJpeg,
                      (quality <= 0) ? 90 : quality, out);
}

Result SaveJpeg(const Image& img, int quality, const std::wstring& path) {
    std::vector<u8> bytes;
    const Result r = EncodeJpeg(img, quality, bytes);
    if (!r.ok()) return r;
    return WriteEntireFile(path, bytes.data(), bytes.size());
}

Result SaveRaw(const u8* data, size_t size, const std::wstring& path) {
    // No codec involved - "ham olarak kaydet" writes the bytes verbatim.
    if (!data && size) return Fail(Status::InvalidArgument, L"Bos arabellek yazilamaz.");
    return WriteEntireFile(path, data, size);
}

// ---------------------------------------------------------------------------
// Win32 helpers
// ---------------------------------------------------------------------------
Result LoadIconAsImage(HINSTANCE inst, int resourceId, int size, Image& out) {
    out.clear();
    if (size <= 0 || size > 1024) return Fail(Status::InvalidArgument, L"Gecersiz simge boyutu.");

    HICON icon = static_cast<HICON>(::LoadImageW(inst, MAKEINTRESOURCEW(resourceId),
                                                 IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    if (!icon) return Fail(Status::NotFound, L"Simge kaynagi bulunamadi.");
    SRO_SCOPE_EXIT { ::DestroyIcon(icon); };

    if (!out.resize(size, size)) return Fail(Status::OutOfMemory, L"Simge icin bellek ayrilamadi.");

    BITMAPINFO bi;
    out.fillBitmapInfo(bi);
    void* bits = nullptr;
    HDC screen = ::GetDC(nullptr);
    HBITMAP dib = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) ::DeleteObject(dib);
        ::ReleaseDC(nullptr, screen);
        out.clear();
        return Fail(Status::OutOfMemory, L"DIB olusturulamadi.");
    }

    HDC mem = ::CreateCompatibleDC(screen);
    HGDIOBJ old = ::SelectObject(mem, dib);
    std::memset(bits, 0, static_cast<size_t>(size) * size * 4);
    ::DrawIconEx(mem, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
    ::SelectObject(mem, old);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);

    std::memcpy(out.data(), bits, static_cast<size_t>(size) * size * 4);
    ::DeleteObject(dib);

    // GDI gave us BGRA; and DrawIconEx leaves alpha at 0 for icons without an
    // alpha channel, which would make the whole image invisible.
    SwapRB(out.data(), static_cast<size_t>(size) * size);
    if (!out.hasTransparency()) return Result::Ok();

    bool anyOpaque = false;
    const u8* p = out.data();
    for (size_t i = 0, n = static_cast<size_t>(size) * size; i < n && !anyOpaque; ++i)
        if (p[i * 4 + 3] != 0) anyOpaque = true;
    if (!anyOpaque) {
        u8* q = out.data();
        for (size_t i = 0, n = static_cast<size_t>(size) * size; i < n; ++i) q[i * 4 + 3] = 255;
    }
    return Result::Ok();
}

HBITMAP CreateDibSection(const Image& img) { return img.createHBitmap(); }

} // namespace ImageIO
} // namespace sro
