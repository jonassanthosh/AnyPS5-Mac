#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTTHREADSTACK_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTTHREADSTACK_HPP

#include <cstddef>
#include <cstdint>

using GuestThreadStackProvider = bool (*)(std::size_t desiredBytes, std::uintptr_t* low, std::uintptr_t* high);

extern "C" void GuestThreadStackSetProvider_nid_no_patch(GuestThreadStackProvider provider);
extern "C" bool GuestThreadStack_nid_no_patch(std::size_t desiredBytes, std::uintptr_t* low, std::uintptr_t* high);

#endif
