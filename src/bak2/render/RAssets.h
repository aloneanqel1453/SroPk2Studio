// SkillEffect Studio - render/RAssets.h
// Read-only asset access for the renderer. The integrator adapts the client module's VFS to IAssetSource;
// everything here only READS (pk2 / loose files are never written).
#pragma once

#include "Efp.h"
#include "RFormats.h"
#include "RImage.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace se::render {

enum class Arc : int { Data = 0, Particles = 1, Media = 2 };

class IAssetSource {
public:
    virtual ~IAssetSource() = default;
    // innerPath: backslash separated, relative to the archive root (L"res\\mob\\..." in Data,
    // L"skill\\europe\\x.efp" in Particles). Case-insensitive. false when missing.
    virtual bool Read(Arc arc, const std::wstring& innerPath, std::vector<std::uint8_t>& out) = 0;
};

std::wstring Cp949ToW(const std::string& s);       // raw file strings (CP949) -> UTF-16
std::string WToUtf8(const std::wstring& s);
std::wstring NormPath(const std::wstring& p);      // '/' -> '\', trims spaces and leading '\'
std::wstring LowerW(const std::wstring& s);
std::wstring DirOf(const std::wstring& p);         // "a\b\c.x" -> "a\b"
std::wstring JoinPath(const std::wstring& dir, const std::wstring& rel);

// decoded texture: float RGBA (efp_sim: float32 = byte / 255) for the CPU rasterizer + RGBA8 for GL
struct RTexture {
    int w = 0, h = 0;
    std::vector<float> rgba;      // w*h*4
    Image img;                    // RGBA8 (level 0)
    std::wstring path;
    // alpha statistics (model material decisions, sro-bmt-bumpflag-and-blend-gate)
    float midAlpha = 0, zeroAlpha = 0, meanRgb = 0;
    bool anyAlpha = false;
};

class AssetCache {
public:
    explicit AssetCache(IAssetSource* src) : m_src(src) {}
    IAssetSource* Source() const { return m_src; }
    bool Read(Arc arc, const std::wstring& path, std::vector<std::uint8_t>& out);
    // all return nullptr when missing / undecodable (negative results are cached too)
    std::shared_ptr<RTexture> Texture(Arc arc, const std::wstring& path);
    std::shared_ptr<const EfpFile> Efp(const std::wstring& path, std::string* err = nullptr);   // Particles
    std::shared_ptr<const BmsFile> Bms(Arc arc, const std::wstring& path);
    std::shared_ptr<const BmtFile> Bmt(Arc arc, const std::wstring& path);
    std::shared_ptr<const BskFile> Bsk(Arc arc, const std::wstring& path);
    std::shared_ptr<const BanFile> Ban(Arc arc, const std::wstring& path);
    std::shared_ptr<const BsrFile> Bsr(Arc arc, const std::wstring& path);
    void Clear();
    // memory cap: textures (float + 8-bit copies, the big part) and meshes / animations nobody holds any more are
    // dropped when the cache is above maxBytes; returns the approximate bytes after
    size_t Trim(size_t maxBytes);
    size_t ApproxBytes();
    std::vector<std::wstring> TakeWarnings();

private:
    template <class T, class F> std::shared_ptr<const T> Get(std::map<std::wstring, std::shared_ptr<const T>>& m, Arc arc,
                                                             const std::wstring& path, F parse);
    void Warn(const std::wstring& w);
    IAssetSource* m_src;
    std::mutex m_mx;
    std::map<std::wstring, std::shared_ptr<RTexture>> m_tex;
    std::map<std::wstring, std::shared_ptr<const EfpFile>> m_efp;
    std::map<std::wstring, std::shared_ptr<const BmsFile>> m_bms;
    std::map<std::wstring, std::shared_ptr<const BmtFile>> m_bmt;
    std::map<std::wstring, std::shared_ptr<const BskFile>> m_bsk;
    std::map<std::wstring, std::shared_ptr<const BanFile>> m_ban;
    std::map<std::wstring, std::shared_ptr<const BsrFile>> m_bsr;
    std::vector<std::wstring> m_warn;
};

}  // namespace se::render
