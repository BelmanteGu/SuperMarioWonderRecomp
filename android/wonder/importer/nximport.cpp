// On-device importer (nximport.h). Formats, as Atmosphere/hactool read them:
//   NSP     PFS0: "PFS0", file count, string table size; 0x18-byte entries {offset, size, name offset}; data after it.
//   XCI     gamecard image: 0x1000 header, "HEAD" at 0x100, root HFS0 offset at 0x130 (its header size at 0x138). HFS0 is PFS0
//           with 0x40-byte entries {offset, size, name offset, hashed size, reserved, SHA-256}. The root holds the
//           partitions update / normal / secure (/ logo); the game's NCAs sit in secure (and normal). The update
//           partition carries the console firmware, not the game's patch: it is skipped. A cartridge has no tickets
//           (standard crypto: key area key from prod.keys). A split image (.xc0 .xc1 / .xci.00 .xci.01) is joined.
//   NCA3    0xC00 header, AES-XTS (header_key, 0x200 sectors, big-endian tweak). Section table at 0x240, fs headers at
//           0x400 + 0x200 * i (SHA-256 of each at 0x280). Body: AES-CTR, counter = upper IV (fs header 0x140, u64 LE:
//           generation | secure value << 32) : (absolute NCA offset >> 4). Key: key area slot 2 (key_area_key_*_rev)
//           or the title key (ticket 0x180, decrypted with titlekek_rev; or title.keys).
//   sparse  base NCA of an update-split title (fs header 0x148): a bucket tree {virtual, physical, storage} mapping the
//           section; storage 0 = data at physical_offset + physical, decrypted with its *virtual* counter; storage 1 =
//           zeros (the update replaces it). The table itself sits at physical_offset + table offset and is decrypted
//           with the sparse generation (<< 16) in place of the upper IV's generation.
//   BKTR    the update's patch RomFS (fs header 0x100): an indirect bucket tree {virtual, physical, storage} over
//           storage 0 = the base RomFS section, storage 1 = this section's data, which is AES-CTR-Ex: a second bucket
//           tree {offset, encryption, generation} gives the generation of each piece of the counter.
//   IVFC    RomFS hash tree: levels 0..n-2 hash the next level (SHA-256 per 2^order block, the last block padded with
//           zeros); level 0 is hashed by the master hash in the fs header. The last level is the RomFS image.
//   NSO     "NSO0", three LZ4 segments (text, ro, data) with SHA-256 of each decompressed segment.
#include "nximport.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

#include "nxcrypto.h"

#ifdef __APPLE__
#define fdatasync fsync  // macOS (host test build) has no fdatasync
#endif

namespace nxi {

namespace {

struct Fail : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Cancelled {};

[[noreturn]] void fail(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    throw Fail(buf);
}

inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32; }
inline void wr_be64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = uint8_t(v >> (56 - 8 * i));
}
inline void wr_be32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (24 - 8 * i));
}

std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) s += d[p[i] >> 4], s += d[p[i] & 15];
    return s;
}
std::string lower(std::string s) {
    for (char& c : s) c = char(tolower(uint8_t(c)));
    return s;
}
bool ends_with(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
std::string tid_str(uint64_t t) {
    char b[20];
    snprintf(b, sizeof b, "%016llX", (unsigned long long)t);
    return b;
}

// ---- storages: a readable byte range; read() throws on any short read ----

struct Storage {
    uint64_t size = 0;
    virtual ~Storage() = default;
    virtual void read(uint64_t off, void* dst, size_t n) = 0;
};

struct FdStorage : Storage {
    int fd;
    std::string name;
    FdStorage(int f, std::string n) : fd(f), name(std::move(n)) {
        struct stat st{};
        if (fstat(fd, &st) != 0) fail("%s: cannot stat (%s)", name.c_str(), strerror(errno));
        size = uint64_t(st.st_size);
    }
    void read(uint64_t off, void* dst, size_t n) override {
        if (off + n > size) fail("%s: read past the end (offset %llu)", name.c_str(), (unsigned long long)off);
        uint8_t* p = static_cast<uint8_t*>(dst);
        while (n) {
            ssize_t r = pread(fd, p, n, off_t(off));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) fail("%s: read error at %llu (%s)", name.c_str(), (unsigned long long)off, r < 0 ? strerror(errno) : "end of file");
            p += r, off += uint64_t(r), n -= size_t(r);
        }
    }
};

struct SubStorage : Storage {
    Storage* parent;
    uint64_t base;
    SubStorage(Storage* p, uint64_t b, uint64_t s) : parent(p), base(b) {
        size = s;
        if (b + s > p->size) fail("a content entry lies outside its container (truncated NSP/XCI?)");
    }
    void read(uint64_t off, void* dst, size_t n) override {
        if (off + n > size) fail("read past the end of a content entry");
        parent->read(base + off, dst, n);
    }
};

// Several files read as one (a split XCI).
struct CatStorage : Storage {
    std::vector<Storage*> parts;
    std::vector<uint64_t> start;
    explicit CatStorage(std::vector<Storage*> p) : parts(std::move(p)) {
        for (Storage* s : parts) start.push_back(size), size += s->size;
    }
    void read(uint64_t off, void* dst, size_t n) override {
        if (off + n > size) fail("read past the end of the split image");
        uint8_t* out = static_cast<uint8_t*>(dst);
        while (n) {
            size_t i = size_t(std::upper_bound(start.begin(), start.end(), off) - start.begin()) - 1;
            uint64_t in = off - start[i];
            size_t m = size_t(std::min<uint64_t>(n, parts[i]->size - in));
            parts[i]->read(in, out, m);
            out += m, off += m, n -= m;
        }
    }
};

constexpr size_t kChunk = 4u << 20;

// AES-CTR section: counter = upper IV : ((abs + offset) >> 4).
void ctr_read(Storage* raw, uint64_t raw_off, uint64_t ctr_off, const Aes128& key, const uint8_t upper[8], void* dst,
              size_t n, std::vector<uint8_t>& buf) {
    // raw_off/ctr_off address the first byte wanted; decrypt from the 16-byte block that contains it
    if ((raw_off - ctr_off) & 15) fail("encrypted data not aligned with its counter");
    uint64_t skip = ctr_off & 15;
    uint8_t* out = static_cast<uint8_t*>(dst);
    while (n) {
        size_t want = std::min<size_t>(n, kChunk);
        size_t span = size_t((skip + want + 15) & ~uint64_t(15));
        uint64_t rstart = raw_off - skip;
        size_t avail = size_t(std::min<uint64_t>(span, raw->size - rstart));
        buf.resize(span);
        raw->read(rstart, buf.data(), avail);
        if (avail < span) std::memset(buf.data() + avail, 0, span - avail);
        uint8_t iv[16];
        std::memcpy(iv, upper, 8);
        wr_be64(iv + 8, (ctr_off - skip) >> 4);
        aes_ctr(key, iv, buf.data(), span);
        std::memcpy(out, buf.data() + skip, want);
        out += want, n -= want, raw_off += want, ctr_off += want;
        skip = ctr_off & 15;
    }
}

struct CtrStorage : Storage {
    Storage* nca;
    uint64_t abs;
    const Aes128* key;
    uint8_t upper[8];
    std::vector<uint8_t> buf;
    CtrStorage(Storage* n, uint64_t a, uint64_t s, const Aes128* k, const uint8_t up[8]) : nca(n), abs(a), key(k) {
        size = s;
        std::memcpy(upper, up, 8);
    }
    void read(uint64_t off, void* dst, size_t n) override {
        if (off + n > size) fail("read past the end of an NCA section");
        ctr_read(nca, abs + off, abs + off, *key, upper, dst, n, buf);
    }
};

// ---- bucket trees (Atmosphere fssystem_bucket_tree): 0x4000-byte nodes; node 0 (+ L2 nodes) index the entry sets,
// each entry set is {index, count, end offset} + entries. We read every entry set in order.

struct BucketTree {
    std::vector<uint8_t> entries;  // count * entry_size
    size_t entry_size = 0;
    int32_t count = 0;
    uint64_t end = 0;
    const uint8_t* entry(size_t i) const { return entries.data() + i * entry_size; }
};

BucketTree parse_bucket_tree(const std::vector<uint8_t>& table, size_t entry_size, int32_t count, const char* what) {
    const size_t node = 0x4000;
    BucketTree t;
    t.entry_size = entry_size;
    t.count = count;
    if (count <= 0) return t;
    const int32_t per_set = int32_t((node - 0x10) / entry_size);
    const int32_t sets = (count + per_set - 1) / per_set;
    const int32_t per_node = int32_t((node - 0x10) / 8);
    int32_t l2 = 0;
    if (sets > per_node) {
        int32_t l2_count = (sets + per_node - 1) / per_node;
        l2 = (sets - (per_node - (l2_count - 1)) + per_node - 1) / per_node;
    }
    const size_t set_base = node * size_t(1 + l2);
    if (table.size() < 0x10) fail("%s: table too small", what);
    t.end = rd64(table.data() + 8);  // node 0 header: {index, count, end offset}
    t.entries.reserve(size_t(count) * entry_size);
    for (int32_t s = 0; s < sets; s++) {
        size_t b = set_base + size_t(s) * node;
        if (b + node > table.size()) fail("%s: table truncated", what);
        int32_t idx = int32_t(rd32(table.data() + b)), n = int32_t(rd32(table.data() + b + 4));
        if (idx != s || n <= 0 || n > per_set) fail("%s: bad entry set %d (wrong key or damaged NSP)", what, s);
        t.entries.insert(t.entries.end(), table.begin() + long(b + 0x10), table.begin() + long(b + 0x10 + size_t(n) * entry_size));
    }
    if (t.entries.size() != size_t(count) * entry_size) fail("%s: %zu entries, header says %d", what, t.entries.size() / entry_size, count);
    return t;
}

// Sparse base section (storage 0 = data at physical_offset + p, decrypted with the virtual counter; 1 = zeros).
struct SparseStorage : Storage {
    Storage* nca;
    uint64_t abs, phys;
    const Aes128* key;
    uint8_t upper[8];
    std::vector<uint64_t> v, p;
    std::vector<int32_t> si;
    std::vector<uint8_t> buf;
    void read(uint64_t off, void* dst, size_t n) override {
        if (off + n > size) fail("read past the end of the sparse section");
        uint8_t* out = static_cast<uint8_t*>(dst);
        while (n) {
            size_t i = size_t(std::upper_bound(v.begin(), v.end(), off) - v.begin()) - 1;
            uint64_t seg_end = i + 1 < v.size() ? v[i + 1] : size;
            size_t m = size_t(std::min<uint64_t>(n, seg_end - off));
            if (si[i] == 0)
                ctr_read(nca, phys + p[i] + (off - v[i]), abs + off, *key, upper, out, m, buf);
            else
                std::memset(out, 0, m);
            out += m, off += m, n -= m;
        }
    }
};

// The update's physical data: AES-CTR-Ex (generation per piece).
struct CtrExStorage : Storage {
    Storage* nca;
    uint64_t abs;
    const Aes128* key;
    uint8_t upper[8];  // the fs header's; bytes 4..7 replaced by each piece's generation
    std::vector<uint64_t> off_;
    std::vector<uint8_t> enc_;
    std::vector<uint32_t> gen_;
    uint64_t covered = 0;  // the pieces cover [0, covered); after it (the tables) the fs header's own counter
    std::vector<uint8_t> buf;
    void read(uint64_t off, void* dst, size_t n) override {
        if (off + n > size) fail("read past the end of the patch section");
        uint8_t* out = static_cast<uint8_t*>(dst);
        while (n) {
            uint8_t iv_up[8];
            std::memcpy(iv_up, upper, 8);
            uint64_t seg_end = size;
            bool plain = false;
            if (off < covered && !off_.empty()) {
                size_t i = size_t(std::upper_bound(off_.begin(), off_.end(), off) - off_.begin());
                if (i == 0) fail("patch section: no counter entry for offset %llu", (unsigned long long)off);
                i--;
                seg_end = i + 1 < off_.size() ? off_[i + 1] : covered;
                wr_be32(iv_up + 4, gen_[i]);
                plain = enc_[i] == 1;
            }
            size_t m = size_t(std::min<uint64_t>(n, seg_end - off));
            if (plain)
                nca->read(abs + off, out, m);
            else
                ctr_read(nca, abs + off, abs + off, *key, iv_up, out, m, buf);
            out += m, off += m, n -= m;
        }
    }
};

struct IndirectStorage : Storage {
    Storage* st[2];
    std::vector<uint64_t> v, p;
    std::vector<int32_t> si;
    void read(uint64_t off, void* dst, size_t n) override {
        if (off + n > size) fail("read past the end of the patched RomFS");
        uint8_t* out = static_cast<uint8_t*>(dst);
        while (n) {
            size_t i = size_t(std::upper_bound(v.begin(), v.end(), off) - v.begin());
            if (i == 0) fail("patched RomFS: no entry for offset %llu", (unsigned long long)off);
            i--;
            uint64_t seg_end = i + 1 < v.size() ? v[i + 1] : size;
            size_t m = size_t(std::min<uint64_t>(n, seg_end - off));
            int32_t s = si[i];
            if (s != 0 && s != 1) fail("patched RomFS: bad storage index %d", s);
            st[s]->read(p[i] + (off - v[i]), out, m);
            out += m, off += m, n -= m;
        }
    }
};

// ---- keys ----

struct Keys {
    std::map<std::string, std::vector<uint8_t>> named;  // prod.keys
    std::map<std::string, std::array<uint8_t, 16>> title;  // rights id (lower hex) -> plain title key
    ~Keys() {
        for (auto& [k, v] : named) secure_zero(v.data(), v.size());
        for (auto& [k, v] : title) secure_zero(v.data(), v.size());
    }

    static bool parse_hex(const std::string& s, std::vector<uint8_t>& out) {
        if (s.size() % 2) return false;
        out.resize(s.size() / 2);
        for (size_t i = 0; i < out.size(); i++) {
            auto nib = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                c = char(tolower(uint8_t(c)));
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            int a = nib(s[2 * i]), b = nib(s[2 * i + 1]);
            if (a < 0 || b < 0) return false;
            out[i] = uint8_t(a << 4 | b);
        }
        return true;
    }
    static void each_line(const std::string& text, const std::function<void(std::string, std::string)>& fn) {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto trim = [](std::string s) {
                size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
                return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
            };
            fn(lower(trim(line.substr(0, eq))), trim(line.substr(eq + 1)));
        }
    }
    void load_prod(const std::string& text) {
        each_line(text, [&](std::string k, std::string v) {
            std::vector<uint8_t> b;
            if (parse_hex(v, b) && !b.empty()) named[k] = std::move(b);
        });
    }
    void load_title(const std::string& text) {
        each_line(text, [&](std::string k, std::string v) {
            std::vector<uint8_t> b;
            if (k.size() == 32 && parse_hex(v, b) && b.size() == 16) {
                std::array<uint8_t, 16> a;
                std::memcpy(a.data(), b.data(), 16);
                title[k] = a;
                secure_zero(b.data(), b.size());
            }
        });
    }
    bool get(const std::string& name, size_t len, uint8_t* out) const {
        auto it = named.find(name);
        if (it == named.end() || it->second.size() != len) return false;
        std::memcpy(out, it->second.data(), len);
        return true;
    }
    static std::string idx(const char* base, int rev) {
        char b[64];
        snprintf(b, sizeof b, "%s_%02x", base, rev);
        return b;
    }
    // AES-ECB decrypt one 16-byte block of src with key k.
    static void unwrap(const uint8_t k[16], const uint8_t src[16], uint8_t out[16]) {
        Aes128 a;
        a.set_key(k);
        a.decrypt_block(src, out);
    }
    // Missing derived keys from master_key_xx + the public sources (hactool pki.c), so a keys file with only master
    // keys works too.
    bool derive(const std::string& name, uint8_t out[16]) const {
        if (get(name, 16, out)) return true;
        int rev = -1;
        auto tail = name.rfind('_');
        if (tail != std::string::npos) rev = int(strtol(name.c_str() + tail + 1, nullptr, 16));
        uint8_t mk[16], src[16];
        if (rev < 0 || !get(idx("master_key", rev), 16, mk)) return false;
        bool ok = false;
        if (name.rfind("titlekek_", 0) == 0 && get("titlekek_source", 16, src)) {
            unwrap(mk, src, out);
            ok = true;
        } else if (name.rfind("key_area_key_", 0) == 0) {
            std::string which = name.substr(13, tail - 13);  // application / ocean / system
            uint8_t kek_src[16], key_src[16], kek[16], srckek[16];
            if (get("key_area_key_" + which + "_source", 16, src) && get("aes_kek_generation_source", 16, kek_src) &&
                get("aes_key_generation_source", 16, key_src)) {
                unwrap(mk, kek_src, kek);
                unwrap(kek, src, srckek);
                unwrap(srckek, key_src, out);
                ok = true;
            }
            secure_zero(kek, 16), secure_zero(srckek, 16);
        }
        secure_zero(mk, 16), secure_zero(src, 16);
        return ok;
    }
    bool header_key(uint8_t out[32]) const {
        if (get("header_key", 32, out)) return true;
        // header_kek = generate_kek(header_kek_source, master_key_00, aes_kek_generation_source,
        // aes_key_generation_source); header_key = header_key_source decrypted with it (hactool pki.c)
        uint8_t mk[16], kek_src[16], key_src[16], hkek_src[16], kek[16], srckek[16], hkek[16], hsrc[32];
        bool ok = get("master_key_00", 16, mk) && get("aes_kek_generation_source", 16, kek_src) &&
                  get("aes_key_generation_source", 16, key_src) && get("header_kek_source", 16, hkek_src) &&
                  get("header_key_source", 32, hsrc);
        if (ok) {
            unwrap(mk, kek_src, kek);
            unwrap(kek, hkek_src, srckek);
            unwrap(srckek, key_src, hkek);
            unwrap(hkek, hsrc, out);
            unwrap(hkek, hsrc + 16, out + 16);
        }
        secure_zero(mk, 16), secure_zero(kek, 16), secure_zero(srckek, 16), secure_zero(hkek, 16), secure_zero(hsrc, 32);
        secure_zero(key_src, 16);
        return ok;
    }
};

struct Ticket {
    uint8_t enc_key[16];
    uint8_t type;  // 0 common, 1 personalized (needs the console's RSA key: not usable here)
};

// ---- NCA ----

struct Section {
    bool present = false;
    uint64_t off = 0, size = 0;
    const uint8_t* fs = nullptr;  // 0x200 fs header
    uint8_t upper[8];
    uint8_t partition() const { return fs[2]; }
    uint8_t crypt() const { return fs[4]; }
    bool is_patch() const { return rd32(fs + 0x110) == 0x52544B42 && rd32(fs + 0x118) > 0; }   // indirect "BKTR"
    bool is_sparse() const { return rd32(fs + 0x158) == 0x52544B42 && rd32(fs + 0x160) > 0; }  // sparse "BKTR"
};

struct Nca {
    std::string name;
    std::string src;  // the NSP/XCI it came from
    std::unique_ptr<SubStorage> file;
    uint8_t hdr[0xC00];
    uint8_t content_type = 0;
    uint64_t title_id = 0;
    int rev = 0;
    bool has_rights = false;
    uint8_t rights[16];
    Aes128 key;
    bool key_ok = false;
    std::string key_problem;
    Section sec[4];
    std::vector<std::unique_ptr<Storage>> owned;
    ~Nca() { secure_zero(hdr, sizeof hdr); }
};

std::unique_ptr<Nca> open_nca(Storage* nsp, uint64_t off, uint64_t size, const std::string& name, const Keys& keys,
                              const uint8_t header_key[32], const std::map<std::string, Ticket>& tickets) {
    auto n = std::make_unique<Nca>();
    n->name = name;
    n->file = std::make_unique<SubStorage>(nsp, off, size);
    if (size < 0xC00) return nullptr;
    n->file->read(0, n->hdr, 0xC00);
    Aes128 k1, k2;
    k1.set_key(header_key);
    k2.set_key(header_key + 16);
    aes_xts_decrypt(k1, k2, 0, 0x200, n->hdr, 0xC00);
    if (std::memcmp(n->hdr + 0x200, "NCA3", 4) != 0) {
        if (std::memcmp(n->hdr + 0x200, "NCA2", 4) == 0 || std::memcmp(n->hdr + 0x200, "NCA0", 4) == 0)
            fail("%s: old NCA format (NCA2/NCA0) is not supported", name.c_str());
        fail("%s: the NCA header does not decrypt (header_key in prod.keys is wrong or missing)", name.c_str());
    }
    const uint8_t* h = n->hdr + 0x200;
    n->content_type = h[5];
    n->title_id = rd64(h + 0x10);
    int gen = std::max<int>(h[6], h[0x20]);
    n->rev = gen ? gen - 1 : 0;
    std::memcpy(n->rights, h + 0x30, 16);
    for (int i = 0; i < 16; i++) n->has_rights |= n->rights[i] != 0;
    for (int i = 0; i < 4; i++) {
        uint32_t start = rd32(h + 0x40 + 16 * i), end = rd32(h + 0x44 + 16 * i);
        if (!start && !end) continue;
        Section& s = n->sec[i];
        s.present = true;
        s.off = uint64_t(start) * 0x200;
        s.size = uint64_t(end - start) * 0x200;
        s.fs = n->hdr + 0x400 + 0x200 * i;
        uint8_t digest[32];
        sha256(s.fs, 0x200, digest);
        if (std::memcmp(digest, h + 0x80 + 0x20 * i, 32) != 0) fail("%s: fs header %d hash mismatch (damaged NSP)", name.c_str(), i);
        for (int j = 0; j < 8; j++) s.upper[j] = s.fs[0x147 - j];  // u64 LE -> big-endian upper counter half
    }
    // the content key
    uint8_t ck[16];
    if (n->has_rights) {
        std::string rid = hex(n->rights, 16);
        auto t = tickets.find(rid);
        auto tk = keys.title.find(rid);
        if (t != tickets.end() && t->second.type == 0) {
            uint8_t kek[16];
            if (keys.derive(Keys::idx("titlekek", n->rev), kek)) {
                Keys::unwrap(kek, t->second.enc_key, ck);
                n->key_ok = true;
            } else {
                n->key_problem = "prod.keys has no " + Keys::idx("titlekek", n->rev) + " (newer keys needed)";
            }
            secure_zero(kek, 16);
        }
        if (!n->key_ok && tk != keys.title.end()) {
            std::memcpy(ck, tk->second.data(), 16);
            n->key_ok = true;
        }
        if (!n->key_ok && n->key_problem.empty())
            n->key_problem = "no ticket in the NSP/XCI and no line for rights id " + rid + " in title.keys";
    } else {
        static const char* kinds[3] = {"key_area_key_application", "key_area_key_ocean", "key_area_key_system"};
        uint8_t kak[16];
        int which = h[7] < 3 ? h[7] : 0;
        if (keys.derive(Keys::idx(kinds[which], n->rev), kak)) {
            Keys::unwrap(kak, h + 0x100 + 0x20, ck);  // key area slot 2: the AES-CTR key
            n->key_ok = true;
        } else {
            n->key_problem = "prod.keys has no " + Keys::idx(kinds[which], n->rev) + " (newer keys needed)";
        }
        secure_zero(kak, 16);
    }
    if (n->key_ok) n->key.set_key(ck);
    secure_zero(ck, 16);
    return n;
}

// The decrypted section as stored (for a patch section: its *physical* bytes, i.e. CTR-Ex; tables are plain CTR).
Storage* plain_section(Nca& n, int i) {
    Section& s = n.sec[i];
    if (!n.key_ok) fail("%s: %s", n.name.c_str(), n.key_problem.c_str());
    uint8_t c = s.crypt();
    if (c == 1) {
        n.owned.push_back(std::make_unique<SubStorage>(n.file.get(), s.off, s.size));
    } else if (c == 3 || c == 4) {
        n.owned.push_back(std::make_unique<CtrStorage>(n.file.get(), s.off, s.size, &n.key, s.upper));
    } else {
        fail("%s: section %d uses encryption type %d, which this importer does not handle", n.name.c_str(), i, c);
    }
    return n.owned.back().get();
}

Storage* sparse_section(Nca& n, int i) {
    Section& s = n.sec[i];
    if (!n.key_ok) fail("%s: %s", n.name.c_str(), n.key_problem.c_str());
    const uint8_t* fs = s.fs;
    uint64_t b_off = rd64(fs + 0x148), b_size = rd64(fs + 0x150);
    int32_t count = int32_t(rd32(fs + 0x160));
    uint64_t phys = rd64(fs + 0x168);
    uint16_t gen = rd16(fs + 0x170);
    auto sp = std::make_unique<SparseStorage>();
    sp->nca = n.file.get();
    sp->abs = s.off;
    sp->phys = phys;
    sp->key = &n.key;
    std::memcpy(sp->upper, s.upper, 8);
    std::vector<uint8_t> table(b_size);
    uint8_t up[8];
    std::memcpy(up, s.upper, 4);
    wr_be32(up + 4, uint32_t(gen) << 16);
    std::vector<uint8_t> buf;
    ctr_read(n.file.get(), phys + b_off, phys + b_off, n.key, up, table.data(), table.size(), buf);
    BucketTree t = parse_bucket_tree(table, 0x14, count, "sparse table");
    if (t.end != s.size) fail("%s: sparse table covers %llu bytes, the section has %llu", n.name.c_str(), (unsigned long long)t.end, (unsigned long long)s.size);
    for (int32_t e = 0; e < count; e++) {
        const uint8_t* p = t.entry(size_t(e));
        sp->v.push_back(rd64(p));
        sp->p.push_back(rd64(p + 8));
        sp->si.push_back(int32_t(rd32(p + 16)));
    }
    if (sp->v.empty() || sp->v[0] != 0) fail("%s: sparse table does not start at 0", n.name.c_str());
    sp->size = t.end;
    n.owned.push_back(std::move(sp));
    return n.owned.back().get();
}

// The patched section: indirect over (base section, CTR-Ex physical data).
Storage* patched_section(Nca& n, int i, Storage* base) {
    Section& s = n.sec[i];
    if (!n.key_ok) fail("%s: %s", n.name.c_str(), n.key_problem.c_str());
    const uint8_t* fs = s.fs;
    Storage* plain = plain_section(n, i);  // tables: plain CTR
    uint64_t ind_off = rd64(fs + 0x100), ind_size = rd64(fs + 0x108);
    int32_t ind_count = int32_t(rd32(fs + 0x118));
    uint64_t ex_off = rd64(fs + 0x120), ex_size = rd64(fs + 0x128);
    int32_t ex_count = int32_t(rd32(fs + 0x138));
    if (rd32(fs + 0x130) != 0x52544B42) fail("%s: patch section without its AES-CTR-Ex table", n.name.c_str());
    std::vector<uint8_t> ind(ind_size), ex(ex_size);
    plain->read(ind_off, ind.data(), ind.size());
    plain->read(ex_off, ex.data(), ex.size());
    BucketTree ti = parse_bucket_tree(ind, 0x14, ind_count, "indirect table");
    BucketTree te = parse_bucket_tree(ex, 0x10, ex_count, "AES-CTR-Ex table");

    auto cx = std::make_unique<CtrExStorage>();
    cx->nca = n.file.get();
    cx->abs = s.off;
    cx->key = &n.key;
    std::memcpy(cx->upper, s.upper, 8);
    cx->size = s.size;
    cx->covered = te.end;
    for (int32_t e = 0; e < ex_count; e++) {
        const uint8_t* p = te.entry(size_t(e));
        cx->off_.push_back(rd64(p));
        cx->enc_.push_back(p[8]);
        cx->gen_.push_back(rd32(p + 12));
    }
    Storage* phys = cx.get();
    n.owned.push_back(std::move(cx));

    auto in = std::make_unique<IndirectStorage>();
    in->st[0] = base;
    in->st[1] = phys;
    for (int32_t e = 0; e < ind_count; e++) {
        const uint8_t* p = ti.entry(size_t(e));
        in->v.push_back(rd64(p));
        in->p.push_back(rd64(p + 8));
        in->si.push_back(int32_t(rd32(p + 16)));
    }
    in->size = ti.end;
    if (in->v.empty() || in->v[0] != 0) fail("%s: indirect table does not start at 0", n.name.c_str());
    n.owned.push_back(std::move(in));
    return n.owned.back().get();
}

// ---- IVFC (RomFS hash tree) ----

struct VerifiedLevel {
    Storage* st = nullptr;
    uint64_t off = 0, size = 0;
    uint32_t bs = 0;
    std::vector<uint8_t> hashes;  // the previous level: one SHA-256 per block of this one
    // cache
    uint64_t c_start = ~0ull;
    std::vector<uint8_t> cache;
    std::vector<uint8_t> pad;

    void load(uint64_t start) {
        uint64_t chunk = std::max<uint64_t>(kChunk / bs, 1) * bs;
        c_start = start / chunk * chunk;
        size_t n = size_t(std::min<uint64_t>(chunk, size - c_start));
        cache.resize(n);
        st->read(off + c_start, cache.data(), n);
        for (size_t b = 0; b < n; b += bs) {
            uint64_t blk = (c_start + b) / bs;
            if ((blk + 1) * 32 > hashes.size()) fail("RomFS hash table too short");
            uint8_t d[32];
            size_t m = std::min<size_t>(bs, n - b);
            if (m == bs) {
                sha256(cache.data() + b, bs, d);
            } else {  // last block: zero padded
                pad.assign(bs, 0);
                std::memcpy(pad.data(), cache.data() + b, m);
                sha256(pad.data(), bs, d);
            }
            if (std::memcmp(d, hashes.data() + blk * 32, 32) != 0)
                fail("RomFS block %llu fails its SHA-256 check (wrong title key, wrong base/update pair, or a damaged NSP)",
                     (unsigned long long)blk);
        }
    }
    void read(uint64_t pos, void* dst, size_t n) {
        if (pos + n > size) fail("read past the end of the RomFS");
        uint8_t* out = static_cast<uint8_t*>(dst);
        while (n) {
            if (c_start == ~0ull || pos < c_start || pos >= c_start + cache.size()) load(pos);
            size_t in = size_t(pos - c_start), m = std::min(n, cache.size() - in);
            std::memcpy(out, cache.data() + in, m);
            out += m, pos += m, n -= m;
        }
    }
};

// Loads and checks the hash levels; returns the data level (the RomFS image) ready for verified reads.
std::unique_ptr<VerifiedLevel> open_ivfc(Storage* sec, const uint8_t* fs, const std::string& what) {
    const uint8_t* iv = fs + 0x8;
    if (rd32(iv) != 0x43465649) fail("%s: RomFS section without an IVFC header", what.c_str());
    uint32_t levels = rd32(iv + 0xC);
    if (levels < 2 || levels > 7) fail("%s: IVFC with %u levels", what.c_str(), levels);
    uint32_t nl = levels - 1;  // stored levels; the last one is the data
    std::vector<uint8_t> prev(iv + 0xC0, iv + 0xC0 + 0x20);  // master hash
    std::vector<uint8_t> cur;
    for (uint32_t l = 0; l + 1 < nl; l++) {
        const uint8_t* lv = iv + 0x10 + 0x18 * l;
        uint64_t off = rd64(lv), size = rd64(lv + 8);
        uint32_t bs = 1u << rd32(lv + 16);
        if (size > (256ull << 20)) fail("%s: IVFC level %u too large", what.c_str(), l);
        cur.assign(size, 0);
        sec->read(off, cur.data(), cur.size());
        std::vector<uint8_t> blk(bs);
        for (uint64_t b = 0; b * bs < size; b++) {
            size_t m = size_t(std::min<uint64_t>(bs, size - b * bs));
            std::fill(blk.begin(), blk.end(), 0);
            std::memcpy(blk.data(), cur.data() + b * bs, m);
            uint8_t d[32];
            sha256(blk.data(), bs, d);
            if ((b + 1) * 32 > prev.size() || std::memcmp(d, prev.data() + b * 32, 32) != 0)
                fail("%s: IVFC level %u fails its hash check (wrong key?)", what.c_str(), l);
        }
        prev.swap(cur);
    }
    const uint8_t* lv = iv + 0x10 + 0x18 * (nl - 1);
    auto d = std::make_unique<VerifiedLevel>();
    d->st = sec;
    d->off = rd64(lv);
    d->size = rd64(lv + 8);
    d->bs = 1u << rd32(lv + 16);
    d->hashes = std::move(prev);
    if (d->off + d->size > sec->size) fail("%s: RomFS lies outside its section", what.c_str());
    return d;
}

// ---- LZ4 block ----

bool lz4_decompress(const uint8_t* src, size_t n, uint8_t* dst, size_t cap) {
    const uint8_t *ip = src, *iend = src + n;
    uint8_t *op = dst, *oend = dst + cap;
    while (ip < iend) {
        uint8_t token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (ip >= iend) return false;
                b = *ip++;
                lit += b;
            } while (b == 255);
        }
        if (size_t(iend - ip) < lit || size_t(oend - op) < lit) return false;
        std::memcpy(op, ip, lit);
        op += lit, ip += lit;
        if (ip >= iend) break;  // the last sequence has only literals
        if (iend - ip < 2) return false;
        size_t offset = size_t(ip[0] | ip[1] << 8);
        ip += 2;
        if (offset == 0 || offset > size_t(op - dst)) return false;
        size_t ml = token & 15;
        if (ml == 15) {
            uint8_t b;
            do {
                if (ip >= iend) return false;
                b = *ip++;
                ml += b;
            } while (b == 255);
        }
        ml += 4;
        if (size_t(oend - op) < ml) return false;
        const uint8_t* m = op - offset;
        for (size_t k = 0; k < ml; k++) op[k] = m[k];
        op += ml;
    }
    return op == oend;
}

// NSO -> flat text|ro|data image (tools/nso2bin.py).
std::vector<uint8_t> nso_to_flat(const std::vector<uint8_t>& nso, const std::string& name) {
    if (nso.size() < 0x100 || std::memcmp(nso.data(), "NSO0", 4) != 0) fail("%s: not an NSO", name.c_str());
    uint32_t flags = rd32(nso.data() + 0xC);
    uint64_t end = 0;
    for (int i = 0; i < 3; i++) {
        const uint8_t* s = nso.data() + 0x10 + 0x10 * i;
        end = std::max<uint64_t>(end, uint64_t(rd32(s + 4)) + rd32(s + 8));
    }
    if (end > (1ull << 30)) fail("%s: implausible NSO size", name.c_str());
    std::vector<uint8_t> img(end, 0);
    for (int i = 0; i < 3; i++) {
        const uint8_t* s = nso.data() + 0x10 + 0x10 * i;
        uint32_t foff = rd32(s), moff = rd32(s + 4), size = rd32(s + 8);
        uint32_t csize = rd32(nso.data() + 0x60 + 4 * i);
        if (uint64_t(foff) + csize > nso.size()) fail("%s: segment %d outside the file", name.c_str(), i);
        if (flags & (1u << i)) {
            if (!lz4_decompress(nso.data() + foff, csize, img.data() + moff, size)) fail("%s: segment %d does not decompress", name.c_str(), i);
        } else {
            if (csize != size) fail("%s: segment %d size mismatch", name.c_str(), i);
            std::memcpy(img.data() + moff, nso.data() + foff, size);
        }
        if (flags & (8u << i)) {
            uint8_t d[32];
            sha256(img.data() + moff, size, d);
            if (std::memcmp(d, nso.data() + 0xA0 + 0x20 * i, 32) != 0) fail("%s: segment %d fails its SHA-256 check", name.c_str(), i);
        }
    }
    return img;
}

// ---- containers: PFS0 (NSP, 0x18-byte entries) and HFS0 (XCI, 0x40-byte entries) ----

struct ContEntry {
    std::string name;
    uint64_t off, size;  // absolute in the storage that was read
};

// The file table of the PFS0/HFS0 at `base` of `st`.
std::vector<ContEntry> read_pfs(Storage* st, uint64_t base, bool hfs, const std::string& label) {
    const char* magic = hfs ? "HFS0" : "PFS0";
    const size_t esz = hfs ? 0x40 : 0x18;
    uint8_t h[16];
    if (base + 16 > st->size) fail("%s: too small to be a %s", label.c_str(), magic);
    st->read(base, h, 16);
    if (std::memcmp(h, magic, 4) != 0) fail("%s: not an %s (no %s header)", label.c_str(), hfs ? "HFS0 partition" : "NSP (not an XCI? gamecard images need the .xci extension)", magic);
    uint32_t count = rd32(h + 4), strsz = rd32(h + 8);
    if ((count == 0 && !hfs) || count > 4096 || strsz > (1u << 20)) fail("%s: implausible %s header", label.c_str(), magic);
    std::vector<uint8_t> tab(size_t(count) * esz + strsz);
    if (base + 16 + tab.size() > st->size) fail("%s: truncated %s header", label.c_str(), magic);
    st->read(base + 16, tab.data(), tab.size());
    uint64_t data = base + 16 + tab.size();
    std::vector<ContEntry> out;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = tab.data() + i * esz;
        uint32_t so = rd32(e + 16);
        if (so >= strsz) fail("%s: bad name in the %s", label.c_str(), magic);
        const char* nm = reinterpret_cast<const char*>(tab.data() + count * esz + so);
        out.push_back({std::string(nm, strnlen(nm, strsz - so)), data + rd64(e), rd64(e + 8)});
        if (out.back().off + out.back().size > st->size) fail("%s: %s entry %s lies outside the file (truncated?)", label.c_str(), magic, out.back().name.c_str());
    }
    return out;
}

// The files of a gamecard image that can hold the game: every file of the "secure" and "normal" partitions.
std::vector<ContEntry> read_xci(Storage* st, const std::string& label, std::string* parts_log) {
    uint8_t h[0x148];
    if (st->size < 0x1000) fail("%s: too small to be an XCI", label.c_str());
    st->read(0, h, sizeof h);
    if (std::memcmp(h + 0x100, "HEAD", 4) != 0)
        fail("%s: not an XCI (no HEAD at 0x100); a split image needs all its parts (.xc0 .xc1 ...) in the folder", label.c_str());
    uint64_t root = rd64(h + 0x130), root_hdr = rd64(h + 0x138);
    if (root < 0x200 || root > st->size) fail("%s: bad root partition offset in the XCI header", label.c_str());
    std::vector<ContEntry> parts = read_pfs(st, root, true, label);
    if (root_hdr && 16 + root_hdr > st->size) fail("%s: bad root partition header size", label.c_str());
    std::vector<ContEntry> out;
    bool secure = false;
    for (const ContEntry& p : parts) {
        std::string pn = lower(p.name);
        if (parts_log) *parts_log += (parts_log->empty() ? "" : ", ") + p.name;
        if (pn != "secure" && pn != "normal") continue;  // update: firmware; logo: cartridge art
        secure |= pn == "secure";
        std::vector<ContEntry> files = read_pfs(st, p.off, true, label + "/" + p.name);
        out.insert(out.end(), files.begin(), files.end());
    }
    if (!secure) fail("%s: the XCI has no secure partition (damaged or not a game card image)", label.c_str());
    return out;
}

// ---- output ----

void mkdirs(const std::string& path) {
    std::string cur;
    size_t pos = 0;
    while (pos != std::string::npos) {
        pos = path.find('/', pos + 1);
        cur = path.substr(0, pos);
        if (cur.empty()) continue;
        if (mkdir(cur.c_str(), 0775) != 0 && errno != EEXIST) fail("cannot create folder %s (%s)", cur.c_str(), strerror(errno));
    }
}

struct Writer {
    std::string final_path, part;
    int fd = -1;
    uint32_t crc = 0;
    uint64_t written = 0;
    explicit Writer(const std::string& path) : final_path(path), part(path + ".part") {
        fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
        if (fd < 0) fail("cannot write %s (%s)", part.c_str(), strerror(errno));
    }
    void write(const void* data, size_t n) {
        crc = crc32_update(crc, data, n);
        const uint8_t* p = static_cast<const uint8_t*>(data);
        while (n) {
            ssize_t w = ::write(fd, p, n);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) fail("write error on %s (%s)", final_path.c_str(), w < 0 ? strerror(errno) : "disk full?");
            p += w, n -= size_t(w), written += uint64_t(w);
        }
    }
    void commit() {
        if (fdatasync(fd) != 0 && errno != EINVAL && errno != EROFS) fail("sync of %s failed (%s)", final_path.c_str(), strerror(errno));
        ::close(fd);
        fd = -1;
        if (rename(part.c_str(), final_path.c_str()) != 0) fail("cannot rename %s (%s)", part.c_str(), strerror(errno));
    }
    ~Writer() {
        if (fd >= 0) {
            ::close(fd);
            unlink(part.c_str());
        }
    }
};

uint64_t file_size(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? uint64_t(st.st_size) : ~0ull;
}

std::string read_small(Storage& s, uint64_t max) {
    if (s.size > max) fail("file too large to be a key file");
    std::string t(s.size, '\0');
    s.read(0, t.data(), t.size());
    return t;
}

struct Journal {  // <out>/.import/journal.tsv: "<kind>\t<size>\t<crc32>\t<path>" per finished output
    std::string path;
    std::map<std::string, std::pair<uint64_t, uint32_t>> done;
    FILE* f = nullptr;
    void open(const std::string& p, bool reset) {
        path = p;
        if (!reset) {
            std::ifstream in(p);
            std::string line;
            while (std::getline(in, line)) {
                std::istringstream ls(line);
                std::string kind, size, crc, rel;
                if (std::getline(ls, kind, '\t') && std::getline(ls, size, '\t') && std::getline(ls, crc, '\t') && std::getline(ls, rel))
                    done[rel] = {strtoull(size.c_str(), nullptr, 10), uint32_t(strtoul(crc.c_str(), nullptr, 16))};
            }
        }
        f = fopen(p.c_str(), reset ? "w" : "a");
        if (!f) fail("cannot write %s (%s)", p.c_str(), strerror(errno));
    }
    void add(const char* kind, const std::string& rel, uint64_t size, uint32_t crc) {
        fprintf(f, "%s\t%llu\t%08x\t%s\n", kind, (unsigned long long)size, crc, rel.c_str());
        fflush(f);
        done[rel] = {size, crc};
    }
    ~Journal() {
        if (f) fclose(f);
    }
};

struct RomfsFile {
    std::string path;
    uint64_t off, size;
};

void walk_romfs(const std::vector<uint8_t>& dirs, const std::vector<uint8_t>& files, uint32_t dir, const std::string& prefix,
                std::vector<RomfsFile>& out, std::vector<std::string>& out_dirs, int depth) {
    if (depth > 64) fail("RomFS: directory tree too deep");
    if (size_t(dir) + 0x18 > dirs.size()) fail("RomFS: bad directory entry");
    const uint8_t* d = dirs.data() + dir;
    uint32_t child = rd32(d + 8), file = rd32(d + 12);
    while (file != 0xFFFFFFFF) {
        if (size_t(file) + 0x20 > files.size()) fail("RomFS: bad file entry");
        const uint8_t* f = files.data() + file;
        uint32_t nlen = rd32(f + 0x1C);
        if (size_t(file) + 0x20 + nlen > files.size()) fail("RomFS: bad file name");
        std::string name(reinterpret_cast<const char*>(f + 0x20), nlen);
        if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos) fail("RomFS: unsafe file name");
        out.push_back({prefix + name, rd64(f + 8), rd64(f + 16)});
        file = rd32(f + 4);
    }
    while (child != 0xFFFFFFFF) {
        if (size_t(child) + 0x18 > dirs.size()) fail("RomFS: bad directory entry");
        const uint8_t* c = dirs.data() + child;
        uint32_t nlen = rd32(c + 0x14);
        if (size_t(child) + 0x18 + nlen > dirs.size()) fail("RomFS: bad directory name");
        std::string name(reinterpret_cast<const char*>(c + 0x18), nlen);
        if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos) fail("RomFS: unsafe folder name");
        out_dirs.push_back(prefix + name);
        walk_romfs(dirs, files, child, prefix + name + "/", out, out_dirs, depth + 1);
        child = rd32(c + 4);
    }
}

struct Run {
    const Options& opt;
    std::chrono::steady_clock::time_point last_report{};
    Progress pr;
    explicit Run(const Options& o) : opt(o) {}
    void log(const char* fmt, ...) {
        if (!opt.on_log) return;
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        opt.on_log(buf);
    }
    void check_cancel() {
        if (opt.cancel && opt.cancel->load()) throw Cancelled{};
    }
    void report(bool force = false) {
        check_cancel();
        if (!opt.on_progress) return;
        auto now = std::chrono::steady_clock::now();
        if (!force && now - last_report < std::chrono::milliseconds(200)) return;
        last_report = now;
        opt.on_progress(pr);
    }
};

void check_space(const std::string& dir, uint64_t need) {
    struct statvfs vs{};
    if (statvfs(dir.c_str(), &vs) != 0) return;
    uint64_t free_b = uint64_t(vs.f_bavail) * vs.f_frsize;
    uint64_t margin = 64ull << 20;
    if (free_b < need + margin)
        fail("not enough free space: %.2f GB needed, %.2f GB free on the data folder's storage", double(need + margin) / 1e9,
             double(free_b) / 1e9);
}

int verify_outputs(Run& run, const std::string& out, std::string& error) {
    std::ifstream in(out + "/.import/index.tsv");
    if (!in) {
        error = "nothing to verify: no finished import in " + out;
        return 2;
    }
    struct E {
        std::string rel;
        uint64_t size;
        uint32_t crc;
    };
    std::vector<E> list;
    uint64_t total = 0;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string kind, size, crc, rel;
        if (std::getline(ls, kind, '\t') && std::getline(ls, size, '\t') && std::getline(ls, crc, '\t') && std::getline(ls, rel)) {
            list.push_back({rel, strtoull(size.c_str(), nullptr, 10), uint32_t(strtoul(crc.c_str(), nullptr, 16))});
            total += list.back().size;
        }
    }
    run.pr = {"verify", 0, total, ""};
    std::vector<uint8_t> buf(kChunk);
    size_t bad = 0;
    std::string first_bad;
    for (const E& e : list) {
        run.pr.item = e.rel;
        run.report();
        std::string p = out + "/" + e.rel;
        int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
        uint32_t crc = 0;
        uint64_t got = 0;
        if (fd >= 0) {
            ssize_t r;
            while ((r = ::read(fd, buf.data(), buf.size())) > 0) {
                crc = crc32_update(crc, buf.data(), size_t(r));
                got += uint64_t(r);
                run.pr.done += uint64_t(r);
                run.report();
            }
            ::close(fd);
        }
        if (fd < 0 || got != e.size || crc != e.crc) {
            if (!bad++) first_bad = e.rel;
        }
    }
    run.pr.phase = "done";
    run.report(true);
    if (bad) {
        error = std::to_string(bad) + " of " + std::to_string(list.size()) + " files differ from the import (first: " + first_bad +
                "); run the import again to repair them";
        return 3;
    }
    run.log("verify: %zu files, %.2f GB, all match", list.size(), double(total) / 1e9);
    return 0;
}

int import_main(Run& run, const std::vector<InputFile>& inputs, std::string& error) {
    const Options& opt = run.opt;
    if (!crypto_self_test()) fail("internal error: crypto self-test failed");
    if (opt.out_dir.empty()) fail("no output folder");
    mkdirs(opt.out_dir + "/.import");
    if (opt.verify_only) return verify_outputs(run, opt.out_dir, error);

    // inputs
    Keys keys;
    struct Container {
        std::string name;
        Storage* st;
        bool xci;
    };
    std::vector<std::unique_ptr<Storage>> owned_files;
    std::vector<Container> conts;
    std::map<std::string, std::map<int, Storage*>> split;  // stem -> part number -> file
    bool have_prod = false;
    for (const InputFile& in : inputs) {
        std::string n = lower(in.name);
        if (n == "prod.keys" || n == "keys.txt" || n == "keys.dat") {
            FdStorage s(in.fd, in.name);
            keys.load_prod(read_small(s, 1 << 20));
            have_prod = true;
        } else if (n == "title.keys") {
            FdStorage s(in.fd, in.name);
            keys.load_title(read_small(s, 4 << 20));
        } else if (ends_with(n, ".nsp") || ends_with(n, ".xci")) {
            owned_files.push_back(std::make_unique<FdStorage>(in.fd, in.name));
            conts.push_back({in.name, owned_files.back().get(), ends_with(n, ".xci")});
        } else {
            // split XCI part: x.xc0 x.xc1 ... or x.xci.00 x.xci.01 ...
            size_t d = n.size();
            while (d > 0 && isdigit(uint8_t(n[d - 1]))) d--;
            size_t cut = std::string::npos;
            if (d < n.size() && d >= 3 && n.compare(d - 3, 3, ".xc") == 0 && n.size() - d == 1)
                cut = d - 3;
            else if (d < n.size() && d >= 5 && n.compare(d - 5, 5, ".xci.") == 0)
                cut = d - 5;
            if (cut != std::string::npos) {
                owned_files.push_back(std::make_unique<FdStorage>(in.fd, in.name));
                split[in.name.substr(0, cut)][int(strtol(n.c_str() + d, nullptr, 10))] = owned_files.back().get();
            }
        }
    }
    for (auto& [stem, parts] : split) {
        std::vector<Storage*> list;
        if (parts.begin()->first != 0) fail("%s: the first part of the split XCI (.xc0 / .xci.00) is missing", stem.c_str());
        int expect = 0;
        for (auto& [idx, st] : parts) {
            if (idx != expect++) fail("%s: a part of the split XCI is missing (found %zu parts, numbering has a gap)", stem.c_str(), parts.size());
            list.push_back(st);
        }
        owned_files.push_back(std::make_unique<CatStorage>(std::move(list)));
        conts.push_back({stem + ".xci", owned_files.back().get(), true});
        run.log("split XCI %s: %zu parts joined", stem.c_str(), parts.size());
    }
    if (!have_prod) fail("prod.keys not found in the chosen folder");
    if (conts.empty()) fail("no .nsp or .xci file in the chosen folder");
    uint8_t hk[32];
    if (!keys.header_key(hk)) fail("prod.keys has no header_key");
    run.log("keys: %zu named keys, %zu title keys (values are never shown)", keys.named.size(), keys.title.size());

    // scan the containers: tickets first (any container's ticket can serve any NCA), then the NCAs
    run.pr = {"scan", 0, conts.size(), ""};
    struct Entry {
        Storage* nsp;
        std::string cont;
        std::string name;
        uint64_t off, size;
    };
    std::vector<Entry> entries;
    std::map<std::string, Ticket> tickets;
    for (const Container& c : conts) {
        std::string parts;
        std::vector<ContEntry> files;
        if (c.xci) {
            files = read_xci(c.st, c.name, &parts);
            run.log("%s: gamecard image, partitions %s; %zu files in secure/normal", c.name.c_str(), parts.c_str(), files.size());
        } else {
            files = read_pfs(c.st, 0, false, c.name);
        }
        for (const ContEntry& f : files) {
            entries.push_back({c.st, c.name, f.name, f.off, f.size});
            if (!ends_with(lower(f.name), ".tik") || f.size < 0x2C0) continue;
            uint8_t t[0x2C0];
            c.st->read(f.off, t, sizeof t);
            if (rd32(t) != 0x10004) continue;  // RSA-2048 SHA-256: the ticket data at 0x140
            Ticket tk;
            std::memcpy(tk.enc_key, t + 0x180, 16);
            tk.type = t[0x281];
            tickets[hex(t + 0x2A0, 16)] = tk;
            secure_zero(t, sizeof t);
        }
        run.pr.done++;
        run.report();
    }
    std::vector<std::unique_ptr<Nca>> ncas;
    for (auto& e : entries) {
        std::string n = lower(e.name);
        if (!ends_with(n, ".nca") && !ends_with(n, ".ncz")) continue;
        if (ends_with(n, ".ncz")) fail("%s: compressed NCZ content is not supported; use an uncompressed NSP/XCI", e.name.c_str());
        auto nca = open_nca(e.nsp, e.off, e.size, e.name, keys, hk, tickets);
        if (nca && nca->content_type == 0) {
            nca->src = e.cont;
            run.log("found program %s in %s: title %s%s%s", e.name.c_str(), e.cont.c_str(), tid_str(nca->title_id).c_str(),
                    nca->key_ok ? "" : " (no key: ", nca->key_ok ? "" : (nca->key_problem + ")").c_str());
            for (int i = 0; i < 4; i++)
                if (nca->sec[i].present)
                    run.log("  section %d: %s, encryption %d%s%s, %.1f MiB", i, nca->sec[i].partition() ? "PFS0" : "RomFS",
                            nca->sec[i].crypt(), nca->sec[i].is_sparse() ? ", sparse" : "", nca->sec[i].is_patch() ? ", patch" : "",
                            double(nca->sec[i].size) / 1048576.0);
            ncas.push_back(std::move(nca));
        }
    }
    secure_zero(hk, sizeof hk);

    // choose the title: the base program and at most one update program
    std::set<uint64_t> titles;
    for (auto& n : ncas) titles.insert(n->title_id & ~0xFFFull);
    uint64_t app = opt.title_id & ~0xFFFull;
    if (!app) {
        if (titles.size() != 1) {
            std::string list;
            for (uint64_t t : titles) list += " " + tid_str(t);
            fail(titles.empty() ? "no game program found in the NSP/XCI files" : "several games in the folder (%s); choose one", list.c_str());
        }
        app = *titles.begin();
    } else if (!titles.count(app)) {
        std::string list;
        for (uint64_t t : titles) list += " " + tid_str(t);
        fail("the folder has no NSP of title %s (found:%s)", tid_str(app).c_str(), list.empty() ? " nothing" : list.c_str());
    }
    Nca *base = nullptr, *patch = nullptr;
    int base_romfs = -1, patch_romfs = -1;
    for (auto& n : ncas) {
        if ((n->title_id & ~0xFFFull) != app) continue;
        int romfs = -1;
        bool is_patch = false;
        for (int i = 0; i < 4; i++)
            if (n->sec[i].present && n->sec[i].partition() == 0) {
                romfs = i;
                is_patch |= n->sec[i].is_patch();
            }
        if (is_patch) {
            if (patch) fail("two updates for the same game in the folder (%s, %s): keep only one update", (patch->src + "/" + patch->name).c_str(), (n->src + "/" + n->name).c_str());
            patch = n.get(), patch_romfs = romfs;
        } else {
            if (base) fail("two base programs for the same game in the folder (%s, %s): keep only one", (base->src + "/" + base->name).c_str(), (n->src + "/" + n->name).c_str());
            base = n.get(), base_romfs = romfs;
        }
    }
    if (!base) fail("the base game of %s is missing: add its NSP or XCI (the update alone has only the changes)", tid_str(app).c_str());
    run.log("title %s: base %s in %s%s, update %s", tid_str(app).c_str(), base->name.c_str(), base->src.c_str(),
            base_romfs >= 0 && base->sec[base_romfs].is_sparse() ? " (sparse)" : "",
            patch ? (patch->name + " in " + patch->src).c_str() : "none");
    if (patch)
        run.log("update program %s (title %s)", patch->name.c_str(), tid_str(patch->title_id).c_str());
    else
        run.log("no update found (a patch NSP/XCI of %s in the folder would be applied): importing the base game as dumped",
                tid_str(app).c_str());

    // source identity: a different pair of NSPs restarts the import from scratch
    std::string ident = tid_str(app) + " " + base->src + "/" + base->name + " " + std::to_string(base->file->size) + " " +
                        (patch ? patch->src + "/" + patch->name + " " + std::to_string(patch->file->size) : std::string("-"));
    std::string root = opt.out_dir;
    std::string old_ident;
    {
        std::ifstream f(root + "/.import/source.txt");
        std::getline(f, old_ident);
    }
    bool reset = old_ident != ident;
    if (reset) {
        unlink((root + "/.import/index.tsv").c_str());
        unlink((root + "/.import/complete").c_str());
        std::ofstream f(root + "/.import/source.txt", std::ios::trunc);
        f << ident << "\n";
    }
    Journal journal;
    journal.open(root + "/.import/journal.tsv", reset);
    auto done_ok = [&](const std::string& rel, uint64_t size) {
        auto it = journal.done.find(rel);
        return it != journal.done.end() && it->second.first == size && file_size(root + "/" + rel) == size;
    };

    if (!patch && base_romfs >= 0 && base->sec[base_romfs].is_sparse())
        fail("the base game holds only part of the game (the rest comes with its update): add the update NSP or XCI "
             "to the folder");

    // ---- ExeFS: from the update if there is one ----
    Nca* exe_nca = patch ? patch : base;
    int exe_sec = -1;
    for (int i = 0; i < 4; i++)
        if (exe_nca->sec[i].present && exe_nca->sec[i].partition() == 1) {
            exe_sec = i;
            break;
        }
    if (exe_sec < 0) fail("%s has no ExeFS", exe_nca->name.c_str());
    Storage* exe = plain_section(*exe_nca, exe_sec);
    const uint8_t* efs = exe_nca->sec[exe_sec].fs;
    uint32_t pfs_bs = rd32(efs + 0x28);
    uint64_t ht_off = rd64(efs + 0x30), ht_size = rd64(efs + 0x38), pfs_off = rd64(efs + 0x40), pfs_size = rd64(efs + 0x48);
    if (!pfs_bs || ht_size > (64u << 20) || pfs_size > (1ull << 31)) fail("%s: implausible ExeFS layout", exe_nca->name.c_str());
    std::vector<uint8_t> ht(ht_size), pfs(pfs_size);
    exe->read(ht_off, ht.data(), ht.size());
    uint8_t d[32];
    sha256(ht.data(), ht.size(), d);
    if (std::memcmp(d, efs + 0x8, 32) != 0) fail("%s: ExeFS hash table check failed (wrong key?)", exe_nca->name.c_str());
    exe->read(pfs_off, pfs.data(), pfs.size());
    for (uint64_t b = 0; b * pfs_bs < pfs_size; b++) {
        size_t m = size_t(std::min<uint64_t>(pfs_bs, pfs_size - b * pfs_bs));
        sha256(pfs.data() + b * pfs_bs, m, d);
        if ((b + 1) * 32 > ht.size() || std::memcmp(d, ht.data() + b * 32, 32) != 0) fail("%s: ExeFS block %llu fails its check", exe_nca->name.c_str(), (unsigned long long)b);
    }
    if (std::memcmp(pfs.data(), "PFS0", 4) != 0) fail("%s: ExeFS is not a PFS0", exe_nca->name.c_str());
    {
        uint32_t count = rd32(pfs.data() + 4), strsz = rd32(pfs.data() + 8);
        uint64_t data0 = 16 + uint64_t(count) * 0x18 + strsz;
        if (data0 > pfs.size()) fail("ExeFS header truncated");
        mkdirs(root + "/analysis");
        if (opt.keep_exefs) mkdirs(root + "/exefs");
        uint64_t exe_total = 0;
        for (uint32_t i = 0; i < count; i++) exe_total += rd64(pfs.data() + 16 + i * 0x18 + 8);
        run.pr = {"exefs", 0, exe_total, ""};
        bool has_main = false;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t* e = pfs.data() + 16 + i * 0x18;
            uint64_t off = data0 + rd64(e), size = rd64(e + 8);
            uint32_t so = rd32(e + 16);
            if (off + size > pfs.size() || so >= strsz) fail("ExeFS entry %u out of range", i);
            const char* nm = reinterpret_cast<const char*>(pfs.data() + 16 + count * 0x18 + so);
            std::string name(nm, strnlen(nm, strsz - so));
            if (name.empty() || name.find('/') != std::string::npos || name[0] == '.') fail("ExeFS: unsafe file name");
            run.pr.item = name;
            run.report(true);
            std::vector<uint8_t> blob(pfs.begin() + long(off), pfs.begin() + long(off + size));
            std::string rel;
            std::vector<uint8_t> outb;
            if (size >= 4 && std::memcmp(blob.data(), "NSO0", 4) == 0) {
                if (name == "rtld") {  // the runtime does not load rtld (HLE): keep it out of analysis/
                    run.pr.done += size;
                    continue;
                }
                outb = nso_to_flat(blob, name);
                rel = "analysis/" + name + ".bin";
                if (opt.keep_exefs) {
                    Writer raw(root + "/exefs/" + name);
                    raw.write(blob.data(), blob.size());
                    raw.commit();
                }
                has_main |= name == "main";
            } else {
                outb = std::move(blob);
                rel = "analysis/" + name;
            }
            if (!done_ok(rel, outb.size()) || journal.done[rel].second != crc32_update(0, outb.data(), outb.size())) {
                Writer w(root + "/" + rel);
                w.write(outb.data(), outb.size());
                w.commit();
                journal.add("exefs", rel, outb.size(), w.crc);
            }
            run.log("exefs: %s (%llu bytes)", rel.c_str(), (unsigned long long)outb.size());
            run.pr.done += size;
            run.report();
        }
        if (!has_main) fail("the ExeFS has no main");
    }
    pfs.clear(), pfs.shrink_to_fit();

    // ---- RomFS ----
    if (base_romfs < 0) fail("%s has no RomFS section", base->name.c_str());
    Storage* base_sec = base->sec[base_romfs].is_sparse() ? sparse_section(*base, base_romfs) : plain_section(*base, base_romfs);
    Storage* romfs_sec;
    const uint8_t* romfs_fs;
    std::string romfs_name;
    if (patch) {
        if (patch_romfs < 0) fail("%s has no patch RomFS", patch->name.c_str());
        romfs_sec = patched_section(*patch, patch_romfs, base_sec);
        romfs_fs = patch->sec[patch_romfs].fs;
        romfs_name = patch->name;
    } else {
        if (base->sec[base_romfs].is_sparse()) fail("the base game holds only part of the game; add its update NSP or XCI to the folder");
        romfs_sec = base_sec;
        romfs_fs = base->sec[base_romfs].fs;
        romfs_name = base->name;
    }
    auto rom = open_ivfc(romfs_sec, romfs_fs, romfs_name);
    uint8_t rh[0x50];
    rom->read(0, rh, sizeof rh);
    if (rd64(rh) != 0x50) fail("RomFS header not recognised (wrong base/update pair?)");
    uint64_t dir_off = rd64(rh + 0x18), dir_size = rd64(rh + 0x20), file_off = rd64(rh + 0x38), file_size_ = rd64(rh + 0x40),
             data_off = rd64(rh + 0x48);
    if (dir_size > (64u << 20) || file_size_ > (64u << 20)) fail("RomFS tables implausibly large");
    std::vector<uint8_t> dirs(dir_size), files(file_size_);
    rom->read(dir_off, dirs.data(), dirs.size());
    rom->read(file_off, files.data(), files.size());
    std::vector<RomfsFile> list;
    std::vector<std::string> dir_list;
    walk_romfs(dirs, files, 0, "", list, dir_list, 0);
    std::sort(list.begin(), list.end(), [](const RomfsFile& a, const RomfsFile& b) { return a.off < b.off; });
    uint64_t total = 0, remaining = 0;
    for (auto& f : list) {
        if (data_off + f.off + f.size > rom->size) fail("RomFS file %s lies outside the image", f.path.c_str());
        total += f.size;
        if (!done_ok("extract/romfs/" + f.path, f.size)) remaining += f.size;
    }
    run.log("romfs: %zu files, %zu folders, %.2f GB (%.2f GB still to write)", list.size(), dir_list.size(), double(total) / 1e9,
            double(remaining) / 1e9);
    check_space(root, remaining);
    mkdirs(root + "/extract/romfs");
    for (auto& dname : dir_list) mkdirs(root + "/extract/romfs/" + dname);
    run.pr = {"romfs", 0, total, ""};
    run.report(true);
    std::vector<uint8_t> buf(kChunk);
    for (auto& f : list) {
        std::string rel = "extract/romfs/" + f.path;
        run.pr.item = f.path;
        if (done_ok(rel, f.size)) {
            run.pr.done += f.size;
            run.report();
            continue;
        }
        Writer w(root + "/" + rel);
        for (uint64_t pos = 0; pos < f.size;) {
            size_t m = size_t(std::min<uint64_t>(buf.size(), f.size - pos));
            rom->read(data_off + f.off + pos, buf.data(), m);
            w.write(buf.data(), m);
            pos += m;
            run.pr.done += m;
            run.report();
        }
        w.commit();
        journal.add("romfs", rel, f.size, w.crc);
    }

    // ---- index + completion marker ----
    {
        std::ofstream idx(root + "/.import/index.tsv.part", std::ios::trunc);
        for (auto& [rel, sc] : journal.done) {
            bool keep = rel.rfind("analysis/", 0) == 0;
            idx << (keep ? "exefs" : "romfs") << "\t" << sc.first << "\t" << std::hex;
            char c[16];
            snprintf(c, sizeof c, "%08x", sc.second);
            idx << c << std::dec << "\t" << rel << "\n";
        }
        idx.close();
        if (!idx || rename((root + "/.import/index.tsv.part").c_str(), (root + "/.import/index.tsv").c_str()) != 0)
            fail("cannot write the import index");
        std::ofstream mark(root + "/.import/complete", std::ios::trunc);
        mark << ident << "\n" << list.size() << " romfs files, " << total << " bytes\n";
    }
    run.pr.phase = "done";
    run.report(true);
    run.log("import complete: %zu RomFS files, %.2f GB", list.size(), double(total) / 1e9);
    return 0;
}

}  // namespace

int run_import(const std::vector<InputFile>& inputs, const Options& opt, std::string& error) {
    Run run(opt);
    try {
        return import_main(run, inputs, error);
    } catch (const Cancelled&) {
        error = "cancelled (run the import again to continue where it stopped)";
        return 4;
    } catch (const Fail& e) {
        error = e.what();
        return 1;
    } catch (const std::bad_alloc&) {
        error = "out of memory";
        return 1;
    }
}

}  // namespace nxi
