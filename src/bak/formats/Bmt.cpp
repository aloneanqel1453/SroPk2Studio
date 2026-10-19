// ============================================================================
//  SRO Asset Studio - formats/Bmt.cpp
//
//  JMXVBMT 0102 material set parser.  Wiki: JMXVBMT.md
//
//  Per material the file stores four Color4 blocks, a float, a flag word and
//  a diffuse map descriptor; a normal map descriptor follows only when
//  PrimMtrlFlag bit14 (0x2000) is set - it does not exist at all in VSRO 1.188
//  files, which is why the flag must be honoured rather than probed.
// ============================================================================
#include "Bmt.h"
#include "../core/I18n.h"   // UI strings follow the chosen language

namespace sro {

namespace {

constexpr UINT kPathCodePage = 1252;

/// Smallest possible material record: two empty length-prefixed strings,
/// 4 x Color4, float, flags, and the diffuse map trailer.
constexpr size_t kMinMaterialBytes = 4 /*name len*/ + 64 /*colours*/ + 4 /*float*/ +
                                     4 /*flags*/ + 4 /*path len*/ + 4 + 1 + 1 + 1;

Color4 ReadColor4(ByteReader& r) {
    Color4 c;
    c.r = r.readF32(); c.g = r.readF32(); c.b = r.readF32(); c.a = r.readF32();
    return c;
}

std::wstring FormatColor(const Color4& c) {
    return FormatW(L"%.2f %.2f %.2f %.2f", c.r, c.g, c.b, c.a);
}

} // namespace

// ---------------------------------------------------------------------------
// BmtTextureRef::resolve
// ---------------------------------------------------------------------------
std::wstring BmtTextureRef::resolve(const std::wstring& bmtFolder) const {
    if (path.empty()) return path;
    if (isAbsolute)   return NormalizeInnerPath(path);
    return NormalizeInnerPath(PathJoin(bmtFolder, path));
}

// ---------------------------------------------------------------------------
// BmtFile::find
// ---------------------------------------------------------------------------
const BmtMaterial* BmtFile::find(const std::wstring& name) const {
    for (const BmtMaterial& m : materials)
        if (IEqualsW(m.name, name)) return &m;
    return nullptr;
}

// ---------------------------------------------------------------------------
// IsBmt
// ---------------------------------------------------------------------------
bool IsBmt(const u8* data, size_t size) {
    return data && size >= 7 && std::memcmp(data, kBmtSignaturePrefix, 7) == 0;
}

// ---------------------------------------------------------------------------
// ParseBmt
// ---------------------------------------------------------------------------
Result ParseBmt(const u8* data, size_t size, BmtFile& out) {
    out = BmtFile();

    if (!data || size < 16)
        return Result::Fail(Status::Truncated, T(L"BMT: dosya 16 bayttan kısa."));
    if (!IsBmt(data, size))
        return Result::Fail(Status::BadFormat, T(L"BMT: imza \"JMXVBMT\" değil."));

    ByteReader r(data, size);
    out.signature = r.readSignature(12);
    if (out.signature.size() >= 12)
        out.version = MultiByteToWide(out.signature.substr(8, 4), kPathCodePage);

    const u32 count = r.readU32();
    if (r.failed())
        return Result::Fail(Status::Truncated, T(L"BMT: malzeme sayısı okunamadı."));
    if (count && count > r.remaining() / kMinMaterialBytes)
        return Result::Fail(Status::BadFormat,
                            T(L"BMT: malzeme sayısı dosya boyutuna sığmıyor (") +
                            FormatCountTr(count) + L").");

    // --- read the material array, optionally honouring the bump-map block ---
    //
    // Bit 0x2000 is documented as "a normal-map block follows", and on some
    // clients it does. On others it does NOT: Origin's karkadann.bmt sets 0x2000
    // on both materials, yet the bytes right after material 0's diffuse block are
    // material 1's NAME. Trusting the flag there consumed a phantom string plus a
    // u32, desynced the reader and lost every material after the first - so four
    // of the model's five meshes ended up with no material, no texture, and
    // rendered as flat white ("bozuk goruntu"). The flag alone is not decidable,
    // so parse it BOTH ways and keep the reading that actually explains the file.
    struct Attempt {
        std::vector<BmtMaterial> mats;
        size_t consumed = 0;
        bool   complete = false;
    };
    const size_t arrayStart = r.pos();

    auto readArray = [&](bool honourBump) -> Attempt {
        Attempt a;
        ByteReader rr(data, size);
        rr.seek(arrayStart);
        a.mats.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            BmtMaterial m;
            m.name      = rr.readLenPrefixedStringW(kPathCodePage, 4096);
            m.diffuse   = ReadColor4(rr);
            m.ambient   = ReadColor4(rr);
            m.specular  = ReadColor4(rr);
            m.emissive  = ReadColor4(rr);
            m.unkFloat0 = rr.readF32();
            m.flags     = rr.readU32();

            m.diffuseMap.path       = rr.readLenPrefixedStringW(kPathCodePage, 4096);
            m.diffuseMap.unkFloat   = rr.readF32();
            m.diffuseMap.unkByte0   = rr.readU8();
            m.diffuseMap.unkByte1   = rr.readU8();
            m.diffuseMap.isAbsolute = rr.readBool8();

            if (honourBump && !rr.failed() && (m.flags & kMtrlFlagBumpMap)) {
                m.hasNormalMap   = true;
                m.normalMap.path = rr.readLenPrefixedStringW(kPathCodePage, 4096);
                m.normalMapUnk   = rr.readU32();
            }
            if (rr.failed()) break;
            a.mats.push_back(std::move(m));
        }
        a.consumed = rr.pos();
        a.complete = (a.mats.size() == count) && !rr.failed();
        return a;
    };

    Attempt withBump = readArray(true);
    // The winner is the one that reads EVERY material and lands on (or nearest
    // to) the end of the file; a desynced reading overshoots or truncates.
    auto score = [&](const Attempt& a) -> long long {
        if (!a.complete) return -1;
        const long long over = static_cast<long long>(size) - static_cast<long long>(a.consumed);
        return over >= 0 ? (1000000 - over) : -1;   // trailing slack is fine, overrun is not
    };

    Attempt best = std::move(withBump);
    if (score(best) < 1000000) {                    // not an exact fit: try the other reading
        Attempt noBump = readArray(false);
        if (score(noBump) > score(best)) best = std::move(noBump);
    }

    if (!best.complete) {
        return Result::Fail(Status::Truncated,
                            L"BMT: " + FormatCountTr(best.mats.size()) +
                            T(L". malzemede dosya bitti (") + FormatCountTr(count) +
                            T(L" bekleniyordu)."));
    }
    out.materials = std::move(best.mats);

    return Result::Ok();
}

// ---------------------------------------------------------------------------
// summaryTr
// ---------------------------------------------------------------------------
std::wstring BmtFile::summaryTr() const {
    std::wstring s;
    s += T(L"İmza          : ") + MultiByteToWide(signature, 1252) + L"\n";
    s += T(L"Malzeme sayısı: ") + FormatCountTr(materials.size()) + L"\n";

    const size_t shown = (materials.size() < 24) ? materials.size() : 24;
    for (size_t i = 0; i < shown; ++i) {
        const BmtMaterial& m = materials[i];
        s += L"\n[" + FormatCountTr(i) + L"] " + m.name + L"\n";
        s += T(L"    Diffuse   : ") + FormatColor(m.diffuse) + L"\n";
        s += T(L"    Ambient   : ") + FormatColor(m.ambient) + L"\n";
        s += T(L"    Specular  : ") + FormatColor(m.specular) + L"\n";
        s += T(L"    Emissive  : ") + FormatColor(m.emissive) + L"\n";
        s += FormatW(T(L"    Bayraklar : 0x%08X%s\n"), m.flags,
                     m.usesAlpha() ? T(L"  (alfa / metalik parlaklık)") : L"");
        if (!m.diffuseMap.path.empty()) {
            s += T(L"    Doku      : ") + m.diffuseMap.path;
            s += m.diffuseMap.isAbsolute ? T(L"   (mutlak yol)\n") : T(L"   (göreli yol)\n");
        }
        if (m.hasNormalMap && !m.normalMap.path.empty())
            s += T(L"    Normal    : ") + m.normalMap.path + L"\n";
    }
    if (materials.size() > shown)
        s += T(L"\n... ve ") + FormatCountTr(materials.size() - shown) + T(L" malzeme daha\n");

    return s;
}

} // namespace sro
