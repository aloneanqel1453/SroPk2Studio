// SkillEffect Studio v3 - client/Vfs.h
// Any-client loader: one layered, read-only virtual file system over a Silkroad client.
//
//   what the user may open                         how it is read
//   a client folder (Media.pk2 Data.pk2 ...)       pk2 names in any case; a missing pk2 is tolerated (problem list)
//   any .pk2 of a client                           its folder is the client
//   loose folders (<root>\Media|Data|Particles)    used when the pk2 is missing (LooseMode::Fallback, default),
//                                                  in front of the pk2 (Front) or never (Ignore)
//   an extracted Media folder (has server_dep)     Media layer only
//   a textdata folder / one textdata .txt          textdata only (inner server_dep\silkroad\textdata\<name>)
//   a parent folder holding exactly one client     that client (Info problem says so)
//
// BEFORE / AFTER: overlay folders (a SkillEffect Studio output folder: <dir>\Media\server_dep\..., <dir>\Data\...,
// <dir>\Particles\..., or pk2 COPIES <dir>\Media.pk2 ...) and in-memory staged files are read IN FRONT of the client
// in View::After only. View::Before is always the untouched client. Nothing in this module can write a file.
//
// Every failure is explained in plain Turkish (Problem::tr) with an English twin (Problem::en).
// Thread-safe: reads may run on worker threads while the UI thread stages files / adds overlays.
#pragma once

#include "../core/Text.h"
#include "KeyFinder.h"
#include "Pk2Archive.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace se::client {

enum class ArcKind : int { Media = 0, Data = 1, Particles = 2, Map = 3, Music = 4 };
constexpr int kArcKinds = 5;
const wchar_t* ArcFileName(ArcKind a);    // Media.pk2 ...
const wchar_t* ArcFolderName(ArcKind a);  // Media ...

enum class View { Before, After };
enum class LooseMode { Fallback, Front, Ignore };
enum class Source { None, Staged, OverlayFolder, OverlayPk2, Pk2, LooseFolder, TextdataFolder };
const wchar_t* SourceNameTr(Source s);
const wchar_t* SourceNameEn(Source s);

enum class OpenedAs { None, ClientFolder, Pk2File, LooseRoot, MediaFolder, TextdataFolder };

struct Problem {
    enum Level { Info = 0, Warn = 1, Error = 2 } level = Info;
    std::wstring code;  // stable id: NO_PATH, NO_CLIENT, KEY_FAIL, KEY_FOUND, NO_MEDIA, NO_DATA, NO_PARTICLES, ...
    std::wstring tr, en;
};

struct Hit {
    ArcKind arc = ArcKind::Media;
    Source src = Source::None;
    std::wstring where;  // archive / folder path ("" for staged)
    bool after = false;  // came from an overlay / staged file
};

struct VfsItem {
    std::wstring name;
    bool folder = false;
    std::uint64_t size = 0;
    Source src = Source::None;
    bool after = false;
};

struct ArcState {
    bool present = false;       // readable from some layer
    Source src = Source::None;  // primary source (Pk2 / LooseFolder ...)
    std::wstring path;          // pk2 path or folder
    std::wstring keyHowTr;      // how the key was found (never the key)
    bool keyFound = false;
    std::wstring keyHowEn;
};

enum class Change { Missing, Same, Added, Modified };

struct VfsOptions {
    std::string key;                 // typed pk2 key ("" = 169841 + discovery)
    bool autoKey = true;             // run FindKey when the key does not open an archive
    bool scanBinaries = true;        // allow the exe/dll static scan stage of FindKey
    LooseMode loose = LooseMode::Fallback;
    std::vector<std::wstring> overlays;  // AFTER layers, front first
    bool openMap = true, openMusic = false;
    std::function<void(int percent, const std::wstring& what)> progress;
    const std::atomic<bool>* cancel = nullptr;
};

class Vfs {
public:
    Vfs();
    ~Vfs();
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;

    // false when nothing readable was found; Problems() holds the reasons either way
    bool Open(const std::wstring& path, const VfsOptions& opt = {});
    void Close();
    bool IsOpen() const;

    // ---- AFTER layers
    bool AddOverlay(const std::wstring& dir, std::wstring* whyTr = nullptr);  // front-most
    void ClearOverlays();
    std::vector<std::wstring> Overlays() const;
    void Stage(ArcKind a, std::wstring_view inner, Bytes data);  // in-memory pending file (front of everything)
    bool Unstage(ArcKind a, std::wstring_view inner);
    void ClearStaged();
    size_t StagedCount() const;
    void SetView(View v) { m_view = v; }
    View GetView() const { return m_view; }

    // ---- reading (view: the current one when omitted)
    bool Read(ArcKind a, std::wstring_view inner, Bytes& out, Hit* hit = nullptr) const;
    bool ReadIn(View v, ArcKind a, std::wstring_view inner, Bytes& out, Hit* hit = nullptr) const;
    bool ReadText(ArcKind a, std::wstring_view inner, std::wstring& out, Hit* hit = nullptr) const;
    bool Exists(ArcKind a, std::wstring_view inner, Hit* hit = nullptr) const;
    bool ExistsIn(View v, ArcKind a, std::wstring_view inner, Hit* hit = nullptr) const;
    bool Size(ArcKind a, std::wstring_view inner, std::uint64_t& size) const;
    // Data -> Particles -> Media -> Map -> Music; a leading "data\" / "particles\" ... picks the archive
    bool ReadAny(std::wstring_view inner, Bytes& out, Hit* hit = nullptr) const;
    // merged folder listing (front layer wins on duplicate names); false when no layer has the folder
    bool List(ArcKind a, std::wstring_view dir, std::vector<VfsItem>& out) const;
    // glob over inner paths ("*.efp", "res\\char\\*.bsr"; no '\' = file name only), case-insensitive.
    // Builds the archive index on first use (progress / cancel optional). Results are unique and sorted.
    size_t Find(ArcKind a, std::wstring_view glob, std::vector<std::wstring>& out, size_t maxResults = 100000,
                const std::function<void(int)>& progress = {}, const std::atomic<bool>* cancel = nullptr) const;

    // ---- BEFORE / AFTER
    Change Compare(ArcKind a, std::wstring_view inner) const;
    struct Changed {
        ArcKind arc;
        std::wstring inner;
        Change change;
        Source src;
    };
    // staged files + files of overlay folders (overlay pk2 copies are compared only for staged / folder paths)
    std::vector<Changed> ChangedFiles() const;

    // ---- textdata (server_dep\silkroad\textdata\ of Media)
    static const wchar_t* TextdataDir();  // L"server_dep\\silkroad\\textdata"
    bool ReadTextdata(std::wstring_view name, Bytes& out, Hit* hit = nullptr) const;
    bool HasTextdata(std::wstring_view name) const;
    // the files the client reads for a table base, e.g. "characterdata" -> CharacterData_5000.txt ... :
    //   <base>.txt is an index (every line one *.txt name) -> the listed files that exist (missing -> problems)
    //   <base>.txt holds data -> just <base>.txt
    //   no <base>.txt -> <base>_<N>.txt files (fallback, Warn)
    std::vector<std::wstring> TextdataParts(std::wstring_view base, std::vector<Problem>* problems = nullptr) const;
    std::vector<std::wstring> TextdataFiles() const;  // every file name in the textdata folder

    // ---- copying OUT of the client (for transfers / exports; the client itself is never written)
    // An output folder is safe when it is not inside any open client folder and neither it nor an ancestor holds
    // *.pk2 / sro_client.exe / silkroad.exe (a game folder). whyTr says why not.
    static bool IsSafeOutputDir(const std::wstring& dir, const std::vector<std::wstring>& clientRoots, std::wstring* whyTr);
    // <outRoot>\<Media|Data|Particles...>\<inner>, atomic (temp + move), folders created; refuses unsafe folders
    bool ExtractTo(ArcKind a, std::wstring_view inner, const std::wstring& outRoot, std::wstring* whyTr = nullptr) const;

    // ---- info
    const std::vector<Problem>& Problems() const { return m_problems; }
    int ErrorCount() const;
    ArcState Arc(ArcKind a) const;
    OpenedAs Kind() const { return m_kind; }
    const std::wstring& Root() const { return m_root; }   // client folder ("" for textdata-only)
    const std::wstring& Opened() const { return m_opened; }
    std::string KeySpec() const { return m_keySpec; }     // memory only; never write it anywhere
    std::wstring Describe(bool en) const;                 // one line, for the status bar / log
    std::wstring Fingerprint() const;                     // changes when any archive / folder changes (caches)
    std::shared_ptr<const Pk2Archive> Archive(ArcKind a) const;  // primary pk2 (Before view) or null

    // inner path normalisation: '/' -> '\', no leading '\' or ".\", no doubled '\'
    static std::wstring Norm(std::wstring_view inner);

private:
    struct Layer;
    bool OpenClientFolder(const std::wstring& root, const VfsOptions& opt);
    void AddProblem(Problem::Level lv, const wchar_t* code, std::wstring tr, std::wstring en);
    bool LayerRead(const Layer& L, const std::wstring& inner, Bytes* out, std::uint64_t* size) const;
    bool LayerList(const Layer& L, const std::wstring& dir, std::vector<VfsItem>& out) const;

    mutable std::shared_mutex m_mx;
    std::vector<std::shared_ptr<Layer>> m_layers;  // front first (after-layers first)
    std::map<std::pair<int, std::wstring>, Bytes> m_staged;  // (arc, lower inner)
    std::vector<std::wstring> m_overlays;
    std::vector<Problem> m_problems;
    OpenedAs m_kind = OpenedAs::None;
    std::wstring m_root, m_opened;
    std::string m_keySpec;
    ArcState m_arc[kArcKinds];
    std::atomic<View> m_view{View::Before};
};

// actual file / folder name inside dir matching name case-insensitively ("" when none)
std::wstring FindChildI(const std::wstring& dir, const std::wstring& name, bool wantDir);
// case-insensitive glob with * and ?
bool GlobMatchI(std::wstring_view pattern, std::wstring_view text);

}  // namespace se::client
