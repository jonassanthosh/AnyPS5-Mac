#ifndef CORE_TRANSLATOR_RUNTIME_INCLUDE_TRANSLATOR_RUNTIME_HPP
#define CORE_TRANSLATOR_RUNTIME_INCLUDE_TRANSLATOR_RUNTIME_HPP

#include "translator/GuestContext.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace translator {

struct TranslatedEntry {
    std::uint64_t vaddr;
    TranslatedFunction function;
};

struct TranslatedImage {
    const char* name;
    const TranslatedEntry* functions;
    std::size_t count;
    std::uint64_t* base;
};

using ContextImport = void (*)(GuestContext*);

struct ImportResolution {
    enum class Kind { Missing, Function, Context, Data, Guest };
    Kind kind = Kind::Missing;
    void* address = nullptr;
    ContextImport handler = nullptr;
};

using ImportResolver = std::function<ImportResolution(const std::string& name, bool data)>;

struct LoadedImageInfo {
    std::string name;
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::uint64_t entry = 0;
    std::uint64_t init = 0;
    std::vector<std::uint64_t> initArray;
    std::size_t tlsModuleId = 0;
    const void* programHeaders = nullptr;
    std::size_t programHeaderCount = 0;
    std::size_t unresolvedImports = 0;
};

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

    void RegisterImport(const std::string& name, void* hostFunction);
    void RegisterContextImport(const std::string& name, ContextImport handler);
    void RegisterDataImport(const std::string& name, void* hostAddress);
    void SetImportResolver(ImportResolver resolver);

    std::size_t LoadImage(const std::filesystem::path& path, const TranslatedImage& translated, bool main);
    void UnloadImage(std::size_t index);
    const LoadedImageInfo& Image(std::size_t index) const;
    std::size_t ImageCount() const;
    std::uint64_t GuestExport(const std::string& nid) const;
    std::uint64_t ImageExport(std::size_t index, const std::string& nameOrNid) const;

    bool IsTranslatedFunction(std::uint64_t address) const;
    std::string DescribeAddress(std::uint64_t address) const;
    void* NativeImportAt(std::uint64_t address) const;
    void InstallCallbackBridge();

    std::uint64_t ImageBase() const;
    std::uint64_t EntryAddress() const;
    std::uint64_t SymbolAddress(const std::string& name) const;

    GuestContext* CurrentContext();
    GuestContext* PrepareThread(std::size_t stackBytes);
    GuestContext* ExistingContext();
    std::uint64_t CallGuest(std::uint64_t target, std::initializer_list<std::uint64_t> arguments);
    std::uint64_t CallGuestWithRegisters(std::uint64_t target);
    std::uint64_t TlsAddress(GuestContext* c, std::uint64_t module, std::uint64_t offset);

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
