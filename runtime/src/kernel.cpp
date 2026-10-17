// A small Horizon kernel: handles, threads, synchronization (mutex/condvar/address arbitration), events,
// memory and GetInfo. Services (IPC) live in ipc.cpp.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <nx/game.h>

#include "kernel_state.h"
#include "platform.h"

namespace nx {

namespace kstate {  // kernel_state.h

std::recursive_mutex g_k;
std::vector<KThread*> g_sync_waiters;
std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_handles;
uint32_t g_next_handle = 0x10000;
std::atomic<uint64_t> g_next_thread_id{0x50};
uint64_t g_heap_size = 0;

std::vector<KThread*> g_mutex_waiters;
std::vector<KThread*> g_cv_waiters;
std::vector<KThread*> g_addr_waiters;

thread_local KThread* t_self = nullptr;

std::mutex g_all_lock;
std::vector<std::weak_ptr<KThread>> g_all;

}  // namespace kstate

using namespace kstate;

uint32_t handle_add(std::shared_ptr<KObject> o) {
    std::lock_guard lk(g_k);
    uint32_t h = g_next_handle++;
    g_handles[h] = std::move(o);
    return h;
}

std::shared_ptr<KObject> handle_get(uint32_t h) {
    std::lock_guard lk(g_k);
    if (h == kPseudoCurrentThread && t_self) return t_self->self.lock();
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second;
}

void handle_close(uint32_t h) {
    std::lock_guard lk(g_k);
    g_handles.erase(h);
}

// A waitable object changed (event signalled, thread exited...): wake the threads waiting on objects, only them.
// Mutex, condition variable and address waiters are woken one by one (wake()): one global condition variable woke
// every waiting thread of the process on each signal (the engine's job system broadcasts all the time).
void notify_all() {
    std::lock_guard lk(g_k);
    for (KThread* t : g_sync_waiters) t->cv.notify_one();
}

KThread* self_thread() { return t_self; }

uint64_t heap_size() { return g_heap_size; }

CPU& current_cpu() { return t_self->ctx; }

uint64_t system_tick() { return plat::monotonic_ns() * 192 / 10000; }  // 19.2 MHz

// The spin throttles below (a 0.5 ms nap for threads spinning on the tick or on yields) were made for Hades on phones.
// Wonder's job system joins its workers by yielding in a loop: every join became >= 1 ms and the main thread spent
// ~18% of its time in forced naps, holding the game near 26 fps (docs/nvn/DEBUG.md #37). NX_SPIN_NAPS=0/1 overrides.
static bool spin_naps() {
    static const bool on = [] {
        if (const char* e = std::getenv("NX_SPIN_NAPS")) return e[0] == '1';
        return std::strcmp(nx::game::kInfo.id, "hades") == 0;
    }();
    return on;
}

// nn::os::GetSystemTick. Hades' main loop (nnMain, main+2496fc) polls it without pause until its fixed step is
// due: on the Switch that thread owns a core, on a phone it pins a big core at 100% (80% of that thread's time was
// clock reads) and the heat lowers everyone's clocks. A thread asking again within 20 us, 50 times in a row, is
// spinning: let it sleep 0.5 ms (the loop keeps sub-millisecond precision against a 16.6 ms step).
// Hades only (spin_naps()): Wonder's job loops read the tick legitimately.
std::atomic<uint64_t> g_tick_calls{0}, g_yield_calls{0}, g_sleep_calls{0};
static void hle_get_system_tick(CPU& c) {
    g_tick_calls.fetch_add(1, std::memory_order_relaxed);
    if (KThread* t = spin_naps() ? t_self : nullptr) {
        uint64_t now = plat::monotonic_ns();
        if (now - t->last_tick_call < 20'000) {
            if (++t->tick_spins >= 50) {
                t->tick_spins = 0;
                plat::sleep_ns(500'000);
            }
        } else {
            t->tick_spins = 0;
        }
        t->last_tick_call = plat::monotonic_ns();
    }
    c.x[0] = system_tick();
}
NX_HLE("_ZN2nn2os13GetSystemTickEv", hle_get_system_tick);

// ---- waiting --------------------------------------------------------------------------------------------------

// Wait (with g_k held) until `self->wake` or timeout (ns, <0 = forever). Returns false on timeout.
static bool block(std::unique_lock<std::recursive_mutex>& lk, KThread* self, int64_t timeout_ns) {
    if (timeout_ns < 0) {
        self->cv.wait(lk, [&] { return self->wake; });
        return true;
    }
    auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout_ns);
    return self->cv.wait_until(lk, until, [&] { return self->wake; });
}

// Wake a blocked thread (with g_k held).
static void wake(KThread* t) {
    t->wake = true;
    t->wait_result = 0;
    t->cv.notify_one();
}

static void remove(std::vector<KThread*>& v, KThread* t) { v.erase(std::remove(v.begin(), v.end(), t), v.end()); }

static uint32_t load32(uint64_t a) { return __atomic_load_n(reinterpret_cast<uint32_t*>(a), __ATOMIC_SEQ_CST); }
static void store32(uint64_t a, uint32_t v) { __atomic_store_n(reinterpret_cast<uint32_t*>(a), v, __ATOMIC_SEQ_CST); }
static bool cas32(uint64_t a, uint32_t expected, uint32_t v) {
    return __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(a), &expected, v, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

// Hand the mutex at `addr` to its next waiter (or clear it).
static void unlock_mutex(uint64_t addr) {
    KThread* next = nullptr;
    int more = 0;
    for (KThread* t : g_mutex_waiters)
        if (t->wait_mutex == addr) {
            if (!next)
                next = t;
            else
                more++;
        }
    if (!next) {
        store32(addr, 0);
        return;
    }
    remove(g_mutex_waiters, next);
    store32(addr, next->wait_tag | (more ? kHandleWaitMask : 0));
    wake(next);
}

// ---- svc implementations --------------------------------------------------------------------------------------

static uint32_t svc_arbitrate_lock(uint32_t owner, uint64_t addr, uint32_t tag) {
    std::unique_lock lk(g_k);
    KThread* self = t_self;
    if (load32(addr) != (owner | kHandleWaitMask)) return 0;
    self->wake = false;
    self->wait_mutex = addr;
    self->wait_tag = tag;
    g_mutex_waiters.push_back(self);
    block(lk, self, -1);
    return self->wait_result;
}

static uint32_t svc_arbitrate_unlock(uint64_t addr) {
    std::unique_lock lk(g_k);
    unlock_mutex(addr);
    return 0;
}

static uint32_t svc_wait_process_wide_key(uint64_t mutex, uint64_t key, uint32_t tag, int64_t timeout) {
    std::unique_lock lk(g_k);
    KThread* self = t_self;
    // Like Horizon: the condvar's "has waiters" word is written BEFORE the mutex is released. The SDK's Signal/
    // Broadcast read that word in guest code and skip the svc when it is 0; the other order lost wake-ups (a thread
    // took the freed mutex, posted, saw 0, and the waiter then slept forever).
    self->wake = false;
    self->wait_mutex = mutex;
    self->wait_key = key;
    self->wait_tag = tag;
    self->wait_result = 0;
    g_cv_waiters.push_back(self);
    store32(key, 1);
    unlock_mutex(mutex);
    if (block(lk, self, timeout)) return self->wait_result;
    // Timed out (not handed the mutex): leave the condvar, or the mutex queue a signal moved us to, and return
    // without the mutex. Like Horizon: the SDK re-locks it itself (TimedWait -> InternalCriticalSection::Enter);
    // returning with it held made that Enter wait on ourselves forever.
    remove(g_cv_waiters, self);
    remove(g_mutex_waiters, self);
    bool others = std::any_of(g_cv_waiters.begin(), g_cv_waiters.end(), [&](KThread* t) { return t->wait_key == key; });
    if (!others) store32(key, 0);
    return 0xEA01;
}

static uint32_t svc_signal_process_wide_key(uint64_t key, int32_t count) {
    std::unique_lock lk(g_k);
    int n = 0;
    for (size_t i = 0; i < g_cv_waiters.size() && (count <= 0 || n < count);) {
        KThread* t = g_cv_waiters[i];
        if (t->wait_key != key) {
            i++;
            continue;
        }
        g_cv_waiters.erase(g_cv_waiters.begin() + i);
        n++;
        // Try to give it the mutex right away.
        for (;;) {
            uint32_t v = load32(t->wait_mutex);
            if (v == 0) {
                if (!cas32(t->wait_mutex, 0, t->wait_tag)) continue;
                wake(t);
                break;
            }
            if (!(v & kHandleWaitMask) && !cas32(t->wait_mutex, v, v | kHandleWaitMask)) continue;
            g_mutex_waiters.push_back(t);
            break;
        }
    }
    bool others = std::any_of(g_cv_waiters.begin(), g_cv_waiters.end(), [&](KThread* t) { return t->wait_key == key; });
    if (!others) store32(key, 0);
    return 0;
}

// NX_TRACE_ADDR=<hex>: log every wait on and signal to that address (lost wakeups in the guest's sync objects).
static uint64_t trace_addr() {
    static uint64_t a = [] {
        const char* e = std::getenv("NX_TRACE_ADDR");
        return e ? std::strtoull(e, nullptr, 16) : 0ull;
    }();
    return a;
}

static uint32_t svc_wait_for_address(uint64_t addr, uint32_t type, int32_t value, int64_t timeout) {
    if (addr == trace_addr())
        log("arb: thread %x waits on %llx type %u value %d (memory %d) timeout %lld", t_self ? t_self->handle : 0,
            (unsigned long long)addr, type, value, int32_t(load32(addr)), (long long)timeout);
    std::unique_lock lk(g_k);
    KThread* self = t_self;
    int32_t cur = int32_t(load32(addr));
    switch (type) {
        case 0:  // WaitIfLessThan
            if (!(cur < value)) return 0xFA01;
            break;
        case 1:  // DecrementAndWaitIfLessThan
            if (!(cur < value)) return 0xFA01;
            store32(addr, uint32_t(cur - 1));
            break;
        case 2:  // WaitIfEqual
            if (cur != value) return 0xFA01;
            break;
        default:
            return 0xF001;
    }
    if (timeout == 0) return 0xEA01;
    self->wake = false;
    self->wait_addr = addr;
    self->wait_result = 0;
    g_addr_waiters.push_back(self);
    if (!block(lk, self, timeout)) {
        remove(g_addr_waiters, self);
        return 0xEA01;
    }
    return self->wait_result;
}

static int wake_addr(uint64_t addr, int32_t count) {
    int n = 0;
    for (size_t i = 0; i < g_addr_waiters.size() && (count <= 0 || n < count);) {
        KThread* t = g_addr_waiters[i];
        if (t->wait_addr != addr) {
            i++;
            continue;
        }
        g_addr_waiters.erase(g_addr_waiters.begin() + i);
        wake(t);
        n++;
    }
    return n;
}

static uint32_t svc_signal_to_address(uint64_t addr, uint32_t type, int32_t value, int32_t count) {
    if (addr == trace_addr())
        log("arb: thread %x signals %llx type %u value %d count %d (memory %d)", t_self ? t_self->handle : 0,
            (unsigned long long)addr, type, value, count, int32_t(load32(addr)));
    std::unique_lock lk(g_k);
    int waiters = int(std::count_if(g_addr_waiters.begin(), g_addr_waiters.end(), [&](KThread* t) { return t->wait_addr == addr; }));
    switch (type) {
        case 0:  // Signal
            break;
        case 1:  // SignalAndIncrementIfEqual
            if (!cas32(addr, uint32_t(value), uint32_t(value + 1))) return 0xFA01;
            break;
        case 2: {  // SignalAndModifyByWaitingCountIfEqual
            int32_t updated = value;
            if (count <= 0)
                updated = waiters ? value - 2 : value + 1;
            else if (waiters)
                updated = waiters <= count ? value - 1 : value;
            else
                updated = value + 1;
            if (!cas32(addr, uint32_t(value), uint32_t(updated))) return 0xFA01;
            break;
        }
        default:
            return 0xF001;
    }
    wake_addr(addr, count);
    return 0;
}

static uint32_t svc_wait_synchronization(CPU& c, uint64_t handles, int32_t count, int64_t timeout) {
    std::unique_lock lk(g_k);
    std::vector<std::shared_ptr<KObject>> objs;
    for (int i = 0; i < count; i++) {
        auto o = handle_get(ld<uint32_t>(handles + 4 * i));
        if (!o) return 0xE401;  // invalid handle
        objs.push_back(o);
    }
    auto ready = [&]() -> int {
        for (int i = 0; i < count; i++)
            if (objs[i]->signaled()) return i;
        return -1;
    };
    auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout < 0 ? 0 : timeout);
    KThread* self = t_self;
    self->cancel = false;
    g_sync_waiters.push_back(self);
    struct Leave {
        KThread* t;
        ~Leave() { remove(g_sync_waiters, t); }
    } leave{self};
    for (;;) {
        int i = ready();
        if (i >= 0) {
            c.x[1] = uint32_t(i);
            objs[i]->consume();
            return 0;
        }
        if (self->cancel) {
            self->cancel = false;
            return 0xEC01;
        }
        if (timeout == 0) return 0xEA01;
        if (timeout < 0)
            self->cv.wait(lk);
        else if (self->cv.wait_until(lk, until) == std::cv_status::timeout && ready() < 0)
            return 0xEA01;
    }
}

// ---- threads --------------------------------------------------------------------------------------------------

static void thread_main(void* p) {
    KThread* t = static_cast<KThread*>(p);
    t_self = t;
    t->host_tid = plat::current_thread_id();
    char name[16];
    std::snprintf(name, sizeof name, "g%llx", (unsigned long long)t->id);
    plat::set_thread_name(name);
    call(t->ctx, t->entry);
    // Returning from the entry point is like svcExitThread.
    std::lock_guard lk(g_k);
    t->exited = true;
    notify_all();
}

static std::shared_ptr<KThread> new_thread(uint64_t entry, uint64_t arg, uint64_t sp, int prio, int core) {
    auto t = std::make_shared<KThread>();
    t->self = t;
    t->id = g_next_thread_id++;
    t->entry = entry;
    t->prio = prio;
    t->core = core;
    t->tls = kernel_alloc(0x1000);  // one TLS page per thread (0x200 used)
    // Static TLS block for the thread (what rtld and the libc provide on Horizon): the thread pointer, stored at
    // TLS region +0x1F0 where nn::os loads TPIDR_EL0 from. main's TLS is 0x440 bytes of .tbss at tp+0x10.
    uint64_t block = kernel_alloc(0x4000);
    init_tls_block(block);  // main's .tdata initial values (Wonder: 16 bytes, {-1, 0}); the rest is .tbss, zero
    st<uint64_t>(t->tls + 0x1F0, block);
    t->ctx.tpidr = block;
    t->ctx.x[0] = arg;
    t->ctx.x[31] = sp;
    t->ctx.tpidrro = t->tls;
    t->handle = handle_add(t);
    std::lock_guard al(g_all_lock);
    g_all.push_back(t);
    return t;
}

uint32_t create_main_thread(uint64_t stack_size) {
    uint64_t stack = kernel_alloc(stack_size);
    vm_set(stack, stack_size, kMemStack, 3);
    auto t = new_thread(0, 0, stack + stack_size, 44, 0);
    t->started = true;
    t_self = t.get();
    t->host_tid = plat::current_thread_id();
    return t->handle;
}


// ---- dispatch -------------------------------------------------------------------------------------------------

static void write_meminfo(uint64_t p, const VmInfo& v) {
    st<uint64_t>(p + 0, v.addr);
    st<uint64_t>(p + 8, v.size);
    st<uint32_t>(p + 16, v.state);
    st<uint32_t>(p + 20, 0);  // attribute
    st<uint32_t>(p + 24, v.perm);
    st<uint32_t>(p + 28, 0);
    st<uint32_t>(p + 32, 0);
    st<uint32_t>(p + 36, 0);
}

static void svc_impl(CPU& c, uint32_t imm, uint64_t pc) {
    if (g_trace) log("svc %#x from %s", imm, describe(pc).c_str());
    uint32_t rc = 0;
    switch (imm) {
        case 0x01: {  // SetHeapSize(size) -> addr
            uint64_t size = c.x[1];
            if (size > kHeapMax) {
                rc = 0xD001;
                break;
            }
            if (size > g_heap_size) commit(kHeapBase + g_heap_size, size - g_heap_size);
            vm_set(kHeapBase, kHeapMax, kMemInaccessible, 0);
            if (size) vm_set(kHeapBase, size, kMemHeap, 3);
            g_heap_size = size;
            c.x[1] = kHeapBase;
            break;
        }
        case 0x02:  // SetMemoryPermission
        case 0x03:  // SetMemoryAttribute
            break;
        case 0x04: {  // MapMemory(dst, src, size): src becomes inaccessible, so a copy behaves the same
            uint64_t dst = c.x[0], src = c.x[1], size = c.x[2];
            commit(dst, size);
            std::memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<void*>(src), size);
            vm_set(dst, size, dst - kStackBase < kStackSize ? kMemStack : kMemAlias, 3);
            break;
        }
        case 0x05: {  // UnmapMemory(dst, src, size)
            uint64_t dst = c.x[0], src = c.x[1], size = c.x[2];
            std::memcpy(reinterpret_cast<void*>(src), reinterpret_cast<void*>(dst), size);
            decommit(dst, size);
            vm_set(dst, size, kMemFree, 0);
            break;
        }
        case 0x06: {  // QueryMemory(meminfo*, pageinfo*, addr)
            write_meminfo(c.x[0], vm_query(c.x[2]));
            c.x[1] = 0;
            break;
        }
        case 0x07:
            log("svcExitProcess");
            plat::exit_process(0);
        case 0x08: {  // CreateThread(entry x1, arg x2, stack_top x3, prio w4, core w5) -> handle
            auto t = new_thread(c.x[1], c.x[2], c.x[3], int(c.x[4]), int32_t(c.x[5]));
            c.x[1] = t->handle;
            if (g_trace) log("CreateThread %s handle %x", describe(t->entry).c_str(), t->handle);
            break;
        }
        case 0x09: {  // StartThread(handle)
            auto t = std::dynamic_pointer_cast<KThread>(handle_get(uint32_t(c.x[0])));
            if (!t) {
                rc = 0xE401;
                break;
            }
            t->started = true;
            plat::start_thread(thread_main, t.get(), 64ull << 20);  // host stack: recompiled calls nest deeply
            break;
        }
        case 0x0A: {  // ExitThread
            {
                // The lock must be released before the host thread ends: pthread_exit runs no C++ destructors on
                // macOS, so a lock_guard alive across it kept the kernel locked for good (Wonder, ~frame 50).
                std::lock_guard lk(g_k);
                t_self->exited = true;
                notify_all();
            }
            plat::exit_thread();
        }
        case 0x0B: {  // SleepThread(ns)
            int64_t ns = int64_t(c.x[0]);
            if (ns > 0) {
                g_sleep_calls.fetch_add(1, std::memory_order_relaxed);
                plat::sleep_ns(ns);
                return;
            }
            // Yield (0, -1, -2). On the Switch a thread yielding in a loop only takes time nobody else wants; here
            // each yield is a syscall on a real core (the network thread polls a flag this way and burned a whole
            // core, heating the phone). Past 32 yields in a row (each within 2 ms of the last), every yield sleeps
            // 0.5 ms until the thread does something else for a while.
            g_yield_calls.fetch_add(1, std::memory_order_relaxed);
            KThread* t = t_self;
            uint64_t now = plat::monotonic_ns();
            if (now - t->last_yield > 2'000'000) t->yields = 0;
            if (!spin_naps() || ++t->yields <= 32)
                plat::yield();
            else
                plat::sleep_ns(500'000);
            t->last_yield = plat::monotonic_ns();
            return;
        }
        case 0x0C: {  // GetThreadPriority(handle) -> prio
            auto t = std::dynamic_pointer_cast<KThread>(handle_get(uint32_t(c.x[1])));
            c.x[1] = t ? t->prio : 44;
            break;
        }
        case 0x0D: {  // SetThreadPriority(handle, prio)
            auto t = std::dynamic_pointer_cast<KThread>(handle_get(uint32_t(c.x[0])));
            if (t) t->prio = int(c.x[1]);
            break;
        }
        case 0x0E: {  // GetThreadCoreMask(handle) -> core, mask
            auto t = std::dynamic_pointer_cast<KThread>(handle_get(uint32_t(c.x[2])));
            c.x[1] = t ? uint32_t(t->core) : 0;
            c.x[2] = 0xF;
            break;
        }
        case 0x0F:  // SetThreadCoreMask(handle, ideal core, mask)
            rc = set_thread_core_mask(uint32_t(c.x[0]), int32_t(c.x[1]), c.x[2]);
            break;
        case 0x10:  // GetCurrentProcessorNumber
            c.x[0] = t_self ? uint32_t(std::max(t_self->core, 0)) : 0;
            return;
        case 0x11: {  // SignalEvent(handle)
            auto e = std::dynamic_pointer_cast<KEvent>(handle_get(uint32_t(c.x[0])));
            if (!e) {
                rc = 0xE401;
                break;
            }
            std::lock_guard lk(g_k);
            e->state->signaled = true;
            notify_all();
            break;
        }
        case 0x12:    // ClearEvent(handle)
        case 0x17: {  // ResetSignal(handle)
            auto e = std::dynamic_pointer_cast<KEvent>(handle_get(uint32_t(c.x[0])));
            if (!e) {
                rc = 0xE401;
                break;
            }
            std::lock_guard lk(g_k);
            if (imm == 0x17 && !e->state->signaled) rc = 0xFA01;
            e->state->signaled = false;
            break;
        }
        case 0x13: {  // MapSharedMemory(handle, addr, size, perm)
            auto s = std::dynamic_pointer_cast<KSharedMemory>(handle_get(uint32_t(c.x[0])));
            if (!s) {
                rc = 0xE401;
                break;
            }
            rc = map_shared_memory(*s, c.x[1], c.x[2]);
            break;
        }
        case 0x14:  // UnmapSharedMemory
            break;
        case 0x15: {  // CreateTransferMemory(addr x1, size x2, perm w3) -> handle
            auto tm = std::make_shared<KTransferMemory>();
            tm->addr = c.x[1];
            tm->size = c.x[2];
            c.x[1] = handle_add(tm);
            break;
        }
        case 0x16:  // CloseHandle
            handle_close(uint32_t(c.x[0]));
            break;
        case 0x18:  // WaitSynchronization(out idx, handles x1, count w2, timeout x3)
            rc = svc_wait_synchronization(c, c.x[1], int32_t(c.x[2]), int64_t(c.x[3]));
            break;
        case 0x19: {  // CancelSynchronization(thread handle)
            auto t = std::dynamic_pointer_cast<KThread>(handle_get(uint32_t(c.x[0])));
            std::lock_guard lk(g_k);
            if (t) t->cancel = true;
            notify_all();
            break;
        }
        case 0x1A:
            rc = svc_arbitrate_lock(uint32_t(c.x[0]), c.x[1], uint32_t(c.x[2]));
            break;
        case 0x1B:
            rc = svc_arbitrate_unlock(c.x[0]);
            break;
        case 0x1C:
            rc = svc_wait_process_wide_key(c.x[0], c.x[1], uint32_t(c.x[2]), int64_t(c.x[3]));
            break;
        case 0x1D:
            rc = svc_signal_process_wide_key(c.x[0], int32_t(c.x[1]));
            break;
        case 0x1E:  // GetSystemTick
            c.x[0] = system_tick();
            return;
        case 0x1F: {  // ConnectToNamedPort(name x1) -> handle
            rc = ipc_connect(reinterpret_cast<const char*>(c.x[1]), c.x[1] ? &c.x[1] : nullptr);
            break;
        }
        case 0x21:  // SendSyncRequest(handle)
            rc = ipc_request(c, uint32_t(c.x[0]), t_self->tls, 0x100);
            break;
        case 0x22:  // SendSyncRequestWithUserBuffer(buf, size, handle)
            rc = ipc_request(c, uint32_t(c.x[2]), c.x[0], c.x[1]);
            break;
        case 0x24:  // GetProcessId
            c.x[1] = 0x51;
            break;
        case 0x25: {  // GetThreadId(handle) -> id
            auto t = std::dynamic_pointer_cast<KThread>(handle_get(uint32_t(c.x[1])));
            c.x[1] = t ? t->id : 0;
            break;
        }
        case 0x26:  // Break(reason, arg, size)
            if (c.x[2] == 4 && c.x[1]) {
                uint32_t r = ld<uint32_t>(c.x[1]);
                log("abort with Result %#x (module %u, description %u)", r, r & 0x1FF, (r >> 9) & 0x1FFF);
            }
            backtrace(c);
            fatal("svcBreak(%llx, %llx, %llx) from %s", (unsigned long long)c.x[0], (unsigned long long)c.x[1],
                  (unsigned long long)c.x[2], describe(c.x[30]).c_str());
        case 0x27: {  // OutputDebugString(str, len)
            std::string s(reinterpret_cast<const char*>(c.x[0]), c.x[1]);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            log("[guest] %s", s.c_str());
            break;
        }
        case 0x28:
            fatal("svcReturnFromException(%llx)", (unsigned long long)c.x[0]);
        case 0x29: {  // GetInfo(type w1, handle w2, sub x3) -> x1
            c.x[1] = get_info(uint32_t(c.x[1]), uint32_t(c.x[2]), c.x[3], rc);
            break;
        }
        case 0x2A:  // FlushEntireDataCache
            break;
        case 0x2C:  // MapPhysicalMemory(addr, size)
            commit(c.x[0], c.x[1]);
            vm_set(c.x[0], c.x[1], kMemHeap, 3);
            break;
        case 0x2D:  // UnmapPhysicalMemory
            decommit(c.x[0], c.x[1]);
            vm_set(c.x[0], c.x[1], c.x[0] - kAliasBase < kAliasSize ? kMemFree : kMemInaccessible, 0);
            break;
        case 0x34:
            rc = svc_wait_for_address(c.x[0], uint32_t(c.x[1]), int32_t(c.x[2]), int64_t(c.x[3]));
            break;
        case 0x35:
            rc = svc_signal_to_address(c.x[0], uint32_t(c.x[1]), int32_t(c.x[2]), int32_t(c.x[3]));
            break;
        case 0x36:  // SynchronizePreemptionState
            break;
        case 0x45: {  // CreateEvent -> write handle x1, read handle x2
            auto st = std::make_shared<EventState>();
            auto w = std::make_shared<KEvent>();
            auto r = std::make_shared<KEvent>();
            w->state = r->state = st;
            c.x[1] = handle_add(w);
            c.x[2] = handle_add(r);
            break;
        }
        case 0x51:  // MapTransferMemory(handle, addr, size, perm)
            rc = map_transfer_memory(uint32_t(c.x[0]), c.x[1], c.x[2]);
            break;
        case 0x52:  // UnmapTransferMemory(handle, addr, size)
            rc = unmap_transfer_memory(uint32_t(c.x[0]), c.x[1], c.x[2]);
            break;
        default:
            log("unhandled svc %#x from %s (x0 %llx x1 %llx x2 %llx)", imm, describe(pc).c_str(),
                (unsigned long long)c.x[0], (unsigned long long)c.x[1], (unsigned long long)c.x[2]);
            rc = 0;
    }
    c.x[0] = rc;
}


// NX_SVC_PROFILE=1: wall time of the game main thread's kernel calls that took 30 us or more, by call and caller,
// logged every 600 frames (where the main thread sleeps: job barriers, mutexes, waits on the replay).
namespace {
thread_local int t_svc_role = 0;  // 1 game main, 2 the thread that presents
std::mutex g_svc_lock;
std::map<std::tuple<int, uint32_t, uint64_t>, std::pair<uint64_t, uint64_t>> g_svc_prof;  // (role, svc, pc) -> (ns, calls)
bool svc_profile_on() {
    static const bool on = std::getenv("NX_SVC_PROFILE") != nullptr;
    return on;
}
}  // namespace

void svc_profile_mark(int role) { t_svc_role = role; }

void svc_profile_report(uint64_t frames, int role) {
    if (!svc_profile_on()) return;
    std::lock_guard lk(g_svc_lock);
    std::vector<std::pair<uint64_t, std::tuple<int, uint32_t, uint64_t>>> v;
    uint64_t total = 0;
    for (auto& [k, val] : g_svc_prof)
        if (std::get<0>(k) == role) v.push_back({val.first, k}), total += val.first;
    std::sort(v.rbegin(), v.rend());
    log("svc profile: role %d thread blocked %.2f ms per frame in calls over 30 us (over %llu frames)", role,
        double(total) / 1e6 / double(frames), (unsigned long long)frames);
    for (size_t i = 0; i < v.size() && i < 8; i++) {
        auto& val = g_svc_prof[v[i].second];
        log("svc profile: role %d: %.2f ms/frame, %.1f calls/frame: svc %#x from %s", role, double(val.first) / 1e6 / double(frames),
            double(val.second) / double(frames), std::get<1>(v[i].second), describe(std::get<2>(v[i].second)).c_str());
    }
    for (auto it = g_svc_prof.begin(); it != g_svc_prof.end();)
        it = std::get<0>(it->first) == role ? g_svc_prof.erase(it) : std::next(it);
}

void svc(CPU& c, uint32_t imm, uint64_t pc) {
    if (!t_svc_role || !svc_profile_on()) return svc_impl(c, imm, pc);
    uint64_t t0 = plat::monotonic_ns();
    svc_impl(c, imm, pc);
    uint64_t dt = plat::monotonic_ns() - t0;
    if (dt >= 30000) {
        std::lock_guard lk(g_svc_lock);
        auto& e = g_svc_prof[{t_svc_role, imm, pc}];
        e.first += dt, e.second++;
    }
}

}  // namespace nx
