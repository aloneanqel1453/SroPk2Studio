// ============================================================================
//  SRO Asset Studio - core/Pk2v2Archive.cpp   (see Pk2v2Archive.h)
// ============================================================================
#include "Pk2v2Archive.h"
#include "Pk2v2Crypto.h"
#include "Pk2v2KeyTable.h"
#include "AssetCrypt.h"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace sro {

namespace {
std::wstring lower(const std::wstring& s){ return ToLowerW(s); }

std::wstring decodeName(const std::string& ansi){
    if (ansi.empty()) return std::wstring();
    bool ascii=true; for (unsigned char c:ansi) if (c>=0x80){ascii=false;break;}
    if (ascii) return std::wstring(ansi.begin(), ansi.end());
    std::wstring w = MultiByteToWide(ansi, CP_ACP);
    if (w.empty() || w.find((wchar_t)0xFFFD)!=std::wstring::npos){
        std::wstring k = MultiByteToWide(ansi, 949);
        if (!k.empty() && k.find((wchar_t)0xFFFD)==std::wstring::npos) return k;
    }
    if (w.empty()) w = MultiByteToWide(ansi, 1252);
    return w;
}

static bool ScanBufferForPk2v2Key(const u8* buf, size_t bufLen, const u8 vtag[12], u8 keyOut[32]) {
    if (bufLen < 12 + 0x38 + 32) return false;
    for (size_t pos = 0; pos + 12 + 0x38 + 32 <= bufLen; ++pos) {
        if (std::memcmp(buf + pos, vtag, 12) == 0) {
            const u8* cand = buf + pos + 0x38;
            u8 tagCheck[12];
            if (pk2v2::VerifyTag(cand, tagCheck) && std::memcmp(tagCheck, vtag, 12) == 0) {
                std::memcpy(keyOut, cand, 32);
                return true;
            }
        }
    }
    return false;
}

static bool ScanProcessForPk2v2Key(DWORD pid, const u8 vtag[12], u8 keyOut[32]) {
    HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hp) return false;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    u8* addr = (u8*)si.lpMinimumApplicationAddress;
    u8* maxAddr = (u8*)si.lpMaximumApplicationAddress;

    MEMORY_BASIC_INFORMATION mbi;
    bool found = false;
    while (addr < maxAddr && VirtualQueryEx(hp, addr, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT && 
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_READONLY || mbi.Protect == PAGE_EXECUTE_READWRITE)) {
            size_t sz = mbi.RegionSize;
            if (sz > 0 && sz <= 32 * 1024 * 1024) {
                std::vector<u8> buf(sz);
                SIZE_T nRead = 0;
                if (ReadProcessMemory(hp, addr, buf.data(), sz, &nRead) && nRead >= 12 + 0x38 + 32) {
                    if (ScanBufferForPk2v2Key(buf.data(), (size_t)nRead, vtag, keyOut)) {
                        found = true;
                        break;
                    }
                }
            }
        }
        addr += mbi.RegionSize;
    }
    CloseHandle(hp);
    return found;
}

static bool ScanLiveClientProcesses(const u8 vtag[12], u8 keyOut[32]) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    std::vector<DWORD> pids;
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring name = pe.szExeFile;
            for (wchar_t& c : name) c = (wchar_t)towlower(c);
            if (name.find(L"sro_client") != std::wstring::npos ||
                name.find(L"client") != std::wstring::npos ||
                name.find(L"silkroad") != std::wstring::npos) {
                pids.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    for (DWORD pid : pids) {
        if (ScanProcessForPk2v2Key(pid, vtag, keyOut)) return true;
    }
    return false;
}

static bool ScanDumpFiles(const std::wstring& archivePath, const u8 vtag[12], u8 keyOut[32]) {
    std::vector<std::wstring> candidates;
    std::wstring dir = PathParent(archivePath);
    candidates.push_back(PathJoin(dir, L"sro_client.dmp"));
    candidates.push_back(PathJoin(dir, L"SRO_Client.DMP"));
    candidates.push_back(PathJoin(PathJoin(dir, L"Dump"), L"sro_client.dmp"));
    candidates.push_back(PathJoin(PathJoin(dir, L"Dump"), L"SRO_Client.DMP"));

    // Dynamically discover any other .dmp in client folder or Dump subfolder
    WIN32_FIND_DATAW wfd;
    std::wstring p1 = PathJoin(dir, L"*.dmp");
    HANDLE hf = FindFirstFileW(p1.c_str(), &wfd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                candidates.push_back(PathJoin(dir, wfd.cFileName));
        } while (FindNextFileW(hf, &wfd));
        FindClose(hf);
    }
    std::wstring p2 = PathJoin(PathJoin(dir, L"Dump"), L"*.dmp");
    hf = FindFirstFileW(p2.c_str(), &wfd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                candidates.push_back(PathJoin(PathJoin(dir, L"Dump"), wfd.cFileName));
        } while (FindNextFileW(hf, &wfd));
        FindClose(hf);
    }

    for (const auto& path : candidates) {
        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) continue;

        LARGE_INTEGER fsz;
        if (GetFileSizeEx(hFile, &fsz) && fsz.QuadPart > 0) {
            DWORD chunkSize = 8 * 1024 * 1024;
            std::vector<u8> chunk(chunkSize + 128);
            u64 offset = 0;
            bool found = false;
            while (offset < (u64)fsz.QuadPart) {
                DWORD toRead = (DWORD)(std::min<u64>)((u64)chunkSize + 128, (u64)fsz.QuadPart - offset);
                LARGE_INTEGER li; li.QuadPart = (i64)offset;
                SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN);
                DWORD nRead = 0;
                if (!ReadFile(hFile, chunk.data(), toRead, &nRead, nullptr) || nRead < 12 + 0x38 + 32)
                    break;

                if (ScanBufferForPk2v2Key(chunk.data(), nRead, vtag, keyOut)) {
                    found = true;
                    break;
                }
                if (nRead <= 128) break;
                offset += (nRead - 128);
            }
            CloseHandle(hFile);
            if (found) return true;
        } else {
            CloseHandle(hFile);
        }
    }
    return false;
}
} // namespace

Pk2v2Archive::Pk2v2Archive() {}
Pk2v2Archive::~Pk2v2Archive() { close(); }

bool Pk2v2Archive::IsPk2v2(const std::wstring& path){
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h==INVALID_HANDLE_VALUE) return false;
    u8 magic[8]={}; DWORD got=0;
    bool ok = ReadFile(h, magic, 8, &got, nullptr) && got==8;
    CloseHandle(h);
    static const u8 sig[8]={'P','K','2','v','2',0,0,0};
    return ok && std::memcmp(magic, sig, 8)==0;
}

// ---------------------------------------------------------------------------
// open: header -> password resolve -> ChaCha20 + ZSTD -> parse -> tree
// ---------------------------------------------------------------------------
Result Pk2v2Archive::open(const std::wstring& path, const std::string& password, const LogFn& log){
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    close();

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h==INVALID_HANDLE_VALUE) return Result::Fail(Status::AccessDenied, T(L"Dosya acilamadi: ")+path);
    m_handle = h; m_path = path;
    LARGE_INTEGER sz; GetFileSizeEx(h,&sz); m_fileSize=(u64)sz.QuadPart;

    u8 hdr[0x80]={}; DWORD got=0;
    LARGE_INTEGER z; z.QuadPart=0; SetFilePointerEx(h,z,nullptr,FILE_BEGIN);
    if (!ReadFile(h,hdr,0x80,&got,nullptr) || got!=0x80){ close(); return Result::Fail(Status::IoError,T(L"Baslik okunamadi.")); }
    static const u8 sig[8]={'P','K','2','v','2',0,0,0};
    if (std::memcmp(hdr,sig,8)!=0){ close(); return Result::Fail(Status::BadFormat,T(L"PK2v2 imzasi yok.")); }
    u32 version = *(u32*)(hdr+0x08);
    u32 hsize   = *(u32*)(hdr+0x0c);
    u32 flags   = *(u32*)(hdr+0x10);
    u64 dirOff  = *(u64*)(hdr+0x18);
    u64 dirClen = *(u64*)(hdr+0x20);
    u64 dirPlain= *(u64*)(hdr+0x28);
    const u8* q16  = hdr+0x40;
    const u8* vtag = hdr+0x50;
    const u8* nonW1= hdr+0x64;
    if (version!=2 || hsize!=0x80){ close(); return Result::Fail(Status::BadFormat,T(L"PK2v2 surum/baslik beklenmedik.")); }
    if (dirOff+dirClen>m_fileSize && dirOff>m_fileSize){ close(); return Result::Fail(Status::BadFormat,T(L"Dizin ofseti dosya disinda.")); }

    // --- resolve password or direct key against the verify tag ------------
    std::string pw;
    bool have=false;
    u8 key[32]={};
    auto tryPw=[&](const std::string& cand)->bool{
        if (pk2v2::PasswordMatches(cand, q16, vtag)){
            pw=cand;
            if (pk2v2::DeriveKey(pw, q16, key)) {
                have=true;
                return true;
            }
        }
        return false;
    };
    if (!password.empty()) tryPw(password);
    if (!have){ char* e=nullptr; size_t n=0;
        if (_dupenv_s(&e,&n,"PK2V2_PASSWORD")==0 && e){ tryPw(std::string(e)); free(e);} }
    if (!have){ for (const std::string& c : Pk2v2PasswordCandidates()) if (tryPw(c)) break; }
    if (!have){ // the caller gave none and nothing is known - try empty once
        tryPw(std::string());
    }
    if (!have){
        // Try known 32-byte direct ChaCha20 keys (Sailor Arabian Dawn)
        for (const auto& k : Pk2v2KnownKeys()){
            u8 tagCheck[12];
            if (pk2v2::VerifyTag(k.data(), tagCheck) && std::memcmp(tagCheck, vtag, 12) == 0){
                std::memcpy(key, k.data(), 32);
                have = true;
                break;
            }
        }
    }
    if (!have){
        // Dynamic fallback 1: scan running sro_client processes in real-time
        if (ScanLiveClientProcesses(vtag, key)){
            have = true;
            if (log) log(L"PK2v2 anahtari calisan istemci surecinden otomatik olarak yakalandi.");
        }
    }
    if (!have){
        // Dynamic fallback 2: scan available crash/memory dump files
        if (ScanDumpFiles(m_path, vtag, key)){
            have = true;
            if (log) log(L"PK2v2 anahtari bellek dokumunden (DMP) otomatik olarak yakalandi.");
        }
    }
    if (!have){
        close();
        return Result::Fail(Status::WrongKey,
            T(L"PK2v2 parolasi gerekli (bu istemciye ozgu calisma-zamani sirri). "
            L"--password ile verin veya PK2V2_PASSWORD ortam degiskenini ayarlayin."));
    }

    // --- read + decrypt + decompress the directory ------------------------
    if (log) log(L"PK2v2 dizini cozuluyor (ChaCha20 + ZSTD)...");
    std::vector<u8> ct;
    Result rr = readRaw(dirOff, dirClen, ct);
    if (!rr.ok()){ close(); return rr; }

    std::memcpy(m_key,key,32);

    std::vector<u8> dirPlainBuf;
    if (flags & 1){ // ChaCha20
        std::vector<u8> tmp(ct.size());
        pk2v2::ChaCha20Decrypt(key, nonW1, ct.data(), ct.size(), tmp.data());
        ct.swap(tmp);
    }
    if (flags & 2){ // ZSTD
        if (ct.size()<4 || !(ct[0]==0x28&&ct[1]==0xB5&&ct[2]==0x2F&&ct[3]==0xFD)){
            close(); return Result::Fail(Status::WrongKey,T(L"Cozulen dizin ZSTD cercevesi degil (yanlis parola?)."));
        }
        if (!pk2v2::ZstdDecompress(ct.data(), ct.size(), (size_t)dirPlain, dirPlainBuf)){
            close(); return Result::Fail(Status::BadFormat,T(L"Dizin ZSTD acilamadi."));
        }
    } else {
        dirPlainBuf.swap(ct);
    }

    // --- parse the "2LBT" directory blob ----------------------------------
    const u8* b = dirPlainBuf.data(); size_t bn = dirPlainBuf.size();
    if (bn<0x10 || *(u32*)b != pk2v2::kDirMagic){ close(); return Result::Fail(Status::BadFormat,T(L"Dizin imzasi (2LBT) yok.")); }
    u32 count = *(u32*)(b+4);
    size_t pool = 0x10 + (size_t)count*0x40;
    if (pool>bn){ close(); return Result::Fail(Status::BadFormat,T(L"Dizin girdi sayisi tutarsiz.")); }

    struct Raw {
        u64 off, compSize, plainSize, nonce;
        i32 parent;
        u32 nameOff;
        u16 nameLen;
        u8 type;
        u8 flags;
    };
    std::vector<Raw> raws(count);
    std::vector<std::string> names(count);
    for (u32 i = 0; i < count; i++) {
        const u8* e = b + 0x10 + (size_t)i * 0x40;
        Raw r;
        r.off = *(u64*)(e + 0x00);
        r.compSize = *(u64*)(e + 0x08);
        r.plainSize = *(u64*)(e + 0x10);
        r.nonce = *(u64*)(e + 0x28);
        r.parent = *(i32*)(e + 0x30);
        r.nameOff = *(u32*)(e + 0x34);
        r.nameLen = *(u16*)(e + 0x38);
        r.type = e[0x3a];
        r.flags = e[0x3b];
        raws[i] = r;
        size_t no = pool + r.nameOff;
        if (no + r.nameLen <= bn) names[i].assign((const char*)(b + no), r.nameLen);
    }

    // --- build full inner paths from the parent chain ---------------------
    std::vector<std::wstring> paths(count);
    std::vector<char> doneP(count,0);
    std::function<std::wstring(u32,int)> pathOf = [&](u32 i,int depth)->std::wstring{
        if (doneP[i]) return paths[i];
        if (depth>4096){ doneP[i]=1; return paths[i]; }
        std::wstring nm = decodeName(names[i]);
        std::wstring p;
        const Raw& r=raws[i];
        if (r.parent<0 || (u32)r.parent>=count || (u32)r.parent==i || nm==L"." ){
            p = (nm==L"."||nm.empty()) ? std::wstring() : nm;
        } else {
            std::wstring pp = pathOf((u32)r.parent, depth+1);
            p = pp.empty() ? nm : (pp + L"\\" + nm);
        }
        paths[i]=p; doneP[i]=1; return p;
    };

    m_recs.clear(); m_recs.reserve(count);
    m_folderCount=0; m_fileCount=0;
    for (u32 i=0;i<count;i++){
        std::wstring p = pathOf(i,0);
        if (p.empty()) continue;                 // root sentinel
        Rec rec;
        rec.path = p;
        rec.offset = raws[i].off;
        rec.compSize = raws[i].compSize;
        rec.plainSize = raws[i].plainSize;
        rec.nonce = raws[i].nonce;
        rec.flags = raws[i].flags;
        rec.folder = (raws[i].type == 1);
        if (rec.folder) m_folderCount++; else m_fileCount++;
        m_recs.push_back(std::move(rec));
    }

    rebuildTree();
    m_open = true;
    if (log) log(FormatW(L"PK2v2 acildi: %llu klasor, %llu dosya.",
                         (unsigned long long)m_folderCount,(unsigned long long)m_fileCount));
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// tree
// ---------------------------------------------------------------------------
void Pk2v2Archive::rebuildTree(){
    m_nodes.clear(); m_byPath.clear(); m_fileRec.clear();
    m_nodes.emplace_back();
    Pk2Node& rootN = m_nodes.front();
    rootN.parent=nullptr; rootN.loaded=true;
    rootN.entry.type=Pk2EntryType::Folder; rootN.entry.nameAnsi="."; rootN.entry.name=L".";
    m_byPath[L""]=&rootN;

    for (size_t i=0;i<m_recs.size();++i){
        const Rec& rec=m_recs[i];
        if (rec.folder){ intern(rec.path); continue; }
        std::wstring folder, name=rec.path;
        size_t s = rec.path.find_last_of(L'\\');
        if (s!=std::wstring::npos){ folder=rec.path.substr(0,s); name=rec.path.substr(s+1); }
        Pk2Node* parent = intern(folder);
        m_nodes.emplace_back();
        Pk2Node* fn=&m_nodes.back();
        fn->parent=parent; fn->loaded=true;
        fn->name=name; fn->fullPath=rec.path;
        fn->entry.type=Pk2EntryType::File; fn->entry.name=name;
        fn->entry.offset=(i64)rec.offset; fn->entry.size=(u32)rec.plainSize;
        parent->files.push_back(fn);
        m_byPath[lower(rec.path)]=fn;
        m_fileRec[fn]=i;
    }
}

Pk2Node* Pk2v2Archive::intern(const std::wstring& folderPath){
    if (folderPath.empty()) return &m_nodes.front();
    std::wstring key=lower(folderPath);
    auto it=m_byPath.find(key);
    if (it!=m_byPath.end()) return it->second;
    std::wstring parentPath, name=folderPath;
    size_t s=folderPath.find_last_of(L'\\');
    if (s!=std::wstring::npos){ parentPath=folderPath.substr(0,s); name=folderPath.substr(s+1); }
    Pk2Node* parent=intern(parentPath);
    m_nodes.emplace_back();
    Pk2Node* fn=&m_nodes.back();
    fn->parent=parent; fn->loaded=true;
    fn->name=name; fn->fullPath=folderPath;
    fn->entry.type=Pk2EntryType::Folder; fn->entry.name=name;
    parent->folders.push_back(fn);
    m_byPath[key]=fn;
    return fn;
}

// ---------------------------------------------------------------------------
// raw IO
// ---------------------------------------------------------------------------
Result Pk2v2Archive::readRaw(u64 off, u64 size, std::vector<u8>& out) const {
    if (!m_handle) return Result::Fail(Status::NotFound,T(L"Arsiv kapali."));
    try { out.resize((size_t)size); } catch(...){ return Result::Fail(Status::OutOfMemory,T(L"Bellek yetersiz.")); }
    if (size==0) return Result::Ok();
    LARGE_INTEGER li; li.QuadPart=(i64)off;
    if (!SetFilePointerEx((HANDLE)m_handle,li,nullptr,FILE_BEGIN)) return Result::Fail(Status::IoError,T(L"Konumlandirma hatasi."));
    size_t done=0;
    while (done<(size_t)size){
        DWORD want=(DWORD)(std::min<u64>)(1u<<20,size-done), gotN=0;
        if (!ReadFile((HANDLE)m_handle,out.data()+done,want,&gotN,nullptr)||gotN==0) return Result::Fail(Status::IoError,T(L"Okuma hatasi."));
        done+=gotN;
    }
    return Result::Ok();
}

// ---------------------------------------------------------------------------
// IArchive surface
// ---------------------------------------------------------------------------
bool Pk2v2Archive::isOpen() const { return m_open; }
const std::wstring& Pk2v2Archive::path() const { return m_path; }
u64  Pk2v2Archive::fileSize() const { return m_fileSize; }

Pk2HeaderInfo Pk2v2Archive::headerInfo() const {
    Pk2HeaderInfo hi; hi.signature=L"PK2v2"; hi.versionText=L"2"; hi.encrypted=true; hi.signatureValid=m_open;
    return hi;
}

Pk2Node* Pk2v2Archive::root() const { return m_nodes.empty()?nullptr:const_cast<Pk2Node*>(&m_nodes.front()); }
Result Pk2v2Archive::loadChildren(Pk2Node*){ return Result::Ok(); }

void Pk2v2Archive::childrenOf(Pk2Node* node, std::vector<Pk2Node*>& folders, std::vector<Pk2Node*>& files){
    folders.clear(); files.clear();
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!node) return; folders=node->folders; files=node->files;
}

Result Pk2v2Archive::findNode(const std::wstring& innerPath, Pk2Node** out){
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (out) *out=nullptr;
    std::wstring key=innerPath; for (wchar_t& c:key) if (c==L'/') c=L'\\';
    while (!key.empty()&&key.front()==L'\\') key.erase(0,1);
    while (!key.empty()&&key.back()==L'\\') key.pop_back();
    auto it=m_byPath.find(lower(key));
    if (it==m_byPath.end()) return Result::Fail(Status::NotFound);
    if (out) *out=it->second; return Result::Ok();
}
Result Pk2v2Archive::findFile(const std::wstring& innerPath, Pk2Node** out){
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Pk2Node* n=nullptr; Result r=findNode(innerPath,&n); if (!r.ok()) return r;
    if (!n||!n->isFile()) return Result::Fail(Status::NotFound);
    if (out) *out=n; return Result::Ok();
}

Result Pk2v2Archive::readFile(const Pk2Node* node, std::vector<u8>& out) const {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    out.clear();
    if (!node || !node->isFile()) return Result::Fail(Status::InvalidArgument, T(L"Dosya dugumu degil."));

    auto it = m_fileRec.find(node);
    if (it == m_fileRec.end() || it->second >= m_recs.size())
        return Result::Fail(Status::NotFound, T(L"Dosya kaydi bulunamadi."));

    const Rec& rec = m_recs[it->second];
    if (rec.compSize == 0) return Result::Ok();

    if (rec.offset + rec.compSize > m_fileSize)
        return Result::Fail(Status::BadFormat, T(L"Dosya verisi arsiv disinda: ") + node->fullPath);

    std::vector<u8> raw;
    Result r = readRaw(rec.offset, rec.compSize, raw);
    if (!r.ok()) return r;

    // Decrypt with ChaCha20 using archive key and entry's 8-byte nonce
    std::vector<u8> dec(raw.size());
    pk2v2::ChaCha20Decrypt8B(m_key, (const u8*)&rec.nonce, raw.data(), raw.size(), dec.data(), 0);

    // Decompress if compressed (rec.flags == 1 or compressed frame detected)
    if (rec.flags == 1 || (dec.size() >= 4 && dec[0] == 0x28 && dec[1] == 0xB5 && dec[2] == 0x2F && dec[3] == 0xFD)) {
        if (dec.size() >= 4 && dec[0] == 0x28 && dec[1] == 0xB5 && dec[2] == 0x2F && dec[3] == 0xFD) {
            // Direct single-frame ZSTD
            if (!pk2v2::ZstdDecompress(dec.data(), dec.size(), (size_t)rec.plainSize, out)) {
                return Result::Fail(Status::BadFormat, T(L"Dosya ZSTD acilamadi: ") + node->fullPath);
            }
        } else if (dec.size() >= 8) {
            u32 numChunks = *(u32*)dec.data();
            if (numChunks >= 1 && 4 + (size_t)numChunks * 4 <= dec.size()) {
                const u32* chunkSizes = (const u32*)(dec.data() + 4);
                size_t totalChunkComp = 0;
                for (u32 j = 0; j < numChunks; ++j) totalChunkComp += chunkSizes[j];

                if (4 + (size_t)numChunks * 4 + totalChunkComp <= dec.size()) {
                    // Multi-chunk ZSTD file
                    out.clear();
                    out.reserve((size_t)rec.plainSize);
                    size_t chunkOff = 4 + (size_t)numChunks * 4;
                    for (u32 j = 0; j < numChunks; ++j) {
                        size_t chunkCompSz = chunkSizes[j];
                        size_t remainingPlain = (size_t)rec.plainSize - out.size();
                        size_t expectedChunkPlain = (std::min<size_t>)(1024 * 1024, remainingPlain);

                        std::vector<u8> chunkOut;
                        if (!pk2v2::ZstdDecompress(dec.data() + chunkOff, chunkCompSz, expectedChunkPlain, chunkOut)) {
                            return Result::Fail(Status::BadFormat, T(L"ZSTD parca acilamadi: ") + node->fullPath);
                        }
                        out.insert(out.end(), chunkOut.begin(), chunkOut.end());
                        chunkOff += chunkCompSz;
                    }
                } else if (dec.size() >= 12 && dec[8] == 0x28 && dec[9] == 0xB5 && dec[10] == 0x2F && dec[11] == 0xFD) {
                    if (!pk2v2::ZstdDecompress(dec.data() + 8, dec.size() - 8, (size_t)rec.plainSize, out)) {
                        return Result::Fail(Status::BadFormat, T(L"Dosya ZSTD acilamadi: ") + node->fullPath);
                    }
                } else {
                    out.swap(dec);
                }
            } else if (dec.size() >= 12 && dec[8] == 0x28 && dec[9] == 0xB5 && dec[10] == 0x2F && dec[11] == 0xFD) {
                if (!pk2v2::ZstdDecompress(dec.data() + 8, dec.size() - 8, (size_t)rec.plainSize, out)) {
                    return Result::Fail(Status::BadFormat, T(L"Dosya ZSTD acilamadi: ") + node->fullPath);
                }
            } else {
                out.swap(dec);
            }
        } else {
            out.swap(dec);
        }
    } else {
        out.swap(dec);
    }

    DecryptIfEncrypted(node->fullPath.empty() ? node->name : node->fullPath, out);
    return Result::Ok();
}
Result Pk2v2Archive::readFile(const std::wstring& innerPath, std::vector<u8>& out){
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Pk2Node* n=nullptr; Result r=findNode(innerPath,&n); if (!r.ok()) return r;
    if (!n||!n->isFile()) return Result::Fail(Status::NotFound,T(L"Bu yol bir dosya degil: ")+innerPath);
    return readFile(n,out);
}
Result Pk2v2Archive::readFilePrefix(const Pk2Node* node, size_t maxBytes, std::vector<u8>& out) const {
    Result r = readFile(node, out);
    if (!r.ok()) return r;
    if (maxBytes > 0 && out.size() > maxBytes) {
        out.resize(maxBytes);
    }
    return Result::Ok();
}
Result Pk2v2Archive::readFilePrefix(const std::wstring& innerPath, size_t maxBytes, std::vector<u8>& out){
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Pk2Node* n=nullptr; Result r=findNode(innerPath,&n); if (!r.ok()) return r;
    if (!n||!n->isFile()) return Result::Fail(Status::NotFound,T(L"Bu yol bir dosya degil: ")+innerPath);
    return readFilePrefix(n,maxBytes,out);
}

Result Pk2v2Archive::extractFile(const Pk2Node* node, const std::wstring& outFilePath) const {
    std::vector<u8> buf; Result r=readFile(node,buf); if (!r.ok()) return r;
    std::wstring parent=PathParent(outFilePath);
    if (!parent.empty()&&!EnsureDirectory(parent)) return Result::Fail(Status::IoError,T(L"Klasor olusturulamadi: ")+parent);
    return WriteEntireFileAtomic(outFilePath, buf.data(), buf.size());
}

Result Pk2v2Archive::extractTree(Pk2Node* node, const std::wstring& outDir, const ProgressFn& progress){
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_open) return Result::Fail(Status::InvalidArgument,T(L"Arsiv acik degil."));
    if (!node) return Result::Fail(Status::InvalidArgument,T(L"Bos dugum."));
    std::wstring base=outDir;
    Result worst=Result::Ok();
    std::function<Result(Pk2Node*)> rec=[&](Pk2Node* n)->Result{
        if (n->isFile()){
            std::wstring rel=n->fullPath; for (wchar_t& c:rel) if (c==L'/') c=L'\\';
            std::wstring target=PathJoin(base, rel);
            Result r=extractFile(n,target);
            if (!r.ok()&&worst.ok()) worst=r;
            if (!ReportProgress(progress,0,0,n->fullPath.c_str())) return Result::Fail(Status::Cancelled);
            return Result::Ok();
        }
        for (Pk2Node* f:n->files){ Result r=rec(f); if (r.status==Status::Cancelled) return r; }
        for (Pk2Node* d:n->folders){ Result r=rec(d); if (r.status==Status::Cancelled) return r; }
        return Result::Ok();
    };
    Result r=rec(node);
    if (r.status==Status::Cancelled) return r;
    return worst;
}
Result Pk2v2Archive::extractAll(const std::wstring& outDir, const ProgressFn& progress){
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_open) return Result::Fail(Status::InvalidArgument,T(L"Arsiv acik degil."));
    return extractTree(root(), outDir, progress);
}

Result Pk2v2Archive::walkAll(const std::function<bool(Pk2Node*)>& visit, const ProgressFn& progress){
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_open) return Result::Fail(Status::InvalidArgument,T(L"Arsiv acik degil."));
    std::vector<Pk2Node*> st; st.push_back(root()); u64 seen=0;
    while (!st.empty()){
        Pk2Node* n=st.back(); st.pop_back();
        for (Pk2Node* f:n->files){ if (visit&&!visit(f)) return Result::Fail(Status::Cancelled); }
        for (size_t i=n->folders.size(); i-->0;){ Pk2Node* d=n->folders[i]; if (visit&&!visit(d)) return Result::Fail(Status::Cancelled); st.push_back(d); }
        if (((++seen)&0x3FF)==0 && !ReportProgress(progress,seen,0,nullptr)) return Result::Fail(Status::Cancelled);
    }
    return Result::Ok();
}
Result Pk2v2Archive::search(const std::wstring& needle, std::vector<Pk2Node*>& results, size_t maxResults, const ProgressFn& progress){
    results.clear();
    if (needle.empty()) return Result::Ok();
    bool hit=false;
    Result r=walkAll([&](Pk2Node* n){
        if (FilterMatches(n->name,n->fullPath,needle)){ results.push_back(n); if (maxResults&&results.size()>=maxResults){hit=true;return false;} }
        return true;
    }, progress);
    if (hit && r.status==Status::Cancelled) return Result::Ok();
    return r;
}
Result Pk2v2Archive::countEntries(bool, u64& folders, u64& files, const ProgressFn&){
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    folders=m_folderCount; files=m_fileCount; return Result::Ok();
}

// writing: unsupported
static Result RO(){ return Result::Fail(Status::AccessDenied, T(L"PK2v2 salt okunur acilir.")); }
Result Pk2v2Archive::addFile(Pk2Node*,const std::wstring&,const u8*,size_t,bool){ return RO(); }
Result Pk2v2Archive::importFile(Pk2Node*,const std::wstring&,const std::wstring&,bool,const ProgressFn&){ return RO(); }
Result Pk2v2Archive::importFolder(Pk2Node*,const std::wstring&,const ProgressFn&){ return RO(); }
Result Pk2v2Archive::addFolder(Pk2Node*,const std::wstring&,Pk2Node**){ return RO(); }
Result Pk2v2Archive::remove(Pk2Node*){ return RO(); }
Result Pk2v2Archive::rename(Pk2Node*,const std::wstring&){ return RO(); }
Result Pk2v2Archive::compact(u64*,const ProgressFn&){ return RO(); }
Result Pk2v2Archive::flush(){ return Result::Ok(); }

void Pk2v2Archive::close(){
    if (m_handle){ CloseHandle((HANDLE)m_handle); m_handle=nullptr; }
    m_open=false; m_fileSize=0; m_path.clear();
    m_recs.clear(); m_nodes.clear(); m_byPath.clear(); m_fileRec.clear();
    m_folderCount=m_fileCount=0;
    SecureZeroMemory(m_key,sizeof(m_key));
}

} // namespace sro
