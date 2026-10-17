// nn::fs at the API level, on host files. The game imports these by name (main), so no fsp-srv IPC is involved:
//   rom:   -> private/extract/romfs (the RomFS with the update applied)
//   save:  -> private/user/save/<mount>
//   cache: -> private/user/cache<index>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <nx/runtime.h>

#include "platform.h"

namespace fs = std::filesystem;

namespace nx {

namespace {

// NX_TRACE_FS=1: every file the game opens (and the ones it looks for and does not find).
bool trace_fs() {
    static const bool on = g_trace || std::getenv("NX_TRACE_FS") != nullptr;
    return on;
}

// The last opens, newest last: a crash report that says what was being loaded (log_recent_files, read by the crash
// handlers; try_lock there, a handler must not wait for a thread that holds the lock).
constexpr size_t kRecentOps = 24;
std::mutex g_recent_lock;
struct RecentOp {
    int64_t thread = 0;
    uint64_t seq = 0;
    char text[200] = {0};
} g_recent[kRecentOps];
uint64_t g_recent_seq = 0;

void note_open(const char* what, const std::string& guest_path, const char* result) {
    std::lock_guard lk(g_recent_lock);
    RecentOp& r = g_recent[g_recent_seq % kRecentOps];
    r.thread = plat::current_thread_id();
    r.seq = ++g_recent_seq;
    std::snprintf(r.text, sizeof r.text, "%s %s: %s", what, guest_path.c_str(), result);
}

constexpr uint32_t kOk = 0;
constexpr uint32_t kPathNotFound = 0x202;       // fs 1
constexpr uint32_t kPathAlreadyExists = 0x402;  // fs 2
constexpr uint32_t kTargetNotFound = 0x2EE202;  // fs 6001-ish: generic "not mounted"

std::mutex g_fs;
std::map<std::string, fs::path> g_mounts;
// Open files: shared so a read in progress survives a close from another thread. Reads run outside g_fs (which
// only guards the tables): one global lock made every streaming thread (Bink animations, FMOD banks) wait behind
// any big read, felt as hitches.
struct File {
    FILE* f;
    std::string path;
    File(FILE* file, std::string p) : f(file), path(std::move(p)) {}
    File(const File&) = delete;  // owns the FILE: one instance only
    File& operator=(const File&) = delete;
    ~File() { std::fclose(f); }
};
std::map<uint64_t, std::shared_ptr<File>> g_files;

// Files the runtime adds or replaces (mods): key = path under the mount point, '/' separated, lower case.
std::map<std::string, std::string> g_virtual;

std::string virtual_key(const fs::path& p) {
    auto g = p.lexically_normal().generic_u8string();
    std::string s(g.begin(), g.end());
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// The virtual file whose key the resolved path ends with, or nullptr.
const std::string* find_virtual(const fs::path& resolved) {
    if (g_virtual.empty() || resolved.empty()) return nullptr;
    std::string k = virtual_key(resolved);
    for (auto& [key, data] : g_virtual)
        if (k.size() >= key.size() + 1 && k.compare(k.size() - key.size(), key.size(), key) == 0 &&
            k[k.size() - key.size() - 1] == '/')
            return &data;
    return nullptr;
}
struct Dir {
    std::vector<fs::directory_entry> entries;
    size_t pos = 0;
};
std::map<uint64_t, Dir> g_dirs;
uint64_t g_next = 1;

fs::path data_root() { return fs::u8path(plat::data_root()); }

std::string utf8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string cstr(uint64_t p) { return p ? std::string(reinterpret_cast<const char*>(p)) : std::string(); }

// "mount:/a/b" -> host path; empty when the mount is unknown.
fs::path resolve(const std::string& guest) {
    auto colon = guest.find(':');
    if (colon == std::string::npos) return {};
    auto it = g_mounts.find(guest.substr(0, colon));
    if (it == g_mounts.end()) return {};
    std::string rest = guest.substr(colon + 1);
    while (!rest.empty() && (rest[0] == '/' || rest[0] == '\\')) rest.erase(0, 1);
    return it->second / fs::u8path(rest);
}

void mount(const std::string& name, const fs::path& p) {
    fs::create_directories(p);
    g_mounts[name] = p;
    log("fs: mounted %s: -> %s", name.c_str(), p.string().c_str());
}

// ---- mounts ---------------------------------------------------------------------------------------------------

void QueryMountRomCacheSize(CPU& c) {
    st<uint64_t>(c.x[0], 0x1000);
    c.x[0] = kOk;
}

void MountRom(CPU& c) {
    std::lock_guard lk(g_fs);
    mount(cstr(c.x[0]), data_root() / "extract" / "romfs");
    c.x[0] = kOk;
}

void EnsureSaveData(CPU& c) { c.x[0] = kOk; }

void MountSaveData(CPU& c) {
    std::lock_guard lk(g_fs);
    std::string name = cstr(c.x[0]);
    mount(name, data_root() / "user" / "save");
    c.x[0] = kOk;
}

void Unmount(CPU& c) {
    std::lock_guard lk(g_fs);
    g_mounts.erase(cstr(c.x[0]));
    c.x[0] = kOk;
}

void Commit(CPU& c) { c.x[0] = kOk; }
void EnableGlobalFileDataCache(CPU& c) { c.x[0] = kOk; }
void DisableGlobalFileDataCache(CPU& c) { c.x[0] = kOk; }

// ---- files ----------------------------------------------------------------------------------------------------

void CreateFile(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    int64_t size = int64_t(c.x[1]);
    if (p.empty() || !fs::exists(p.parent_path())) {
        c.x[0] = kPathNotFound;
        return;
    }
    if (fs::exists(p)) {
        c.x[0] = kPathAlreadyExists;
        return;
    }
    FILE* f = plat::fopen_utf8(utf8(p), "wb");
    if (f) {
        if (size > 0) {
            plat::fseek64(f, size - 1, SEEK_SET);
            fputc(0, f);
        }
        fclose(f);
    }
    c.x[0] = f ? kOk : kPathNotFound;
}

void DeleteFile(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    std::error_code ec;
    c.x[0] = !p.empty() && fs::remove(p, ec) ? kOk : kPathNotFound;
}

void CreateDirectory(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    if (p.empty()) {
        c.x[0] = kPathNotFound;
        return;
    }
    if (fs::exists(p)) {
        c.x[0] = kPathAlreadyExists;
        return;
    }
    std::error_code ec;
    c.x[0] = fs::create_directory(p, ec) ? kOk : kPathNotFound;
}

void GetEntryType(CPU& c) {  // (DirectoryEntryType* out, path)
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[1]));
    if (find_virtual(p)) {
        st<uint32_t>(c.x[0], 1);
        c.x[0] = kOk;
        return;
    }
    std::error_code ec;
    if (p.empty() || !fs::exists(p, ec)) {
        c.x[0] = kPathNotFound;
        return;
    }
    st<uint32_t>(c.x[0], fs::is_directory(p) ? 0 : 1);
    c.x[0] = kOk;
}

void OpenFile(CPU& c) {  // (FileHandle* out, path, int mode: 1 read, 2 write, 4 append)
    std::lock_guard lk(g_fs);
    std::string guest = cstr(c.x[1]);
    fs::path p = resolve(guest);
    int mode = int(c.x[2]);
    if (const std::string* v = find_virtual(p)) {  // served from memory, through a temporary file
        FILE* f = std::tmpfile();
        if (!f) {
            c.x[0] = kPathNotFound;
            return;
        }
        std::fwrite(v->data(), 1, v->size(), f);
        std::fflush(f);
        uint64_t h = g_next++;
        g_files[h] = std::make_shared<File>(f, guest);
        st<uint64_t>(c.x[0], h);
        note_open("OpenFile", guest, "virtual");
        if (trace_fs()) log("fs: OpenFile(%s) -> virtual, %zu bytes", guest.c_str(), v->size());
        c.x[0] = kOk;
        return;
    }
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(p, ec)) {
        note_open("OpenFile", guest, p.empty() ? "unknown mount" : "not found");
        if (trace_fs()) log("fs: OpenFile(%s) not found", guest.c_str());
        c.x[0] = kPathNotFound;
        return;
    }
    FILE* f = plat::fopen_utf8(utf8(p), (mode & 2) ? "r+b" : "rb");
    if (!f) {
        note_open("OpenFile", guest, "the host could not open it");
        if (trace_fs()) log("fs: OpenFile(%s) failed to open on the host (errno %d)", guest.c_str(), errno);
        c.x[0] = kPathNotFound;
        return;
    }
    uint64_t h = g_next++;
    g_files[h] = std::make_shared<File>(f, guest);
    st<uint64_t>(c.x[0], h);
    note_open("OpenFile", guest, "ok");
    if (trace_fs()) log("fs: OpenFile(%s, %d) -> %llu", guest.c_str(), mode, (unsigned long long)h);
    c.x[0] = kOk;
}

std::shared_ptr<File> open_file(uint64_t h) {
    std::lock_guard lk(g_fs);
    auto it = g_files.find(h);
    return it == g_files.end() ? nullptr : it->second;
}

void CloseFile(CPU& c) {
    std::lock_guard lk(g_fs);
    g_files.erase(c.x[0]);
}

void GetFileSize(CPU& c) {  // (s64* out, FileHandle)
    auto file = open_file(c.x[1]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    std::lock_guard lk(g_fs);  // the FILE position is shared with writers
    FILE* f = file->f;
    int64_t cur = plat::ftell64(f);
    plat::fseek64(f, 0, SEEK_END);
    st<int64_t>(c.x[0], plat::ftell64(f));
    if (g_trace) log("fs: size %s = %lld", file->path.c_str(), (long long)ld<int64_t>(c.x[0]));
    plat::fseek64(f, cur, SEEK_SET);
    c.x[0] = kOk;
}

uint64_t read_at(const File& file, int64_t off, uint64_t buf, uint64_t size) {
    uint64_t t0 = plat::monotonic_ns();
    uint64_t n = plat::read_at(file.f, off, reinterpret_cast<void*>(buf), size);
    uint64_t ms = (plat::monotonic_ns() - t0) / 1'000'000;
    if (g_trace) log("fs: read %s at %lld size %llu -> %llu", file.path.c_str(), (long long)off, (unsigned long long)size,
                     (unsigned long long)n);
    if (trace_fs() && file.path.find(".bwav") != std::string::npos) {  // streamed audio: is it being consumed?
        static std::mutex m;
        static std::map<std::string, uint64_t> reads;
        std::lock_guard lk(m);
        uint64_t k = ++reads[file.path];
        if (k <= 3 || k % 50 == 0) log("fs: stream read #%llu %s at %lld", (unsigned long long)k, file.path.c_str(), (long long)off);
    }
    if (ms >= 30)
        log("fs: slow read %s: %llu bytes at %lld took %llu ms", file.path.c_str(), (unsigned long long)size,
            (long long)off, (unsigned long long)ms);
    return n;
}

void ReadFileOut(CPU& c) {  // (size_t* out, FileHandle, s64 off, void* buf, size_t size)
    auto file = open_file(c.x[1]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    st<uint64_t>(c.x[0], read_at(*file, int64_t(c.x[2]), c.x[3], c.x[4]));
    c.x[0] = kOk;
}

void ReadFile(CPU& c) {  // (FileHandle, s64 off, void* buf, size_t size)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    read_at(*file, int64_t(c.x[1]), c.x[2], c.x[3]);
    c.x[0] = kOk;
}

void WriteFile(CPU& c) {  // (FileHandle, s64 off, const void* buf, size_t size, const WriteOption&)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    std::lock_guard lk(g_fs);
    plat::fseek64(file->f, int64_t(c.x[1]), SEEK_SET);
    fwrite(reinterpret_cast<const void*>(c.x[2]), 1, c.x[3], file->f);
    fflush(file->f);  // positional reads bypass the FILE buffer (saves only: rare and small)
    c.x[0] = kOk;
}

void FlushFile(CPU& c) {
    if (auto file = open_file(c.x[0])) {
        std::lock_guard lk(g_fs);
        fflush(file->f);
    }
    c.x[0] = kOk;
}

// ---- directories ----------------------------------------------------------------------------------------------

void OpenDirectory(CPU& c) {  // (DirectoryHandle* out, path, int mode: 1 dirs, 2 files)
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[1]));
    int mode = int(c.x[2]);
    std::error_code ec;
    bool found = !p.empty() && fs::is_directory(p, ec);
    note_open("OpenDirectory", cstr(c.x[1]), found ? "ok" : "not found");
    if (trace_fs()) log("fs: OpenDirectory(%s, %d)%s", cstr(c.x[1]).c_str(), mode, found ? "" : " not found");
    if (!found) {
        c.x[0] = kPathNotFound;
        return;
    }
    Dir d;
    for (auto& e : fs::directory_iterator(p, ec)) {
        bool dir = e.is_directory();
        if ((dir && (mode & 1)) || (!dir && (mode & 2))) d.entries.push_back(e);
    }
    uint64_t h = g_next++;
    g_dirs[h] = std::move(d);
    st<uint64_t>(c.x[0], h);
    c.x[0] = kOk;
}

void ReadDirectory(CPU& c) {  // (s64* out_count, DirectoryEntry* entries, DirectoryHandle, s64 max)
    std::lock_guard lk(g_fs);
    auto it = g_dirs.find(c.x[2]);
    if (it == g_dirs.end()) {
        c.x[0] = kTargetNotFound;
        return;
    }
    Dir& d = it->second;
    int64_t n = 0, max = int64_t(c.x[3]);
    for (; n < max && d.pos < d.entries.size(); n++, d.pos++) {
        const auto& e = d.entries[d.pos];
        uint64_t out = c.x[1] + uint64_t(n) * 0x310;
        std::memset(reinterpret_cast<void*>(out), 0, 0x310);
        auto u8 = e.path().filename().u8string();
        std::string name(u8.begin(), u8.end());
        std::memcpy(reinterpret_cast<void*>(out), name.c_str(), std::min<size_t>(name.size(), 0x300));
        bool dir = e.is_directory();
        st<int8_t>(out + 0x304, dir ? 0 : 1);
        st<int64_t>(out + 0x308, dir ? 0 : int64_t(e.file_size()));
    }
    st<int64_t>(c.x[0], n);
    c.x[0] = kOk;
}

void GetDirectoryEntryCount(CPU& c) {  // (s64* out, DirectoryHandle)
    std::lock_guard lk(g_fs);
    auto it = g_dirs.find(c.x[1]);
    st<int64_t>(c.x[0], it == g_dirs.end() ? 0 : int64_t(it->second.entries.size()));
    c.x[0] = kOk;
}

void CloseDirectory(CPU& c) {
    std::lock_guard lk(g_fs);
    g_dirs.erase(c.x[0]);
}

// ---- cache storage --------------------------------------------------------------------------------------------

void OpenCacheStorageList(CPU& c) {
    st<uint64_t>(c.x[0], 1);
    c.x[0] = kOk;
}
void ReadCacheStorageList(CPU& c) {  // (int* out, CacheStorageInfo*, handle, int)
    st<int32_t>(c.x[0], 0);
    c.x[0] = kOk;
}
void CloseCacheStorageList(CPU& c) {}
void CreateCacheStorage(CPU& c) { c.x[0] = kOk; }
void DeleteCacheStorage(CPU& c) { c.x[0] = kOk; }
void MountCacheStorage(CPU& c) {  // (name, int index)
    std::lock_guard lk(g_fs);
    mount(cstr(c.x[0]), data_root() / "user" / ("cache" + std::to_string(int(c.x[1]))));
    c.x[0] = kOk;
}

// ---- SDK 16 additions (Super Mario Bros. Wonder) ---------------------------------------------------------------
// MountRom(name) without a cache buffer, and ReadFile(size_t* out, FileHandle, s64, void*, size_t, const ReadOption&)
// are the older calls with one argument less / more: the same functions serve them.

void IsSaveDataExisting(CPU& c) { c.x[0] = 1; }  // (const account::Uid&) -> bool: EnsureSaveData made it

void SetFileSize(CPU& c) {  // (FileHandle, s64 size)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    std::lock_guard lk(g_fs);
    std::fflush(file->f);
#if defined(_WIN32)
    bool ok = _chsize_s(_fileno(file->f), int64_t(c.x[1])) == 0;
#else
    bool ok = ftruncate(fileno(file->f), off_t(c.x[1])) == 0;
#endif
    c.x[0] = ok ? kOk : kPathNotFound;
}

void DeleteDirectoryRecursively(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    std::error_code ec;
    if (p.empty() || !fs::is_directory(p, ec)) {
        c.x[0] = kPathNotFound;
        return;
    }
    fs::remove_all(p, ec);
    c.x[0] = ec ? kPathNotFound : kOk;
}

}  // namespace

NX_HLE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", QueryMountRomCacheSize);
NX_HLE("_ZN2nn2fs8MountRomEPKcPvm", MountRom);
NX_HLE("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", EnsureSaveData);
NX_HLE("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", MountSaveData);
NX_HLE("_ZN2nn2fs7UnmountEPKc", Unmount);
NX_HLE("_ZN2nn2fs10DeleteFileEPKc", DeleteFile);
NX_HLE("_ZN2nn2fs10CreateFileEPKcl", CreateFile);
NX_HLE("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", OpenFile);
NX_HLE("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", GetFileSize);
NX_HLE("_ZN2nn2fs15CreateDirectoryEPKc", CreateDirectory);
NX_HLE("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", GetEntryType);
NX_HLE("_ZN2nn2fs9CloseFileENS0_10FileHandleE", CloseFile);
NX_HLE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", ReadFileOut);
NX_HLE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", ReadFile);
NX_HLE("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", WriteFile);
NX_HLE("_ZN2nn2fs9FlushFileENS0_10FileHandleE", FlushFile);
NX_HLE("_ZN2nn2fs25EnableGlobalFileDataCacheEPvm", EnableGlobalFileDataCache);
NX_HLE("_ZN2nn2fs26DisableGlobalFileDataCacheEv", DisableGlobalFileDataCache);
NX_HLE("_ZN2nn2fs6CommitEPKc", Commit);
NX_HLE("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", OpenDirectory);
NX_HLE("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", ReadDirectory);
NX_HLE("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", CloseDirectory);
NX_HLE("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", GetDirectoryEntryCount);
NX_HLE("_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE", OpenCacheStorageList);
NX_HLE("_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi", ReadCacheStorageList);
NX_HLE("_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE", CloseCacheStorageList);
NX_HLE("_ZN2nn2fs18CreateCacheStorageEill", CreateCacheStorage);
NX_HLE("_ZN2nn2fs18DeleteCacheStorageEi", DeleteCacheStorage);
NX_HLE("_ZN2nn2fs17MountCacheStorageEPKci", MountCacheStorage);
NX_HLE("_ZN2nn2fs8MountRomEPKc", MountRom);
NX_HLE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", ReadFileOut);
NX_HLE("_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE", IsSaveDataExisting);
NX_HLE("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", SetFileSize);
NX_HLE("_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", DeleteDirectoryRecursively);

void log_recent_files() {
    std::unique_lock lk(g_recent_lock, std::try_to_lock);
    if (!lk.owns_lock()) return;
    log("  last file opens (newest last; thread = host thread id):");
    for (uint64_t k = 0; k < kRecentOps; k++) {
        const RecentOp& r = g_recent[(g_recent_seq + k) % kRecentOps];
        if (r.seq) log("    #%llu thread %lld  %s", (unsigned long long)r.seq, (long long)r.thread, r.text);
    }
}

void add_virtual_file(const std::string& path, std::string data) {
    std::lock_guard lk(g_fs);
    g_virtual[virtual_key(fs::u8path(path))] = std::move(data);
}

std::string read_game_file(const std::string& relative_to_romfs) {
    FILE* f = plat::fopen_utf8(utf8(data_root() / "extract" / "romfs" / fs::u8path(relative_to_romfs)), "rb");
    if (!f) return {};
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

}  // namespace nx
