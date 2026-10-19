// SkillEffect Studio v6 - render/EfxThumb.h  (M18 effect library)
// Small moving pictures of .efp effects for the library page:
//   * RenderEfxThumb: the RunEffectSheet pipeline (EfpEffect at 20 Hz -> DrawEffect -> SoftRaster) without labels or
//     reference figure, the camera fitted to the effect's particle bounds over the frame times; 96 x 96, 4 frames
//     (0.3 / 0.6 / 0.9 / 1.2 s) side by side in one strip. A file that does not parse gets a red "broken" strip.
//   * EfxThumbs: background workers (cancel / restart safe) with a disk cache
//     %LOCALAPPDATA%\SkillEffectStudio\thumbs\<client finger>\  (key = inner path + size + FNV-64 of the bytes),
//     512 MB cap (oldest files go first), atomic writes (temp + MoveFileEx). Decoded strips are not kept in memory:
//     the UI reads the few visible ones (Strip) and keeps its own small cache.
//   * tags from name tokens (fire / flame -> ateş ...), a reverse index efp -> presentation keys (FxDb) and
//     key -> skills -> owner objects (catalog), both header-only templates so this module stays client-free.
// Nothing here writes into a client; the source is read-only (IAssetSource).
#pragma once

#include "RAssets.h"
#include "RImage.h"

#include <atomic>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace se::render {

constexpr int kThumbSize = 96;
constexpr int kThumbFrames = 4;

// one strip (kThumbFrames * size wide, size high). false + err when the file is missing / does not parse.
bool RenderEfxThumb(AssetCache& cache, const std::wstring& path, Image& strip, std::string& err, int size = kThumbSize,
                    const std::vector<double>& times = {0.3, 0.6, 0.9, 1.2});
void BrokenThumb(Image& strip, int size = kThumbSize);   // red strip with a white cross (the GUI writes the word)

std::uint64_t Fnv64(const void* data, std::size_t n, std::uint64_t h = 1469598103934665603ull);
std::wstring Hex64(std::uint64_t v);
std::wstring ThumbCacheRoot();                                  // %LOCALAPPDATA%\SkillEffectStudio\thumbs ("" = none)
std::wstring ClientFinger(const std::wstring& particlesPath);   // 16 hex digits of path + size + write time

// ------------------------------------------------------------------ tags
struct EfxTag {
    const wchar_t* id;    // "fire"
    const wchar_t* tr;    // "ateş"
    const wchar_t* en;    // "fire"
};
const std::vector<EfxTag>& EfxTagList();
std::vector<int> EfxTagsOf(const std::wstring& path);   // indices into EfxTagList(), from the file name tokens

// ------------------------------------------------------------------ background thumbnails + disk cache
enum class ThumbState : int { Pending = 0, Ready = 1, Broken = 2, Missing = 3 };

class EfxThumbs {
public:
    // called once on a worker thread: the source every worker reads (thread-safe reads, e.g. its own client Vfs)
    using OpenFn = std::function<std::shared_ptr<IAssetSource>()>;
    struct Options {
        std::wstring cacheDir;                   // "" = memory only (then strips stay in RAM)
        std::uint64_t capBytes = 512ull << 20;   // disk cap of cacheDir
        int threads = 0;                         // 0 = min(4, cores - 1)
        bool render = true;                      // false = cache lookups only (tests)
    };
    struct Progress {
        int total = 0, done = 0, cached = 0, rendered = 0, broken = 0;
        double seconds = 0;
        bool running = false;
    };

    EfxThumbs() = default;
    ~EfxThumbs();
    EfxThumbs(const EfxThumbs&) = delete;
    EfxThumbs& operator=(const EfxThumbs&) = delete;

    void Start(OpenFn open, std::vector<std::wstring> paths, const Options& o);   // stops a running job first
    void Stop();                                  // cancel + join (safe to call twice)
    bool Busy() const { return m_running.load(); }
    void Prioritize(const std::vector<std::wstring>& paths);   // render these next (visible cells)
    ThumbState State(const std::wstring& path) const;
    bool Strip(const std::wstring& path, Image& out) const;    // the decoded strip (disk cache or memory)
    std::string Error(const std::wstring& path) const;         // why a strip is broken ("" = fine)
    Progress Prog() const;
    std::uint64_t Generation() const { return m_gen.load(); }  // grows when an item finished (UI repaints)
    const std::wstring& CacheDir() const { return m_opt.cacheDir; }

private:
    struct Item {
        ThumbState state = ThumbState::Pending;
        std::wstring file;          // cache file ("" = none)
        Image mem;                  // memory-only strips
        std::string err;
        bool fromCache = false;
    };
    void Worker(std::shared_ptr<IAssetSource>* src);
    bool Next(std::size_t& idx);
    void Finish(std::size_t idx, Item&& it);
    void EnforceCap();

    Options m_opt;
    std::vector<std::wstring> m_paths;
    std::unordered_map<std::wstring, std::size_t> m_index;   // lower path -> index
    std::vector<Item> m_items;
    std::vector<char> m_taken;
    std::vector<std::size_t> m_prio;
    std::size_t m_next = 0;
    mutable std::mutex m_mx;
    std::thread m_boss;
    std::atomic<bool> m_cancel{false}, m_running{false};
    std::atomic<std::uint64_t> m_gen{0};
    std::atomic<int> m_done{0}, m_cached{0}, m_rendered{0}, m_broken{0};
    std::atomic<std::uint64_t> m_t0{0}, m_t1{0};
};

// ------------------------------------------------------------------ reverse index: efp -> presentation keys
class EfxUseIndex {
public:
    void Clear() { m_keys.clear(); }
    void Add(const std::wstring& efpPath, const std::wstring& key);
    const std::vector<std::wstring>* KeysOf(const std::wstring& efpPath) const;   // nullptr = nobody
    std::size_t Size() const { return m_keys.size(); }
    // every .efp of every effective skilleffectset row of an se::sp::FxDb (ObjName and ObjName2)
    template <class Db>
    void AddFxDb(const Db& db) {
        for (const auto& k : db.Keys())
            for (const auto* r : db.Effects(k))
                if (r)
                    for (const std::wstring& p : {r->ResPath(), r->Res2Path()})
                        if (p.size() > 4 && _wcsicmp(p.c_str() + p.size() - 4, L".efp") == 0) Add(p, k);
    }

private:
    std::unordered_map<std::wstring, std::vector<std::wstring>> m_keys;   // lower path -> keys
};

// presentation key -> skills -> the object that shows it (monster family MSKILL_<stem>_ -> MOB_<stem>, DefaultSkill
// users, player skills -> objId 0 + race); header-only over se::client::Catalog
struct EfxUser {
    int skillId = 0;
    int objId = 0;     // 0 = a player skill (open a player body of `race`)
    int race = -1;     // player skills: 0 CH, 1 EU
    int mastery = 0;
    std::wstring key;
};

template <class Cat>
class EfxKeyIndex {
public:
    void Build(const Cat& c) {
        m_byKey.clear();
        for (const auto& s : c.Skills()) {
            if (!s.service) continue;
            std::wstring k = s.presKey.empty() ? s.code : s.presKey;
            for (auto& ch : k) ch = static_cast<wchar_t>(towupper(ch));
            auto& v = m_byKey[k];
            bool replaced = false;   // one skill per (key, family): the highest level
            for (auto& id : v)
                if (const auto* o = c.SkillById(id))
                    if (Family(o->code) == Family(s.code)) {
                        if (s.level > o->level) id = s.id;
                        replaced = true;
                        break;
                    }
            if (!replaced) v.push_back(s.id);
        }
    }
    std::vector<EfxUser> Users(const Cat& c, const std::vector<std::wstring>& keys, std::size_t max = 60) const {
        std::vector<EfxUser> out;
        std::set<long long> seen;
        for (const auto& key0 : keys) {
            std::wstring key = key0;
            for (auto& ch : key) ch = static_cast<wchar_t>(towupper(ch));
            auto it = m_byKey.find(key);
            if (it == m_byKey.end()) continue;
            for (int sid : it->second) {
                const auto* s = c.SkillById(sid);
                if (!s) continue;
                EfxUser u;
                u.skillId = sid;
                u.key = key0;
                u.mastery = s->mastery;
                std::vector<int> owners = c.UsersOf(sid);
                if (owners.empty() && s->original)
                    for (int o : c.UsersOf(s->original)) owners.push_back(o);
                if (owners.empty())
                    if (int o = MonsterOf(c, s->code)) owners.push_back(o);
                if (owners.empty()) {
                    const std::wstring u2 = Upper(s->code);
                    if (u2.rfind(L"SKILL_CH_", 0) == 0 || u2.rfind(L"SKILL_EU_", 0) == 0) {
                        u.race = u2.rfind(L"SKILL_EU_", 0) == 0 ? 1 : 0;
                        if (seen.insert(static_cast<long long>(sid)).second) out.push_back(u);
                    }
                } else {
                    for (std::size_t i = 0; i < owners.size() && i < 3; ++i) {
                        u.objId = owners[i];
                        if (seen.insert((static_cast<long long>(u.objId) << 32) | static_cast<unsigned>(sid)).second) out.push_back(u);
                    }
                }
                if (out.size() >= max) return out;
            }
        }
        return out;
    }

private:
    static std::wstring Upper(std::wstring s) {
        for (auto& ch : s) ch = static_cast<wchar_t>(towupper(ch));
        return s;
    }
    // the code without its trailing level / step digits (SKILL_CH_BOW_CALL_A_12 -> SKILL_CH_BOW_CALL_A_)
    static std::wstring Family(const std::wstring& code) {
        std::size_t n = code.size();
        while (n && iswdigit(code[n - 1])) --n;
        return Upper(code.substr(0, n));
    }
    // MSKILL_CH_TIGERWOMAN_ATTACK03 -> MOB_CH_TIGERWOMAN (the longest MOB_ code that exists)
    static int MonsterOf(const Cat& c, const std::wstring& code) {
        const std::wstring u = Upper(code);
        if (u.rfind(L"MSKILL_", 0) != 0) return 0;
        std::wstring rest = u.substr(7);
        while (!rest.empty()) {
            if (const auto* o = c.ObjectByCode(L"MOB_" + rest)) return o->id;
            const std::size_t cut = rest.find_last_of(L'_');
            if (cut == std::wstring::npos) break;
            rest.resize(cut);
        }
        return 0;
    }
    std::map<std::wstring, std::vector<int>> m_byKey;
};

}  // namespace se::render
