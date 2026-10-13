#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GENERAL_GENERALMACROS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GENERAL_GENERALMACROS_HPP

#ifdef _WIN32
#define APS5_VABI __attribute__((sysv_abi))
#else
#define APS5_VABI
#endif

#if defined(__APPLE__) && defined(__aarch64__)
#include <cstdint>

template <typename T>
struct GuestStackArg {
    std::uint64_t slot;
    GuestStackArg(T value) : slot(static_cast<std::uint64_t>(value)) {}
    operator T() const { return static_cast<T>(slot); }
};
#else
template <typename T>
using GuestStackArg = T;
#endif

#endif
