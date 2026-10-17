// Guest CPU context and helpers used by recompiled AArch64 code.
//
// Guest memory is host memory: the module is mapped at NX_BASE and every guest pointer is a host pointer.
// Both sides are little endian, so loads and stores are plain (unaligned-safe) memory accesses.
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// Generated code adds each module's offset to this. Darwin selects and reserves a layout before the generated
// module tables initialize: newer shared caches occupy the old 0x3_0000_0000 module address.
#if defined(__APPLE__)
#include <nx/mac_address_space.h>
#endif
#ifndef NX_BASE
#if defined(__APPLE__)
#define NX_BASE (nx::mac_address_space().modules)
#else
#define NX_BASE 0x0000004000000000ull
#endif
#endif

namespace nx {

union alignas(16) V128 {
    uint8_t u8[16];
    int8_t s8[16];
    uint16_t u16[8];
    int16_t s16[8];
    uint32_t u32[4];
    int32_t s32[4];
    uint64_t u64[2];
    int64_t s64[2];
    float f32[4];
    double f64[2];
};

struct CPU {
    uint64_t x[32];  // x[31] is SP; XZR is handled by the recompiler
    V128 v[32];
    uint8_t nf, zf, cf, vf;
    uint64_t fpcr, fpsr;
    uint64_t tpidr, tpidrro;
    uint64_t excl_addr = ~0ull;
    uint64_t excl_val, excl_val_hi;
};

using Fn = void (*)(CPU&);

// Provided by the runtime.
void call(CPU& c, uint64_t target);  // indirect call / tail call through the function table
[[noreturn]] void unimpl(CPU& c, uint64_t pc, const char* what);
void svc(CPU& c, uint32_t imm, uint64_t pc);
uint64_t mrs(CPU& c, uint32_t sysreg);
void msr(CPU& c, uint32_t sysreg, uint64_t value);
void trace_sp(CPU& c, uint64_t pc);  // debug (NX_TRACE_SP at recompile time)

// ---- setjmp / longjmp -----------------------------------------------------------------------------------------
// A guest longjmp throws LongJmp; the recompiled function that called setjmp on that buffer catches it, restores
// the callee-saved registers it saved at setjmp time and resumes right after the setjmp call.

struct LongJmp {
    uint64_t buf;
    uint64_t val;
};

struct JmpState {
    uint64_t buf = 0;
    uint64_t x[13];  // x19..x30, sp
    uint64_t d[8];   // d8..d15
    void save(const CPU& c) {
        buf = c.x[0];
        for (int k = 0; k < 13; k++) x[k] = c.x[19 + k];
        for (int k = 0; k < 8; k++) d[k] = c.v[8 + k].u64[0];
    }
    void restore(CPU& c) const {
        for (int k = 0; k < 13; k++) c.x[19 + k] = x[k];
        for (int k = 0; k < 8; k++) {
            c.v[8 + k].u64[0] = d[k];
            c.v[8 + k].u64[1] = 0;
        }
    }
};

// Helpers the generated code calls millions of times: always inlined (the compiler then drops flag bits nobody
// reads; left to itself it kept adds32/adds64 as calls in the huge generated functions).
#if defined(_MSC_VER)
#define NX_INLINE __forceinline
#define NX_RESTRICT
#else
#define NX_INLINE inline __attribute__((always_inline))
// The generated functions' CPU context: guest memory never overlaps it (the guest cannot see the host heap), so the
// compiler may keep guest registers in host registers across guest loads and stores instead of reloading them.
#define NX_RESTRICT __restrict
#endif

// ---- memory -------------------------------------------------------------------------------------------------

template <typename T>
NX_INLINE T ld(uint64_t a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T));
    return v;
}

template <typename T>
NX_INLINE void st(uint64_t a, T v) {
    std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T));
}

template <typename T>
NX_INLINE T ld_acq(uint64_t a) {
    return __atomic_load_n(reinterpret_cast<T*>(a), __ATOMIC_ACQUIRE);
}

template <typename T>
NX_INLINE void st_rel(uint64_t a, T v) {
    __atomic_store_n(reinterpret_cast<T*>(a), v, __ATOMIC_RELEASE);
}

// Exclusive monitor: LDXR remembers (address, value); STXR succeeds if memory still holds that value.
template <typename T>
NX_INLINE T ldxr(CPU& c, uint64_t a) {
    T v = __atomic_load_n(reinterpret_cast<T*>(a), __ATOMIC_ACQUIRE);
    c.excl_addr = a;
    c.excl_val = v;
    return v;
}

template <typename T>
NX_INLINE uint32_t stxr(CPU& c, uint64_t a, T v) {
    if (c.excl_addr != a) return 1;
    c.excl_addr = ~0ull;
    T expected = static_cast<T>(c.excl_val);
    return __atomic_compare_exchange_n(reinterpret_cast<T*>(a), &expected, v, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST)
               ? 0
               : 1;
}

NX_INLINE void clrex(CPU& c) { c.excl_addr = ~0ull; }

// Exclusive pairs (LDXP/STXP). Two 32-bit registers are one 64-bit access; two 64-bit registers are one 128-bit
// access, compared and swapped as a whole (the monitor keeps the high half in excl_val_hi).
template <typename T>
NX_INLINE void ldxp(CPU& c, uint64_t a, T& lo, T& hi) {
    if constexpr (sizeof(T) == 4) {
        uint64_t v = ldxr<uint64_t>(c, a);
        lo = T(v), hi = T(v >> 32);
    } else {
        unsigned __int128 v = __atomic_load_n(reinterpret_cast<unsigned __int128*>(a), __ATOMIC_ACQUIRE);
        c.excl_addr = a;
        c.excl_val = uint64_t(v), c.excl_val_hi = uint64_t(v >> 64);
        lo = T(v), hi = T(v >> 64);
    }
}

template <typename T>
NX_INLINE uint32_t stxp(CPU& c, uint64_t a, T lo, T hi) {
    if constexpr (sizeof(T) == 4) {
        return stxr<uint64_t>(c, a, uint64_t(lo) | (uint64_t(hi) << 32));
    } else {
        if (c.excl_addr != a) return 1;
        c.excl_addr = ~0ull;
        unsigned __int128 expected = (unsigned __int128)c.excl_val_hi << 64 | c.excl_val;
        unsigned __int128 v = (unsigned __int128)hi << 64 | lo;
        return __atomic_compare_exchange_n(reinterpret_cast<unsigned __int128*>(a), &expected, v, false,
                                           __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
                   ? 0
                   : 1;
    }
}

// LDSMAX / LDSMIN / LDUMAX / LDUMIN: no builtin, a compare-exchange loop. Returns the old value.
template <typename T, bool Signed, bool Max>
NX_INLINE T amo_minmax(T* p, T v) {
    using C = std::conditional_t<Signed, std::make_signed_t<T>, T>;
    T old = __atomic_load_n(p, __ATOMIC_RELAXED);
    for (;;) {
        T nv = (Max ? C(v) > C(old) : C(v) < C(old)) ? v : old;
        if (__atomic_compare_exchange_n(p, &old, nv, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) return old;
    }
}

NX_INLINE void barrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

// ---- flags --------------------------------------------------------------------------------------------------

NX_INLINE uint64_t adds64(CPU& c, uint64_t a, uint64_t b, uint64_t carry) {
    uint64_t r = a + b + carry;
    c.nf = r >> 63;
    c.zf = r == 0;
    c.cf = carry ? (r <= a) : (r < a);
    c.vf = ((a ^ r) & (b ^ r)) >> 63;
    return r;
}

NX_INLINE uint32_t adds32(CPU& c, uint32_t a, uint32_t b, uint32_t carry) {
    uint64_t wide = uint64_t(a) + b + carry;
    uint32_t r = uint32_t(wide);
    c.nf = r >> 31;
    c.zf = r == 0;
    c.cf = wide >> 32;
    c.vf = ((a ^ r) & (b ^ r)) >> 31;
    return r;
}

NX_INLINE uint64_t subs64(CPU& c, uint64_t a, uint64_t b) { return adds64(c, a, ~b, 1); }
NX_INLINE uint32_t subs32(CPU& c, uint32_t a, uint32_t b) { return adds32(c, a, ~b, 1); }

NX_INLINE void logic64(CPU& c, uint64_t r) {
    c.nf = r >> 63;
    c.zf = r == 0;
    c.cf = 0;
    c.vf = 0;
}

NX_INLINE void logic32(CPU& c, uint32_t r) {
    c.nf = r >> 31;
    c.zf = r == 0;
    c.cf = 0;
    c.vf = 0;
}

NX_INLINE void set_nzcv(CPU& c, uint32_t nzcv) {
    c.nf = (nzcv >> 3) & 1;
    c.zf = (nzcv >> 2) & 1;
    c.cf = (nzcv >> 1) & 1;
    c.vf = nzcv & 1;
}

NX_INLINE uint64_t get_nzcv(const CPU& c) {
    return (uint64_t(c.nf) << 31) | (uint64_t(c.zf) << 30) | (uint64_t(c.cf) << 29) | (uint64_t(c.vf) << 28);
}

template <typename F>
NX_INLINE void fcmp(CPU& c, F a, F b) {
    if (std::isnan(a) || std::isnan(b))
        set_nzcv(c, 0b0011);
    else if (a == b)
        set_nzcv(c, 0b0110);
    else if (a < b)
        set_nzcv(c, 0b1000);
    else
        set_nzcv(c, 0b0010);
}

// ---- integer helpers ----------------------------------------------------------------------------------------

inline uint16_t bswap(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t bswap(uint32_t v) { return __builtin_bswap32(v); }
inline uint64_t bswap(uint64_t v) { return __builtin_bswap64(v); }

// CRC32 / CRC32C (reflected polynomials), `size` bytes of `data` into `acc`.
template <int size>
inline uint32_t crc32(uint32_t acc, uint64_t data, uint32_t poly) {
    for (int k = 0; k < size * 8; k++) {
        uint32_t bit = (acc ^ uint32_t(data >> k)) & 1;
        acc = (acc >> 1) ^ (bit ? poly : 0);
    }
    return acc;
}

inline uint64_t ror64(uint64_t v, unsigned s) { return std::rotr(v, int(s & 63)); }
inline uint32_t ror32(uint32_t v, unsigned s) { return std::rotr(v, int(s & 31)); }
inline uint64_t asr64(uint64_t v, unsigned s) { return uint64_t(int64_t(v) >> (s & 63)); }
inline uint32_t asr32(uint32_t v, unsigned s) { return uint32_t(int32_t(v) >> (s & 31)); }

inline uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
inline int64_t sdiv64(int64_t a, int64_t b) {
    if (!b) return 0;
    if (a == std::numeric_limits<int64_t>::min() && b == -1) return a;
    return a / b;
}
inline int32_t sdiv32(int32_t a, int32_t b) {
    if (!b) return 0;
    if (a == std::numeric_limits<int32_t>::min() && b == -1) return a;
    return a / b;
}

inline uint64_t umulh(uint64_t a, uint64_t b) {
#if defined(_MSC_VER)
    return __umulh(a, b);
#else
    return uint64_t((unsigned __int128)a * b >> 64);
#endif
}
inline int64_t smulh(int64_t a, int64_t b) {
#if defined(_MSC_VER)
    return __mulh(a, b);
#else
    return int64_t((__int128)a * b >> 64);
#endif
}

inline uint64_t rbit64(uint64_t v) {
    v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
    v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
    v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
    return nx::bswap(v);
}
inline uint32_t rbit32(uint32_t v) { return uint32_t(rbit64(v) >> 32); }
inline uint64_t rev16_64(uint64_t v) { return ((v >> 8) & 0x00FF00FF00FF00FFull) | ((v & 0x00FF00FF00FF00FFull) << 8); }
inline uint32_t rev16_32(uint32_t v) { return ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8); }
inline uint64_t rev32_64(uint64_t v) {
    return uint64_t(nx::bswap(uint32_t(v))) | (uint64_t(nx::bswap(uint32_t(v >> 32))) << 32);
}

// ---- floating point -----------------------------------------------------------------------------------------

inline float f16_to_f32(uint16_t h) {
    uint32_t s = uint32_t(h >> 15) << 31, e = (h >> 10) & 0x1F, m = h & 0x3FF, bits;
    if (e == 0) {
        if (m == 0) {
            bits = s;
        } else {  // subnormal
            e = 127 - 15 + 1;
            while (!(m & 0x400)) {
                m <<= 1;
                e--;
            }
            bits = s | (e << 23) | ((m & 0x3FF) << 13);
        }
    } else if (e == 31) {
        bits = s | 0x7F800000u | (m << 13);
    } else {
        bits = s | ((e + 127 - 15) << 23) | (m << 13);
    }
    return std::bit_cast<float>(bits);
}

inline uint16_t f32_to_f16(float f) {
    uint32_t x = std::bit_cast<uint32_t>(f);
    uint32_t s = (x >> 16) & 0x8000;
    int32_t e = int32_t((x >> 23) & 0xFF) - 127 + 15;
    uint32_t m = x & 0x7FFFFF;
    if (((x >> 23) & 0xFF) == 0xFF) return uint16_t(s | 0x7C00 | (m ? 0x200 | (m >> 13) : 0));
    if (e >= 31) return uint16_t(s | 0x7C00);
    if (e <= 0) {
        if (e < -10) return uint16_t(s);
        m |= 0x800000;
        uint32_t shift = uint32_t(14 - e);
        uint32_t r = m >> shift, rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (r & 1))) r++;
        return uint16_t(s | r);
    }
    uint32_t r = (uint32_t(e) << 10) | (m >> 13), rem = m & 0x1FFF;
    if (rem > 0x1000 || (rem == 0x1000 && (r & 1))) r++;
    return uint16_t(s | r);
}

// Float -> integer with AArch64 saturation (NaN -> 0). Rounding already applied by the caller.
template <typename I, typename F>
inline I fcvt_sat(F v) {
    if (std::isnan(v)) return 0;
    if (v <= F(std::numeric_limits<I>::min())) return std::numeric_limits<I>::min();
    if (v >= F(std::numeric_limits<I>::max())) return std::numeric_limits<I>::max();
    return I(v);
}

template <typename F>
inline F round_away(F v) { return std::round(v); }
template <typename F>
inline F round_even(F v) { return std::nearbyint(v); }  // host default rounding: nearest-even

template <typename F>
inline bool is_snan(F v) {
    if constexpr (sizeof(F) == 4)
        return std::isnan(v) && !(std::bit_cast<uint32_t>(v) & 0x00400000u);
    else
        return std::isnan(v) && !(std::bit_cast<uint64_t>(v) & 0x0008000000000000ull);
}

template <typename F>
inline F quiet(F v) {
    if constexpr (sizeof(F) == 4)
        return std::bit_cast<F>(std::bit_cast<uint32_t>(v) | 0x00400000u);
    else
        return std::bit_cast<F>(std::bit_cast<uint64_t>(v) | 0x0008000000000000ull);
}

// FMAXNM/FMINNM: a quiet NaN is a missing value, a signaling NaN propagates (quieted); +0 > -0.
template <typename F>
inline F fmaxnm(F a, F b) {
    if (is_snan(a)) return quiet(a);
    if (is_snan(b)) return quiet(b);
    if (std::isnan(a)) return std::isnan(b) ? a : b;
    if (std::isnan(b)) return a;
    if (a == 0 && b == 0) return std::signbit(a) ? b : a;
    return a > b ? a : b;
}
template <typename F>
inline F fminnm(F a, F b) {
    if (is_snan(a)) return quiet(a);
    if (is_snan(b)) return quiet(b);
    if (std::isnan(a)) return std::isnan(b) ? a : b;
    if (std::isnan(b)) return a;
    if (a == 0 && b == 0) return std::signbit(a) ? a : b;
    return a < b ? a : b;
}
template <typename F>
inline F fmax_(F a, F b) {
    if (std::isnan(a) || std::isnan(b)) return a + b;
    if (a == 0 && b == 0) return std::signbit(a) ? b : a;
    return a > b ? a : b;
}
template <typename F>
inline F fmin_(F a, F b) {
    if (std::isnan(a) || std::isnan(b)) return a + b;
    if (a == 0 && b == 0) return std::signbit(a) ? a : b;
    return a < b ? a : b;
}

// FRSQRTS/FRECPS: (0 * inf) gives the exact step result instead of NaN.
template <typename F>
inline F frsqrts(F a, F b) {
    if ((std::isinf(a) && b == 0) || (a == 0 && std::isinf(b))) return F(1.5);
    // Fused on hardware, (3 - a * b) / 2 rounded once. Halving a normal operand is exact, so fold the / 2 into it
    // and the fma rounds once without overflowing in between.
    const F lim = std::numeric_limits<F>::min() * 2;
    if (std::fabs(a) >= lim) return std::fma(-(a * F(0.5)), b, F(1.5));
    if (std::fabs(b) >= lim) return std::fma(-a, b * F(0.5), F(1.5));
    return std::fma(-a, b, F(3)) / F(2);
}

template <typename F>
inline F frecps(F a, F b) {
    if ((std::isinf(a) && b == 0) || (a == 0 && std::isinf(b))) return F(2);
    return std::fma(-a, b, F(2));  // fused on hardware
}

// ---- vector helpers -----------------------------------------------------------------------------------------

inline V128 vzero() {
    V128 r;
    r.u64[0] = r.u64[1] = 0;
    return r;
}

template <typename T>
inline T sat(int64_t v) {
    if (v < int64_t(std::numeric_limits<T>::min())) return std::numeric_limits<T>::min();
    if (v > int64_t(std::numeric_limits<T>::max())) return std::numeric_limits<T>::max();
    return T(v);
}

// ---- crypto extension (AES, SHA-1, SHA-256, PMULL.1Q) ----------------------------------------------------------
// Used by the SDK (nn::crypto, ssl, fs). Hosts with the same instructions (arm64 built with +aes/+sha2) use them;
// everywhere else, or with NX_SOFT_CRYPTO, the ARM pseudocode below. Only instantiated by modules that need it.

#if defined(__aarch64__) && defined(__ARM_FEATURE_AES) && defined(__ARM_FEATURE_SHA2) && !defined(NX_SOFT_CRYPTO)
#define NX_HW_CRYPTO 1
}  // namespace nx
#include <arm_neon.h>
namespace nx {
inline uint8x16_t to_neon(V128 v) { return vld1q_u8(v.u8); }
inline V128 from_neon(uint8x16_t n) { V128 v; vst1q_u8(v.u8, n); return v; }
#endif

namespace crypto_detail {
inline const uint8_t* aes_sbox() {
    static const uint8_t s[256] = {
        0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,
        0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
        0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,
        0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
        0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,
        0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
        0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,
        0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
        0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,
        0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
        0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
    return s;
}
inline const uint8_t* aes_inv_sbox() {
    static uint8_t inv[256];
    static const bool ready = [] {
        for (int k = 0; k < 256; k++) inv[aes_sbox()[k]] = uint8_t(k);
        return true;
    }();
    (void)ready;
    return inv;
}
inline uint8_t xtime(uint8_t b) { return uint8_t((b << 1) ^ ((b & 0x80) ? 0x1b : 0)); }
inline uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (; b; b >>= 1, a = xtime(a))
        if (b & 1) r ^= a;
    return r;
}
// State byte k is row k % 4, column k / 4 (FIPS-197 column-major, as AArch64 holds it).
inline V128 shift_rows(V128 s, bool inverse) {
    V128 r;
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            int src = inverse ? (col - row + 4) % 4 : (col + row) % 4;
            r.u8[col * 4 + row] = s.u8[src * 4 + row];
        }
    return r;
}
inline V128 mix_columns(V128 s, bool inverse) {
    static const uint8_t fwd[4] = {2, 3, 1, 1}, inv[4] = {14, 11, 13, 9};
    const uint8_t* m = inverse ? inv : fwd;
    V128 r;
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            uint8_t v = 0;
            for (int k = 0; k < 4; k++) v ^= gmul(s.u8[col * 4 + k], m[(k - row + 4) % 4]);
            r.u8[col * 4 + row] = v;
        }
    return r;
}
inline uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace crypto_detail

// AESE: SubBytes(ShiftRows(d ^ key)); AESD the inverse; AESMC / AESIMC: (Inv)MixColumns.
inline V128 aese(V128 d, V128 k) {
#ifdef NX_HW_CRYPTO
    return from_neon(vaeseq_u8(to_neon(d), to_neon(k)));
#else
    for (int j = 0; j < 16; j++) d.u8[j] ^= k.u8[j];
    d = crypto_detail::shift_rows(d, false);
    for (auto& b : d.u8) b = crypto_detail::aes_sbox()[b];
    return d;
#endif
}
inline V128 aesd(V128 d, V128 k) {
#ifdef NX_HW_CRYPTO
    return from_neon(vaesdq_u8(to_neon(d), to_neon(k)));
#else
    for (int j = 0; j < 16; j++) d.u8[j] ^= k.u8[j];
    d = crypto_detail::shift_rows(d, true);
    for (auto& b : d.u8) b = crypto_detail::aes_inv_sbox()[b];
    return d;
#endif
}
inline V128 aesmc(V128 s) {
#ifdef NX_HW_CRYPTO
    return from_neon(vaesmcq_u8(to_neon(s)));
#else
    return crypto_detail::mix_columns(s, false);
#endif
}
inline V128 aesimc(V128 s) {
#ifdef NX_HW_CRYPTO
    return from_neon(vaesimcq_u8(to_neon(s)));
#else
    return crypto_detail::mix_columns(s, true);
#endif
}

// SHA-1: SHA1C / SHA1P / SHA1M (choose / parity / majority), 4 rounds on (X = d, Y = e) with W = w.
inline V128 sha1_rounds(V128 x, uint32_t y, V128 w, int kind) {
    using namespace crypto_detail;
    for (int e = 0; e < 4; e++) {
        uint32_t b = x.u32[1], c = x.u32[2], d = x.u32[3], t;
        if (kind == 0) t = (b & c) | (~b & d);
        else if (kind == 1) t = b ^ c ^ d;
        else t = (b & c) | (b & d) | (c & d);
        y = y + rol(x.u32[0], 5) + t + w.u32[e];
        x.u32[1] = rol(x.u32[1], 30);
        uint32_t top = x.u32[3];  // <Y, X> = ROL(Y:X, 32)
        x.u32[3] = x.u32[2], x.u32[2] = x.u32[1], x.u32[1] = x.u32[0], x.u32[0] = y;
        y = top;
    }
    return x;
}
inline V128 sha1c(V128 d, uint32_t e, V128 w) { return sha1_rounds(d, e, w, 0); }
inline V128 sha1p(V128 d, uint32_t e, V128 w) { return sha1_rounds(d, e, w, 1); }
inline V128 sha1m(V128 d, uint32_t e, V128 w) { return sha1_rounds(d, e, w, 2); }
inline uint32_t sha1h(uint32_t e) { return crypto_detail::rol(e, 30); }
inline V128 sha1su0(V128 d, V128 n, V128 m) {
    V128 r;
    r.u64[0] = d.u64[1], r.u64[1] = n.u64[0];
    r.u64[0] ^= d.u64[0] ^ m.u64[0], r.u64[1] ^= d.u64[1] ^ m.u64[1];
    return r;
}
inline V128 sha1su1(V128 d, V128 n) {
    using namespace crypto_detail;
    uint32_t t[4] = {d.u32[0] ^ n.u32[1], d.u32[1] ^ n.u32[2], d.u32[2] ^ n.u32[3], d.u32[3]};
    V128 r;
    for (int k = 0; k < 4; k++) r.u32[k] = rol(t[k], 1);
    r.u32[3] ^= rol(t[0], 2);
    return r;
}

// SHA-256: SHA256H returns X (the first half of the state), SHA256H2 returns Y; 4 rounds each.
inline V128 sha256_hash(V128 x, V128 y, V128 w, bool part1) {
    using namespace crypto_detail;
    for (int e = 0; e < 4; e++) {
        uint32_t ch = (y.u32[0] & y.u32[1]) | (~y.u32[0] & y.u32[2]);
        uint32_t maj = (x.u32[0] & x.u32[1]) | (x.u32[0] & x.u32[2]) | (x.u32[1] & x.u32[2]);
        uint32_t s1 = ror(y.u32[0], 6) ^ ror(y.u32[0], 11) ^ ror(y.u32[0], 25);
        uint32_t s0 = ror(x.u32[0], 2) ^ ror(x.u32[0], 13) ^ ror(x.u32[0], 22);
        uint32_t t = y.u32[3] + s1 + ch + w.u32[e];
        x.u32[3] = t + x.u32[3];
        y.u32[3] = t + s0 + maj;
        uint32_t x3 = x.u32[3], y3 = y.u32[3];  // <Y, X> = ROL(Y:X, 32)
        x.u32[3] = x.u32[2], x.u32[2] = x.u32[1], x.u32[1] = x.u32[0], x.u32[0] = y3;
        y.u32[3] = y.u32[2], y.u32[2] = y.u32[1], y.u32[1] = y.u32[0], y.u32[0] = x3;
    }
    return part1 ? x : y;
}
inline V128 sha256h(V128 d, V128 n, V128 w) {
#ifdef NX_HW_CRYPTO
    return from_neon(vreinterpretq_u8_u32(vsha256hq_u32(vreinterpretq_u32_u8(to_neon(d)), vreinterpretq_u32_u8(to_neon(n)),
                                                        vreinterpretq_u32_u8(to_neon(w)))));
#else
    return sha256_hash(d, n, w, true);
#endif
}
inline V128 sha256h2(V128 d, V128 n, V128 w) {
#ifdef NX_HW_CRYPTO
    return from_neon(vreinterpretq_u8_u32(vsha256h2q_u32(vreinterpretq_u32_u8(to_neon(d)), vreinterpretq_u32_u8(to_neon(n)),
                                                         vreinterpretq_u32_u8(to_neon(w)))));
#else
    return sha256_hash(n, d, w, false);
#endif
}
inline V128 sha256su0(V128 d, V128 n) {
    using namespace crypto_detail;
    uint32_t t[4] = {d.u32[1], d.u32[2], d.u32[3], n.u32[0]};
    V128 r;
    for (int e = 0; e < 4; e++) r.u32[e] = (ror(t[e], 7) ^ ror(t[e], 18) ^ (t[e] >> 3)) + d.u32[e];
    return r;
}
inline V128 sha256su1(V128 d, V128 n, V128 m) {
    using namespace crypto_detail;
    uint32_t t0[4] = {n.u32[1], n.u32[2], n.u32[3], m.u32[0]};
    auto sig1 = [](uint32_t v) { return ror(v, 17) ^ ror(v, 19) ^ (v >> 10); };
    V128 r;
    r.u32[0] = sig1(m.u32[2]) + d.u32[0] + t0[0];
    r.u32[1] = sig1(m.u32[3]) + d.u32[1] + t0[1];
    r.u32[2] = sig1(r.u32[0]) + d.u32[2] + t0[2];
    r.u32[3] = sig1(r.u32[1]) + d.u32[3] + t0[3];
    return r;
}

// PMULL.1Q: carry-less 64 x 64 -> 128.
inline V128 clmul64(uint64_t a, uint64_t b) {
    uint64_t lo = 0, hi = 0;
    for (int k = 0; k < 64; k++)
        if ((b >> k) & 1) {
            lo ^= a << k;
            if (k) hi ^= a >> (64 - k);
        }
    V128 r;
    r.u64[0] = lo, r.u64[1] = hi;
    return r;
}

}  // namespace nx
