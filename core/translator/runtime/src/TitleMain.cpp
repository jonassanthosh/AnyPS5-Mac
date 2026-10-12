#include "translator/GuestJump.hpp"
#include "translator/Runtime.hpp"
#include <nid/NidCompute.hpp>
#include <map>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" const translator::TranslatedEntry aps5TranslatedFunctions[];
extern "C" const std::size_t aps5TranslatedFunctionCount;

namespace {

using GuestImageRegister = const void* (*)(const char*, std::uintptr_t, const void*, std::uint16_t, std::size_t, bool);

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
    void* address = dlsym(RTLD_DEFAULT, nid.c_str());
    if (address == nullptr) {
        std::fprintf(stderr, "[title] unresolved %s import %s\n", data ? "data" : "function", name.c_str());
        return resolution;
    }
    resolution.kind = data ? translator::ImportResolution::Kind::Data : translator::ImportResolution::Kind::Function;
    resolution.address = address;
    return resolution;
}

void RegisterMainImage(translator::Runtime& runtime, const std::string& name) {
    auto registerImage = reinterpret_cast<GuestImageRegister>(dlsym(RTLD_DEFAULT, "GuestImageRegister_nid_no_patch"));
    if (registerImage == nullptr) {
        throw std::runtime_error("libc.prx does not provide the guest image registry");
    }
    std::size_t count = 0;
    const void* headers = runtime.ImageProgramHeaders(count);
    registerImage(name.c_str(), static_cast<std::uintptr_t>(runtime.ImageBase()), headers, static_cast<std::uint16_t>(count), 1, true);
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

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <eboot.elf> <system library directory> [arguments...]\n", argv[0]);
        return 2;
    }
    try {
        auto& runtime = translator::Runtime::Instance();
        LoadLibraries(argv[2]);
        runtime.RegisterFunctions(aps5TranslatedFunctions, aps5TranslatedFunctionCount);
        runtime.SetImportResolver(Resolve);
        runtime.LoadImage(argv[1]);
        std::fprintf(stderr, "[title] mapped %s at 0x%llx (%llu bytes), %zu translated functions, %zu unresolved imports\n",
            argv[1], static_cast<unsigned long long>(runtime.ImageBase()), static_cast<unsigned long long>(runtime.ImageSize()),
            aps5TranslatedFunctionCount, runtime.UnresolvedImportCount());
        RegisterMainImage(runtime, std::filesystem::path(argv[1]).filename().string());
        runtime.InstallCallbackBridge();
        std::vector<std::string> arguments{std::filesystem::path(argv[1]).filename().string()};
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
        return 1;
    }
}
