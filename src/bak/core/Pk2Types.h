// ============================================================================
//  SRO Asset Studio - core/Pk2Types.h
//
//  On-disk JMXPACK structures (VERIFIED against real archives, SPEC.md §1)
//  plus the friendly decoded types the rest of the program works with.
//
//  Key facts that drive the design:
//   * The root block sits at offset 256, immediately after the 256-byte header.
//   * A folder's children are a CHAIN of 2560-byte blocks. Entry[19].NextBlock
//     points at the next block of the same folder (0 = end of chain).
//   * Only the 2560-byte blocks are encrypted. File payloads are plaintext.
//   * "." and ".." are self/parent pointers and must be skipped when listing.
//   * Deleting marks an entry Empty; the payload stays behind => "Compact".
//   * Offset/NextBlock are 64-bit. Size is 32-bit. A real archive is 6.26 GB.
//
//  This header is data only - no owner, nothing to implement.
// ============================================================================
#pragma once

#include "../Common.h"
#include "Blowfish.h"   // BaseKeyId, used by Pk2CreateOptions
#include "Obfuscate.h"  // OBF() - keep format signatures out of a strings dump
#include <string>

namespace sro {

// ---------------------------------------------------------------------------
// Raw on-disk layout
// ---------------------------------------------------------------------------
#pragma pack(push, 1)

/// 256 bytes at file offset 0.
struct Pk2RawHeader {
    char Signature[30];   ///< "JoyMax File Manager!\n", NUL padded
    u8   Version[4];      ///< 02 00 00 01 => 2.0.0.1
    u8   Encrypted;       ///< 1 = block tables are Blowfish encrypted
    u8   Checksum[16];    ///< only the first 3 bytes are meaningful (oracle 1)
    u8   Reserved[205];
};

/// 128 bytes. 20 of these form a block.
struct Pk2RawEntry {
    u8  Type;             ///< 0 = Empty, 1 = Folder, 2 = File
    char Name[89];        ///< NUL terminated, ANSI
    u64 CreateTime;       ///< Windows FILETIME
    u64 ModifyTime;       ///< Windows FILETIME
    i64 Offset;           ///< Folder: first block of its chain. File: payload offset.
    u32 Size;             ///< payload size in bytes (files only)
    i64 NextBlock;        ///< only meaningful on entry index 19 (the last one)
    u8  Padding[2];       ///< keeps the struct a multiple of 8 so Blowfish fits
};

/// 2560 bytes = 20 entries. This is the unit that gets encrypted.
struct Pk2RawBlock {
    Pk2RawEntry Entries[20];
};

#pragma pack(pop)

static_assert(sizeof(Pk2RawHeader) == 256,  "Pk2RawHeader must be 256 bytes");
static_assert(sizeof(Pk2RawEntry)  == 128,  "Pk2RawEntry must be 128 bytes");
static_assert(sizeof(Pk2RawBlock)  == 2560, "Pk2RawBlock must be 2560 bytes");

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr u64    kPk2HeaderSize      = 256;
constexpr u64    kPk2RootBlockOffset = 256;
constexpr size_t kPk2EntrySize       = 128;
constexpr size_t kPk2EntriesPerBlock = 20;
constexpr size_t kPk2BlockSize       = 2560;
constexpr size_t kPk2MaxNameBytes    = 89;   ///< including the NUL terminator

/// Exactly what a stock archive carries in Pk2RawHeader::Signature.
/// Obfuscated so the signature does not appear as plaintext in a strings dump.
inline std::string Pk2Signature() { return OBF("JoyMax File Manager!\n"); }

// ---------------------------------------------------------------------------
// Sailor Online / SailSafe variant (format-level, NOT a per-server key).
//
// Some private servers wrap the archive with the "SailSafe" anti-tamper layer.
// The first bytes are then NOT the JoyMax header but a fixed 788-byte SailSafe
// signature block, and the encrypted directory starts at 0x33E instead of 256.
// The block cipher is still ordinary SRO Blowfish (this program's Blowfish
// decodes it as-is); only the ROOT-BLOCK OFFSET differs. This is a property of
// the SailSafe container format, so recognising it here does not violate RULE 0
// (no per-server key is embedded - the key is still discovered by the oracle).
//
// Detection: an archive whose first 8 bytes equal kSailorSignature. That block
// is byte-identical across every SailSafe pk2 (it is a constant the client
// verifies with memcmp), which is exactly why it is a safe format signature.
// ---------------------------------------------------------------------------
constexpr u8 kSailorSignature[8] = { 0x44, 0x4A, 0x26, 0x2D, 0x44, 0x40, 0x57, 0x2C };
constexpr u64 kSailorRootBlockOffset = 0x33E;   ///< first directory block for SailSafe

/// The plaintext the checksum oracle encrypts. Note "Joymax Pak File" produces
/// the same first block (both begin "Joymax P"), so testing one is enough.
/// Obfuscated so the probe does not appear as plaintext in a strings dump.
inline std::string Pk2ChecksumProbe() { return OBF("Joymax Pack File"); }

/// How many bytes of Pk2RawHeader::Checksum are actually comparable.
constexpr size_t kPk2ChecksumCompareBytes = 3;

// ---------------------------------------------------------------------------
// Decoded types
// ---------------------------------------------------------------------------
enum class Pk2EntryType : u8 {
    Empty  = 0,
    Folder = 1,
    File   = 2
};

/// A single directory entry, decoded and annotated with where it physically
/// lives so it can be rewritten in place (rename / delete / resize).
struct Pk2Entry {
    Pk2EntryType type       = Pk2EntryType::Empty;
    std::wstring name;                 ///< display name (converted from ANSI)
    std::string  nameAnsi;             ///< exactly as stored on disk
    u64          createTime = 0;
    u64          modifyTime = 0;
    i64          offset     = 0;       ///< payload offset (file) / first block (folder)
    u32          size       = 0;       ///< payload size, files only
    i64          nextBlock  = 0;       ///< chain link, meaningful on slot 19

    // --- physical location of THIS entry inside the archive ---------------
    i64 blockOffset = 0;   ///< file offset of the containing 2560-byte block
    int slot        = 0;   ///< 0..19 within that block

    /// Absolute file offset of these 128 bytes.
    i64 entryFileOffset() const { return blockOffset + static_cast<i64>(slot) * 128; }

    bool isFile()   const { return type == Pk2EntryType::File; }
    bool isFolder() const { return type == Pk2EntryType::Folder; }
    bool isEmpty()  const { return type == Pk2EntryType::Empty; }

    /// "." and ".." self/parent pointers, which must never be shown to the user.
    bool isDotEntry() const { return nameAnsi == "." || nameAnsi == ".."; }

    /// A real, listable child.
    bool isListable() const { return !isEmpty() && !isDotEntry() && !name.empty(); }
};

/// Header fields decoded for the diagnostics dialog.
struct Pk2HeaderInfo {
    std::wstring signature;        ///< trimmed, printable form
    u8           version[4] = { 0, 0, 0, 0 };
    std::wstring versionText;      ///< "2.0.0.1"
    bool         encrypted = false;
    u8           checksum[16] = {};
    bool         signatureValid = false;
};

/// Result of a full archive walk (explicit, expensive - never done at open).
struct Pk2Diagnostics {
    Pk2HeaderInfo header;
    u64 archiveSize      = 0;
    u64 blockCount       = 0;   ///< number of 2560-byte block tables walked
    u64 folderCount      = 0;
    u64 fileCount        = 0;
    u64 emptyEntryCount  = 0;   ///< deleted / never used slots
    u64 totalPayloadBytes = 0;  ///< sum of live file sizes
    u64 deadBytes        = 0;   ///< bytes not reachable from any live entry
    double fragmentation = 0.0; ///< deadBytes / archiveSize, 0..1
    u64 largestFileSize  = 0;
    std::wstring largestFilePath;
};

/// Options for creating a brand-new archive.
struct Pk2CreateOptions {
    bool        encrypted = true;              ///< write Encrypted = 1
    std::string asciiKey  = "169841";          ///< key the new archive is sealed with
    BaseKeyId   baseKey   = BaseKeyId::Std;    ///< matching base key
};

} // namespace sro
