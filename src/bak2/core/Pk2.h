// SkillEffect Studio - core/Pk2.h
// Read-only JMXPACK (.pk2) reader: Blowfish (SRO variant, little-endian words) over the
// 2560-byte directory blocks. The archive is opened with GENERIC_READ only and never written.
#pragma once

#include "Text.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace se {

class Pk2Reader {
public:
    Pk2Reader();
    ~Pk2Reader();
    Pk2Reader(const Pk2Reader&) = delete;
    Pk2Reader& operator=(const Pk2Reader&) = delete;

    // asciiKey: "169841" for vSRO (default). Fails when the header checksum does not match the key.
    bool Open(const std::wstring& path, const std::string& asciiKey, std::wstring& err);
    // innerPath like L"server_dep\\silkroad\\textdata\\skilleffect.txt" (case-insensitive)
    bool Read(const std::wstring& innerPath, Bytes& out, std::wstring& err);
    bool Exists(const std::wstring& innerPath);
    // entries of a folder (name, isFolder); innerDir "" = root
    bool List(const std::wstring& innerDir, std::vector<std::pair<std::wstring, bool>>& out);
    const std::wstring& Path() const { return m_path; }

private:
    struct Entry {
        std::uint8_t type = 0;  // 1 folder, 2 file
        std::string name;       // lower-case
        std::int64_t off = 0;
        std::uint32_t size = 0;
    };
    struct Impl;
    bool Find(const std::wstring& innerPath, Entry& e, std::wstring& err);
    bool Block(std::int64_t off, std::vector<Entry>& out, std::wstring& err);
    bool ReadAt(std::int64_t off, void* dst, std::uint32_t n);

    std::unique_ptr<Impl> m_impl;
    std::wstring m_path;
    std::unordered_map<std::int64_t, std::vector<Entry>> m_dirs;
};

// key spec: "169841" (published STD base key), "169841@PARADISE", "169841@ZERO", "ascii@<hex base key>" or "raw:<hex>"
bool Pk2KeyBytes(const std::string& spec, std::vector<std::uint8_t>& key);
bool Pk2KeyOpens(const std::wstring& pk2, const std::string& spec);

struct KeyFindReport {
    bool found = false;
    std::string key;                 // spec usable with Pk2Reader::Open (never written to disk by the tool)
    std::wstring howTr, howEn;       // where it came from
    std::wstring whyTr, whyEn;       // plain reason when not found
    int tests = 0, filesScanned = 0;
};
// root-block oracle (entry 0 = folder ".", every entry type <= 2): typed key + known keys + 169841 x {STD, PARADISE,
// ZERO}, then a static scan of the exe/dll files in the archive's folder (strings near "Wrong BlowfishKey" ...).
// Read-only: no process is started or scanned.
KeyFindReport FindPk2Key(const std::wstring& pk2, const std::string& typed, const std::vector<std::string>& known,
                         const std::function<void(int)>& progress = {});

struct Pk2PutFile {
    std::wstring inner;  // e.g. server_dep\silkroad\textdata\skilldataenc.txt (a missing folder is created)
    Bytes data;
};

// "pk2 kopyasina paketle": copies src to dst (never in place; refuses dst == src), writes the files into the COPY
// (existing entry: data appended at the end and the entry repointed; new name: a free entry slot or a new chained
// directory block; a missing folder: a new directory block with "." / ".." + its entry in the parent), re-reads every
// file from the copy to verify, then publishes it (an existing dst becomes .bak, or is replaced when !backupExisting).
// Every inner path must be exact CP949 and no data may hold a noted lossy table row (v5 M3: "ENC2", nothing written).
// The log ends with the largest written offset and, past 2 GiB, the P3 size note (information only, v6 P1).
bool Pk2WriteCopy(const std::wstring& src, const std::wstring& dst, const std::string& asciiKey,
                  const std::vector<Pk2PutFile>& files, const std::function<void(int)>& progress, std::wstring& log,
                  std::wstring& err, bool backupExisting = true);

// ---- v5 M4: 2 GiB guard
constexpr std::int64_t kPk2OffsetLimit = 0x80000000LL;  // 2^31: P3 size NOTE only (v6: the client reads any size)
constexpr unsigned kPk2PreferInPlace = 1u;  // a changed file that fits its old slot is written there (in the COPY)
constexpr unsigned kPk2InPlaceOver2G = 2u;  // ... only when appending it would end past kPk2OffsetLimit

struct Pk2CopyOptions {
    bool backupExisting = true;
    unsigned flags = 0;                // kPk2PreferInPlace | kPk2InPlaceOver2G (a slot shared with another entry: append)
    std::int64_t testAppendFloor = 0;  // TEST HOOK only: appended data / blocks start at >= this offset (sparse copy)
};

struct Pk2CopyReport {
    std::int64_t srcSize = 0, dstSize = 0;  // archive size before / after
    std::int64_t maxEnd = 0;                // largest offset + size written (data or directory block); 0 = nothing
    std::int64_t maxDataOff = 0;            // largest data offset written
    std::wstring maxInner;                  // the file at maxDataOff
    int inPlace = 0, appended = 0, newFiles = 0, newBlocks = 0;
    bool p3 = false;                        // maxEnd > kPk2OffsetLimit (size note; never blocks)
    std::vector<std::wstring> over2G;       // files whose data ends past 2 GiB
    std::wstring p3Tr, p3En;                // P3 size note, information only (empty when !p3); also in the log
};

bool Pk2WriteCopy(const std::wstring& src, const std::wstring& dst, const std::string& asciiKey,
                  const std::vector<Pk2PutFile>& files, const std::function<void(int)>& progress, std::wstring& log,
                  std::wstring& err, const Pk2CopyOptions& opt, Pk2CopyReport* rep = nullptr);
// Dry run of Pk2WriteCopy on the source (read-only, nothing copied): the same placement, so the P3 note can be shown BEFORE
// writing (Prova / check). rep.dstSize = the size the copy would have.
bool Pk2PlanCopy(const std::wstring& src, const std::string& asciiKey, const std::vector<Pk2PutFile>& files,
                 const Pk2CopyOptions& opt, Pk2CopyReport& rep, std::wstring& log, std::wstring& err);

}  // namespace se
