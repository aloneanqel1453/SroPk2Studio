#include "file_types.hpp"

#include "text.hpp"

#include <cstring>

namespace pk2 {
namespace {

// Palette (matches the UI theme).
constexpr std::uint32_t C_TXT  = 0x9FB0C8;
constexpr std::uint32_t C_DATA = 0x8EA0BC;
constexpr std::uint32_t C_CFG  = 0x7E8AA0;
constexpr std::uint32_t C_MESH = 0x9B6BFF;
constexpr std::uint32_t C_MAT  = 0xFFC24B;
constexpr std::uint32_t C_TEX  = 0xFF6FD8;
constexpr std::uint32_t C_SKEL = 0x36E0C8;
constexpr std::uint32_t C_ANIM = 0xFFC24B;
constexpr std::uint32_t C_RES  = 0x6AA8FF;
constexpr std::uint32_t C_CPD  = 0x6AA8FF;
constexpr std::uint32_t C_EFF  = 0xFF6FD8;
constexpr std::uint32_t C_CAM  = 0x8B74FF;
constexpr std::uint32_t C_MAP  = 0xFF9F47;
constexpr std::uint32_t C_IMG  = 0xFF6FD8;
constexpr std::uint32_t C_SND  = 0x45E0A0;
constexpr std::uint32_t C_SCR  = 0x5AD1FF;
constexpr std::uint32_t C_UI   = 0x5AD1FF;
constexpr std::uint32_t C_ARC  = 0x9BA7BC;
constexpr std::uint32_t C_UNK  = 0x7E8AA0;

std::wstring ext_of(std::wstring_view name) {
    std::size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos)
        return {};
    return to_lower(name.substr(dot + 1));
}

TypeInfo info(FileCategory c, std::uint32_t color, const char* key, const wchar_t* sig = L"") {
    return TypeInfo{c, color, key, sig};
}

} // namespace

TypeInfo classify(std::wstring_view name) {
    std::wstring e = ext_of(name);
    // SV.T is the client version file (a Blowfish-encrypted number), not a JMXVMAPT terrain texture.
    {
        std::size_t cut = name.find_last_of(L"/\\");
        std::wstring_view b = (cut == std::wstring_view::npos) ? name : name.substr(cut + 1);
        if (b.size() == 4 && (b[0] | 0x20) == L's' && (b[1] | 0x20) == L'v' && b[2] == L'.' && (b[3] | 0x20) == L't')
            return info(FileCategory::Unknown, C_DATA, "type_version");
    }
    // Copies the client keeps next to an asset ("x.ddj.tmp", "x.txt.bak"): the type of what they copy.
    if ((e == L"tmp" || e == L"bak" || e == L"old") && name.size() > e.size() + 1) {
        const std::wstring_view inner = name.substr(0, name.size() - e.size() - 1);
        if (inner.find_last_of(L'.') != std::wstring_view::npos && inner.find_last_of(L'.') + 1 < inner.size() &&
            inner.find_last_of(L"/\\") < inner.find_last_of(L'.') + 1)
            return classify(inner);
    }
    if (e == L"pk2" || e == L"pk3" || e == L"lib" || e == L"vp") return info(FileCategory::Archive, C_ARC, "type_archive");
    if (e == L"txt")  return info(FileCategory::Text, C_TXT, "type_text");
    if (e == L"csv" || e == L"xml" || e == L"json" || e == L"ini" || e == L"cfg" || e == L"log")
        return info(FileCategory::Text, C_TXT, "type_text");
    // Shader and C sources the client ships as text.
    if (e == L"psh" || e == L"vsh" || e == L"fx" || e == L"hlsl" || e == L"c" || e == L"h" || e == L"cpp")
        return info(FileCategory::Script, C_SCR, "type_source");
    if (e == L"exe" || e == L"dll") return info(FileCategory::Unknown, C_DATA, "type_program");
    if (e == L"lua")  return info(FileCategory::Script, C_SCR, "type_script");
    if (e == L"bms")  return info(FileCategory::Mesh, C_MESH, "type_mesh", L"JMXVBMS");
    if (e == L"bmt")  return info(FileCategory::Material, C_MAT, "type_material", L"JMXVBMT");
    if (e == L"ddj")  return info(FileCategory::Texture, C_TEX, "type_texture", L"JMXVDDJ");
    if (e == L"bsk")  return info(FileCategory::Skeleton, C_SKEL, "type_skeleton", L"JMXVBSK");
    if (e == L"ban")  return info(FileCategory::Animation, C_ANIM, "type_animation", L"JMXVBAN");
    if (e == L"bsr")  return info(FileCategory::Resource, C_RES, "type_resource", L"JMXVRES");
    if (e == L"cpd")  return info(FileCategory::Compound, C_CPD, "type_compound", L"JMXVCPD");
    if (e == L"efp" || e == L"eff") return info(FileCategory::Effect, C_EFF, "type_effect", L"JMXVEFF");
    if (e == L"nvm")  return info(FileCategory::MapNav, C_MAP, "type_mapnav", L"JMXVNVM");
    if (e == L"dof")  return info(FileCategory::MapDungeon, C_MAP, "type_mapdungeon", L"JMXVDOF");
    if (e == L"o2" || e == L"o") return info(FileCategory::MapObject, C_MAP, "type_mapobject", L"JMXVMAPO");
    if (e == L"m")    return info(FileCategory::MapMesh, C_MAP, "type_mapmesh", L"JMXVMAPM");
    if (e == L"t")    return info(FileCategory::MapTexture, C_MAP, "type_maptex", L"JMXVMAPT");
    if (e == L"mfo")  return info(FileCategory::MapProject, C_MAP, "type_mapproject", L"JMXVMFO");
    if (e == L"ifo")  return info(FileCategory::MapEnv, C_MAP, "type_mapinfo"); // refine via signature
    if (e == L"2dt")  return info(FileCategory::UI, C_UI, "type_ui");
    if (e == L"dds")  return info(FileCategory::Texture, C_TEX, "type_texture");
    if (e == L"jpg" || e == L"jpeg" || e == L"png" || e == L"bmp" || e == L"tga" || e == L"gif")
        return info(FileCategory::Image, C_IMG, "type_image");
    if (e == L"wav" || e == L"mp3" || e == L"ogg" || e == L"flac" || e == L"wma")
        return info(FileCategory::Sound, C_SND, "type_sound");
    if (e == L"dat")  return info(FileCategory::Config, C_CFG, "type_config");
    return info(FileCategory::Unknown, C_UNK, "type_unknown");
}

TypeInfo classify(std::wstring_view name, const std::uint8_t* head, std::size_t head_len) {
    if (head && head_len >= 12) {
        auto sig = [&](const char* s) { return std::memcmp(head, s, std::strlen(s)) == 0; };
        if (sig("JMXVBMS"))  return info(FileCategory::Mesh, C_MESH, "type_mesh", L"JMXVBMS");
        if (sig("JMXVBMT"))  return info(FileCategory::Material, C_MAT, "type_material", L"JMXVBMT");
        if (sig("JMXVDDJ"))  return info(FileCategory::Texture, C_TEX, "type_texture", L"JMXVDDJ");
        if (sig("JMXVBSK"))  return info(FileCategory::Skeleton, C_SKEL, "type_skeleton", L"JMXVBSK");
        if (sig("JMXVBAN"))  return info(FileCategory::Animation, C_ANIM, "type_animation", L"JMXVBAN");
        if (sig("JMXVRES"))  return info(FileCategory::Resource, C_RES, "type_resource", L"JMXVRES");
        if (sig("JMXVCPD"))  return info(FileCategory::Compound, C_CPD, "type_compound", L"JMXVCPD");
        if (sig("JMXVNVM"))  return info(FileCategory::MapNav, C_MAP, "type_mapnav", L"JMXVNVM");
        if (sig("JMXVDOF"))  return info(FileCategory::MapDungeon, C_MAP, "type_mapdungeon", L"JMXVDOF");
        if (sig("JMXVMAPO")) return info(FileCategory::MapObject, C_MAP, "type_mapobject", L"JMXVMAPO");
        if (sig("JMXVMAPM")) return info(FileCategory::MapMesh, C_MAP, "type_mapmesh", L"JMXVMAPM");
        if (sig("JMXVMAPT")) return info(FileCategory::MapTexture, C_MAP, "type_maptex", L"JMXVMAPT");
        if (sig("JMXVMFO"))  return info(FileCategory::MapProject, C_MAP, "type_mapproject", L"JMXVMFO");
        if (sig("JMXVENVI")) return info(FileCategory::MapEnv, C_MAP, "type_mapenv", L"JMXVENVI");
        if (sig("JMXVOBJI")) return info(FileCategory::MapObject, C_MAP, "type_mapobjidx", L"JMXVOBJI");
        if (sig("JMXV2DTI")) return info(FileCategory::MapTexture, C_MAP, "type_map2dti", L"JMXV2DTI");
        if (sig("JMXVEFF"))  return info(FileCategory::Effect, C_EFF, "type_effect", L"JMXVEFF");
        if (sig("JMXVCAMR")) return info(FileCategory::Camera, C_CAM, "type_camera", L"JMXVCAMR");
        if (sig("JMXVCPD"))  return info(FileCategory::Compound, C_CPD, "type_compound", L"JMXVCPD");
        if (sig("JoyMax Pack")) return info(FileCategory::Archive, C_ARC, "type_archive", L"JMXPACK");
        if (head_len >= 4 && std::memcmp(head, "DDS ", 4) == 0)
            return info(FileCategory::Texture, C_TEX, "type_texture");
    }
    return classify(name);
}

bool is_text(FileCategory c) noexcept {
    return c == FileCategory::Text || c == FileCategory::Script;
}
bool is_texture(const std::wstring& name) noexcept {
    FileCategory c = classify(name).category;
    return c == FileCategory::Texture || c == FileCategory::Image;
}
bool is_sound(const std::wstring& name) noexcept {
    return classify(name).category == FileCategory::Sound;
}

} // namespace pk2
