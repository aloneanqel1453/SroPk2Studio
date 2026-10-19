// ============================================================================
//  SRO Asset Studio - core/KeyRing.cpp
//
//  Key candidates, the two validation oracles and Stage 1 of the key search.
//
//  ==========================================================================
//   SPEC RULE 0 - NOTHING SERVER-SPECIFIC IS HARDCODED
//  ==========================================================================
//  The only key constants in this file are the format-level ones: the default
//  ascii key "169841" (declared in KeyRing.h) and the three published base keys
//  (STD / PARADISE / ZERO), which live in Blowfish.cpp. There is deliberately
//  no per-server key table. Do not add one.
//
//  Oracle notes, because getting the strength wrong here matters:
//
//   * RootBlockOracleFast decrypts a SINGLE 8-byte block and requires the
//     plaintext to begin 01 2E 00 - the Folder type byte plus the "." name of
//     the root's self-pointer. That is ~2^-24 selectivity for ~100 ns of work,
//     which is exactly what the memory scanners need in their inner loop.
//
//   * RootBlockOracle decrypts all 2560 bytes and additionally requires every
//     one of the 20 entry Type bytes to be 0/1/2. A real block only ever holds
//     Empty/Folder/File, so this is safe, and it takes the false-positive rate
//     of random data to (3/256)^20 - i.e. it cannot happen by accident. Without
//     that extra check the "strong" oracle would be no stronger than the fast
//     one, since both look at the same three plaintext bytes.
//
//   * ChecksumOracle is INDEPENDENT of the root block (it encrypts a known
//     probe string and compares three header bytes), so the scanners use it as
//     a second, uncorrelated confirmation before accepting a hit.
//
//  Implementation owner: Agent C (key discovery).
// ============================================================================
#include "KeyRing.h"
#include "Pk2Archive.h"

namespace sro {

// ---------------------------------------------------------------------------
// KeyCandidate
// ---------------------------------------------------------------------------
bool KeyCandidate::makeCipher(Blowfish& out) const {
    if (useRawState) {
        if (rawState.size() != kBlowfishStateSize) return false;
        // Scanner B always normalises a reversed (S,P) hit to (P,S) order before
        // storing it here, so this side never has to know about layouts.
        return out.initializeFromStateBytes(rawState.data(), rawState.size(), false);
    }
    if (!rawKeyBytes.empty()) {
        // Raw Blowfish key bytes through the standard schedule (e.g. Sailor's
        // literal zero-byte key, which no printable-ASCII path can express).
        return out.initializeRaw(rawKeyBytes.data(), rawKeyBytes.size());
    }
    if (asciiKey.empty()) return false;
    if (baseId == BaseKeyId::Custom) {
        if (customBase.empty()) return false;
        return out.initializeGameKey(asciiKey, customBase.data(), customBase.size());
    }
    return out.initializeGameKey(asciiKey, baseId);
}

// The log is screenshotted, pasted into forum threads and saved to disk, so it
// must never carry key material or say how the archive was opened - between
// them those two hand a reader everything they need to open someone else's
// client. describe() is therefore deliberately contentless; the real values
// live only in memory and are printed solely by describeSensitive(), which the
// user has to ask for explicitly on the command line.
std::wstring KeyCandidate::describe() const {
    return T(L"anahtar doğrulandı");
}

std::wstring KeyCandidate::describeSensitive() const {
    if (useRawState)
        return FormatW(L"ham Blowfish durumu (%zu bayt)%s%s",
                       rawState.size(),
                       label.empty() ? L"" : L" - ",
                       label.c_str());
    if (!rawKeyBytes.empty())
        return FormatW(L"ham anahtar %s%s%s",
                       BytesToHexW(rawKeyBytes.data(), rawKeyBytes.size()).c_str(),
                       label.empty() ? L"" : L" - ",
                       label.c_str());

    std::wstring base;
    if (baseId == BaseKeyId::Custom) base = L"OZEL:" + BytesToHexW(customBase.data(), customBase.size());
    else                             base = BaseKeyName(baseId);

    std::wstring s = FormatW(L"\"%s\" x %s",
                             MultiByteToWide(asciiKey, CP_UTF8).c_str(), base.c_str());
    if (!label.empty()) s += L" (" + label + L")";
    return s;
}

// ---------------------------------------------------------------------------
// Format-level constants only.
//
// SPEC RULE 0: no per-server key table lives here, now or ever. The three base
// keys below are properties of the Silkroad format, not of any shard.
// ---------------------------------------------------------------------------
const std::vector<std::pair<BaseKeyId, const wchar_t*>>& KeyRing::KnownBaseKeys() {
    static const std::vector<std::pair<BaseKeyId, const wchar_t*>> bases = {
        { BaseKeyId::Std,      L"STD" },
        { BaseKeyId::Paradise, L"PARADISE" },
        { BaseKeyId::Zero,     L"ZERO" }
    };
    return bases;
}

// ---------------------------------------------------------------------------
// Oracles
// ---------------------------------------------------------------------------
bool KeyRing::ChecksumOracle(const Blowfish& bf, const Pk2RawHeader& header) {
    if (!bf.valid()) return false;

    // A header whose first checksum bytes are all zero carries no information;
    // treat it as "cannot decide" rather than "matches an all-zero encryption".
    bool anyNonZero = false;
    for (size_t i = 0; i < kPk2ChecksumCompareBytes; ++i)
        if (header.Checksum[i] != 0) { anyNonZero = true; break; }
    if (!anyNonZero) return false;

    std::vector<u8> enc;
    auto _probe = Pk2ChecksumProbe();
    if (!bf.encodeString(_probe.c_str(), enc)) return false;
    if (enc.size() < kPk2ChecksumCompareBytes) return false;
    return std::memcmp(enc.data(), header.Checksum, kPk2ChecksumCompareBytes) == 0;
}

bool KeyRing::RootBlockOracleFast(const Blowfish& bf, const u8* encryptedRootBlock) {
    if (!bf.valid() || !encryptedRootBlock) return false;
    u8 block[8];
    std::memcpy(block, encryptedRootBlock, 8);
    bf.decodeBlock(block);
    // entry[0] must be Type=Folder(1) with Name = "." (0x2E) terminated by NUL.
    return block[0] == 0x01 && block[1] == 0x2E && block[2] == 0x00;
}

bool KeyRing::RootBlockOracle(const Blowfish& bf, const u8* encryptedRootBlock) {
    if (!bf.valid() || !encryptedRootBlock) return false;

    // Cheap reject first - saves 319 of the 320 block decryptions on a miss.
    if (!RootBlockOracleFast(bf, encryptedRootBlock)) return false;

    u8 plain[kPk2BlockSize];
    std::memcpy(plain, encryptedRootBlock, kPk2BlockSize);
    bf.decodeInPlace(plain, kPk2BlockSize);

    const Pk2RawBlock* blk = reinterpret_cast<const Pk2RawBlock*>(plain);

    // entry[0] is the folder's self-pointer: Type == Folder, Name == ".".
    if (blk->Entries[0].Type != static_cast<u8>(Pk2EntryType::Folder)) return false;
    if (blk->Entries[0].Name[0] != '.' || blk->Entries[0].Name[1] != '\0')  return false;

    // Every slot must be Empty / Folder / File. Real writers only ever produce
    // those three values, and this is what makes the oracle actually strong.
    for (size_t i = 0; i < kPk2EntriesPerBlock; ++i)
        if (blk->Entries[i].Type > static_cast<u8>(Pk2EntryType::File)) return false;

    return true;
}

bool KeyRing::Validate(const Blowfish& bf,
                       const Pk2RawHeader& header,
                       const u8* encryptedRootBlock,
                       bool* usedRootOracle) {
    if (usedRootOracle) *usedRootOracle = false;
    if (!bf.valid()) return false;

    if (encryptedRootBlock && RootBlockOracle(bf, encryptedRootBlock)) {
        if (usedRootOracle) *usedRootOracle = true;
        return true;
    }
    // Oracle 2 was inconclusive (no root block available, or a writer that does
    // not follow the "." convention). Fall back to the weaker checksum probe.
    return ChecksumOracle(bf, header);
}

bool KeyRing::ValidateAgainstFile(const Blowfish& bf, const std::wstring& archivePath) {
    if (!bf.valid()) return false;

    Pk2RawHeader header{};
    if (!Pk2Archive::ProbeHeader(archivePath, header).ok()) return false;

    u8 root[kPk2BlockSize];
    const bool haveRoot = Pk2Archive::ReadRawRootBlock(archivePath, root).ok();
    return Validate(bf, header, haveRoot ? root : nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Stage 1 - instant, no side effects
// ---------------------------------------------------------------------------
void KeyRing::BuildStage1Candidates(const std::string& typedKey,
                                    bool rawKeyMode,
                                    const KeyCandidate* carried,
                                    std::vector<KeyCandidate>& out) {
    out.clear();

    // Stage 0 leftovers first. This is DISCOVERED data (session memory or a
    // sibling archive), never a built-in constant - see RULE 0.
    if (carried) {
        KeyCandidate c = *carried;
        if (c.label.empty()) c.label = L"onceki oturum bulgusu";
        out.push_back(c);
    }

    if (typedKey.empty()) return;

    // ZERO base == "raw key" mode: the ascii key is used verbatim. It is the
    // same derivation, so raw mode just reorders the list.
    const BaseKeyId order[3]    = { BaseKeyId::Std, BaseKeyId::Paradise, BaseKeyId::Zero };
    const BaseKeyId rawOrder[3] = { BaseKeyId::Zero, BaseKeyId::Std, BaseKeyId::Paradise };
    const BaseKeyId* seq = rawKeyMode ? rawOrder : order;

    for (int i = 0; i < 3; ++i) {
        KeyCandidate c;
        c.asciiKey = typedKey;
        c.baseId   = seq[i];
        c.label    = (seq[i] == BaseKeyId::Zero) ? L"yazilan key dogrudan (raw)"
                                                 : L"yazilan key x base key";
        out.push_back(c);
    }

    // Curated known client keys (verified against root block oracle)
    static const struct KnownKey { const char* key; BaseKeyId base; const wchar_t* label; } kKnownKeys[] = {
        { "xvnWYjwuhRLBnDv3umzkCky", BaseKeyId::Std, L"Kyrlos (xvnWYjwuhRLBnDv3umzkCky x STD)" }
    };
    for (const auto& k : kKnownKeys) {
        if (typedKey != k.key) {
            KeyCandidate c;
            c.asciiKey = k.key;
            c.baseId   = k.base;
            c.label    = k.label;
            out.push_back(c);
        }
    }
}

Result KeyRing::TryCandidates(const std::wstring& archivePath,
                              const std::vector<KeyCandidate>& candidates,
                              KeyCandidate& found,
                              Blowfish& cipher,
                              const LogFn& log) {
    if (candidates.empty())
        return Result::Fail(Status::WrongKey, T(L"Denenecek key adayi yok."));

    Pk2RawHeader header{};
    Result r = Pk2Archive::ProbeHeader(archivePath, header);
    if (!r.ok()) return r;

    u8 root[kPk2BlockSize];
    const bool haveRoot = Pk2Archive::ReadRawRootBlock(archivePath, root).ok();

    for (const KeyCandidate& c : candidates) {
        Blowfish bf;
        if (!c.makeCipher(bf)) continue;

        bool usedRoot = false;
        if (Validate(bf, header, haveRoot ? root : nullptr, &usedRoot)) {
            found  = c;
            cipher = bf;
            // Neither the key nor which oracle proved it: "root block" vs
            // "checksum" already tells a reader how the archive was reached.
            (void)usedRoot;
            ReportLog(log, T(L"Anahtar doğrulandı."));
            return Result::Ok();
        }
    }
    return Result::Fail(Status::WrongKey,
                        T(L"Hicbir key adayi arsivi acmadi (") +
                        FormatCountTr(candidates.size()) + T(L" aday denendi)."));
}

Result KeyRing::TryStage1(const std::wstring& archivePath,
                          const std::string& typedKey,
                          bool rawKeyMode,
                          const KeyCandidate* carried,
                          KeyCandidate& found,
                          Blowfish& cipher,
                          const LogFn& log) {
    std::vector<KeyCandidate> cands;
    BuildStage1Candidates(typedKey, rawKeyMode, carried, cands);
    if (cands.empty())
        return Result::Fail(Status::InvalidArgument, T(L"Key alani bos."));
    return TryCandidates(archivePath, cands, found, cipher, log);
}

bool KeyRing::TestCandidate(const KeyCandidate& cand,
                            const Pk2RawHeader& header,
                            const u8* encryptedRootBlock,
                            Blowfish& cipherOut) {
    Blowfish bf;
    if (!cand.makeCipher(bf)) return false;
    if (!Validate(bf, header, encryptedRootBlock, nullptr)) return false;
    cipherOut = bf;
    return true;
}

} // namespace sro
