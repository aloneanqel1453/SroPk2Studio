#pragma once
// Bridge to the vendored BAK-2 skill presentation code: se::client::Catalog (characterdata /
// itemdata / skilldata ENC / textdata names, src/bak2/client) + se::sp::FxDb (skilleffect.txt,
// effectsound.txt, src/bak2/skillplay). Client text files only, read through the ReadFn once at
// load(); nothing is written. Playback: bak2::View::play_skill (bak2_view.hpp).
// std types only, pimpl. Single-threaded after load().
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bak2 {

// Same alias as bak2_view.hpp: reads one asset by inner path (lower case, '/' separated,
// relative to the archive root; the app resolves Media / Data / Particles siblings).
using ReadFn = std::function<bool(const std::wstring& innerPath, std::vector<std::uint8_t>& out)>;

struct SkillItem {
    int id = 0;
    std::wstring code;    // Basic_Code
    std::wstring name;    // textdata name (Turkish UI: TR > EN > KR, other UI languages: EN > TR > KR), else the code
    std::wstring group;   // player skills: the mastery (Sword, Bow, Cold ...); else the presentation key
};

class SkillDb {
public:
    // Reads server_dep/silkroad/textdata/{characterdata, itemdata, skilldataenc (else skilldata),
    // textdataname + the name tables, textuisystem, skilleffect, effectsound}.txt and the files
    // their index lists, plus type.txt / SV.T of Media. false + err when no skill could be read.
    static std::unique_ptr<SkillDb> load(ReadFn read, std::wstring& err);
    ~SkillDb();
    SkillDb(const SkillDb&) = delete;
    SkillDb& operator=(const SkillDb&) = delete;

    std::size_t size() const;                                   // skills in skilldata
    // BAK-2 Catalog::SearchSkills: id, code, KR / EN / TR names (case-insensitive substring);
    // exact id / code first, then code prefix, name, code substring.
    std::vector<SkillItem> search(const std::wstring& query, std::size_t max = 200) const;
    // Readable text: skill row, presentation key, every skilleffectset row in plain words
    // (se::sp::DescribeRow) and the skill timeline (se::sp::Timeline::Dump, built without models).
    // Names, groups and texts follow the UI language (pk2::L) when they are made.
    std::wstring describe(int id) const;
    std::wstring summary() const;                               // what was loaded (Catalog::Summary + files)
    std::vector<std::wstring> problems() const;                 // load remarks (missing tables ...)

    struct Impl;

private:
    SkillDb();
    friend class View;
    std::unique_ptr<Impl> p_;
};

}  // namespace bak2
