#include <cstdlib>
// Android host (NativeActivity): memory (mmap at fixed addresses), pthreads, the app's window (ANativeWindow),
// game controllers + touch, AAudio, the Vulkan loader and android surface, a signal crash handler and
// android_main. Game data lives in the app's external files dir (see docs/ANDROID.md):
//   <files>/analysis/{main,sdk}.bin   module images (tools/nso2bin.py)
//   <files>/extract/romfs/...         the RomFS
//   <files>/user/...                  saves
//   <files>/config.txt                optional: resolution=WxH
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <malloc.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"
#include "platform_android.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace nx {
void boot();
}

namespace nx::plat {

namespace {
android_app* g_app = nullptr;
std::string g_root;
std::atomic<ANativeWindow*> g_window{nullptr};
std::atomic<uint64_t> g_window_gen{0};  // bumped for every new window
std::atomic<bool> g_visible{false};
}  // namespace

// ---- memory ---------------------------------------------------------------------------------------------------

bool reserve(uint64_t addr, uint64_t size) {
    void* p = mmap(reinterpret_cast<void*>(addr), size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (uint64_t(p) != addr) {  // old kernels ignore the flag and treat addr as a hint
        munmap(p, size);
        return false;
    }
    return true;
}

bool commit(uint64_t addr, uint64_t size) {
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    return mprotect(reinterpret_cast<void*>(a), e - a, PROT_READ | PROT_WRITE) == 0;
}

void decommit(uint64_t addr, uint64_t size) {
    madvise(reinterpret_cast<void*>(addr), size, MADV_DONTNEED);
    mprotect(reinterpret_cast<void*>(addr), size, PROT_NONE);
}

// ---- threads and time -----------------------------------------------------------------------------------------

struct ThreadStart {
    ThreadFn fn;
    void* arg;
};

static void* thread_entry(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    s.fn(s.arg);
    return nullptr;
}

void start_thread(ThreadFn fn, void* arg, size_t stack_size) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack_size);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &a, thread_entry, new ThreadStart{fn, arg});
    pthread_attr_destroy(&a);
}

void exit_thread() { pthread_exit(nullptr); }
void exit_process(int code) { _exit(code); }

void sleep_ns(int64_t ns) {
    timespec ts{time_t(ns / 1000000000), long(ns % 1000000000)};
    nanosleep(&ts, nullptr);
}

// Wonder's job system yields after every job item and in its barriers (svc YieldThread; DEBUG.md #44). NX_YIELD=spin:
// a short pause on the core instead of the scheduler (the main thread made ~52 voluntary context switches per frame
// in course, PHASE3.md B5); unset or sched: sched_yield, as before.
void yield() {
    static const bool spin = [] {
        const char* e = std::getenv("NX_YIELD");
        return e && std::strcmp(e, "spin") == 0;
    }();
    if (!spin) {
        sched_yield();
        return;
    }
    for (int k = 0; k < 64; k++) asm volatile("yield" ::: "memory");
}
void set_thread_name(const char* name) { pthread_setname_np(pthread_self(), name); }
int64_t current_thread_id() { return gettid(); }

// ---- sampling profiler (simpleperf is not always allowed on retail phones) ------------------------------------

namespace {
constexpr uint32_t kMaxSamples = 1u << 18;
constexpr int kFrames = 12;
struct Sample {
    int32_t tid;
    uint64_t guest_lr;
    uint64_t pc[kFrames];  // host pc, lr, then return addresses from the frame-pointer chain
};
std::atomic<uint32_t> g_nsamples{0};
Sample* g_samples = nullptr;
const int64_t* g_prof_tids = nullptr;
const uint64_t* const* g_prof_lr = nullptr;
size_t g_prof_count = 0;

void on_sample(int, siginfo_t*, void* uc) {
    uint32_t i = g_nsamples.fetch_add(1, std::memory_order_relaxed);
    if (i >= kMaxSamples) return;
    Sample& s = g_samples[i];
    const mcontext_t& m = static_cast<ucontext_t*>(uc)->uc_mcontext;
    s.tid = gettid();
    s.guest_lr = 0;
    for (size_t k = 0; k < g_prof_count; k++)
        if (g_prof_tids[k] == s.tid) s.guest_lr = *g_prof_lr[k];
    std::memset(s.pc, 0, sizeof s.pc);
    s.pc[0] = m.pc;
    s.pc[1] = m.regs[30];
    // Frame records (fp -> [prev fp, lr]) on this thread's stack only: between sp and sp + 256 MiB, going up.
    uint64_t fp = m.regs[29], lo = m.sp, hi = m.sp + (256ull << 20);
    for (int k = 2; k < kFrames && fp >= lo && fp < hi && (fp & 7) == 0; k++) {
        const uint64_t* rec = reinterpret_cast<const uint64_t*>(fp);
        s.pc[k] = rec[1];
        if (rec[0] <= fp) break;
        fp = rec[0];
    }
}
}  // namespace

bool profile(const int64_t* tids, const uint64_t* const* guest_lr, size_t count, int seconds, const std::string& path) {
    if (!g_samples) g_samples = new Sample[kMaxSamples];
    g_prof_tids = tids;
    g_prof_lr = guest_lr;
    g_prof_count = count;
    g_nsamples = 0;
    struct sigaction sa{};
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    std::vector<timer_t> timers;
    for (size_t k = 0; k < count; k++) {
        // The thread's CPU-time clock (Linux: MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)).
        clockid_t clock = clockid_t((~uint32_t(tids[k]) << 3) | 6);
        sigevent sev{};
        sev.sigev_notify = SIGEV_THREAD_ID;
        sev.sigev_signo = SIGPROF;
        sev.sigev_notify_thread_id = int(tids[k]);
        timer_t t;
        if (timer_create(clock, &sev, &t) != 0) continue;
        itimerspec its{{0, 1'000'000}, {0, 1'000'000}};
        timer_settime(t, 0, &its, nullptr);
        timers.push_back(t);
    }
    sleep_ns(int64_t(seconds) * 1'000'000'000);
    for (timer_t t : timers) timer_delete(t);
    sleep_ns(10'000'000);
    g_prof_count = 0;
    uint32_t n = std::min(g_nsamples.load(), kMaxSamples);
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    Dl_info self{};
    dladdr(reinterpret_cast<void*>(&on_sample), &self);
    for (uint32_t i = 0; i < n; i++) {
        const Sample& s = g_samples[i];
        std::fprintf(f, "%d %llx", s.tid, (unsigned long long)s.guest_lr);
        for (int k = 0; k < kFrames && s.pc[k]; k++) {
            Dl_info d{};
            if (dladdr(reinterpret_cast<void*>(s.pc[k]), &d) && d.dli_fbase) {
                const char* name = d.dli_fbase == self.dli_fbase ? "self" : d.dli_fname;
                const char* slash = std::strrchr(name, '/');
                std::fprintf(f, " %s:%llx", slash ? slash + 1 : name,
                             (unsigned long long)(s.pc[k] - uint64_t(d.dli_fbase)));
            } else {
                std::fprintf(f, " ?:%llx", (unsigned long long)s.pc[k]);
            }
        }
        std::fputc('\n', f);
    }
    std::fclose(f);
    return true;
}

uint64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// ---- files and output -----------------------------------------------------------------------------------------

FILE* fopen_utf8(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }
int64_t ftell64(FILE* f) { return ftello(f); }
int fseek64(FILE* f, int64_t off, int origin) { return fseeko(f, off, origin); }
uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size) {
    // A size that does not fit ssize_t (the game passes -1) reads nothing, as fread did (and the game expects).
    if (size > uint64_t(SSIZE_MAX)) return 0;
    int fd = fileno(f);
    uint64_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, static_cast<uint8_t*>(buf) + done, size_t(size - done), off + int64_t(done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += uint64_t(n);
    }
    return done;
}
bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

void log_line(const char* line) { __android_log_write(ANDROID_LOG_INFO, nx::game::kInfo.id, line); }

std::string data_root() { return g_root; }

std::string setting(const std::string& key) {
    std::ifstream f(g_root + "/config.txt");
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        auto eq = line.find('=');
        if (eq != std::string::npos && line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return {};
}

// ---- window ---------------------------------------------------------------------------------------------------

// The window handed out stays valid for good (we hold a reference): Android destroys the activity's window when
// the app goes to the background or the screen locks (APP_CMD_TERM_WINDOW), and until then we wait for one.
void* create_window() {
    ANativeWindow* w;
    while (!(w = g_window.load())) sleep_ns(10'000'000);
    ANativeWindow_acquire(w);
    return w;
}

void release_window(void* w) { ANativeWindow_release(static_cast<ANativeWindow*>(w)); }

uint64_t window_generation() { return g_window_gen; }

ANativeWindow* android_window() { return g_window.load(); }  // platform_android.h

bool window_minimized() { return !g_visible || !g_window; }

void screen_size(uint32_t& w, uint32_t& h) {
    ANativeWindow* win = static_cast<ANativeWindow*>(create_window());
    int32_t a = ANativeWindow_getWidth(win), b = ANativeWindow_getHeight(win);
    ANativeWindow_release(win);
    w = uint32_t(std::max(a, b));  // landscape
    h = uint32_t(std::min(a, b));
}

// The render size: resolution=WxH if set, else the screen's size times the render scale (render_scale=0.5..1, or
// the GPU's default), never taller than 1080 (the game's art is made for 720p/1080p; more only costs GPU time).
void window_size(uint32_t& w, uint32_t& h) {
    std::string r = setting("resolution");
    unsigned cw = 0, ch = 0;
    if (!r.empty() && std::sscanf(r.c_str(), "%ux%u", &cw, &ch) == 2 && cw && ch) {
        w = cw;
        h = ch;
        return;
    }
    uint32_t sw = 0, sh = 0;
    screen_size(sw, sh);
    std::string rs = setting("render_scale");
    float scale = rs.empty() ? default_render_scale() : std::strtof(rs.c_str(), nullptr);
    scale = std::clamp(scale, 0.25f, 1.0f);
    scale = std::min(scale, 1080.0f / float(sh));
    w = uint32_t(float(sw) * scale + 0.5f) & ~1u;
    h = uint32_t(float(sh) * scale + 0.5f) & ~1u;
}

// A GPU tier from the Vulkan device name (queried once, before the game starts its renderer): current Adreno 7xx
// and 8xx, Mali-G7xx and newer, Immortalis, Xclipse render at 1; Adreno 6xx at 0.85; anything older at 2/3
// (about 720p, the Switch's handheld resolution).
float default_render_scale() {
    static float scale = [] {
        auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(vulkan_get_instance_proc_addr());
        if (!gipa) return 2.0f / 3;
        auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        VkInstance inst = VK_NULL_HANDLE;
        if (!create || create(&ci, nullptr, &inst) != VK_SUCCESS) return 2.0f / 3;
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
        auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(inst, "vkGetPhysicalDeviceProperties"));
        auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
        uint32_t n = 1;
        VkPhysicalDevice pd = VK_NULL_HANDLE;
        float s = 2.0f / 3;
        if (enumerate(inst, &n, &pd) >= 0 && n && pd) {
            VkPhysicalDeviceProperties p{};
            props(pd, &p);
            std::string name = p.deviceName;
            int adreno = 0, mali = 0;
            std::sscanf(name.c_str() + (name.find("Adreno") != std::string::npos ? name.find("Adreno") : name.size()),
                        "Adreno (TM) %d", &adreno);
            if (auto m = name.find("Mali-G"); m != std::string::npos) std::sscanf(name.c_str() + m, "Mali-G%d", &mali);
            if (adreno >= 700 || mali >= 710 || name.find("Immortalis") != std::string::npos ||
                name.find("Xclipse") != std::string::npos)
                s = 1.0f;
            else if (adreno >= 610 || mali >= 76)
                s = 0.85f;
            nx::log("gpu: %s -> default render scale %.2f", p.deviceName, s);
        }
        destroy(inst, nullptr);
        return s;
    }();
    return scale;
}

// ---- Vulkan ---------------------------------------------------------------------------------------------------

void* vulkan_get_instance_proc_addr() {
    void* lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    return lib ? dlsym(lib, "vkGetInstanceProcAddr") : nullptr;
}

const char* vulkan_surface_extension() { return VK_KHR_ANDROID_SURFACE_EXTENSION_NAME; }

int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void* gipa) {
    auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa)(
        static_cast<VkInstance>(instance), "vkCreateAndroidSurfaceKHR"));
    VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    ci.window = static_cast<ANativeWindow*>(native_window);
    VkSurfaceKHR s = VK_NULL_HANDLE;
    VkResult r = create(static_cast<VkInstance>(instance), &ci, nullptr, &s);
    *surface = reinterpret_cast<uint64_t>(s);
    return r;
}

}  // namespace nx::plat

// ---- crash handler and entry ----------------------------------------------------------------------------------

namespace {

struct sigaction g_previous[32];  // the handlers before ours (debuggerd's)

void crash(int sig, siginfo_t* info, void* uc) {
    if ((sig == SIGSEGV || sig == SIGBUS) && nx::plat::write_watch_fault(info->si_addr)) return;  // write_watch.cpp
    nx::log("CRASH: signal %d, fault address %p", sig, info->si_addr);
    if (nx::self_thread()) {
        nx::CPU& c = nx::current_cpu();
        nx::log("  guest lr %s  sp %llx", nx::describe(c.x[30]).c_str(), (unsigned long long)c.x[31]);
        for (int k = 0; k < 32; k += 4)
            nx::log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k],
                    (unsigned long long)c.x[k + 1], (unsigned long long)c.x[k + 2], (unsigned long long)c.x[k + 3]);
        nx::backtrace(c);
        nx::log_recent_files();
    }
    // Hand the signal to the previous handler, Android's debuggerd: it logs the host backtrace (a tombstone).
    const struct sigaction& prev = g_previous[sig];
    if (prev.sa_flags & SA_SIGINFO) {
        if (prev.sa_sigaction) prev.sa_sigaction(sig, info, uc);
    } else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN && prev.sa_handler) {
        prev.sa_handler(sig);
    }
    _exit(128 + sig);
}

void game_thread(void*) {
    try {
        nx::boot();
    } catch (const nx::LongJmp& j) {
        nx::fatal("longjmp to buffer %llx with no setjmp frame", (unsigned long long)j.buf);
    } catch (const std::exception& e) {
        nx::fatal("host exception: %s", e.what());
    }
}

void request_immersive();  // below: runs on the UI thread

void on_cmd(android_app* app, int32_t cmd) {
    using namespace nx::plat;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            g_window = app->window;
            g_window_gen++;
            g_visible = true;
            request_immersive();
            break;
        case APP_CMD_TERM_WINDOW:  // the window goes away once this returns
            g_window = nullptr;
            g_visible = false;
            break;
        case APP_CMD_GAINED_FOCUS:
            g_visible = g_window != nullptr;
            request_immersive();  // bars shown by a swipe hide again
            break;
        case APP_CMD_LOST_FOCUS:
            break;
        case APP_CMD_PAUSE:  // home, app switcher, screen off: the game holds (no window), so does its sound
            audio_pause(true);
            break;
        case APP_CMD_RESUME:
            audio_pause(false);
            break;
    }
}

}  // namespace

// ---- immersive mode: the status and navigation bars hidden (a swipe from an edge shows them for a moment) --------
// Only the UI thread may touch the window's views; android_main runs on another. Our ANativeActivity_onCreate (on
// the UI thread) registers a pipe with the UI thread's looper: writing a byte to it runs the request there.

extern "C" void glue_ANativeActivity_onCreate(ANativeActivity* activity, void* saved, size_t size);

namespace {

ANativeActivity* g_activity = nullptr;
int g_ui_pipe[2] = {-1, -1};

void hide_system_bars(JNIEnv* env, jobject activity) {
    jclass act = env->GetObjectClass(activity);
    jobject window = env->CallObjectMethod(activity, env->GetMethodID(act, "getWindow", "()Landroid/view/Window;"));
    if (env->ExceptionCheck() || !window) {
        env->ExceptionClear();
        return;
    }
    jclass winc = env->GetObjectClass(window);
    jmethodID get_ctl = env->GetMethodID(winc, "getInsetsController", "()Landroid/view/WindowInsetsController;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    jobject ctl = get_ctl ? env->CallObjectMethod(window, get_ctl) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear(), ctl = nullptr;
    if (ctl) {  // Android 11+
        jclass cc = env->GetObjectClass(ctl);
        jclass types = env->FindClass("android/view/WindowInsets$Type");
        jint bars = env->CallStaticIntMethod(types, env->GetStaticMethodID(types, "systemBars", "()I"));
        env->CallVoidMethod(ctl, env->GetMethodID(cc, "setSystemBarsBehavior", "(I)V"), 2);  // show transient by swipe
        env->CallVoidMethod(ctl, env->GetMethodID(cc, "hide", "(I)V"), bars);
    } else {  // Android 10: immersive sticky | hide navigation | fullscreen | layout stable/hide navigation/fullscreen
        jobject decor = env->CallObjectMethod(window, env->GetMethodID(winc, "getDecorView", "()Landroid/view/View;"));
        if (decor)
            env->CallVoidMethod(decor, env->GetMethodID(env->GetObjectClass(decor), "setSystemUiVisibility", "(I)V"), 0x1706);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
}

int on_ui_request(int fd, int, void*) {
    char buf[16];
    while (read(fd, buf, sizeof buf) > 0) {
    }
    if (g_activity) hide_system_bars(g_activity->env, g_activity->clazz);
    return 1;
}

void request_immersive() {
    char c = 1;
    if (g_ui_pipe[1] >= 0) (void)write(g_ui_pipe[1], &c, 1);
}

}  // namespace

// Frames presented so far, for the launcher's FPS counter (GameActivity.nativeFrames, in the game's own process).
extern "C" JNIEXPORT jlong JNICALL Java_io_github_belmantegu_wonderrecomp_GameActivity_nativeFrames(JNIEnv*, jclass) {
    return jlong(nx::g_frame.load());
}

extern "C" JNIEXPORT void ANativeActivity_onCreate(ANativeActivity* activity, void* saved, size_t size) {
    g_activity = activity;
    if (g_ui_pipe[0] < 0 && pipe2(g_ui_pipe, O_NONBLOCK | O_CLOEXEC) == 0)
        ALooper_addFd(ALooper_forThread(), g_ui_pipe[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, on_ui_request, nullptr);
    glue_ANativeActivity_onCreate(activity, saved, size);
}

// All-files access (Android 11+): needed to read Internal storage/<game title>. Asks for it by opening the system page.
static bool has_all_files_access(android_app* app) {
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);
    jclass envc = env->FindClass("android/os/Environment");
    jmethodID m = env->GetStaticMethodID(envc, "isExternalStorageManager", "()Z");
    bool ok = m && env->CallStaticBooleanMethod(envc, m);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return ok;
}

static void request_all_files_access(android_app* app) {
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);
    jclass uric = env->FindClass("android/net/Uri");
    jobject uri = env->CallStaticObjectMethod(uric, env->GetStaticMethodID(uric, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"),
                                              env->NewStringUTF((std::string("package:") + nx::game::kInfo.android_package).c_str()));
    jclass intentc = env->FindClass("android/content/Intent");
    jobject intent = env->NewObject(intentc, env->GetMethodID(intentc, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V"),
                                    env->NewStringUTF("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION"), uri);
    jclass actc = env->GetObjectClass(app->activity->clazz);
    env->CallVoidMethod(app->activity->clazz, env->GetMethodID(actc, "startActivity", "(Landroid/content/Intent;)V"), intent);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

static bool has_data(const std::string& root) { return access((root + "/analysis/main.bin").c_str(), R_OK) == 0; }

extern "C" void android_main(android_app* app) {
    using namespace nx::plat;
    g_app = app;
    // Game data: Internal storage/<game title> (put there by the player, needs all-files access) or the app's own
    // external files folder (adb push).
    // The folder picked on the home screen (games/<game>/java Home.java writes <files>/data_root.txt) comes first.
    std::string chosen;
    if (app->activity->internalDataPath) {
        std::ifstream f(std::string(app->activity->internalDataPath) + "/data_root.txt");
        std::getline(f, chosen);
        while (!chosen.empty() && (chosen.back() == '\r' || chosen.back() == '\n' || chosen.back() == ' ')) chosen.pop_back();
    }
    const std::string shared = !chosen.empty() ? chosen : std::string("/storage/emulated/0/") + nx::game::kInfo.title;
    const std::string own = app->activity->externalDataPath ? app->activity->externalDataPath : shared;
    if (!has_data(own) && !has_all_files_access(app)) {
        request_all_files_access(app);
        while (!has_all_files_access(app)) {  // keep the app responsive until the switch is turned on
            int events;
            android_poll_source* src;
            while (ALooper_pollOnce(100, nullptr, &events, reinterpret_cast<void**>(&src)) >= 0) {
                if (src) src->process(app, src);
                if (app->destroyRequested) _exit(0);
            }
        }
    }
    // The picked folder wins over the app's own one; else the own one (adb push), else Internal storage/<title>.
    g_root = !chosen.empty() && has_data(chosen) ? chosen : has_data(own) ? own : shared;
    // config.txt's NX_* lines become environment variables (the NVN layer reads its switches with getenv), without
    // overriding a variable already set. Android defaults: handheld (720p, the lighter GPU load).
    {
        std::ifstream f(g_root + "/config.txt");
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            auto eq = line.find('=');
            if (line.rfind("NX_", 0) == 0 && eq != std::string::npos)
                setenv(line.substr(0, eq).c_str(), line.substr(eq + 1).c_str(), 0);
        }
        // NX_HANDHELD=0 in config.txt (the launcher's TV mode): docked, 1080p. svc_am.cpp tests only whether the variable
        // exists, so "0" must remove it.
        if (const char* h = getenv("NX_HANDHELD"); h && h[0] == '0')
            unsetenv("NX_HANDHELD");
        else
            setenv("NX_HANDHELD", "1", 0);
    }
    // The heap (scudo) zero-fills every allocation and gives freed pages back at once by default: in the replay, a
    // memset under each allocation and madvise churn (docs/nvn/android-perf/PLAN.md, phase 2). NX_MALLOC_TUNE=0: as
    // the system set it.
    if (const char* e = getenv("NX_MALLOC_TUNE"); !(e && e[0] == '0')) {
        int zero = mallopt(M_BIONIC_ZERO_INIT, 0), decay = mallopt(M_DECAY_TIME, 1);
        nx::log("heap: zero-fill off (%d), page release delayed (%d)", zero, decay);
    }
    start_memory_guard();
    gpu_turbo();
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    // NX_SHOW_WHEN_LOCKED=1 (unattended test runs, tools/wonder_phone_course.sh): shown over the lock screen and turning
    // the screen on, as an alarm app; the phone itself stays locked.
    uint32_t flags = AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN;
    if (const char* e = getenv("NX_SHOW_WHEN_LOCKED"); e && e[0] == '1')
        flags |= AWINDOW_FLAG_SHOW_WHEN_LOCKED | AWINDOW_FLAG_TURN_SCREEN_ON;
    ANativeActivity_setWindowFlags(app->activity, flags, 0);

    struct sigaction sa{};
    sa.sa_sigaction = crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(s, &sa, &g_previous[s]);

    nx::log("%s (Switch recomp) on Android, data at %s (own folder %s: %s)", nx::game::kInfo.title, g_root.c_str(),
            own.c_str(), has_data(own) ? "has the data" : "no analysis/main.bin");
    static bool started = false;
    for (;;) {
        int events;
        android_poll_source* src;
        while (ALooper_pollOnce(started ? 0 : 50, nullptr, &events, reinterpret_cast<void**>(&src)) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) _exit(0);
        }
        if (!started && g_window) {
            started = true;
            start_thread(game_thread, nullptr, 256ull << 20);  // the guest main thread; deep host recursion
        }
        if (started) sleep_ns(2'000'000);
    }
}
