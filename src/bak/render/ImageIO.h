// ============================================================================
//  SRO Asset Studio - render/ImageIO.h
//
//  Image encode/decode through WIC (windowscodecs.lib), plus a hand written
//  TGA decoder because WIC does not ship one.
//
//  Everything converts to / from the RGBA8 Image type.
//
//  COM note: WIC needs CoInitializeEx. Call ImageIO::Startup() once per thread
//  that touches these functions (the GUI thread and every worker), and
//  Shutdown() on the way out. The helpers are safe to call after a failed
//  Startup - they just return Status::Unsupported.
//
//  Implementation owner: Agent E (render).
// ============================================================================
#pragma once

#include "../Common.h"
#include "Image.h"

namespace sro {

namespace ImageIO {

/// CoInitializeEx(COINIT_APARTMENTTHREADED) + create the WIC factory for this
/// thread. Idempotent per thread.
Result Startup();
/// Release the factory and CoUninitialize for this thread.
void   Shutdown();

/// RAII wrapper for Startup/Shutdown on a worker thread.
class Scope {
public:
    Scope()  { m_ok = Startup().ok(); }
    ~Scope() { if (m_ok) Shutdown(); }
    bool ok() const { return m_ok; }
private:
    bool m_ok = false;
};

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

/// Decode PNG / JPEG / BMP / GIF / TIFF / ICO from memory via WIC.
Result DecodeWic(const u8* data, size_t size, Image& out, ImageMeta* meta);

/// Decode an uncompressed or RLE TGA (8/15/16/24/32 bpp), honouring the
/// origin bit. Hand written - WIC has no TGA codec.
Result DecodeTga(const u8* data, size_t size, Image& out, ImageMeta* meta);

/// Dispatch on content: DDJ/DDS go to the DDS decoder, TGA to DecodeTga,
/// everything else to WIC.
Result DecodeAny(const u8* data, size_t size, Image& out, ImageMeta* meta);

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

/// Encode `img` as a PNG into memory. Alpha is preserved.
Result EncodePng(const Image& img, std::vector<u8>& out);

/// Encode and write a PNG. This is what `--shot` and "PNG olarak kaydet" use.
Result SavePng(const Image& img, const std::wstring& path);

/// Encode as JPEG with `quality` in 1..100.
Result EncodeJpeg(const Image& img, int quality, std::vector<u8>& out);
Result SaveJpeg(const Image& img, int quality, const std::wstring& path);

/// Write the raw bytes of a .dds / .ddj payload straight out (no re-encode).
Result SaveRaw(const u8* data, size_t size, const std::wstring& path);

// ---------------------------------------------------------------------------
// Win32 helpers
// ---------------------------------------------------------------------------

/// Load an icon resource and rasterise it into an Image (for the About box).
Result LoadIconAsImage(HINSTANCE inst, int resourceId, int size, Image& out);

/// Create a 32-bit top-down DIB section holding a copy of `img`.
/// Caller owns the returned HBITMAP.
HBITMAP CreateDibSection(const Image& img);

} // namespace ImageIO

} // namespace sro
