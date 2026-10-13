#include "prx/libc/include/GuestThreadStack.hpp"
#include <atomic>

namespace {

std::atomic<GuestThreadStackProvider> provider{nullptr};

}

extern "C" void GuestThreadStackSetProvider_nid_no_patch(GuestThreadStackProvider value) {
    provider.store(value, std::memory_order_release);
}

extern "C" bool GuestThreadStack_nid_no_patch(std::size_t desiredBytes, std::uintptr_t* low, std::uintptr_t* high) {
    const auto current = provider.load(std::memory_order_acquire);
    return current != nullptr && current(desiredBytes, low, high);
}
