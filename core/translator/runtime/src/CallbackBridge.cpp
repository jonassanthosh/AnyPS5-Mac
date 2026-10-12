#include "translator/Runtime.hpp"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <sys/ucontext.h>

extern "C" void aps5_guest_callback_bridge();

namespace translator {

namespace {

constexpr GuestRegister ArgumentRegisters[] = {Rdi, Rsi, Rdx, Rcx, R8, R9};

struct sigaction previousBus;
struct sigaction previousSegv;

void Forward(int signal, siginfo_t* info, void* context) {
    const auto& previous = signal == SIGBUS ? previousBus : previousSegv;
    if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
        previous.sa_sigaction(signal, info, context);
        return;
    }
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN && previous.sa_handler != nullptr) {
        previous.sa_handler(signal);
        return;
    }
    std::signal(signal, SIG_DFL);
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
    } catch (const std::exception& error) {
        std::fprintf(stderr, "native-to-guest call of 0x%llx failed: %s\n", static_cast<unsigned long long>(target), error.what());
        std::abort();
    }
    return nullptr;
}
