#ifndef CORE_TRANSLATOR_RUNTIME_INCLUDE_TRANSLATOR_RUNTIME_HPP
#define CORE_TRANSLATOR_RUNTIME_INCLUDE_TRANSLATOR_RUNTIME_HPP

#include "translator/GuestContext.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>

namespace translator {

struct TranslatedEntry {
    std::uint64_t vaddr;
    TranslatedFunction function;
};

using ContextImport = void (*)(GuestContext*);

void JumpIndirect(GuestContext* c);
void CallIndirect(GuestContext* c, std::uint64_t target);
[[noreturn]] void ReturnMismatch(GuestContext* c, std::uint64_t expected);
[[noreturn]] void Unsupported(GuestContext* c, std::uint64_t address, const char* mnemonic);
[[noreturn]] void FellOffCode(GuestContext* c, std::uint64_t address);
[[noreturn]] void Trap(GuestContext* c, std::uint64_t address, const char* mnemonic);
[[noreturn]] void DivideError(GuestContext* c, std::uint64_t address);
void RepStos(GuestContext* c, unsigned width);
void RepMovs(GuestContext* c, unsigned width);
void Cpuid(GuestContext* c);
void Rdtsc(GuestContext* c);
std::uint64_t HostRandom();
std::uint64_t GsBase(GuestContext* c, std::uint64_t offset);

class Runtime {
public:
    static Runtime& Instance();

    void RegisterFunctions(const TranslatedEntry* entries, std::size_t count);
    void RegisterImport(const std::string& name, void* hostFunction);
    void RegisterContextImport(const std::string& name, ContextImport handler);
    void RegisterDataImport(const std::string& name, void* hostAddress);
    void LoadImage(const std::filesystem::path& path);

    std::uint64_t ImageBase() const;
    std::uint64_t EntryAddress() const;
    std::uint64_t SymbolAddress(const std::string& name) const;

    GuestContext* CurrentContext();
    std::uint64_t CallGuest(std::uint64_t target, std::initializer_list<std::uint64_t> arguments);

    void Dispatch(GuestContext* c, std::uint64_t target);

    std::size_t DispatchCount() const;
    std::size_t ImportCallCount() const;

private:
    Runtime();
    struct State;
    State* _state;
};

}

#endif
