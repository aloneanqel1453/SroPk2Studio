// SkillEffect Studio - core/Client.cpp
#include "Client.h"

#include "Source.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace se {

const wchar_t* ArcName(Arc a) {
    switch (a) {
        case Arc::Media: return L"Media.pk2";
        case Arc::Data: return L"Data.pk2";
        default: return L"Particles.pk2";
    }
}

std::wstring FindNameI(const std::wstring& dir, const std::wstring& name, bool wantDir) {
    if (dir.empty()) return {};
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring hit;
    do {
        const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir == wantDir && _wcsicmp(fd.cFileName, name.c_str()) == 0) {
            hit = JoinPath(dir, fd.cFileName);
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return hit;
}

namespace {

const wchar_t* kArcFile[kArcCount] = {L"Media.pk2", L"Data.pk2", L"Particles.pk2"};
const wchar_t* kArcDir[kArcCount] = {L"Media", L"Data", L"Particles"};

std::wstring FindTextdata(const std::wstring& p) {
    const wchar_t* cands[] = {L"server_dep\\silkroad\\textdata", L"Media\\server_dep\\silkroad\\textdata",
                              L"0Media\\server_dep\\silkroad\\textdata", L"textdata", L"silkroad\\textdata", L""};
    for (const auto* c : cands) {
        const std::wstring d = *c ? JoinPath(p, c) : p;
        if (FileExists(JoinPath(d, L"skilleffect.txt")) || FileExists(JoinPath(d, L"characterdata.txt"))) return d;
    }
    return {};
}

bool HasAnyArchive(const std::wstring& d) {
    for (int i = 0; i < kArcCount; ++i)
        if (!FindNameI(d, kArcFile[i], false).empty()) return true;
    return false;
}

std::wstring FindRootUp(std::wstring d) {
    for (int i = 0; i < 6 && !d.empty(); ++i) {
        if (HasAnyArchive(d)) return d;
        const std::wstring up = DirOf(d);
        if (up == d) break;
        d = up;
    }
    return {};
}

bool IsKeyError(const std::wstring& e) { return e.find(L"anahtar") != std::wstring::npos || e.find(L"key") != std::wstring::npos; }

}  // namespace

bool ClientBase::Open(const std::wstring& p, const std::string& pk2Key, std::wstring& err) {
    const std::wstring keepOverlay = overlay;
    *this = ClientBase{};
    overlay = keepOverlay;
    path = p;
    key = pk2Key.empty() ? std::string(kDefaultPk2Key) : pk2Key;
    if (EndsWithI(p, L".pk2") && FileExists(p)) {
        auto r = std::make_shared<Pk2Reader>();
        if (!r->Open(p, key, err)) {
            if (!IsKeyError(err)) return false;
            const KeyFindReport rep = FindPk2Key(p, key, {});
            if (!rep.found) {
                err = rep.whyTr + L"\n" + rep.whyEn;
                return false;
            }
            key = rep.key;
            keyNote = L"Pk2 anahtarı bulundu: " + rep.howTr + L" / key found: " + rep.howEn;
            err.clear();
            if (!r->Open(p, key, err)) return false;
        }
        if (!r->Exists(std::wstring(kTextdataInner) + L"skilleffect.txt") &&
            !r->Exists(std::wstring(kTextdataInner) + L"characterdata.txt")) {
            err = L"Bu pk2'de server_dep\\silkroad\\textdata yok (Media.pk2 mi?) / no textdata in: " + p;
            return false;
        }
        kind = Kind::Pk2;
        mediaPk2 = p;
        root = DirOf(p);
        m_pk[0] = r;
        m_tried[0] = true;
    } else if (DirExists(p)) {
        const std::wstring media = FindNameI(p, L"Media.pk2", false);
        if (!media.empty()) {
            const std::string keepKey = key;
            if (!Open(media, keepKey, err)) return false;
            path = p;
            return true;
        }
        textdataDir = FindTextdata(p);
        if (textdataDir.empty()) {  // loose "media" folder in any case
            const std::wstring md = FindNameI(p, L"Media", true);
            if (!md.empty()) textdataDir = FindTextdata(md);
        }
        if (textdataDir.empty() && !HasAnyArchive(p) && FindNameI(p, L"Data", true).empty() && FindNameI(p, L"Particles", true).empty()) {
            err = L"Klasörde Media.pk2 / Data.pk2 / Particles.pk2 ya da server_dep\\silkroad\\textdata yok / not a client folder: " + p;
            return false;
        }
        kind = Kind::Folder;
        root = textdataDir.empty() ? p : FindRootUp(textdataDir);
        if (root.empty() && (HasAnyArchive(p) || !FindNameI(p, L"Data", true).empty())) root = p;
    } else if (FileExists(p)) {
        kind = Kind::File;
        singleFile = p;
        textdataDir = DirOf(p);
        root = FindRootUp(textdataDir);
    } else {
        err = L"Bulunamadı / not found: " + p;
        return false;
    }
    if (!root.empty()) {
        for (int i = 0; i < kArcCount; ++i) {
            if (i == 0 && !mediaPk2.empty()) {
                arcPath[0] = mediaPk2;
                continue;
            }
            const std::wstring f = FindNameI(root, kArcFile[i], false);
            if (!f.empty()) arcPath[i] = f;
            else {
                const std::wstring d = FindNameI(root, kArcDir[i], true);
                if (!d.empty() && i != 0) m_dir[i] = d;
                else if (i != 0) missing.push_back(kArcFile[i]);
            }
        }
    }
    return true;
}

Pk2Reader* ClientBase::Reader(Arc a) const {
    const int i = static_cast<int>(a);
    if (!m_tried[i]) {
        m_tried[i] = true;
        if (!arcPath[i].empty()) {
            auto r = std::make_shared<Pk2Reader>();
            std::wstring e;
            if (r->Open(arcPath[i], key, e)) m_pk[i] = r;
            else if (IsKeyError(e)) {  // a sibling archive with another key (rare): search quietly
                const KeyFindReport rep = FindPk2Key(arcPath[i], key, {});
                if (rep.found && r->Open(arcPath[i], rep.key, e)) m_pk[i] = r;
            }
        }
    }
    return m_pk[i].get();
}

bool ClientBase::HasArchive(Arc a) const {
    return Reader(a) != nullptr || !m_dir[static_cast<int>(a)].empty();
}

std::wstring ClientBase::OverlayFile(Arc a, const std::wstring& inner) const {
    if (overlay.empty()) return {};
    std::wstring in = inner;
    for (auto& c : in)
        if (c == L'/') c = L'\\';
    const std::wstring f = JoinPath(JoinPath(overlay, kArcDir[static_cast<int>(a)]), in);
    return FileExists(f) ? f : std::wstring();
}

bool ClientBase::FromOverlay(Arc a, const std::wstring& inner) const { return !OverlayFile(a, inner).empty(); }

bool ClientBase::ReadTextdata(const std::wstring& name, Bytes& out) const {
    std::wstring e;
    const std::wstring ov = OverlayFile(Arc::Media, std::wstring(kTextdataInner) + name);
    if (!ov.empty()) return ReadFileBytes(ov, out, e);
    if (kind == Kind::Pk2) {
        Pk2Reader* r = Reader(Arc::Media);
        return r && r->Read(std::wstring(kTextdataInner) + name, out, e);
    }
    if (textdataDir.empty()) return false;
    const std::wstring f = JoinPath(textdataDir, name);
    return FileExists(f) && ReadFileBytes(f, out, e);
}

bool ClientBase::HasTextdata(const std::wstring& name) const {
    if (!OverlayFile(Arc::Media, std::wstring(kTextdataInner) + name).empty()) return true;
    if (kind == Kind::Pk2) {
        Pk2Reader* r = Reader(Arc::Media);
        return r && r->Exists(std::wstring(kTextdataInner) + name);
    }
    return !textdataDir.empty() && FileExists(JoinPath(textdataDir, name));
}

int ClientBase::Exists(Arc a, const std::wstring& inner) const {
    if (!OverlayFile(a, inner).empty()) return 1;
    if (Pk2Reader* r = Reader(a)) return r->Exists(inner) ? 1 : 0;
    const std::wstring& d = m_dir[static_cast<int>(a)];
    if (!d.empty()) return FileExists(JoinPath(d, inner)) ? 1 : 0;
    return -1;
}

bool ClientBase::ReadResource(Arc a, const std::wstring& inner, Bytes& out) const {
    std::wstring e;
    const std::wstring ov = OverlayFile(a, inner);
    if (!ov.empty()) return ReadFileBytes(ov, out, e);
    if (Pk2Reader* r = Reader(a)) return r->Read(inner, out, e);
    const std::wstring& d = m_dir[static_cast<int>(a)];
    return !d.empty() && FileExists(JoinPath(d, inner)) && ReadFileBytes(JoinPath(d, inner), out, e);
}

bool ClientBase::ReadAny(const std::wstring& inner, Bytes& out, Arc* where) const {
    for (Arc a : {Arc::Data, Arc::Particles, Arc::Media})
        if (ReadResource(a, inner, out)) {
            if (where) *where = a;
            return true;
        }
    return false;
}

bool ClientBase::List(Arc a, const std::wstring& innerDir, std::vector<std::pair<std::wstring, bool>>& out) const {
    out.clear();
    if (Pk2Reader* r = Reader(a)) return r->List(innerDir, out);
    const std::wstring& d = m_dir[static_cast<int>(a)];
    if (d.empty()) return false;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(JoinPath(d, innerDir), L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
            out.push_back({fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

std::wstring ClientBase::Describe(bool en) const {
    std::wstring s;
    switch (kind) {
        case Kind::Pk2: s = mediaPk2; break;
        case Kind::Folder: s = textdataDir.empty() ? root : textdataDir; break;
        case Kind::File: s = singleFile; break;
        default: return en ? L"not opened" : L"açılmadı";
    }
    std::wstring arcs;
    for (int i = 1; i < kArcCount; ++i)
        if (!arcPath[i].empty() || !m_dir[i].empty()) arcs += std::wstring(arcs.empty() ? L"" : L", ") + ArcName(static_cast<Arc>(i));
    if (!arcs.empty()) s += L"  (+ " + arcs + L")";
    return s;
}

}  // namespace se
