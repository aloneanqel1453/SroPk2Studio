#pragma once
// Bridge to the vendored "SRO Asset Studio" core (src/bak, namespace sro).
// Header is std-only (pimpl) so app code can include it without pulling in
// <windows.h> or any BAK header.
//
// Process-wide state (BAK design): the asset-cipher state (keystream vault,
// vSRO gateway key, GoPro key, AES spec) and the sibling-container resolver are
// singletons, shared by every Archive opened in the process.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bakb {

struct Preview;

// Language of BAK's own texts (format summaries, preview notes, BAK error messages):
// "en", "tr", "zh" (Simplified), "ja", "vi"; anything else gives English. Process-wide;
// call it whenever the UI language changes. The bridge's own texts follow pk2::L().
void set_ui_language(std::wstring_view tag);

// Long write operations report (bytes or items done, total, current inner path)
// and stop when the callback returns false. May be empty.
using Progress = std::function<bool(std::uint64_t done, std::uint64_t total, const std::wstring& cur)>;

struct Entry {
    std::wstring path;          // inner path, '\\' separated, no leading separator
    std::uint64_t size = 0;     // payload bytes (0 for folders)
    bool folder = false;
    std::int64_t mtime = 0;     // modify time as stored in the entry (FILETIME ticks for PK2), 0 if none
};

class Archive {
public:
    // Opens read-only: a directory (extracted client), PK2v2 ("PK2v2" magic),
    // VSROPLUS .vp (key auto-derived from the client binaries next to it) or
    // JMXPACK .pk2/.pk3/.lib (Blowfish: plaintext table check, then `key` x
    // {STD, PARADISE, ZERO} base keys plus the raw-key reading). For PK2v2
    // `key` is tried as the container password (then PK2V2_PASSWORD).
    // `deep_key_search` additionally runs BAK's KeyFinder when no stage-1 key
    // fits: session/sibling keys, a scan of the binaries next to the archive
    // and of an already-running client (it never launches the game). Slow.
    // Also arms the vSRO / GoPro asset ciphers when the archive (or a sibling)
    // carries their markers.
    static std::unique_ptr<Archive> open(const std::wstring& path, const std::string& key,
                                         std::wstring& err, bool deep_key_search = false);

    // Same, and with `writable` opens the BAK backend for writing where it can:
    //   PK2/PK3/LIB (Pk2Archive, in place; compact keeps <path>.bak) - falls back
    //     to read-only when the file is locked (running client) or read-only;
    //   Folder (FolderArchive, plain file operations);
    //   VP (VpArchive) - only while BAK can reproduce the container signature
    //     and, for a payload-encrypted (L33T) container, once the payload key is
    //     known (see recover_vp_payload_key) - otherwise every byte written would
    //     be keyed wrong;
    //   PK2v2 stays read-only (BAK has no PK2v2 writer).
    // can_write() reports the outcome; protection_report() says why.
    static std::unique_ptr<Archive> open(const std::wstring& path, const std::string& key,
                                         std::wstring& err, bool deep_key_search, bool writable);

    std::wstring kind() const;                 // "PK2", "PK2v2", "VP", "Folder"
    bool can_write() const;                    // true only when the write ops below can succeed
    const std::wstring& path() const;

    // ---- writing (inner paths, '\\' or '/'; missing parent folders are created)
    // Payloads are stored exactly as given: BAK never encrypts an asset (its asset
    // ciphers are decrypt-only), so a file written into a shard that encrypts
    // assets is stored plain. A VP container's own payload layer IS applied
    // (VpArchive encrypts under the container key). After any mutation entries()
    // is rebuilt on its next call. All return false with `err` set on failure.
    bool add_file(const std::wstring& inner, const std::vector<std::uint8_t>& bytes,
                  std::wstring& err);                                   // overwrites
    bool remove(const std::wstring& inner, std::wstring& err);          // file or folder (recursive)
    bool add_folder(const std::wstring& inner, std::wstring& err);      // true if it already exists
    bool rename(const std::wstring& inner, const std::wstring& newName, std::wstring& err);  // leaf name
    // Contents of `diskDir` (files + subfolders, recursively) land under
    // `innerDest` ("" = root), overwriting. Progress counts bytes. A failed file
    // does not stop the import; err then names how many failed and the first error.
    bool import_folder(const std::wstring& diskDir, const std::wstring& innerDest,
                       const Progress& progress, std::wstring& err);
    // Rewrite without dead space (PK2: <path>.new -> <path>.bak + swap; VP the
    // same; Folder: no-op). Refused for a SailSafe PK2: BAK's rebuild writes a
    // JoyMax header, which the Sailor client rejects. `saved` may be null.
    bool compact(std::uint64_t* saved, const Progress& progress, std::wstring& err);
    bool flush(std::wstring& err);

    // ---- protection / keys
    // L33T VP payload key (file bytes): BAK's RecoverL33tPayloadKey on a running
    // sro_client.exe, then any *.dmp full-memory dump next to the archive. Needs
    // one of the two; read-only towards the process. True when payloads are
    // decryptable afterwards (or were never encrypted); `how` explains either way.
    bool recover_vp_payload_key(std::wstring& how);
    // Multi-line summary in the UI language: container kind/version, key scheme, write
    // status and reason, asset ciphers armed, VP signature / payload key, siblings.
    std::wstring protection_report() const;

    // Full flattened tree (folders included), built on first call and again on
    // the first call after a write. A JMXPACK archive is lazy, so the first call
    // walks every block chain. Do not hold the reference across a write.
    const std::vector<Entry>& entries() const;
    // Entries the backend had to skip (payload outside the file: truncated archive).
    std::uint64_t skipped_entries() const;

    // Payload, already asset-decrypted when a known asset cipher applies.
    // `siblings` = when the primary lacks the path, also try the other
    // containers in the same folder (Data/Media/Particles/Map, read-only).
    bool read(const std::wstring& path, std::vector<std::uint8_t>& out, std::wstring& err,
              bool siblings = false) const;

    // Per-asset cipher recovery for shards that encrypt individual assets:
    // known profiles, S-box, single-byte XOR, solve-from-archive, module dumps
    // next to the archive, whole-file AES key scan of the client binaries
    // (15 s budget) and, last, a running client of this game. Walks the archive
    // reading .bms/.bsr until it finds an encrypted one: run it off the UI
    // thread. Returns true when a cipher was recovered (or one was already
    // known this session); `how` gets a short note either way.
    bool recover_asset_cipher(std::wstring& how);

    // Every asset `path` references, recursively (.bsr -> .bmt/.bms/.bsk/.ban/
    // .ddj, .bmt -> .ddj, .efp -> .ddj/.bms, ...), root included, resolved via
    // this archive and its siblings. `missing` gets unresolved references.
    std::vector<std::wstring> dependencies(const std::wstring& path,
                                           std::vector<std::wstring>& missing) const;

    // Avatar costume: the extra .bsr pieces (hat / attach / wings / transform)
    // that complete the set the selected .bsr belongs to, from the archive's
    // itemdata + charactervisualchange. Empty when it is not a catalogued avatar.
    std::vector<std::wstring> avatar_set(const std::wstring& bsr_path, std::wstring& family) const;

    ~Archive();
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

private:
    Archive();
    struct Impl;
    std::unique_ptr<Impl> p_;
    friend class Effect;
    friend bool build_preview(const Archive*, const std::wstring&, const std::vector<std::uint8_t>&,
                              int, int, Preview&, std::wstring&, bool);
};

// Readable text description of an asset (FormatId + the BAK parsers: Efp,
// MapFiles, Nif2dt, ClientData, Bsr/Bms/Bmt/Bsk/Ban, Ddj, Ifo, TextFile).
// `name` may be a bare file name or the inner path (some client data files are
// recognised by folder). Empty when the format is unknown.
std::wstring describe(const std::wstring& name, const std::vector<std::uint8_t>& bytes);

// Decrypt one asset blob in place when it is protected by a known asset cipher
// (Sailor, HntSRO S-box, vSRO, GoPro, AES, recovered keystream). Returns false
// when it is not encrypted or no active cipher fits.
bool decrypt_asset(const std::wstring& name, std::vector<std::uint8_t>& bytes);

// ---------------------------------------------------------------------------
// BAK's preview engine (ui/PreviewPane.cpp decoder, src/bak/preview): what the
// SRO Asset Studio preview pane would show for one asset.
// ---------------------------------------------------------------------------
struct PreviewImage {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> bgra;       // straight alpha, rows top-down, stride width*4
};

// A 3D scene for an interactive viewer: BAK's MeshView geometry with its transforms applied
// (upright, in world space), per-part material flags and the decoded textures.
struct Mesh3D {
    struct Part {
        std::vector<float> pos, nrm, uv;        // 3, 3 and 2 floats per vertex
        std::vector<std::uint32_t> idx;         // triangle list
        int tex = -1;                           // index into textures, -1 = untextured
        float alpha_test = 0.0f;                // cut-out threshold, 0 = opaque
        bool two_sided = false, blend = false, additive = false;
        std::vector<float> col;                 // 4 floats per vertex (rgb tint, alpha), empty = white
        bool layer = false;                     // splat layer: blended by vertex alpha over the part below
    };
    std::vector<Part> parts;
    std::vector<PreviewImage> textures;
    float min[3]{}, max[3]{};                   // bounds of every part
    [[nodiscard]] bool empty() const noexcept { return parts.empty(); }
};

struct Preview {
    Mesh3D mesh;                          // build_preview(..., want_mesh): the 3D scene instead of `image`
    std::wstring format;                  // BAK format name ("Resource / model (BSR)", ...)
    std::wstring summary;                 // file info text + summary + MeshView::detailsTr()
    std::wstring text;                    // text / table body of a text-like file (may be large)
    PreviewImage image;                   // texture at native size, 3D render or .2dt layout
    std::vector<PreviewImage> frames;     // animation frames; build_preview leaves it empty
                                          // (.efp animation is rendered by BAK-2, not here)
    float frame_ms = 0;                   // frame time when `frames` is filled
    bool is_3d = false;                   // `image` is a 3D render (.bms/.bsr/.bsk/.ban/.nvm/.cpd/.o2)
    std::vector<PreviewImage> pages;      // .2dt with tabs: every distinct tab page (pages[0] == image)
    std::vector<std::wstring> page_labels;// tab caption of each page
    bool generic = false;                 // no decoder knows the format (BAK would fall back to hex)
    bool text_guess = false;              // BAK only guessed "plain text": trust `text` only when it reads well
};

// `assets` resolves texture / mesh references (siblings included) and the
// avatar catalog; may be null for self-contained files. width/height is the
// 3D render size (>= 16). An .efp yields its summary only. `want_mesh`: a 3D
// asset comes back as `mesh` (for a live viewer) instead of a rendered `image`.
bool build_preview(const Archive* assets, const std::wstring& inner_path,
                   const std::vector<std::uint8_t>& bytes, int width, int height,
                   Preview& out, std::wstring& err, bool want_mesh = false);

// ---------------------------------------------------------------------------
// NewInterface .2dt (Media\resinfo UI layout: u32 count + 976-byte widget
// records) as a table. Read with BAK's ParseNif2dt; edits are written back in
// place into the fixed records.
// ---------------------------------------------------------------------------
bool ui2dt_is(const std::vector<std::uint8_t>& bytes);
// BAK's summary plus an aligned, read-only listing of every widget (preview).
std::wstring ui2dt_listing(const std::vector<std::uint8_t>& bytes);
// Header line ("#" + column captions in the UI language) + one TAB-separated row
// per widget (CRLF): the editor form.
std::wstring ui2dt_table(const std::vector<std::uint8_t>& bytes);
// Applies an edited ui2dt_table() text (any UI language: lines starting with "#"
// are skipped). Rows keep their count and order (the # column); only fields whose
// text changed are rewritten, every other byte stays identical. `changed` gets the
// number of rewritten fields. False + `err` (line, column, reason) on bad input;
// `bytes` is then untouched.
bool ui2dt_apply(std::vector<std::uint8_t>& bytes, const std::wstring& table, std::size_t& changed,
                 std::wstring& err);

// ---------------------------------------------------------------------------
// Particle effect (.efp) for a 3D viewer. BAK's EffectScene emits, per visible
// particle, one draw batch in world space: a camera-facing plate (4 vertices,
// 6 indices), a swept ribbon, or an instanced .bms mesh. Left-handed, Y up.
// ---------------------------------------------------------------------------
struct FxVertex { float pos[3]; float uv[2]; };

struct FxBatch {
    std::vector<FxVertex> vertices;
    std::vector<std::uint32_t> indices;   // triangle list
    int texture = -1;                     // index into FxSceneInfo::textures, -1 = untextured
    float color[4]{1, 1, 1, 1};           // rgb tint (stage gain applied), a = opacity
    std::uint32_t src_blend = 5;          // authored D3DBLEND (5 SRCALPHA, 2 ONE, ...)
    std::uint32_t dst_blend = 2;          // src=dst=0: opaque, writes depth
    bool two_sided = true;
};

struct FxSceneInfo {
    std::vector<std::wstring> textures;   // inner paths (.ddj/.dds), readable via Archive::read(..., siblings=true)
    float duration = 0;                   // seconds; sample() does not wrap
    std::size_t emitters = 0;
    float radius = 0;                     // extent the effect actually covers
    float camera_distance = 0;            // distance that frames it at a 45 degree fov
};

struct FxView {                           // camera basis in world space
    float right[3]{1, 0, 0};
    float up[3]{0, 1, 0};
    float forward[3]{0, 0, 1};            // from the eye towards the target
};

class Effect {
public:
    // `efp_path` (inner path, may be empty) is the base folder for relative
    // references. With `assets`, mesh emitters load their .bms and texture
    // paths are resolved against the archive and its siblings; without it,
    // mesh emitters draw nothing (BAK's rule) and textures stay as authored.
    static std::unique_ptr<Effect> load(const std::vector<std::uint8_t>& efp, std::wstring& err,
                                        const Archive* assets = nullptr,
                                        const std::wstring& efp_path = {});
    const FxSceneInfo& info() const;
    void sample(float t_seconds, const FxView& view, std::vector<FxBatch>& out) const;

    ~Effect();
    Effect(const Effect&) = delete;
    Effect& operator=(const Effect&) = delete;

private:
    Effect();
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace bakb
