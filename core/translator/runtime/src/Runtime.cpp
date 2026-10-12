#include "translator/Runtime.hpp"
#include "translator/Semantics.hpp"
#include "ElfImage.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <sys/mman.h>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" void aps5_host_call_sysv(translator::GuestContext* context, void* function);

namespace translator {

namespace {

constexpr std::uint32_t RX8664_64 = 1;
constexpr std::uint32_t RX8664GlobDat = 6;
constexpr std::uint32_t RX8664JumpSlot = 7;
constexpr std::uint32_t RX8664Relative = 8;
constexpr std::uint64_t ImportStride = 16;
constexpr std::uint64_t GuestStackSize = 8ull << 20;
constexpr std::uint64_t TlsBlockSize = 64ull << 10;

std::string Hex(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

struct Import {
    std::string name;
    void* hostFunction = nullptr;
    ContextImport contextHandler = nullptr;
};

struct ThreadState {
    std::unique_ptr<GuestContext> context;
    void* stack = nullptr;
    void* tls = nullptr;

    ~ThreadState() {
        if (stack != nullptr) munmap(stack, GuestStackSize);
        if (tls != nullptr) munmap(tls, TlsBlockSize);
    }
};

thread_local std::unique_ptr<ThreadState> currentThread;

}

struct Runtime::State {
    std::unordered_map<std::uint64_t, TranslatedFunction> functions;
    std::vector<Import> imports;
    std::unordered_map<std::string, std::size_t> importIndex;
    std::unordered_map<std::string, void*> dataImports;
    std::unordered_map<std::string, std::uint64_t> symbols;
    std::uint8_t* image = nullptr;
    std::uint64_t imageSize = 0;
    std::uint64_t entry = 0;
    std::uint8_t* importRegion = nullptr;
    std::uint64_t importRegionSize = 0;
    std::uint64_t sentinel = 0;
    std::atomic<std::size_t> dispatches{0};
    std::atomic<std::size_t> importCalls{0};
};

Runtime& Runtime::Instance() {
    static Runtime runtime;
    return runtime;
}

Runtime::Runtime() : _state(new State) {
    _state->importRegionSize = 1ull << 20;
    void* region = mmap(nullptr, _state->importRegionSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) {
        throw std::runtime_error("Cannot reserve the import address range");
    }
    _state->importRegion = static_cast<std::uint8_t*>(region);
    _state->sentinel = reinterpret_cast<std::uint64_t>(region) + _state->importRegionSize - ImportStride;
}

void Runtime::RegisterFunctions(const TranslatedEntry* entries, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        _state->functions.emplace(entries[i].vaddr, entries[i].function);
    }
}

void Runtime::RegisterImport(const std::string& name, void* hostFunction) {
    _state->importIndex[name] = _state->imports.size();
    _state->imports.push_back({name, hostFunction, nullptr});
}

void Runtime::RegisterContextImport(const std::string& name, ContextImport handler) {
    _state->importIndex[name] = _state->imports.size();
    _state->imports.push_back({name, nullptr, handler});
}

void Runtime::RegisterDataImport(const std::string& name, void* hostAddress) {
    _state->dataImports[name] = hostAddress;
}

void Runtime::LoadImage(const std::filesystem::path& path) {
    auto elf = ElfImage::Load(path);
    _state->imageSize = (elf.ImageSize() + 0xffff) & ~0xffffull;
    void* memory = mmap(nullptr, _state->imageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        throw std::runtime_error("Cannot map guest image of " + std::to_string(_state->imageSize) + " bytes");
    }
    _state->image = static_cast<std::uint8_t*>(memory);
    for (const auto& segment : elf.Segments()) {
        std::memcpy(_state->image + segment.vaddr, elf.Bytes().data() + segment.fileOffset, segment.fileSize);
    }
    auto base = reinterpret_cast<std::uint64_t>(_state->image);
    _state->entry = elf.Entry();
    for (const auto& symbol : elf.Symbols()) {
        if (symbol.defined && !symbol.name.empty()) {
            _state->symbols[symbol.name] = symbol.value;
        }
    }
    const auto& dynamic = elf.DynamicSymbols();
    for (const auto& relocation : elf.Relocations()) {
        auto* slot = reinterpret_cast<std::uint64_t*>(_state->image + relocation.offset);
        switch (relocation.type) {
        case RX8664Relative:
            *slot = base + static_cast<std::uint64_t>(relocation.addend);
            break;
        case RX8664_64:
        case RX8664GlobDat:
        case RX8664JumpSlot: {
            if (relocation.symbolIndex >= dynamic.size()) {
                throw std::runtime_error("Relocation at " + Hex(relocation.offset) + " names a missing symbol");
            }
            const auto& symbol = dynamic[relocation.symbolIndex];
            if (symbol.defined) {
                *slot = base + symbol.value + static_cast<std::uint64_t>(relocation.addend);
            } else if (auto data = _state->dataImports.find(symbol.name); data != _state->dataImports.end()) {
                *slot = reinterpret_cast<std::uint64_t>(data->second) + static_cast<std::uint64_t>(relocation.addend);
            } else if (auto import = _state->importIndex.find(symbol.name); import != _state->importIndex.end()) {
                *slot = reinterpret_cast<std::uint64_t>(_state->importRegion) + import->second * ImportStride;
            } else {
                throw std::runtime_error("Unresolved guest import " + symbol.name);
            }
            break;
        }
        default:
            throw std::runtime_error("Unsupported relocation type " + std::to_string(relocation.type) + " at " +
                Hex(relocation.offset));
        }
    }
}

std::uint64_t Runtime::ImageBase() const {
    return reinterpret_cast<std::uint64_t>(_state->image);
}

std::uint64_t Runtime::EntryAddress() const {
    return ImageBase() + _state->entry;
}

std::uint64_t Runtime::SymbolAddress(const std::string& name) const {
    auto it = _state->symbols.find(name);
    if (it == _state->symbols.end()) {
        throw std::runtime_error("Unknown guest symbol " + name);
    }
    return ImageBase() + it->second;
}

GuestContext* Runtime::CurrentContext() {
    if (!currentThread) {
        auto state = std::make_unique<ThreadState>();
        state->context = std::make_unique<GuestContext>();
        std::memset(state->context.get(), 0, sizeof(GuestContext));
        state->stack = mmap(nullptr, GuestStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        state->tls = mmap(nullptr, TlsBlockSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (state->stack == MAP_FAILED || state->tls == MAP_FAILED) {
            throw std::runtime_error("Cannot allocate a guest thread stack");
        }
        auto* context = state->context.get();
        context->stackBase = reinterpret_cast<std::uint64_t>(state->stack);
        context->stackSize = GuestStackSize;
        context->gpr[Rsp] = context->stackBase + GuestStackSize - 64;
        context->fsBase = reinterpret_cast<std::uint64_t>(state->tls) + TlsBlockSize / 2;
        *reinterpret_cast<std::uint64_t*>(context->fsBase) = context->fsBase;
        context->mxcsr = 0x1f80;
        context->runtime = this;
        currentThread = std::move(state);
    }
    currentThread->context->imageBase = ImageBase();
    return currentThread->context.get();
}

std::uint64_t Runtime::CallGuest(std::uint64_t target, std::initializer_list<std::uint64_t> arguments) {
    static constexpr GuestRegister argumentRegisters[] = {Rdi, Rsi, Rdx, Rcx, R8, R9};
    if (arguments.size() > 6) {
        throw std::runtime_error("CallGuest supports at most six integer arguments");
    }
    auto* c = CurrentContext();
    std::size_t i = 0;
    for (auto argument : arguments) {
        c->gpr[argumentRegisters[i++]] = argument;
    }
    auto savedRsp = c->gpr[Rsp];
    c->gpr[Rsp] = (savedRsp - 128) & ~0xfull;
    c->gpr[Rsp] -= 8;
    St<std::uint64_t>(c->gpr[Rsp], _state->sentinel);
    Dispatch(c, target);
    if (c->returnTarget != _state->sentinel) {
        throw std::runtime_error("Guest call to " + Hex(target) + " returned to " + Hex(c->returnTarget));
    }
    c->gpr[Rsp] = savedRsp;
    return c->gpr[Rax];
}

void Runtime::Dispatch(GuestContext* c, std::uint64_t target) {
    _state->dispatches.fetch_add(1, std::memory_order_relaxed);
    auto region = reinterpret_cast<std::uint64_t>(_state->importRegion);
    if (target >= region && target < region + _state->imports.size() * ImportStride) {
        _state->importCalls.fetch_add(1, std::memory_order_relaxed);
        const auto& import = _state->imports[(target - region) / ImportStride];
        if (import.contextHandler != nullptr) {
            import.contextHandler(c);
        } else {
            aps5_host_call_sysv(c, import.hostFunction);
        }
        c->returnTarget = Ld<std::uint64_t>(c->gpr[Rsp]);
        c->gpr[Rsp] += 8;
        return;
    }
    auto it = _state->functions.find(target - ImageBase());
    if (it == _state->functions.end()) {
        throw std::runtime_error("No translated code at guest address " + Hex(target) + " (image offset " +
            Hex(target - ImageBase()) + "); runtime-generated or undiscovered code needs the JIT fallback");
    }
    it->second(c);
}

std::size_t Runtime::DispatchCount() const {
    return _state->dispatches.load();
}

std::size_t Runtime::ImportCallCount() const {
    return _state->importCalls.load();
}

void JumpIndirect(GuestContext* c) {
    static_cast<Runtime*>(c->runtime)->Dispatch(c, c->jumpTarget);
}

void CallIndirect(GuestContext* c, std::uint64_t target) {
    static_cast<Runtime*>(c->runtime)->Dispatch(c, target);
}

void ReturnMismatch(GuestContext* c, std::uint64_t expected) {
    throw std::runtime_error("Guest returned to " + Hex(c->returnTarget) + " instead of " + Hex(expected));
}

void Unsupported(GuestContext*, std::uint64_t address, const char* mnemonic) {
    throw std::runtime_error(std::string("Untranslated instruction ") + mnemonic + " at image offset " + Hex(address));
}

void FellOffCode(GuestContext*, std::uint64_t address) {
    throw std::runtime_error("Execution reached undiscovered code at image offset " + Hex(address));
}

void Trap(GuestContext*, std::uint64_t address, const char* mnemonic) {
    throw std::runtime_error(std::string("Guest executed ") + mnemonic + " at image offset " + Hex(address));
}

void DivideError(GuestContext*, std::uint64_t address) {
    throw std::runtime_error("Guest divide error at image offset " + Hex(address));
}

void RepStos(GuestContext* c, unsigned width) {
    auto count = c->gpr[Rcx];
    auto value = c->gpr[Rax];
    if (c->df != 0) {
        throw std::runtime_error("rep stos with the direction flag set");
    }
    auto* destination = reinterpret_cast<std::uint8_t*>(c->gpr[Rdi]);
    if (width == 1) {
        std::memset(destination, static_cast<int>(value & 0xff), count);
    } else {
        for (std::uint64_t i = 0; i < count; ++i) {
            std::memcpy(destination + i * width, &value, width);
        }
    }
    c->gpr[Rdi] += count * width;
    c->gpr[Rcx] = 0;
}

void RepMovs(GuestContext* c, unsigned width) {
    auto count = c->gpr[Rcx];
    if (c->df != 0) {
        throw std::runtime_error("rep movs with the direction flag set");
    }
    auto* destination = reinterpret_cast<std::uint8_t*>(c->gpr[Rdi]);
    const auto* source = reinterpret_cast<const std::uint8_t*>(c->gpr[Rsi]);
    for (std::uint64_t i = 0; i < count * width; ++i) {
        destination[i] = source[i];
    }
    c->gpr[Rdi] += count * width;
    c->gpr[Rsi] += count * width;
    c->gpr[Rcx] = 0;
}

void Cpuid(GuestContext* c) {
    auto leaf = static_cast<std::uint32_t>(c->gpr[Rax]);
    std::uint32_t eax = 0;
    std::uint32_t ebx = 0;
    std::uint32_t ecx = 0;
    std::uint32_t edx = 0;
    if (leaf == 0) {
        eax = 7;
        std::memcpy(&ebx, "Auth", 4);
        std::memcpy(&edx, "enti", 4);
        std::memcpy(&ecx, "cAMD", 4);
    } else if (leaf == 1) {
        eax = 0x00870f10;
        edx = (1u << 25) | (1u << 26) | (1u << 23) | (1u << 15) | (1u << 8);
        ecx = (1u << 0) | (1u << 9) | (1u << 19) | (1u << 20) | (1u << 23) | (1u << 12) | (1u << 28) | (1u << 27);
    } else if (leaf == 7) {
        ebx = (1u << 3) | (1u << 5) | (1u << 8);
    }
    c->gpr[Rax] = eax;
    c->gpr[Rbx] = ebx;
    c->gpr[Rcx] = ecx;
    c->gpr[Rdx] = edx;
}

void Rdtsc(GuestContext* c) {
    auto ticks = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    c->gpr[Rax] = ticks & 0xffffffff;
    c->gpr[Rdx] = ticks >> 32;
}

std::uint64_t HostRandom() {
    thread_local std::mt19937_64 generator{std::random_device{}()};
    return generator();
}

std::uint64_t GsBase(GuestContext*, std::uint64_t) {
    throw std::runtime_error("GS-relative guest memory access");
}

}
