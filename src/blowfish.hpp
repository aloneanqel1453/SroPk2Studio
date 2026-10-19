#pragma once
// Blowfish (ECB) — port of SRO.Security.Blowfish (C#).
// Used by Silkroad .pk2 to encode/decode 2560-byte PackFileBlocks.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace pk2 {

class Blowfish {
public:
    Blowfish() = default;

    // Set up the cipher with a key. Default pk2 key is the ASCII string "169841".
    void initialize(std::span<const std::uint8_t> key);
    void initialize(std::string_view key);

    // Output length after zero-padding to the next multiple of 8.
    [[nodiscard]] static std::size_t output_length(std::size_t len) noexcept {
        return (len % 8 == 0) ? len : len + (8 - (len % 8));
    }

    // Encode: zero-pads input to a multiple of 8, then ECB-enciphers each block.
    [[nodiscard]] std::vector<std::uint8_t> encode(std::span<const std::uint8_t> data) const;
    // Decode: requires a length that is a non-zero multiple of 8.
    [[nodiscard]] std::vector<std::uint8_t> decode(std::span<const std::uint8_t> data) const;

private:
    void encipher(std::uint32_t& xl, std::uint32_t& xr) const noexcept;
    void decipher(std::uint32_t& xl, std::uint32_t& xr) const noexcept;
    [[nodiscard]] std::uint32_t f(std::uint32_t x) const noexcept;

    std::uint32_t p_[18]{};
    std::uint32_t s_[4][256]{};
};

} // namespace pk2
