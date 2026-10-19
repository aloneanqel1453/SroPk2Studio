// ============================================================================
//  SRO Asset Studio - formats/Ban.cpp
//
//  JMXVBAN 0102 animation parser.  Wiki: JMXVBAN.md
//
//  Version note: the two leading ints were INTRODUCED in 0102. Older files go
//  straight from the signature to the name, so the version digits are parsed
//  and honoured instead of being assumed.
//
//  The per-bone keyframe count is read and bounds-checked independently of
//  keyframeTimeCount - they normally match, but a parser that assumes they do
//  walks off the end of a re-exported file.
// ============================================================================
#include "Ban.h"
#include "../core/I18n.h"   // UI strings follow the chosen language

namespace sro {

namespace {

constexpr UINT kPathCodePage = 1252;

/// One keyframe = quaternion (16) + translation (12).
constexpr size_t kKeyframeBytes = 28;

/// "JMXVBAN 0102" -> 102. Returns 0 when the digits are not numeric.
int ParseVersionDigits(const std::string& signature) {
    if (signature.size() < 12) return 0;
    int v = 0;
    for (size_t i = 8; i < 12; ++i) {
        const char c = signature[i];
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
    }
    return v;
}

/// Normalised linear interpolation - cheaper than slerp and visually identical
/// for the small angular deltas between adjacent animation keyframes.
Quat Nlerp(const Quat& a, const Quat& b, f32 t) {
    // Take the shorter arc.
    const f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const f32 s = (dot < 0.0f) ? -1.0f : 1.0f;
    Quat q(a.x + (b.x * s - a.x) * t,
           a.y + (b.y * s - a.y) * t,
           a.z + (b.z * s - a.z) * t,
           a.w + (b.w * s - a.w) * t);
    const f32 n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n > 1e-12f) { q.x /= n; q.y /= n; q.z /= n; q.w /= n; }
    else            { q = Quat(0, 0, 0, 1); }
    return q;
}

} // namespace

// ---------------------------------------------------------------------------
// Small accessors
// ---------------------------------------------------------------------------
const wchar_t* BanFile::typeNameTr() const {
    return typeEnum() == BanType::Cyclic ? T(L"Döngüsel") : T(L"Tek seferlik");
}

const BanAnimatedBone* BanFile::findBone(const std::wstring& boneName) const {
    for (const BanAnimatedBone& b : bones)
        if (IEqualsW(b.name, boneName)) return &b;
    return nullptr;
}

// ---------------------------------------------------------------------------
// IsBan
// ---------------------------------------------------------------------------
bool IsBan(const u8* data, size_t size) {
    if (!data) return false;
    if (size >= 7 && std::memcmp(data, kBanSignaturePrefix, 7) == 0) return true;
    if (size >= 4 && std::memcmp(data, "BAN ", 4) == 0) return true;
    return false;
}

// ---------------------------------------------------------------------------
// ParseBan
// ---------------------------------------------------------------------------
Result ParseBan(const u8* data, size_t size, BanFile& out, bool loadKeyframes) {
    out = BanFile();

    if (!data || size < 16)
        return Result::Fail(Status::Truncated, T(L"BAN: dosya 16 bayttan kısa."));
    if (!IsBan(data, size))
        return Result::Fail(Status::BadFormat, T(L"BAN: imza \"JMXVBAN\" değil."));

    ByteReader r(data, size);
    const bool isLegacy101 = (std::memcmp(data, "BAN ", 4) == 0);
    auto readV101StringW = [](ByteReader& reader, UINT cp) -> std::wstring {
        const u32 len = reader.readU32();
        if (len == 0 || reader.failed()) return std::wstring();
        if (!reader.canRead(len)) { reader.skip(reader.remaining() + 1); return std::wstring(); }
        const u8* p = reader.readPtr(len);
        reader.skip(1); // skip trailing NUL
        return MultiByteToWide(reinterpret_cast<const char*>(p), len, cp);
    };

    if (isLegacy101) {
        out.signature = r.readSignature(4);
        out.version = L"0101";
        (void)r.readU32(); // version 101 int
        out.name       = readV101StringW(r, kPathCodePage);
        out.durationMs = r.readI32();
        out.fps        = r.readI32();
        out.type       = r.readU32();
    } else {
        out.signature = r.readSignature(12);
        // Guard the substr: readSignature stops at the first NUL, so a .ban whose
        // 8th byte is 0x00 yields a 7-char "JMXVBAN" and an unguarded substr(8)
        // throws std::out_of_range (a crash in the CLI, which has no try/catch).
        // Matches Bsr/Bms/Bsk, which all length-check before slicing the version.
        if (out.signature.size() >= 12)
            out.version = TrimW(MultiByteToWide(out.signature.substr(8, 4), kPathCodePage));

        const int version = ParseVersionDigits(out.signature);
        if (version >= 102) {
            out.int0 = r.readI32();
            out.int1 = r.readI32();
        }

        out.name       = r.readLenPrefixedStringW(kPathCodePage, 4096);
        out.durationMs = r.readI32();
        out.fps        = r.readI32();
        out.type       = r.readU32();
    }

    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"BAN: başlık eksik."));

    // -----------------------------------------------------------------------
    // Keyframe timings (v102+ has shared time array; v101 stores timestamps inline)
    // -----------------------------------------------------------------------
    if (!isLegacy101) {
        const u32 timeCount = r.readU32();
        if (r.failed())
            return Result::Fail(Status::Truncated, T(L"BAN: kare zamanı sayısı okunamadı."));
        if (timeCount > r.remaining() / 4)
            return Result::Fail(Status::BadFormat,
                                T(L"BAN: kare sayısı dosya boyutuna sığmıyor (") +
                                FormatCountTr(timeCount) + L").");

        out.keyframeTimesMs.reserve(timeCount);
        for (u32 i = 0; i < timeCount; ++i) out.keyframeTimesMs.push_back(r.readU32());
        if (r.failed())
            return Result::Fail(Status::Truncated, T(L"BAN: kare zamanları eksik."));
    }

    // -----------------------------------------------------------------------
    // Animated bones
    // -----------------------------------------------------------------------
    const u32 boneCount = r.readU32();
    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"BAN: kemik sayısı okunamadı."));
    // Minimum per bone: 4 byte name length + 4 byte keyframe count.
    if (boneCount > r.remaining() / 8)
        return Result::Fail(Status::BadFormat,
                            T(L"BAN: kemik sayısı dosya boyutuna sığmıyor (") +
                            FormatCountTr(boneCount) + L").");

    bool partial = false;
    out.bones.reserve(boneCount);

    const size_t kfBytes = isLegacy101 ? 36 : kKeyframeBytes;

    for (u32 i = 0; i < boneCount; ++i) {
        BanAnimatedBone b;
        b.name = isLegacy101 ? readV101StringW(r, kPathCodePage)
                             : r.readLenPrefixedStringW(kPathCodePage, 1024);
        const u32 kf = r.readU32();
        if (r.failed()) { partial = true; break; }
        if (kf > r.remaining() / kfBytes) { partial = true; break; }

        if (loadKeyframes && kf) {
            b.keyframes.resize(kf);
            for (u32 k = 0; k < kf; ++k) {
                if (isLegacy101) {
                    const u32 t1 = r.readU32();
                    (void)r.readU32(); // t2
                    if (out.keyframeTimesMs.size() < kf) {
                        out.keyframeTimesMs.push_back(t1);
                    }
                }
                b.keyframes[k].rotation    = r.readQuat();
                b.keyframes[k].translation = r.readVec3();
            }
            if (r.failed()) { partial = true; break; }
        } else if (kf) {
            r.skip(static_cast<size_t>(kf) * kfBytes);
            if (r.failed()) { partial = true; break; }
        }

        out.bones.push_back(std::move(b));
    }

    if (partial)
        return Result::Fail(Status::Truncated,
                            T(L"BAN: dosya eksik - ") + FormatCountTr(out.bones.size()) +
                            L"/" + FormatCountTr(boneCount) + T(L" kemik okunabildi."));
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// SampleBan
// ---------------------------------------------------------------------------
bool SampleBan(const BanFile& anim, f32 timeMs,
               std::vector<Quat>& outRotations,
               std::vector<Vec3>& outTranslations) {
    outRotations.clear();
    outTranslations.clear();
    if (anim.bones.empty()) return false;

    const size_t timeCount = anim.keyframeTimesMs.size();

    // Wrap or clamp the requested time.
    f32 t = timeMs;
    const f32 duration = (anim.durationMs > 0)
                       ? static_cast<f32>(anim.durationMs)
                       : (timeCount ? static_cast<f32>(anim.keyframeTimesMs.back()) : 0.0f);
    if (duration > 0.0f) {
        if (anim.typeEnum() == BanType::Cyclic) {
            t = std::fmod(t, duration);
            if (t < 0.0f) t += duration;
        } else {
            if (t < 0.0f)      t = 0.0f;
            if (t > duration)  t = duration;
        }
    } else {
        t = 0.0f;
    }

    // Locate the bracketing key indices from the shared timing table. When the
    // file carries no timing table we fall back to a uniform distribution over
    // each bone's own keyframe list.
    size_t k0 = 0, k1 = 0;
    f32    frac = 0.0f;
    bool   haveTimeTable = timeCount >= 2;

    if (haveTimeTable) {
        k1 = timeCount - 1;
        for (size_t i = 1; i < timeCount; ++i) {
            if (static_cast<f32>(anim.keyframeTimesMs[i]) >= t) { k0 = i - 1; k1 = i; break; }
            k0 = i;
        }
        const f32 t0 = static_cast<f32>(anim.keyframeTimesMs[k0]);
        const f32 t1 = static_cast<f32>(anim.keyframeTimesMs[k1]);
        frac = (t1 > t0) ? ((t - t0) / (t1 - t0)) : 0.0f;
    }

    outRotations.resize(anim.bones.size());
    outTranslations.resize(anim.bones.size());

    for (size_t bi = 0; bi < anim.bones.size(); ++bi) {
        const BanAnimatedBone& b = anim.bones[bi];
        if (b.keyframes.empty()) {
            outRotations[bi]    = Quat(0, 0, 0, 1);
            outTranslations[bi] = Vec3(0, 0, 0);
            continue;
        }

        size_t a = 0, c = 0;
        f32    f = 0.0f;

        if (haveTimeTable) {
            a = (k0 < b.keyframes.size()) ? k0 : b.keyframes.size() - 1;
            c = (k1 < b.keyframes.size()) ? k1 : b.keyframes.size() - 1;
            f = frac;
        } else if (b.keyframes.size() >= 2 && duration > 0.0f) {
            const f32 step = duration / static_cast<f32>(b.keyframes.size() - 1);
            const f32 idx  = (step > 0.0f) ? (t / step) : 0.0f;
            const f32 fl   = std::floor(idx);
            a = static_cast<size_t>((fl < 0.0f) ? 0.0f : fl);
            if (a >= b.keyframes.size() - 1) { a = b.keyframes.size() - 2; f = 1.0f; }
            else                              { f = idx - fl; }
            c = a + 1;
        }

        const BanKeyframe& ka = b.keyframes[a];
        const BanKeyframe& kc = b.keyframes[c];
        outRotations[bi] = Nlerp(ka.rotation, kc.rotation, f);
        outTranslations[bi] = Vec3(ka.translation.x + (kc.translation.x - ka.translation.x) * f,
                                   ka.translation.y + (kc.translation.y - ka.translation.y) * f,
                                   ka.translation.z + (kc.translation.z - ka.translation.z) * f);
    }
    return true;
}

// ---------------------------------------------------------------------------
// summaryTr
// ---------------------------------------------------------------------------
std::wstring BanFile::summaryTr() const {
    std::wstring s;
    s += T(L"İmza          : ") + MultiByteToWide(signature, 1252) + L"\n";
    if (!name.empty()) s += T(L"Ad            : ") + name + L"\n";
    s += FormatW(T(L"Süre          : %d ms\n"), durationMs);
    s += FormatW(T(L"Kare hızı     : %d fps\n"), fps);
    s += T(L"Tür           : ") + std::wstring(typeNameTr()) +
         FormatW(L" (%u)\n", type);
    s += T(L"Anahtar kare  : ") + FormatCountTr(keyframeTimesMs.size()) + L"\n";
    s += T(L"Hareketli kemik: ") + FormatCountTr(bones.size()) + L"\n";

    if (!keyframeTimesMs.empty()) {
        std::wstring times;
        const size_t shown = (keyframeTimesMs.size() < 10) ? keyframeTimesMs.size() : 10;
        for (size_t i = 0; i < shown; ++i) {
            if (i) times += L", ";
            times += FormatW(L"%u", keyframeTimesMs[i]);
        }
        if (keyframeTimesMs.size() > shown) times += L", ...";
        s += T(L"Kare zamanları: ") + times + T(L" ms\n");
    }

    u64 totalKeys = 0;
    for (const BanAnimatedBone& b : bones) totalKeys += b.keyframes.size();
    if (totalKeys)
        s += T(L"Toplam anahtar: ") + FormatCountTr(totalKeys) + L"\n";

    const size_t shown = (bones.size() < 12) ? bones.size() : 12;
    if (shown) s += T(L"\nKemikler (ilk ") + FormatCountTr(shown) + T(L"):\n");
    for (size_t i = 0; i < shown; ++i)
        s += L"  " + bones[i].name + L"  (" +
             FormatCountTr(bones[i].keyframes.size()) + T(L" kare)\n");
    if (bones.size() > shown)
        s += T(L"  ... ve ") + FormatCountTr(bones.size() - shown) + T(L" kemik daha\n");

    return s;
}

} // namespace sro
