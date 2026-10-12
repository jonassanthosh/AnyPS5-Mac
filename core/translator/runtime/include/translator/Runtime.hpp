#ifndef CORE_TRANSLATOR_RUNTIME_INCLUDE_TRANSLATOR_RUNTIME_HPP
#define CORE_TRANSLATOR_RUNTIME_INCLUDE_TRANSLATOR_RUNTIME_HPP

#include "translator/GuestContext.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <string>

namespace translator {

struct TranslatedEntry {
    std::uint64_t vaddr;
    TranslatedFunction function;
};

using ContextImport = void (*)(GuestContext*);

struct ImportResolution {
    enum class Kind { Missing, Function, Context, Data };
    Kind kind = Kind::Missing;
    void* address = nullptr;
    ContextImport handler = nullptr;
};

using ImportResolver = std::function<ImportResolution(const std::string& name, bool data)>;

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
    void SetImportResolver(ImportResolver resolver);
    void LoadImage(const std::filesystem::path& path);
    const void* ImageProgramHeaders(std::size_t& count) const;
    std::uint64_t ImageSize() const;
    std::size_t UnresolvedImportCount() const;

    bool IsTranslatedFunction(std::uint64_t address) const;
    void* NativeImportAt(std::uint64_t address) const;
    void InstallCallbackBridge();

    std::uint64_t ImageBase() const;
    std::uint64_t EntryAddress() const;
    std::uint64_t SymbolAddress(const std::string& name) const;

    GuestContext* CurrentContext();
    std::uint64_t CallGuest(std::uint64_t target, std::initializer_list<std::uint64_t> arguments);
    std::uint64_t CallGuestWithRegisters(std::uint64_t target);

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
