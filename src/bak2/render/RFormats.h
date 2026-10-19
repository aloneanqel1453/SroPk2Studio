// SkillEffect Studio - render/RFormats.h
// Read-only parsers for the model formats (JMXVBMS / BMT / BSK / BAN / RES(.bsr) / CPD). Layouts and the
// known traps follow SRO Asset Studio's verified loaders (sro-bmt-bumpflag-and-blend-gate: the 0x2000 bump
// flag can lie -> read both ways and keep the reading that explains the file; stale BMS face offset).
// Strings are CP949 -> UTF-16.
#pragma once

#include "RMath.h"

#include <cstdint>
#include <string>
#include <vector>

namespace se::render {

struct V3f { float x = 0, y = 0, z = 0; };
struct V2f { float x = 0, y = 0; };

struct BmsVertex { V3f pos, nrm; V2f uv; };
struct BmsSkin { std::uint8_t b0 = 0xFF, b1 = 0xFF; std::uint16_t w0 = 0, w1 = 0; };
struct BmsFile {
    std::wstring name, material;
    std::uint32_t vertexFlag = 0;
    std::vector<BmsVertex> verts;
    std::vector<std::uint16_t> idx;         // triangle list
    std::vector<std::wstring> boneNames;
    std::vector<BmsSkin> skin;              // per vertex when skinned
    V3f bbMin, bbMax;
    bool Skinned() const { return !boneNames.empty() && skin.size() == verts.size(); }
};

struct BmtMaterial {
    std::wstring name;
    float diffuse[4] = {}, ambient[4] = {}, specular[4] = {}, emissive[4] = {};
    float power = 0;
    std::uint32_t flags = 0;
    std::wstring diffuseMap;
    bool absolute = false;
    std::wstring normalMap;
};
struct BmtFile { std::vector<BmtMaterial> mats; const BmtMaterial* Find(const std::wstring& n) const; };

struct BskBone {
    std::uint8_t type = 0;   // 0 bone, 1 dummy
    std::wstring name, parent;
    Quat rotParent; V3 transParent;
    Quat rotOrigin; V3 transOrigin;
    Quat rotLocal; V3 transLocal;
    int parentIndex = -1;
};
struct BskFile { std::vector<BskBone> bones; int Find(const std::wstring& n) const; };

struct BanKey { Quat q; V3 t; };
struct BanBone { std::wstring name; std::vector<BanKey> keys; };
struct BanFile {
    std::wstring name;
    std::int32_t durationMs = 0, fps = 0;
    std::uint32_t type = 0;               // 1 = cyclic
    std::vector<std::uint32_t> keyTimes;  // ms
    std::vector<BanBone> bones;
    double LengthMs() const;              // max(durationMs, last key time), 1000 when both missing
};

struct BsrAniEvent { std::uint32_t keyTime = 0, type = 0, index = 0, unk = 0; };
struct BsrAniEntry { std::uint32_t type = 0, fileIndex = 0; std::vector<BsrAniEvent> events; float walkLength = 0; };
struct BsrAniGroup { std::wstring name; std::vector<BsrAniEntry> entries; };
struct BsrParticle { std::wstring path, bone; bool enabled = false; V3 offset; };
struct BsrSound { std::wstring path, event; std::int32_t timeMs = 0; };
struct BsrFile {
    std::uint16_t type = 0, category = 0;
    std::wstring name;
    std::vector<std::pair<std::uint32_t, std::wstring>> materialSets;   // (id, .bmt)
    std::vector<std::wstring> meshes;                                     // .bms
    std::vector<std::wstring> animations;                                 // .ban
    bool hasSkeleton = false;
    std::wstring skeleton, attachBone;
    std::vector<std::pair<std::wstring, std::vector<std::uint32_t>>> meshGroups;
    std::vector<BsrAniGroup> aniGroups;
    std::vector<BsrParticle> particles;   // mod palette ModDataParticle (bone-seated .efp)
    std::vector<BsrSound> sounds;
    bool partial = false;
};

struct CpdFile { std::uint32_t type = 0; std::wstring name, collision; std::vector<std::wstring> parts; };

bool ParseBms(const std::uint8_t* d, std::size_t n, BmsFile& out, std::string& err);
bool ParseBmt(const std::uint8_t* d, std::size_t n, BmtFile& out, std::string& err);
bool ParseBsk(const std::uint8_t* d, std::size_t n, BskFile& out, std::string& err);
bool ParseBan(const std::uint8_t* d, std::size_t n, BanFile& out, std::string& err);
bool ParseBsr(const std::uint8_t* d, std::size_t n, BsrFile& out, std::string& err);
bool ParseCpd(const std::uint8_t* d, std::size_t n, CpdFile& out, std::string& err);

// ResourceAnimationType (SilkroadDoc ResourceAnimationType.md): "ATTACK1" / "ANI_ATTACK1" / "SKILL_3" ... -> id
int AniTypeId(const std::wstring& token);
std::wstring AniTypeName(int id);   // "" when unknown

}  // namespace se::render
