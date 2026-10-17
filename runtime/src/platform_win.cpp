// Windows host: memory, threads, time, files, the game window (F11 / Alt+Enter fullscreen), XInput + keyboard,
// XAudio2, the Vulkan loader and win32 surface, crash handler, hardware watchpoints (NX_WATCH) and main().
#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#include <windows.h>
#include <xaudio2.h>
#include <xinput.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"

namespace nx {
HWND g_window = nullptr;
void boot();
}  // namespace nx

namespace nx::plat {

// ---- memory ---------------------------------------------------------------------------------------------------

bool reserve(uint64_t addr, uint64_t size) {
    return VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE, PAGE_NOACCESS) != nullptr;
}

bool commit(uint64_t addr, uint64_t size) {
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    return VirtualAlloc(reinterpret_cast<void*>(a), e - a, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

void decommit(uint64_t addr, uint64_t size) { VirtualFree(reinterpret_cast<void*>(addr), size, MEM_DECOMMIT); }

// ---- threads and time -----------------------------------------------------------------------------------------

struct ThreadStart {
    ThreadFn fn;
    void* arg;
};

static DWORD WINAPI thread_entry(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    s.fn(s.arg);
    return 0;
}

void start_thread(ThreadFn fn, void* arg, size_t stack_size) {
    CreateThread(nullptr, stack_size, thread_entry, new ThreadStart{fn, arg}, STACK_SIZE_PARAM_IS_A_RESERVATION,
                 nullptr);
}

void exit_thread() { ExitThread(0); }
void exit_process(int code) { ExitProcess(UINT(code)); }
void sleep_ns(int64_t ns) { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); }
void yield() { SwitchToThread(); }
void set_thread_name(const char*) {}
float default_render_scale() { return 1.0f; }
void release_window(void*) {}
uint64_t window_generation() { return 1; }
void audio_pause(bool) {}

std::string setting(const std::string& key) {
    std::string env = "NX_";
    for (char ch : key) env += char(std::toupper(static_cast<unsigned char>(ch)));
    const char* v = std::getenv(env.c_str());
    return v ? v : "";
}
int64_t current_thread_id() { return int64_t(GetCurrentThreadId()); }
bool profile(const int64_t*, const uint64_t* const*, size_t, int, const std::string&) { return false; }

uint64_t monotonic_ns() {
    static const LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(double(t.QuadPart) * 1e9 / double(f.QuadPart));
}

// ---- files and output -----------------------------------------------------------------------------------------

FILE* fopen_utf8(const std::string& path, const char* mode) {
    std::wstring wp = std::filesystem::u8path(path).wstring();
    std::wstring wm(mode, mode + std::strlen(mode));
    return _wfopen(wp.c_str(), wm.c_str());
}
int64_t ftell64(FILE* f) { return _ftelli64(f); }
int fseek64(FILE* f, int64_t off, int origin) { return _fseeki64(f, off, origin); }
uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size) {
    _lock_file(f);  // seek + read as one step for this FILE
    _fseeki64_nolock(f, off, SEEK_SET);
    uint64_t n = _fread_nolock(buf, 1, size, f);
    _unlock_file(f);
    return n;
}
bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(path), ec);
    return !ec;
}

void log_line(const char* line) { std::fprintf(stderr, "%s\n", line); }

std::string data_root() {
    const char* r = std::getenv("NX_DATA_ROOT");
    return r ? r : nx::game::kInfo.pc_data_root;
}

// ---- window ---------------------------------------------------------------------------------------------------

static std::wstring widen(const char* s) { return std::wstring(s, s + std::strlen(s)); }  // ASCII titles
// The game renders its swapchain at the render resolution; the window can be any size (the compositor scales).

static constexpr DWORD kWindowed = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
static bool g_fullscreen = false;
static WINDOWPLACEMENT g_placement{sizeof(WINDOWPLACEMENT)};

static void toggle_fullscreen(HWND h) {
    if (!g_fullscreen) {
        MONITORINFO mi{sizeof mi};
        GetWindowPlacement(h, &g_placement);
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(h, GWL_STYLE, kWindowed);
        SetWindowPlacement(h, &g_placement);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = !g_fullscreen;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_CLOSE:
            log("window closed");
            ExitProcess(0);
        case WM_KEYDOWN:
            if (w == VK_F11) toggle_fullscreen(h);
            break;
        case WM_SYSKEYDOWN:
            if (w == VK_RETURN) {  // Alt+Enter
                toggle_fullscreen(h);
                return 0;
            }
            break;
        case WM_SYSCHAR:
            if (w == VK_RETURN) return 0;  // no beep
            break;
    }
    return DefWindowProcW(h, msg, w, l);
}

// The window lives on its own thread so its message loop keeps running whatever the game threads do.
static void window_thread(std::atomic<bool>* ready) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    const std::wstring cls = widen(nx::game::kInfo.title) + L"Recomp";  // tools/capture.ps1 finds the window by it
    wc.lpszClassName = cls.c_str();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    // The render resolution's aspect (NX_RESOLUTION, default 16:9), about 80% of the screen height, centred.
    unsigned rw = 16, rh = 9;
    if (const char* r = std::getenv("NX_RESOLUTION")) std::sscanf(r, "%ux%u", &rw, &rh);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ch = std::max(540, sh * 8 / 10), cw = int(int64_t(ch) * rw / std::max(1u, rh));
    RECT r{0, 0, cw, ch};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (sh - h) / 2;
    // NX_HEADLESS: created hidden (the surface still needs a window), never brought to the front.
    const DWORD style = headless() ? (kWindowed & ~WS_VISIBLE) : kWindowed;
    g_window = CreateWindowW(wc.lpszClassName, (widen(nx::game::kInfo.title) + L" (Switch recomp)  -  F11: fullscreen").c_str(), style,
                             std::max(0, x), std::max(0, y), w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (const char* f = std::getenv("NX_FULLSCREEN"); f && *f == '1' && !headless()) toggle_fullscreen(g_window);
    if (!headless()) SetForegroundWindow(g_window);
    *ready = true;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

void* create_window() {
    if (!g_window) {
        static std::atomic<bool> ready{false};
        std::thread(window_thread, &ready).detach();
        while (!ready) Sleep(1);
    }
    return g_window;
}

bool window_minimized() { return g_window && IsIconic(g_window); }

void window_size(uint32_t& w, uint32_t& h) {
    RECT r{};
    if (g_window) GetClientRect(g_window, &r);
    w = uint32_t(r.right - r.left);
    h = uint32_t(r.bottom - r.top);
}

void screen_size(uint32_t& w, uint32_t& h) { window_size(w, h); }

// ---- input: XInput pad 0 + keyboard ---------------------------------------------------------------------------

static bool key(int vk) { return g_window && GetForegroundWindow() == g_window && (GetAsyncKeyState(vk) & 0x8000); }

bool headless() {  // NX_HEADLESS (platform.h)
    static const bool on = std::getenv("NX_HEADLESS") != nullptr;
    return on;
}

void read_pad(PadState& p) {
    XINPUT_STATE xs{};
    if (XInputGetState(0, &xs) == ERROR_SUCCESS) {
        const auto& g = xs.Gamepad;
        if (g.wButtons & XINPUT_GAMEPAD_B) p.buttons |= kA;  // Nintendo layout
        if (g.wButtons & XINPUT_GAMEPAD_A) p.buttons |= kB;
        if (g.wButtons & XINPUT_GAMEPAD_Y) p.buttons |= kX;
        if (g.wButtons & XINPUT_GAMEPAD_X) p.buttons |= kY;
        if (g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) p.buttons |= kL;
        if (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) p.buttons |= kR;
        if (g.bLeftTrigger > 100) p.buttons |= kZL;
        if (g.bRightTrigger > 100) p.buttons |= kZR;
        if (g.wButtons & XINPUT_GAMEPAD_START) p.buttons |= kPlus;
        if (g.wButtons & XINPUT_GAMEPAD_BACK) p.buttons |= kMinus;
        if (g.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) p.buttons |= kStickL;
        if (g.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) p.buttons |= kStickR;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) p.buttons |= kLeft;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_UP) p.buttons |= kUp;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) p.buttons |= kRight;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) p.buttons |= kDown;
        auto dz = [](int v) { return (v > -7849 && v < 7849) ? 0 : std::max(-kStickMax, std::min(kStickMax, v)); };
        p.lx = dz(g.sThumbLX);
        p.ly = dz(g.sThumbLY);
        p.rx = dz(g.sThumbRX);
        p.ry = dz(g.sThumbRY);
    }
    // Keyboard: WASD / arrows move, Space A, Shift B (dash), J X, K Y, Q/E L/R, Z/C ZL/ZR, Enter +, Esc -.
    if (key('A') || key(VK_LEFT)) p.lx = -kStickMax;
    if (key('D') || key(VK_RIGHT)) p.lx = kStickMax;
    if (key('W') || key(VK_UP)) p.ly = kStickMax;
    if (key('S') || key(VK_DOWN)) p.ly = -kStickMax;
    if (key(VK_SPACE)) p.buttons |= kA;
    if (key(VK_SHIFT)) p.buttons |= kB;
    if (key('J')) p.buttons |= kX;
    if (key('K')) p.buttons |= kY;
    if (key('Q')) p.buttons |= kL;
    if (key('E')) p.buttons |= kR;
    if (key('Z')) p.buttons |= kZL;
    if (key('C')) p.buttons |= kZR;
    if (key(VK_RETURN)) p.buttons |= kPlus;
    if (key(VK_ESCAPE)) p.buttons |= kMinus;
    input_script(p);  // NX_INPUT_SCRIPT (input_script.cpp): unattended runs
}

// ---- audio: XAudio2 -------------------------------------------------------------------------------------------

namespace {

std::atomic<uint32_t> g_queued{0};
AudioDone g_done = nullptr;

struct Voice : IXAudio2VoiceCallback {
    void STDMETHODCALLTYPE OnBufferEnd(void* ctx) noexcept override {
        auto* p = static_cast<std::pair<void*, std::vector<uint8_t>>*>(ctx);
        g_queued--;
        g_done(p->first);
        delete p;
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {}
};

IXAudio2* g_xa = nullptr;
IXAudio2SourceVoice* g_voice = nullptr;
Voice g_callback;

}  // namespace

bool audio_open(AudioDone done) {
    g_done = done;
    // Headless test runs (and NX_MUTE=1) are silent: no device, the callers pace themselves with a timer.
    if (headless() || std::getenv("NX_MUTE")) return false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
    IXAudio2MasteringVoice* master = nullptr;
    if (FAILED(g_xa->CreateMasteringVoice(&master))) return false;
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = 48000 * 4;
    if (FAILED(g_xa->CreateSourceVoice(&g_voice, &wf, 0, 2.0f, &g_callback))) return false;
    g_voice->Start();
    return true;
}

bool audio_submit(const void* data, size_t bytes, void* ctx) {
    if (!g_voice) return false;
    auto* p = new std::pair<void*, std::vector<uint8_t>>(
        ctx, std::vector<uint8_t>(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes));
    XAUDIO2_BUFFER xb{};
    xb.AudioBytes = UINT32(bytes);
    xb.pAudioData = p->second.data();
    xb.pContext = p;
    g_queued++;
    g_voice->SubmitSourceBuffer(&xb);
    return true;
}

uint32_t audio_queued() { return g_queued; }

// ---- Vulkan ---------------------------------------------------------------------------------------------------

void* vulkan_get_instance_proc_addr() {
    HMODULE m = LoadLibraryW(L"vulkan-1.dll");
    return m ? reinterpret_cast<void*>(GetProcAddress(m, "vkGetInstanceProcAddr")) : nullptr;
}

const char* vulkan_surface_extension() { return VK_KHR_WIN32_SURFACE_EXTENSION_NAME; }

int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void* gipa) {
    auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa)(static_cast<VkInstance>(instance), "vkCreateWin32SurfaceKHR"));
    VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    ci.hinstance = GetModuleHandleW(nullptr);
    ci.hwnd = static_cast<HWND>(native_window);
    VkSurfaceKHR s = VK_NULL_HANDLE;
    VkResult r = create(static_cast<VkInstance>(instance), &ci, nullptr, &s);
    *surface = reinterpret_cast<uint64_t>(s);
    return r;
}

}  // namespace nx::plat

// ---- crash handler, watchpoints, entry --------------------------------------------------------------------------

namespace {

// NX_WATCH=<hex address>: hardware write watchpoint (DR0, 8 bytes) on the main thread; each write (up to 30) logs
// the host module/offset and the guest context.
uint64_t g_watch = 0;

void arm_watch(HANDLE thread) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(thread, &ctx);
    ctx.Dr0 = g_watch;
    ctx.Dr7 = (1ull << 0) | (1ull << 16) | (3ull << 18);  // L0, R/W0 = write, LEN0 = 8 bytes
    SetThreadContext(thread, &ctx);
}

void dump_guest(nx::CPU& c) {
    using namespace nx;
    log("  guest lr %s  sp %llx  tpidr %llx  tpidrro %llx", describe(c.x[30]).c_str(), (unsigned long long)c.x[31],
        (unsigned long long)c.tpidr, (unsigned long long)c.tpidrro);
    for (int k = 0; k < 32; k += 4)
        log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
            (unsigned long long)c.x[k + 2], (unsigned long long)c.x[k + 3]);
    backtrace(c);
}

uint64_t exe_va(void* p) { return uint64_t(p) - uint64_t(GetModuleHandleW(nullptr)) + 0x140000000ull; }

LONG WINAPI watch_handler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_watch) return EXCEPTION_CONTINUE_SEARCH;
    nx::log("WATCH: %llx written (now %llx) at %s.exe+%llx (tools/symbolize.py)", (unsigned long long)g_watch,
            (unsigned long long)nx::ld<uint64_t>(g_watch), nx::game::kInfo.id, (unsigned long long)exe_va(e->ExceptionRecord->ExceptionAddress));
    dump_guest(nx::current_cpu());
    static int hits = 0;
    if (++hits >= 30) e->ContextRecord->Dr7 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// "<module>+<offset>" for a host code address; our own exe as the linker's VA (tools/symbolize.py reads the map).
std::string host_where(uint64_t pc) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(pc), &m))
        return "?";
    char b[MAX_PATH + 32];
    if (m == GetModuleHandleW(nullptr)) {
        std::snprintf(b, sizeof b, "%s.exe+%llx", nx::game::kInfo.id, (unsigned long long)exe_va(reinterpret_cast<void*>(pc)));
        return b;
    }
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = std::max(std::strrchr(path, '\\'), std::strrchr(path, '/'));
    std::snprintf(b, sizeof b, "%s+%llx", name ? name + 1 : path, (unsigned long long)(pc - uint64_t(m)));
    return b;
}

// The host call stack at the fault, unwound from its context with the x64 unwind tables.
void host_backtrace(const CONTEXT& at) {
    CONTEXT c = at;
    for (int k = 0; k < 16 && c.Rip; k++) {
        nx::log("  host #%-2d %s", k, host_where(c.Rip).c_str());
        DWORD64 base = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (!f) {  // a leaf function: the return address is at rsp
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &handler_data, &frame, nullptr);
    }
}

LONG WINAPI crash_handler(EXCEPTION_POINTERS* e) {
    auto* r = e->ExceptionRecord;
    uint64_t info = r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0;
    nx::log("CRASH: exception %08lx at %s (tools/symbolize.py for the exe), data address %llx", r->ExceptionCode,
            host_where(uint64_t(r->ExceptionAddress)).c_str(), (unsigned long long)info);
    host_backtrace(*e->ContextRecord);
    dump_guest(nx::current_cpu());
    nx::log_recent_files();
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

// NX_THROW_TRACE=1: log the host call stack of every C++ throw that is not a guest longjmp (first chance).
LONG WINAPI throw_tracer(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH;
    void* frames[24];
    USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    std::string s = "C++ throw, host stack:";
    for (USHORT k = 0; k < n; k++) {
        char b[32];
        std::snprintf(b, sizeof b, " %llx", (unsigned long long)exe_va(frames[k]));
        s += b;
    }
    nx::log("%s", s.c_str());
    return EXCEPTION_CONTINUE_SEARCH;
}

int main() {
    nx::plat::start_memory_guard();  // before anything can grow: a leak must never freeze the machine (memguard_win.cpp)
    SetUnhandledExceptionFilter(crash_handler);
    if (const char* t = std::getenv("NX_THROW_TRACE"); t && *t == '1') AddVectoredExceptionHandler(1, throw_tracer);
    if (const char* w = std::getenv("NX_WATCH")) {
        g_watch = std::strtoull(w, nullptr, 16);
        AddVectoredExceptionHandler(1, watch_handler);
        arm_watch(GetCurrentThread());
        nx::log("watching writes to %llx", (unsigned long long)g_watch);
    }
    try {
        nx::boot();
    } catch (const nx::LongJmp& j) {
        nx::fatal("longjmp to buffer %llx with no setjmp frame", (unsigned long long)j.buf);
    } catch (const std::exception& e) {
        nx::fatal("host exception: %s", e.what());
    }
    return 0;
}
