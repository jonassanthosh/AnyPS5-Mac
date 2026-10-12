#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTLONGDOUBLE_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTLONGDOUBLE_HPP

#include <limits>
#include <stdexcept>
#include <string>

#if defined(__x86_64__)
inline void RequireGuestLongDouble(const char*) {
    static_assert(sizeof(long double) == 16 && std::numeric_limits<long double>::digits == 64,
        "guest long double requires x87 extended precision storage");
}
#else
[[noreturn]] inline void RequireGuestLongDouble(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": the guest's x87 long double has no host equivalent on this architecture");
}
#endif

#endif
