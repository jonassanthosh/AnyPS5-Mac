#ifndef CORE_TRANSLATOR_RUNTIME_INCLUDE_TRANSLATOR_SEMANTICS_HPP
#define CORE_TRANSLATOR_RUNTIME_INCLUDE_TRANSLATOR_SEMANTICS_HPP

#include "translator/GuestContext.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <type_traits>

#ifndef APS5_TRANSLATOR_TSO
#define APS5_TRANSLATOR_TSO 1
#endif

#ifndef APS5_TRANSLATOR_VECTOR_TSO
#define APS5_TRANSLATOR_VECTOR_TSO 1
#endif

namespace translator {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u128 = unsigned __int128;
using i128 = __int128;

constexpr u32 FlagCf = 1u << 0;
constexpr u32 FlagPf = 1u << 2;
constexpr u32 FlagAf = 1u << 4;
constexpr u32 FlagZf = 1u << 6;
constexpr u32 FlagSf = 1u << 7;
constexpr u32 FlagOf = 1u << 11;

template <std::size_t TSize>
struct SameSizeInteger;
template <> struct SameSizeInteger<1> { using Type = u8; };
template <> struct SameSizeInteger<2> { using Type = u16; };
template <> struct SameSizeInteger<4> { using Type = u32; };
template <> struct SameSizeInteger<8> { using Type = u64; };

template <typename T>
constexpr bool IsScalarAccess = sizeof(T) <= 8 && (sizeof(T) & (sizeof(T) - 1)) == 0 && std::is_trivially_copyable_v<T>;

template <typename T>
inline T Ld(u64 address) {
    T value;
#if APS5_TRANSLATOR_TSO
    if constexpr (IsScalarAccess<T>) {
        if ((address & (sizeof(T) - 1)) == 0) {
            using Bits = typename SameSizeInteger<sizeof(T)>::Type;
            Bits bits = __atomic_load_n(reinterpret_cast<const Bits*>(address), __ATOMIC_ACQUIRE);
            std::memcpy(&value, &bits, sizeof(T));
            return value;
        }
    }
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    if constexpr (APS5_TRANSLATOR_VECTOR_TSO || IsScalarAccess<T>) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    }
#else
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
#endif
    return value;
}

template <typename T>
inline void St(u64 address, T value) {
#if APS5_TRANSLATOR_TSO
    if constexpr (IsScalarAccess<T>) {
        if ((address & (sizeof(T) - 1)) == 0) {
            using Bits = typename SameSizeInteger<sizeof(T)>::Type;
            Bits bits;
            std::memcpy(&bits, &value, sizeof(T));
            __atomic_store_n(reinterpret_cast<Bits*>(address), bits, __ATOMIC_RELEASE);
            return;
        }
    }
    if constexpr (APS5_TRANSLATOR_VECTOR_TSO || IsScalarAccess<T>) {
        __atomic_thread_fence(__ATOMIC_RELEASE);
    }
#endif
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
}

template <typename T>
inline T LdStack(u64 address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

template <typename T>
inline void StStack(u64 address, T value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
}

template <typename T>
inline T& AtomicRef(u64 address) {
    return *reinterpret_cast<T*>(address);
}

inline u8 Parity(u64 value) {
    return static_cast<u8>((__builtin_popcount(static_cast<u32>(value & 0xff)) & 1) == 0);
}

template <typename T>
constexpr u32 SignShift = sizeof(T) * 8 - 1;

template <u32 TMask, typename T>
inline void FlagsResult(GuestContext* c, T r) {
    if constexpr ((TMask & FlagZf) != 0) c->zf = r == 0;
    if constexpr ((TMask & FlagSf) != 0) c->sf = static_cast<u8>(r >> SignShift<T>);
    if constexpr ((TMask & FlagPf) != 0) c->pf = Parity(r);
}

template <u32 TMask, typename T>
inline void FlagsLogic(GuestContext* c, T r) {
    FlagsResult<TMask>(c, r);
    if constexpr ((TMask & FlagCf) != 0) c->cf = 0;
    if constexpr ((TMask & FlagOf) != 0) c->of = 0;
    if constexpr ((TMask & FlagAf) != 0) c->af = 0;
}

template <u32 TMask, typename T>
inline T AddWithFlags(GuestContext* c, T a, T b, u8 carryIn) {
    T r = static_cast<T>(a + b + carryIn);
    if constexpr ((TMask & FlagCf) != 0) {
        T partial;
        bool c1 = __builtin_add_overflow(a, b, &partial);
        T full;
        bool c2 = __builtin_add_overflow(partial, static_cast<T>(carryIn), &full);
        c->cf = c1 || c2;
    }
    if constexpr ((TMask & FlagOf) != 0) c->of = static_cast<u8>(((a ^ r) & (b ^ r)) >> SignShift<T>);
    if constexpr ((TMask & FlagAf) != 0) c->af = static_cast<u8>(((a ^ b ^ r) >> 4) & 1);
    FlagsResult<TMask>(c, r);
    return r;
}

template <u32 TMask, typename T>
inline T SubWithFlags(GuestContext* c, T a, T b, u8 borrowIn) {
    T r = static_cast<T>(a - b - borrowIn);
    if constexpr ((TMask & FlagCf) != 0) {
        T partial;
        bool b1 = __builtin_sub_overflow(a, b, &partial);
        T full;
        bool b2 = __builtin_sub_overflow(partial, static_cast<T>(borrowIn), &full);
        c->cf = b1 || b2;
    }
    if constexpr ((TMask & FlagOf) != 0) c->of = static_cast<u8>(((a ^ b) & (a ^ r)) >> SignShift<T>);
    if constexpr ((TMask & FlagAf) != 0) c->af = static_cast<u8>(((a ^ b ^ r) >> 4) & 1);
    FlagsResult<TMask>(c, r);
    return r;
}

template <u32 TMask, typename T>
inline T ShlWithFlags(GuestContext* c, T a, u32 count) {
    if (count == 0) return a;
    T r = static_cast<T>(a << count);
    u8 carry = count <= sizeof(T) * 8 ? static_cast<u8>((a >> (sizeof(T) * 8 - count)) & 1) : 0;
    if constexpr ((TMask & FlagCf) != 0) c->cf = carry;
    if constexpr ((TMask & FlagOf) != 0) c->of = static_cast<u8>((r >> SignShift<T>) ^ carry);
    FlagsResult<TMask>(c, r);
    return r;
}

template <u32 TMask, typename T>
inline T ShrWithFlags(GuestContext* c, T a, u32 count) {
    if (count == 0) return a;
    T r = count < sizeof(T) * 8 ? static_cast<T>(a >> count) : 0;
    if constexpr ((TMask & FlagCf) != 0) c->cf = static_cast<u8>((a >> (count - 1)) & 1);
    if constexpr ((TMask & FlagOf) != 0) c->of = static_cast<u8>(a >> SignShift<T>);
    FlagsResult<TMask>(c, r);
    return r;
}

template <u32 TMask, typename T>
inline T SarWithFlags(GuestContext* c, T a, u32 count) {
    if (count == 0) return a;
    using S = std::make_signed_t<T>;
    T r = static_cast<T>(static_cast<S>(a) >> count);
    if constexpr ((TMask & FlagCf) != 0) c->cf = static_cast<u8>((static_cast<S>(a) >> (count - 1)) & 1);
    if constexpr ((TMask & FlagOf) != 0) c->of = 0;
    FlagsResult<TMask>(c, r);
    return r;
}

template <typename T>
inline T Rol(T a, u32 count) {
    count &= sizeof(T) * 8 - 1;
    return count == 0 ? a : static_cast<T>((a << count) | (a >> (sizeof(T) * 8 - count)));
}

template <typename T>
inline T Ror(T a, u32 count) {
    count &= sizeof(T) * 8 - 1;
    return count == 0 ? a : static_cast<T>((a >> count) | (a << (sizeof(T) * 8 - count)));
}

template <typename T>
inline T ReadVec(const GuestContext* c, u32 index) {
    T value;
    std::memcpy(&value, c->ymm[index], sizeof(T));
    return value;
}

template <typename T>
inline void WriteVec(GuestContext* c, u32 index, T value, bool zeroUpper) {
    std::memcpy(c->ymm[index], &value, sizeof(T));
    if (zeroUpper && sizeof(T) < 32) std::memset(c->ymm[index] + sizeof(T), 0, 32 - sizeof(T));
}

template <typename T>
inline void WriteVecLow(GuestContext* c, u32 index, T value) {
    std::memcpy(c->ymm[index], &value, sizeof(T));
}

inline i32 CvtF64ToI32(double v) {
    if (std::isnan(v) || v >= 2147483648.0 || v < -2147483648.0) return INT32_MIN;
    return static_cast<i32>(v);
}

inline i64 CvtF64ToI64(double v) {
    if (std::isnan(v) || v >= 9223372036854775808.0 || v < -9223372036854775808.0) return INT64_MIN;
    return static_cast<i64>(v);
}

inline i32 CvtRoundF64ToI32(double v) {
    return CvtF64ToI32(std::nearbyint(v));
}

inline i64 CvtRoundF64ToI64(double v) {
    return CvtF64ToI64(std::nearbyint(v));
}

template <typename T>
inline T SseMin(T a, T b) {
    return a < b ? a : b;
}

template <typename T>
inline T SseMax(T a, T b) {
    return a > b ? a : b;
}

template <typename T, int TBytes>
using Vec = T __attribute__((vector_size(TBytes)));

template <typename TVector>
constexpr int Lanes = sizeof(TVector) / sizeof(std::declval<TVector>()[0]);

template <typename TFloats>
inline auto CvttPs2Dq(TFloats value) {
    Vec<i32, sizeof(TFloats)> result;
    for (int i = 0; i < Lanes<TFloats>; ++i) {
        result[i] = CvtF64ToI32(static_cast<double>(value[i]));
    }
    return result;
}

template <typename TVector>
inline TVector FmaVec(TVector a, TVector b, TVector addend) {
    TVector result;
    for (int i = 0; i < Lanes<TVector>; ++i) {
        result[i] = std::fma(a[i], b[i], addend[i]);
    }
    return result;
}

template <typename TWide, typename TNarrow, typename TSource>
inline TNarrow Saturate(TSource value) {
    if (value < static_cast<TSource>(std::numeric_limits<TNarrow>::min())) return std::numeric_limits<TNarrow>::min();
    if (value > static_cast<TSource>(std::numeric_limits<TNarrow>::max())) return std::numeric_limits<TNarrow>::max();
    return static_cast<TNarrow>(value);
}

template <typename TNarrow, typename TSourceVector, typename TResult>
inline TResult Pack(TSourceVector a, TSourceVector b) {
    TResult result;
    constexpr int perLane = 16 / sizeof(a[0]);
    constexpr int lanes = sizeof(TSourceVector) / 16;
    for (int lane = 0; lane < lanes; ++lane) {
        for (int i = 0; i < perLane; ++i) {
            result[lane * perLane * 2 + i] = Saturate<int, TNarrow>(a[lane * perLane + i]);
            result[lane * perLane * 2 + perLane + i] = Saturate<int, TNarrow>(b[lane * perLane + i]);
        }
    }
    return result;
}

template <typename TVector>
inline TVector Permute2x128(TVector a, TVector b, u32 imm) {
    u8 source[64];
    std::memcpy(source, &a, 32);
    std::memcpy(source + 32, &b, 32);
    u8 out[32];
    for (int half = 0; half < 2; ++half) {
        u32 control = (imm >> (half * 4)) & 0xf;
        if ((control & 8) != 0) {
            std::memset(out + half * 16, 0, 16);
        } else {
            std::memcpy(out + half * 16, source + (control & 3) * 16, 16);
        }
    }
    TVector result;
    std::memcpy(&result, out, 32);
    return result;
}

template <typename TVector>
inline TVector ShiftVariable(TVector a, TVector counts, int direction) {
    using TElement = std::remove_reference_t<decltype(a[0])>;
    constexpr unsigned bits = sizeof(TElement) * 8;
    TVector result;
    for (int i = 0; i < Lanes<TVector>; ++i) {
        auto count = static_cast<std::make_unsigned_t<TElement>>(counts[i]);
        if (direction < 0) {
            result[i] = count >= bits ? 0 : static_cast<TElement>(a[i] << count);
        } else if (std::is_signed_v<TElement>) {
            result[i] = static_cast<TElement>(a[i] >> (count >= bits ? bits - 1 : count));
        } else {
            result[i] = count >= bits ? 0 : static_cast<TElement>(a[i] >> count);
        }
    }
    return result;
}

template <typename TVector>
inline TVector ShuffleBytes(TVector a, TVector control) {
    TVector result;
    for (int i = 0; i < static_cast<int>(sizeof(TVector)); ++i) {
        int lane = i & ~15;
        result[i] = (control[i] & 0x80) != 0 ? 0 : a[lane + (control[i] & 15)];
    }
    return result;
}

template <typename TVector>
inline Vec<i16, sizeof(TVector)> MaddUbsw(TVector a, TVector b) {
    Vec<i16, sizeof(TVector)> result;
    for (int i = 0; i < static_cast<int>(sizeof(TVector)) / 2; ++i) {
        int sum = static_cast<int>(static_cast<u8>(a[2 * i])) * static_cast<i8>(b[2 * i]) +
            static_cast<int>(static_cast<u8>(a[2 * i + 1])) * static_cast<i8>(b[2 * i + 1]);
        result[i] = Saturate<int, i16>(sum);
    }
    return result;
}

template <typename TVector>
inline Vec<i32, sizeof(TVector)> MaddWd(TVector a, TVector b) {
    Vec<i32, sizeof(TVector)> result;
    for (int i = 0; i < static_cast<int>(sizeof(TVector)) / 4; ++i) {
        result[i] = static_cast<i32>(a[2 * i]) * b[2 * i] + static_cast<i32>(a[2 * i + 1]) * b[2 * i + 1];
    }
    return result;
}

template <int TBytes>
inline u32 MoveMask8(const GuestContext* c, u32 index) {
    u32 mask = 0;
    for (int i = 0; i < TBytes; ++i) {
        mask |= static_cast<u32>(c->ymm[index][i] >> 7) << i;
    }
    return mask;
}

template <int TBytes>
inline u32 MoveMask32(const GuestContext* c, u32 index) {
    u32 mask = 0;
    for (int i = 0; i < TBytes / 4; ++i) {
        mask |= static_cast<u32>(c->ymm[index][i * 4 + 3] >> 7) << i;
    }
    return mask;
}

template <int TBytes>
inline u32 MoveMask64(const GuestContext* c, u32 index) {
    u32 mask = 0;
    for (int i = 0; i < TBytes / 8; ++i) {
        mask |= static_cast<u32>(c->ymm[index][i * 8 + 7] >> 7) << i;
    }
    return mask;
}

template <typename T>
inline void FlagsUcomi(GuestContext* c, T a, T b) {
    bool unordered = std::isnan(a) || std::isnan(b);
    c->zf = unordered || a == b;
    c->pf = unordered;
    c->cf = unordered || a < b;
    c->of = 0;
    c->sf = 0;
    c->af = 0;
}

}

#endif
