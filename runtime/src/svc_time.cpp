// time:u / time:a / time:s: clocks, time zone, and the shared memory the SDK reads the clocks from.
#include <chrono>
#include <cstring>
#include <ctime>

#include <nx/runtime.h>

#include "ipc.h"
#include "platform.h"

namespace nx {

namespace {

// UUID of the steady clock source (any constant works; the SDK compares system clock time points against it).
constexpr uint64_t kSourceLo = 0x4E58525443524543ull, kSourceHi = 0x484144455354494Dull;

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t steady_seconds() { return int64_t(system_tick() / 19200000); }

// Portable UTC <-> calendar helpers (MSVC and bionic spell these differently).
void utc_tm(time_t t, std::tm& out) {
#if defined(_WIN32)
    gmtime_s(&out, &t);
#else
    gmtime_r(&t, &out);
#endif
}

int64_t utc_from_tm(std::tm& tm) {
#if defined(_WIN32)
    return int64_t(_mkgmtime(&tm));
#else
    return int64_t(timegm(&tm));
#endif
}

// Seconds west of UTC, like _get_timezone (so local = utc - offset).
long utc_offset_west() {
    time_t now = std::time(nullptr);
    std::tm lt{}, gt{};
#if defined(_WIN32)
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    utc_tm(now, gt);
    lt.tm_isdst = 0;
    return long(utc_from_tm(gt) - utc_from_tm(lt));
}

void write_steady_clock_context(uint64_t p) {
    st<uint64_t>(p, 0);
    st<uint64_t>(p + 8, kSourceLo);
    st<uint64_t>(p + 16, kSourceHi);
}

void write_system_clock_context(uint64_t p, int64_t offset) {
    st<int64_t>(p, offset);
    st<int64_t>(p + 8, 0);
    st<uint64_t>(p + 16, kSourceLo);
    st<uint64_t>(p + 24, kSourceHi);
}

void write_continuous_adjustment(uint64_t p) {
    st<uint64_t>(p, plat::monotonic_ns());
    st<int64_t>(p + 8, 1);
    st<int64_t>(p + 16, 0);
    write_system_clock_context(p + 24, 0);
}

std::shared_ptr<KSharedMemory> time_shared_memory() {
    static std::shared_ptr<KSharedMemory> shm = [] {
        auto s = make_shared_memory("time", 0x1000);
        uint64_t p = s->host;
        std::memset(reinterpret_cast<void*>(p), 0, 0x1000);
        int64_t offset = unix_now() - steady_seconds();
        // Layout matches Horizon time shared memory (see Ryujinx TimeSharedMemory).
        write_steady_clock_context(p + 0x8);
        write_steady_clock_context(p + 0x20);
        write_system_clock_context(p + 0x40, offset);
        write_system_clock_context(p + 0x60, offset);
        write_system_clock_context(p + 0x88, offset);
        write_system_clock_context(p + 0xa8, offset);
        st<uint8_t>(p + 0xcc, 0);  // automatic user clock correction disabled
        write_continuous_adjustment(p + 0xd8);
        write_continuous_adjustment(p + 0xd8 + 56);
        return s;
    }();
    return shm;
}

std::shared_ptr<Service> system_clock() {
    auto s = std::make_shared<Service>("ISystemClock");
    s->cmds[0] = [](IpcCtx& c) { c.push<int64_t>(unix_now()); return 0u; };  // GetCurrentTime
    s->cmds[2] = [](IpcCtx& c) {                                              // GetSystemClockContext
        c.push<int64_t>(unix_now() - steady_seconds());
        c.push<int64_t>(0);
        c.push<uint64_t>(kSourceLo);
        c.push<uint64_t>(kSourceHi);
        return 0u;
    };
    return s;
}

std::shared_ptr<Service> steady_clock() {
    auto s = std::make_shared<Service>("ISteadyClock");
    s->cmds[0] = [](IpcCtx& c) {  // GetCurrentTimePoint
        c.push<int64_t>(steady_seconds());
        c.push<uint64_t>(kSourceLo);
        c.push<uint64_t>(kSourceHi);
        return 0u;
    };
    return s;
}

// Calendar conversion in UTC (the host's local offset is applied as the "rule").
void push_calendar(IpcCtx& c, int64_t t) {
    long offset = utc_offset_west();
    time_t lt = time_t(t - offset);
    std::tm tm{};
    utc_tm(lt, tm);
    // CalendarTime {s16 year; s8 month, day, hour, minute, second; pad}
    c.push<int16_t>(int16_t(tm.tm_year + 1900));
    c.push<int8_t>(int8_t(tm.tm_mon + 1));
    c.push<int8_t>(int8_t(tm.tm_mday));
    c.push<int8_t>(int8_t(tm.tm_hour));
    c.push<int8_t>(int8_t(tm.tm_min));
    c.push<int8_t>(int8_t(tm.tm_sec));
    c.push<int8_t>(0);
    // CalendarAdditionalInfo {u32 day_of_week; u32 day_of_year; char tz_name[8]; u32 is_dst; s32 utc_offset}
    c.push<uint32_t>(uint32_t(tm.tm_wday));
    c.push<uint32_t>(uint32_t(tm.tm_yday));
    char name[8] = "UTC";
    for (char ch : name) c.push<char>(ch);
    c.push<uint32_t>(0);
    c.push<int32_t>(int32_t(-offset));
}

std::shared_ptr<Service> time_zone() {
    auto s = std::make_shared<Service>("ITimeZoneService");
    s->cmds[0] = [](IpcCtx& c) {  // GetDeviceLocationName
        char name[0x24] = "UTC";
        for (char ch : name) c.push<char>(ch);
        return 0u;
    };
    s->cmds[100] = [](IpcCtx& c) { push_calendar(c, c.arg<int64_t>(0)); return 0u; };  // ToCalendarTime (rule in buffer)
    s->cmds[101] = [](IpcCtx& c) { push_calendar(c, c.arg<int64_t>(0)); return 0u; };  // ToCalendarTimeWithMyRule
    s->cmds[201] = [](IpcCtx& c) {  // ToPosixTimeWithMyRule(CalendarTime) -> count, times in buffer
        std::tm tm{};
        tm.tm_year = c.arg<int16_t>(0) - 1900;
        tm.tm_mon = c.arg<int8_t>(2) - 1;
        tm.tm_mday = c.arg<int8_t>(3);
        tm.tm_hour = c.arg<int8_t>(4);
        tm.tm_min = c.arg<int8_t>(5);
        tm.tm_sec = c.arg<int8_t>(6);
        long offset = utc_offset_west();
        int64_t t = utc_from_tm(tm) + offset;
        Buffer out = c.out_buf(0);
        if (out.addr && out.size >= 8) st<int64_t>(out.addr, t);
        c.push<uint32_t>(1);
        return 0u;
    };
    return s;
}

void time_static(Service& s) {
    s.cmds[0] = [](IpcCtx& c) { c.push_object(system_clock()); return 0u; };  // GetStandardUserSystemClock
    s.cmds[1] = [](IpcCtx& c) { c.push_object(system_clock()); return 0u; };  // GetStandardNetworkSystemClock
    s.cmds[2] = [](IpcCtx& c) { c.push_object(steady_clock()); return 0u; };  // GetStandardSteadyClock
    s.cmds[3] = [](IpcCtx& c) { c.push_object(time_zone()); return 0u; };     // GetTimeZoneService
    s.cmds[4] = [](IpcCtx& c) { c.push_object(system_clock()); return 0u; };  // GetStandardLocalSystemClock
    s.cmds[5] = [](IpcCtx& c) { c.push_object(system_clock()); return 0u; };  // GetEphemeralNetworkSystemClock
    s.cmds[20] = [](IpcCtx& c) {                                              // GetSharedMemoryNativeHandle
        c.out_copy.push_back(handle_add(time_shared_memory()));
        return 0u;
    };
    s.cmds[100] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // IsStandardUserSystemClockAutomaticCorrectionEnabled
    s.cmds[300] = [](IpcCtx& c) { c.push<int64_t>(unix_now()); return 0u; };  // CalculateMonotonicSystemClockBaseTimePoint
}

#if defined(__APPLE__)
// Wonder calls this in nnMain before the nn::os native-handle import path maps time:u's shared memory. Remove when
// SharedMemoryImplByHorizon can attach service handles returned by GetSharedMemoryNativeHandle.
static void time_initialize_hle(CPU& c) { (void)c; }
NX_HLE("_ZN2nn4time10InitializeEv", time_initialize_hle);
#endif

ServiceRegistrar r_time_u("time:u", [] { auto s = std::make_shared<Service>("time:u"); time_static(*s); return s; });
ServiceRegistrar r_time_a("time:a", [] { auto s = std::make_shared<Service>("time:a"); time_static(*s); return s; });
ServiceRegistrar r_time_s("time:s", [] { auto s = std::make_shared<Service>("time:s"); time_static(*s); return s; });

}  // namespace

}  // namespace nx
