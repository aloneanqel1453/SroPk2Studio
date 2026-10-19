// ============================================================================
//  SRO Asset Studio - core/AssetCipherGopro.cpp
// ============================================================================
#include "AssetCipherGopro.h"

#include <mutex>
#include <cstring>
#include <algorithm>

#include "AesCore.h"
#include "IArchive.h"
#include "SiblingArchives.h"

namespace sro {

namespace {

struct GoproProfile {
    const wchar_t* name;
    u8 key[32];
    u8 iv[16];
};

static const GoproProfile kKnownProfiles[] = {
    {
        L"Republic90 / VSROPlus",
        {
            0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbd, 0xef,
            0x29, 0x01, 0x06, 0xbb, 0x99, 0x3d, 0xfe, 0x7e,
            0xa4, 0xe9, 0xef, 0xba, 0x40, 0xf5, 0x3e, 0x99,
            0x4b, 0x05, 0x14, 0x95, 0xfb, 0x2b, 0xf2, 0x7d
        },
        {
            0xcb, 0x36, 0x32, 0xef, 0x72, 0x3d, 0xb0, 0x6e,
            0x2a, 0x4d, 0xf8, 0xd9, 0xa6, 0xd3, 0x85, 0x88
        }
    }
};

static std::mutex g_mtx;
static bool       g_active = false;
static u8         g_key[32] = {};
static u8         g_iv[16]  = {};

} // namespace

void GoproSetKey(const u8 key[32], const u8 iv[16]) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!key || !iv) {
        g_active = false;
        std::memset(g_key, 0, sizeof(g_key));
        std::memset(g_iv, 0, sizeof(g_iv));
        return;
    }
    std::memcpy(g_key, key, 32);
    std::memcpy(g_iv, iv, 16);
    g_active = true;
}

bool GoproActive() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_active;
}

void GoproClear() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_active = false;
    SecureZeroMemory(g_key, sizeof(g_key));
    SecureZeroMemory(g_iv, sizeof(g_iv));
}

std::wstring GoproStatusTr() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_active ? L"GoPro / VSROPlus (AES-256-CBC)" : L"(yok)";
}

bool GoproTryInit(IArchive& archive) {
    // Check if GoPro protector files exist in the archive or sibling containers
    const wchar_t* kMarkers[] = {
        L"server_dep\\silkroad\\textdata\\gopro\\clienthash.txt",
        L"server_dep\\silkroad\\textdata\\gopro\\encryptedfiles.txt",
        L"textdata\\gopro\\clienthash.txt",
        L"textdata\\gopro\\encryptedfiles.txt"
    };

    bool found = false;
    for (const wchar_t* m : kMarkers) {
        std::vector<u8> raw;
        if (archive.isOpen() && archive.readFile(m, raw).ok() && !raw.empty()) {
            found = true;
            break;
        }
        raw.clear();
        if (SiblingArchives::Instance().read(m, raw) && !raw.empty()) {
            found = true;
            break;
        }
    }

    if (!found) {
        // Also check if vsroplus_lib.dll exists next to archive
        if (archive.isOpen()) {
            std::wstring dir = PathParent(archive.path());
            if (FileExists(dir + L"\\vsroplus_lib.dll") ||
                FileExists(PathParent(dir) + L"\\vsroplus_lib.dll")) {
                found = true;
            }
        }
    }

    if (found) {
        // Arm with default/known GoPro profile
        GoproSetKey(kKnownProfiles[0].key, kKnownProfiles[0].iv);
        return true;
    }

    return false;
}

bool GoproDecryptIfEncrypted(const std::wstring& innerPath, std::vector<u8>& buf) {
    if (buf.size() < 16 || (buf.size() % 16) != 0) return false;

    // Already plaintext JMXV asset
    if (buf[0] == 'J' && buf[1] == 'M' && buf[2] == 'X' && buf[3] == 'V')
        return false;

    // Already plaintext PNG
    if (buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G')
        return false;

    // Gather candidate key/iv pairs to test
    struct Cand { const u8* k; const u8* iv; };
    std::vector<Cand> cands;

    u8 sessK[32], sessIv[16];
    bool hasSess = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_active) {
            std::memcpy(sessK, g_key, 32);
            std::memcpy(sessIv, g_iv, 16);
            hasSess = true;
        }
    }
    if (hasSess) {
        cands.push_back({ sessK, sessIv });
    }

    for (const auto& p : kKnownProfiles) {
        if (!hasSess || std::memcmp(p.key, sessK, 32) != 0) {
            cands.push_back({ p.key, p.iv });
        }
    }

    const std::wstring ext = ToLowerW(PathExtension(innerPath));
    const bool isPng = (ext == L".png");
    const bool isTxt = (ext == L".txt" || ext == L".ini");

    for (const auto& c : cands) {
        aes::Key kExp;
        if (!aes::Expand(c.k, 32, kExp)) continue;

        u8 probe[16] = {};
        aes::DecryptBlock(kExp, buf.data(), probe);
        for (int i = 0; i < 16; ++i) probe[i] ^= c.iv[i];

        bool match = false;
        if (probe[0] == 'J' && probe[1] == 'M' && probe[2] == 'X' && probe[3] == 'V') {
            match = true;
        } else if (isPng && probe[0] == 0x89 && probe[1] == 'P' && probe[2] == 'N' && probe[3] == 'G') {
            match = true;
        } else if (isTxt && probe[0] >= 0x20 && probe[0] <= 0x7E &&
                            probe[1] >= 0x20 && probe[1] <= 0x7E &&
                            probe[2] >= 0x20 && probe[2] <= 0x7E) {
            match = true;
        }

        if (match) {
            std::vector<u8> plain(buf.size());
            if (!aes::CbcDecrypt(c.k, 32, c.iv, buf.data(), buf.size(), plain.data()))
                return false;

            // Strip PKCS#7 padding if present
            const u8 last = plain.back();
            if (last >= 1 && last <= 16 && plain.size() >= last) {
                bool pad = true;
                for (size_t i = plain.size() - last; i < plain.size(); ++i) {
                    if (plain[i] != last) { pad = false; break; }
                }
                if (pad) plain.resize(plain.size() - last);
            }

            buf.swap(plain);
            return true;
        }
    }

    return false;
}

bool GoproLooksEncrypted(const u8* data, size_t size) {
    if (!data || size < 16 || (size % 16) != 0) return false;
    if (data[0] == 'J' && data[1] == 'M' && data[2] == 'X' && data[3] == 'V') return false;

    // Test probe with known profile
    aes::Key kExp;
    if (!aes::Expand(kKnownProfiles[0].key, 32, kExp)) return false;

    u8 probe[16] = {};
    aes::DecryptBlock(kExp, data, probe);
    for (int i = 0; i < 16; ++i) probe[i] ^= kKnownProfiles[0].iv[i];

    return (probe[0] == 'J' && probe[1] == 'M' && probe[2] == 'X' && probe[3] == 'V');
}

} // namespace sro
