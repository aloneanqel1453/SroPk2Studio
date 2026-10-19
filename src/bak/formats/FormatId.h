// ============================================================================
//  SRO Asset Studio - formats/FormatId.h
//
//  Central file-type identification: extension first, content signature second.
//  Everything that decides "what preview do I show?" goes through here so the
//  tree, the preview pane and the CLI --shot path always agree.
//
//  Implementation owner: Agent D (formats).
// ============================================================================
#pragma once

#include "../Common.h"

namespace sro {

// ---------------------------------------------------------------------------
// Format identity
// ---------------------------------------------------------------------------
enum class FormatId : int {
    Unknown = 0,

    // container
    Pk2,

    // textures / images
    Ddj,          ///< JMXVDDJ 1000 : 20-byte header then a raw DDS
    Dds,
    Png, Jpeg, Bmp, Tga, Gif, Ico,

    // geometry / animation
    Bms,          ///< JMXVBMS 0110 : mesh
    Bmt,          ///< JMXVBMT 0102 : material set
    Bsr,          ///< JMXVRES 0109 : resource (composed model)
    Bsk,          ///< JMXVBSK      : skeleton
    Ban,          ///< JMXVBAN 0102 : animation
    Cpd,          ///< JMXVCPD 0101 : compound

    // effects / ui
    Efp,          ///< JMXVEFF 0011/0012/0013 : particle effect
    Nif2dt,       ///< .2dt : NewInterface entry table

    // world / map
    Nvm,          ///< JMXVNVM 1000 : navigation mesh
    Dof,          ///< dungeon object file
    MapM,         ///< JMXVMAPM1000 : *.m  terrain mesh
    MapT,         ///< JMXVMAPT1001 : *.t  terrain lightmap
    MapO,         ///< JMXVMAPO1001 : *.o / *.o2 object placement
    Mfo,          ///< JMXVMFO 1000 : map project

    // *.ifo text variants
    IfoEnvironment,   ///< environment.ifo   (JMXVENVI is the binary sibling)
    IfoObject,        ///< object.ifo
    IfoObjectString,  ///< objectstring.ifo
    IfoObjectExt,     ///< objext.ifo
    IfoTile2d,        ///< tile2d.ifo  (JMXV2DTI)
    Envi,             ///< JMXVENVI1003 binary environment set

    // text-ish
    Text, Csv, Ini, Lua, Xml,
    Sct,          ///< Silkroad script, syntax-highlighted like Lua

    // client data files that live inside the archives (formats/ClientData.h)
    DivisionInfo,     ///< divisioninfo.txt
    GatePort,         ///< gateport.txt
    SilkroadVersion,  ///< sv.t
    SilkCfg,          ///< silkcfg.dat
    WndPos,           ///< wndpos.dat
    GmWarpFort,       ///< gmwpfort.dat
    AiNavData,        ///< AI_NAVIGATION\*.dat

    // audio
    Wav, Mp3, Ogg,

    // fallback
    Binary
};

/// How the preview pane should render a format.
enum class PreviewKind : int {
    None = 0,
    Image,      ///< ImageView   (.ddj .dds .png .jpg .bmp .tga)
    Mesh,       ///< MeshView    (.bms .bsr)
    Skeleton,   ///< MeshView in skeleton mode (.bsk)
    Animation,  ///< summary + playback on a skeleton (.ban)
    Material,   ///< material table with texture thumbnails (.bmt)
    Ui2dt,      ///< entry table + laid-out UI preview (.2dt)
    Text,       ///< TextView with encoding detection
    Table,      ///< TableView for tab/comma separated data
    Summary,    ///< structured header/count summary (.efp .nvm .dof .m .t .o2 .mfo .cpd)
    Properties, ///< decoded property list for the client data files (ClientData.h)
    Audio,      ///< duration/format + MCI play/stop
    Hex         ///< HexView fallback
};

/// Static description of a format.
struct FormatInfo {
    FormatId     id            = FormatId::Unknown;
    const wchar_t* extension   = L"";      ///< canonical extension incl. dot, "" if none
    const wchar_t* displayName = L"";      ///< Turkish, shown in the tree "Tur" column
    PreviewKind  preview       = PreviewKind::Hex;
    bool         isText        = false;
};

// ---------------------------------------------------------------------------
// Identification
// ---------------------------------------------------------------------------

/// By file name / extension only. Cheap; used while filling the tree.
///
/// NOTE: several client data files are identified by their exact NAME rather
/// than their extension (divisioninfo.txt, gateport.txt, sv.t, silkcfg.dat,
/// wndpos.dat, gmwpfort.dat). Pass the FULL inner path when you have it - the
/// AI_NAVIGATION *.dat family can only be told apart by its folder.
FormatId IdentifyByExtension(const std::wstring& fileName);

/// By leading bytes. `data` may be a short prefix (256 bytes is plenty).
/// Returns FormatId::Unknown when nothing matches.
FormatId IdentifyByContent(const u8* data, size_t size);

/// Extension first, then content as a tie-breaker / correction.
/// Some formats share an extension (.ifo has four variants, .o2 vs .o),
/// so content wins whenever it is conclusive.
FormatId Identify(const std::wstring& fileName, const u8* data, size_t size);

/// Full static description.
const FormatInfo& GetFormatInfo(FormatId id);

/// Turkish display name, e.g. L"Doku (DDJ)".
const wchar_t* FormatDisplayName(FormatId id);

/// Short type tag for the tree's "Tür" column (e.g. "BSR", "DDJ", "OGG",
/// "Metin"), so long descriptions don't steal room from the file path.
const wchar_t* FormatShortTag(FormatId id);

/// Which viewer to open.
PreviewKind PreviewKindFor(FormatId id);

/// True for formats the text viewer should handle.
bool IsTextFormat(FormatId id);

/// True for formats the image viewer should handle.
bool IsImageFormat(FormatId id);

/// A short note for the hex viewer's header line, e.g.
/// L"JMXVBMS 0110 (mesh)" or L"Taninmayan imza".
std::wstring DescribeSignature(const u8* data, size_t size);

/// Suggested extension when extracting with conversion
/// (.ddj -> L".png", .bms/.bsr -> L".obj"); empty when no conversion applies.
std::wstring ConvertedExtensionFor(FormatId id);

} // namespace sro
