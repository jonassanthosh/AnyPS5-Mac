#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTIMAGEREGISTRY_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTIMAGEREGISTRY_HPP

#include "prx/libc/include/specifics/linux/ElfTypes.hpp"
#include <cstddef>
#include <cstdint>

struct GuestImageLoader {
    void* (*load)(const char* path);
    void* (*symbol)(void* image, const char* name);
    void (*unload)(void* image);
};

extern "C" {

const void* GuestImageRegister_nid_no_patch(const char* name, std::uintptr_t base, const Elf64_Phdr* headers,
    std::uint16_t count, std::size_t tlsModuleId, bool main);
void GuestImageUnregister_nid_no_patch(const void* image);
const void* GuestImageContaining_nid_no_patch(std::uintptr_t address);
const void* GuestImageMain_nid_no_patch();
const char* GuestImageName_nid_no_patch(const void* image);
std::uintptr_t GuestImageBase_nid_no_patch(const void* image);
int GuestImageIterate_nid_no_patch(int (*callback)(dl_phdr_info*, std::size_t, void*), void* data);
void GuestImageSetLoader_nid_no_patch(const GuestImageLoader* loader);
const GuestImageLoader* GuestImageLoaderInterface_nid_no_patch();

}

#endif
