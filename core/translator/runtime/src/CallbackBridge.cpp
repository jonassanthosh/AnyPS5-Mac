#include "translator/Runtime.hpp"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <dlfcn.h>
#include <execinfo.h>
#include <stdexcept>
#include <sys/ucontext.h>

extern "C" void aps5_guest_callback_bridge();

namespace translator {

namespace {

constexpr GuestRegister ArgumentRegisters[] = {Rdi, Rsi, Rdx, Rcx, R8, R9};

struct sigaction previousBus;
struct sigaction previousSegv;
struct sigaction previousAbort;

void Report(int signal, siginfo_t* info, void* raw) {
    auto& state = static_cast<ucontext_t*>(raw)->uc_mcontext->__ss;
    auto& runtime = Runtime::Instance();
    const auto pc = static_cast<std::uint64_t>(state.__pc);
    const auto lr = static_cast<std::uint64_t>(state.__lr);
    const auto fault = reinterpret_cast<std::uint64_t>(info->si_addr);
    std::fprintf(stderr, "[crash] signal %d: fault address 0x%llx %s, pc 0x%llx %s, lr 0x%llx %s\n", signal,
        static_cast<unsigned long long>(fault), runtime.DescribeAddress(fault).c_str(), static_cast<unsigned long long>(pc),
        runtime.DescribeAddress(pc).c_str(), static_cast<unsigned long long>(lr), runtime.DescribeAddress(lr).c_str());
    if (auto* c = runtime.ExistingContext()) {
        std::fprintf(stderr, "[crash] guest rip-relevant state: rsp 0x%llx rax 0x%llx rdi 0x%llx rsi 0x%llx last jump 0x%llx %s\n",
            static_cast<unsigned long long>(c->gpr[Rsp]), static_cast<unsigned long long>(c->gpr[Rax]),
            static_cast<unsigned long long>(c->gpr[Rdi]), static_cast<unsigned long long>(c->gpr[Rsi]),
            static_cast<unsigned long long>(c->jumpTarget), runtime.DescribeAddress(c->jumpTarget).c_str());
    }
    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, 2);
}

void Forward(int signal, siginfo_t* info, void* context) {
    const auto& previous = signal == SIGBUS ? previousBus : previousSegv;
    if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
        previous.sa_sigaction(signal, info, context);
        struct sigaction current{};
        if (sigaction(signal, nullptr, &current) == 0 && (current.sa_flags & SA_SIGINFO) == 0 && current.sa_handler == SIG_DFL) {
            Report(signal, info, context);
        }
        return;
    }
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN && previous.sa_handler != nullptr) {
        previous.sa_handler(signal);
        return;
    }
    Report(signal, info, context);
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

void AbortHandler(int signal, siginfo_t* info, void* raw) {
    Report(signal, info, raw);
    sigaction(SIGABRT, &previousAbort, nullptr);
    std::raise(signal);
}

void Handler(int signal, siginfo_t* info, void* raw) {
    auto* context = static_cast<ucontext_t*>(raw);
    auto& state = context->uc_mcontext->__ss;
    const auto pc = static_cast<std::uint64_t>(state.__pc);
    auto& runtime = Runtime::Instance();
    if (pc == reinterpret_cast<std::uint64_t>(info->si_addr) &&
        (runtime.IsTranslatedFunction(pc) || runtime.NativeImportAt(pc) != nullptr)) {
        state.__x[16] = pc;
        state.__pc = reinterpret_cast<std::uint64_t>(&aps5_guest_callback_bridge);
        return;
    }
    Forward(signal, info, raw);
}

}

void Runtime::InstallCallbackBridge() {
    struct sigaction action{};
    action.sa_sigaction = Handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGBUS, &action, &previousBus) != 0 || sigaction(SIGSEGV, &action, &previousSegv) != 0) {
        throw std::runtime_error("Cannot install the native-to-guest callback bridge");
    }
    struct sigaction abortAction{};
    abortAction.sa_sigaction = AbortHandler;
    abortAction.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&abortAction.sa_mask);
    sigaction(SIGABRT, &abortAction, &previousAbort);
}

}

namespace {

void AwaitProcessExit() {
    using Await = void (*)();
    static const auto await = reinterpret_cast<Await>(::dlsym(RTLD_DEFAULT, "LibcAwaitExit_nid_postfix"));
    if (await == nullptr) return;
    try {
        await();
    } catch (...) {
    }
}

}

extern "C" void* Aps5GuestCallbackFromNative(std::uint64_t target, const std::uint64_t* integers, const std::uint8_t* vectors,
    std::uint8_t* result) {
    using namespace translator;
    auto& runtime = Runtime::Instance();
    if (void* native = runtime.NativeImportAt(target)) {
        return native;
    }
    try {
        auto* c = runtime.CurrentContext();
        const auto saved = std::make_unique<GuestContext>(*c);
        for (int i = 0; i < 6; ++i) {
            c->gpr[ArgumentRegisters[i]] = integers[i];
        }
        for (int i = 0; i < 8; ++i) {
            std::memcpy(c->ymm[i], vectors + i * 16, 16);
        }
        c->gpr[Rax] = 0;
        const auto value = runtime.CallGuestWithRegisters(target);
        std::memcpy(result, &value, sizeof(value));
        std::memcpy(result + 16, c->ymm[0], 16);
        *c = *saved;
    } catch (const std::exception& error) {
        AwaitProcessExit();
        std::fprintf(stderr, "native-to-guest call of 0x%llx failed: %s\n", static_cast<unsigned long long>(target), error.what());
        std::abort();
    }
    return nullptr;
}
