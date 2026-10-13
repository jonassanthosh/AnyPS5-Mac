#include "translator/GuestJump.hpp"
#include "translator/Semantics.hpp"
#include <cstring>

namespace translator {

namespace {

constexpr std::uint64_t ReturnSlot = 0;
constexpr std::uint64_t RbxSlot = 8;
constexpr std::uint64_t RspSlot = 16;
constexpr std::uint64_t RbpSlot = 24;
constexpr std::uint64_t R12Slot = 32;
constexpr std::uint64_t MxcsrSlot = 64;
constexpr std::uint64_t ControlWordSlot = 68;
constexpr std::uint16_t DefaultControlWord = 0x37f;

struct RegisterFile {
    u64 gpr[16];
    u64 rip;
    u64 rflags;
    u64 fsBase;
    u32 mxcsr;
    u8 xmm[16][16];
};

}

void GuestSetjmp(GuestContext* c) {
    const auto buffer = c->gpr[Rdi];
    St<u64>(buffer + ReturnSlot, LdStack<u64>(c->gpr[Rsp]));
    St<u64>(buffer + RbxSlot, c->gpr[Rbx]);
    St<u64>(buffer + RspSlot, c->gpr[Rsp] + 8);
    St<u64>(buffer + RbpSlot, c->gpr[Rbp]);
    for (int i = 0; i < 4; ++i) {
        St<u64>(buffer + R12Slot + i * 8, c->gpr[R12 + i]);
    }
    St<u32>(buffer + MxcsrSlot, c->mxcsr);
    St<u16>(buffer + ControlWordSlot, DefaultControlWord);
    c->gpr[Rax] = 0;
}

void GuestLongjmp(GuestContext* c) {
    const auto buffer = c->gpr[Rdi];
    const auto value = static_cast<u32>(c->gpr[Rsi]);
    c->gpr[Rbx] = Ld<u64>(buffer + RbxSlot);
    c->gpr[Rbp] = Ld<u64>(buffer + RbpSlot);
    for (int i = 0; i < 4; ++i) {
        c->gpr[R12 + i] = Ld<u64>(buffer + R12Slot + i * 8);
    }
    c->mxcsr = Ld<u32>(buffer + MxcsrSlot);
    const auto resumeRsp = Ld<u64>(buffer + RspSlot) - 8;
    StStack<u64>(resumeRsp, Ld<u64>(buffer + ReturnSlot));
    c->gpr[Rsp] = resumeRsp;
    c->gpr[Rax] = value != 0 ? value : 1;
}

bool ProvideGuestVarArgs(void* list, void* saveArea, unsigned fixedIntegers, std::uint64_t last) {
    static constexpr GuestRegister integerRegisters[] = {Rdi, Rsi, Rdx, Rcx, R8, R9};
    auto* c = Runtime::Instance().ExistingContext();
    if (c == nullptr || fixedIntegers == 0 || fixedIntegers > 6) {
        return false;
    }
    const auto lastRegister = c->gpr[integerRegisters[fixedIntegers - 1]];
    if (lastRegister != last && static_cast<u32>(lastRegister) != static_cast<u32>(last)) {
        return false;
    }
    auto* save = static_cast<std::uint8_t*>(saveArea);
    for (int i = 0; i < 6; ++i) {
        std::memcpy(save + i * 8, &c->gpr[integerRegisters[i]], 8);
    }
    for (int i = 0; i < 8; ++i) {
        std::memcpy(save + 48 + i * 16, c->ymm[i], 16);
    }
    struct SysvVaList {
        u32 gpOffset;
        u32 fpOffset;
        u64 overflowArea;
        u64 registerSaveArea;
    } value{fixedIntegers * 8, 48, c->gpr[Rsp] + 8, reinterpret_cast<u64>(saveArea)};
    static_assert(sizeof(SysvVaList) == 24);
    std::memcpy(list, &value, sizeof(value));
    return true;
}

void GuestTlsGetAddress(GuestContext* c) {
    const auto descriptor = c->gpr[Rdi];
    const auto module = Ld<u64>(descriptor);
    const auto offset = Ld<u64>(descriptor + 8);
    c->gpr[Rax] = Runtime::Instance().TlsAddress(c, module, offset);
}

bool ProvideGuestThreadStack(std::size_t desiredBytes, std::uintptr_t* low, std::uintptr_t* high) {
    auto* c = Runtime::Instance().PrepareThread(desiredBytes);
    *low = static_cast<std::uintptr_t>(c->stackBase);
    *high = static_cast<std::uintptr_t>(c->stackBase + c->stackSize);
    return true;
}

bool ProvideGuestRegisters(void* registers) {
    auto* c = Runtime::Instance().ExistingContext();
    if (c == nullptr) {
        return false;
    }
    RegisterFile file{};
    std::memcpy(file.gpr, c->gpr, sizeof(file.gpr));
    file.rip = c->returnTarget;
    file.rflags = 0x202 | (u64(c->cf) << 0) | (u64(c->pf) << 2) | (u64(c->af) << 4) | (u64(c->zf) << 6) | (u64(c->sf) << 7) |
        (u64(c->df) << 10) | (u64(c->of) << 11);
    file.fsBase = c->fsBase;
    file.mxcsr = c->mxcsr;
    for (int i = 0; i < 16; ++i) {
        std::memcpy(file.xmm[i], c->ymm[i], 16);
    }
    std::memcpy(registers, &file, sizeof(file));
    return true;
}

void ProvideGuestInstall(const void* registers) {
    RegisterFile file;
    std::memcpy(&file, registers, sizeof(file));
    GuestResume resume{};
    std::memcpy(resume.gpr, file.gpr, sizeof(resume.gpr));
    resume.rip = file.rip;
    resume.mxcsr = file.mxcsr;
    throw resume;
}

void ApplyGuestResume(GuestContext* c, const GuestResume& resume) {
    std::memcpy(c->gpr, resume.gpr, sizeof(c->gpr));
    c->mxcsr = resume.mxcsr;
    c->gpr[Rsp] -= 8;
    StStack<u64>(c->gpr[Rsp], resume.rip);
}

const std::vector<std::pair<std::string, ContextImport>>& RuntimeContextImports() {
    static const std::vector<std::pair<std::string, ContextImport>> imports = {
        {"setjmp", GuestSetjmp},
        {"_setjmp", GuestSetjmp},
        {"longjmp", GuestLongjmp},
        {"_longjmp", GuestLongjmp},
        {"__tls_get_addr", GuestTlsGetAddress},
    };
    return imports;
}

}
