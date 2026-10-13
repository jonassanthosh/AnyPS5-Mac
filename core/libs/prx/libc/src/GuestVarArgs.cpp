#include "prx/libc/include/general/GuestVarArgs.hpp"

#if defined(__aarch64__)

#include <atomic>
#include <stdexcept>

namespace {

std::atomic<GuestVarArgsProvider> provider{nullptr};

}

extern "C" void GuestVarArgsSetProvider_nid_no_patch(GuestVarArgsProvider value) {
    provider.store(value, std::memory_order_release);
}

extern "C" void GuestVarArgsStart_nid_no_patch(VaList* list, void* saveArea, unsigned fixedIntegers, std::uint64_t last) {
    const auto current = provider.load(std::memory_order_acquire);
    if (current == nullptr || !current(list, saveArea, fixedIntegers, last)) {
        throw std::runtime_error("variadic guest call: the caller's registers are not available");
    }
}

#endif
