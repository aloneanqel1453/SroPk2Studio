#pragma once
// Silkroad file-type recognition (SilkroadDoc wiki "Formats" index).
// Primary: file extension. Accurate: 12-byte leading signature (e.g. .ifo is
// overloaded across JMXVENVI / JMXVOBJI / JMXV2DTI).
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace pk2 {

enum class FileCategory {
    Folder, Text, Data, Config,
    Mesh, Material, Texture, Skeleton, Animation, Resource, Compound, Effect, Camera,
    MapNav, MapObject, MapMesh, MapTexture, MapEnv, MapDungeon, MapProject,
    Image, Sound, Script, UI, Archive, Unknown
};

struct TypeInfo {
    FileCategory category = FileCategory::Unknown;
    std::uint32_t color = 0x9BA7BC; // display accent (0xRRGGBB)
    const char* name_key = "type_unknown"; // i18n key for the friendly name
    const wchar_t* signature = L""; // detected JMX* signature when known
};

// Classify by file name (extension) only — fast, for the tree.
[[nodiscard]] TypeInfo classify(std::wstring_view name);

// Refine using the leading bytes (first >= 12). Disambiguates .ifo and confirms JMXV*.
[[nodiscard]] TypeInfo classify(std::wstring_view name, const std::uint8_t* head, std::size_t head_len);

// Convenience predicates.
[[nodiscard]] bool is_text(FileCategory c) noexcept;
[[nodiscard]] bool is_texture(const std::wstring& name) noexcept;
[[nodiscard]] bool is_sound(const std::wstring& name) noexcept;

} // namespace pk2
