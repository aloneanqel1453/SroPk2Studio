// SkillEffect Studio - render/RGif.h
// Animated GIF writer (dependency-free): per-frame median-cut palette (256), 5-5-5 nearest-colour cache, LZW.
// Used for "save the preview as GIF" (skill / effect clips to share). Atomic: <path>.tmp then rename.
#pragma once

#include "RImage.h"

#include <cstdint>
#include <string>
#include <vector>

namespace se::render {

class GifWriter {
public:
    bool Begin(const std::wstring& path, int w, int h, int delayCs = 5, bool loop = true);   // delay in 1/100 s
    bool AddFrame(const Image& img, int delayCs = -1);
    bool End(std::string& err);
    int Frames() const { return m_frames; }

private:
    void Put8(std::uint8_t b) { m_buf.push_back(b); }
    void Put16(std::uint16_t v) { m_buf.push_back(static_cast<std::uint8_t>(v)); m_buf.push_back(static_cast<std::uint8_t>(v >> 8)); }
    std::wstring m_path;
    std::vector<std::uint8_t> m_buf;
    int m_w = 0, m_h = 0, m_delay = 5, m_frames = 0;
    bool m_open = false;
};

}  // namespace se::render
