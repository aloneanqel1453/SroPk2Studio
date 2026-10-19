// SkillEffect Studio v3 - client/KeyFinder.h
// Pk2 key discovery for ANY client (SROAssetStudio KeyFinder logic minus everything that starts or reads a process).
// Every candidate is proven with the root-block oracle against the target archive before it is used.
//   stage 0  session memory (archive fingerprint -> key, process lifetime only) + keys that opened sibling archives
//   stage 1  typed key, 169841, user hint files (pk2key.txt / pk2_key.txt / key.txt in the client folder, read only)
//            each x {STD, PARADISE, ZERO base} + the raw-bytes reading of a hex key
//   stage 2  static scan of the exe / dll files of the client folder (and its bin/ subfolder): ascii candidates within
//            +-4 KB of locator strings ("Wrong BlowfishKey", "GFXFileManager.dll", ".?AVCBlowFish@@", ...), the
//            8/10/12/16 non-zero bytes right before a candidate tried as its base key; whole-file sweep as fallback
// RULES: no key is ever written to disk or put in a log line by this module (reports say HOW, not WHAT);
//        nothing is launched, no process memory is read.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace se::client {

struct KeyReport {
    bool found = false;
    bool unencrypted = false;   // the archive is not encrypted: any key works
    std::string spec;           // usable key spec (memory only)
    int stage = -1;             // 0 session/sibling, 1 typed/default/hint, 2 file scan
    std::wstring howTr, howEn;  // e.g. "istemci dosyasından okundu: GFXFileManager.dll" (never the key itself)
    std::wstring whyTr, whyEn;  // plain reason when not found
    int tests = 0, filesScanned = 0;
    bool cancelled = false;
};

struct KeyFindOptions {
    std::string typed;                    // the key typed by the user (may be empty)
    std::vector<std::string> extra;       // keys that opened sibling archives (stage 0)
    bool readHintFiles = true;            // pk2key.txt ... next to the archive
    bool scanBinaries = true;             // stage 2
    int maxTests = 800000;                // stage 2 budget
    std::function<void(int percent, const std::wstring& what)> progress;
    const std::atomic<bool>* cancel = nullptr;
};

KeyReport FindKey(const std::wstring& pk2Path, const KeyFindOptions& opt);

// session memory (process lifetime only, never persisted)
void RememberKey(const std::wstring& pk2Path, const std::string& spec);
std::string RecallKey(const std::wstring& pk2Path);  // "" when unknown or the archive changed
void ForgetKeys();                                    // wipes the session memory

}  // namespace se::client
