// SkillEffect Studio - core/Client.h
// The user's client, read-only: Media.pk2 (textdata) + Data.pk2 / Particles.pk2 (resources) from the client
// folder (archive names in any case, loose media / data / particles folders when an archive is missing), or an
// extracted Media / textdata folder, or a single skilleffect.txt. An optional OVERLAY folder (a SkillEffect Studio
// output folder: <dir>\Media\server_dep\..., <dir>\Data\..., <dir>\Particles\...) is read IN FRONT of the client
// ("after" preview). Nothing here can write.
#pragma once

#include "Pk2.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace se {

enum class Arc : int { Media = 0, Data = 1, Particles = 2 };
constexpr int kArcCount = 3;
const wchar_t* ArcName(Arc a);  // Media.pk2 / Data.pk2 / Particles.pk2

using Progress = std::function<void(int percent, const std::wstring& what)>;

class ClientBase {
public:
    enum class Kind { None, Pk2, Folder, File } kind = Kind::None;

    // path: Media.pk2 (any case), a client folder, a Media/textdata folder or a skilleffect.txt.
    // A key that does not open the archive is searched (FindPk2Key); keyNote says how it was found.
    bool Open(const std::wstring& path, const std::string& pk2Key, std::wstring& err);
    // resources only (Data / Particles) are enough: browsing models / effects of a client without textdata
    bool IsOpen() const { return kind != Kind::None; }
    bool ReadTextdata(const std::wstring& name, Bytes& out) const;  // name relative to ...\textdata\ (case-insensitive)
    bool HasTextdata(const std::wstring& name) const;
    // 1 = present, 0 = missing, -1 = the archive is not available (cannot tell)
    int Exists(Arc a, const std::wstring& inner) const;
    bool ReadResource(Arc a, const std::wstring& inner, Bytes& out) const;
    // Data, then Particles, then Media (where = the archive it came from)
    bool ReadAny(const std::wstring& inner, Bytes& out, Arc* where = nullptr) const;
    bool HasArchive(Arc a) const;
    std::wstring Describe(bool en) const;
    // entries of a folder inside an archive (pk2 or loose folder)
    bool List(Arc a, const std::wstring& innerDir, std::vector<std::pair<std::wstring, bool>>& out) const;

    // "after" view: a SkillEffect Studio output folder read in front of the client (empty = none)
    void SetOverlay(const std::wstring& dir) { overlay = dir; }
    bool FromOverlay(Arc a, const std::wstring& inner) const;  // the overlay holds this file

    std::wstring path;          // what the user opened
    std::wstring root;          // client folder (where the .pk2 files are), may be empty
    std::wstring mediaPk2;      // textdata from this archive (Kind::Pk2)
    std::wstring textdataDir;   // textdata from this folder (Kind::Folder / File)
    std::wstring singleFile;    // only a skilleffect.txt was opened (Kind::File)
    std::wstring arcPath[kArcCount];
    std::wstring overlay;
    std::string key = "169841";
    std::wstring keyNote;       // "anahtar bulundu: ..." when the typed key did not open the archive
    std::vector<std::wstring> missing;  // archives the client folder does not have (Data.pk2 ...)

private:
    mutable std::shared_ptr<Pk2Reader> m_pk[kArcCount];
    mutable bool m_tried[kArcCount]{};
    std::wstring m_dir[kArcCount];  // loose archive folders (e.g. <root>\Data)
    Pk2Reader* Reader(Arc a) const;
    std::wstring OverlayFile(Arc a, const std::wstring& inner) const;
};

// the actual file / folder name in dir matching name case-insensitively (empty when none)
std::wstring FindNameI(const std::wstring& dir, const std::wstring& name, bool wantDir);

}  // namespace se
