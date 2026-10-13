#include "translator/GuestJump.hpp"
#include "translator/Runtime.hpp"
#include <nid/NidCompute.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

extern "C" const translator::TranslatedImage* const aps5TitleImages[];
extern "C" const std::size_t aps5TitleImageCount;

namespace {

using GuestImageRegister = const void* (*)(const char*, std::uintptr_t, const void*, std::uint16_t, std::size_t, bool);
using GuestImageUnregister = void (*)(const void*);

struct GuestImageLoaderTable {
    void* (*load)(const char* path);
    void* (*symbol)(void* image, const char* name);
    void (*unload)(void* image);
};

struct PendingModuleArgs {
    std::size_t args;
    const void* argp;
};

struct LoadedModule {
    std::string name;
    std::size_t index;
    const void* registration;
    std::size_t references;
};

constexpr std::size_t GuestHostStackBytes = 64ull << 20;

std::map<std::string, std::filesystem::path> guestFiles;
std::mutex modulesLock;
std::recursive_mutex loadLock;
std::vector<std::unique_ptr<LoadedModule>> modules;

template <typename T>
T Required(const char* name) {
    auto* symbol = dlsym(RTLD_DEFAULT, name);
    if (symbol == nullptr) {
        throw std::runtime_error(std::string("System libraries do not provide ") + name);
    }
    return reinterpret_cast<T>(symbol);
}

void LoadLibraries(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> libraries;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".prx") {
            libraries.push_back(entry.path());
        }
    }
    std::sort(libraries.begin(), libraries.end(), [](const auto& a, const auto& b) {
        auto rank = [](const std::filesystem::path& path) {
            const auto name = path.filename().string();
            return name == "libc.prx" ? 0 : name == "libkernel.prx" ? 1 : 2;
        };
        return std::make_pair(rank(a), a.filename().string()) < std::make_pair(rank(b), b.filename().string());
    });
    for (const auto& library : libraries) {
        if (dlopen(library.c_str(), RTLD_NOW | RTLD_GLOBAL) == nullptr) {
            throw std::runtime_error(std::string("Cannot load system library: ") + dlerror());
        }
    }
    std::fprintf(stderr, "[title] loaded %zu system libraries from %s\n", libraries.size(), directory.c_str());
}

void IndexGuestFiles(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        const auto extension = entry.path().extension();
        if (entry.is_regular_file() && (extension == ".prx" || extension == ".sprx" || entry.path().filename() == "eboot.bin")) {
            guestFiles.emplace(entry.path().filename().string(), entry.path());
        }
    }
}

const translator::TranslatedImage* TranslatedFor(const std::string& name) {
    for (std::size_t i = 0; i < aps5TitleImageCount; ++i) {
        if (name == aps5TitleImages[i]->name) return aps5TitleImages[i];
    }
    return nullptr;
}

const std::map<std::string, translator::ContextImport>& RuntimeImportsByNid() {
    static const auto imports = [] {
        std::map<std::string, translator::ContextImport> byNid;
        for (const auto& [name, handler] : translator::RuntimeContextImports()) {
            byNid.emplace(Nid::ComputeNid(name, ""), handler);
        }
        return byNid;
    }();
    return imports;
}

translator::ImportResolution Resolve(const std::string& name, bool data) {
    translator::ImportResolution resolution;
    const auto nid = name.substr(0, name.find('#'));
    if (auto found = RuntimeImportsByNid().find(nid); !data && found != RuntimeImportsByNid().end()) {
        resolution.kind = translator::ImportResolution::Kind::Context;
        resolution.handler = found->second;
        return resolution;
    }
    if (const auto guest = translator::Runtime::Instance().GuestExport(nid); guest != 0) {
        resolution.kind = translator::ImportResolution::Kind::Guest;
        resolution.address = reinterpret_cast<void*>(guest);
        return resolution;
    }
    void* address = dlsym(RTLD_DEFAULT, nid.c_str());
    if (address == nullptr) {
        std::fprintf(stderr, "[title] unresolved %s import %s\n", data ? "data" : "function", name.c_str());
        return resolution;
    }
    resolution.kind = data ? translator::ImportResolution::Kind::Data : translator::ImportResolution::Kind::Function;
    resolution.address = address;
    return resolution;
}

const void* RegisterWithLibc(std::size_t index, bool main) {
    const auto& info = translator::Runtime::Instance().Image(index);
    static const auto registerImage = Required<GuestImageRegister>("GuestImageRegister_nid_no_patch");
    return registerImage(info.name.c_str(), static_cast<std::uintptr_t>(info.base), info.programHeaders,
        static_cast<std::uint16_t>(info.programHeaderCount), info.tlsModuleId, main);
}

void* LoadModule(const char* path) {
    const auto name = std::filesystem::path(path).filename().string();
    const auto* translated = TranslatedFor(name);
    const auto file = guestFiles.find(name);
    if (translated == nullptr || file == guestFiles.end()) {
        std::fprintf(stderr, "[title] guest module %s is not part of this translation\n", path);
        return nullptr;
    }
    std::lock_guard loading(loadLock);
    {
        std::lock_guard lock(modulesLock);
        for (const auto& module : modules) {
            if (module->name == name) {
                ++module->references;
                return module.get();
            }
        }
    }
    try {
        auto& runtime = translator::Runtime::Instance();
        const auto index = runtime.LoadImage(file->second, *translated, false);
        const auto& info = runtime.Image(index);
        std::fprintf(stderr, "[title] loaded guest module %s at 0x%llx, %zu unresolved imports\n", name.c_str(),
            static_cast<unsigned long long>(info.base), info.unresolvedImports);
        auto module = std::make_unique<LoadedModule>(LoadedModule{name, index, RegisterWithLibc(index, false), 1});
        static const auto pendingArgs = Required<const void* (*)()>("__aps5_get_pending_module_args_nid_no_patch");
        static const auto setResult = Required<void (*)(int)>("__aps5_set_module_init_result_nid_no_patch");
        const auto* pending = static_cast<const PendingModuleArgs*>(pendingArgs());
        if (info.init != 0) {
            const auto result = runtime.CallGuest(info.base + info.init,
                {pending != nullptr ? pending->args : 0, reinterpret_cast<std::uint64_t>(pending != nullptr ? pending->argp : nullptr), 0});
            setResult(static_cast<int>(result));
        }
        for (const auto initializer : info.initArray) {
            runtime.CallGuest(initializer, {0, 0, 0});
        }
        std::lock_guard lock(modulesLock);
        modules.push_back(std::move(module));
        return modules.back().get();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[title] loading guest module %s failed: %s\n", name.c_str(), error.what());
        return nullptr;
    }
}

void* ModuleSymbol(void* handle, const char* name) {
    const auto* module = static_cast<const LoadedModule*>(handle);
    return reinterpret_cast<void*>(translator::Runtime::Instance().ImageExport(module->index, name));
}

void UnloadModule(void* handle) {
    auto* module = static_cast<LoadedModule*>(handle);
    {
        std::lock_guard lock(modulesLock);
        if (--module->references != 0) return;
    }
    static const auto unregisterImage = Required<GuestImageUnregister>("GuestImageUnregister_nid_no_patch");
    unregisterImage(module->registration);
    translator::Runtime::Instance().UnloadImage(module->index);
    std::lock_guard lock(modulesLock);
    std::erase_if(modules, [module](const auto& loaded) { return loaded.get() == module; });
}

std::vector<std::uint64_t> BuildProcessStack(std::vector<std::string>& arguments) {
    std::vector<std::uint64_t> stack;
    stack.push_back(arguments.size());
    for (auto& argument : arguments) {
        stack.push_back(reinterpret_cast<std::uint64_t>(argument.c_str()));
    }
    stack.push_back(0);
    stack.push_back(0);
    stack.push_back(0);
    stack.push_back(0);
    return stack;
}

struct TitleArguments {
    int argc;
    char** argv;
};

int RunTitle(int argc, char** argv) {
    try {
        auto& runtime = translator::Runtime::Instance();
        IndexGuestFiles(argv[1]);
        LoadLibraries(argv[2]);
        runtime.SetImportResolver(Resolve);
        const auto* mainTranslation = TranslatedFor("eboot.bin");
        const auto eboot = guestFiles.find("eboot.bin");
        if (mainTranslation == nullptr || eboot == guestFiles.end()) {
            throw std::runtime_error("The title directory has no eboot.bin matching this translation");
        }
        const auto main = runtime.LoadImage(eboot->second, *mainTranslation, true);
        const auto& info = runtime.Image(main);
        std::fprintf(stderr, "[title] mapped eboot.bin at 0x%llx (%llu bytes), %zu translated images, %zu unresolved imports\n",
            static_cast<unsigned long long>(info.base), static_cast<unsigned long long>(info.size), aps5TitleImageCount,
            info.unresolvedImports);
        RegisterWithLibc(main, true);
        static const GuestImageLoaderTable loader{LoadModule, ModuleSymbol, UnloadModule};
        Required<void (*)(const void*)>("GuestImageSetLoader_nid_no_patch")(&loader);
        Required<void (*)(bool (*)(void*, void*, unsigned, std::uint64_t))>("GuestVarArgsSetProvider_nid_no_patch")(
            translator::ProvideGuestVarArgs);
        Required<void (*)(bool (*)(std::size_t, std::uintptr_t*, std::uintptr_t*))>("GuestThreadStackSetProvider_nid_no_patch")(
            translator::ProvideGuestThreadStack);
        Required<void (*)(bool (*)(void*))>("GuestRegistersSetProvider_nid_no_patch")(translator::ProvideGuestRegisters);
        Required<void (*)(void (*)(const void*))>("GuestRegistersSetInstaller_nid_no_patch")(translator::ProvideGuestInstall);
        Required<void (*)(void* (*)(std::uintptr_t))>("GuestNativeImportSetResolver_nid_no_patch")(
            [](std::uintptr_t address) { return translator::Runtime::Instance().NativeImportAt(address); });
        runtime.InstallCallbackBridge();
        for (std::size_t i = 0; i < aps5TitleImageCount; ++i) {
            const std::string name = aps5TitleImages[i]->name;
            const auto file = guestFiles.find(name);
            if (file == guestFiles.end() || file->second.parent_path().filename() != "sce_module" ||
                std::filesystem::exists(std::filesystem::path(argv[2]) / name)) {
                continue;
            }
            if (LoadModule(file->second.c_str()) == nullptr) {
                throw std::runtime_error("Cannot load the title's module " + name);
            }
        }
        std::vector<std::string> arguments{"eboot.bin"};
        for (int i = 3; i < argc; ++i) {
            arguments.emplace_back(argv[i]);
        }
        auto stack = BuildProcessStack(arguments);
        std::fprintf(stderr, "[title] entering guest entry point 0x%llx\n", static_cast<unsigned long long>(runtime.EntryAddress()));
        const auto result = runtime.CallGuest(runtime.EntryAddress(), {reinterpret_cast<std::uint64_t>(stack.data()), 0});
        std::fprintf(stderr, "[title] guest entry returned %lld\n", static_cast<long long>(result));
        return static_cast<int>(result);
    } catch (const std::exception& error) {
        std::fflush(stdout);
        std::fprintf(stderr, "[title] failed: %s\n", error.what());
        std::fflush(stderr);
        std::_Exit(1);
    }
}

void* RunTitleThread(void* context) {
    const auto* arguments = static_cast<const TitleArguments*>(context);
    std::exit(RunTitle(arguments->argc, arguments->argv));
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <decrypted title directory> <system library directory> [arguments...]\n", argv[0]);
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    TitleArguments arguments{argc, argv};
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, GuestHostStackBytes);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, RunTitleThread, &arguments) != 0) {
        std::fprintf(stderr, "[title] failed: cannot start the title thread\n");
        return 1;
    }
    pthread_attr_destroy(&attributes);
#ifdef __APPLE__
    auto* keepAlive = CFRunLoopTimerCreate(nullptr, CFAbsoluteTimeGetCurrent() + 1e9, 1e9, 0, 0, [](CFRunLoopTimerRef, void*) {}, nullptr);
    CFRunLoopAddTimer(CFRunLoopGetMain(), keepAlive, kCFRunLoopCommonModes);
    for (;;) {
        CFRunLoopRun();
    }
#else
    pthread_join(thread, nullptr);
    return 0;
#endif
}
