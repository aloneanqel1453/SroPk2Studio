// ============================================================================
//  SRO Asset Studio - core/Obfuscate.h
//
//  Compile-time string obfuscation. A literal wrapped in OBF("...") is stored in
//  the binary as XOR-encrypted bytes (a per-site rolling key) and decrypted only
//  at runtime, into a scratch buffer that is wiped when it goes out of scope. So
//  a `strings` dump or a static disassembly of the .exe shows only noise where a
//  key, an RSA PEM, a container magic or a derivation label used to sit.
//
//  WHY: the program embeds format-level secrets (the VSROPLUS/L33T RSA private
//  keys, the payload XOR key, the "L33T-FILESTREAM"/"L33T-SIGN" KDF labels, the
//  "JoyMax File Manager!" signature). In plaintext those hand a reader the whole
//  decrypt recipe for every supported game. Obfuscated, dumping the exe reveals
//  nothing about how an archive is opened. See [[sro-asset-studio-key-privacy]]
//  and [[sroviewer-binary-hardening]].
//
//  USAGE:
//     std::string pem = OBF("-----BEGIN RSA PRIVATE KEY-----\n...");
//     const char*  s  = OBF("L33T-FILESTREAM").c_str();   // valid until the
//                                                          // temporary dies
//  OBF() yields an ObfBuf (an owning, self-wiping std::string-like holder).
//  Take .str() / .c_str() / .data() / .size() from it. Keep the ObfBuf alive as
//  long as you use the pointer (bind it to a named local for anything non-trivial).
//
//  The key is derived from __COUNTER__ + length, so every call site encrypts with
//  a different keystream - there is no single XOR byte to spot across the binary.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace sro { namespace obf {

// --- per-site keystream ----------------------------------------------------
// A cheap, compile-time PRNG seeded by a site-unique value. Not cryptographic -
// it only has to defeat `strings` and a casual disassembly, and to differ per
// call site so there is no constant to grep for.
constexpr std::uint32_t Seed(std::uint32_t site, std::size_t len) {
    std::uint32_t x = site * 2654435761u + static_cast<std::uint32_t>(len) * 40503u + 0x9E3779B9u;
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
    return x ? x : 0xA5A5A5A5u;
}
constexpr std::uint8_t KeyByte(std::uint32_t seed, std::size_t i) {
    std::uint32_t x = seed + static_cast<std::uint32_t>(i) * 0x9E3779B9u;
    x ^= x >> 13; x *= 0x5BD1E995u; x ^= x >> 15;
    return static_cast<std::uint8_t>(x);
}

// --- compile-time encrypted storage ----------------------------------------
// The literal is XOR-encrypted at compile time into this array (N includes the
// trailing NUL, which we also encrypt so the plaintext length is not obvious).
template <std::size_t N, std::uint32_t Site>
struct Obfuscated {
    std::uint8_t data[N];

    constexpr explicit Obfuscated(const char (&lit)[N]) : data{} {
        const std::uint32_t seed = Seed(Site, N);
        for (std::size_t i = 0; i < N; ++i)
            data[i] = static_cast<std::uint8_t>(lit[i]) ^ KeyByte(seed, i);
    }
};

// --- runtime holder: decrypts on construction, wipes on destruction --------
// Owns the plaintext for its lifetime. Movable, non-copyable. Implicitly
// convertible to std::string / const char* for drop-in use at call sites.
class ObfBuf {
public:
    template <std::size_t N, std::uint32_t Site>
    ObfBuf(const Obfuscated<N, Site>& enc) {
        const std::uint32_t seed = Seed(Site, N);
        m_str.resize(N - 1);   // drop the (encrypted) trailing NUL from the value
        for (std::size_t i = 0; i + 1 < N; ++i)
            m_str[i] = static_cast<char>(enc.data[i] ^ KeyByte(seed, i));
    }
    ObfBuf(ObfBuf&&) noexcept = default;
    ObfBuf& operator=(ObfBuf&&) noexcept = default;
    ObfBuf(const ObfBuf&) = delete;
    ObfBuf& operator=(const ObfBuf&) = delete;

    ~ObfBuf() {
        // Best-effort scrub so the plaintext does not linger on the heap after use.
        volatile char* p = m_str.empty() ? nullptr : &m_str[0];
        for (std::size_t i = 0; i < m_str.size(); ++i) p[i] = 0;
    }

    const std::string& str()  const { return m_str; }
    const char*        c_str() const { return m_str.c_str(); }
    const char*        data()  const { return m_str.data(); }
    std::size_t        size()  const { return m_str.size(); }
    operator const std::string&() const { return m_str; }

private:
    std::string m_str;
};

}} // namespace sro::obf

// OBF(literal) -> ObfBuf. __COUNTER__ makes each call site's keystream unique.
//
// The encrypted bytes are forced into a `static constexpr` object inside an
// immediately-invoked lambda. That guarantees the compiler materialises the
// XOR-encrypted array (not the plaintext literal) in .rdata: the plaintext
// exists only transiently at runtime inside the returned ObfBuf. Without the
// `static constexpr` step, an optimising build may keep the original literal.
#define OBF(str)                                                               \
    ([] {                                                                       \
        constexpr std::uint32_t kSite = (__COUNTER__ + 1u);                     \
        static constexpr ::sro::obf::Obfuscated<sizeof(str), kSite> kEnc{str};  \
        return ::sro::obf::ObfBuf(kEnc);                                         \
    }())
