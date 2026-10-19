// ============================================================================
//  SRO Asset Studio - core/AssetCipherSailor.cpp
//  See AssetCipherSailor.h for the format writeup.
// ============================================================================
#include "AssetCipherSailor.h"
#include "SailorKeyTable.h"   // kSailorKeyMasked[4096], kSailorKeyMaskSite
#include "Obfuscate.h"        // obf::Seed / obf::KeyByte (compile-time PRNG)

#include <array>
#include <cstring>

namespace sro {
namespace {

// The 4096-byte additive keystream is never stored in the clear (a `strings` or
// static-disassembly dump of the .exe must reveal nothing usable). SailorKeyTable.h
// ships it XOR-masked with the Obfuscate.h compile-time PRNG; we unmask it once,
// lazily, into a function-local buffer that is scrubbed when the program exits.
// The masked table and the mask keystream are separate byte streams, so neither
// the real key head (69 a1 85 a1 ...) nor any run of it appears in .rdata.
struct SailorKeyTable {
    std::array<u8, 4096> k;
    SailorKeyTable() {
        const u32 seed = obf::Seed(kSailorKeyMaskSite, 4096);
        for (size_t i = 0; i < 4096; ++i)
            k[i] = static_cast<u8>(kSailorKeyMasked[i] ^ obf::KeyByte(seed, i));
    }
    ~SailorKeyTable() {                       // best-effort scrub on shutdown
        volatile u8* p = k.data();
        for (size_t i = 0; i < 4096; ++i) p[i] = 0;
    }
};

// Unmasked keystream. Built on first use, reused thereafter.
inline const u8* SailorKey() {
    static const SailorKeyTable kTable;
    return kTable.k.data();
}

// The two Sailor encrypted signatures. Both are the FIRST plaintext bytes run
// through the same additive step  cipher[i] = (plain[i] + kSailorKey[i]) & 0xff,
// so they are fixed prefixes that never collide with the corresponding
// plaintext (which starts with the raw BOM ff fe, or the raw magic "JMXV").
//
// Textdata: plaintext begins with the UTF-16LE BOM ff fe -> { 0x68, 0x9F }.
inline u8 kSig0() { return static_cast<u8>((0xFF + SailorKey()[0]) & 0xFF); } // 0x68
inline u8 kSig1() { return static_cast<u8>((0xFE + SailorKey()[1]) & 0xFF); } // 0x9F

// Binary asset (navmesh *.nvm and any JMXV asset the shard encrypts this way):
// plaintext begins with the JMXV magic "JMXV" -> { 0xB3, 0xEE, 0xDD, 0xF7 }.
inline u8 kSigJ(size_t i) {
    static const char kJmxv[4] = { 'J', 'M', 'X', 'V' };
    return static_cast<u8>((static_cast<u8>(kJmxv[i]) + SailorKey()[i]) & 0xFF);
}
inline bool LooksEncryptedBom(const u8* d)  { return d[0] == kSig0() && d[1] == kSig1(); }
inline bool LooksEncryptedJmxv(const u8* d) {
    return d[0] == kSigJ(0) && d[1] == kSigJ(1) && d[2] == kSigJ(2) && d[3] == kSigJ(3);
}

// The unencrypted trailer the shard appends after the payload, as UTF-16LE.
// Kept as raw bytes so we do not depend on the host's wchar/encoding.
static const u8 kTrailer[] = {
    0x0D,0x00, 0x0A,0x00,                                     // "\r\n"
    'S',0x00,'a',0x00,'i',0x00,'l',0x00,'o',0x00,'r',0x00,    // "Sailor"
    ' ',0x00,
    'O',0x00,'n',0x00,'l',0x00,'i',0x00,'n',0x00,'e',0x00,    // "Online"
    ' ',0x00,
    'C',0x00,'u',0x00,'s',0x00,'t',0x00,'o',0x00,'m',0x00,    // "Custom"
    ' ',0x00,
    'E',0x00,'n',0x00,'c',0x00,'r',0x00,'y',0x00,'p',0x00,    // "Encryp"
    't',0x00,'i',0x00,'o',0x00,'n',0x00,                      // "tion"
};

// Locate the appended trailer so it can be stripped. The archived length is
// sometimes odd, which truncates the trailer's final 0x00 high byte, so we
// accept the marker with or without that last byte. Returns the byte offset of
// the trailer (start of "\r\n") or npos when absent.
size_t FindTrailer(const u8* d, size_t n) {
    const size_t full = sizeof(kTrailer);
    for (size_t tlen : { full, full - 1 }) {
        if (n < tlen) continue;
        const size_t at = n - tlen;
        if (std::memcmp(d + at, kTrailer, tlen) == 0) return at;
    }
    return static_cast<size_t>(-1);
}

// A decrypted payload must be valid UTF-16LE beginning with a BOM. We check the
// BOM and that the (even-length) high bytes are overwhelmingly 0x00, which is
// the fingerprint of the ASCII-dominated Silkroad textdata and cannot be
// produced by subtracting the table from an unrelated file.
bool LooksLikeDecryptedTextdata(const std::vector<u8>& p) {
    // A BOM (2 bytes) is the minimum: some numbered tables are empty and ship as
    // nothing but the encrypted BOM plus the appended trailer, so the decrypted
    // payload is exactly "ff fe". Accept that as a valid (empty) UTF-16LE file.
    if (p.size() < 2) return false;
    if (p[0] != 0xFF || p[1] != 0xFE) return false;     // UTF-16LE BOM
    const size_t n = p.size() & ~size_t(1);
    size_t sampled = 0, zeroHigh = 0;
    for (size_t i = 3; i < n; i += 2) {                 // high byte of each char
        ++sampled;
        if (p[i] == 0x00) ++zeroHigh;
    }
    if (sampled == 0) return true;
    // Silkroad textdata is ASCII-dominated; even files rich in Korean/Turkish
    // names sit well above 80% zero high bytes.
    return zeroHigh * 100 >= sampled * 80;
}

// A decrypted binary payload must carry the JMXV signature followed by a text
// version tag ("NVM 1000", "BSR 0109", ...). That is far stronger than the
// 4-byte magic gate the encrypted-signature check already passed, so a foreign
// file that merely happens to start with the encrypted-JMXV prefix is rejected.
bool LooksLikeDecryptedJmxv(const std::vector<u8>& p) {
    if (p.size() < 12) return false;
    if (!(p[0] == 'J' && p[1] == 'M' && p[2] == 'X' && p[3] == 'V')) return false;
    for (size_t i = 4; i < 12; ++i)                     // "NVM 1000" etc. is ASCII text
        if (p[i] < 0x20 || p[i] > 0x7E) return false;
    return true;
}

// Some tables are single-byte ANSI/EUC-KR text, not UTF-16 (e.g. regioninfo.txt
// "#TOWN\t...", dungeon/dungeoninfo.txt "1\t1\t\"Dungeon\\...\""). These carry NO
// BOM and NO fixed first byte, so they cannot be recognised by a signature - the
// only sound test is to subtract the key and see whether the result reads as
// Silkroad text. A real ANSI textdata table is dominated by printable ASCII with
// many tabs and CRLF line breaks (Korean names add high bytes, so we do not
// require pure ASCII, only that control/garbage bytes stay rare and structure is
// present). This is deliberately strict so an unrelated high-entropy file that
// is NOT this cipher can never pass.
bool LooksLikeDecryptedAnsiText(const std::vector<u8>& p) {
    const size_t n = p.size();
    if (n < 16) return false;
    // First byte of a Silkroad ANSI table is a normal text lead: a digit, '#',
    // '"', a letter, or whitespace. Never a control byte.
    const u8 c0 = p[0];
    const bool leadOk = (c0 == '#' || c0 == '"' || c0 == '\t' ||
                         (c0 >= '0' && c0 <= '9') ||
                         (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'));
    if (!leadOk) return false;

    const size_t sample = n < 4096 ? n : 4096;
    size_t printable = 0, tabs = 0, newlines = 0, ctrl = 0, high = 0;
    for (size_t i = 0; i < sample; ++i) {
        const u8 b = p[i];
        if (b == '\t') { ++tabs; ++printable; }
        else if (b == '\r' || b == '\n') { ++newlines; ++printable; }
        else if (b >= 0x20 && b < 0x7F) ++printable;
        else if (b >= 0x81) ++high;              // EUC-KR / high-ANSI lead/trail
        else ++ctrl;                             // NUL and other control bytes
    }
    // Structure: real tables have tabs (columns) and line breaks (rows).
    if (tabs == 0 || newlines == 0) return false;
    // Garbage/control bytes must be rare - a wrong subtraction produces ~1/8 of
    // them; genuine text has almost none (only the stray padding byte).
    if (ctrl * 100 > sample * 2) return false;
    // The bulk must be printable ASCII; Korean high bytes make up the rest.
    if ((printable + high) * 100 < sample * 92) return false;
    if (printable * 100 < sample * 40) return false;
    return true;
}

// Additive decrypt of the first `len` bytes into a fresh buffer.
std::vector<u8> SubtractKey(const u8* d, size_t len) {
    const u8* K = SailorKey();
    std::vector<u8> out(len);
    for (size_t i = 0; i < len; ++i)
        out[i] = static_cast<u8>((d[i] - K[i & 4095]) & 0xFF);
    return out;
}

} // namespace

std::vector<u8> SailorEncryptForTest(const std::vector<u8>& plain) {
    const u8* K = SailorKey();
    std::vector<u8> c(plain.size());
    for (size_t i = 0; i < plain.size(); ++i)
        c[i] = static_cast<u8>((plain[i] + K[i & 4095]) & 0xFF);
    return c;
}

bool SailorLooksEncrypted(const u8* data, size_t size) {
    if (!data || size < 4) return false;
    return LooksEncryptedBom(data) || LooksEncryptedJmxv(data);
}

bool SailorDecryptIfEncrypted(const std::wstring& /*fileName*/, std::vector<u8>& buf) {
    if (buf.size() < 4) return false;

    // Binary JMXV asset (navmesh *.nvm and any JMXV asset encrypted this way).
    // No trailer is appended to these, so the whole buffer is the payload.
    if (LooksEncryptedJmxv(buf.data())) {
        std::vector<u8> out = SubtractKey(buf.data(), buf.size());
        if (!LooksLikeDecryptedJmxv(out)) return false;     // self-verify on JMXV
        buf.swap(out);
        return true;
    }

    // Textdata (UTF-16LE, BOM). Everything before the appended plaintext trailer
    // (if present) is the payload.
    if (LooksEncryptedBom(buf.data())) {
        const size_t trailerAt = FindTrailer(buf.data(), buf.size());
        const size_t payloadLen =
            (trailerAt == static_cast<size_t>(-1)) ? buf.size() : trailerAt;

        std::vector<u8> out = SubtractKey(buf.data(), payloadLen);

        // Self-verify: accept only a result that is genuinely Silkroad textdata,
        // so a foreign file that happens to start with 0x68 0x9F is never mangled.
        if (!LooksLikeDecryptedTextdata(out)) return false;

        buf.swap(out);
        return true;
    }

    // ANSI/EUC-KR textdata (regioninfo.txt, dungeon/dungeoninfo.txt, ...). No BOM
    // and no fixed signature, so we cannot pre-screen by prefix: subtract the key
    // over the whole buffer and keep the result only if it self-verifies as
    // Silkroad ANSI text. The validator is strict (needs tabs, line breaks, almost
    // no control bytes), so a genuinely-plaintext or unrelated file is left as-is.
    // Skip anything that already begins with a known plaintext container magic so
    // a real JMXV/DDS/RIFF/OggS asset is never even trial-decrypted.
    {
        const u8* d = buf.data();
        const bool knownPlain =
            (d[0]=='J'&&d[1]=='M'&&d[2]=='X'&&d[3]=='V') ||   // JMXV*
            (d[0]=='D'&&d[1]=='D'&&d[2]=='S'&&d[3]==' ')  ||   // DDS
            (d[0]=='R'&&d[1]=='I'&&d[2]=='F'&&d[3]=='F')  ||   // RIFF/wav
            (d[0]=='O'&&d[1]=='g'&&d[2]=='g'&&d[3]=='S')  ||   // Ogg
            (d[0]==0xFF&&d[1]==0xFE) ||                        // raw UTF-16 BOM
            (d[0]==0x89&&d[1]=='P'&&d[2]=='N'&&d[3]=='G') ||   // PNG
            (d[0]=='B'&&d[1]=='M');                            // BMP
        if (!knownPlain) {
            // The same unencrypted UTF-16LE "…Sailor Online Custom Encryption"
            // trailer is appended to ANSI tables too, so strip it (in the raw,
            // still-encrypted buffer) before subtracting the key - otherwise it
            // decrypts to garbage tacked onto the end of the last row.
            const size_t trailerAt = FindTrailer(buf.data(), buf.size());
            const size_t payloadLen =
                (trailerAt == static_cast<size_t>(-1)) ? buf.size() : trailerAt;
            std::vector<u8> out = SubtractKey(buf.data(), payloadLen);
            if (LooksLikeDecryptedAnsiText(out)) {
                buf.swap(out);
                return true;
            }
        }
    }

    return false;
}

} // namespace sro
