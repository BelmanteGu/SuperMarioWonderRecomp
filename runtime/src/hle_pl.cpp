// nn::pl (shared system fonts) at the API level.
//
// On the console the pl sysmodule loads the system fonts (system archives 0100000000000810-814) into shared memory
// in BFTTF form: an 8-byte header {magic, size} and the TrueType data, both XORed with a key. The game hands that
// to nn::fontll::ScalableFontEngineHelper::Decode (sdk+3093c0 in SDK 16), which accepts the data already decoded
// when the first word is the plain magic 0x18029A7F, and then only skips the header.
//
// So any TrueType font works: <data root>/fonts/shared_<type>.ttf (0 standard, 1 simplified Chinese, 2 its
// extension, 3 traditional Chinese, 4 Korean, 5 Nintendo extension), else shared.ttf, else the host's own
// (NX_SHARED_FONT, or a system font on macOS, Android and Windows). The fonts on our own firmware dump can go there once decoded.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

namespace {

struct Font {
    uint64_t addr = 0, size = 0;  // header + data, in guest memory
};

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> v;
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return v;
    plat::fseek64(f, 0, SEEK_END);
    int64_t n = plat::ftell64(f);
    plat::fseek64(f, 0, SEEK_SET);
    if (n > 0) {
        v.resize(size_t(n));
        v.resize(std::fread(v.data(), 1, v.size(), f));
    }
    std::fclose(f);
    return v;
}

const Font& font(uint32_t type) {
    static std::mutex lock;
    static Font fonts[6];
    std::lock_guard lk(lock);
    Font& f = fonts[type < 6 ? type : 0];
    if (f.addr) return f;
    std::string root = plat::data_root() + "/fonts/";
    std::vector<std::string> candidates = {root + "shared_" + std::to_string(type) + ".ttf", root + "shared.ttf"};
    if (const char* e = std::getenv("NX_SHARED_FONT")) candidates.push_back(e);
#if defined(__APPLE__)
    candidates.push_back("/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
    candidates.push_back("/System/Library/Fonts/Supplemental/Arial.ttf");
#elif defined(__ANDROID__)
    candidates.push_back("/system/fonts/Roboto-Regular.ttf");
#elif defined(_WIN32)
    // Arial Unicode (Office installs it) covers the most; else Arial, as on the Mac.
    candidates.push_back("C:/Windows/Fonts/arialuni.ttf");
    candidates.push_back("C:/Windows/Fonts/arial.ttf");
#elif defined(__linux__)
    // Liberation Sans has Arial's metrics; else the common distribution fonts.
    candidates.push_back("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
#endif
    std::vector<uint8_t> ttf;
    for (const auto& path : candidates) {
        ttf = read_file(path);
        if (!ttf.empty()) {
            log("pl: shared font %u <- %s (%zu bytes)", type, path.c_str(), ttf.size());
            break;
        }
    }
    if (ttf.empty()) log("pl: no font for shared font %u (put one in %s)", type, root.c_str());
    f.size = ttf.size() + 8;
    f.addr = kernel_alloc(f.size);
    st<uint32_t>(f.addr, 0x18029A7F);  // plain magic: "already decoded"
    st<uint32_t>(f.addr + 4, uint32_t(ttf.size()));
    if (!ttf.empty()) std::memcpy(reinterpret_cast<void*>(f.addr + 8), ttf.data(), ttf.size());
    return f;
}

void GetSharedFontLoadState(CPU& c) { c.x[0] = 1; }  // SharedFontLoadState_Loaded
void GetSharedFontAddress(CPU& c) { c.x[0] = font(uint32_t(c.x[0])).addr; }
void GetSharedFontSize(CPU& c) { c.x[0] = font(uint32_t(c.x[0])).size; }

}  // namespace

NX_HLE("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", GetSharedFontLoadState);
NX_HLE("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", GetSharedFontAddress);
NX_HLE("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", GetSharedFontSize);

}  // namespace nx
