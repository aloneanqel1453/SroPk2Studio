// SkillEffect Studio - render/RBytes.h
// Bounds-checked little-endian reader for the JMXV binary formats (latches a fail flag, never throws).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace se::render {

class RReader {
public:
    RReader(const std::uint8_t* d, std::size_t n) : m_d(d), m_n(n) {}
    bool Fail() const { return m_fail; }
    bool Ok() const { return !m_fail; }
    std::size_t Pos() const { return m_pos; }
    std::size_t Size() const { return m_n; }
    std::size_t Left() const { return m_pos <= m_n ? m_n - m_pos : 0; }
    void ClearFail() { m_fail = false; }
    bool Seek(std::size_t p) { if (p > m_n) { m_fail = true; return false; } m_pos = p; return true; }
    bool SeekSection(std::uint64_t off) { if (off == 0 || off > m_n) return false; m_pos = static_cast<std::size_t>(off); return true; }
    bool Skip(std::size_t k) { if (k > Left()) { m_fail = true; m_pos = m_n; return false; } m_pos += k; return true; }
    bool Raw(void* dst, std::size_t k) {
        if (k > Left()) { m_fail = true; m_pos = m_n; if (dst && k) std::memset(dst, 0, k); return false; }
        if (k) std::memcpy(dst, m_d + m_pos, k);
        m_pos += k;
        return true;
    }
    const std::uint8_t* Ptr() const { return m_d + m_pos; }
    std::uint8_t U8() { std::uint8_t v = 0; Raw(&v, 1); return v; }
    std::uint16_t U16() { std::uint16_t v = 0; Raw(&v, 2); return v; }
    std::uint32_t U32() { std::uint32_t v = 0; Raw(&v, 4); return v; }
    std::int32_t I32() { std::int32_t v = 0; Raw(&v, 4); return v; }
    float F32() { float v = 0; Raw(&v, 4); return v; }
    // u32 length + bytes (raw bytes, usually CP949 / ASCII)
    std::string Str(std::uint32_t maxLen = 1u << 20) {
        std::uint32_t n = U32();
        if (m_fail || n > maxLen || n > Left()) { m_fail = true; return {}; }
        std::string s(reinterpret_cast<const char*>(m_d + m_pos), n);
        m_pos += n;
        return s;
    }

private:
    const std::uint8_t* m_d;
    std::size_t m_n;
    std::size_t m_pos = 0;
    bool m_fail = false;
};

}  // namespace se::render
