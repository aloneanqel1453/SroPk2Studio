// ============================================================================
//  SRO Asset Studio - formats/AvatarSet.cpp
//
//  See AvatarSet.h. Ports the SroEffectViewer avatar catalog (Avatar.h) and its
//  equipAvatarSet()/famOf() family grouping into the studio, reading textdata
//  straight out of the open archive instead of a folder on disk.
// ============================================================================
#include "AvatarSet.h"

#include "TextFile.h"
#include "Bsk.h"
#include "../core/IArchive.h"
#include "../core/SiblingArchives.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>

namespace sro {

// ---------------------------------------------------------------------------
// Small local string helpers (wide, case-insensitive where noted).
// ---------------------------------------------------------------------------
namespace {

bool StartsWithW(const std::wstring& s, const wchar_t* prefix) {
    size_t i = 0;
    for (; prefix[i]; ++i)
        if (i >= s.size() || s[i] != prefix[i]) return false;
    return true;
}
bool EndsWithW(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    if (n > s.size()) return false;
    return s.compare(s.size() - n, n, suffix) == 0;
}
bool ContainsW(const std::wstring& s, const wchar_t* needle) {
    return s.find(needle) != std::wstring::npos;
}

int ParseIntW(const std::wstring& s) {
    int v = 0; bool any = false;
    for (wchar_t c : s) {
        if (c >= L'0' && c <= L'9') { v = v * 10 + (c - L'0'); any = true; }
        else if (any) break;
    }
    return v;
}

// Split one line on TABs. Interior cells are kept verbatim; a trailing CR is
// stripped by the caller before this runs.
std::vector<std::wstring> SplitTabs(const std::wstring& line) {
    std::vector<std::wstring> cols;
    size_t p = 0;
    for (;;) {
        const size_t tb = line.find(L'\t', p);
        cols.push_back(line.substr(p, (tb == std::wstring::npos ? line.size() : tb) - p));
        if (tb == std::wstring::npos) break;
        p = tb + 1;
    }
    return cols;
}

// Iterate the \n-normalised text line by line, CR trimmed.
template <class F>
void ForEachLine(const std::wstring& text, F&& fn) {
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find(L'\n', start);
        std::wstring line = text.substr(start, (nl == std::wstring::npos ? text.size() : nl) - start);
        start = (nl == std::wstring::npos) ? text.size() + 1 : nl + 1;
        while (!line.empty() && (line.back() == L'\r')) line.pop_back();
        if (line.empty()) continue;
        fn(line);
    }
}

// Family stem, ported verbatim from SroEffectViewer main.cpp equipAvatarSet's
// famOf: drop a trailing _A/_B variant letter and a kind suffix, but NEVER
// "_DRESS" (which is part of a family stem: ..._DRESS / ..._DRESS_HAT / ...).
std::wstring FamOf(std::wstring c) {
    size_t u = c.rfind(L'_');
    if (u != std::wstring::npos && c.size() - u == 2) c = c.substr(0, u);   // _A variant
    u = c.rfind(L'_');
    if (u != std::wstring::npos) {
        const std::wstring last = c.substr(u + 1);
        static const wchar_t* sfx[] = { L"HAT", L"SUIT", L"ATTACH", L"ATTCH",
                                        L"WING", L"CAP", L"MASK", L"SHOES" };
        for (const wchar_t* s : sfx) if (last == s) { c = c.substr(0, u); break; }
    }
    return c;
}

// ---------------------------------------------------------------------------
// Filename-based family grouping. Costume pieces sit in the SAME folder and
// share a common NAME STEM plus a "kind" suffix token, e.g.
//     avatar_m_2012_new_arabia            (dress / base)
//     avatar_m_2012_new_arabia_hat        (hat)
//     avatar_m_2012_new_arabia_attach     (attach)
// or, with the base itself carrying a token:
//     avatar_w_angel_wing                 (wing / attach)
//     avatar_w_angel_wing_dress           (dress)
//     avatar_w_angel_wing_dress_hat       (hat)
// The itemdata reference names do NOT reliably match these physical files on
// private servers, so the set is discovered geometrically: reduce every name to
// its family base by peeling recognised kind tokens off the end, then a sibling
// belongs to the set when its base matches and the leftover tokens are ALL kind
// tokens (which keeps unrelated recolors / variants out).
// ---------------------------------------------------------------------------
bool IsKindToken(const std::wstring& t) {
    static const wchar_t* k[] = { L"dress", L"hat", L"attach", L"attch", L"wing",
                                  L"cap", L"mask", L"shoes", L"suit", L"robe", L"cape" };
    for (const wchar_t* x : k) if (t == x) return true;
    return false;
}

// Split a stem on '_' into tokens.
std::vector<std::wstring> Tokens(const std::wstring& s) {
    std::vector<std::wstring> t;
    size_t p = 0;
    while (p <= s.size()) {
        const size_t u = s.find(L'_', p);
        t.push_back(s.substr(p, (u == std::wstring::npos ? s.size() : u) - p));
        if (u == std::wstring::npos) break;
        p = u + 1;
    }
    return t;
}

// Peel trailing kind tokens off a lowercase stem, but never shrink below 3
// tokens (so "avatar_w_x" stays a family, never collapses to "avatar_w").
std::wstring FamilyBaseOf(const std::wstring& stemLower) {
    std::vector<std::wstring> t = Tokens(stemLower);
    while (t.size() > 3 && IsKindToken(t.back())) t.pop_back();
    std::wstring base;
    for (size_t i = 0; i < t.size(); ++i) { if (i) base += L'_'; base += t[i]; }
    return base;
}

// Does `stemLower` == base, or base + only-kind-token suffixes?
bool StemInFamily(const std::wstring& stemLower, const std::wstring& base) {
    if (stemLower == base) return true;
    if (stemLower.size() <= base.size() + 1) return false;
    if (stemLower.compare(0, base.size(), base) != 0 || stemLower[base.size()] != L'_') return false;
    for (const std::wstring& tok : Tokens(stemLower.substr(base.size() + 1)))
        if (!IsKindToken(tok)) return false;
    return true;
}

// Kind of a family member from its suffix tokens: hat wins, then attach/wing,
// else dress/base.
int KindOfStem(const std::wstring& stemLower, const std::wstring& base) {
    std::wstring rem;
    if (stemLower.size() > base.size() && stemLower.compare(0, base.size(), base) == 0)
        rem = stemLower.substr(base.size());
    if (rem.find(L"hat") != std::wstring::npos || rem.find(L"cap") != std::wstring::npos) return 1;
    if (rem.find(L"attach") != std::wstring::npos || rem.find(L"attch") != std::wstring::npos ||
        rem.find(L"wing")   != std::wstring::npos || rem.find(L"cape")  != std::wstring::npos) return 3;
    return 2;
}

// -----------------------------------------------------------------------
// itemdata / avataritemdata row -> AvatarSetItem. Mirrors Avatar.h
// loadAvatarsFromFile: any item whose model lives under item\Avatar\ is a
// wearable avatar; kind from TypeID4 (col12) with the col9==3/col11==13 gate,
// falling back to code words.
// -----------------------------------------------------------------------
void ParseItemDataDoc(const std::wstring& text, std::vector<AvatarSetItem>& out) {
    ForEachLine(text, [&](const std::wstring& line) {
        if (line.size() < 8) return;
        std::vector<std::wstring> c = SplitTabs(line);
        if (c.size() < 4 || c[0] != L"1") return;             // service flag on

        // The CODE cell sits in col 2 in the full itemdata_*.txt layout but in
        // col 1 in the slim resinfo\avataritemdata.txt layout - find it wherever
        // it is instead of assuming a fixed column (that assumption silently
        // dropped every avataritemdata avatar).
        int codeIdx = -1;
        for (int i = 1; i < static_cast<int>(c.size()); ++i)
            if (StartsWithW(c[i], L"ITEM_")) { codeIdx = i; break; }
        if (codeIdx < 0) return;

        // the wearable model = a .bsr cell whose path mentions "avatar" (folder
        // OR file name), covering item\Avatar\x.bsr and res\item\avatar\x.bsr.
        std::wstring bsr;
        for (const std::wstring& t : c) {
            const std::wstring l = ToLowerW(t);
            if (EndsWithW(l, L".bsr") && l.find(L"avatar") != std::wstring::npos) {
                bsr = NormalizeInnerPath(t); break;
            }
        }
        if (bsr.empty()) return;

        AvatarSetItem a;
        a.code    = c[codeIdx];
        a.bsrRel  = bsr;
        a.bsrBase = ToLowerW(PathFileName(bsr));

        // Full itemdata carries TypeID4 (col12) behind the col9==3 / col11==13
        // gate: 1 hat, 2 dress/suit, 3 attach, 4 = ALSO attach. The slim layout
        // has no such column, so fall through to the code-word heuristic.
        int k = 0;
        if (static_cast<int>(c.size()) > 12 && c[9] == L"3" && c[11] == L"13")
            k = ParseIntW(c[12]);
        a.kind = (k == 4) ? 3 : k;
        const std::wstring cl = ToLowerW(a.code);
        if (a.kind < 1 || a.kind > 3) {                       // code-word fallback (ordered)
            if      (ContainsW(cl, L"hat") || ContainsW(cl, L"cap") || ContainsW(cl, L"crown")) a.kind = 1;
            else if (ContainsW(cl, L"dress") || ContainsW(cl, L"suit") || ContainsW(cl, L"robe")) a.kind = 2;
            else if (ContainsW(cl, L"wing") || ContainsW(cl, L"attach") || ContainsW(cl, L"attch") ||
                     ContainsW(cl, L"ensign") || ContainsW(cl, L"flag")) a.kind = 3;
            else a.kind = 2;
        }

        if      (cl.find(L"_m_") != std::wstring::npos) a.gender = L'm';
        else if (cl.find(L"_w_") != std::wstring::npos) a.gender = L'w';
        else if (a.bsrBase.find(L"_m_") != std::wstring::npos) a.gender = L'm';
        else if (a.bsrBase.find(L"_w_") != std::wstring::npos) a.gender = L'w';

        out.push_back(std::move(a));
    });
}

// -----------------------------------------------------------------------
// charactervisualchange row -> attach/replace on the matching item. Mirrors
// Avatar.h applyVisualChange: cond 4 = ADDITIONAL attached piece (wings),
// cond 7 = REPLACES the body model (transform). Everything else (remove mask,
// fx groups) is irrelevant to a static bind-pose preview and dropped.
// -----------------------------------------------------------------------
void ApplyVisualChangeDoc(const std::wstring& text,
                          std::unordered_map<std::wstring, AvatarSetItem*>& byCode) {
    ForEachLine(text, [&](const std::wstring& line) {
        if (line.size() < 8 || StartsWithW(line, L"//")) return;
        std::vector<std::wstring> c = SplitTabs(line);
        if (c.size() < 21 || c[0] != L"1") return;
        if (c[3] != L"4" && c[3] != L"7") return;             // exact-item-code rows only
        auto it = byCode.find(c[9]);
        if (it == byCode.end()) return;
        AvatarSetItem& a = *it->second;

        std::wstring bsr = (a.gender == L'w') ? c[20] : c[19];
        if (bsr.empty() || bsr == L"xxx") bsr = (a.gender == L'w') ? c[19] : c[20];
        if (bsr.empty() || bsr == L"xxx") return;

        bsr = NormalizeInnerPath(bsr);
        if (StartsWithW(ToLowerW(bsr), L"res\\")) bsr = bsr.substr(4);   // resolveRes prepends the res dir
        if (c[3] == L"7") a.replaceBsr = bsr;
        else              a.attachBsr  = bsr;
    });
}

// core name for variant inheritance: strip the "ITEM_<source>_" prefix down to
// "AVATAR_...". EVENT / GACHA copies with no visualchange row of their own then
// inherit the MALL variant's transform/wings.
std::wstring CoreOf(const std::wstring& code) {
    const size_t p = code.find(L"_AVATAR_");
    return (p == std::wstring::npos) ? code : code.substr(p + 1);
}

void ParseAllTextData(const std::vector<std::vector<u8>>& itemBufs,
                      const std::vector<std::vector<u8>>& cvcBufs,
                      AvatarCatalog& cat) {
    // 1) items
    for (const std::vector<u8>& buf : itemBufs) {
        if (buf.empty()) continue;
        TextDocument doc;
        if (!LoadTextDocument(buf.data(), buf.size(), doc).ok()) continue;
        ParseItemDataDoc(doc.text, cat.items);
    }
    // de-duplicate by code (an item can appear in itemdata AND avataritemdata)
    std::sort(cat.items.begin(), cat.items.end(),
              [](const AvatarSetItem& a, const AvatarSetItem& b) { return a.code < b.code; });
    cat.items.erase(std::unique(cat.items.begin(), cat.items.end(),
                    [](const AvatarSetItem& a, const AvatarSetItem& b) { return a.code == b.code; }),
                    cat.items.end());

    // 2) visualchange (attach/replace)
    std::unordered_map<std::wstring, AvatarSetItem*> byCode;
    byCode.reserve(cat.items.size() * 2);
    for (AvatarSetItem& a : cat.items) byCode[a.code] = &a;
    for (const std::vector<u8>& buf : cvcBufs) {
        if (buf.empty()) continue;
        TextDocument doc;
        if (!LoadTextDocument(buf.data(), buf.size(), doc).ok()) continue;
        ApplyVisualChangeDoc(doc.text, byCode);
    }

    // 3) variant inheritance by core name
    std::map<std::wstring, const AvatarSetItem*> byCore;
    for (const AvatarSetItem& a : cat.items)
        if (!a.replaceBsr.empty() || !a.attachBsr.empty()) byCore.emplace(CoreOf(a.code), &a);
    for (AvatarSetItem& a : cat.items) {
        if (!a.replaceBsr.empty() || !a.attachBsr.empty()) continue;
        auto it = byCore.find(CoreOf(a.code));
        if (it == byCore.end() || it->second == &a) continue;
        const AvatarSetItem* s = it->second;
        a.replaceBsr = s->replaceBsr;
        if (a.attachBsr.empty()) a.attachBsr = s->attachBsr;
    }
}

} // namespace

// ---------------------------------------------------------------------------
const AvatarSetItem* AvatarCatalog::findByBsrBase(const std::wstring& bsrBaseLower) const {
    for (const AvatarSetItem& a : items)
        if (a.bsrBase == bsrBaseLower) return &a;
    return nullptr;
}

// ---------------------------------------------------------------------------
const AvatarCatalog& GetAvatarCatalog(IArchive* archive, const AssetLoader& loader) {
    static std::mutex        mtx;
    static AvatarCatalog     cache;
    static AvatarCatalog     kEmpty;
    static IArchive*         kArch  = nullptr;
    static u64               kSize  = 0;
    static std::wstring      kPath;
    static bool              kBuilt = false;

    std::lock_guard<std::mutex> lk(mtx);
    if (!archive || !loader) return kEmpty;

    const u64          sz  = archive->fileSize();
    const std::wstring pth = archive->path();
    if (kBuilt && archive == kArch && sz == kSize && pth == kPath) return cache;

    cache  = AvatarCatalog();
    kArch  = archive; kSize = sz; kPath = pth; kBuilt = true;

    // 1) Enumerate textdata under the archive lock (walkAll holds it for the
    //    whole traversal, so nodes cannot be freed mid-walk; we keep only the
    //    path strings). Cap the counts so a pathological archive cannot stall.
    constexpr size_t kMaxFiles = 512;
    std::vector<std::wstring> itemPaths, cvcPaths;
    auto collect = [&](const std::wstring& fullPath) -> bool {
        const std::wstring nameL = ToLowerW(PathFileName(fullPath));
        if (!EndsWithW(nameL, L".txt"))   return true;
        if (EndsWithW(nameL, L"enc.txt")) return true;       // encrypted shard - skip
        if (StartsWithW(nameL, L"itemdata") || StartsWithW(nameL, L"avataritemdata")) {
            if (itemPaths.size() < kMaxFiles) itemPaths.push_back(fullPath);
        } else if (StartsWithW(nameL, L"charactervisualchange")) {
            if (cvcPaths.size() < kMaxFiles) cvcPaths.push_back(fullPath);
        }
        return true;
    };
    archive->walkAll([&](Pk2Node* n) -> bool {
        if (!n || !n->isFile()) return true;
        collect(n->fullPath);
        // Physical avatar .bsr list from the archive the user actually opened
        // (the geometry archive). This is the robust basis for "wear the set":
        // the family pieces sit in the SAME folder as the selected one.
        const std::wstring lp = ToLowerW(n->fullPath);
        if (EndsWithW(lp, L".bsr") && lp.find(L"avatar") != std::wstring::npos &&
            cache.avatarBsrPaths.size() < 40000)
            cache.avatarBsrPaths.push_back(n->fullPath);
        return true;
    }, ProgressFn{});

    // The .bsr models live in Data.pk2 but itemdata/charactervisualchange live in
    // the neighbouring Media.pk2. When the user opened the geometry archive, the
    // primary walk finds no textdata - reach into the sibling containers (Media
    // first) so selecting an avatar still dresses the whole set.
    if (itemPaths.empty()) {
        SiblingArchives::Instance().enumerate(
            [&](const std::wstring& full) -> bool { return collect(full); }, L"media");
    }

    if (itemPaths.empty()) return cache;   // no catalog reachable (e.g. Map.pk2 alone)

    // 2) Read the bytes via the loader (honours sibling archives + decryption).
    auto readAll = [&](const std::vector<std::wstring>& paths) {
        std::vector<std::vector<u8>> bufs;
        bufs.reserve(paths.size());
        for (const std::wstring& p : paths) {
            std::vector<u8> b;
            if (loader(p, b) && !b.empty()) bufs.push_back(std::move(b));
        }
        return bufs;
    };
    const std::vector<std::vector<u8>> itemBufs = readAll(itemPaths);
    const std::vector<std::vector<u8>> cvcBufs  = readAll(cvcPaths);

    ParseAllTextData(itemBufs, cvcBufs, cache);
    return cache;
}

// ---------------------------------------------------------------------------
AvatarSetPlan PlanAvatarSet(const AvatarCatalog& cat, const std::wstring& selectedBsrInnerPath) {
    AvatarSetPlan plan;

    const std::wstring norm      = NormalizeInnerPath(selectedBsrInnerPath);
    const std::wstring selBase   = ToLowerW(PathFileName(norm));       // avatar_x.bsr
    if (selBase.empty()) return plan;
    std::wstring selStem = selBase;
    if (EndsWithW(selStem, L".bsr")) selStem.resize(selStem.size() - 4);
    const std::wstring selFolder = ToLowerW(PathParent(norm));         // res\item\avatar
    const std::wstring famBase   = FamilyBaseOf(selStem);
    plan.family = famBase;

    auto addUnique = [&](const std::wstring& rel) {
        if (rel.empty()) return;
        const std::wstring b = ToLowerW(PathFileName(rel));
        if (b == selBase) return;                                     // never repeat the selection
        for (const std::wstring& e : plan.pieces)
            if (ToLowerW(PathFileName(e)) == b) return;               // de-dup within the plan
        plan.pieces.push_back(rel);
    };

    // ---- 1) physical folder siblings sharing the family base ----------------
    // one piece per kind, preferring the shortest name (the canonical piece).
    std::map<int, std::wstring> byKind;
    for (const std::wstring& p : cat.avatarBsrPaths) {
        const std::wstring norm2 = NormalizeInnerPath(p);
        if (ToLowerW(PathParent(norm2)) != selFolder) continue;       // same folder only
        std::wstring base = ToLowerW(PathFileName(norm2));
        if (base == selBase) continue;                                // skip the selection
        std::wstring stem = base;
        if (EndsWithW(stem, L".bsr")) stem.resize(stem.size() - 4);
        if (!StemInFamily(stem, famBase)) continue;                   // different family / variant
        const int kind = KindOfStem(stem, famBase);
        auto it = byKind.find(kind);
        if (it == byKind.end() ||
            PathFileName(norm2).size() < PathFileName(it->second).size())
            byKind[kind] = norm2;
    }
    for (const auto& kv : byKind) addUnique(kv.second);

    // ---- 2) catalog (itemdata + charactervisualchange) enrichment -----------
    // Adds pieces the file names cannot reveal: a transform body, or wings that
    // live in a DIFFERENT folder and are linked only by the item's CVC row.
    if (const AvatarSetItem* q = cat.findByBsrBase(selBase)) {
        if (q->kind == 2 && !q->replaceBsr.empty()) addUnique(q->replaceBsr);
        addUnique(q->attachBsr);
        const std::wstring fam = FamOf(q->code);
        for (const AvatarSetItem& av : cat.items) {
            if (&av == q) continue;
            if (av.gender != L'a' && q->gender != L'a' && av.gender != q->gender) continue;
            if (FamOf(av.code) != fam) continue;
            addUnique((av.kind == 2 && !av.replaceBsr.empty()) ? av.replaceBsr : av.bsrRel);
            addUnique(av.attachBsr);
        }
    }

    plan.totalPieces = static_cast<int>(plan.pieces.size()) + 1;      // + the selected piece
    return plan;
}

// ---------------------------------------------------------------------------
bool LoadAvatarCharSkeleton(const std::wstring& avatarInnerPath,
                            const AssetLoader& loader, BskFile& out) {
    if (!loader) return false;
    const std::wstring lp = ToLowerW(avatarInnerPath);
    const bool female = (lp.find(L"_w_") != std::wstring::npos ||
                         lp.find(L"woman")   != std::wstring::npos ||
                         lp.find(L"_female") != std::wstring::npos);

    // Cache the two standard skeletons for the session (Bip01 hierarchy is the
    // same across bodies, so this is safe and avoids re-parsing an 80 KB body).
    static std::mutex mtx;
    static BskFile cachedF, cachedM;
    static bool haveF = false, haveM = false;
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (female && haveF) { out = cachedF; return true; }
        if (!female && haveM) { out = cachedM; return true; }
    }

    // Reference bodies whose skeletons carry a full Bip01 rig. The first list the
    // user named explicitly; the rest are common fallbacks across clients.
    const wchar_t* femaleRefs[] = {
        L"res\\char\\china\\chinawoman_necromencerb.bsr",
        L"res\\char\\europe\\europewoman_angel.bsr",
        L"res\\char\\europe\\europewoman_adventurer.bsr",
        L"res\\char\\china\\chinawoman_adventurer.bsr" };
    const wchar_t* maleRefs[] = {
        L"res\\char\\china\\chinaman_monkey.bsr",
        L"res\\char\\europe\\europeman_angel.bsr",
        L"res\\char\\europe\\europeman_adventurer.bsr",
        L"res\\char\\china\\chinaman_adventurer.bsr" };

    auto tryRef = [&](const wchar_t* ref) -> bool {
        std::vector<u8> bb;
        if (!loader(ref, bb) || bb.empty()) return false;
        BsrFile bsr;
        ParseBsr(bb.data(), bb.size(), bsr);
        if (!bsr.hasSkeleton || bsr.skeletonPath.empty()) return false;
        std::vector<u8> sk;
        if (!loader(NormalizeInnerPath(bsr.skeletonPath), sk) || sk.empty()) return false;
        BskFile bsk;
        if (!ParseBsk(sk.data(), sk.size(), bsk).ok() || bsk.bones.empty()) return false;
        bool hasSpine = false;
        for (const BskBone& b : bsk.bones)
            if (ToLowerW(b.name).find(L"spine") != std::wstring::npos) { hasSpine = true; break; }
        if (!hasSpine) return false;
        out = std::move(bsk);
        return true;
    };

    bool ok = false;
    for (const wchar_t* r : (female ? femaleRefs : maleRefs)) if (tryRef(r)) { ok = true; break; }
    if (ok) {
        std::lock_guard<std::mutex> lk(mtx);
        if (female) { cachedF = out; haveF = true; } else { cachedM = out; haveM = true; }
    }
    return ok;
}

} // namespace sro
