// ============================================================================
//  SRO Asset Studio - formats/Bsk.h
//
//  JMXVBSK - skeleton (bone hierarchy).  Wiki: JMXVBSK.md
//
//    12  signature
//     4  u32 boneCount
//     per bone:
//        u8   type              0 = CPrimBone, 1 = CPrimDummy
//        len-prefixed name
//        len-prefixed parent name
//        Quat rotationToParent  + Vec3 translationToParent
//        Quat rotationToOrigin  + Vec3 translationToOrigin
//        Quat rotationToLocal   + Vec3 translationToLocal
//        u32  childCount, then len-prefixed child names
//     4  u32 unk0    (asserted 0)
//     4  u32 unk1    (asserted 0)
//
//  The viewer draws bones as lines using translationToOrigin, so the skeleton
//  is renderable without resolving the hierarchy - but the hierarchy is built
//  anyway for the tree view and for .ban playback.
//
//  Implementation owner: Agent D (formats).
// ============================================================================
#pragma once

#include "../Common.h"

namespace sro {

constexpr char kBskSignaturePrefix[] = "JMXVBSK";

enum class BskBoneType : u8 {
    Bone  = 0,   ///< CPrimBone
    Dummy = 1    ///< CPrimDummy (attachment point)
};

struct BskBone {
    BskBoneType  type = BskBoneType::Bone;
    std::wstring name;
    std::wstring parentName;

    Quat rotationToParent;
    Vec3 translationToParent;
    Quat rotationToOrigin;
    Vec3 translationToOrigin;
    Quat rotationToLocal;
    Vec3 translationToLocal;

    std::vector<std::wstring> childNames;

    // --- resolved hierarchy (filled by ParseBsk) --------------------------
    int              parentIndex = -1;   ///< -1 = root
    std::vector<int> childIndices;
};

struct BskFile {
    std::string  signature;
    std::wstring version;
    std::vector<BskBone> bones;
    u32 unk0 = 0, unk1 = 0;

    Aabb bounds;                     ///< over translationToOrigin

    /// Case-insensitive lookup; -1 when absent.
    int findBone(const std::wstring& name) const;
    /// Indices of the bones with no parent.
    std::vector<int> roots() const;

    std::wstring summaryTr() const;
};

Result ParseBsk(const u8* data, size_t size, BskFile& out);

bool IsBsk(const u8* data, size_t size);

} // namespace sro
