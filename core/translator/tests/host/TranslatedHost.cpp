#include "translator/GuestJump.hpp"
#include "translator/Runtime.hpp"
#include "translator/Semantics.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <pthread.h>
#include <string>

extern "C" const translator::TranslatedImage aps5TranslatedImage;

namespace {

using translator::GuestContext;
using translator::Runtime;

extern "C" void HostPrintI64(const char* label, long value) {
    std::printf("%s=%ld\n", label, value);
}

extern "C" void HostPrintF64(const char* label, double value) {
    std::printf("%s=%.9g\n", label, value);
}

extern "C" void HostPrintF32(const char* label, float value) {
    std::printf("%s=%.7g\n", label, static_cast<double>(value));
}

extern "C" double HostMix(int a, double b, float c, long d, double e) {
    return a * 1000.0 + b * 100.0 + c * 10.0 + d + e / 10.0;
}

extern "C" long HostClockNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000000000L + now.tv_nsec;
}

class SysVArguments {
public:
    explicit SysVArguments(GuestContext* c) : _c(c), _stack(c->gpr[translator::Rsp] + 8) {}

    std::uint64_t NextInteger() {
        static constexpr translator::GuestRegister registers[] = {
            translator::Rdi, translator::Rsi, translator::Rdx, translator::Rcx, translator::R8, translator::R9};
        if (_integer < 6) {
            return _c->gpr[registers[_integer++]];
        }
        return NextStack();
    }

    double NextDouble() {
        if (_vector < 8) {
            return translator::ReadVec<double>(_c, _vector++);
        }
        auto bits = NextStack();
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

private:
    std::uint64_t NextStack() {
        auto value = translator::Ld<std::uint64_t>(_stack);
        _stack += 8;
        return value;
    }

    GuestContext* _c;
    std::uint64_t _stack;
    unsigned _integer = 0;
    unsigned _vector = 0;
};

void HostPrintfImport(GuestContext* c) {
    SysVArguments arguments(c);
    const char* format = reinterpret_cast<const char*>(arguments.NextInteger());
    std::string output;
    for (const char* p = format; *p != '\0'; ++p) {
        if (*p != '%') {
            output += *p;
            continue;
        }
        const char* start = p++;
        while (*p != '\0' && std::strchr("-+ #0123456789.", *p) != nullptr) ++p;
        while (*p == 'l' || *p == 'h' || *p == 'z') ++p;
        std::string spec(start, p + 1);
        char buffer[256];
        switch (*p) {
        case '%':
            output += '%';
            continue;
        case 'f': case 'g': case 'e': case 'F': case 'G': case 'E':
            std::snprintf(buffer, sizeof(buffer), spec.c_str(), arguments.NextDouble());
            break;
        case 's':
            std::snprintf(buffer, sizeof(buffer), spec.c_str(), reinterpret_cast<const char*>(arguments.NextInteger()));
            break;
        case 'c':
            std::snprintf(buffer, sizeof(buffer), spec.c_str(), static_cast<int>(arguments.NextInteger()));
            break;
        default: {
            std::string widened = spec;
            auto conversion = widened.back();
            widened.erase(std::remove_if(widened.begin(), widened.end(), [](char ch) { return ch == 'l' || ch == 'h' || ch == 'z'; }),
                widened.end());
            widened.insert(widened.size() - 1, "ll");
            auto value = arguments.NextInteger();
            bool narrow = spec.find('l') == std::string::npos && spec.find('z') == std::string::npos;
            if (narrow) {
                value = (conversion == 'd' || conversion == 'i') ? static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)))
                                                                 : static_cast<std::uint32_t>(value);
            }
            std::snprintf(buffer, sizeof(buffer), widened.c_str(), value);
            break;
        }
        }
        output += buffer;
    }
    std::fputs(output.c_str(), stdout);
    c->gpr[translator::Rax] = output.size();
}

extern "C" long HostSum8(long a, long b, long c, long d, long e, long f, long g, long h) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
}

extern "C" void HostQsort(void* base, unsigned long count, unsigned long size, int (*compare)(const void*, const void*)) {
    std::qsort(base, count, size, compare);
}

extern "C" long HostThreadCreate(void* (*body)(void*), void* argument) {
    pthread_t thread;
    if (pthread_create(&thread, nullptr, body, argument) != 0) {
        std::abort();
    }
    return reinterpret_cast<long>(thread);
}

extern "C" void HostThreadJoin(long handle) {
    pthread_join(reinterpret_cast<pthread_t>(handle), nullptr);
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <guest.elf>\n", argv[0]);
        return 2;
    }
    try {
        auto& runtime = Runtime::Instance();
        runtime.RegisterImport("host_print_i64", reinterpret_cast<void*>(HostPrintI64));
        runtime.RegisterImport("host_print_f64", reinterpret_cast<void*>(HostPrintF64));
        runtime.RegisterImport("host_print_f32", reinterpret_cast<void*>(HostPrintF32));
        runtime.RegisterImport("host_mix", reinterpret_cast<void*>(HostMix));
        runtime.RegisterImport("host_clock_ns", reinterpret_cast<void*>(HostClockNs));
        runtime.RegisterImport("host_qsort", reinterpret_cast<void*>(HostQsort));
        runtime.RegisterImport("host_thread_create", reinterpret_cast<void*>(HostThreadCreate));
        runtime.RegisterImport("host_thread_join", reinterpret_cast<void*>(HostThreadJoin));
        runtime.RegisterImport("memcpy", reinterpret_cast<void*>(static_cast<void* (*)(void*, const void*, size_t)>(std::memcpy)));
        runtime.RegisterImport("memset", reinterpret_cast<void*>(static_cast<void* (*)(void*, int, size_t)>(std::memset)));
        runtime.RegisterImport("fmaf", reinterpret_cast<void*>(static_cast<float (*)(float, float, float)>(std::fmaf)));
        runtime.RegisterContextImport("host_printf", HostPrintfImport);
        for (const auto& [name, handler] : translator::RuntimeContextImports()) {
            runtime.RegisterContextImport(name, handler);
        }
        runtime.RegisterImport("host_sum8", reinterpret_cast<void*>(HostSum8));
        runtime.LoadImage(argv[1], aps5TranslatedImage, true);
        runtime.InstallCallbackBridge();
        auto result = runtime.CallGuest(runtime.SymbolAddress("test_main"), {});
        std::printf("result=%ld\n", static_cast<long>(result));
        if (std::getenv("APS5_TRANSLATOR_STATS") != nullptr) {
            std::fprintf(stderr, "dispatches=%zu imports=%zu\n", runtime.DispatchCount(), runtime.ImportCallCount());
        }
    } catch (const std::exception& error) {
        std::fflush(stdout);
        std::fprintf(stderr, "translated run failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
