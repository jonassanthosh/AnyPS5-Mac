#include "translator/GuestJump.hpp"
#include "translator/Semantics.hpp"

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

const std::vector<std::pair<std::string, ContextImport>>& RuntimeContextImports() {
    static const std::vector<std::pair<std::string, ContextImport>> imports = {
        {"setjmp", GuestSetjmp},
        {"_setjmp", GuestSetjmp},
        {"longjmp", GuestLongjmp},
        {"_longjmp", GuestLongjmp},
    };
    return imports;
}

}
