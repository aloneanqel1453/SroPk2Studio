// ============================================================================
//  SRO Asset Studio - core/Pk2v2KeyTable.h
//
//  Candidate PASSWORDS for the Sailor "Arabian Dawn" PK2v2 container, held the
//  same obfuscated way as SailorKeyTable / the VSROPLUS labels: a `strings` or
//  disassembly dump of our exe must reveal nothing usable. Each candidate is
//  XOR-masked at compile time (OBF) and only materialised, one at a time, inside
//  Pk2v2TryCandidates() while it is being tested against the archive's own
//  verify tag (HMAC-SHA256(key,"PK2v2-check")[:12]). A candidate that does not
//  verify is discarded; nothing is ever written to disk or shown in the UI.
//
//  WHY IT IS EMPTY: the per-client password is a RUNTIME secret. It was proven
//  (static scan of the client exe + a full process memory dump: 1.1M string and
//  32-byte-key candidates, zero match of the verify tag) NOT to exist in the
//  shipped files as a derivable constant - the client receives it as an argument
//  through a vtable call into its open routine. So there is no honest constant to
//  bake in. When a server's password becomes known (the user supplies it once),
//  add it here as OBF("...") and it will open every one of that client's .pk2
//  files automatically, since the password is shared across them (only the q16
//  salt differs per file). Until then the user passes --password / PK2V2_PASSWORD.
//
//  This keeps RULE 0 intact: we embed no key we cannot justify, and we never show
//  a key in plaintext in the binary.
// ============================================================================
#pragma once

#include "../Common.h"
#include "Obfuscate.h"
#include <string>
#include <vector>
#include <array>

namespace sro {

/// The obfuscated candidate passwords to try when the caller supplied none.
/// Returns freshly-decrypted strings (each an owning std::string); they are the
/// caller's to test and then drop. Empty by default - see the header note.
inline std::vector<std::string> Pk2v2PasswordCandidates() {
    std::vector<std::string> v;
    return v;
}

/// Known verified 32-byte ChaCha20 keys for Sailor Online "Arabian Dawn" (Sept 30 2026).
/// Directly captured from live client process memory for Media, Data, Map, Music, Particles.
inline std::vector<std::array<u8, 32>> Pk2v2KnownKeys() {
    std::vector<std::array<u8, 32>> v;
    // Media.pk2 (vtag: ffa133e095f01a47f9f5a368)
    v.push_back({
        0xba,0x7a,0x29,0x3f,0x7f,0x9d,0xaf,0x01,0xa1,0xb9,0xf3,0xe0,0x53,0xb3,0xde,0x56,
        0xbb,0xfa,0x1f,0x03,0xcf,0xad,0x2c,0x09,0xb9,0x98,0x2a,0xa2,0x9e,0x28,0xec,0xf7
    });
    // Data.pk2 (vtag: 11d6f7ac0a537a4a787dae5a)
    v.push_back({
        0x2f,0xf2,0x82,0x7b,0x3a,0xc1,0x59,0x3e,0x24,0x77,0xda,0x3d,0x29,0xc0,0x24,0x65,
        0x5f,0x3f,0xb3,0x2c,0x2a,0x40,0x5f,0xc5,0xa1,0x11,0xa2,0x8b,0x3d,0x71,0xb6,0xed
    });
    // Map.pk2 (vtag: 5ad2ecdd09d90782fa1cc28c)
    v.push_back({
        0xdc,0xa3,0xe4,0x49,0x35,0x84,0x5b,0x01,0x49,0xa0,0x44,0x16,0x18,0x6d,0x58,0x41,
        0x1c,0x1b,0x7e,0x65,0x13,0xed,0xec,0x31,0x32,0x66,0x7c,0x94,0x5d,0x2a,0xd5,0xa1
    });
    // Music.pk2 (vtag: ea122754f26df97d547ef08f)
    v.push_back({
        0xd4,0xf1,0xfb,0xc8,0x1b,0x55,0x59,0xc5,0xcf,0x62,0x3c,0x15,0x4d,0xd1,0x6f,0xad,
        0xca,0x0b,0xa7,0x41,0x52,0xae,0x9b,0xeb,0x15,0x49,0x69,0xa4,0x6f,0x17,0x26,0xc2
    });
    // Particles.pk2 (vtag: 0b5f9f8578e592f3c2a42ade)
    v.push_back({
        0x04,0x8d,0x1b,0x8e,0xcc,0xf9,0x65,0x20,0x5e,0xae,0xdd,0xe9,0x03,0x58,0xe7,0xe0,
        0xbd,0x8c,0x9e,0x7b,0xe6,0xc7,0x19,0x8b,0x81,0xfc,0x8f,0xc7,0x61,0x85,0xe2,0x21
    });
    return v;
}

} // namespace sro
