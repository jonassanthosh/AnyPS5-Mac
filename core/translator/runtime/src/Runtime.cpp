#include "translator/Runtime.hpp"
#include "translator/GuestJump.hpp"
#include "translator/Semantics.hpp"
#include "ElfImage.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
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
constexpr std::uint32_t RX8664DtpMod64 = 16;
constexpr std::uint32_t RX8664DtpOff64 = 17;
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
    bool missing = false;
    std::atomic<std::uint64_t> guestTarget{0};

    Import(std::string importName, void* host, ContextImport handler)
        : name(std::move(importName)), hostFunction(host), contextHandler(handler) {}
};

struct PendingSlot {
    std::string nid;
    std::uint64_t* slot;
    std::uint64_t addend;
    std::size_t import;
};

struct TlsTemplate {
    std::uint64_t vaddr = 0;
    std::uint64_t fileSize = 0;
    std::uint64_t memorySize = 0;
    std::uint64_t align = 1;
};

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) {
    return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
}

struct LoadedImage {
    LoadedImageInfo info;
    std::uint8_t* memory = nullptr;
    const TranslatedImage* translated = nullptr;
    std::unordered_map<std::uint64_t, TranslatedFunction> functions;
    std::vector<ElfProgramHeader> headers;
    TlsTemplate tls;
    std::unordered_map<std::string, std::uint64_t> exports;
    std::unordered_map<std::string, std::uint64_t> symbols;
    std::atomic<bool> live{false};
};

constexpr std::size_t MaximumImages = 128;

struct ThreadState {
    std::unique_ptr<GuestContext> context;
    void* stack = nullptr;
    std::size_t stackBytes = 0;
    void* tls = nullptr;
    std::vector<void*> dynamicTls;

    ~ThreadState() {
        if (stack != nullptr) munmap(stack, stackBytes);
        if (tls != nullptr) munmap(tls, TlsBlockSize);
        for (auto* block : dynamicTls) std::free(block);
    }
};

thread_local std::unique_ptr<ThreadState> currentThread;

std::string NidOf(const std::string& name) {
    return name.substr(0, name.find('#'));
}

}

struct Runtime::State {
    std::array<std::unique_ptr<LoadedImage>, MaximumImages> images;
    std::atomic<std::size_t> imageCount{0};
    std::mutex loadLock;
    std::size_t mainImage = MaximumImages;
    std::deque<Import> imports;
    std::unordered_map<std::string, std::size_t> importIndex;
    std::vector<PendingSlot> pendingSlots;
    std::unordered_map<std::string, void*> dataImports;
    std::uint8_t* importRegion = nullptr;
    std::uint64_t importRegionSize = 0;
    std::uint64_t sentinel = 0;
    ImportResolver resolver;
    std::atomic<std::size_t> dispatches{0};
    std::atomic<std::size_t> importCalls{0};

    LoadedImage* Containing(std::uint64_t address) const {
        const auto count = imageCount.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < count; ++i) {
            auto* image = images[i].get();
            if (image != nullptr && image->live.load(std::memory_order_acquire) && address >= image->info.base &&
                address < image->info.base + image->info.size) {
                return image;
            }
        }
        return nullptr;
    }

    const LoadedImage& Main() const {
        if (mainImage == MaximumImages) {
            throw std::runtime_error("No main guest image is loaded");
        }
        return *images[mainImage];
    }
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

void Runtime::RegisterImport(const std::string& name, void* hostFunction) {
    if ((_state->imports.size() + 2) * ImportStride >= _state->importRegionSize) {
        throw std::runtime_error("Too many guest imports for the import address range");
    }
    _state->importIndex[name] = _state->imports.size();
    _state->imports.emplace_back(name, hostFunction, nullptr);
}

void Runtime::RegisterContextImport(const std::string& name, ContextImport handler) {
    if ((_state->imports.size() + 2) * ImportStride >= _state->importRegionSize) {
        throw std::runtime_error("Too many guest imports for the import address range");
    }
    _state->importIndex[name] = _state->imports.size();
    _state->imports.emplace_back(name, nullptr, handler);
}

void Runtime::RegisterDataImport(const std::string& name, void* hostAddress) {
    _state->dataImports[name] = hostAddress;
}

void Runtime::SetImportResolver(ImportResolver resolver) {
    _state->resolver = std::move(resolver);
}

bool Runtime::IsTranslatedFunction(std::uint64_t address) const {
    const auto* image = _state->Containing(address);
    return image != nullptr && image->functions.count(address - image->info.base) != 0;
}

std::string Runtime::DescribeAddress(std::uint64_t address) const {
    if (const auto* image = _state->Containing(address)) {
        return image->info.name + "+" + Hex(address - image->info.base);
    }
    auto region = reinterpret_cast<std::uint64_t>(_state->importRegion);
    if (address >= region && address < region + _state->imports.size() * ImportStride) {
        return "import " + _state->imports[(address - region) / ImportStride].name;
    }
    return {};
}

void* Runtime::NativeImportAt(std::uint64_t address) const {
    auto region = reinterpret_cast<std::uint64_t>(_state->importRegion);
    if (address < region || address >= region + _state->imports.size() * ImportStride || (address - region) % ImportStride != 0) {
        return nullptr;
    }
    return _state->imports[(address - region) / ImportStride].hostFunction;
}

std::size_t Runtime::LoadImage(const std::filesystem::path& path, const TranslatedImage& translated, bool main) {
    std::lock_guard lock(_state->loadLock);
    const auto index = _state->imageCount.load();
    if (index >= MaximumImages) {
        throw std::runtime_error("Too many guest images");
    }
    if (main && _state->mainImage != MaximumImages) {
        throw std::runtime_error("A main guest image is already loaded");
    }
    auto elf = ElfImage::Load(path);
    auto image = std::make_unique<LoadedImage>();
    auto& info = image->info;
    info.name = path.filename().string();
    info.tlsModuleId = index + 1;
    image->headers = elf.ProgramHeaders();
    for (const auto& header : elf.ProgramHeaders()) {
        if (header.type == 7) {
            image->tls = {header.vaddr, header.fileSize, header.memorySize, std::max<std::uint64_t>(header.align, 1)};
        }
    }
    info.size = (elf.ImageSize() + 0xffff) & ~0xffffull;
    void* memory = mmap(nullptr, info.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        throw std::runtime_error("Cannot map guest image of " + std::to_string(info.size) + " bytes");
    }
    image->memory = static_cast<std::uint8_t*>(memory);
    for (const auto& segment : elf.Segments()) {
        std::memcpy(image->memory + segment.vaddr, elf.Bytes().data() + segment.fileOffset, segment.fileSize);
    }
    const auto base = reinterpret_cast<std::uint64_t>(image->memory);
    info.base = base;
    info.entry = elf.Entry();
    info.init = elf.Init();
    info.programHeaders = image->headers.data();
    info.programHeaderCount = image->headers.size();
    image->translated = &translated;
    *translated.base = base;
    for (std::size_t i = 0; i < translated.count; ++i) {
        image->functions.emplace(translated.functions[i].vaddr, translated.functions[i].function);
    }
    for (const auto& symbol : elf.Symbols()) {
        if (symbol.defined && !symbol.name.empty()) image->symbols[symbol.name] = symbol.value;
    }
    for (const auto& symbol : elf.DynamicSymbols()) {
        if (symbol.defined && symbol.value != 0 && !symbol.name.empty()) {
            image->exports[NidOf(symbol.name)] = base + symbol.value;
            image->symbols.emplace(symbol.name, symbol.value);
        }
    }
    const auto& dynamic = elf.DynamicSymbols();
    for (const auto& relocation : elf.Relocations()) {
        auto* slot = reinterpret_cast<std::uint64_t*>(image->memory + relocation.offset);
        const auto addend = static_cast<std::uint64_t>(relocation.addend);
        if (relocation.type == RX8664Relative) {
            *slot = base + addend;
            continue;
        }
        if (relocation.symbolIndex >= dynamic.size()) {
            throw std::runtime_error("Relocation at " + Hex(relocation.offset) + " names a missing symbol");
        }
        const auto& symbol = dynamic[relocation.symbolIndex];
        if (relocation.type == RX8664DtpMod64) {
            *slot = info.tlsModuleId;
            continue;
        }
        if (relocation.type == RX8664DtpOff64) {
            if (!symbol.defined) throw std::runtime_error("TLS offset relocation against an imported symbol " + symbol.name);
            *slot = symbol.value + addend;
            continue;
        }
        if (relocation.type != RX8664_64 && relocation.type != RX8664GlobDat && relocation.type != RX8664JumpSlot) {
            throw std::runtime_error("Unsupported relocation type " + std::to_string(relocation.type) + " at " + Hex(relocation.offset) +
                " in " + info.name);
        }
        if (symbol.defined) {
            *slot = base + symbol.value + addend;
        } else if (auto data = _state->dataImports.find(symbol.name); data != _state->dataImports.end()) {
            *slot = reinterpret_cast<std::uint64_t>(data->second) + addend;
        } else if (auto import = _state->importIndex.find(symbol.name); import != _state->importIndex.end()) {
            *slot = reinterpret_cast<std::uint64_t>(_state->importRegion) + import->second * ImportStride + addend;
            if (_state->imports[import->second].missing) {
                _state->pendingSlots.push_back({NidOf(symbol.name), slot, addend, import->second});
            }
        } else {
            const bool data = symbol.type == 1;
            ImportResolution resolution;
            if (_state->resolver) {
                resolution = _state->resolver(symbol.name, data);
            }
            if (resolution.kind == ImportResolution::Kind::Data || resolution.kind == ImportResolution::Kind::Guest) {
                if (resolution.kind == ImportResolution::Kind::Data) _state->dataImports[symbol.name] = resolution.address;
                *slot = reinterpret_cast<std::uint64_t>(resolution.address) + addend;
                continue;
            }
            if (data) {
                throw std::runtime_error("Unresolved guest data import " + symbol.name + " in " + info.name);
            }
            if (resolution.kind == ImportResolution::Kind::Context) {
                RegisterContextImport(symbol.name, resolution.handler);
            } else {
                RegisterImport(symbol.name, resolution.address);
                if (resolution.kind == ImportResolution::Kind::Missing) {
                    ++info.unresolvedImports;
                    _state->imports.back().missing = true;
                    _state->pendingSlots.push_back({NidOf(symbol.name), slot, addend, _state->imports.size() - 1});
                }
            }
            *slot = reinterpret_cast<std::uint64_t>(_state->importRegion) + (_state->imports.size() - 1) * ImportStride + addend;
        }
    }
    if (elf.InitArray() != 0) {
        for (std::uint64_t offset = 0; offset + 8 <= elf.InitArraySize(); offset += 8) {
            std::uint64_t target;
            std::memcpy(&target, image->memory + elf.InitArray() + offset, sizeof(target));
            if (target != 0 && target != ~0ull) info.initArray.push_back(target);
        }
    }
    image->live.store(true, std::memory_order_release);
    const auto* loaded = image.get();
    _state->images[index] = std::move(image);
    _state->imageCount.store(index + 1, std::memory_order_release);
    if (main) _state->mainImage = index;
    std::erase_if(_state->pendingSlots, [&](const PendingSlot& pending) {
        const auto found = loaded->exports.find(pending.nid);
        if (found == loaded->exports.end()) return false;
        *pending.slot = found->second + pending.addend;
        _state->imports[pending.import].guestTarget.store(found->second, std::memory_order_release);
        return true;
    });
    return index;
}

void Runtime::UnloadImage(std::size_t index) {
    std::lock_guard lock(_state->loadLock);
    if (index >= _state->imageCount.load() || !_state->images[index] || index == _state->mainImage) {
        throw std::runtime_error("Cannot unload guest image " + std::to_string(index));
    }
    _state->images[index]->live.store(false, std::memory_order_release);
}

const LoadedImageInfo& Runtime::Image(std::size_t index) const {
    if (index >= _state->imageCount.load() || !_state->images[index]) {
        throw std::runtime_error("No guest image " + std::to_string(index));
    }
    return _state->images[index]->info;
}

std::size_t Runtime::ImageCount() const {
    return _state->imageCount.load();
}

std::uint64_t Runtime::GuestExport(const std::string& nid) const {
    const auto count = _state->imageCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        const auto* image = _state->images[i].get();
        if (image == nullptr || !image->live.load()) continue;
        if (auto it = image->exports.find(nid); it != image->exports.end()) return it->second;
    }
    return 0;
}

std::uint64_t Runtime::ImageExport(std::size_t index, const std::string& nameOrNid) const {
    if (index >= _state->imageCount.load() || !_state->images[index]) return 0;
    const auto& image = *_state->images[index];
    if (auto it = image.exports.find(NidOf(nameOrNid)); it != image.exports.end()) return it->second;
    if (auto it = image.symbols.find(nameOrNid); it != image.symbols.end()) return image.info.base + it->second;
    return 0;
}

std::uint64_t Runtime::ImageBase() const {
    return _state->Main().info.base;
}

std::uint64_t Runtime::EntryAddress() const {
    return ImageBase() + _state->Main().info.entry;
}

std::uint64_t Runtime::SymbolAddress(const std::string& name) const {
    const auto& main = _state->Main();
    auto it = main.symbols.find(name);
    if (it == main.symbols.end()) {
        throw std::runtime_error("Unknown guest symbol " + name);
    }
    return main.info.base + it->second;
}

GuestContext* Runtime::CurrentContext() {
    return PrepareThread(0);
}

GuestContext* Runtime::PrepareThread(std::size_t stackBytes) {
    if (!currentThread) {
        const std::size_t size = std::max<std::size_t>(AlignUp(stackBytes, 0x4000), GuestStackSize);
        auto state = std::make_unique<ThreadState>();
        state->context = std::make_unique<GuestContext>();
        std::memset(state->context.get(), 0, sizeof(GuestContext));
        state->stackBytes = size;
        state->stack = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        state->tls = mmap(nullptr, TlsBlockSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (state->stack == MAP_FAILED || state->tls == MAP_FAILED) {
            throw std::runtime_error("Cannot allocate a guest thread stack");
        }
        auto* context = state->context.get();
        context->stackBase = reinterpret_cast<std::uint64_t>(state->stack);
        context->stackSize = size;
        context->gpr[Rsp] = context->stackBase + size - 64;
        context->fsBase = reinterpret_cast<std::uint64_t>(state->tls) + TlsBlockSize / 2;
        *reinterpret_cast<std::uint64_t*>(context->fsBase) = context->fsBase;
        if (_state->mainImage != MaximumImages) {
            const auto& main = _state->Main();
            if (main.tls.memorySize != 0) {
                const auto size = AlignUp(main.tls.memorySize, main.tls.align);
                if (size > TlsBlockSize / 2) {
                    throw std::runtime_error("Guest TLS block of " + std::to_string(size) + " bytes exceeds the reserved area");
                }
                std::memcpy(reinterpret_cast<void*>(context->fsBase - size), main.memory + main.tls.vaddr, main.tls.fileSize);
            }
        }
        context->mxcsr = 0x1f80;
        context->runtime = this;
        currentThread = std::move(state);
    }
    return currentThread->context.get();
}

std::uint64_t Runtime::TlsAddress(GuestContext* c, std::uint64_t module, std::uint64_t offset) {
    if (module == 0 || module > _state->imageCount.load() || !_state->images[module - 1]) {
        throw std::runtime_error("TLS access to unknown guest module " + std::to_string(module));
    }
    const auto& image = *_state->images[module - 1];
    if (module - 1 == _state->mainImage) {
        return c->fsBase - AlignUp(image.tls.memorySize, image.tls.align) + offset;
    }
    CurrentContext();
    auto& blocks = currentThread->dynamicTls;
    if (blocks.size() < module) blocks.resize(module, nullptr);
    if (blocks[module - 1] == nullptr) {
        const auto size = std::max<std::uint64_t>(AlignUp(image.tls.memorySize, 16), 16);
        void* block = std::aligned_alloc(std::max<std::uint64_t>(image.tls.align, 16), size);
        if (block == nullptr) throw std::runtime_error("Cannot allocate guest TLS");
        std::memset(block, 0, size);
        std::memcpy(block, image.memory + image.tls.vaddr, image.tls.fileSize);
        blocks[module - 1] = block;
    }
    return reinterpret_cast<std::uint64_t>(blocks[module - 1]) + offset;
}

GuestContext* Runtime::ExistingContext() {
    return currentThread ? currentThread->context.get() : nullptr;
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
    return CallGuestWithRegisters(target);
}

std::uint64_t Runtime::CallGuestWithRegisters(std::uint64_t target) {
    auto* c = CurrentContext();
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
        static const std::string traceFilter = [] {
            const char* value = std::getenv("APS5_TRACE_IMPORTS");
            return value != nullptr ? std::string(value) : std::string();
        }();
        const bool trace = !traceFilter.empty() &&
            (traceFilter == "1" || traceFilter.find(import.name.substr(0, import.name.find('#'))) != std::string::npos);
        const auto rdi = c->gpr[Rdi];
        const auto rsi = c->gpr[Rsi];
        const auto rdx = c->gpr[Rdx];
        if (import.contextHandler != nullptr) {
            import.contextHandler(c);
        } else if (import.hostFunction != nullptr) {
            try {
                aps5_host_call_sysv(c, import.hostFunction);
            } catch (const GuestResume& resume) {
                ApplyGuestResume(c, resume);
            }
        } else if (const auto target = import.guestTarget.load(std::memory_order_acquire); target != 0) {
            Dispatch(c, target);
            return;
        } else {
            throw std::runtime_error("Guest called unresolved import " + import.name);
        }
        if (trace) {
            std::fprintf(stderr, "[import] %s(0x%llx, 0x%llx, 0x%llx) = 0x%llx from %s", import.name.c_str(), static_cast<unsigned long long>(rdi),
                static_cast<unsigned long long>(rsi), static_cast<unsigned long long>(rdx), static_cast<unsigned long long>(c->gpr[Rax]),
                DescribeAddress(Ld<std::uint64_t>(c->gpr[Rsp])).c_str());
            auto frame = c->gpr[Rbp];
            for (int depth = 0; depth < 8 && frame >= c->gpr[Rsp] && frame < c->stackBase + c->stackSize; ++depth) {
                std::fprintf(stderr, " < %s", DescribeAddress(Ld<std::uint64_t>(frame + 8)).c_str());
                frame = Ld<std::uint64_t>(frame);
            }
            std::fprintf(stderr, "\n");
        }
        c->returnTarget = Ld<std::uint64_t>(c->gpr[Rsp]);
        c->gpr[Rsp] += 8;
        return;
    }
    auto* image = _state->Containing(target);
    if (image != nullptr) {
        if (auto it = image->functions.find(target - image->info.base); it != image->functions.end()) {
            it->second(c);
            return;
        }
        throw std::runtime_error("No translated code at guest address " + Hex(target) + " (" + image->info.name + " offset " +
            Hex(target - image->info.base) + "); runtime-generated or undiscovered code needs the JIT fallback");
    }
    throw std::runtime_error("Guest control transfer to " + Hex(target) + ", outside every guest image");
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
