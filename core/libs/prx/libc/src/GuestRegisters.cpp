#include "prx/libc/include/GuestRegisters.hpp"
#include <atomic>
#include <cstdlib>

namespace {

std::atomic<GuestRegistersProvider> provider{nullptr};
std::atomic<GuestRegistersInstaller> installer{nullptr};
std::atomic<GuestNativeImportResolver> importResolver{nullptr};

}

extern "C" void GuestRegistersSetProvider_nid_no_patch(GuestRegistersProvider value) {
    provider.store(value, std::memory_order_release);
}

extern "C" bool GuestRegistersCapture_nid_no_patch(GuestRegisterFile* registers) {
    const auto current = provider.load(std::memory_order_acquire);
    return current != nullptr && current(registers);
}

extern "C" void GuestRegistersSetInstaller_nid_no_patch(GuestRegistersInstaller value) {
    installer.store(value, std::memory_order_release);
}

extern "C" void GuestRegistersInstall_nid_no_patch(const GuestRegisterFile* registers) {
    const auto current = installer.load(std::memory_order_acquire);
    if (current != nullptr) current(registers);
    std::abort();
}

extern "C" void GuestNativeImportSetResolver_nid_no_patch(GuestNativeImportResolver value) {
    importResolver.store(value, std::memory_order_release);
}

extern "C" std::uintptr_t GuestNativeImport_nid_no_patch(std::uintptr_t address) {
    const auto current = importResolver.load(std::memory_order_acquire);
    if (current == nullptr) return address;
    const auto native = current(address);
    return native != nullptr ? reinterpret_cast<std::uintptr_t>(native) : address;
}
