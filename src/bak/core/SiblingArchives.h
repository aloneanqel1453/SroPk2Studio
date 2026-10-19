// ============================================================================
//  SRO Asset Studio - core/SiblingArchives.h
//
//  A Silkroad client spreads ONE asset across several containers. Measured on
//  "Sro Kim Hoa 160":
//
//      res\srokimhoa\avatar\..._set6_attach.bsr   -> Data.pk3
//      prim\mesh\item\srokimhoa\..._p1.bms        -> (another container)
//      icon\srokimhoa\avatar\..._set6_attach.ddj  -> Media.pk3
//
//  Resolving a model's references against only the ONE archive the user opened
//  therefore makes perfectly good models look broken: the .bsr parses, every
//  mesh reference misses, and the viewer has nothing to draw.
//
//  This resolver answers the reads the primary archive could not. It opens the
//  other containers sitting next to it, lazily (a JMXPACK open is header + root
//  block, so it costs nothing until something is actually missing) and strictly
//  read-only.
//
//  Keys: a shard keys every .pk2/.pk3 the same way, so a sibling is tried FIRST
//  with the primary's already-verified cipher - no second key hunt, no client.
//  If that fails the sibling gets the ordinary stage-1 candidates, and .vp
//  siblings derive their own container key exactly as they do standalone.
//  Every key still passes the root-block oracle before use, and nothing is
//  written or remembered.
// ============================================================================
#pragma once

#include "../Common.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sro {

class Blowfish;

class SiblingArchives {
public:
    static SiblingArchives& Instance();

    /// Point the resolver at the folder holding `primaryPath`. `cipher` is the
    /// primary's verified JMXPACK cipher, or nullptr for a .vp primary.
    /// Re-configuring for a different archive drops everything already open.
    void configure(const std::wstring& primaryPath, const Blowfish* cipher);

    /// Read `innerPath` out of any sibling. Assets come back DECRYPTED, exactly
    /// as the primary archive would return them. False when nobody has it.
    bool read(const std::wstring& innerPath, std::vector<u8>& out);

    /// Enumerate files across sibling containers so a caller can DISCOVER files
    /// it can only read by exact path afterwards (e.g. the textdata shards in a
    /// neighbouring Media archive when the user opened Data). Containers whose
    /// file name contains `preferNameSubstr` (case-insensitive) take priority:
    /// when at least one matches, ONLY those are walked - textdata always ships
    /// in Media, so this avoids walking a 2 GB Data.pk2 for nothing. `visit`
    /// receives each inner path; return false to stop early.
    void enumerate(const std::function<bool(const std::wstring& innerPath)>& visit,
                   const std::wstring& preferNameSubstr = std::wstring());

    /// Which sibling supplied the last successful read (for the preview note).
    std::wstring lastSourceTr() const;

    /// Sibling file names opened so far.
    std::vector<std::wstring> openedTr() const;

    void closeAll();

    ~SiblingArchives();

private:
    SiblingArchives();
    SiblingArchives(const SiblingArchives&) = delete;
    SiblingArchives& operator=(const SiblingArchives&) = delete;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sro
