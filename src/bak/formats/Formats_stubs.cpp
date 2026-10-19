// ============================================================================
//  SRO Asset Studio - formats/Formats_stubs.cpp
//
//  Home of the small "client data" parsers (divisioninfo, gateport, sv.t,
//  silkcfg, wndpos, gmwpfort, AI navigation). The heavier asset decoders that
//  once lived here as Phase-A stubs have all moved to their own translation
//  units (Ddj.cpp, Bms.cpp, Bmt.cpp, Bsr.cpp, Bsk.cpp, Ban.cpp, Efp.cpp,
//  Nif2dt.cpp, MapFiles.cpp, TextFile.cpp) - do NOT re-add them here or every
//  symbol collides at link time.
//
//  Every parser below:
//    * drives its reads through sro::ByteReader (bounds-checked, latching fail)
//    * validates the header/shape before trusting any field
//    * refuses to allocate on an unvalidated length (ByteReader enforces this)
//    * returns Status::Truncated / BadFormat (never crashes) on a short or
//      garbage file, so the preview pane can fall back to the hex view.
//
//  Code and comments: English.  User-facing strings: Turkish.
// ============================================================================
#include "Ddj.h"
#include "../core/I18n.h"   // UI strings follow the chosen language
#include "Bms.h"
#include "Bmt.h"
#include "Bsr.h"
#include "Bsk.h"
#include "Ban.h"
#include "Efp.h"
#include "Nif2dt.h"
#include "MapFiles.h"
#include "TextFile.h"
#include "ClientData.h"
#include "../core/Blowfish.h"

namespace sro {

namespace {

// silkcfg.dat graphic level (SilkCfg.md: 0=Low .. 4=Do not change).
const wchar_t* GraphicLevelNameTr(u32 g) {
    switch (g) {
        case 0: return T(L"Düşük");
        case 1: return T(L"Orta");
        case 2: return T(L"Yüksek");
        case 3: return T(L"Büyük ölçekli savaş");
        case 4: return T(L"Değiştirme");
        default: return T(L"Bilinmeyen");
    }
}

// silkcfg.dat brightness (SilkCfg.md: 0=Very Dark .. 4=Very Bright).
const wchar_t* BrightnessNameTr(u32 b) {
    switch (b) {
        case 0: return T(L"Çok karanlık");
        case 1: return T(L"Karanlık");
        case 2: return T(L"Normal");
        case 3: return T(L"Aydınlık");
        case 4: return T(L"Çok aydınlık");
        default: return T(L"Bilinmeyen");
    }
}

} // namespace

// ===========================================================================
// Ddj  ->  moved to formats/Ddj.cpp (textures agent). Nothing left here.
// ===========================================================================

// ===========================================================================
// Bms / Bmt / Bsr / Bsk / Ban / Efp / Nif2dt / MapFiles / TextFile
//
// IMPLEMENTED - the stubs that used to live here were replaced by the real
// parsers in Bms.cpp, Bmt.cpp, Bsr.cpp, Bsk.cpp, Ban.cpp, Efp.cpp, Nif2dt.cpp,
// MapFiles.cpp and TextFile.cpp. Do not re-add them here: every symbol would
// collide at link time.
// ===========================================================================

// ===========================================================================
// ClientData - detection
// ===========================================================================
ClientDataKind DetectClientData(const std::wstring& innerPath) {
    // Real: pure name matching, cheap, and the tree wants it before any
    // decoder exists. Validation of the contents still happens in the parsers.
    const std::wstring name = ToLowerW(PathFileName(innerPath));
    const std::wstring path = ToLowerW(NormalizeInnerPath(innerPath));

    if (name == L"divisioninfo.txt") return ClientDataKind::DivisionInfo;
    if (name == L"gateport.txt")     return ClientDataKind::GatePort;
    if (name == L"sv.t")             return ClientDataKind::SilkroadVersion;
    if (name == L"silkcfg.dat")      return ClientDataKind::SilkCfg;
    if (name == L"wndpos.dat")       return ClientDataKind::WndPos;
    if (name == L"gmwpfort.dat")     return ClientDataKind::GmWarpFort;
    if (EndsWithI(name, L".dat") &&
        (path.find(L"ai_navigation") != std::wstring::npos || name.rfind(L"ainavdata", 0) == 0))
        return ClientDataKind::AiNavData;      // navmesh\ainavdata_<region>.dat in Data.pk2
    return ClientDataKind::None;
}

const wchar_t* ClientDataNameTr(ClientDataKind kind) {
    switch (kind) {
        case ClientDataKind::DivisionInfo:    return T(L"Sunucu bölgeleri (divisioninfo.txt)");
        case ClientDataKind::GatePort:        return T(L"Gateway portu (gateport.txt)");
        case ClientDataKind::SilkroadVersion: return T(L"İstemci sürümü (sv.t)");
        case ClientDataKind::SilkCfg:         return T(L"İstemci ayarları (silkcfg.dat)");
        case ClientDataKind::WndPos:          return T(L"Pencere konumları (wndpos.dat)");
        case ClientDataKind::GmWarpFort:      return T(L"GM ışınlanma noktaları (gmwpfort.dat)");
        case ClientDataKind::AiNavData:       return T(L"AI navigasyon verisi");
        default:                              return T(L"Bilinmeyen");
    }
}

// ===========================================================================
// divisioninfo.txt  (DIVISIONINFO.md)
//
//   [u8 locale][u8 divisionCount]
//   per division: [u32 nameLen][name][u8 0]
//                 [u8 gatewayCount]
//                 per gateway: [u32 ipLen][ip][u8 0]
//
// Binary despite the ".txt" extension. Counts are single bytes, so no length
// can drive a large allocation; the length-prefixed strings are bounds-checked
// by ByteReader.
// ===========================================================================
Result ParseDivisionInfo(const u8* data, size_t size, DivisionInfoFile& out) {
    out = DivisionInfoFile();
    ByteReader r(data, size);

    out.locale = r.readU8();
    const u8 divisionCount = r.readU8();
    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"divisioninfo.txt başlığı eksik."));

    for (u8 i = 0; i < divisionCount; ++i) {
        DivisionEntry div;
        div.name = r.readLenPrefixedStringW(1252);
        r.readU8();                         // trailing '\0' after the name
        const u8 gatewayCount = r.readU8();
        if (r.failed()) break;

        for (u8 g = 0; g < gatewayCount; ++g) {
            std::wstring ip = r.readLenPrefixedStringW(1252);
            r.readU8();                     // trailing '\0' after the ip
            if (r.failed()) break;
            div.gatewayAddresses.push_back(std::move(ip));
        }
        if (r.failed()) break;
        out.divisions.push_back(std::move(div));
    }

    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"divisioninfo.txt beklenenden kısa."));
    return Result::Ok();
}

std::wstring DivisionInfoFile::summaryTr() const {
    std::wstring s;
    s += T(L"Yerel kimlik  : ") + FormatW(L"%u", locale) + L"\n";
    s += T(L"Bölge sayısı  : ") + FormatCountTr(divisions.size()) + L"\n";
    for (const DivisionEntry& d : divisions) {
        s += T(L"\nBölge: ") + (d.name.empty() ? T(L"(adsız)") : d.name) + L"\n";
        if (d.gatewayAddresses.empty())
            s += T(L"    (gateway adresi yok)\n");
        for (const std::wstring& ip : d.gatewayAddresses)
            s += T(L"    Gateway: ") + ip + L"\n";
    }
    return s;
}

// ===========================================================================
// gateport.txt  (Gateport.md)
//
//   [char[8] port]  e.g. "15779" NUL-padded to 8 bytes.
// ===========================================================================
Result ParseGatePort(const u8* data, size_t size, GatePortFile& out) {
    out = GatePortFile();
    if (size == 0)
        return Result::Fail(Status::Truncated, T(L"gateport.txt boş."));

    ByteReader r(data, size);
    const size_t n = (size < 8) ? size : 8;      // the field is 8 bytes; be lenient
    const std::string raw = r.readFixedString(n);
    out.portText = TrimW(MultiByteToWide(raw, 1252));

    if (out.portText.empty())
        return Result::Fail(Status::BadFormat, T(L"gateport.txt geçerli bir port içermiyor."));

    wchar_t* end = nullptr;
    const unsigned long v = wcstoul(out.portText.c_str(), &end, 10);
    if (end != out.portText.c_str() && v <= 0xFFFFul)
        out.port = static_cast<u16>(v);

    return Result::Ok();
}

std::wstring GatePortFile::summaryTr() const {
    return T(L"Gateway portu : ") + portText + L"\n";
}

// ===========================================================================
// sv.t  (SV.md) - the client version number
//
//   [u32 length (==8)][u8 buffer[8]][padding to 1024]
//
// `buffer` is Blowfish-encrypted; its first 4 plaintext bytes are the ASCII
// version digits, the last 4 are padding. The key is the FIRST 8 BYTES of the
// ASCII string "SILKROADVERSION" (i.e. "SILKROAD") used as a RAW key.
// ===========================================================================
Result ParseSilkroadVersion(const u8* data, size_t size, SilkroadVersionFile& out) {
    out = SilkroadVersionFile();
    ByteReader r(data, size);

    out.declaredLength = r.readU32();
    u8 buffer[8] = {};
    if (!r.readBytes(buffer, sizeof(buffer)))
        return Result::Fail(Status::Truncated, T(L"sv.t şifreli tampon için çok kısa."));

    Blowfish bf;
    if (!bf.initializeRaw(reinterpret_cast<const u8*>(kSilkroadVersionKeySource),
                          kSilkroadVersionKeyBytes))
        return Result::Fail(Status::BadFormat, T(L"sv.t için Blowfish anahtarı kurulamadı."));

    std::vector<u8> plain;
    if (!bf.decode(buffer, sizeof(buffer), plain) || plain.size() < 4)
        return Result::Fail(Status::BadFormat, T(L"sv.t şifresi çözülemedi."));

    out.decrypted = true;

    // First 4 plaintext bytes are the ASCII version digits.
    std::string digits;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>(plain[static_cast<size_t>(i)]);
        if (c >= '0' && c <= '9') digits.push_back(c);
        else break;
    }
    if (digits.empty())
        return Result::Fail(Status::BadFormat, T(L"sv.t çözülen sürüm rakam içermiyor."));

    out.versionText = MultiByteToWide(digits, 1252);
    out.version     = static_cast<u32>(strtoul(digits.c_str(), nullptr, 10));
    return Result::Ok();
}

std::wstring SilkroadVersionFile::summaryTr() const {
    if (!decrypted || versionText.empty())
        return T(L"İstemci sürümü: (çözülemedi)\n");
    return T(L"İstemci sürümü: ") + versionText + L"\n";
}

// ===========================================================================
// silkcfg.dat  (SilkCfg.md)
//
//   [u32 version][u32 unk(==1)][u32 width][u32 height]
//   [u8 graphic1][u8 brightness][u8 unk][u8 soundEnabled][u8 languageId]
//   if version==3: [u8 graphic2][u8 graphicIndex]
//
// The whole block only exists for version < 4.
// ===========================================================================
Result ParseSilkCfg(const u8* data, size_t size, SilkCfgFile& out) {
    out = SilkCfgFile();
    ByteReader r(data, size);

    out.version = r.readU32();
    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"silkcfg.dat başlığı eksik."));
    if (out.version >= 4)
        return Result::Fail(Status::Unsupported, T(L"silkcfg.dat sürümü desteklenmiyor."));

    r.readU32();                             // unkUInt1 (==1)
    out.width  = r.readU32();
    out.height = r.readU32();

    const u8 graphic1     = r.readU8();
    const u8 brightness   = r.readU8();
    r.readU8();                              // unkByte2
    const u8 soundEnabled = r.readU8();
    const u8 languageId   = r.readU8();
    if (out.version == 3) {
        r.readU8();                          // graphic2
        r.readU8();                          // graphicIndex
    }
    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"silkcfg.dat beklenenden kısa."));

    out.graphicsLevel = graphic1;
    out.brightness    = brightness;
    out.soundVolume   = soundEnabled;        // stored as a 0/1 "enabled" flag
    out.language      = languageId;
    return Result::Ok();
}

std::wstring SilkCfgFile::summaryTr() const {
    std::wstring s;
    s += T(L"Sürüm         : ") + FormatW(L"%u", version) + L"\n";
    s += T(L"Çözünürlük    : ") + FormatW(L"%u x %u", width, height) + L"\n";
    s += T(L"Grafik düzeyi : ") + std::wstring(GraphicLevelNameTr(graphicsLevel)) +
         FormatW(L" (%u)", graphicsLevel) + L"\n";
    s += T(L"Parlaklık     : ") + std::wstring(BrightnessNameTr(brightness)) +
         FormatW(L" (%u)", brightness) + L"\n";
    s += T(L"Ses           : ") + std::wstring(soundVolume ? T(L"Açık") : T(L"Kapalı")) + L"\n";
    s += T(L"Dil kimliği   : ") + FormatW(L"%u", language) + L"\n";
    return s;
}

// ===========================================================================
// wndpos.dat  (wndpos.md)
//
//   [u32 version(==3)][u32 windowCount(ignored)][u32 width][u32 height]
//   10 x [u32 x][u32 y]  in a fixed, named order.
// ===========================================================================
Result ParseWndPos(const u8* data, size_t size, WndPosFile& out) {
    out = WndPosFile();
    ByteReader r(data, size);

    const u32 version = r.readU32();
    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"wndpos.dat başlığı eksik."));
    if (version != 3)
        return Result::Fail(Status::BadFormat, T(L"wndpos.dat sürümü beklenen 3 değil."));

    r.readU32();                             // windowCount (unused, count is fixed)
    r.readU32();                             // width
    r.readU32();                             // height

    static const wchar_t* const kNames[10] = {
        L"GDR_MAINPOPUP", L"GDR_STORE", L"GDR_STORAGEROOM", L"GDR_EXCHANGE",
        L"GDR_WORLDMAP",  L"GDR_COS_WND", L"GDR_GAMEGUIDE", L"GDR_ALCHEMYBOX",
        L"GDR_AUTO_POTION", L"GDR_EXT_QUICK_SLOT"
    };

    for (int i = 0; i < 10; ++i) {
        WndPosEntry e;
        e.name = kNames[i];
        e.x = static_cast<i32>(r.readU32());
        e.y = static_cast<i32>(r.readU32());
        if (r.failed()) break;
        out.windows.push_back(std::move(e));
    }

    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"wndpos.dat beklenenden kısa."));
    return Result::Ok();
}

std::wstring WndPosFile::summaryTr() const {
    std::wstring s;
    s += T(L"Pencere sayısı : ") + FormatCountTr(windows.size()) + L"\n\n";
    for (const WndPosEntry& w : windows)
        s += FormatW(L"%-18s  X %d, Y %d\n", w.name.c_str(), w.x, w.y);
    return s;
}

// ===========================================================================
// gmwpfort.dat  (gmwpfort.md) - GM warp points
//
//   [u32 wpCount]
//   per warp: [u32 nameLen][name]
//             [u16 regionId][u16 pad][f32 x][f32 y][f32 z][u32 worldId]
//
// wpCount is not trusted for allocation: entries are pushed one at a time and
// ByteReader latches failure the moment the buffer runs out, so a corrupt count
// can never over-read or spin.
// ===========================================================================
Result ParseGmWarpFort(const u8* data, size_t size, GmWarpFortFile& out) {
    out = GmWarpFortFile();
    ByteReader r(data, size);

    const u32 wpCount = r.readU32();
    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"gmwpfort.dat başlığı eksik."));

    for (u32 i = 0; i < wpCount; ++i) {
        GmWarpPoint p;
        p.name     = r.readLenPrefixedStringW(1252);
        p.regionId = r.readU16();
        r.readU16();                         // struct padding
        p.x     = r.readF32();
        p.y     = r.readF32();
        p.z     = r.readF32();
        p.world = r.readU32();
        if (r.failed()) break;
        out.points.push_back(std::move(p));
    }

    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"gmwpfort.dat beklenenden kısa."));
    return Result::Ok();
}

std::wstring GmWarpFortFile::summaryTr() const {
    std::wstring s;
    s += T(L"Işınlanma noktası sayısı : ") + FormatCountTr(points.size()) + L"\n";

    const size_t shown = (points.size() < 20) ? points.size() : 20;
    if (shown) s += T(L"\nNoktalar (ilk ") + FormatCountTr(shown) + T(L"):\n");
    for (size_t i = 0; i < shown; ++i) {
        const GmWarpPoint& p = points[i];
        s += FormatW(T(L"    %-24s  bölge %u  konum(%.1f, %.1f, %.1f)  dünya %u\n"),
                     p.name.c_str(), p.regionId, p.x, p.y, p.z, p.world);
    }
    if (points.size() > shown)
        s += T(L"    ... ve ") + FormatCountTr(points.size() - shown) + T(L" nokta daha\n");
    return s;
}

// ===========================================================================
// AI_NAVIGATION\*.dat  (AINavData.md) - summary only
//
//   [u8 version(==1)][u32 simpleDungeonDataOffset][u16 regionId][u32 blockCount]
//
// We only read the header. The per-block cell/edge lookup tables are large and
// not needed for a preview summary, so we stop here and flag `partial`.
// ===========================================================================
Result ParseAiNavData(const u8* data, size_t size, AiNavDataFile& out) {
    out = AiNavDataFile();
    ByteReader r(data, size);

    const u8 version = r.readU8();
    r.readU32();                             // simpleDungeonDataOffset (unused here)
    out.regionId   = r.readU16();
    out.blockCount = r.readU32();
    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"AI navigasyon başlığı eksik."));
    if (version != 1)
        return Result::Fail(Status::Unsupported, T(L"AI navigasyon sürümü desteklenmiyor."));

    out.partial = true;                      // cell/edge tables intentionally skipped
    return Result::Ok();
}

std::wstring AiNavDataFile::summaryTr() const {
    std::wstring s;
    s += T(L"Bölge kimliği : ") + FormatW(L"%u", regionId) + L"\n";
    s += T(L"Blok sayısı   : ") + FormatCountTr(blockCount) + L"\n";
    if (partial)
        s += T(L"NOT           : Yalnızca özet okundu (hücre/kenar tabloları atlandı).\n");
    return s;
}

// ===========================================================================
// One-call dispatch for the preview pane.
// ===========================================================================
Result DescribeClientData(const std::wstring& innerPath, const u8* data, size_t size,
                          ClientDataKind& kindOut, std::wstring& propertyTextTr) {
    kindOut = DetectClientData(innerPath);
    propertyTextTr.clear();

    switch (kindOut) {
        case ClientDataKind::DivisionInfo: {
            DivisionInfoFile f;
            const Result r = ParseDivisionInfo(data, size, f);
            if (!r.ok()) return r;
            propertyTextTr = f.summaryTr();
            return Result::Ok();
        }
        case ClientDataKind::GatePort: {
            GatePortFile f;
            const Result r = ParseGatePort(data, size, f);
            if (!r.ok()) return r;
            propertyTextTr = f.summaryTr();
            return Result::Ok();
        }
        case ClientDataKind::SilkroadVersion: {
            SilkroadVersionFile f;
            const Result r = ParseSilkroadVersion(data, size, f);
            if (!r.ok()) return r;
            propertyTextTr = f.summaryTr();
            return Result::Ok();
        }
        case ClientDataKind::SilkCfg: {
            SilkCfgFile f;
            const Result r = ParseSilkCfg(data, size, f);
            if (!r.ok()) return r;
            propertyTextTr = f.summaryTr();
            return Result::Ok();
        }
        case ClientDataKind::WndPos: {
            WndPosFile f;
            const Result r = ParseWndPos(data, size, f);
            if (!r.ok()) return r;
            propertyTextTr = f.summaryTr();
            return Result::Ok();
        }
        case ClientDataKind::GmWarpFort: {
            GmWarpFortFile f;
            const Result r = ParseGmWarpFort(data, size, f);
            if (!r.ok()) return r;
            propertyTextTr = f.summaryTr();
            return Result::Ok();
        }
        case ClientDataKind::AiNavData: {
            AiNavDataFile f;
            const Result r = ParseAiNavData(data, size, f);
            if (!r.ok()) return r;
            propertyTextTr = f.summaryTr();
            return Result::Ok();
        }
        case ClientDataKind::None:
        default:
            return Result::Fail(Status::NotFound, T(L"Bilinen bir istemci veri dosyası değil."));
    }
}

} // namespace sro
