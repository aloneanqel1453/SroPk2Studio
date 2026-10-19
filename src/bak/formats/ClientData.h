// ============================================================================
//  SRO Asset Studio - formats/ClientData.h
//
//  Small, high-value client data files that live INSIDE the archives
//  (SPEC.md §3, "Client data files that live inside the archives").
//  Each gets a decoded property view instead of a hex dump.
//
//    divisioninfo.txt   DIVISIONINFO.md  locale byte + division names + gateway IPs
//    gateport.txt       Gateport.md      the 8-char port string, e.g. "15779"
//    sv.t               SV.md            the CLIENT VERSION NUMBER
//    silkcfg.dat        SilkCfg.md       version, resolution, graphics, sound, language
//    wndpos.dat         wndpos.md        10 named window positions
//    gmwpfort.dat       gmwpfort.md      GM warp points: name + region/x/y/z/world
//    AI_NAVIGATION\*.dat AINavData.md    region id + block/cell/edge counts (summary)
//
//  MATCHING RULE: match on the FILE NAME (case-insensitive) first, then
//  VALIDATE the header. A file that fails validation falls back to the hex
//  view rather than showing wrong numbers - never guess.
//
//  Everything here uses the same bounds-checked sro::ByteReader.
//
//  Implementation owner: Agent D (formats).
// ============================================================================
#pragma once

#include "../Common.h"

namespace sro {

// ---------------------------------------------------------------------------
// Which client data file this is.
// ---------------------------------------------------------------------------
enum class ClientDataKind : int {
    None = 0,
    DivisionInfo,   ///< divisioninfo.txt
    GatePort,       ///< gateport.txt
    SilkroadVersion,///< sv.t
    SilkCfg,        ///< silkcfg.dat
    WndPos,         ///< wndpos.dat
    GmWarpFort,     ///< gmwpfort.dat
    AiNavData       ///< AI_NAVIGATION\*.dat
};

/// Name-based detection. `innerPath` is the full path inside the archive, so
/// the AI_NAVIGATION folder can be recognised. Returns ClientDataKind::None
/// when the name does not match a known file.
ClientDataKind DetectClientData(const std::wstring& innerPath);

/// Turkish display name, e.g. L"İstemci sürümü (sv.t)".
const wchar_t* ClientDataNameTr(ClientDataKind kind);

// ---------------------------------------------------------------------------
// divisioninfo.txt
// ---------------------------------------------------------------------------
struct DivisionEntry {
    std::wstring              name;
    std::vector<std::wstring> gatewayAddresses;
};

struct DivisionInfoFile {
    u8                         locale = 0;
    std::vector<DivisionEntry> divisions;
    std::wstring summaryTr() const;
};

Result ParseDivisionInfo(const u8* data, size_t size, DivisionInfoFile& out);

// ---------------------------------------------------------------------------
// gateport.txt
// ---------------------------------------------------------------------------
struct GatePortFile {
    std::wstring portText;   ///< as stored, e.g. "15779"
    u16          port = 0;   ///< parsed, 0 when unparseable
    std::wstring summaryTr() const;
};

Result ParseGatePort(const u8* data, size_t size, GatePortFile& out);

// ---------------------------------------------------------------------------
// sv.t  - the client version number
//
// Layout:  u32 length (8), then a Blowfish-encrypted buffer.
// The Blowfish key is the FIRST 8 BYTES of the ASCII string "SILKROADVERSION"
// (i.e. "SILKROAD"). Decrypt and the plaintext is the version digits.
//
// Note this is a FORMAT-level constant, not a server key, so it is allowed
// under SPEC RULE 0. It is also not secret, so RULE 1 does not apply to it.
// ---------------------------------------------------------------------------
constexpr char kSilkroadVersionKeySource[] = "SILKROADVERSION";
constexpr size_t kSilkroadVersionKeyBytes  = 8;   ///< "SILKROAD"

struct SilkroadVersionFile {
    u32          declaredLength = 0;
    std::wstring versionText;      ///< decrypted digits, e.g. "1188"
    u32          version = 0;      ///< parsed, 0 when unparseable
    bool         decrypted = false;
    std::wstring summaryTr() const;
};

Result ParseSilkroadVersion(const u8* data, size_t size, SilkroadVersionFile& out);

// ---------------------------------------------------------------------------
// silkcfg.dat
// ---------------------------------------------------------------------------
struct SilkCfgFile {
    u32 version      = 0;
    u32 width        = 0;
    u32 height       = 0;
    u32 graphicsLevel= 0;
    u32 brightness   = 0;
    u32 soundVolume  = 0;
    u32 language     = 0;
    bool fullScreen  = false;
    std::wstring summaryTr() const;
};

Result ParseSilkCfg(const u8* data, size_t size, SilkCfgFile& out);

// ---------------------------------------------------------------------------
// wndpos.dat
// ---------------------------------------------------------------------------
struct WndPosEntry {
    std::wstring name;    ///< GDR_MAINPOPUP, GDR_STORE, ...
    i32 x = 0, y = 0;
};

struct WndPosFile {
    std::vector<WndPosEntry> windows;
    std::wstring summaryTr() const;
};

Result ParseWndPos(const u8* data, size_t size, WndPosFile& out);

// ---------------------------------------------------------------------------
// gmwpfort.dat  - GM warp points
// ---------------------------------------------------------------------------
struct GmWarpPoint {
    std::wstring name;
    u16  regionId = 0;
    f32  x = 0.0f, y = 0.0f, z = 0.0f;
    u32  world = 0;
};

struct GmWarpFortFile {
    std::vector<GmWarpPoint> points;
    std::wstring summaryTr() const;
};

Result ParseGmWarpFort(const u8* data, size_t size, GmWarpFortFile& out);

// ---------------------------------------------------------------------------
// AI_NAVIGATION\*.dat  - summary only
// ---------------------------------------------------------------------------
struct AiNavDataFile {
    std::string signature;
    u16 regionId   = 0;
    u32 blockCount = 0;
    u32 cellCount  = 0;
    u32 edgeCount  = 0;
    bool partial   = false;
    std::wstring summaryTr() const;
};

Result ParseAiNavData(const u8* data, size_t size, AiNavDataFile& out);

// ---------------------------------------------------------------------------
// One-call dispatch for the preview pane.
//
// Produces the Turkish property text for whichever kind `innerPath` names.
// Returns Status::BadFormat when validation fails, which the caller treats as
// "fall back to the hex view".
// ---------------------------------------------------------------------------
Result DescribeClientData(const std::wstring& innerPath,
                          const u8* data, size_t size,
                          ClientDataKind& kindOut,
                          std::wstring& propertyTextTr);

} // namespace sro
