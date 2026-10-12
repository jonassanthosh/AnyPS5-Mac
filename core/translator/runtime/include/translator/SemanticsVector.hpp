#ifndef CORE_TRANSLATOR_RUNTIME_INCLUDE_TRANSLATOR_SEMANTICSVECTOR_HPP
#define CORE_TRANSLATOR_RUNTIME_INCLUDE_TRANSLATOR_SEMANTICSVECTOR_HPP

#include "translator/Semantics.hpp"
#include <algorithm>
#include <arm_neon.h>
#include <cmath>
#include <cstring>

namespace translator {

template <typename T>
inline bool ComparePredicate(T a, T b, unsigned predicate) {
    const bool unordered = std::isnan(a) || std::isnan(b);
    bool result = false;
    switch (predicate & 7) {
    case 0: result = !unordered && a == b; break;
    case 1: result = !unordered && a < b; break;
    case 2: result = !unordered && a <= b; break;
    case 3: result = unordered; break;
    case 4: result = unordered || a != b; break;
    case 5: result = unordered || !(a < b); break;
    case 6: result = unordered || !(a <= b); break;
    default: result = !unordered; break;
    }
    if ((predicate & 0x8) != 0) {
        switch (predicate & 7) {
        case 0: result = unordered || a == b; break;
        case 1: result = unordered || !(a >= b); break;
        case 2: result = unordered || !(a > b); break;
        case 3: result = false; break;
        case 4: result = !unordered && a != b; break;
        case 5: result = !unordered && a >= b; break;
        case 6: result = !unordered && a > b; break;
        default: result = true; break;
        }
    }
    return result;
}

template <typename TFloat, typename TVector>
inline TVector CompareVector(TVector a, TVector b, unsigned predicate) {
    using TBits = std::conditional_t<sizeof(TFloat) == 4, u32, u64>;
    TVector result;
    for (int i = 0; i < Lanes<TVector>; ++i) {
        const TBits mask = ComparePredicate<TFloat>(a[i], b[i], predicate) ? ~TBits{0} : TBits{0};
        TFloat value;
        std::memcpy(&value, &mask, sizeof(value));
        result[i] = value;
    }
    return result;
}

template <typename TVector, typename TMask>
inline TVector BlendBySign(TVector a, TVector b, TMask mask) {
    TVector result;
    for (int i = 0; i < Lanes<TVector>; ++i) {
        result[i] = mask[i] < 0 ? b[i] : a[i];
    }
    return result;
}

template <typename TVector>
inline TVector RoundVector(TVector value, unsigned mode) {
    TVector result;
    for (int i = 0; i < Lanes<TVector>; ++i) {
        const auto x = value[i];
        if ((mode & 4) != 0) {
            result[i] = std::nearbyint(x);
            continue;
        }
        switch (mode & 3) {
        case 0: result[i] = std::nearbyint(x); break;
        case 1: result[i] = std::floor(x); break;
        case 2: result[i] = std::ceil(x); break;
        default: result[i] = std::trunc(x); break;
        }
    }
    return result;
}

template <typename T>
inline T RoundScalar(T value, unsigned mode) {
    if ((mode & 4) != 0) return std::nearbyint(value);
    switch (mode & 3) {
    case 0: return std::nearbyint(value);
    case 1: return std::floor(value);
    case 2: return std::ceil(value);
    default: return std::trunc(value);
    }
}

template <typename TVector>
inline TVector DotProduct128(TVector a, TVector b, unsigned imm) {
    TVector result{};
    for (int lane = 0; lane < Lanes<TVector> / 4; ++lane) {
        float sum = 0.0f;
        float products[4];
        for (int i = 0; i < 4; ++i) {
            products[i] = ((imm >> (4 + i)) & 1) != 0 ? a[lane * 4 + i] * b[lane * 4 + i] : 0.0f;
        }
        sum = (products[0] + products[1]) + (products[2] + products[3]);
        for (int i = 0; i < 4; ++i) {
            result[lane * 4 + i] = ((imm >> i) & 1) != 0 ? sum : 0.0f;
        }
    }
    return result;
}

template <typename TVector>
inline TVector HorizontalAdd(TVector a, TVector b, bool subtract) {
    constexpr int perLane = 16 / sizeof(a[0]);
    TVector result;
    for (int lane = 0; lane < Lanes<TVector> / perLane; ++lane) {
        const int base = lane * perLane;
        for (int i = 0; i < perLane / 2; ++i) {
            result[base + i] = subtract ? a[base + 2 * i] - a[base + 2 * i + 1] : a[base + 2 * i] + a[base + 2 * i + 1];
            result[base + perLane / 2 + i] = subtract ? b[base + 2 * i] - b[base + 2 * i + 1] : b[base + 2 * i] + b[base + 2 * i + 1];
        }
    }
    return result;
}

template <typename TVector>
inline TVector AddSubtract(TVector a, TVector b) {
    TVector result;
    for (int i = 0; i < Lanes<TVector>; ++i) {
        result[i] = (i & 1) == 0 ? a[i] - b[i] : a[i] + b[i];
    }
    return result;
}

template <typename TVector>
inline TVector ReciprocalSqrt(TVector value) {
    TVector result;
    for (int i = 0; i < Lanes<TVector>; ++i) result[i] = 1.0f / std::sqrt(value[i]);
    return result;
}

template <typename TVector>
inline TVector Reciprocal(TVector value) {
    TVector result;
    for (int i = 0; i < Lanes<TVector>; ++i) result[i] = 1.0f / value[i];
    return result;
}

template <typename TVector>
inline TVector SqrtVector(TVector value) {
    TVector result;
    for (int i = 0; i < Lanes<TVector>; ++i) result[i] = std::sqrt(value[i]);
    return result;
}

template <typename TResult, typename TVector>
inline TResult ConvertRoundToInt32(TVector value) {
    TResult result{};
    for (int i = 0; i < Lanes<TVector>; ++i) result[i] = CvtF64ToI32(std::nearbyint(static_cast<double>(value[i])));
    return result;
}

template <typename TResult, typename TVector>
inline TResult ConvertTruncateToInt32(TVector value) {
    TResult result{};
    for (int i = 0; i < Lanes<TVector>; ++i) result[i] = CvtF64ToI32(static_cast<double>(value[i]));
    return result;
}

template <typename TVector>
inline TVector SaturatingAdd(TVector a, TVector b, bool subtract) {
    using T = std::remove_reference_t<decltype(a[0])>;
    TVector result;
    for (int i = 0; i < Lanes<TVector>; ++i) {
        const long long wide = subtract ? static_cast<long long>(a[i]) - static_cast<long long>(b[i])
                                        : static_cast<long long>(a[i]) + static_cast<long long>(b[i]);
        result[i] = static_cast<T>(std::min<long long>(std::max<long long>(wide, std::numeric_limits<T>::min()),
            std::numeric_limits<T>::max()));
    }
    return result;
}

template <typename TVector>
inline TVector AverageRounded(TVector a, TVector b) {
    TVector result;
    for (int i = 0; i < Lanes<TVector>; ++i) result[i] = static_cast<std::remove_reference_t<decltype(a[0])>>((a[i] + b[i] + 1) >> 1);
    return result;
}

template <typename TVector>
inline Vec<u64, sizeof(TVector)> SumAbsoluteDifferences(TVector a, TVector b) {
    Vec<u64, sizeof(TVector)> result{};
    for (int group = 0; group < static_cast<int>(sizeof(TVector)) / 8; ++group) {
        u64 sum = 0;
        for (int i = 0; i < 8; ++i) {
            const int x = a[group * 8 + i];
            const int y = b[group * 8 + i];
            sum += static_cast<u64>(x > y ? x - y : y - x);
        }
        result[group] = sum;
    }
    return result;
}

template <typename TVector>
inline TVector MultiplyHighSigned(TVector a, TVector b) {
    TVector result;
    for (int i = 0; i < Lanes<TVector>; ++i) result[i] = static_cast<i16>((static_cast<i32>(a[i]) * b[i]) >> 16);
    return result;
}

template <typename TVector>
inline Vec<i64, sizeof(TVector)> MultiplyEvenSigned(TVector a, TVector b) {
    Vec<i64, sizeof(TVector)> result;
    for (int i = 0; i < static_cast<int>(sizeof(TVector)) / 8; ++i) result[i] = static_cast<i64>(a[2 * i]) * b[2 * i];
    return result;
}

template <typename TVector>
inline TVector AbsoluteValue(TVector a) {
    TVector result;
    for (int i = 0; i < Lanes<TVector>; ++i) result[i] = a[i] < 0 ? -a[i] : a[i];
    return result;
}

inline Vec<u16, 16> MinimumPosition(Vec<u16, 16> value) {
    u16 minimum = value[0];
    u16 index = 0;
    for (u16 i = 1; i < 8; ++i) {
        if (value[i] < minimum) {
            minimum = value[i];
            index = i;
        }
    }
    Vec<u16, 16> result{};
    result[0] = minimum;
    result[1] = index;
    return result;
}

template <typename TVector, typename TCount>
inline TVector ShiftByRegister(TVector a, TCount countVector, int direction) {
    using T = std::remove_reference_t<decltype(a[0])>;
    constexpr unsigned bits = sizeof(T) * 8;
    u64 count;
    std::memcpy(&count, &countVector, sizeof(count));
    TVector result;
    for (int i = 0; i < Lanes<TVector>; ++i) {
        if (direction < 0) {
            result[i] = count >= bits ? 0 : static_cast<T>(a[i] << count);
        } else if (std::is_signed_v<T>) {
            result[i] = static_cast<T>(a[i] >> (count >= bits ? bits - 1 : count));
        } else {
            result[i] = count >= bits ? 0 : static_cast<T>(a[i] >> count);
        }
    }
    return result;
}

template <typename TVector, typename TControl>
inline TVector PermuteWithinLanes(TVector a, TControl control, bool doubles) {
    TVector result;
    constexpr int perLane = 16 / sizeof(a[0]);
    for (int i = 0; i < Lanes<TVector>; ++i) {
        const int lane = i / perLane * perLane;
        const int select = doubles ? static_cast<int>((control[i] >> 1) & 1) : static_cast<int>(control[i] & 3);
        result[i] = a[lane + select];
    }
    return result;
}

template <typename TVector>
inline TVector PermuteAcrossLanes(TVector a, Vec<u32, 32> control) {
    TVector result;
    for (int i = 0; i < 8; ++i) result[i] = a[control[i] & 7];
    return result;
}

template <typename TVector, typename TMask>
inline TVector MaskedLoad(u64 address, TMask mask) {
    TVector result{};
    using T = std::remove_reference_t<decltype(result[0])>;
    for (int i = 0; i < Lanes<TVector>; ++i) {
        if (mask[i] < 0) result[i] = Ld<T>(address + i * sizeof(T));
    }
    return result;
}

template <typename TVector, typename TMask>
inline void MaskedStore(u64 address, TMask mask, TVector value) {
    using T = std::remove_reference_t<decltype(value[0])>;
    for (int i = 0; i < Lanes<TVector>; ++i) {
        if (mask[i] < 0) St<T>(address + i * sizeof(T), value[i]);
    }
}

template <typename TVector>
inline Vec<float, sizeof(TVector) * 2> HalfToFloat(TVector value) {
    Vec<float, sizeof(TVector) * 2> result;
    for (int i = 0; i < Lanes<TVector>; ++i) {
        __fp16 half;
        const u16 bits = value[i];
        std::memcpy(&half, &bits, sizeof(bits));
        result[i] = static_cast<float>(half);
    }
    return result;
}

template <typename TVector>
inline Vec<u16, sizeof(TVector) / 2> FloatToHalf(TVector value) {
    Vec<u16, sizeof(TVector) / 2> result;
    for (int i = 0; i < Lanes<TVector>; ++i) {
        const __fp16 half = static_cast<__fp16>(value[i]);
        u16 bits;
        std::memcpy(&bits, &half, sizeof(bits));
        result[i] = bits;
    }
    return result;
}

inline uint8x16_t ToNeon(Vec<u8, 16> value) {
    uint8x16_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

inline Vec<u8, 16> FromNeon(uint8x16_t value) {
    Vec<u8, 16> result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

inline Vec<u8, 16> AesEncrypt(Vec<u8, 16> state, Vec<u8, 16> key, bool last) {
    uint8x16_t value = vaeseq_u8(ToNeon(state), vdupq_n_u8(0));
    if (!last) value = vaesmcq_u8(value);
    return FromNeon(veorq_u8(value, ToNeon(key)));
}

inline Vec<u8, 16> AesDecrypt(Vec<u8, 16> state, Vec<u8, 16> key, bool last) {
    uint8x16_t value = vaesdq_u8(ToNeon(state), vdupq_n_u8(0));
    if (!last) value = vaesimcq_u8(value);
    return FromNeon(veorq_u8(value, ToNeon(key)));
}

inline Vec<u8, 16> AesInverseMixColumns(Vec<u8, 16> state) {
    return FromNeon(vaesimcq_u8(ToNeon(state)));
}

inline u32 AesSubWord(u32 word) {
    const auto bytes = FromNeon(vaeseq_u8(vreinterpretq_u8_u32(vdupq_n_u32(word)), vdupq_n_u8(0)));
    u32 result = 0;
    for (int i = 0; i < 4; ++i) result |= static_cast<u32>(bytes[i]) << (8 * i);
    return result;
}

inline Vec<u32, 16> AesKeyGenAssist(Vec<u32, 16> source, u32 rcon) {
    const u32 x1 = AesSubWord(source[1]);
    const u32 x3 = AesSubWord(source[3]);
    Vec<u32, 16> result;
    result[0] = x1;
    result[1] = ((x1 >> 8) | (x1 << 24)) ^ rcon;
    result[2] = x3;
    result[3] = ((x3 >> 8) | (x3 << 24)) ^ rcon;
    return result;
}

inline Vec<u64, 16> CarrylessMultiply(Vec<u64, 16> a, Vec<u64, 16> b, unsigned imm) {
    const poly64_t x = a[imm & 1];
    const poly64_t y = b[(imm >> 4) & 1];
    const poly128_t product = vmull_p64(x, y);
    Vec<u64, 16> result;
    std::memcpy(&result, &product, sizeof(result));
    return result;
}

inline u64 ExtractField(u64 value, unsigned length, unsigned index) {
    length &= 63;
    index &= 63;
    const unsigned width = length == 0 ? 64 : length;
    const u64 mask = width == 64 ? ~u64{0} : ((u64{1} << width) - 1);
    return (value >> index) & mask;
}

inline u64 InsertField(u64 destination, u64 source, unsigned length, unsigned index) {
    length &= 63;
    index &= 63;
    const unsigned width = length == 0 ? 64 : length;
    const u64 mask = width == 64 ? ~u64{0} : ((u64{1} << width) - 1);
    return (destination & ~(mask << index)) | ((source & mask) << index);
}

}

#endif
