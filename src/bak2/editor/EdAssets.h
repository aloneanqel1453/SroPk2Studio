// SkillEffect Studio - editor/EdAssets.h
// Asset browsers (read-only on the client): .efp list with users (skilleffect rows) + a live preview hook the render
// module plugs in, .ddj / icon decoding (JMXVDDJ = DDS: DXT1/3/5 + uncompressed masks) + PNG export (WIC), .wav /
// .ogg probe + playback (wav: PlaySound; ogg: Media Foundation decode when the system has a Vorbis decoder),
// .bsr summary (materials, meshes, .ban list, animation roles + events, ModDataSound track counts vs
// MAX_SOUND_TRACK_PER_ANI = 15) and .ban headers. Self-contained parsers (SPEC.md 2.2 / 2.6 / 2.7).
#pragma once

#include "EdDoc.h"

#include "../core/Client.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace se::ed {

// ---------------------------------------------------------------- images
struct EdImg {
    int w = 0, h = 0;
    std::vector<std::uint8_t> bgra;  // w*h*4, top-down
    bool Valid() const { return w > 0 && h > 0 && bgra.size() == static_cast<size_t>(w) * h * 4; }
};
// .ddj (20-byte JMXVDDJ header) or plain .dds; fmt = "DXT5 256x256" ...
bool DecodeDdjImage(const Bytes& b, EdImg& out, std::wstring* fmt = nullptr);
bool SavePng(const std::wstring& path, const EdImg& img, std::wstring& err);  // atomic (temp + move)
// thumbnails side by side (cell px, cols) for a contact sheet
EdImg ContactSheet(const std::vector<EdImg>& imgs, int cell, int cols);

// ---------------------------------------------------------------- asset lists
struct AssetEntry {
    int arc = 2;                      // 0 Media, 1 Data, 2 Particles, 3 Music
    std::wstring path;                // inner path
    std::vector<std::wstring> users;  // skilleffect keys / creatures using it (capped)
    int uses = 0;
};
// every file with extension ext ("efp", "ddj", "wav", "ogg", "bsr", "ban"; empty = all) under dir (recursive)
std::vector<AssetEntry> ListAssets(const ClientBase& c, Arc a, const std::wstring& dir, const std::wstring& ext, size_t cap,
                                   const std::atomic<bool>* cancel = nullptr);
// lower-case Particles path -> skilleffect keys that use it (SA efp columns, SE ObjPath+ObjName, CI BloodType)
std::unordered_map<std::wstring, std::vector<std::wstring>> EfpUsers(const EdDoc& skilleffect, size_t capPerFile = 24);
void AttachUsers(std::vector<AssetEntry>& list, const std::unordered_map<std::wstring, std::vector<std::wstring>>& users);

// live mini preview of an .efp: the render module provides it (Particles inner path, time in ms, size) -> image
using EfpPreviewFn = std::function<bool(const std::wstring& efpInner, double timeMs, int w, int h, EdImg& out)>;
void SetEfpPreview(EfpPreviewFn f);
const EfpPreviewFn& GetEfpPreview();
// still picture of a model (.bsr Data inner path, size) -> image; the render module provides it (optional)
using ModelPreviewFn = std::function<bool(const std::wstring& bsrInner, int w, int h, EdImg& out)>;
void SetModelPreview(ModelPreviewFn f);
const ModelPreviewFn& GetModelPreview();
// "JMXVEFF 0011" etc. (empty when not an .efp)
std::wstring EfpSignature(const Bytes& b);
// texture / mesh / animation names stored in an .efp (string scan; for the static fallback preview)
std::vector<std::wstring> EfpResourceRefs(const Bytes& b, const wchar_t* ext = L".ddj");
// the first texture of an .efp that the client has, decoded (Particles.pk2: as stored or under textures\)
bool EfpFirstTexture(const ClientBase& c, const Bytes& efp, EdImg& out, std::wstring* path = nullptr);

// ---------------------------------------------------------------- audio
struct AudioInfo {
    bool ok = false;
    std::wstring codec;  // PCM / MS-ADPCM / IMA-ADPCM / Vorbis ...
    int channels = 0, rate = 0, bits = 0;
    double seconds = 0;
    std::wstring err;
};
AudioInfo ProbeAudio(const Bytes& b);
// decode to 16-bit PCM (Media Foundation; .ogg needs a Vorbis decoder on the system)
bool DecodeAudio(const Bytes& b, bool ogg, std::vector<std::int16_t>& pcm, int& channels, int& rate, std::wstring& err);
Bytes MakeWav(const std::vector<std::int16_t>& pcm, int channels, int rate);

class AudioPlayer {
public:
    ~AudioPlayer();
    bool Play(const Bytes& file, bool ogg, std::wstring& err);  // async
    void Stop();
private:
    Bytes m_buf;
};
// Music.pk2 of the client (BGM .ogg); false when missing
bool ReadMusic(const ClientBase& c, const std::wstring& inner, Bytes& out);
std::vector<std::wstring> ListMusic(const ClientBase& c, size_t cap = 5000);

// ---------------------------------------------------------------- models
struct BanInfo {
    std::wstring path;
    bool found = false, ok = false;
    std::wstring name;
    std::uint32_t durationMs = 0, fps = 0, type = 0, keys = 0, bones = 0;
};
struct BsrRole {
    std::uint32_t role = 0;
    std::wstring roleName;  // ANI_* token
    std::wstring group;     // ani group ("default" ...)
    std::uint32_t file = 0; // index into anis
    std::vector<std::array<std::uint32_t, 4>> events;  // keyTime, type (1 = hit), index, unk
};
struct SndSet {
    int palette = 0;        // 0 system set, 1 animation set
    int type = 0, aniType = 0;
    std::wstring setName;   // palette set name ("default", "ambient", status_* ...)
    std::wstring sndName;   // SndSet name
    int tracks = 0, used = 0;
};
struct BsrInfo {
    bool ok = false;
    std::wstring path, name, err;
    std::uint16_t objType = 0;
    std::vector<std::wstring> materials, meshes, anis;
    std::wstring skeleton, attachBone;
    std::vector<BsrRole> roles;
    std::vector<SndSet> sounds;
    std::vector<std::wstring> particles;  // ModData particle .efp paths (Particles.pk2)
    int maxTracks = 0;
};
bool ParseBsrInfo(const Bytes& b, BsrInfo& out);
bool ParseBanInfo(const Bytes& b, BanInfo& out);
// the .bsr (Data.pk2 inner path) + every .ban it lists
BsrInfo ModelAnimations(const ClientBase& c, const std::wstring& bsrInner, std::vector<BanInfo>& bans);
constexpr int kMaxSoundTrackPerAni = 15;

}  // namespace se::ed
