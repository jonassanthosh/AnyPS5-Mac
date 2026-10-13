#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GENERAL_GUESTVARARGS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GENERAL_GUESTVARARGS_HPP

#include "SceTypes.hpp"
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(_WIN32)

#define APS5_GUEST_SYSV_VARARGS 1
#define APS5_SYSV_VA_LIST __builtin_sysv_va_list
#define APS5_SYSV_VA_START(list, last, fixedIntegers) __builtin_sysv_va_start(list, last)
#define APS5_SYSV_VA_END(list) __builtin_sysv_va_end(list)
#define APS5_SYSV_VA_ARG(list, type) __builtin_va_arg(list, type)

#elif defined(__aarch64__)

#define APS5_GUEST_SYSV_VARARGS 1

using GuestVarArgsProvider = bool (*)(VaList* list, void* saveArea, unsigned fixedIntegers, std::uint64_t last);

extern "C" void GuestVarArgsStart_nid_no_patch(VaList* list, void* saveArea, unsigned fixedIntegers, std::uint64_t last);
extern "C" void GuestVarArgsSetProvider_nid_no_patch(GuestVarArgsProvider provider);

using Aps5SysvVaList = VaList[1];

template <typename T>
inline T Aps5SysvVaArg(VaList* list) {
    const void* address;
    if constexpr (sizeof(T) <= 8 && (std::is_floating_point_v<T>)) {
        if (list->fp_offset < 176) {
            address = static_cast<const char*>(list->reg_save_area) + list->fp_offset;
            list->fp_offset += 16;
        } else {
            address = list->overflow_arg_area;
            list->overflow_arg_area = static_cast<char*>(list->overflow_arg_area) + 8;
        }
    } else {
        static_assert(sizeof(T) <= 8);
        if (list->gp_offset < 48) {
            address = static_cast<const char*>(list->reg_save_area) + list->gp_offset;
            list->gp_offset += 8;
        } else {
            address = list->overflow_arg_area;
            list->overflow_arg_area = static_cast<char*>(list->overflow_arg_area) + 8;
        }
    }
    T value;
    std::memcpy(&value, address, sizeof(value));
    return value;
}

#define APS5_SYSV_VA_LIST Aps5SysvVaList
#define APS5_SYSV_VA_START(list, last, fixedIntegers) \
    alignas(16) unsigned char list##SaveArea[176]; \
    GuestVarArgsStart_nid_no_patch(list, list##SaveArea, fixedIntegers, (std::uint64_t)(last))
#define APS5_SYSV_VA_END(list) ((void)0)
#define APS5_SYSV_VA_ARG(list, type) Aps5SysvVaArg<type>(list)

#else

#define APS5_GUEST_SYSV_VARARGS 0

#endif

#endif
