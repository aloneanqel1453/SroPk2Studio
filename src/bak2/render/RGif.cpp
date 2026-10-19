// SkillEffect Studio - render/RGif.cpp
#include "RGif.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace se::render {

namespace {

struct Box { int lo = 0, hi = 0; };   // [lo, hi) into the sample array

void MedianCut(std::vector<std::uint32_t>& px, std::uint8_t pal[256][3], int& count) {
    std::vector<Box> boxes{{0, static_cast<int>(px.size())}};
    auto chan = [](std::uint32_t c, int k) { return static_cast<int>((c >> (8 * k)) & 0xFF); };
    while (boxes.size() < 256) {
        int best = -1, bestK = 0;
        long long bestScore = 0;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            const Box& b = boxes[i];
            if (b.hi - b.lo < 2) continue;
            int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
            for (int j = b.lo; j < b.hi; ++j)
                for (int k = 0; k < 3; ++k) { int v = chan(px[j], k); mn[k] = std::min(mn[k], v); mx[k] = std::max(mx[k], v); }
            for (int k = 0; k < 3; ++k) {
                long long score = static_cast<long long>(mx[k] - mn[k]) * (b.hi - b.lo);
                if (score > bestScore) { bestScore = score; best = static_cast<int>(i); bestK = k; }
            }
        }
        if (best < 0 || bestScore == 0) break;
        Box b = boxes[static_cast<std::size_t>(best)];
        std::sort(px.begin() + b.lo, px.begin() + b.hi, [&](std::uint32_t a, std::uint32_t c) { return chan(a, bestK) < chan(c, bestK); });
        int mid = (b.lo + b.hi) / 2;
        boxes[static_cast<std::size_t>(best)] = {b.lo, mid};
        boxes.push_back({mid, b.hi});
    }
    count = static_cast<int>(boxes.size());
    for (int i = 0; i < count; ++i) {
        long long s[3] = {0, 0, 0};
        const Box& b = boxes[static_cast<std::size_t>(i)];
        for (int j = b.lo; j < b.hi; ++j)
            for (int k = 0; k < 3; ++k) s[k] += chan(px[j], k);
        int n = std::max(1, b.hi - b.lo);
        for (int k = 0; k < 3; ++k) pal[i][k] = static_cast<std::uint8_t>(s[k] / n);
    }
    for (int i = count; i < 256; ++i) pal[i][0] = pal[i][1] = pal[i][2] = 0;
}

struct BitWriter {
    std::vector<std::uint8_t> bytes;
    std::uint32_t acc = 0;
    int n = 0;
    void Put(std::uint32_t code, int bits) {
        acc |= code << n;
        n += bits;
        while (n >= 8) { bytes.push_back(static_cast<std::uint8_t>(acc)); acc >>= 8; n -= 8; }
    }
    void Flush() { if (n > 0) { bytes.push_back(static_cast<std::uint8_t>(acc)); acc = 0; n = 0; } }
};

void Lzw(const std::vector<std::uint8_t>& idx, std::vector<std::uint8_t>& out) {
    const int minCode = 8;
    const std::uint32_t clear = 1u << minCode;
    std::vector<std::uint16_t> tree(4096 * 256, 0);
    BitWriter bw;
    int codeSize = minCode + 1;
    std::uint32_t maxCode = clear + 1;
    bw.Put(clear, codeSize);
    int cur = -1;
    for (std::uint8_t k : idx) {
        if (cur < 0) { cur = k; continue; }
        std::uint16_t& nx = tree[static_cast<std::size_t>(cur) * 256 + k];
        if (nx) { cur = nx; continue; }
        bw.Put(static_cast<std::uint32_t>(cur), codeSize);
        nx = static_cast<std::uint16_t>(++maxCode);
        if (maxCode >= (1u << codeSize)) ++codeSize;
        if (maxCode == 4095) {
            bw.Put(clear, codeSize);
            std::fill(tree.begin(), tree.end(), static_cast<std::uint16_t>(0));
            codeSize = minCode + 1;
            maxCode = clear + 1;
        }
        cur = k;
    }
    if (cur >= 0) bw.Put(static_cast<std::uint32_t>(cur), codeSize);
    bw.Put(clear, codeSize);
    bw.Put(clear + 1, minCode + 1);
    bw.Flush();
    out.push_back(static_cast<std::uint8_t>(minCode));
    for (std::size_t i = 0; i < bw.bytes.size(); i += 255) {
        std::size_t n = std::min<std::size_t>(255, bw.bytes.size() - i);
        out.push_back(static_cast<std::uint8_t>(n));
        out.insert(out.end(), bw.bytes.begin() + static_cast<std::ptrdiff_t>(i), bw.bytes.begin() + static_cast<std::ptrdiff_t>(i + n));
    }
    out.push_back(0);
}

}  // namespace

bool GifWriter::Begin(const std::wstring& path, int w, int h, int delayCs, bool loop) {
    m_path = path;
    m_w = w; m_h = h; m_delay = delayCs; m_frames = 0;
    m_buf.clear();
    const char* sig = "GIF89a";
    m_buf.insert(m_buf.end(), sig, sig + 6);
    Put16(static_cast<std::uint16_t>(w)); Put16(static_cast<std::uint16_t>(h));
    Put8(0x70); Put8(0); Put8(0);   // no global table, 8-bit colour resolution
    if (loop) {
        const std::uint8_t ext[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00};
        m_buf.insert(m_buf.end(), ext, ext + sizeof(ext));
    }
    m_open = w > 0 && h > 0;
    return m_open;
}

bool GifWriter::AddFrame(const Image& img, int delayCs) {
    if (!m_open || img.w != m_w || img.h != m_h || img.Empty()) return false;
    const std::size_t n = static_cast<std::size_t>(m_w) * m_h;
    std::vector<std::uint32_t> sample;
    std::size_t step = std::max<std::size_t>(1, n / 60000);
    for (std::size_t i = 0; i < n; i += step) {
        const std::uint8_t* p = &img.rgba[i * 4];
        sample.push_back(p[0] | (p[1] << 8) | (p[2] << 16));
    }
    std::uint8_t pal[256][3];
    int count = 0;
    MedianCut(sample, pal, count);
    std::vector<std::int16_t> cache(32768, -1);
    std::vector<std::uint8_t> idx(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* p = &img.rgba[i * 4];
        int key = ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3);
        if (cache[static_cast<std::size_t>(key)] < 0) {
            int best = 0, bd = 1 << 30;
            for (int c = 0; c < count; ++c) {
                int dr = p[0] - pal[c][0], dg = p[1] - pal[c][1], db = p[2] - pal[c][2];
                int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                if (d < bd) { bd = d; best = c; }
            }
            cache[static_cast<std::size_t>(key)] = static_cast<std::int16_t>(best);
        }
        idx[i] = static_cast<std::uint8_t>(cache[static_cast<std::size_t>(key)]);
    }
    Put8(0x21); Put8(0xF9); Put8(4); Put8(0x04);   // GCE, disposal 1 (keep)
    Put16(static_cast<std::uint16_t>(delayCs >= 0 ? delayCs : m_delay));
    Put8(0); Put8(0);
    Put8(0x2C); Put16(0); Put16(0); Put16(static_cast<std::uint16_t>(m_w)); Put16(static_cast<std::uint16_t>(m_h));
    Put8(0x87);   // local colour table, 256 entries
    for (int i = 0; i < 256; ++i) { Put8(pal[i][0]); Put8(pal[i][1]); Put8(pal[i][2]); }
    std::vector<std::uint8_t> data;
    Lzw(idx, data);
    m_buf.insert(m_buf.end(), data.begin(), data.end());
    ++m_frames;
    return true;
}

bool GifWriter::End(std::string& err) {
    if (!m_open) { err = "not open"; return false; }
    Put8(0x3B);
    m_open = false;
    std::wstring tmp = m_path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot create file"; return false; }
    DWORD wr = 0;
    BOOL ok = WriteFile(h, m_buf.data(), static_cast<DWORD>(m_buf.size()), &wr, nullptr) && wr == m_buf.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), m_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        err = "write failed";
        return false;
    }
    m_buf.clear();
    return true;
}

}  // namespace se::render
