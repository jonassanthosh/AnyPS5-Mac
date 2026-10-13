#ifndef CORE_TRANSLATOR_RUNTIME_INCLUDE_TRANSLATOR_GUESTJUMP_HPP
#define CORE_TRANSLATOR_RUNTIME_INCLUDE_TRANSLATOR_GUESTJUMP_HPP

#include "translator/Runtime.hpp"
#include <string>
#include <utility>
#include <vector>

namespace translator {

struct GuestResume {
    std::uint64_t gpr[16];
    std::uint64_t rip;
    std::uint32_t mxcsr;
};

void GuestSetjmp(GuestContext* c);
void GuestLongjmp(GuestContext* c);

const std::vector<std::pair<std::string, ContextImport>>& RuntimeContextImports();

bool ProvideGuestVarArgs(void* list, void* saveArea, unsigned fixedIntegers, std::uint64_t last);
bool ProvideGuestThreadStack(std::size_t desiredBytes, std::uintptr_t* low, std::uintptr_t* high);
bool ProvideGuestRegisters(void* registers);
[[noreturn]] void ProvideGuestInstall(const void* registers);
void ApplyGuestResume(GuestContext* c, const GuestResume& resume);

}

#endif
