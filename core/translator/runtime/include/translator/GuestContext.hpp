#ifndef CORE_TRANSLATOR_RUNTIME_INCLUDE_TRANSLATOR_GUESTCONTEXT_HPP
#define CORE_TRANSLATOR_RUNTIME_INCLUDE_TRANSLATOR_GUESTCONTEXT_HPP

#include <cstdint>

namespace translator {

enum GuestRegister : std::uint32_t {
    Rax = 0, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15
};

struct alignas(64) GuestContext {
    std::uint64_t gpr[16];
    alignas(32) std::uint8_t ymm[16][32];
    std::uint8_t cf;
    std::uint8_t pf;
    std::uint8_t af;
    std::uint8_t zf;
    std::uint8_t sf;
    std::uint8_t of;
    std::uint8_t df;
    std::uint32_t mxcsr;
    std::uint64_t fsBase;
    std::uint64_t imageBase;
    std::uint64_t returnTarget;
    std::uint64_t jumpTarget;
    std::uint64_t stackBase;
    std::uint64_t stackSize;
    void* runtime;
};

using TranslatedFunction = void (*)(GuestContext*);

}

#endif
