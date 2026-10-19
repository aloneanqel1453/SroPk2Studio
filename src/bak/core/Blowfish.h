// ============================================================================
//  SRO Asset Studio - core/Blowfish.h
//
//  The SRO variant of Blowfish used to encrypt JMXPACK block tables.
//  Little-endian words; ported from the reference C# implementation in
//  ../SROPK2-API-SRC/.../Blowfish.cs (behaviour confirmed against real files).
//
//  Two ways to get a working cipher:
//    1. From an ASCII key + a 10-byte "base key" (the normal game scheme).
//    2. From a RAW 4168-byte (P,S) state recovered out of a running client's
//       memory - see KeyFinder "Scanner B". This is the only route that works
//       when the server's key derivation is unknown (e.g. Origin's OriDB.lib).
//
//  VERIFIED test vectors (see SPEC.md section 1), exercised by --selftest:
//    ascii "169841" x base STD  ->  Encode("Joymax Pack File")[0..2] == d8 da 30
//    ... and the decrypted root block at offset 256 starts 01 2e 00 00 00 00 00 00
//
//  Implementation owner: Agent B (core).
// ============================================================================
#pragma once

#include "../Common.h"

namespace sro {

// ---------------------------------------------------------------------------
// Raw cipher state.
//
// The classic SRO Blowfish object is laid out in client memory exactly like
// this - 18 P-array words immediately followed by 4 x 256 S-box words, for a
// total of 4168 contiguous bytes. Scanner B walks process memory treating
// every 4-byte-aligned window as one of these.
// ---------------------------------------------------------------------------
struct BlowfishState {
    u32 P[18];
    u32 S[4][256];
};

static_assert(sizeof(BlowfishState) == 4168, "BlowfishState must be exactly 4168 bytes");

/// Size of the raw state, in bytes. Scanner B scans with this stride.
constexpr size_t kBlowfishStateSize = sizeof(BlowfishState);

// ---------------------------------------------------------------------------
// Base keys ("key modification" tables). SPEC.md section 1.
// ---------------------------------------------------------------------------
enum class BaseKeyId : int {
    Std = 0,      ///< 03 F8 E4 44 88 99 3F 64 FE 35  - stock Silkroad
    Paradise,     ///< A8 19 72 BF 87 D2 0A FD 37 54
    Zero,         ///< all zeros == "raw key" mode, the ASCII key is used verbatim
    Custom        ///< caller supplies its own bytes (e.g. found by a binary scan)
};

constexpr size_t kBaseKeyLength = 10;

/// The three built-in base keys, each kBaseKeyLength bytes.
extern const u8 kBaseKeyStd[kBaseKeyLength];
extern const u8 kBaseKeyParadise[kBaseKeyLength];
extern const u8 kBaseKeyZero[kBaseKeyLength];

/// Pointer to a built-in base key; nullptr for BaseKeyId::Custom.
const u8* BaseKeyBytes(BaseKeyId id);
/// Short label for the UI / logs ("STD", "PARADISE", "ZERO", "OZEL").
const wchar_t* BaseKeyName(BaseKeyId id);

// ---------------------------------------------------------------------------
// Blowfish
// ---------------------------------------------------------------------------
class Blowfish {
public:
    /// Maximum Blowfish key length the game uses.
    static constexpr size_t kMaxKeyBytes = 56;

    Blowfish();

    // --- initialisation ----------------------------------------------------

    /// Standard Blowfish key schedule over an arbitrary byte key.
    /// `len` must be 1..kMaxKeyBytes; anything else leaves the object invalid.
    /// Returns true on success.
    bool initializeRaw(const u8* key, size_t len);

    /// The game scheme:  bf_key[i] = ascii[i] ^ base[i]  (base[i] = 0 past baseLen),
    /// clamped to min(strlen(ascii), 56) bytes, then the standard schedule.
    bool initializeGameKey(const std::string& asciiKey, const u8* baseKey, size_t baseLen);

    /// Convenience overload for the built-in base keys.
    /// BaseKeyId::Custom is rejected (returns false) - use the pointer overload.
    bool initializeGameKey(const std::string& asciiKey, BaseKeyId baseId);

    /// Adopt an already-scheduled (P,S) state verbatim - no key schedule runs.
    /// This is how Scanner B revives a cipher whose key derivation is unknown.
    bool initializeFromState(const BlowfishState& st);

    /// Same as above from a raw 4168-byte buffer. `reversedLayout` interprets
    /// the buffer as (S then P) instead of (P then S); some builds order it
    /// that way, so the scanner tries both.
    bool initializeFromStateBytes(const u8* bytes, size_t len, bool reversedLayout = false);

    /// Derive the game's Blowfish key bytes without initialising anything.
    /// `out` must have room for kMaxKeyBytes. Returns the produced length,
    /// or 0 when asciiKey is empty.
    static size_t DeriveGameKey(const std::string& asciiKey,
                                const u8* baseKey, size_t baseLen,
                                u8* out /*[kMaxKeyBytes]*/);

    /// True once any initialize*() succeeded. All crypt calls are no-ops until then.
    bool valid() const { return m_valid; }

    /// Direct read access to the scheduled state (for persisting a Scanner B hit).
    const BlowfishState& state() const { return m_state; }

    /// Copy the state out as raw bytes (kBlowfishStateSize bytes).
    void exportState(u8* dst /*[4168]*/) const;

    // --- crypt -------------------------------------------------------------

    /// Padded output length: next multiple of 8.
    static size_t OutputLength(size_t length) {
        return (length % 8 == 0) ? length : length + (8 - (length % 8));
    }

    /// Encrypt `n` bytes into `out`, zero-padding to OutputLength(n).
    /// Returns false when the object is invalid or n == 0.
    bool encode(const u8* in, size_t n, std::vector<u8>& out) const;

    /// Decrypt `n` bytes into `out`. `n` must be a non-zero multiple of 8.
    bool decode(const u8* in, size_t n, std::vector<u8>& out) const;

    /// In-place variants. `n` must be a multiple of 8 (no padding is applied).
    void encodeInPlace(u8* p, size_t n) const;
    void decodeInPlace(u8* p, size_t n) const;

    /// Single 8-byte block, in place. The hot path for the block-table decrypt
    /// and for Scanner B's inner loop - keep it cheap.
    void encodeBlock(u8* block8) const;
    void decodeBlock(u8* block8) const;

    /// Convenience: encode an ASCII string (used by the checksum oracle,
    /// which encodes "Joymax Pack File" and compares the first 3 bytes).
    bool encodeString(const char* text, std::vector<u8>& out) const;

private:
    void encipher(u32& xl, u32& xr) const;
    void decipher(u32& xl, u32& xr) const;
    u32  feistel(u32 x) const;

    BlowfishState m_state;
    bool          m_valid;
};

} // namespace sro
