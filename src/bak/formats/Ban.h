// ============================================================================
//  SRO Asset Studio - formats/Ban.h
//
//  JMXVBAN 0102 - skeletal animation.  Wiki: JMXVBAN.md
//
//    12  signature "JMXVBAN 0102"
//     4  i32 int0                 (0 in every known file, 0102+)
//     4  i32 int1                 (0 in every known file, 0102+)
//     *  len-prefixed Name
//     4  i32 duration             ms
//     4  i32 framesPerSecond
//     4  i32 type                 0 = OneShot, 1 = Cyclic
//     4  u32 keyframeTimeCount, then that many u32 times (ms)
//     4  u32 animatedBoneCount
//        per bone: len-prefixed name, u32 keyframeCount,
//                  then Quat rotation + Vec3 translation per keyframe
//
//  Keyframe count per bone normally equals keyframeTimeCount, but the parser
//  must not assume it - it bounds-checks each list independently.
//
//  Implementation owner: Agent D (formats).
// ============================================================================
#pragma once

#include "../Common.h"

namespace sro {

constexpr char kBanSignaturePrefix[] = "JMXVBAN";

enum class BanType : u32 {
    OneShot = 0,
    Cyclic  = 1
};

struct BanKeyframe {
    Quat rotation;
    Vec3 translation;
};

struct BanAnimatedBone {
    std::wstring             name;
    std::vector<BanKeyframe> keyframes;
};

struct BanFile {
    std::string  signature;
    std::wstring version;            ///< "0102"
    i32          int0 = 0, int1 = 0;
    std::wstring name;
    i32          durationMs = 0;
    i32          fps        = 0;
    u32          type       = 0;

    std::vector<u32>             keyframeTimesMs;
    std::vector<BanAnimatedBone> bones;

    BanType typeEnum() const { return (type == 1) ? BanType::Cyclic : BanType::OneShot; }
    const wchar_t* typeNameTr() const;

    /// Case-insensitive lookup; nullptr when the bone is not animated.
    const BanAnimatedBone* findBone(const std::wstring& boneName) const;

    std::wstring summaryTr() const;
};

/// `loadKeyframes = false` reads only the header + times (fast info pane pass).
Result ParseBan(const u8* data, size_t size, BanFile& out, bool loadKeyframes);

bool IsBan(const u8* data, size_t size);

/// Sample the animation at `timeMs`, producing per-bone local transforms.
/// `outRotations` / `outTranslations` are indexed the same as `anim.bones`.
/// Interpolation is nlerp on rotation, linear on translation; cyclic
/// animations wrap, one-shot animations clamp.
bool SampleBan(const BanFile& anim, f32 timeMs,
               std::vector<Quat>& outRotations,
               std::vector<Vec3>& outTranslations);

} // namespace sro
