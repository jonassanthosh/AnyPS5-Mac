#include "prx/libc/include/General.hpp"
#include "prx/libc/include/HeapDiagnostics.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace {

std::uint64_t mspaceAtomicIdMask = 0;
std::array<std::uint64_t, 64> mstateTable{};

static_assert(sizeof(LibcHeapInfo) == 32);
static_assert(offsetof(LibcHeapInfo, mspace_atomic_id_mask) == 16);
static_assert(offsetof(LibcHeapInfo, mstate_table) == 24);

}

extern "C" {

void LibcHeapTraceInfo_nid_no_patch(LibcHeapInfo* info) {
    if (info == nullptr)
        throw std::invalid_argument("heap trace info: null output");
    if (info->size != sizeof(LibcHeapInfo))
        throw std::invalid_argument("heap trace info: unsupported structure size");
    info->unknown2 = 0;
    info->mspace_atomic_id_mask = &mspaceAtomicIdMask;
    info->mstate_table = mstateTable.data();
}

// unknown signature
void APS5_VABI sceLibcInternalBacktraceForGame_nid_postfix(const char* heapName) {
    (void)heapName;
    NotImplemented_nid_no_patch(__func__);
}

// unknown signature
void APS5_VABI sceLibcInternalHeapErrorReportForGame_nid_postfix(void* heap, void* block, std::uint32_t error) {
    (void)heap;
    (void)block;
    (void)error;
    NotImplemented_nid_no_patch(__func__);
}

int APS5_VABI sceLibcBacktraceGetBufferSize_nid_postfix(void* context, std::uint32_t* frames, std::size_t* bytes, std::int32_t flags) {
    (void)context;
    (void)flags;
    if (frames != nullptr) *frames = 0;
    if (bytes != nullptr) *bytes = 0;
    return -1;
}

int APS5_VABI sceLibcBacktraceSelf_nid_postfix(std::uint32_t flags, void* buffer, std::size_t bytes) {
    (void)flags;
    (void)buffer;
    (void)bytes;
    return 0;
}

}
