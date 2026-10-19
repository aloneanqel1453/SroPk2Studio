// SkillEffect Studio - core/SkillCrypt.cpp
#include "SkillCrypt.h"

#include "SkillKeyData.h"

#include <cstring>
#include <mutex>

namespace se {

namespace {

constexpr std::uint8_t kMarker[22] = {'\r', 0, '\n', 0, '/', 0, '/', 0, 'e', 0, 'n', 0,
                                      'c', 0, 'r', 0, 'y', 0, 'p', 0, 't', 0};

const std::uint8_t* Key() {
    static std::uint8_t key[detail::kSkillKeyLen];
    static std::once_flag once;
    std::call_once(once, [] {
        for (unsigned i = 0; i < detail::kSkillKeyLen; ++i)
            key[i] = static_cast<std::uint8_t>(detail::kSkillKeyMasked[i] ^
                                               static_cast<std::uint8_t>(0x5A + i * 0x3B + (i >> 8) * 7));
    });
    return key;
}

}  // namespace

bool IsSkillEnc(const Bytes& b) {
    return b.size() >= sizeof(kMarker) && std::memcmp(b.data() + b.size() - sizeof(kMarker), kMarker, sizeof(kMarker)) == 0;
}

bool SkillDecrypt(const Bytes& enc, Bytes& plain) {
    if (!IsSkillEnc(enc)) return false;
    const std::uint8_t* k = Key();
    const size_t n = enc.size() - sizeof(kMarker);
    plain.resize(n);
    for (size_t i = 0, j = 0; i < n; ++i, j = (j + 1 == detail::kSkillKeyLen ? 0 : j + 1))
        plain[i] = static_cast<std::uint8_t>(enc[i] - k[j]);
    return true;
}

Bytes SkillEncrypt(const Bytes& plain) {
    const std::uint8_t* k = Key();
    Bytes out(plain.size() + sizeof(kMarker));
    for (size_t i = 0, j = 0; i < plain.size(); ++i, j = (j + 1 == detail::kSkillKeyLen ? 0 : j + 1))
        out[i] = static_cast<std::uint8_t>(plain[i] + k[j]);
    std::memcpy(out.data() + plain.size(), kMarker, sizeof(kMarker));
    return out;
}

std::wstring SkillDataText(const Bytes& b) {
    if (IsSkillEnc(b)) {
        Bytes p;
        SkillDecrypt(b, p);
        return DecodeText(p);
    }
    return DecodeText(b);
}

}  // namespace se
