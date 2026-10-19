// ============================================================================
//  SRO Asset Studio - formats/Bsk.cpp
//
//  JMXVBSK skeleton parser.  Wiki: JMXVBSK.md
//
//  The file is a flat bone list; each bone names its parent and its children as
//  STRINGS. After reading we resolve those names to indices so the tree view
//  and .ban playback can walk the hierarchy by index. Name resolution is
//  case-insensitive because Silkroad data is inconsistent about bone casing.
// ============================================================================
#include "Bsk.h"
#include "../core/I18n.h"   // UI strings follow the chosen language

namespace sro {

namespace {

constexpr UINT kPathCodePage = 1252;

/// A bone cannot be smaller than: type byte + two empty length-prefixed
/// strings + three (quat, vec3) pairs + child count.
constexpr size_t kMinBoneBytes = 1 + 4 + 4 + (16 + 12) * 3 + 4;

} // namespace

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------
int BskFile::findBone(const std::wstring& name) const {
    for (size_t i = 0; i < bones.size(); ++i)
        if (IEqualsW(bones[i].name, name)) return static_cast<int>(i);
    return -1;
}

std::vector<int> BskFile::roots() const {
    std::vector<int> r;
    for (size_t i = 0; i < bones.size(); ++i)
        if (bones[i].parentIndex < 0) r.push_back(static_cast<int>(i));
    return r;
}

// ---------------------------------------------------------------------------
// IsBsk
// ---------------------------------------------------------------------------
bool IsBsk(const u8* data, size_t size) {
    if (!data) return false;
    if (size >= 7 && std::memcmp(data, kBskSignaturePrefix, 7) == 0) return true;
    if (size >= 4 && std::memcmp(data, "BSK ", 4) == 0) return true;
    return false;
}

// ---------------------------------------------------------------------------
// ParseBsk
// ---------------------------------------------------------------------------
Result ParseBsk(const u8* data, size_t size, BskFile& out) {
    out = BskFile();

    if (!data || size < 16)
        return Result::Fail(Status::Truncated, T(L"BSK: dosya 16 bayttan kısa."));
    if (!IsBsk(data, size))
        return Result::Fail(Status::BadFormat, T(L"BSK: imza \"JMXVBSK\" değil."));

    ByteReader r(data, size);
    const bool isLegacy101 = (std::memcmp(data, "BSK ", 4) == 0);
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
    } else {
        out.signature = r.readSignature(12);
        if (out.signature.size() >= 12)
            out.version = TrimW(MultiByteToWide(out.signature.substr(8, 4), kPathCodePage));
    }

    const u32 boneCount = r.readU32();
    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"BSK: kemik sayısı okunamadı."));
    if (boneCount && boneCount > r.remaining() / kMinBoneBytes)
        return Result::Fail(Status::BadFormat,
                            T(L"BSK: kemik sayısı dosya boyutuna sığmıyor (") +
                            FormatCountTr(boneCount) + L").");

    bool partial = false;
    out.bones.reserve(boneCount);

    for (u32 i = 0; i < boneCount; ++i) {
        BskBone b;
        const u8 type = r.readU8();
        b.type = (type == 1) ? BskBoneType::Dummy : BskBoneType::Bone;
        b.name       = isLegacy101 ? readV101StringW(r, kPathCodePage)
                                   : r.readLenPrefixedStringW(kPathCodePage, 1024);
        b.parentName = isLegacy101 ? readV101StringW(r, kPathCodePage)
                                   : r.readLenPrefixedStringW(kPathCodePage, 1024);

        b.rotationToParent    = r.readQuat();
        b.translationToParent = r.readVec3();
        b.rotationToOrigin    = r.readQuat();
        b.translationToOrigin = r.readVec3();
        b.rotationToLocal     = r.readQuat();
        b.translationToLocal  = r.readVec3();

        const u32 childCount = r.readU32();
        if (r.failed()) { partial = true; break; }
        if (childCount > r.remaining() / 4) { partial = true; break; }

        b.childNames.reserve(childCount);
        for (u32 c = 0; c < childCount; ++c) {
            b.childNames.push_back(isLegacy101 ? readV101StringW(r, kPathCodePage)
                                               : r.readLenPrefixedStringW(kPathCodePage, 1024));
            if (r.failed()) break;
        }
        if (r.failed()) { partial = true; break; }

        out.bones.push_back(std::move(b));
    }

    if (!partial) {
        out.unk0 = r.readU32();
        out.unk1 = r.readU32();
        r.clearFail();       // these two trailing ints are absent in some files
    }

    // -----------------------------------------------------------------------
    // Resolve the hierarchy by name.
    // -----------------------------------------------------------------------
    for (size_t i = 0; i < out.bones.size(); ++i) {
        BskBone& b = out.bones[i];
        if (!b.parentName.empty())
            b.parentIndex = out.findBone(b.parentName);
        // A bone naming itself as its parent would loop the tree walkers.
        if (b.parentIndex == static_cast<int>(i)) b.parentIndex = -1;
    }
    for (BskBone& b : out.bones) {
        b.childIndices.reserve(b.childNames.size());
        for (const std::wstring& cn : b.childNames) {
            const int ci = out.findBone(cn);
            if (ci >= 0) b.childIndices.push_back(ci);
        }
    }

    for (const BskBone& b : out.bones) out.bounds.add(b.translationToOrigin);

    if (partial)
        return Result::Fail(Status::Truncated,
                            T(L"BSK: dosya eksik - ") + FormatCountTr(out.bones.size()) +
                            L"/" + FormatCountTr(boneCount) + T(L" kemik okunabildi."));
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// summaryTr
// ---------------------------------------------------------------------------
std::wstring BskFile::summaryTr() const {
    std::wstring s;
    s += T(L"İmza          : ") + MultiByteToWide(signature, 1252) + L"\n";
    s += T(L"Kemik sayısı  : ") + FormatCountTr(bones.size()) + L"\n";

    size_t dummies = 0;
    for (const BskBone& b : bones)
        if (b.type == BskBoneType::Dummy) ++dummies;
    s += T(L"Bunlardan     : ") + FormatCountTr(bones.size() - dummies) + T(L" kemik, ") +
         FormatCountTr(dummies) + T(L" dummy (bağlantı noktası)\n");

    const std::vector<int> rootList = roots();
    s += T(L"Kök kemik     : ") + FormatCountTr(rootList.size());
    if (!rootList.empty() && rootList[0] < static_cast<int>(bones.size()))
        s += L"  (" + bones[static_cast<size_t>(rootList[0])].name + L")";
    s += L"\n";

    if (bounds.valid()) {
        const Vec3 e = bounds.extent();
        s += FormatW(T(L"İskelet boyutu: %.2f x %.2f x %.2f\n"), e.x, e.y, e.z);
    }

    // First few bones with their parents - enough to sanity-check the file.
    const size_t shown = (bones.size() < 12) ? bones.size() : 12;
    if (shown) s += T(L"\nHiyerarşi (ilk ") + FormatCountTr(shown) + T(L"):\n");
    for (size_t i = 0; i < shown; ++i) {
        const BskBone& b = bones[i];
        s += L"  " + b.name;
        if (b.type == BskBoneType::Dummy) s += T(L" [dummy]");
        if (!b.parentName.empty()) s += L"  <- " + b.parentName;
        if (!b.childIndices.empty())
            s += L"  (" + FormatCountTr(b.childIndices.size()) + T(L" çocuk)");
        s += L"\n";
    }
    if (bones.size() > shown)
        s += T(L"  ... ve ") + FormatCountTr(bones.size() - shown) + T(L" kemik daha\n");

    return s;
}

} // namespace sro
