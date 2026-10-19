// ============================================================================
//  SRO Asset Studio - formats/FormatId.cpp
//
//  Central file-type identification.
//
//  Order of authority:
//    1. content signature  (conclusive - a mis-named .ddj is still a .ddj)
//    2. file name          (client data files are recognised by NAME, not ext)
//    3. extension
//    4. heuristics         (.2dt has no magic; text sniffing)
//
//  Everything here is pure: no allocation on untrusted lengths, no reads past
//  `size`. `data` may legitimately be nullptr / very short - callers pass only
//  the first few hundred bytes of a file.
// ============================================================================
#include "FormatId.h"
#include "../core/I18n.h"   // format names follow the UI language
#include "Nif2dt.h"

namespace sro {

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

/// memcmp against a literal prefix, bounds checked.
bool HasPrefix(const u8* data, size_t size, const char* sig, size_t sigLen) {
    return data && size >= sigLen && std::memcmp(data, sig, sigLen) == 0;
}
template <size_t N>
bool HasPrefix(const u8* data, size_t size, const char (&sig)[N]) {
    return HasPrefix(data, size, sig, N - 1);   // drop the terminating NUL
}

/// Exact byte match against a raw sequence.
bool HasBytes(const u8* data, size_t size, const u8* seq, size_t n) {
    return data && size >= n && std::memcmp(data, seq, n) == 0;
}

/// The Silkroad "JMXV<TAG>" family always carries a 4 digit version. We only
/// key on the tag so a shard shipping "JMXVBMS 0111" still identifies.
bool IsJmxTag(const u8* data, size_t size, const char* tag) {
    const size_t n = std::strlen(tag);
    return HasPrefix(data, size, tag, n);
}

// ---------------------------------------------------------------------------
// The static format table.
//
// One row per FormatId. Kept in enum order so a missing row is obvious, but
// looked up by a linear scan (40-odd entries, called once per tree row).
// ---------------------------------------------------------------------------
const FormatInfo kFormats[] = {
    { FormatId::Unknown,         L"",      L"Bilinmeyen",                        PreviewKind::Hex,        false },

    { FormatId::Pk2,             L".pk2",  L"Arşiv (PK2)",                       PreviewKind::Hex,        false },

    { FormatId::Ddj,             L".ddj",  L"Doku (DDJ)",                        PreviewKind::Image,      false },
    { FormatId::Dds,             L".dds",  L"Doku (DDS)",                        PreviewKind::Image,      false },
    { FormatId::Png,             L".png",  L"Resim (PNG)",                       PreviewKind::Image,      false },
    { FormatId::Jpeg,            L".jpg",  L"Resim (JPEG)",                      PreviewKind::Image,      false },
    { FormatId::Bmp,             L".bmp",  L"Resim (BMP)",                       PreviewKind::Image,      false },
    { FormatId::Tga,             L".tga",  L"Resim (TGA)",                       PreviewKind::Image,      false },
    { FormatId::Gif,             L".gif",  L"Resim (GIF)",                       PreviewKind::Image,      false },
    { FormatId::Ico,             L".ico",  L"Simge (ICO)",                       PreviewKind::Image,      false },

    { FormatId::Bms,             L".bms",  L"Ağ / mesh (BMS)",                   PreviewKind::Mesh,       false },
    { FormatId::Bmt,             L".bmt",  L"Malzeme seti (BMT)",                PreviewKind::Material,   false },
    { FormatId::Bsr,             L".bsr",  L"Kaynak / model (BSR)",              PreviewKind::Mesh,       false },
    { FormatId::Bsk,             L".bsk",  L"İskelet (BSK)",                     PreviewKind::Skeleton,   false },
    { FormatId::Ban,             L".ban",  L"Animasyon (BAN)",                   PreviewKind::Animation,  false },
    { FormatId::Cpd,             L".cpd",  L"Bileşik nesne (CPD)",               PreviewKind::Summary,    false },

    { FormatId::Efp,             L".efp",  L"Parçacık efekti (EFP)",             PreviewKind::Summary,    false },
    { FormatId::Nif2dt,          L".2dt",  L"Arayüz tablosu (2DT)",              PreviewKind::Ui2dt,      false },

    { FormatId::Nvm,             L".nvm",  L"Navigasyon ağı (NVM)",              PreviewKind::Summary,    false },
    { FormatId::Dof,             L".dof",  L"Zindan dosyası (DOF)",              PreviewKind::Summary,    false },
    { FormatId::MapM,            L".m",    L"Arazi ağı (.m)",                    PreviewKind::Summary,    false },
    { FormatId::MapT,            L".t",    L"Arazi ışık haritası (.t)",          PreviewKind::Summary,    false },
    { FormatId::MapO,            L".o",    L"Nesne yerleşimi (.o/.o2)",          PreviewKind::Summary,    false },
    { FormatId::Mfo,             L".mfo",  L"Harita projesi (MFO)",              PreviewKind::Summary,    false },

    { FormatId::IfoEnvironment,  L".ifo",  L"Ortam listesi (environment.ifo)",   PreviewKind::Table,      true  },
    { FormatId::IfoObject,       L".ifo",  L"Nesne listesi (object.ifo)",        PreviewKind::Table,      true  },
    { FormatId::IfoObjectString, L".ifo",  L"Nesne etiketleri (objectstring.ifo)", PreviewKind::Table,    true  },
    { FormatId::IfoObjectExt,    L".ifo",  L"Nesne ek bilgisi (objext.ifo)",     PreviewKind::Table,      true  },
    { FormatId::IfoTile2d,       L".ifo",  L"Zemin doku listesi (tile2d.ifo)",   PreviewKind::Table,      true  },
    { FormatId::Envi,            L".ifo",  L"Ortam ayarları (JMXVENVI)",         PreviewKind::Summary,    false },

    { FormatId::Text,            L".txt",  L"Metin",                             PreviewKind::Text,       true  },
    { FormatId::Csv,             L".csv",  L"CSV tablo",                         PreviewKind::Table,      true  },
    { FormatId::Ini,             L".ini",  L"Ayar dosyası (INI)",                PreviewKind::Text,       true  },
    { FormatId::Lua,             L".lua",  L"Lua betiği",                        PreviewKind::Text,       true  },
    { FormatId::Xml,             L".xml",  L"XML",                               PreviewKind::Text,       true  },
    { FormatId::Sct,             L".sct",  L"Betik (SCT)",                       PreviewKind::Text,       true  },

    { FormatId::DivisionInfo,    L".txt",  L"Sunucu bölgeleri (divisioninfo.txt)", PreviewKind::Properties, false },
    { FormatId::GatePort,        L".txt",  L"Gateway portu (gateport.txt)",      PreviewKind::Properties, false },
    { FormatId::SilkroadVersion, L".t",    L"İstemci sürümü (sv.t)",             PreviewKind::Properties, false },
    { FormatId::SilkCfg,         L".dat",  L"İstemci ayarları (silkcfg.dat)",    PreviewKind::Properties, false },
    { FormatId::WndPos,          L".dat",  L"Pencere konumları (wndpos.dat)",    PreviewKind::Properties, false },
    { FormatId::GmWarpFort,      L".dat",  L"GM ışınlanma noktaları (gmwpfort.dat)", PreviewKind::Properties, false },
    { FormatId::AiNavData,       L".dat",  L"AI navigasyon verisi",              PreviewKind::Properties, false },

    { FormatId::Wav,             L".wav",  L"Ses (WAV)",                         PreviewKind::Audio,      false },
    { FormatId::Mp3,             L".mp3",  L"Ses (MP3)",                         PreviewKind::Audio,      false },
    { FormatId::Ogg,             L".ogg",  L"Ses (OGG)",                         PreviewKind::Audio,      false },

    { FormatId::Binary,          L"",      L"İkili veri",                        PreviewKind::Hex,        false },
};

// ---------------------------------------------------------------------------
// Client data files are matched by NAME (and, for AI_NAVIGATION, by folder).
// Returns FormatId::Unknown when the name is not one of them.
// ---------------------------------------------------------------------------
FormatId IdentifyClientDataByName(const std::wstring& fullPath) {
    const std::wstring name = ToLowerW(PathFileName(fullPath));
    if (name.empty()) return FormatId::Unknown;

    if (name == L"divisioninfo.txt") return FormatId::DivisionInfo;
    if (name == L"gateport.txt")     return FormatId::GatePort;
    if (name == L"sv.t")             return FormatId::SilkroadVersion;
    if (name == L"silkcfg.dat")      return FormatId::SilkCfg;
    if (name == L"wndpos.dat")       return FormatId::WndPos;
    if (name == L"gmwpfort.dat")     return FormatId::GmWarpFort;

    if (EndsWithI(name, L".dat")) {
        const std::wstring lowered = ToLowerW(fullPath);
        // Data.pk2 keeps the same files as navmesh\ainavdata_<region>.dat.
        if (lowered.find(L"ai_navigation") != std::wstring::npos || name.rfind(L"ainavdata", 0) == 0)
            return FormatId::AiNavData;
    }
    return FormatId::Unknown;
}

/// The .ifo family shares one extension; only the file name tells them apart.
FormatId IdentifyIfoByName(const std::wstring& fullPath) {
    const std::wstring name = ToLowerW(PathFileName(fullPath));
    if (name == L"environment.ifo")  return FormatId::IfoEnvironment;
    if (name == L"object.ifo")       return FormatId::IfoObject;
    if (name == L"objectstring.ifo") return FormatId::IfoObjectString;
    if (name == L"objext.ifo")       return FormatId::IfoObjectExt;
    if (name == L"tile2d.ifo")       return FormatId::IfoTile2d;
    // Unknown .ifo: treat as the generic index list. ParseIfo re-detects the
    // real variant from the shape of the first data row.
    return FormatId::IfoObject;
}

/// Cheap "is this printable text?" sniff for the very last fallback.
bool LooksLikeText(const u8* data, size_t size) {
    if (!data || size == 0) return false;
    const size_t n = (size < 512) ? size : 512;
    size_t printable = 0, nul = 0;
    for (size_t i = 0; i < n; ++i) {
        const u8 c = data[i];
        if (c == 0) { ++nul; continue; }
        if (c == '\t' || c == '\r' || c == '\n' || (c >= 0x20 && c != 0x7F)) ++printable;
    }
    // UTF-16LE text has ~50% NULs; plain text has none.
    if (nul * 2 >= n && printable * 2 >= n - nul) return true;
    return nul == 0 && printable >= (n * 9) / 10;
}

} // namespace

// ---------------------------------------------------------------------------
// IdentifyByExtension
// ---------------------------------------------------------------------------
FormatId IdentifyByExtension(const std::wstring& fileName) {
    // Name-matched client data files win over their own extension.
    const FormatId byName = IdentifyClientDataByName(fileName);
    if (byName != FormatId::Unknown) return byName;

    const std::wstring ext = PathExtension(fileName);
    if (ext.empty()) return FormatId::Unknown;

    if (ext == L".pk2")  return FormatId::Pk2;
    if (ext == L".lib")  return FormatId::Pk2;   // Origin ships its pack as .lib
    if (ext == L".ne")   return FormatId::Pk2;   // NewEra Online ships its pack as .ne

    if (ext == L".ddj")  return FormatId::Ddj;
    if (ext == L".dds")  return FormatId::Dds;
    if (ext == L".png")  return FormatId::Png;
    if (ext == L".jpg" || ext == L".jpeg") return FormatId::Jpeg;
    if (ext == L".bmp")  return FormatId::Bmp;
    if (ext == L".tga")  return FormatId::Tga;
    if (ext == L".gif")  return FormatId::Gif;
    if (ext == L".ico")  return FormatId::Ico;

    if (ext == L".bms")  return FormatId::Bms;
    if (ext == L".bmt")  return FormatId::Bmt;
    if (ext == L".bsr")  return FormatId::Bsr;
    if (ext == L".bsk")  return FormatId::Bsk;
    if (ext == L".ban")  return FormatId::Ban;
    if (ext == L".cpd")  return FormatId::Cpd;

    if (ext == L".efp")  return FormatId::Efp;
    if (ext == L".2dt")  return FormatId::Nif2dt;

    if (ext == L".nvm")  return FormatId::Nvm;
    if (ext == L".dof")  return FormatId::Dof;
    if (ext == L".m")    return FormatId::MapM;
    if (ext == L".t")    return FormatId::MapT;
    if (ext == L".o" || ext == L".o2") return FormatId::MapO;
    if (ext == L".mfo")  return FormatId::Mfo;
    if (ext == L".ifo")  return IdentifyIfoByName(fileName);

    if (ext == L".txt")  return FormatId::Text;
    if (ext == L".csv")  return FormatId::Csv;
    if (ext == L".ini" || ext == L".cfg" || ext == L".conf") return FormatId::Ini;
    if (ext == L".lua")  return FormatId::Lua;
    if (ext == L".sct")  return FormatId::Sct;
    if (ext == L".xml" || ext == L".html" || ext == L".htm") return FormatId::Xml;

    if (ext == L".wav")  return FormatId::Wav;
    if (ext == L".mp3")  return FormatId::Mp3;
    if (ext == L".ogg")  return FormatId::Ogg;

    return FormatId::Unknown;
}

// ---------------------------------------------------------------------------
// IdentifyByContent
// ---------------------------------------------------------------------------
FormatId IdentifyByContent(const u8* data, size_t size) {
    if (!data || size < 4) return FormatId::Unknown;

    // --- Silkroad container -------------------------------------------------
    if (HasPrefix(data, size, "JoyMax File Manager!")) return FormatId::Pk2;

    // --- JMXV family (tag first, version tolerated) --------------------------
    if (IsJmxTag(data, size, "JMXVDDJ"))  return FormatId::Ddj;
    if (IsJmxTag(data, size, "JMXVBMS"))  return FormatId::Bms;
    if (IsJmxTag(data, size, "JMXVBMT"))  return FormatId::Bmt;
    if (IsJmxTag(data, size, "JMXVRES"))  return FormatId::Bsr;
    if (IsJmxTag(data, size, "JMXVBSK"))  return FormatId::Bsk;
    if (IsJmxTag(data, size, "JMXVBAN"))  return FormatId::Ban;
    if (IsJmxTag(data, size, "JMXVCPD"))  return FormatId::Cpd;
    if (IsJmxTag(data, size, "JMXVEFF"))  return FormatId::Efp;
    if (IsJmxTag(data, size, "JMXVNVM"))  return FormatId::Nvm;
    if (IsJmxTag(data, size, "JMXVDOF"))  return FormatId::Dof;
    if (IsJmxTag(data, size, "JMXVMAPM")) return FormatId::MapM;
    if (IsJmxTag(data, size, "JMXVMAPT")) return FormatId::MapT;
    if (IsJmxTag(data, size, "JMXVMAPO")) return FormatId::MapO;
    if (IsJmxTag(data, size, "JMXVMFO"))  return FormatId::Mfo;
    if (IsJmxTag(data, size, "JMXVENVI")) return FormatId::Envi;

    // --- images -------------------------------------------------------------
    if (HasPrefix(data, size, "DDS ")) return FormatId::Dds;
    {
        static const u8 kPng[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
        if (HasBytes(data, size, kPng, 8)) return FormatId::Png;
    }
    {
        static const u8 kJpeg[3] = { 0xFF, 0xD8, 0xFF };
        if (HasBytes(data, size, kJpeg, 3)) return FormatId::Jpeg;
    }
    if (HasPrefix(data, size, "BM") && size >= 14) return FormatId::Bmp;
    if (HasPrefix(data, size, "GIF8")) return FormatId::Gif;
    {
        static const u8 kIco[4] = { 0x00, 0x00, 0x01, 0x00 };
        if (HasBytes(data, size, kIco, 4)) return FormatId::Ico;
    }

    // --- text BOMs (BEFORE audio) -------------------------------------------
    // Silkroad client-data .txt files are UTF-16 LE, i.e. they start FF FE.
    // The MP3 frame-sync heuristic below (FF Ex) would otherwise swallow that
    // BOM (FE & E0 == E0) and mis-route the file to the audio player. BOMs win.
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) return FormatId::Text;
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) return FormatId::Text;  // UTF-16 LE
    if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF) return FormatId::Text;  // UTF-16 BE

    // --- audio --------------------------------------------------------------
    if (HasPrefix(data, size, "OggS")) return FormatId::Ogg;
    if (HasPrefix(data, size, "RIFF") && size >= 12 &&
        std::memcmp(data + 8, "WAVE", 4) == 0) return FormatId::Wav;
    if (HasPrefix(data, size, "ID3")) return FormatId::Mp3;
    // Bare MPEG frame sync (no ID3 tag). Kept deliberately narrow: layer-III
    // audio only, and never the FF FE UTF-16 BOM (already returned above).
    if (size >= 2 && data[0] == 0xFF && (data[1] & 0xF6) == 0xF2) return FormatId::Mp3;

    return FormatId::Unknown;
}

// ---------------------------------------------------------------------------
// Identify
// ---------------------------------------------------------------------------
FormatId Identify(const std::wstring& fileName, const u8* data, size_t size) {
    const FormatId byName    = IdentifyClientDataByName(fileName);
    const FormatId byExt     = IdentifyByExtension(fileName);
    const FormatId byContent = IdentifyByContent(data, size);

    if (byContent != FormatId::Unknown) {
        // A BOM only proves "this is text"; the extension knows WHICH text.
        if (byContent == FormatId::Text) {
            if (byName != FormatId::Unknown)                      return byName;
            if (byExt != FormatId::Unknown && IsTextFormat(byExt)) return byExt;
            return FormatId::Text;
        }
        return byContent;   // a real magic number always wins
    }

    // sv.t and friends carry no signature at all - trust the name.
    if (byName != FormatId::Unknown) return byName;
    if (byExt != FormatId::Unknown)  return byExt;

    // .2dt has no magic; the size relation is the only tell.
    if (IsNif2dt(data, size)) return FormatId::Nif2dt;

    if (LooksLikeText(data, size)) return FormatId::Text;
    return (data && size) ? FormatId::Binary : FormatId::Unknown;
}

// ---------------------------------------------------------------------------
// Table lookups
// ---------------------------------------------------------------------------
const FormatInfo& GetFormatInfo(FormatId id) {
    for (const FormatInfo& fi : kFormats)
        if (fi.id == id) return fi;
    return kFormats[0];   // Unknown
}

// The table stores the Turkish name; translate on the way out so the "Tur:"
// line of the File Info panel and the tree's type column follow the UI language
// instead of staying Turkish in all 16.
const wchar_t* FormatDisplayName(FormatId id) { return T(GetFormatInfo(id).displayName); }

const wchar_t* FormatShortTag(FormatId id) {
    switch (id) {
        case FormatId::Pk2:            return L"PK2";
        case FormatId::Ddj:            return L"DDJ";
        case FormatId::Dds:            return L"DDS";
        case FormatId::Png:            return L"PNG";
        case FormatId::Jpeg:           return L"JPG";
        case FormatId::Bmp:            return L"BMP";
        case FormatId::Tga:            return L"TGA";
        case FormatId::Gif:            return L"GIF";
        case FormatId::Ico:            return L"ICO";
        case FormatId::Bms:            return L"BMS";
        case FormatId::Bmt:            return L"BMT";
        case FormatId::Bsr:            return L"BSR";
        case FormatId::Bsk:            return L"BSK";
        case FormatId::Ban:            return L"BAN";
        case FormatId::Cpd:            return L"CPD";
        case FormatId::Efp:            return L"EFP";
        case FormatId::Nif2dt:         return L"2DT";
        case FormatId::Nvm:            return L"NVM";
        case FormatId::Dof:            return L"DOF";
        case FormatId::MapM:           return L"MAP.M";
        case FormatId::MapT:           return L"MAP.T";
        case FormatId::MapO:           return L"MAP.O";
        case FormatId::Mfo:            return L"MFO";
        case FormatId::IfoEnvironment:
        case FormatId::IfoObject:
        case FormatId::IfoObjectString:
        case FormatId::IfoObjectExt:
        case FormatId::IfoTile2d:      return L"IFO";
        case FormatId::Envi:           return L"ENVI";
        case FormatId::Text:           return L"Metin";
        case FormatId::Csv:            return L"CSV";
        case FormatId::Ini:            return L"INI";
        case FormatId::Lua:            return L"LUA";
        case FormatId::Xml:            return L"XML";
        case FormatId::Sct:            return L"SCT";
        case FormatId::DivisionInfo:
        case FormatId::GatePort:
        case FormatId::SilkroadVersion:
        case FormatId::SilkCfg:
        case FormatId::WndPos:
        case FormatId::GmWarpFort:
        case FormatId::AiNavData:      return L"Veri";
        case FormatId::Wav:            return L"WAV";
        case FormatId::Mp3:            return L"MP3";
        case FormatId::Ogg:            return L"OGG";
        case FormatId::Binary:         return L"Bin";
        default:                       return L"?";
    }
}
PreviewKind    PreviewKindFor(FormatId id)    { return GetFormatInfo(id).preview; }
bool           IsTextFormat(FormatId id)      { return GetFormatInfo(id).isText; }

bool IsImageFormat(FormatId id) {
    switch (id) {
        case FormatId::Ddj: case FormatId::Dds: case FormatId::Png:
        case FormatId::Jpeg: case FormatId::Bmp: case FormatId::Tga:
        case FormatId::Gif:  case FormatId::Ico:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// DescribeSignature - the hex viewer's header line
// ---------------------------------------------------------------------------
std::wstring DescribeSignature(const u8* data, size_t size) {
    if (!data || size == 0) return T(L"Boş dosya");

    // JMXV* files carry a printable "JMXVxxx nnnn" in the first 12 bytes.
    if (size >= 12 && std::memcmp(data, "JMXV", 4) == 0) {
        char sig[13] = {};
        std::memcpy(sig, data, 12);
        for (int i = 0; i < 12; ++i)
            if (sig[i] < 0x20 || sig[i] > 0x7E) sig[i] = ' ';
        std::wstring wide = MultiByteToWide(std::string(sig, 12), 1252);
        wide = TrimW(wide);

        const FormatId id = IdentifyByContent(data, size);
        if (id != FormatId::Unknown)
            return wide + L"  (" + FormatDisplayName(id) + L")";
        return wide + T(L"  (tanınmayan JMXV sürümü)");
    }

    const FormatId id = IdentifyByContent(data, size);
    if (id != FormatId::Unknown)
        return std::wstring(FormatDisplayName(id)) + T(L" imzası");

    if (IsNif2dt(data, size))
        return T(L"Arayüz tablosu (2DT) - imzasız, boyut ilişkisinden tanındı");

    if (LooksLikeText(data, size)) return T(L"Düz metin (imzasız)");

    // Show the first bytes so the user has something to go on.
    const size_t n = (size < 8) ? size : 8;
    return T(L"Tanınmayan imza: ") + BytesToHexW(data, n);
}

// ---------------------------------------------------------------------------
// ConvertedExtensionFor
// ---------------------------------------------------------------------------
std::wstring ConvertedExtensionFor(FormatId id) {
    switch (id) {
        case FormatId::Ddj:  return L".png";
        case FormatId::Dds:  return L".png";
        case FormatId::Bms:  return L".obj";
        case FormatId::Bsr:  return L".obj";
        default:             return std::wstring();
    }
}

} // namespace sro
