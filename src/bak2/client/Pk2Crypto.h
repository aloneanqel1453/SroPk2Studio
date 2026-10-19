// SkillEffect Studio v3 - client/Pk2Crypto.h
// JMXPACK (.pk2 / .lib) cipher pieces shared by Pk2Archive and KeyFinder: the SRO Blowfish (textbook key schedule,
// blocks read as two little-endian words), key specs and the two key oracles.
//
// Key spec (same strings as core/Pk2.h, so a key found here opens a core Pk2Reader too):
//   "169841"              ascii key XOR the published STD base key 03 F8 E4 44 88 99 3F 64 FE 35
//   "ascii@PARADISE"      ... XOR A8 19 72 BF 87 D2 0A FD 37 54
//   "ascii@ZERO"          the ascii bytes are the Blowfish key ("raw key" mode of long keys)
//   "ascii@<hex>"         ... XOR a base key read from a client file (8..16 bytes)
//   "raw:<hex>"           Blowfish key bytes (e.g. a non-printable key)
// bf_key[i] = ascii[i] ^ base[i] for i < len(base), ascii[i] after that.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace se::client {

constexpr std::uint32_t kPk2HeaderSize = 256;
constexpr std::uint32_t kPk2BlockSize = 2560;  // 20 entries
constexpr std::uint32_t kPk2EntrySize = 128;
constexpr std::uint32_t kPk2RootOffset = 256;

class SroBlowfish {
public:
    bool Init(const std::uint8_t* key, size_t len);  // false when len == 0 or > 56
    void EncodeBlock(std::uint8_t* b) const;
    void DecodeBlock(std::uint8_t* b) const;
    void Decode(std::uint8_t* b, size_t n) const;  // n multiple of 8

private:
    std::uint32_t F(std::uint32_t x) const;
    void Enc(std::uint32_t& xl, std::uint32_t& xr) const;
    void Dec(std::uint32_t& xl, std::uint32_t& xr) const;
    std::uint32_t P[18]{};
    std::uint32_t S[4][256]{};
};

// spec -> Blowfish key bytes; false when the spec is malformed
bool KeySpecBytes(const std::string& spec, std::vector<std::uint8_t>& key);
// "169841" / "169841@PARADISE" ... ; a plain ascii spec without '@' and without "raw:"
bool KeySpecIsPlainAscii(const std::string& spec);
// label safe for logs / screenshots: never contains the key, only its form ("STD tabanı", "dosyadan taban" ...)
std::wstring KeySpecFormTr(const std::string& spec);

// The 256-byte header + the encrypted root block of an archive (read with GENERIC_READ only).
struct Pk2Probe {
    bool ok = false;           // signature "JoyMax File Manager!" and both blocks read
    bool encrypted = true;     // header byte 34
    std::uint8_t checksum[3]{};  // header bytes 35..37
    std::uint64_t fileSize = 0, lastWrite = 0;
    std::uint8_t header[kPk2HeaderSize]{};
    std::uint8_t root[kPk2BlockSize]{};
    std::wstring whyTr, whyEn;  // when !ok

    bool Load(const std::wstring& path);
    // Oracle 2 (strong): decrypting the root block gives entry 0 = folder "." and every entry type <= 2.
    // Decrypts one 8-byte block first (fast reject), the whole block only on a hit.
    bool RootOracle(const SroBlowfish& bf) const;
    // Oracle 1 (weak, 3 bytes): Encode("Joymax Pack File")[0..2] == header checksum. Some servers changed the probe.
    bool ChecksumOracle(const SroBlowfish& bf) const;
    bool TestSpec(const std::string& spec) const;   // root oracle (or no encryption)
    bool TestKey(const std::uint8_t* key, size_t len) const;
    std::string Fingerprint() const;  // size + checksum + mtime: identifies the archive content for the session cache
};

}  // namespace se::client
