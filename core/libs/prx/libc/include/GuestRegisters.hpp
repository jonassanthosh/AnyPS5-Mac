#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTREGISTERS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTREGISTERS_HPP

#include <cstdint>

struct GuestRegisterFile {
    std::uint64_t gpr[16];
    std::uint64_t rip;
    std::uint64_t rflags;
    std::uint64_t fsBase;
    std::uint32_t mxcsr;
    std::uint8_t xmm[16][16];
};

using GuestRegistersProvider = bool (*)(GuestRegisterFile* registers);
using GuestRegistersInstaller = void (*)(const GuestRegisterFile* registers);
using GuestNativeImportResolver = void* (*)(std::uintptr_t address);

extern "C" void GuestRegistersSetProvider_nid_no_patch(GuestRegistersProvider provider);
extern "C" bool GuestRegistersCapture_nid_no_patch(GuestRegisterFile* registers);
extern "C" void GuestRegistersSetInstaller_nid_no_patch(GuestRegistersInstaller installer);
extern "C" [[noreturn]] void GuestRegistersInstall_nid_no_patch(const GuestRegisterFile* registers);
extern "C" void GuestNativeImportSetResolver_nid_no_patch(GuestNativeImportResolver resolver);
extern "C" std::uintptr_t GuestNativeImport_nid_no_patch(std::uintptr_t address);

#endif
