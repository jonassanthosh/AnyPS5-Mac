#ifdef __APPLE__

#include "prx/libc/include/GuestImageRegistry.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Image {
    std::string name;
    std::uintptr_t base;
    std::vector<Elf64_Phdr> headers;
    std::size_t tlsModuleId;
    bool main;
};

struct Registry {
    std::shared_mutex lock;
    std::vector<std::unique_ptr<Image>> images;
    unsigned long long adds = 0;
    unsigned long long subs = 0;
    std::atomic<const GuestImageLoader*> loader{nullptr};
};

Registry& Images() {
    static auto& registry = *new Registry();
    return registry;
}

bool Contains(const Image& image, std::uintptr_t address) {
    for (const auto& header : image.headers) {
        if (header.p_type != PT_LOAD) continue;
        const auto start = image.base + header.p_vaddr;
        if (address >= start && address - start < header.p_memsz) return true;
    }
    return false;
}

}

extern "C" {

const void* GuestImageRegister_nid_no_patch(const char* name, std::uintptr_t base, const Elf64_Phdr* headers,
    std::uint16_t count, std::size_t tlsModuleId, bool main) {
    if (name == nullptr || headers == nullptr || count == 0) throw std::invalid_argument("guest image registry: incomplete image");
    auto image = std::make_unique<Image>(Image{name, base, std::vector<Elf64_Phdr>(headers, headers + count), tlsModuleId, main});
    auto& registry = Images();
    std::unique_lock lock(registry.lock);
    if (main) {
        for (const auto& existing : registry.images) {
            if (existing->main) throw std::runtime_error("guest image registry: a main image is already registered");
        }
    }
    const auto* result = image.get();
    registry.images.push_back(std::move(image));
    ++registry.adds;
    return result;
}

void GuestImageUnregister_nid_no_patch(const void* image) {
    auto& registry = Images();
    std::unique_lock lock(registry.lock);
    for (auto it = registry.images.begin(); it != registry.images.end(); ++it) {
        if (it->get() == image) {
            registry.images.erase(it);
            ++registry.subs;
            return;
        }
    }
    throw std::invalid_argument("guest image registry: unknown image");
}

const void* GuestImageContaining_nid_no_patch(std::uintptr_t address) {
    auto& registry = Images();
    std::shared_lock lock(registry.lock);
    for (const auto& image : registry.images) {
        if (Contains(*image, address)) return image.get();
    }
    return nullptr;
}

const void* GuestImageMain_nid_no_patch() {
    auto& registry = Images();
    std::shared_lock lock(registry.lock);
    for (const auto& image : registry.images) {
        if (image->main) return image.get();
    }
    return nullptr;
}

const char* GuestImageName_nid_no_patch(const void* image) {
    return static_cast<const Image*>(image)->name.c_str();
}

std::uintptr_t GuestImageBase_nid_no_patch(const void* image) {
    return static_cast<const Image*>(image)->base;
}

int GuestImageIterate_nid_no_patch(int (*callback)(dl_phdr_info*, std::size_t, void*), void* data) {
    auto& registry = Images();
    std::vector<dl_phdr_info> infos;
    {
        std::shared_lock lock(registry.lock);
        for (const auto& image : registry.images) {
            dl_phdr_info info{};
            info.dlpi_addr = image->base;
            info.dlpi_name = image->main ? "" : image->name.c_str();
            info.dlpi_phdr = image->headers.data();
            info.dlpi_phnum = static_cast<std::uint16_t>(image->headers.size());
            info.dlpi_adds = registry.adds;
            info.dlpi_subs = registry.subs;
            info.dlpi_tls_modid = image->tlsModuleId;
            infos.push_back(info);
        }
    }
    for (auto& info : infos) {
        if (const int result = callback(&info, sizeof(info), data); result != 0) return result;
    }
    return 0;
}

void GuestImageSetLoader_nid_no_patch(const GuestImageLoader* loader) {
    Images().loader.store(loader, std::memory_order_release);
}

const GuestImageLoader* GuestImageLoaderInterface_nid_no_patch() {
    return Images().loader.load(std::memory_order_acquire);
}

}

#endif
