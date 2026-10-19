// SkillEffect Studio v3 - client/ClientInfo.cpp
#include "ClientInfo.h"

#include "Pk2Crypto.h"

#include <cstring>

namespace se::client {

namespace {

std::uint32_t Rd32(const Bytes& b, size_t at) {
    if (at + 4 > b.size()) return 0;
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

std::wstring QuotedValue(const std::wstring& text, const wchar_t* key) {
    size_t a = 0;
    while (a < text.size()) {
        size_t z = text.find_first_of(L"\r\n", a);
        if (z == std::wstring::npos) z = text.size();
        const std::wstring line = Trim(std::wstring_view(text).substr(a, z - a));
        a = z + 1;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        if (ToUpper(Trim(std::wstring_view(line).substr(0, eq))) != ToUpper(key)) continue;
        std::wstring v = Trim(std::wstring_view(line).substr(eq + 1));
        if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') v = v.substr(1, v.size() - 2);
        return v;
    }
    return {};
}

}  // namespace

int GameNameOffset(const std::wstring& language) {
    const std::wstring l = ToUpper(Trim(language));
    if (l == L"KOREA" || l == L"KOREAN") return 1;
    if (l == L"ENGLISH" || l == L"USA" || l == L"GLOBAL") return 7;
    if (l == L"VIETNAM" || l == L"VIETNAMESE") return 8;
    if (l == L"TURKEY" || l == L"TURKISH") return 11;
    if (l == L"ARABIC" || l == L"ARABIA") return 13;
    if (l == L"SPAIN" || l == L"SPANISH") return 14;
    if (l == L"GERMANY" || l == L"GERMAN") return 16;
    return -1;
}

ClientInfo ReadClientInfo(const Vfs& vfs) {
    ClientInfo ci;
    Bytes b;
    if (vfs.Read(ArcKind::Media, L"SV.T", b) && b.size() >= 12) {
        const std::uint32_t len = Rd32(b, 0);
        if (len >= 8 && len <= 64 && 4 + len <= b.size()) {
            Bytes c(b.begin() + 4, b.begin() + 4 + (len / 8) * 8);
            SroBlowfish bf;
            const std::uint8_t key[8] = {'S', 'I', 'L', 'K', 'R', 'O', 'A', 'D'};
            bf.Init(key, 8);
            bf.Decode(c.data(), c.size());
            std::string digits;
            for (std::uint8_t ch : c) {
                if (ch == 0) break;
                if (ch < '0' || ch > '9') {
                    digits.clear();
                    break;
                }
                digits += static_cast<char>(ch);
            }
            if (!digits.empty() && digits.size() <= 6) ci.version = std::stoi(digits);
        }
    }
    if (vfs.Read(ArcKind::Media, L"type.txt", b)) {
        const std::wstring t = DecodeText(b);
        ci.language = QuotedValue(t, L"Language");
        ci.country = QuotedValue(t, L"Country");
        ci.gameType = QuotedValue(t, L"GameType");
        ci.gameNameOffset = GameNameOffset(ci.language);
    }
    if (vfs.Read(ArcKind::Media, L"DIVISIONINFO.TXT", b) && b.size() >= 2) {
        // u8 locale, u8 divisions, { u32 len, name, NUL, u8 gateways, { u32 len, host, NUL } }
        size_t p = 0;
        ci.locale = b[p++];
        const int nd = b[p++];
        for (int d = 0; d < nd && p + 4 <= b.size(); ++d) {
            Division dv;
            const std::uint32_t nl = Rd32(b, p);
            p += 4;
            if (nl > 256 || p + nl + 2 > b.size()) break;
            dv.name = FromAnsi(std::string_view(reinterpret_cast<const char*>(&b[p]), nl), 1252);
            p += nl + 1;
            const int ng = b[p++];
            for (int g = 0; g < ng && p + 4 <= b.size(); ++g) {
                const std::uint32_t hl = Rd32(b, p);
                p += 4;
                if (hl > 256 || p + hl + 1 > b.size()) break;
                dv.gateways.push_back(FromAnsi(std::string_view(reinterpret_cast<const char*>(&b[p]), hl), 1252));
                p += hl + 1;
            }
            ci.divisions.push_back(std::move(dv));
        }
    }
    if (vfs.Read(ArcKind::Media, L"GATEPORT.TXT", b)) {
        long long v = 0;
        if (ParseInt(Trim(FromAnsi(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()), 1252)), v)) ci.gatePort = static_cast<int>(v);
    }
    return ci;
}

std::wstring ClientInfo::Describe(bool english) const {
    std::wstring s;
    if (version > 0) s += Fmt(english ? L"version %d" : L"sürüm %d", version);
    if (!language.empty()) s += (s.empty() ? L"" : L", ") + std::wstring(english ? L"language " : L"dil ") + language;
    for (const auto& d : divisions) {
        s += (s.empty() ? L"" : L", ") + d.name;
        if (!d.gateways.empty()) {
            s += L" (";
            for (size_t i = 0; i < d.gateways.size(); ++i) s += (i ? L", " : L"") + d.gateways[i];
            if (gatePort) s += Fmt(L":%d", gatePort);
            s += L")";
        }
    }
    return s.empty() ? (english ? L"unknown client" : L"bilinmeyen istemci") : s;
}

}  // namespace se::client
