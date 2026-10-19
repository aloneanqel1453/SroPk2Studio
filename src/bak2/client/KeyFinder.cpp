// SkillEffect Studio v3 - client/KeyFinder.cpp
#include "KeyFinder.h"

#include "../core/Text.h"
#include "Pk2Crypto.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_set>

namespace se::client {

namespace {

std::mutex g_memMx;
std::map<std::string, std::string>& Mem() {
    static std::map<std::string, std::string> m;
    return m;
}

std::string HexOf(const std::uint8_t* p, size_t n) {
    static const char* hx = "0123456789abcdef";
    std::string h;
    for (size_t t = 0; t < n; ++t) {
        h += hx[p[t] >> 4];
        h += hx[p[t] & 15];
    }
    return h;
}

bool LooksHex(const std::string& s) {
    if (s.size() < 16 || s.size() % 2) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

std::string TrimA(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n' || static_cast<unsigned char>(s[a]) == 0xEF ||
                     static_cast<unsigned char>(s[a]) == 0xBB || static_cast<unsigned char>(s[a]) == 0xBF))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::vector<std::wstring> ListBins(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE fh = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (fh == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring n = fd.cFileName;
        const unsigned long long sz = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        if ((EndsWithI(n, L".exe") || EndsWithI(n, L".dll")) && sz > 0 && sz <= 256ull * 1024 * 1024)
            out.push_back(JoinPath(dir, n));
    } while (FindNextFileW(fh, &fd));
    FindClose(fh);
    return out;
}

}  // namespace

void RememberKey(const std::wstring& pk2Path, const std::string& spec) {
    Pk2Probe p;
    if (!p.Load(pk2Path)) return;
    std::lock_guard<std::mutex> lk(g_memMx);
    Mem()[p.Fingerprint()] = spec;
}

std::string RecallKey(const std::wstring& pk2Path) {
    Pk2Probe p;
    if (!p.Load(pk2Path)) return {};
    std::lock_guard<std::mutex> lk(g_memMx);
    auto it = Mem().find(p.Fingerprint());
    return it == Mem().end() ? std::string() : it->second;
}

void ForgetKeys() {
    std::lock_guard<std::mutex> lk(g_memMx);
    for (auto& kv : Mem()) SecureZeroMemory(kv.second.data(), kv.second.size());
    Mem().clear();
}

KeyReport FindKey(const std::wstring& pk2Path, const KeyFindOptions& opt) {
    KeyReport rep;
    Pk2Probe ap;
    if (!ap.Load(pk2Path)) {
        rep.whyTr = ap.whyTr;
        rep.whyEn = ap.whyEn;
        return rep;
    }
    const std::wstring arcName = FileNameOf(pk2Path);
    if (!ap.encrypted) {
        rep.found = true;
        rep.unencrypted = true;
        rep.spec = opt.typed.empty() ? "169841" : opt.typed;
        rep.stage = 1;
        rep.howTr = L"arşiv şifresiz (anahtar gerekmiyor)";
        rep.howEn = L"archive is not encrypted (no key needed)";
        return rep;
    }
    auto cancelled = [&] { return opt.cancel && opt.cancel->load(); };
    std::unordered_set<std::string> tried;
    auto test = [&](const std::string& spec) -> bool {
        if (spec.empty() || !tried.insert(spec).second) return false;
        ++rep.tests;
        return ap.TestSpec(spec);
    };
    auto hit = [&](const std::string& spec, int stage, const std::wstring& tr, const std::wstring& en) {
        rep.found = true;
        rep.spec = spec;
        rep.stage = stage;
        rep.howTr = tr + L" (" + KeySpecFormTr(spec) + L")";
        rep.howEn = en;
        RememberKey(pk2Path, spec);
    };
    // try a spec; a plain ascii spec also with the PARADISE / ZERO bases, a hex string also as raw key bytes
    auto tryVariants = [&](const std::string& k) -> std::string {
        if (test(k)) return k;
        if (KeySpecIsPlainAscii(k)) {
            for (const char* b : {"@PARADISE", "@ZERO"})
                if (test(k + b)) return k + b;
            if (LooksHex(k) && test("raw:" + k)) return "raw:" + k;
        }
        return {};
    };

    // ---- stage 0: session memory + sibling keys
    if (opt.progress) opt.progress(0, L"Oturum anahtarları deneniyor");
    {
        std::string mem;
        {
            std::lock_guard<std::mutex> lk(g_memMx);
            auto it = Mem().find(ap.Fingerprint());
            if (it != Mem().end()) mem = it->second;
        }
        if (!mem.empty() && test(mem)) {
            hit(mem, 0, L"bu oturumda aynı arşivi açan anahtar", L"key that opened this archive earlier in this session");
            return rep;
        }
        for (const auto& k : opt.extra) {
            const std::string s = tryVariants(k);
            if (!s.empty()) {
                hit(s, 0, L"aynı istemcinin başka bir pk2'sini açan anahtar", L"key of a sibling archive of this client");
                return rep;
            }
        }
    }
    // ---- stage 1: typed, default, hint files
    if (opt.progress) opt.progress(2, L"Yazılan / bilinen anahtarlar deneniyor");
    if (!opt.typed.empty()) {
        const std::string s = tryVariants(TrimA(opt.typed));
        if (!s.empty()) {
            hit(s, 1, L"yazılan anahtar", L"typed key");
            return rep;
        }
    }
    {
        const std::string s = tryVariants("169841");
        if (!s.empty()) {
            hit(s, 1, L"Silkroad varsayılan anahtarı", L"Silkroad default key");
            return rep;
        }
    }
    const std::wstring dir = DirOf(pk2Path);
    if (opt.readHintFiles) {
        for (const wchar_t* hf : {L"pk2key.txt", L"pk2_key.txt", L"key.txt", L"blowfishkey.txt"}) {
            Bytes b;
            std::wstring e;
            const std::wstring p = JoinPath(dir, hf);
            if (!FileExists(p) || !ReadFileBytes(p, b, e) || b.size() > 64 * 1024) continue;
            std::string all(b.begin(), b.end());
            if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xFE) all = ToUtf8(DecodeText(b));
            size_t a = 0;
            while (a < all.size()) {
                size_t z = all.find('\n', a);
                if (z == std::string::npos) z = all.size();
                const std::string line = TrimA(all.substr(a, z - a));
                a = z + 1;
                if (line.empty() || line[0] == '#' || line.rfind("//", 0) == 0) continue;
                const std::string s = tryVariants(line);
                if (!s.empty()) {
                    hit(s, 1, std::wstring(L"istemci klasöründeki ") + hf + L" dosyası", std::wstring(L"hint file ") + hf);
                    return rep;
                }
            }
        }
    }
    if (cancelled()) {
        rep.cancelled = true;
        rep.whyTr = L"Anahtar arama iptal edildi.";
        rep.whyEn = L"Key search cancelled.";
        return rep;
    }
    // ---- stage 2: static scan of the client's exe / dll files (read only; nothing is started)
    std::vector<std::wstring> bins = ListBins(dir);
    {
        const std::wstring sub = JoinPath(dir, L"bin");
        if (DirExists(sub))
            for (auto& b : ListBins(sub)) bins.push_back(b);
    }
    auto rank = [](const std::wstring& n) {
        const std::wstring l = ToUpper(FileNameOf(n));
        if (l.find(L"GFX") != std::wstring::npos) return 0;
        if (l.find(L"CLIENT") != std::wstring::npos) return 1;
        if (l.find(L"GUARD") != std::wstring::npos || l.find(L"LIB") != std::wstring::npos || l.find(L"VERSION") != std::wstring::npos)
            return 2;
        if (l.find(L"LAUNCH") != std::wstring::npos || l.find(L"SILKROAD") != std::wstring::npos) return 3;
        return 4;
    };
    std::stable_sort(bins.begin(), bins.end(), [&](const std::wstring& a, const std::wstring& b) { return rank(a) < rank(b); });
    if (opt.scanBinaries && !bins.empty()) {
        static const char* kLoc[] = {"Wrong BlowfishKey", "MaxiGuard DLL:", "GFXFileManager.dll", ".?AVCBlowFish@@",
                                     "Joymax Pack File", "JoyMax File Manager"};
        // pass 0: windows around locators in every file; pass 1: whole-file sweep of files <= 32 MB
        for (int pass = 0; pass < 2 && !rep.found; ++pass) {
            size_t fi = 0;
            for (const auto& path : bins) {
                if (cancelled()) {
                    rep.cancelled = true;
                    break;
                }
                if (opt.progress)
                    opt.progress(5 + static_cast<int>((pass * bins.size() + fi) * 90 / (2 * bins.size())),
                                 L"Taranıyor: " + FileNameOf(path));
                ++fi;
                Bytes b;
                std::wstring e;
                if (!ReadFileBytes(path, b, e) || b.size() < 16) continue;
                if (pass == 0) ++rep.filesScanned;
                if (pass == 1 && b.size() > 32ull * 1024 * 1024) continue;
                std::vector<std::pair<size_t, size_t>> ranges;
                if (pass == 0) {
                    for (const char* loc : kLoc) {
                        const size_t ln = std::strlen(loc);
                        size_t p = 0;
                        int n = 0;
                        while (p + ln <= b.size() && n < 64) {
                            const auto it = std::search(b.begin() + static_cast<std::ptrdiff_t>(p), b.end(), loc, loc + ln);
                            if (it == b.end()) break;
                            const size_t at = static_cast<size_t>(it - b.begin());
                            ranges.push_back({at > 4096 ? at - 4096 : 0, std::min(b.size(), at + 4096)});
                            p = at + 1;
                            ++n;
                        }
                    }
                } else {
                    ranges.push_back({0, b.size()});
                }
                const size_t minLen = pass == 0 ? 4 : 6;
                for (const auto& rg : ranges) {
                    size_t i = rg.first;
                    while (i < rg.second && !rep.found) {
                        if (rep.tests >= opt.maxTests || cancelled()) break;
                        size_t j = i;
                        while (j < rg.second && b[j] >= 0x21 && b[j] <= 0x7E) ++j;
                        const size_t len = j - i;
                        if (len >= minLen && len <= 56 && j < b.size() && b[j] == 0) {
                            const std::string cand(reinterpret_cast<const char*>(&b[i]), len);
                            std::vector<std::string> specs{cand, cand + "@PARADISE", cand + "@ZERO"};
                            size_t q = i;
                            int pad = 0;
                            while (q > 0 && b[q - 1] == 0 && pad < 8) {
                                --q;
                                ++pad;
                            }
                            for (size_t bl : {size_t{10}, size_t{8}, size_t{12}, size_t{16}}) {
                                if (q < bl) continue;
                                bool nz = true;
                                for (size_t t = q - bl; t < q; ++t) nz = nz && b[t] != 0;
                                if (nz) specs.push_back(cand + "@" + HexOf(&b[q - bl], bl));
                            }
                            for (const auto& sp : specs) {
                                if (test(sp)) {
                                    hit(sp, 2, L"istemci dosyasından okundu: " + FileNameOf(path),
                                        L"read from the client file " + FileNameOf(path));
                                    break;
                                }
                            }
                        }
                        i = j + 1;
                    }
                    if (rep.found || rep.tests >= opt.maxTests || cancelled()) break;
                }
                if (rep.found || rep.tests >= opt.maxTests) break;
            }
            if (rep.cancelled || rep.tests >= opt.maxTests) break;
        }
        if (rep.found) {
            if (opt.progress) opt.progress(100, L"Anahtar bulundu");
            return rep;
        }
    }
    if (opt.progress) opt.progress(100, L"Anahtar bulunamadı");
    if (rep.cancelled) {
        rep.whyTr = L"Anahtar arama iptal edildi.";
        rep.whyEn = L"Key search cancelled.";
        return rep;
    }
    rep.whyTr = Fmt(L"%s için pk2 anahtarı bulunamadı. Denenenler: yazılan anahtar, 169841 (STD / PARADISE / tabansız), "
                    L"istemci klasöründeki anahtar dosyaları (pk2key.txt) ve klasördeki %d exe/dll dosyasında %d aday. "
                    L"Bu sunucunun anahtarı korumalı (şifreli) bir dosyanın içinde olabilir. Anahtarı sunucu yöneticisinden "
                    L"öğrenip 'Pk2 anahtarı' kutusuna yazın (örnek: 169841, ANAHTAR@PARADISE, ANAHTAR@ZERO) ya da istemci "
                    L"klasörüne pk2key.txt olarak koyun. Araç oyunu asla çalıştırmaz, oyunun belleğini okumaz.",
                    arcName.c_str(), rep.filesScanned, rep.tests);
    rep.whyEn = Fmt(L"No pk2 key found for %s. Tried: the typed key, 169841 (STD / PARADISE / no base), hint files "
                    L"(pk2key.txt) and %d candidates in %d exe/dll files. The key may be inside a protected file: ask the "
                    L"server admin and type it in the 'Pk2 key' box or put it in pk2key.txt. The tool never starts the game "
                    L"or reads its memory.",
                    arcName.c_str(), rep.tests, rep.filesScanned);
    return rep;
}

}  // namespace se::client
