#include "Emitter.hpp"
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <set>
#include <stdexcept>

namespace translator {

namespace {

constexpr std::uint32_t TrackedFlags = (1u << 0) | (1u << 2) | (1u << 4) | (1u << 6) | (1u << 7) | (1u << 11);

std::string Hex(std::uint64_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << value << "ull";
    return stream.str();
}

std::string HexName(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << value;
    return stream.str();
}

std::string UnsignedType(unsigned bits) {
    switch (bits) {
    case 8: return "u8";
    case 16: return "u16";
    case 32: return "u32";
    case 64: return "u64";
    case 128: return "u128";
    default: throw std::runtime_error("No unsigned type of width " + std::to_string(bits));
    }
}

std::string SignedType(unsigned bits) {
    switch (bits) {
    case 8: return "i8";
    case 16: return "i16";
    case 32: return "i32";
    case 64: return "i64";
    case 128: return "i128";
    default: throw std::runtime_error("No signed type of width " + std::to_string(bits));
    }
}

std::string MaskLiteral(std::uint32_t mask) {
    std::ostringstream stream;
    stream << "0x" << std::hex << mask << "u";
    return stream.str();
}

std::string Mnemonic(const ZydisDecodedInstruction& instruction) {
    return ZydisMnemonicGetString(instruction.mnemonic);
}

std::uint32_t FlagsRead(const ZydisDecodedInstruction& instruction) {
    return instruction.cpu_flags != nullptr ? instruction.cpu_flags->tested & TrackedFlags : 0;
}

std::uint32_t FlagsWritten(const ZydisDecodedInstruction& instruction) {
    if (instruction.cpu_flags == nullptr) {
        return 0;
    }
    const auto* flags = instruction.cpu_flags;
    return (flags->modified | flags->set_0 | flags->set_1 | flags->undefined) & TrackedFlags;
}

std::string Condition(const std::string& suffix) {
    static const std::map<std::string, std::string> conditions = {
        {"o", "c->of"}, {"no", "!c->of"}, {"b", "c->cf"}, {"nb", "!c->cf"}, {"z", "c->zf"}, {"nz", "!c->zf"},
        {"be", "(c->cf || c->zf)"}, {"nbe", "(!c->cf && !c->zf)"}, {"s", "c->sf"}, {"ns", "!c->sf"},
        {"p", "c->pf"}, {"np", "!c->pf"}, {"l", "(c->sf != c->of)"}, {"nl", "(c->sf == c->of)"},
        {"le", "(c->zf || c->sf != c->of)"}, {"nle", "(!c->zf && c->sf == c->of)"}};
    auto it = conditions.find(suffix);
    if (it == conditions.end()) {
        throw std::runtime_error("Unknown condition suffix " + suffix);
    }
    return it->second;
}

bool IsHighByte(ZydisRegister reg) {
    return reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH || reg == ZYDIS_REGISTER_BH;
}

bool IsGpr(ZydisRegister reg) {
    auto registerClass = ZydisRegisterGetClass(reg);
    return registerClass == ZYDIS_REGCLASS_GPR8 || registerClass == ZYDIS_REGCLASS_GPR16 ||
        registerClass == ZYDIS_REGCLASS_GPR32 || registerClass == ZYDIS_REGCLASS_GPR64;
}

bool IsVectorRegister(ZydisRegister reg) {
    auto registerClass = ZydisRegisterGetClass(reg);
    return registerClass == ZYDIS_REGCLASS_XMM || registerClass == ZYDIS_REGCLASS_YMM;
}

int GprIndex(ZydisRegister reg) {
    if (IsHighByte(reg)) {
        return ZydisRegisterGetId(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg));
    }
    return ZydisRegisterGetId(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg));
}

int VectorIndex(ZydisRegister reg) {
    return ZydisRegisterGetId(reg);
}

bool IsVex(const ZydisDecodedInstruction& instruction) {
    return instruction.encoding == ZYDIS_INSTRUCTION_ENCODING_VEX ||
        instruction.encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX;
}

bool HasLock(const ZydisDecodedInstruction& instruction) {
    return (instruction.attributes & ZYDIS_ATTRIB_HAS_LOCK) != 0;
}

bool HasRep(const ZydisDecodedInstruction& instruction) {
    return (instruction.attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE)) != 0;
}

struct VectorOp {
    std::string element;
    std::string op;
};

const std::map<ZydisMnemonic, VectorOp>& PackedBinaryOps() {
    static const std::map<ZydisMnemonic, VectorOp> ops = {
        {ZYDIS_MNEMONIC_ADDPS, {"f32", "+"}}, {ZYDIS_MNEMONIC_ADDPD, {"f64", "+"}},
        {ZYDIS_MNEMONIC_SUBPS, {"f32", "-"}}, {ZYDIS_MNEMONIC_SUBPD, {"f64", "-"}},
        {ZYDIS_MNEMONIC_MULPS, {"f32", "*"}}, {ZYDIS_MNEMONIC_MULPD, {"f64", "*"}},
        {ZYDIS_MNEMONIC_DIVPS, {"f32", "/"}}, {ZYDIS_MNEMONIC_DIVPD, {"f64", "/"}},
        {ZYDIS_MNEMONIC_ANDPS, {"u32", "&"}}, {ZYDIS_MNEMONIC_ANDPD, {"u64", "&"}}, {ZYDIS_MNEMONIC_PAND, {"u64", "&"}},
        {ZYDIS_MNEMONIC_ORPS, {"u32", "|"}}, {ZYDIS_MNEMONIC_ORPD, {"u64", "|"}}, {ZYDIS_MNEMONIC_POR, {"u64", "|"}},
        {ZYDIS_MNEMONIC_XORPS, {"u32", "^"}}, {ZYDIS_MNEMONIC_XORPD, {"u64", "^"}}, {ZYDIS_MNEMONIC_PXOR, {"u64", "^"}},
        {ZYDIS_MNEMONIC_ANDNPS, {"u32", "andn"}}, {ZYDIS_MNEMONIC_ANDNPD, {"u64", "andn"}},
        {ZYDIS_MNEMONIC_PANDN, {"u64", "andn"}},
        {ZYDIS_MNEMONIC_PADDB, {"u8", "+"}}, {ZYDIS_MNEMONIC_PADDW, {"u16", "+"}},
        {ZYDIS_MNEMONIC_PADDD, {"u32", "+"}}, {ZYDIS_MNEMONIC_PADDQ, {"u64", "+"}},
        {ZYDIS_MNEMONIC_PSUBB, {"u8", "-"}}, {ZYDIS_MNEMONIC_PSUBW, {"u16", "-"}},
        {ZYDIS_MNEMONIC_PSUBD, {"u32", "-"}}, {ZYDIS_MNEMONIC_PSUBQ, {"u64", "-"}},
        {ZYDIS_MNEMONIC_PMULLW, {"u16", "*"}}, {ZYDIS_MNEMONIC_PMULLD, {"u32", "*"}},
        {ZYDIS_MNEMONIC_PCMPEQB, {"i8", "=="}}, {ZYDIS_MNEMONIC_PCMPEQW, {"i16", "=="}},
        {ZYDIS_MNEMONIC_PCMPEQD, {"i32", "=="}}, {ZYDIS_MNEMONIC_PCMPEQQ, {"i64", "=="}},
        {ZYDIS_MNEMONIC_PCMPGTB, {"i8", ">"}}, {ZYDIS_MNEMONIC_PCMPGTW, {"i16", ">"}},
        {ZYDIS_MNEMONIC_PCMPGTD, {"i32", ">"}}, {ZYDIS_MNEMONIC_PCMPGTQ, {"i64", ">"}},
        {ZYDIS_MNEMONIC_PMINSD, {"i32", "min"}}, {ZYDIS_MNEMONIC_PMAXSD, {"i32", "max"}},
        {ZYDIS_MNEMONIC_PMINUD, {"u32", "min"}}, {ZYDIS_MNEMONIC_PMAXUD, {"u32", "max"}},
        {ZYDIS_MNEMONIC_PMINUB, {"u8", "min"}}, {ZYDIS_MNEMONIC_PMAXUB, {"u8", "max"}},
        {ZYDIS_MNEMONIC_PMINSW, {"i16", "min"}}, {ZYDIS_MNEMONIC_PMAXSW, {"i16", "max"}},
        {ZYDIS_MNEMONIC_MINPS, {"f32", "ssemin"}}, {ZYDIS_MNEMONIC_MAXPS, {"f32", "ssemax"}},
        {ZYDIS_MNEMONIC_MINPD, {"f64", "ssemin"}}, {ZYDIS_MNEMONIC_MAXPD, {"f64", "ssemax"}},
    };
    return ops;
}

const std::map<ZydisMnemonic, VectorOp>& ScalarBinaryOps() {
    static const std::map<ZydisMnemonic, VectorOp> ops = {
        {ZYDIS_MNEMONIC_ADDSS, {"f32", "+"}}, {ZYDIS_MNEMONIC_ADDSD, {"f64", "+"}},
        {ZYDIS_MNEMONIC_SUBSS, {"f32", "-"}}, {ZYDIS_MNEMONIC_SUBSD, {"f64", "-"}},
        {ZYDIS_MNEMONIC_MULSS, {"f32", "*"}}, {ZYDIS_MNEMONIC_MULSD, {"f64", "*"}},
        {ZYDIS_MNEMONIC_DIVSS, {"f32", "/"}}, {ZYDIS_MNEMONIC_DIVSD, {"f64", "/"}},
        {ZYDIS_MNEMONIC_MINSS, {"f32", "ssemin"}}, {ZYDIS_MNEMONIC_MINSD, {"f64", "ssemin"}},
        {ZYDIS_MNEMONIC_MAXSS, {"f32", "ssemax"}}, {ZYDIS_MNEMONIC_MAXSD, {"f64", "ssemax"}},
        {ZYDIS_MNEMONIC_SQRTSS, {"f32", "sqrt"}}, {ZYDIS_MNEMONIC_SQRTSD, {"f64", "sqrt"}},
    };
    return ops;
}

unsigned ElementBits(const std::string& element) {
    return static_cast<unsigned>(std::stoi(element.substr(1)));
}

std::string ElementCType(const std::string& element) {
    if (element == "f32") return "float";
    if (element == "f64") return "double";
    return element;
}

std::string VectorType(const std::string& element, unsigned bits) {
    return "Vec<" + ElementCType(element) + ", " + std::to_string(bits / 8) + ">";
}

std::string BinaryExpression(const std::string& op, const std::string& a, const std::string& b) {
    if (op == "andn") return "(~(" + a + ") & (" + b + "))";
    if (op == "min") return "((" + a + ") < (" + b + ") ? (" + a + ") : (" + b + "))";
    if (op == "max") return "((" + a + ") > (" + b + ") ? (" + a + ") : (" + b + "))";
    if (op == "ssemin") return "((" + a + ") < (" + b + ") ? (" + a + ") : (" + b + "))";
    if (op == "ssemax") return "((" + a + ") > (" + b + ") ? (" + a + ") : (" + b + "))";
    if (op == "sqrt") return "std::sqrt(" + b + ")";
    return "((" + a + ") " + op + " (" + b + "))";
}

}

Emitter::Emitter(const ElfImage& image, const Discovery& discovery, const Program& program, EmitOptions options)
    : _image(image), _discovery(discovery), _program(program), _options(options) {}

std::string Emitter::Load(const ZydisDecodedOperand& operand, const std::string& type) const {
    return (operand.mem.base == ZYDIS_REGISTER_RSP ? "LdStack<" : "Ld<") + type + ">";
}

std::string Emitter::Store(const ZydisDecodedOperand& operand, const std::string& type) const {
    return (operand.mem.base == ZYDIS_REGISTER_RSP ? "StStack<" : "St<") + type + ">";
}

std::string Emitter::FunctionName(std::uint64_t entry) const {
    return "F_" + HexName(entry);
}

std::string Emitter::Absolute(std::uint64_t vaddr) const {
    return "(c->imageBase + " + Hex(vaddr) + ")";
}

std::string Emitter::Address(const DecodedInstruction& decoded, const ZydisDecodedOperand& operand) const {
    const auto& mem = operand.mem;
    std::string expression;
    if (mem.base == ZYDIS_REGISTER_RIP) {
        expression = Absolute(decoded.Next() + static_cast<std::uint64_t>(mem.disp.value));
    } else {
        std::vector<std::string> parts;
        if (mem.base != ZYDIS_REGISTER_NONE) {
            parts.push_back("c->gpr[" + std::to_string(GprIndex(mem.base)) + "]");
        }
        if (mem.index != ZYDIS_REGISTER_NONE) {
            std::string index = "c->gpr[" + std::to_string(GprIndex(mem.index)) + "]";
            parts.push_back(mem.scale > 1 ? index + " * " + std::to_string(mem.scale) : index);
        }
        if (mem.disp.value != 0 || parts.empty()) {
            parts.push_back(Hex(static_cast<std::uint64_t>(mem.disp.value)));
        }
        for (std::size_t i = 0; i < parts.size(); ++i) {
            expression += (i == 0 ? "" : " + ") + parts[i];
        }
        expression = "(u64)(" + expression + ")";
    }
    if (decoded.instruction.address_width == 32) {
        expression = "(u64)(u32)(" + expression + ")";
    }
    if (mem.segment == ZYDIS_REGISTER_FS) {
        expression = "(c->fsBase + " + expression + ")";
    } else if (mem.segment == ZYDIS_REGISTER_GS) {
        expression = "GsBase(g, " + expression + ")";
    }
    return expression;
}

std::string Emitter::Read(const DecodedInstruction& decoded, int index) const {
    return Read(decoded, index, decoded.operands[index].size);
}

std::string Emitter::Read(const DecodedInstruction& decoded, int index, unsigned bits) const {
    const auto& operand = decoded.operands[index];
    auto type = UnsignedType(bits);
    switch (operand.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER: {
        auto reg = operand.reg.value;
        if (IsVectorRegister(reg)) {
            return "ReadVec<" + type + ">(c, " + std::to_string(VectorIndex(reg)) + ")";
        }
        if (!IsGpr(reg)) {
            throw std::runtime_error("Unsupported register operand " + std::string(ZydisRegisterGetString(reg)));
        }
        auto gpr = "c->gpr[" + std::to_string(GprIndex(reg)) + "]";
        if (IsHighByte(reg)) {
            return "(u8)(" + gpr + " >> 8)";
        }
        return bits == 64 ? gpr : "(" + type + ")" + gpr;
    }
    case ZYDIS_OPERAND_TYPE_MEMORY:
        return Load(operand, type) + "(" + Address(decoded, operand) + ")";
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        return "(" + type + ")" + Hex(operand.imm.value.u);
    default:
        throw std::runtime_error("Unsupported operand type");
    }
}

std::string Emitter::Write(const DecodedInstruction& decoded, int index, const std::string& value) const {
    const auto& operand = decoded.operands[index];
    unsigned bits = operand.size;
    auto type = UnsignedType(bits);
    if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        return Store(operand, type) + "(" + Address(decoded, operand) + ", (" + type + ")(" + value + "));";
    }
    auto reg = operand.reg.value;
    if (IsVectorRegister(reg)) {
        return "WriteVecLow<" + type + ">(c, " + std::to_string(VectorIndex(reg)) + ", (" + type + ")(" + value + "));";
    }
    auto gpr = "c->gpr[" + std::to_string(GprIndex(reg)) + "]";
    if (IsHighByte(reg)) {
        return gpr + " = (" + gpr + " & ~0xff00ull) | ((u64)(u8)(" + value + ") << 8);";
    }
    switch (bits) {
    case 64: return gpr + " = (u64)(" + value + ");";
    case 32: return gpr + " = (u64)(u32)(" + value + ");";
    case 16: return gpr + " = (" + gpr + " & ~0xffffull) | (u16)(" + value + ");";
    case 8: return gpr + " = (" + gpr + " & ~0xffull) | (u8)(" + value + ");";
    default: throw std::runtime_error("Unsupported register write width");
    }
}

std::string Emitter::ReadVector(const DecodedInstruction& decoded, int index, const std::string& type) const {
    const auto& operand = decoded.operands[index];
    if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        return Load(operand, type) + "(" + Address(decoded, operand) + ")";
    }
    return "ReadVec<" + type + ">(c, " + std::to_string(VectorIndex(operand.reg.value)) + ")";
}

std::string Emitter::CallTo(const std::string& target, std::uint64_t returnAddress) const {
    return "c->gpr[Rsp] -= 8; StStack<u64>(c->gpr[Rsp], " + Absolute(returnAddress) + "); out(); " + target +
        " in(); if (g->returnTarget != " + Absolute(returnAddress) + ") [[unlikely]] { ReturnMismatch(g, " +
        Absolute(returnAddress) + "); }";
}

std::map<std::uint64_t, std::vector<Emitter::FlagMask>> Emitter::ComputeLiveFlags(
    const TranslatedFunctionInfo& function) const {
    std::map<std::uint64_t, FlagMask> liveIn;
    for (const auto& [start, block] : function.blocks) {
        liveIn[start] = 0;
    }
    auto exitLive = _options.conservativeFlags ? TrackedFlags : 0u;
    auto blockLiveOut = [&](const BasicBlock& block) {
        FlagMask live = 0;
        const auto& last = block.instructions.back().instruction;
        bool leavesFunction = last.mnemonic == ZYDIS_MNEMONIC_RET ||
            (last.mnemonic == ZYDIS_MNEMONIC_JMP && block.successors.empty()) ||
            (last.mnemonic == ZYDIS_MNEMONIC_JMP && !block.jumpTableRecovered &&
                last.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE &&
                block.instructions.back().operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE);
        if (leavesFunction) {
            live |= exitLive;
        }
        for (auto successor : block.successors) {
            auto it = liveIn.find(successor);
            live |= it != liveIn.end() ? it->second : TrackedFlags;
        }
        return live;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = function.blocks.rbegin(); it != function.blocks.rend(); ++it) {
            const auto& block = it->second;
            FlagMask live = blockLiveOut(block);
            for (auto instruction = block.instructions.rbegin(); instruction != block.instructions.rend(); ++instruction) {
                const auto& decoded = instruction->instruction;
                if (decoded.mnemonic == ZYDIS_MNEMONIC_CALL) {
                    live = exitLive;
                    continue;
                }
                live = (live & ~FlagsWritten(decoded)) | FlagsRead(decoded);
            }
            if (live != liveIn[block.start]) {
                liveIn[block.start] = live;
                changed = true;
            }
        }
    }
    std::map<std::uint64_t, std::vector<FlagMask>> liveAfter;
    for (const auto& [start, block] : function.blocks) {
        std::vector<FlagMask> masks(block.instructions.size());
        FlagMask live = blockLiveOut(block);
        for (std::size_t i = block.instructions.size(); i-- > 0;) {
            masks[i] = live;
            const auto& decoded = block.instructions[i].instruction;
            if (decoded.mnemonic == ZYDIS_MNEMONIC_CALL) {
                live = exitLive;
                continue;
            }
            live = (live & ~FlagsWritten(decoded)) | FlagsRead(decoded);
        }
        liveAfter[start] = std::move(masks);
    }
    return liveAfter;
}

void Emitter::Emit(const std::vector<std::ostream*>& shards) {
    std::uint64_t executableStart = UINT64_MAX;
    std::uint64_t executableEnd = 0;
    for (const auto& segment : _image.Segments()) {
        if ((segment.flags & 1) != 0) {
            executableStart = std::min(executableStart, segment.vaddr);
            executableEnd = std::max(executableEnd, segment.vaddr + segment.fileSize);
        }
    }
    std::vector<bool> covered(executableEnd > executableStart ? executableEnd - executableStart : 0);
    for (auto* out : shards) {
        *out << "#include \"translator/Semantics.hpp\"\n#include \"translator/Runtime.hpp\"\n\n"
             << "using namespace translator;\n\n";
        for (const auto& [entry, function] : _program.functions) {
            *out << "void " << FunctionName(entry) << "(GuestContext* __restrict g);\n";
        }
        *out << "\n";
    }
    std::size_t index = 0;
    for (const auto& [entry, function] : _program.functions) {
        auto materialized = _discovery.Materialize(function);
        for (const auto& [start, block] : materialized.blocks) {
            for (const auto& decoded : block.instructions) {
                for (std::uint64_t byte = decoded.address; byte < decoded.Next() && byte < executableEnd; ++byte) {
                    if (byte >= executableStart) covered[byte - executableStart] = true;
                }
            }
        }
        EmitFunction(*shards[index++ % shards.size()], materialized);
    }
    _statistics.executableBytes = covered.size();
    _statistics.coveredBytes = static_cast<std::size_t>(std::count(covered.begin(), covered.end(), true));
    auto& first = *shards.front();
    first << "extern \"C\" const TranslatedEntry aps5TranslatedFunctions[] = {\n";
    for (const auto& [entry, function] : _program.functions) {
        first << "    {" << Hex(entry) << ", " << FunctionName(entry) << "},\n";
    }
    first << "};\nextern \"C\" const std::size_t aps5TranslatedFunctionCount = " << _program.functions.size() << ";\n";
}

void Emitter::EmitFunction(std::ostream& out, const TranslatedFunctionInfo& function) {
    auto liveAfter = ComputeLiveFlags(function);
    std::uint32_t usedGpr = 1u << 4;
    std::uint32_t writtenGpr = 1u << 4;
    std::uint32_t usedVector = 0;
    std::uint32_t writtenVector = 0;
    for (const auto& [start, block] : function.blocks) {
        for (const auto& decoded : block.instructions) {
            for (int i = 0; i < decoded.instruction.operand_count; ++i) {
                const auto& operand = decoded.operands[i];
                bool writes = (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0;
                if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
                    auto reg = operand.reg.value;
                    if (IsGpr(reg)) {
                        usedGpr |= 1u << GprIndex(reg);
                        writtenGpr |= writes ? 1u << GprIndex(reg) : 0;
                    } else if (IsVectorRegister(reg)) {
                        usedVector |= 1u << VectorIndex(reg);
                        writtenVector |= writes ? 1u << VectorIndex(reg) : 0;
                    }
                } else if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                    for (auto reg : {operand.mem.base, operand.mem.index}) {
                        if (reg != ZYDIS_REGISTER_NONE && IsGpr(reg)) {
                            usedGpr |= 1u << GprIndex(reg);
                        }
                    }
                }
            }
            if (decoded.instruction.mnemonic == ZYDIS_MNEMONIC_VZEROUPPER) {
                usedVector = writtenVector = 0xffff;
            }
        }
    }
    out << "void " << FunctionName(function.entry) << "(GuestContext* __restrict g) {\n";
    out << "    GuestContext local;\n    GuestContext* __restrict c = &local;\n";
    out << "    c->imageBase = g->imageBase; c->fsBase = g->fsBase;\n";
    out << "    auto in = [&]() __attribute__((always_inline)) {";
    for (int i = 0; i < 16; ++i) {
        if ((usedGpr >> i) & 1) out << " c->gpr[" << i << "] = g->gpr[" << i << "];";
    }
    for (int i = 0; i < 16; ++i) {
        if ((usedVector >> i) & 1) out << " std::memcpy(c->ymm[" << i << "], g->ymm[" << i << "], 32);";
    }
    out << " c->cf = g->cf; c->pf = g->pf; c->af = g->af; c->zf = g->zf; c->sf = g->sf; c->of = g->of; c->df = g->df; };\n";
    out << "    auto out = [&]() __attribute__((always_inline)) {";
    for (int i = 0; i < 16; ++i) {
        if ((writtenGpr >> i) & 1) out << " g->gpr[" << i << "] = c->gpr[" << i << "];";
    }
    for (int i = 0; i < 16; ++i) {
        if ((writtenVector >> i) & 1) out << " std::memcpy(g->ymm[" << i << "], c->ymm[" << i << "], 32);";
    }
    out << " g->cf = c->cf; g->pf = c->pf; g->af = c->af; g->zf = c->zf; g->sf = c->sf; g->of = c->of; g->df = c->df; };\n";
    out << "    in();\n";
    out << "    goto L_" << HexName(function.entry) << ";\n";
    for (const auto& [start, block] : function.blocks) {
        out << "L_" << HexName(start) << ":\n";
        const auto& masks = liveAfter[start];
        for (std::size_t i = 0; i < block.instructions.size(); ++i) {
            const auto& decoded = block.instructions[i];
            ++_statistics.instructions;
            std::ostringstream body;
            bool translated = false;
            try {
                translated = EmitInstruction(body, function, block, decoded, masks[i]);
            } catch (const std::exception&) {
                translated = false;
            }
            if (translated) {
                ++_statistics.translated;
                out << body.str();
            } else {
                ++_statistics.unsupported[Mnemonic(decoded.instruction)];
                out << "    out(); Unsupported(g, " << Hex(decoded.address) << ", \"" << Mnemonic(decoded.instruction) << "\");\n";
            }
        }
        const auto& last = block.instructions.back();
        if (!IsBlockTerminator(last.instruction)) {
            if (!block.successors.empty()) {
                out << "    goto L_" << HexName(block.successors.front()) << ";\n";
            } else {
                out << "    out(); FellOffCode(g, " << Hex(last.Next()) << ");\n";
            }
        }
    }
    out << "}\n\n";
}

bool Emitter::EmitInstruction(std::ostream& out, const TranslatedFunctionInfo& function, const BasicBlock& block,
    const DecodedInstruction& decoded, FlagMask liveAfter) {
    const auto& instruction = decoded.instruction;
    auto written = FlagsWritten(instruction);
    if (written != 0) {
        _statistics.flagWrites += __builtin_popcount(written);
        _statistics.flagWritesElided += __builtin_popcount(written & ~liveAfter);
    }
    if (EmitControl(out, function, block, decoded)) {
        return true;
    }
    if (EmitInteger(out, decoded, liveAfter & written)) {
        return true;
    }
    return EmitVector(out, decoded);
}

bool Emitter::EmitControl(std::ostream& out, const TranslatedFunctionInfo& function, const BasicBlock& block,
    const DecodedInstruction& decoded) {
    const auto& instruction = decoded.instruction;
    const auto& operand = decoded.operands[0];
    std::string mnemonic = Mnemonic(instruction);
    if (instruction.meta.category == ZYDIS_CATEGORY_COND_BR) {
        auto target = decoded.Next() + operand.imm.value.u;
        std::string condition;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_JRCXZ) {
            condition = "c->gpr[Rcx] == 0";
        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_JECXZ) {
            condition = "(u32)c->gpr[Rcx] == 0";
        } else {
            condition = Condition(mnemonic.substr(1));
        }
        out << "    if (" << condition << ") goto L_" << HexName(target) << ";\n";
        out << "    goto L_" << HexName(decoded.Next()) << ";\n";
        return true;
    }
    switch (instruction.mnemonic) {
    case ZYDIS_MNEMONIC_JMP: {
        if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            auto target = decoded.Next() + operand.imm.value.u;
            if (function.blocks.count(target) != 0 && !(target == function.entry && block.successors.empty())) {
                out << "    goto L_" << HexName(target) << ";\n";
            } else if (_program.functions.count(target) != 0) {
                out << "    out(); [[clang::musttail]] return " << FunctionName(target) << "(g);\n";
            } else {
                out << "    g->jumpTarget = " << Absolute(target) << "; out();\n    [[clang::musttail]] return JumpIndirect(g);\n";
            }
            return true;
        }
        ++_statistics.indirectJumps;
        if (!block.jumpTableRecovered) {
            if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                ++(operand.mem.base == ZYDIS_REGISTER_RIP ? _statistics.indirectThroughRipMemory
                                                           : _statistics.indirectThroughOtherMemory);
            } else {
                ++_statistics.indirectThroughRegister;
                bool tableShaped = false;
                for (const auto& previous : block.instructions) {
                    if (previous.instruction.mnemonic == ZYDIS_MNEMONIC_MOVSXD &&
                        previous.operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && previous.operands[1].mem.scale == 4) {
                        tableShaped = true;
                    }
                }
                _statistics.indirectTableShaped += tableShaped ? 1 : 0;
            }
        }
        out << "    { u64 target = " << Read(decoded, 0, 64) << ";\n";
        if (block.jumpTableRecovered) {
            ++_statistics.recoveredJumpTables;
            out << "      switch (target - c->imageBase) {\n";
            for (auto target : block.jumpTableTargets) {
                out << "      case " << Hex(target) << ": goto L_" << HexName(target) << ";\n";
            }
            out << "      default: break;\n      }\n";
        }
        out << "      g->jumpTarget = target; }\n    out(); [[clang::musttail]] return JumpIndirect(g);\n";
        return true;
    }
    case ZYDIS_MNEMONIC_CALL: {
        if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            auto target = decoded.Next() + operand.imm.value.u;
            std::string call = _program.functions.count(target) != 0
                ? FunctionName(target) + "(g);"
                : "CallIndirect(g, " + Absolute(target) + ");";
            out << "    " << CallTo(call, decoded.Next()) << "\n";
        } else {
            out << "    { u64 target = " << Read(decoded, 0, 64) << "; "
                << CallTo("CallIndirect(g, target);", decoded.Next()) << " }\n";
        }
        return true;
    }
    case ZYDIS_MNEMONIC_RET: {
        std::uint64_t extra = instruction.operand_count_visible > 0 ? operand.imm.value.u : 0;
        out << "    g->returnTarget = LdStack<u64>(c->gpr[Rsp]); c->gpr[Rsp] += " << (8 + extra) << "; out();\n    return;\n";
        return true;
    }
    case ZYDIS_MNEMONIC_UD2:
    case ZYDIS_MNEMONIC_HLT:
    case ZYDIS_MNEMONIC_INT3:
        out << "    out(); Trap(g, " << Hex(decoded.address) << ", \"" << mnemonic << "\");\n    return;\n";
        return true;
    default:
        return false;
    }
}

bool Emitter::EmitInteger(std::ostream& out, const DecodedInstruction& decoded, FlagMask flags) {
    const auto& instruction = decoded.instruction;
    const auto& operands = decoded.operands;
    auto mask = MaskLiteral(flags);
    unsigned bits = operands[0].size;
    std::string mnemonic = Mnemonic(instruction);
    bool memoryDestination = operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY;
    if (!operands[0].size && instruction.operand_count_visible > 0 && operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        return false;
    }
    if (instruction.operand_count_visible > 0 && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
        IsVectorRegister(operands[0].reg.value)) {
        return false;
    }
    if (instruction.operand_count_visible > 1 && operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
        IsVectorRegister(operands[1].reg.value)) {
        return false;
    }

    auto rmw = [&](const std::string& computation) {
        auto type = UnsignedType(bits);
        if (HasLock(instruction) && memoryDestination) {
            out << "    { u64 ea = " << Address(decoded, operands[0]) << "; " << type << " a = __atomic_load_n(&AtomicRef<"
                << type << ">(ea), __ATOMIC_SEQ_CST); " << type << " r; do { r = " << computation
                << "; } while (!__atomic_compare_exchange_n(&AtomicRef<" << type
                << ">(ea), &a, r, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); }\n";
            return;
        }
        out << "    { " << type << " a = " << Read(decoded, 0) << "; " << type << " r = " << computation << "; "
            << Write(decoded, 0, "r") << " }\n";
    };
    auto binary = [&](const std::string& computation) {
        auto type = UnsignedType(bits);
        std::string b = Read(decoded, 1, bits);
        out << "    { " << type << " b = " << b << ";\n";
        rmw(computation);
        out << "    }\n";
    };

    switch (instruction.mnemonic) {
    case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_ENDBR64:
    case ZYDIS_MNEMONIC_PAUSE:
    case ZYDIS_MNEMONIC_PREFETCHT0:
    case ZYDIS_MNEMONIC_PREFETCHT1:
    case ZYDIS_MNEMONIC_PREFETCHT2:
    case ZYDIS_MNEMONIC_PREFETCHNTA:
    case ZYDIS_MNEMONIC_PREFETCHW:
        return true;
    case ZYDIS_MNEMONIC_MFENCE:
    case ZYDIS_MNEMONIC_LFENCE:
    case ZYDIS_MNEMONIC_SFENCE:
        out << "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n";
        return true;
    case ZYDIS_MNEMONIC_MOV:
        out << "    " << Write(decoded, 0, Read(decoded, 1, bits)) << "\n";
        return true;
    case ZYDIS_MNEMONIC_MOVZX:
        out << "    " << Write(decoded, 0, "(" + UnsignedType(bits) + ")" + Read(decoded, 1)) << "\n";
        return true;
    case ZYDIS_MNEMONIC_MOVSX:
    case ZYDIS_MNEMONIC_MOVSXD:
        out << "    " << Write(decoded, 0, "(" + UnsignedType(bits) + ")(" + SignedType(bits) + ")(" +
            SignedType(operands[1].size) + ")" + Read(decoded, 1)) << "\n";
        return true;
    case ZYDIS_MNEMONIC_LEA:
        out << "    " << Write(decoded, 0, Address(decoded, operands[1])) << "\n";
        return true;
    case ZYDIS_MNEMONIC_PUSH:
        out << "    { u64 v = " << (operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE
            ? "(u64)" + Hex(operands[0].imm.value.u) : "(u64)" + Read(decoded, 0))
            << "; c->gpr[Rsp] -= 8; StStack<u64>(c->gpr[Rsp], v); }\n";
        return true;
    case ZYDIS_MNEMONIC_POP:
        out << "    { u64 v = LdStack<u64>(c->gpr[Rsp]); c->gpr[Rsp] += 8; " << Write(decoded, 0, "v") << " }\n";
        return true;
    case ZYDIS_MNEMONIC_LEAVE:
        out << "    c->gpr[Rsp] = c->gpr[Rbp]; c->gpr[Rbp] = LdStack<u64>(c->gpr[Rsp]); c->gpr[Rsp] += 8;\n";
        return true;
    case ZYDIS_MNEMONIC_XCHG: {
        auto type = UnsignedType(bits);
        if (operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY || operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            int memory = operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY ? 0 : 1;
            int reg = 1 - memory;
            out << "    { " << type << " old = __atomic_exchange_n(&AtomicRef<" << type << ">(" << Address(decoded, operands[memory])
                << "), " << Read(decoded, reg) << ", __ATOMIC_SEQ_CST); " << Write(decoded, reg, "old") << " }\n";
        } else {
            out << "    { " << type << " a = " << Read(decoded, 0) << "; " << type << " b = " << Read(decoded, 1) << "; "
                << Write(decoded, 0, "b") << " " << Write(decoded, 1, "a") << " }\n";
        }
        return true;
    }
    case ZYDIS_MNEMONIC_ADD:
        binary("AddWithFlags<" + mask + ">(c, a, b, 0)");
        return true;
    case ZYDIS_MNEMONIC_ADC:
        binary("AddWithFlags<" + mask + ">(c, a, b, c->cf)");
        return true;
    case ZYDIS_MNEMONIC_SUB:
        binary("SubWithFlags<" + mask + ">(c, a, b, 0)");
        return true;
    case ZYDIS_MNEMONIC_SBB:
        binary("SubWithFlags<" + mask + ">(c, a, b, c->cf)");
        return true;
    case ZYDIS_MNEMONIC_AND:
        binary("(FlagsLogic<" + mask + ">(c, (" + UnsignedType(bits) + ")(a & b)), (" + UnsignedType(bits) + ")(a & b))");
        return true;
    case ZYDIS_MNEMONIC_OR:
        binary("(FlagsLogic<" + mask + ">(c, (" + UnsignedType(bits) + ")(a | b)), (" + UnsignedType(bits) + ")(a | b))");
        return true;
    case ZYDIS_MNEMONIC_XOR:
        if (operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            operands[0].reg.value == operands[1].reg.value) {
            out << "    " << Write(decoded, 0, "0") << " FlagsLogic<" << mask << ">(c, (" << UnsignedType(bits) << ")0);\n";
            return true;
        }
        binary("(FlagsLogic<" + mask + ">(c, (" + UnsignedType(bits) + ")(a ^ b)), (" + UnsignedType(bits) + ")(a ^ b))");
        return true;
    case ZYDIS_MNEMONIC_CMP:
        out << "    SubWithFlags<" << mask << ">(c, " << Read(decoded, 0) << ", " << Read(decoded, 1, bits) << ", 0);\n";
        return true;
    case ZYDIS_MNEMONIC_TEST:
        out << "    FlagsLogic<" << mask << ">(c, (" << UnsignedType(bits) << ")(" << Read(decoded, 0) << " & "
            << Read(decoded, 1, bits) << "));\n";
        return true;
    case ZYDIS_MNEMONIC_INC:
        rmw("AddWithFlags<(" + mask + " & ~FlagCf)>(c, a, (" + UnsignedType(bits) + ")1, 0)");
        return true;
    case ZYDIS_MNEMONIC_DEC:
        rmw("SubWithFlags<(" + mask + " & ~FlagCf)>(c, a, (" + UnsignedType(bits) + ")1, 0)");
        return true;
    case ZYDIS_MNEMONIC_NEG:
        rmw("SubWithFlags<" + mask + ">(c, (" + UnsignedType(bits) + ")0, a, 0)");
        return true;
    case ZYDIS_MNEMONIC_NOT:
        rmw("(" + UnsignedType(bits) + ")~a");
        return true;
    case ZYDIS_MNEMONIC_SHL:
    case ZYDIS_MNEMONIC_SHR:
    case ZYDIS_MNEMONIC_SAR: {
        std::string name = instruction.mnemonic == ZYDIS_MNEMONIC_SHL ? "Shl"
            : instruction.mnemonic == ZYDIS_MNEMONIC_SHR ? "Shr" : "Sar";
        std::string count = "((u32)" + Read(decoded, 1, 8) + " & " + (bits == 64 ? "0x3f" : "0x1f") + ")";
        rmw(name + "WithFlags<" + mask + ">(c, a, " + count + ")");
        return true;
    }
    case ZYDIS_MNEMONIC_ROL:
    case ZYDIS_MNEMONIC_ROR: {
        bool left = instruction.mnemonic == ZYDIS_MNEMONIC_ROL;
        std::string count = "((u32)" + Read(decoded, 1, 8) + " & " + (bits == 64 ? "0x3f" : "0x1f") + ")";
        auto type = UnsignedType(bits);
        std::string msb = std::to_string(bits - 1);
        std::string computation = std::string("[&]{ u32 n = ") + count + "; " + type + " r = " + (left ? "Rol" : "Ror") +
            "(a, n); if (n != 0) { " +
            ((flags & 1u) ? std::string(left ? "c->cf = r & 1; " : "c->cf = (r >> " + msb + ") & 1; ") : "") +
            ((flags & (1u << 11)) ? std::string(left ? "c->of = ((r >> " + msb + ") ^ r) & 1; "
                : "c->of = ((r >> " + msb + ") ^ (r >> " + std::to_string(bits - 2) + ")) & 1; ") : "") +
            "} return r; }()";
        rmw(computation);
        return true;
    }
    case ZYDIS_MNEMONIC_SHLD:
    case ZYDIS_MNEMONIC_SHRD: {
        bool left = instruction.mnemonic == ZYDIS_MNEMONIC_SHLD;
        auto type = UnsignedType(bits);
        std::string count = "((u32)" + Read(decoded, 2, 8) + " & " + (bits == 64 ? "0x3f" : "0x1f") + ")";
        std::string b = Read(decoded, 1);
        std::string computation = std::string("[&]{ u32 n = ") + count + "; if (n == 0) return a; " + type + " r = (" + type +
            ")(" + (left ? "(a << n) | ((" + type + ")" + b + " >> (" + std::to_string(bits) + " - n))"
                         : "(a >> n) | ((" + type + ")" + b + " << (" + std::to_string(bits) + " - n))") + "); " +
            ((flags & 1u) ? std::string(left ? "c->cf = (a >> (" + std::to_string(bits) + " - n)) & 1; "
                : "c->cf = (a >> (n - 1)) & 1; ") : "") +
            "FlagsResult<" + mask + ">(c, r); return r; }()";
        rmw(computation);
        return true;
    }
    case ZYDIS_MNEMONIC_IMUL: {
        auto type = UnsignedType(bits);
        auto stype = SignedType(bits);
        bool needFlags = (flags & ((1u << 0) | (1u << 11))) != 0;
        if (instruction.operand_count_visible == 1) {
            auto wide = SignedType(bits * 2);
            out << "    { " << wide << " p = (" << wide << ")(" << stype << ")" << Read(decoded, 0) << " * (" << wide
                << ")(" << stype << ")" << (bits == 8 ? "(u8)c->gpr[Rax]" : "(" + type + ")c->gpr[Rax]") << ";\n";
            if (bits == 8) {
                out << "      c->gpr[Rax] = (c->gpr[Rax] & ~0xffffull) | (u16)p;\n";
            } else {
                std::string low = bits == 64 ? "(u64)p" : "(u64)(" + type + ")p";
                std::string high = bits == 64 ? "(u64)(p >> 64)" : "(u64)(" + type + ")(p >> " + std::to_string(bits) + ")";
                if (bits == 16) {
                    out << "      c->gpr[Rax] = (c->gpr[Rax] & ~0xffffull) | (u16)p; c->gpr[Rdx] = (c->gpr[Rdx] & ~0xffffull) | (u16)(p >> 16);\n";
                } else {
                    out << "      c->gpr[Rax] = " << low << "; c->gpr[Rdx] = " << high << ";\n";
                }
            }
            if (needFlags) {
                out << "      c->cf = c->of = p != (" << wide << ")(" << stype << ")p;\n";
            }
            out << "    }\n";
            return true;
        }
        std::string a = instruction.operand_count_visible == 3 ? Read(decoded, 1) : Read(decoded, 0);
        std::string b = instruction.operand_count_visible == 3 ? Read(decoded, 2, bits) : Read(decoded, 1, bits);
        out << "    { " << stype << " r; bool overflow = __builtin_mul_overflow((" << stype << ")" << a << ", (" << stype
            << ")" << b << ", &r); (void)overflow; " << Write(decoded, 0, "(" + type + ")r")
            << (needFlags ? " c->cf = c->of = overflow;" : "") << " }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_MUL: {
        auto type = UnsignedType(bits);
        auto wide = UnsignedType(bits * 2);
        out << "    { " << wide << " p = (" << wide << ")" << Read(decoded, 0) << " * (" << wide << ")("
            << type << ")c->gpr[Rax];\n";
        if (bits == 8) {
            out << "      c->gpr[Rax] = (c->gpr[Rax] & ~0xffffull) | (u16)p; c->cf = c->of = (p >> 8) != 0;\n";
        } else if (bits == 16) {
            out << "      c->gpr[Rax] = (c->gpr[Rax] & ~0xffffull) | (u16)p; c->gpr[Rdx] = (c->gpr[Rdx] & ~0xffffull) | (u16)(p >> 16); c->cf = c->of = (p >> 16) != 0;\n";
        } else {
            out << "      c->gpr[Rax] = (u64)(" << type << ")p; c->gpr[Rdx] = (u64)(" << type << ")(p >> " << bits
                << "); c->cf = c->of = (p >> " << bits << ") != 0;\n";
        }
        out << "    }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_DIV:
    case ZYDIS_MNEMONIC_IDIV: {
        bool isSigned = instruction.mnemonic == ZYDIS_MNEMONIC_IDIV;
        auto type = isSigned ? SignedType(bits) : UnsignedType(bits);
        auto wide = isSigned ? SignedType(bits * 2) : UnsignedType(bits * 2);
        std::string dividend = bits == 8 ? "(" + wide + ")(" + (isSigned ? std::string("i16") : std::string("u16")) + ")c->gpr[Rax]"
            : "(" + wide + ")(((" + UnsignedType(bits * 2) + ")(" + UnsignedType(bits) + ")c->gpr[Rdx] << " + std::to_string(bits) +
                ") | (" + UnsignedType(bits) + ")c->gpr[Rax])";
        out << "    { " << wide << " d = (" << wide << ")(" << type << ")" << Read(decoded, 0) << "; if (d == 0) { out(); DivideError(g, "
            << Hex(decoded.address) << "); } " << wide << " n = " << dividend << "; " << wide << " q = n / d; " << wide
            << " m = n % d; if (q != (" << wide << ")(" << type << ")q) { out(); DivideError(g, " << Hex(decoded.address) << "); }\n";
        if (bits == 8) {
            out << "      c->gpr[Rax] = (c->gpr[Rax] & ~0xffffull) | (u8)q | ((u16)(u8)m << 8);\n";
        } else if (bits == 16) {
            out << "      c->gpr[Rax] = (c->gpr[Rax] & ~0xffffull) | (u16)q; c->gpr[Rdx] = (c->gpr[Rdx] & ~0xffffull) | (u16)m;\n";
        } else {
            out << "      c->gpr[Rax] = (u64)(" << UnsignedType(bits) << ")q; c->gpr[Rdx] = (u64)(" << UnsignedType(bits) << ")m;\n";
        }
        out << "    }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_CBW:
        out << "    c->gpr[Rax] = (c->gpr[Rax] & ~0xffffull) | (u16)(i16)(i8)c->gpr[Rax];\n";
        return true;
    case ZYDIS_MNEMONIC_CWDE:
        out << "    c->gpr[Rax] = (u64)(u32)(i32)(i16)c->gpr[Rax];\n";
        return true;
    case ZYDIS_MNEMONIC_CDQE:
        out << "    c->gpr[Rax] = (u64)(i64)(i32)c->gpr[Rax];\n";
        return true;
    case ZYDIS_MNEMONIC_CWD:
        out << "    c->gpr[Rdx] = (c->gpr[Rdx] & ~0xffffull) | (u16)((i16)c->gpr[Rax] >> 15);\n";
        return true;
    case ZYDIS_MNEMONIC_CDQ:
        out << "    c->gpr[Rdx] = (u64)(u32)((i32)c->gpr[Rax] >> 31);\n";
        return true;
    case ZYDIS_MNEMONIC_CQO:
        out << "    c->gpr[Rdx] = (u64)((i64)c->gpr[Rax] >> 63);\n";
        return true;
    case ZYDIS_MNEMONIC_BSWAP:
        out << "    " << Write(decoded, 0, bits == 64 ? "__builtin_bswap64(" + Read(decoded, 0) + ")"
            : "__builtin_bswap32(" + Read(decoded, 0) + ")") << "\n";
        return true;
    case ZYDIS_MNEMONIC_BT:
    case ZYDIS_MNEMONIC_BTS:
    case ZYDIS_MNEMONIC_BTR:
    case ZYDIS_MNEMONIC_BTC: {
        if (memoryDestination && operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            return false;
        }
        std::string bit = "((u32)" + Read(decoded, 1, bits == 8 ? 8 : 32) + " & " + std::to_string(bits - 1) + ")";
        auto type = UnsignedType(bits);
        out << "    { u32 n = " << bit << ";\n";
        if (instruction.mnemonic == ZYDIS_MNEMONIC_BT) {
            out << "    c->cf = (" << Read(decoded, 0) << " >> n) & 1;\n";
        } else {
            std::string op = instruction.mnemonic == ZYDIS_MNEMONIC_BTS ? "a | ((" + type + ")1 << n)"
                : instruction.mnemonic == ZYDIS_MNEMONIC_BTR ? "a & ~((" + type + ")1 << n)" : "a ^ ((" + type + ")1 << n)";
            rmw("(c->cf = (a >> n) & 1, (" + type + ")(" + op + "))");
        }
        out << "    }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_BSF:
    case ZYDIS_MNEMONIC_BSR: {
        auto type = UnsignedType(bits);
        bool forward = instruction.mnemonic == ZYDIS_MNEMONIC_BSF;
        std::string count = bits == 64 ? (forward ? "__builtin_ctzll(s)" : "63 - __builtin_clzll(s)")
            : (forward ? "__builtin_ctz((u32)s)" : std::to_string(bits - 1) + " - (__builtin_clz((u32)s) - " + std::to_string(32 - bits) + ")");
        out << "    { " << type << " s = " << Read(decoded, 1) << "; c->zf = s == 0; if (s != 0) { "
            << Write(decoded, 0, count) << " } }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_TZCNT:
    case ZYDIS_MNEMONIC_LZCNT:
    case ZYDIS_MNEMONIC_POPCNT: {
        auto type = UnsignedType(bits);
        std::string computation;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_POPCNT) {
            computation = "__builtin_popcountll(s)";
        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_TZCNT) {
            computation = "(s == 0 ? " + std::to_string(bits) + " : __builtin_ctzll(s))";
        } else {
            computation = "(s == 0 ? " + std::to_string(bits) + " : __builtin_clzll(s) - " + std::to_string(64 - bits) + ")";
        }
        out << "    { " << type << " s = " << Read(decoded, 1) << "; " << type << " r = " << computation << "; "
            << Write(decoded, 0, "r") << " ";
        if (instruction.mnemonic == ZYDIS_MNEMONIC_POPCNT) {
            out << "c->zf = s == 0; c->cf = c->of = c->sf = c->pf = c->af = 0; }\n";
        } else {
            out << "c->cf = s == 0; c->zf = r == 0; }\n";
        }
        return true;
    }
    case ZYDIS_MNEMONIC_ANDN:
        out << "    { " << UnsignedType(bits) << " r = ~" << Read(decoded, 1) << " & " << Read(decoded, 2) << "; "
            << Write(decoded, 0, "r") << " FlagsLogic<" << mask << ">(c, r); }\n";
        return true;
    case ZYDIS_MNEMONIC_SHLX:
    case ZYDIS_MNEMONIC_SHRX:
    case ZYDIS_MNEMONIC_SARX: {
        auto type = UnsignedType(bits);
        std::string count = "(" + Read(decoded, 2) + " & " + (bits == 64 ? "0x3f" : "0x1f") + ")";
        std::string value = instruction.mnemonic == ZYDIS_MNEMONIC_SHLX ? Read(decoded, 1) + " << " + count
            : instruction.mnemonic == ZYDIS_MNEMONIC_SHRX ? Read(decoded, 1) + " >> " + count
            : "(" + type + ")((" + SignedType(bits) + ")" + Read(decoded, 1) + " >> " + count + ")";
        out << "    " << Write(decoded, 0, value) << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_MULX: {
        auto type = UnsignedType(bits);
        auto wide = UnsignedType(bits * 2);
        out << "    { " << wide << " p = (" << wide << ")(" << type << ")c->gpr[Rdx] * (" << wide << ")" << Read(decoded, 2)
            << "; " << Write(decoded, 1, "(" + type + ")p") << " " << Write(decoded, 0, "(" + type + ")(p >> " + std::to_string(bits) + ")")
            << " }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_RORX:
        out << "    " << Write(decoded, 0, "Ror(" + Read(decoded, 1) + ", (u32)" + Read(decoded, 2, 8) + ")") << "\n";
        return true;
    case ZYDIS_MNEMONIC_BLSR:
        out << "    { " << UnsignedType(bits) << " s = " << Read(decoded, 1) << "; " << UnsignedType(bits)
            << " r = s & (s - 1); " << Write(decoded, 0, "r") << " c->cf = s == 0; FlagsResult<" << mask << ">(c, r); c->of = 0; }\n";
        return true;
    case ZYDIS_MNEMONIC_BLSI:
        out << "    { " << UnsignedType(bits) << " s = " << Read(decoded, 1) << "; " << UnsignedType(bits)
            << " r = s & (0 - s); " << Write(decoded, 0, "r") << " c->cf = s != 0; FlagsResult<" << mask << ">(c, r); c->of = 0; }\n";
        return true;
    case ZYDIS_MNEMONIC_BZHI:
        out << "    { u32 n = (u32)" << Read(decoded, 2, 8) << "; " << UnsignedType(bits) << " s = " << Read(decoded, 1)
            << "; " << UnsignedType(bits) << " r = n >= " << bits << " ? s : (s & (((" << UnsignedType(bits)
            << ")1 << n) - 1)); " << Write(decoded, 0, "r") << " c->cf = n > " << bits - 1 << "; FlagsResult<" << mask
            << ">(c, r); c->of = 0; }\n";
        return true;
    case ZYDIS_MNEMONIC_CMPXCHG: {
        auto type = UnsignedType(bits);
        std::string accumulator = Read(decoded, 2, bits);
        if (memoryDestination) {
            out << "    { " << type << " expected = " << accumulator << "; " << type << " desired = " << Read(decoded, 1)
                << "; " << type << " observed = expected; bool ok = __atomic_compare_exchange_n(&AtomicRef<" << type << ">("
                << Address(decoded, operands[0]) << "), &observed, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); "
                << "SubWithFlags<" << mask << ">(c, expected, observed, 0); if (!ok) { " << Write(decoded, 2, "observed")
                << " } }\n";
        } else {
            out << "    { " << type << " expected = " << accumulator << "; " << type << " current = " << Read(decoded, 0)
                << "; SubWithFlags<" << mask << ">(c, expected, current, 0); if (expected == current) { "
                << Write(decoded, 0, Read(decoded, 1)) << " } else { " << Write(decoded, 2, "current") << " } }\n";
        }
        return true;
    }
    case ZYDIS_MNEMONIC_XADD: {
        auto type = UnsignedType(bits);
        if (memoryDestination) {
            out << "    { " << type << " b = " << Read(decoded, 1) << "; " << type << " a = __atomic_fetch_add(&AtomicRef<" << type
                << ">(" << Address(decoded, operands[0]) << "), b, __ATOMIC_SEQ_CST); AddWithFlags<" << mask
                << ">(c, a, b, 0); " << Write(decoded, 1, "a") << " }\n";
        } else {
            out << "    { " << type << " a = " << Read(decoded, 0) << "; " << type << " b = " << Read(decoded, 1) << "; "
                << type << " r = AddWithFlags<" << mask << ">(c, a, b, 0); " << Write(decoded, 1, "a") << " "
                << Write(decoded, 0, "r") << " }\n";
        }
        return true;
    }
    case ZYDIS_MNEMONIC_STOSB:
    case ZYDIS_MNEMONIC_STOSW:
    case ZYDIS_MNEMONIC_STOSD:
    case ZYDIS_MNEMONIC_STOSQ:
    case ZYDIS_MNEMONIC_MOVSB:
    case ZYDIS_MNEMONIC_MOVSW:
    case ZYDIS_MNEMONIC_MOVSD:
    case ZYDIS_MNEMONIC_MOVSQ: {
        if (instruction.mnemonic == ZYDIS_MNEMONIC_MOVSD && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            return false;
        }
        bool store = instruction.mnemonic == ZYDIS_MNEMONIC_STOSB || instruction.mnemonic == ZYDIS_MNEMONIC_STOSW ||
            instruction.mnemonic == ZYDIS_MNEMONIC_STOSD || instruction.mnemonic == ZYDIS_MNEMONIC_STOSQ;
        unsigned width = operands[0].size / 8;
        auto type = UnsignedType(width * 8);
        std::string step = "(c->df ? -" + std::to_string(width) + "ll : " + std::to_string(width) + "ll)";
        std::string one = store
            ? "St<" + type + ">(c->gpr[Rdi], (" + type + ")c->gpr[Rax]); c->gpr[Rdi] += " + step + ";"
            : "St<" + type + ">(c->gpr[Rdi], Ld<" + type + ">(c->gpr[Rsi])); c->gpr[Rdi] += " + step + "; c->gpr[Rsi] += " + step + ";";
        if (HasRep(instruction)) {
            out << "    out(); " << (store ? "RepStos" : "RepMovs") << "(g, " << width << "); in();\n";
        } else {
            out << "    " << one << "\n";
        }
        return true;
    }
    case ZYDIS_MNEMONIC_CLD:
        out << "    c->df = 0;\n";
        return true;
    case ZYDIS_MNEMONIC_STD:
        out << "    c->df = 1;\n";
        return true;
    case ZYDIS_MNEMONIC_CLC:
        out << "    c->cf = 0;\n";
        return true;
    case ZYDIS_MNEMONIC_STC:
        out << "    c->cf = 1;\n";
        return true;
    case ZYDIS_MNEMONIC_CMC:
        out << "    c->cf ^= 1;\n";
        return true;
    case ZYDIS_MNEMONIC_CPUID:
        out << "    out(); Cpuid(g); in();\n";
        return true;
    case ZYDIS_MNEMONIC_RDTSC:
        out << "    out(); Rdtsc(g); in();\n";
        return true;
    case ZYDIS_MNEMONIC_RDRAND:
    case ZYDIS_MNEMONIC_RDSEED:
        out << "    " << Write(decoded, 0, "HostRandom()") << " c->cf = 1; c->of = c->sf = c->zf = c->af = c->pf = 0;\n";
        return true;
    default:
        break;
    }

    if (instruction.mnemonic >= ZYDIS_MNEMONIC_CMOVB && instruction.mnemonic <= ZYDIS_MNEMONIC_CMOVZ) {
        auto suffix = mnemonic.substr(4);
        out << "    if (" << Condition(suffix) << ") { " << Write(decoded, 0, Read(decoded, 1)) << " }";
        if (bits == 32) {
            out << " else { " << Write(decoded, 0, Read(decoded, 0)) << " }";
        }
        out << "\n";
        return true;
    }
    if (instruction.mnemonic >= ZYDIS_MNEMONIC_SETB && instruction.mnemonic <= ZYDIS_MNEMONIC_SETZ &&
        mnemonic.rfind("set", 0) == 0) {
        auto suffix = mnemonic.substr(3);
        out << "    " << Write(decoded, 0, "(u8)(" + Condition(suffix) + ")") << "\n";
        return true;
    }
    return false;
}

bool Emitter::EmitVector(std::ostream& out, const DecodedInstruction& decoded) {
    const auto& instruction = decoded.instruction;
    const auto& operands = decoded.operands;
    bool vex = IsVex(instruction);
    int visible = instruction.operand_count_visible;
    for (int i = 0; i < instruction.operand_count; ++i) {
        if (operands[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            ZydisRegisterGetClass(operands[i].reg.value) == ZYDIS_REGCLASS_MMX) {
            return false;
        }
    }
    auto destinationIndex = [&]() { return std::to_string(VectorIndex(operands[0].reg.value)); };
    auto isVectorRegister = [&](int index) {
        return operands[index].type == ZYDIS_OPERAND_TYPE_REGISTER && IsVectorRegister(operands[index].reg.value);
    };
    auto writeFull = [&](const std::string& type, const std::string& value) {
        return "WriteVec<" + type + ">(c, " + destinationIndex() + ", " + value + ", " + (vex ? "true" : "false") + ");";
    };

    static const std::map<ZydisMnemonic, std::pair<unsigned, bool>> interleaves = {
        {ZYDIS_MNEMONIC_PUNPCKLBW, {8, false}}, {ZYDIS_MNEMONIC_PUNPCKHBW, {8, true}},
        {ZYDIS_MNEMONIC_PUNPCKLWD, {16, false}}, {ZYDIS_MNEMONIC_PUNPCKHWD, {16, true}},
        {ZYDIS_MNEMONIC_PUNPCKLDQ, {32, false}}, {ZYDIS_MNEMONIC_PUNPCKHDQ, {32, true}},
        {ZYDIS_MNEMONIC_PUNPCKLQDQ, {64, false}}, {ZYDIS_MNEMONIC_PUNPCKHQDQ, {64, true}},
        {ZYDIS_MNEMONIC_UNPCKLPS, {32, false}}, {ZYDIS_MNEMONIC_UNPCKHPS, {32, true}},
        {ZYDIS_MNEMONIC_UNPCKLPD, {64, false}}, {ZYDIS_MNEMONIC_UNPCKHPD, {64, true}},
        {ZYDIS_MNEMONIC_VPUNPCKLBW, {8, false}}, {ZYDIS_MNEMONIC_VPUNPCKHBW, {8, true}},
        {ZYDIS_MNEMONIC_VPUNPCKLWD, {16, false}}, {ZYDIS_MNEMONIC_VPUNPCKHWD, {16, true}},
        {ZYDIS_MNEMONIC_VPUNPCKLDQ, {32, false}}, {ZYDIS_MNEMONIC_VPUNPCKHDQ, {32, true}},
        {ZYDIS_MNEMONIC_VPUNPCKLQDQ, {64, false}}, {ZYDIS_MNEMONIC_VPUNPCKHQDQ, {64, true}},
        {ZYDIS_MNEMONIC_VUNPCKLPS, {32, false}}, {ZYDIS_MNEMONIC_VUNPCKHPS, {32, true}},
        {ZYDIS_MNEMONIC_VUNPCKLPD, {64, false}}, {ZYDIS_MNEMONIC_VUNPCKHPD, {64, true}},
    };
    if (auto it = interleaves.find(instruction.mnemonic); it != interleaves.end()) {
        unsigned bits = operands[0].size;
        unsigned elementBits = it->second.first;
        bool high = it->second.second;
        unsigned perLane = 128 / elementBits;
        unsigned count = bits / elementBits;
        std::ostringstream lanes;
        bool first = true;
        for (unsigned lane = 0; lane < bits / 128; ++lane) {
            for (unsigned i = 0; i < perLane / 2; ++i) {
                unsigned index = lane * perLane + (high ? perLane / 2 : 0) + i;
                lanes << (first ? "" : ", ") << index << ", " << count + index;
                first = false;
            }
        }
        auto type = VectorType("u" + std::to_string(elementBits), bits);
        out << "    " << writeFull(type, "__builtin_shufflevector(" + ReadVector(decoded, vex ? 1 : 0, type) + ", " +
            ReadVector(decoded, vex ? 2 : 1, type) + ", " + lanes.str() + ")") << "\n";
        return true;
    }
    if (auto it = PackedBinaryOps().find(instruction.mnemonic); it != PackedBinaryOps().end()) {
        unsigned bits = operands[0].size;
        auto type = VectorType(it->second.element, bits);
        out << "    { " << type << " a = " << ReadVector(decoded, 0, type) << "; " << type << " b = "
            << ReadVector(decoded, 1, type) << "; "
            << writeFull(type, "(" + type + ")" + BinaryExpression(it->second.op, "a", "b")) << " }\n";
        return true;
    }
    static const std::map<ZydisMnemonic, ZydisMnemonic> vexAliases = {
        {ZYDIS_MNEMONIC_VADDPS, ZYDIS_MNEMONIC_ADDPS}, {ZYDIS_MNEMONIC_VADDPD, ZYDIS_MNEMONIC_ADDPD},
        {ZYDIS_MNEMONIC_VSUBPS, ZYDIS_MNEMONIC_SUBPS}, {ZYDIS_MNEMONIC_VSUBPD, ZYDIS_MNEMONIC_SUBPD},
        {ZYDIS_MNEMONIC_VMULPS, ZYDIS_MNEMONIC_MULPS}, {ZYDIS_MNEMONIC_VMULPD, ZYDIS_MNEMONIC_MULPD},
        {ZYDIS_MNEMONIC_VDIVPS, ZYDIS_MNEMONIC_DIVPS}, {ZYDIS_MNEMONIC_VDIVPD, ZYDIS_MNEMONIC_DIVPD},
        {ZYDIS_MNEMONIC_VANDPS, ZYDIS_MNEMONIC_ANDPS}, {ZYDIS_MNEMONIC_VANDPD, ZYDIS_MNEMONIC_ANDPD},
        {ZYDIS_MNEMONIC_VPAND, ZYDIS_MNEMONIC_PAND}, {ZYDIS_MNEMONIC_VORPS, ZYDIS_MNEMONIC_ORPS},
        {ZYDIS_MNEMONIC_VORPD, ZYDIS_MNEMONIC_ORPD}, {ZYDIS_MNEMONIC_VPOR, ZYDIS_MNEMONIC_POR},
        {ZYDIS_MNEMONIC_VXORPS, ZYDIS_MNEMONIC_XORPS}, {ZYDIS_MNEMONIC_VXORPD, ZYDIS_MNEMONIC_XORPD},
        {ZYDIS_MNEMONIC_VPXOR, ZYDIS_MNEMONIC_PXOR}, {ZYDIS_MNEMONIC_VANDNPS, ZYDIS_MNEMONIC_ANDNPS},
        {ZYDIS_MNEMONIC_VANDNPD, ZYDIS_MNEMONIC_ANDNPD}, {ZYDIS_MNEMONIC_VPANDN, ZYDIS_MNEMONIC_PANDN},
        {ZYDIS_MNEMONIC_VPADDB, ZYDIS_MNEMONIC_PADDB}, {ZYDIS_MNEMONIC_VPADDW, ZYDIS_MNEMONIC_PADDW},
        {ZYDIS_MNEMONIC_VPADDD, ZYDIS_MNEMONIC_PADDD}, {ZYDIS_MNEMONIC_VPADDQ, ZYDIS_MNEMONIC_PADDQ},
        {ZYDIS_MNEMONIC_VPSUBB, ZYDIS_MNEMONIC_PSUBB}, {ZYDIS_MNEMONIC_VPSUBW, ZYDIS_MNEMONIC_PSUBW},
        {ZYDIS_MNEMONIC_VPSUBD, ZYDIS_MNEMONIC_PSUBD}, {ZYDIS_MNEMONIC_VPSUBQ, ZYDIS_MNEMONIC_PSUBQ},
        {ZYDIS_MNEMONIC_VPMULLW, ZYDIS_MNEMONIC_PMULLW}, {ZYDIS_MNEMONIC_VPMULLD, ZYDIS_MNEMONIC_PMULLD},
        {ZYDIS_MNEMONIC_VPCMPEQB, ZYDIS_MNEMONIC_PCMPEQB}, {ZYDIS_MNEMONIC_VPCMPEQW, ZYDIS_MNEMONIC_PCMPEQW},
        {ZYDIS_MNEMONIC_VPCMPEQD, ZYDIS_MNEMONIC_PCMPEQD}, {ZYDIS_MNEMONIC_VPCMPEQQ, ZYDIS_MNEMONIC_PCMPEQQ},
        {ZYDIS_MNEMONIC_VPCMPGTB, ZYDIS_MNEMONIC_PCMPGTB}, {ZYDIS_MNEMONIC_VPCMPGTW, ZYDIS_MNEMONIC_PCMPGTW},
        {ZYDIS_MNEMONIC_VPCMPGTD, ZYDIS_MNEMONIC_PCMPGTD}, {ZYDIS_MNEMONIC_VPCMPGTQ, ZYDIS_MNEMONIC_PCMPGTQ},
        {ZYDIS_MNEMONIC_VPMINSD, ZYDIS_MNEMONIC_PMINSD}, {ZYDIS_MNEMONIC_VPMAXSD, ZYDIS_MNEMONIC_PMAXSD},
        {ZYDIS_MNEMONIC_VPMINUD, ZYDIS_MNEMONIC_PMINUD}, {ZYDIS_MNEMONIC_VPMAXUD, ZYDIS_MNEMONIC_PMAXUD},
        {ZYDIS_MNEMONIC_VPMINUB, ZYDIS_MNEMONIC_PMINUB}, {ZYDIS_MNEMONIC_VPMAXUB, ZYDIS_MNEMONIC_PMAXUB},
        {ZYDIS_MNEMONIC_VPMINSW, ZYDIS_MNEMONIC_PMINSW}, {ZYDIS_MNEMONIC_VPMAXSW, ZYDIS_MNEMONIC_PMAXSW},
        {ZYDIS_MNEMONIC_VMINPS, ZYDIS_MNEMONIC_MINPS}, {ZYDIS_MNEMONIC_VMAXPS, ZYDIS_MNEMONIC_MAXPS},
        {ZYDIS_MNEMONIC_VMINPD, ZYDIS_MNEMONIC_MINPD}, {ZYDIS_MNEMONIC_VMAXPD, ZYDIS_MNEMONIC_MAXPD},
        {ZYDIS_MNEMONIC_VADDSS, ZYDIS_MNEMONIC_ADDSS}, {ZYDIS_MNEMONIC_VADDSD, ZYDIS_MNEMONIC_ADDSD},
        {ZYDIS_MNEMONIC_VSUBSS, ZYDIS_MNEMONIC_SUBSS}, {ZYDIS_MNEMONIC_VSUBSD, ZYDIS_MNEMONIC_SUBSD},
        {ZYDIS_MNEMONIC_VMULSS, ZYDIS_MNEMONIC_MULSS}, {ZYDIS_MNEMONIC_VMULSD, ZYDIS_MNEMONIC_MULSD},
        {ZYDIS_MNEMONIC_VDIVSS, ZYDIS_MNEMONIC_DIVSS}, {ZYDIS_MNEMONIC_VDIVSD, ZYDIS_MNEMONIC_DIVSD},
        {ZYDIS_MNEMONIC_VMINSS, ZYDIS_MNEMONIC_MINSS}, {ZYDIS_MNEMONIC_VMINSD, ZYDIS_MNEMONIC_MINSD},
        {ZYDIS_MNEMONIC_VMAXSS, ZYDIS_MNEMONIC_MAXSS}, {ZYDIS_MNEMONIC_VMAXSD, ZYDIS_MNEMONIC_MAXSD},
        {ZYDIS_MNEMONIC_VSQRTSS, ZYDIS_MNEMONIC_SQRTSS}, {ZYDIS_MNEMONIC_VSQRTSD, ZYDIS_MNEMONIC_SQRTSD},
    };
    auto mnemonicId = instruction.mnemonic;
    if (auto alias = vexAliases.find(mnemonicId); alias != vexAliases.end()) {
        mnemonicId = alias->second;
        if (auto it = PackedBinaryOps().find(mnemonicId); it != PackedBinaryOps().end()) {
            unsigned bits = operands[0].size;
            auto type = VectorType(it->second.element, bits);
            out << "    { " << type << " a = " << ReadVector(decoded, 1, type) << "; " << type << " b = "
                << ReadVector(decoded, 2, type) << "; " << writeFull(type, "(" + type + ")" + BinaryExpression(it->second.op, "a", "b"))
                << " }\n";
            return true;
        }
    }

    if (auto it = ScalarBinaryOps().find(mnemonicId); it != ScalarBinaryOps().end()) {
        auto element = ElementCType(it->second.element);
        unsigned elementBits = ElementBits(it->second.element);
        int sourceA = vex ? 1 : 0;
        int sourceB = vex ? (visible == 3 ? 2 : 1) : 1;
        std::string b = operands[sourceB].type == ZYDIS_OPERAND_TYPE_MEMORY
            ? "Ld<" + element + ">(" + Address(decoded, operands[sourceB]) + ")"
            : "ReadVec<" + element + ">(c, " + std::to_string(VectorIndex(operands[sourceB].reg.value)) + ")";
        out << "    { Vec<u8, 16> base = ReadVec<Vec<u8, 16>>(c, " << VectorIndex(operands[sourceA].reg.value) << "); "
            << element << " a = ReadVec<" << element << ">(c, " << VectorIndex(operands[sourceA].reg.value) << "); "
            << element << " b = " << b << "; " << element << " r = " << BinaryExpression(it->second.op, "a", "b") << "; "
            << "std::memcpy(&base, &r, sizeof(r)); " << writeFull("Vec<u8, 16>", "base") << " }\n";
        (void)elementBits;
        return true;
    }

    switch (instruction.mnemonic) {
    case ZYDIS_MNEMONIC_MOVAPS:
    case ZYDIS_MNEMONIC_MOVUPS:
    case ZYDIS_MNEMONIC_MOVAPD:
    case ZYDIS_MNEMONIC_MOVUPD:
    case ZYDIS_MNEMONIC_MOVDQA:
    case ZYDIS_MNEMONIC_MOVDQU:
    case ZYDIS_MNEMONIC_LDDQU:
    case ZYDIS_MNEMONIC_MOVNTPS:
    case ZYDIS_MNEMONIC_MOVNTDQ:
    case ZYDIS_MNEMONIC_VMOVAPS:
    case ZYDIS_MNEMONIC_VMOVUPS:
    case ZYDIS_MNEMONIC_VMOVAPD:
    case ZYDIS_MNEMONIC_VMOVUPD:
    case ZYDIS_MNEMONIC_VMOVDQA:
    case ZYDIS_MNEMONIC_VMOVDQU:
    case ZYDIS_MNEMONIC_VMOVNTPS:
    case ZYDIS_MNEMONIC_VMOVNTDQ: {
        unsigned bits = operands[0].size;
        auto type = "Vec<u8, " + std::to_string(bits / 8) + ">";
        if (operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            out << "    St<" << type << ">(" << Address(decoded, operands[0]) << ", ReadVec<" << type << ">(c, "
                << VectorIndex(operands[1].reg.value) << "));\n";
        } else {
            out << "    " << writeFull(type, ReadVector(decoded, 1, type)) << "\n";
        }
        return true;
    }
    case ZYDIS_MNEMONIC_MOVD:
    case ZYDIS_MNEMONIC_MOVQ:
    case ZYDIS_MNEMONIC_VMOVD:
    case ZYDIS_MNEMONIC_VMOVQ: {
        unsigned bits = std::min(operands[0].size, operands[1].size);
        if (operands[0].size == 128 || isVectorRegister(0)) {
            bits = operands[1].size == 128 ? 64 : operands[1].size;
            auto type = UnsignedType(bits);
            std::string source = isVectorRegister(1)
                ? "ReadVec<" + type + ">(c, " + std::to_string(VectorIndex(operands[1].reg.value)) + ")"
                : Read(decoded, 1, bits);
            out << "    { Vec<u8, 16> v = {}; " << type << " s = " << source << "; std::memcpy(&v, &s, sizeof(s)); "
                << "WriteVec<Vec<u8, 16>>(c, " << destinationIndex() << ", v, " << (vex ? "true" : "false") << "); }\n";
            return true;
        }
        auto type = UnsignedType(bits);
        out << "    " << Write(decoded, 0, "ReadVec<" + type + ">(c, " + std::to_string(VectorIndex(operands[1].reg.value)) + ")")
            << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_MOVSS:
    case ZYDIS_MNEMONIC_MOVSD:
    case ZYDIS_MNEMONIC_VMOVSS:
    case ZYDIS_MNEMONIC_VMOVSD: {
        bool single = instruction.mnemonic == ZYDIS_MNEMONIC_MOVSS || instruction.mnemonic == ZYDIS_MNEMONIC_VMOVSS;
        std::string type = single ? "u32" : "u64";
        if (operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            out << "    St<" << type << ">(" << Address(decoded, operands[0]) << ", ReadVec<" << type << ">(c, "
                << VectorIndex(operands[visible - 1].reg.value) << "));\n";
        } else if (operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            out << "    { Vec<u8, 16> v = {}; " << type << " s = Ld<" << type << ">(" << Address(decoded, operands[1])
                << "); std::memcpy(&v, &s, sizeof(s)); WriteVec<Vec<u8, 16>>(c, " << destinationIndex() << ", v, "
                << (vex ? "true" : "false") << "); }\n";
        } else if (vex && visible == 3) {
            out << "    { Vec<u8, 16> v = ReadVec<Vec<u8, 16>>(c, " << VectorIndex(operands[1].reg.value) << "); " << type
                << " s = ReadVec<" << type << ">(c, " << VectorIndex(operands[2].reg.value)
                << "); std::memcpy(&v, &s, sizeof(s)); WriteVec<Vec<u8, 16>>(c, " << destinationIndex() << ", v, true); }\n";
        } else {
            out << "    WriteVecLow<" << type << ">(c, " << destinationIndex() << ", ReadVec<" << type << ">(c, "
                << VectorIndex(operands[1].reg.value) << "));\n";
        }
        return true;
    }
    case ZYDIS_MNEMONIC_CVTSI2SS:
    case ZYDIS_MNEMONIC_CVTSI2SD:
    case ZYDIS_MNEMONIC_VCVTSI2SS:
    case ZYDIS_MNEMONIC_VCVTSI2SD: {
        bool single = instruction.mnemonic == ZYDIS_MNEMONIC_CVTSI2SS || instruction.mnemonic == ZYDIS_MNEMONIC_VCVTSI2SS;
        int source = visible - 1;
        unsigned bits = operands[source].size;
        std::string element = single ? "float" : "double";
        int baseIndex = vex ? 1 : 0;
        out << "    { Vec<u8, 16> base = ReadVec<Vec<u8, 16>>(c, " << VectorIndex(operands[baseIndex].reg.value) << "); "
            << element << " r = (" << element << ")(" << SignedType(bits) << ")" << Read(decoded, source)
            << "; std::memcpy(&base, &r, sizeof(r)); " << writeFull("Vec<u8, 16>", "base") << " }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_CVTTSS2SI:
    case ZYDIS_MNEMONIC_CVTTSD2SI:
    case ZYDIS_MNEMONIC_CVTSS2SI:
    case ZYDIS_MNEMONIC_CVTSD2SI:
    case ZYDIS_MNEMONIC_VCVTTSS2SI:
    case ZYDIS_MNEMONIC_VCVTTSD2SI: {
        bool single = instruction.mnemonic == ZYDIS_MNEMONIC_CVTTSS2SI || instruction.mnemonic == ZYDIS_MNEMONIC_CVTSS2SI ||
            instruction.mnemonic == ZYDIS_MNEMONIC_VCVTTSS2SI;
        bool truncate = instruction.mnemonic == ZYDIS_MNEMONIC_CVTTSS2SI || instruction.mnemonic == ZYDIS_MNEMONIC_CVTTSD2SI ||
            instruction.mnemonic == ZYDIS_MNEMONIC_VCVTTSS2SI || instruction.mnemonic == ZYDIS_MNEMONIC_VCVTTSD2SI;
        std::string element = single ? "float" : "double";
        unsigned bits = operands[0].size;
        std::string source = operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY
            ? "Ld<" + element + ">(" + Address(decoded, operands[1]) + ")"
            : "ReadVec<" + element + ">(c, " + std::to_string(VectorIndex(operands[1].reg.value)) + ")";
        std::string convert = std::string(truncate ? "Cvt" : "CvtRound") + "F64ToI" + std::to_string(bits);
        out << "    " << Write(decoded, 0, convert + "((double)" + source + ")") << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_CVTSS2SD:
    case ZYDIS_MNEMONIC_CVTSD2SS:
    case ZYDIS_MNEMONIC_VCVTSS2SD:
    case ZYDIS_MNEMONIC_VCVTSD2SS: {
        bool toDouble = instruction.mnemonic == ZYDIS_MNEMONIC_CVTSS2SD || instruction.mnemonic == ZYDIS_MNEMONIC_VCVTSS2SD;
        std::string from = toDouble ? "float" : "double";
        std::string to = toDouble ? "double" : "float";
        int source = visible - 1;
        int baseIndex = vex ? 1 : 0;
        std::string value = operands[source].type == ZYDIS_OPERAND_TYPE_MEMORY
            ? "Ld<" + from + ">(" + Address(decoded, operands[source]) + ")"
            : "ReadVec<" + from + ">(c, " + std::to_string(VectorIndex(operands[source].reg.value)) + ")";
        out << "    { Vec<u8, 16> base = ReadVec<Vec<u8, 16>>(c, " << VectorIndex(operands[baseIndex].reg.value) << "); " << to
            << " r = (" << to << ")" << value << "; std::memcpy(&base, &r, sizeof(r)); " << writeFull("Vec<u8, 16>", "base")
            << " }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_UCOMISS:
    case ZYDIS_MNEMONIC_UCOMISD:
    case ZYDIS_MNEMONIC_COMISS:
    case ZYDIS_MNEMONIC_COMISD:
    case ZYDIS_MNEMONIC_VUCOMISS:
    case ZYDIS_MNEMONIC_VUCOMISD:
    case ZYDIS_MNEMONIC_VCOMISS:
    case ZYDIS_MNEMONIC_VCOMISD: {
        bool single = instruction.mnemonic == ZYDIS_MNEMONIC_UCOMISS || instruction.mnemonic == ZYDIS_MNEMONIC_COMISS ||
            instruction.mnemonic == ZYDIS_MNEMONIC_VUCOMISS || instruction.mnemonic == ZYDIS_MNEMONIC_VCOMISS;
        std::string element = single ? "float" : "double";
        std::string b = operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY
            ? "Ld<" + element + ">(" + Address(decoded, operands[1]) + ")"
            : "ReadVec<" + element + ">(c, " + std::to_string(VectorIndex(operands[1].reg.value)) + ")";
        out << "    FlagsUcomi(c, ReadVec<" << element << ">(c, " << VectorIndex(operands[0].reg.value) << "), " << b << ");\n";
        return true;
    }
    case ZYDIS_MNEMONIC_CVTDQ2PS:
    case ZYDIS_MNEMONIC_VCVTDQ2PS: {
        unsigned bits = operands[0].size;
        out << "    " << writeFull(VectorType("f32", bits), "__builtin_convertvector(" + ReadVector(decoded, 1, VectorType("i32", bits)) +
            ", " + VectorType("f32", bits) + ")") << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_CVTTPS2DQ:
    case ZYDIS_MNEMONIC_VCVTTPS2DQ: {
        unsigned bits = operands[0].size;
        out << "    " << writeFull(VectorType("i32", bits), "CvttPs2Dq(" + ReadVector(decoded, 1, VectorType("f32", bits)) + ")")
            << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_SHUFPS:
    case ZYDIS_MNEMONIC_VSHUFPS: {
        unsigned bits = operands[0].size;
        int a = vex ? 1 : 0;
        int b = vex ? 2 : 1;
        auto imm = static_cast<unsigned>(operands[vex ? 3 : 2].imm.value.u);
        auto type = VectorType("u32", bits);
        std::ostringstream lanes;
        for (unsigned half = 0; half < bits / 128; ++half) {
            unsigned base = half * 4;
            unsigned count = bits / 32;
            lanes << (half ? ", " : "") << base + (imm & 3) << ", " << base + ((imm >> 2) & 3) << ", "
                  << count + base + ((imm >> 4) & 3) << ", " << count + base + ((imm >> 6) & 3);
        }
        out << "    " << writeFull(type, "__builtin_shufflevector(" + ReadVector(decoded, a, type) + ", " +
            ReadVector(decoded, b, type) + ", " + lanes.str() + ")") << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_PSHUFD:
    case ZYDIS_MNEMONIC_VPSHUFD: {
        unsigned bits = operands[0].size;
        auto imm = static_cast<unsigned>(operands[2].imm.value.u);
        auto type = VectorType("u32", bits);
        std::ostringstream lanes;
        for (unsigned half = 0; half < bits / 128; ++half) {
            unsigned base = half * 4;
            lanes << (half ? ", " : "") << base + (imm & 3) << ", " << base + ((imm >> 2) & 3) << ", " << base + ((imm >> 4) & 3)
                  << ", " << base + ((imm >> 6) & 3);
        }
        out << "    { " << type << " s = " << ReadVector(decoded, 1, type) << "; "
            << writeFull(type, "__builtin_shufflevector(s, s, " + lanes.str() + ")") << " }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_MOVLHPS:
    case ZYDIS_MNEMONIC_MOVHLPS:
    case ZYDIS_MNEMONIC_VMOVLHPS:
    case ZYDIS_MNEMONIC_VMOVHLPS: {
        bool low = instruction.mnemonic == ZYDIS_MNEMONIC_MOVLHPS || instruction.mnemonic == ZYDIS_MNEMONIC_VMOVLHPS;
        int a = vex ? 1 : 0;
        int b = vex ? 2 : 1;
        out << "    " << writeFull("Vec<u64, 16>", "__builtin_shufflevector(" + ReadVector(decoded, a, "Vec<u64, 16>") + ", " +
            ReadVector(decoded, b, "Vec<u64, 16>") + ", " + (low ? "0, 2" : "3, 1") + ")") << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_VPERM2I128:
    case ZYDIS_MNEMONIC_VPERM2F128:
        out << "    " << writeFull("Vec<u8, 32>", "Permute2x128(" + ReadVector(decoded, 1, "Vec<u8, 32>") + ", " +
            ReadVector(decoded, 2, "Vec<u8, 32>") + ", " + std::to_string(operands[3].imm.value.u & 0xff) + "u)") << "\n";
        return true;
    case ZYDIS_MNEMONIC_VPERMQ:
    case ZYDIS_MNEMONIC_VPERMPD: {
        auto imm = static_cast<unsigned>(operands[2].imm.value.u);
        out << "    { Vec<u64, 32> s = " << ReadVector(decoded, 1, "Vec<u64, 32>") << "; "
            << writeFull("Vec<u64, 32>", "__builtin_shufflevector(s, s, " + std::to_string(imm & 3) + ", " +
                std::to_string((imm >> 2) & 3) + ", " + std::to_string((imm >> 4) & 3) + ", " + std::to_string((imm >> 6) & 3) + ")")
            << " }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_BLENDPS:
    case ZYDIS_MNEMONIC_VBLENDPS:
    case ZYDIS_MNEMONIC_VPBLENDD:
    case ZYDIS_MNEMONIC_BLENDPD:
    case ZYDIS_MNEMONIC_VBLENDPD:
    case ZYDIS_MNEMONIC_PBLENDW:
    case ZYDIS_MNEMONIC_VPBLENDW: {
        unsigned bits = operands[0].size;
        bool word = instruction.mnemonic == ZYDIS_MNEMONIC_PBLENDW || instruction.mnemonic == ZYDIS_MNEMONIC_VPBLENDW;
        bool quad = instruction.mnemonic == ZYDIS_MNEMONIC_BLENDPD || instruction.mnemonic == ZYDIS_MNEMONIC_VBLENDPD;
        unsigned elementBits = word ? 16 : quad ? 64 : 32;
        auto type = VectorType("u" + std::to_string(elementBits), bits);
        auto imm = static_cast<unsigned>(operands[visible - 1].imm.value.u);
        unsigned count = bits / elementBits;
        unsigned perLane = 128 / elementBits;
        std::ostringstream lanes;
        for (unsigned i = 0; i < count; ++i) {
            unsigned selector = word ? (i % perLane) : i;
            lanes << (i ? ", " : "") << (((imm >> selector) & 1) != 0 ? count + i : i);
        }
        out << "    " << writeFull(type, "__builtin_shufflevector(" + ReadVector(decoded, vex ? 1 : 0, type) + ", " +
            ReadVector(decoded, vex ? 2 : 1, type) + ", " + lanes.str() + ")") << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_PMULUDQ:
    case ZYDIS_MNEMONIC_VPMULUDQ: {
        auto type = VectorType("u64", operands[0].size);
        out << "    " << writeFull(type, "(" + ReadVector(decoded, vex ? 1 : 0, type) + " & 0xffffffffull) * (" +
            ReadVector(decoded, vex ? 2 : 1, type) + " & 0xffffffffull)") << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_PACKUSWB:
    case ZYDIS_MNEMONIC_PACKSSWB:
    case ZYDIS_MNEMONIC_PACKSSDW:
    case ZYDIS_MNEMONIC_PACKUSDW:
    case ZYDIS_MNEMONIC_VPACKUSWB:
    case ZYDIS_MNEMONIC_VPACKSSWB:
    case ZYDIS_MNEMONIC_VPACKSSDW:
    case ZYDIS_MNEMONIC_VPACKUSDW: {
        std::string name = Mnemonic(instruction);
        if (name[0] == 'v') name = name.substr(1);
        bool fromDword = name.back() == 'w' && name[name.size() - 2] == 'd';
        bool unsignedResult = name.find("packus") == 0;
        unsigned bits = operands[0].size;
        auto source = VectorType(fromDword ? "i32" : "i16", bits);
        std::string narrow = fromDword ? (unsignedResult ? "u16" : "i16") : (unsignedResult ? "u8" : "i8");
        auto result = VectorType(narrow, bits);
        out << "    " << writeFull(result, "Pack<" + narrow + ", " + source + ", " + result + ">(" +
            ReadVector(decoded, vex ? 1 : 0, source) + ", " + ReadVector(decoded, vex ? 2 : 1, source) + ")") << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_PSHUFB:
    case ZYDIS_MNEMONIC_VPSHUFB: {
        auto type = VectorType("u8", operands[0].size);
        out << "    " << writeFull(type, "ShuffleBytes(" + ReadVector(decoded, vex ? 1 : 0, type) + ", " +
            ReadVector(decoded, vex ? 2 : 1, type) + ")") << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_PMADDUBSW:
    case ZYDIS_MNEMONIC_VPMADDUBSW: {
        auto type = VectorType("u8", operands[0].size);
        out << "    " << writeFull(VectorType("i16", operands[0].size), "MaddUbsw(" + ReadVector(decoded, vex ? 1 : 0, type) + ", " +
            ReadVector(decoded, vex ? 2 : 1, type) + ")") << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_PMADDWD:
    case ZYDIS_MNEMONIC_VPMADDWD: {
        auto type = VectorType("i16", operands[0].size);
        out << "    " << writeFull(VectorType("i32", operands[0].size), "MaddWd(" + ReadVector(decoded, vex ? 1 : 0, type) + ", " +
            ReadVector(decoded, vex ? 2 : 1, type) + ")") << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_PMOVZXBW: case ZYDIS_MNEMONIC_PMOVZXBD: case ZYDIS_MNEMONIC_PMOVZXWD: case ZYDIS_MNEMONIC_PMOVZXDQ:
    case ZYDIS_MNEMONIC_PMOVSXBW: case ZYDIS_MNEMONIC_PMOVSXBD: case ZYDIS_MNEMONIC_PMOVSXWD: case ZYDIS_MNEMONIC_PMOVSXDQ:
    case ZYDIS_MNEMONIC_VPMOVZXBW: case ZYDIS_MNEMONIC_VPMOVZXBD: case ZYDIS_MNEMONIC_VPMOVZXWD: case ZYDIS_MNEMONIC_VPMOVZXDQ:
    case ZYDIS_MNEMONIC_VPMOVSXBW: case ZYDIS_MNEMONIC_VPMOVSXBD: case ZYDIS_MNEMONIC_VPMOVSXWD: case ZYDIS_MNEMONIC_VPMOVSXDQ: {
        std::string name = Mnemonic(instruction);
        if (name[0] == 'v') name = name.substr(1);
        bool sign = name[4] == 's';
        auto width = [](char code) { return code == 'b' ? 8u : code == 'w' ? 16u : code == 'd' ? 32u : 64u; };
        unsigned from = width(name[6]);
        unsigned to = width(name[7]);
        unsigned bits = operands[0].size;
        unsigned count = bits / to;
        std::string fromType = (sign ? "i" : "u") + std::to_string(from);
        std::string toType = (sign ? "i" : "u") + std::to_string(to);
        auto sourceType = VectorType(fromType, count * from < 128 ? 16 * 8 : count * from);
        std::ostringstream lanes;
        for (unsigned i = 0; i < count; ++i) lanes << (i ? ", " : "") << i;
        std::string source = operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY
            ? "[&]{ " + sourceType + " v = {}; std::memcpy(&v, (const void*)" + Address(decoded, operands[1]) + ", " +
                std::to_string(count * from / 8) + "); return v; }()"
            : ReadVector(decoded, 1, sourceType);
        out << "    { " << sourceType << " s = " << source << "; "
            << writeFull(VectorType(toType, bits), "__builtin_convertvector(__builtin_shufflevector(s, s, " + lanes.str() + "), " +
                VectorType(toType, bits) + ")") << " }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_PALIGNR:
    case ZYDIS_MNEMONIC_VPALIGNR: {
        if (operands[0].size != 128) return false;
        unsigned shift = static_cast<unsigned>(operands[visible - 1].imm.value.u);
        std::ostringstream lanes;
        for (unsigned i = 0; i < 16; ++i) {
            unsigned source = i + shift;
            lanes << (i ? ", " : "") << (source < 16 ? source : source < 32 ? source + 16 : 48);
        }
        out << "    { Vec<u8, 16> low = " << ReadVector(decoded, vex ? 2 : 1, "Vec<u8, 16>") << "; Vec<u8, 16> high = "
            << ReadVector(decoded, vex ? 1 : 0, "Vec<u8, 16>") << "; Vec<u8, 16> z = {}; Vec<u8, 32> lo2 = __builtin_shufflevector(low, high, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31); Vec<u8, 32> z2 = __builtin_shufflevector(z, z, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31); "
            << writeFull("Vec<u8, 16>", "__builtin_shufflevector(lo2, z2, " + lanes.str() + ")") << " }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_PSLLD:
    case ZYDIS_MNEMONIC_PSRLD:
    case ZYDIS_MNEMONIC_PSRAD:
    case ZYDIS_MNEMONIC_PSLLQ:
    case ZYDIS_MNEMONIC_PSRLQ:
    case ZYDIS_MNEMONIC_PSLLW:
    case ZYDIS_MNEMONIC_PSRLW:
    case ZYDIS_MNEMONIC_PSRAW:
    case ZYDIS_MNEMONIC_VPSLLD:
    case ZYDIS_MNEMONIC_VPSRLD:
    case ZYDIS_MNEMONIC_VPSRAD:
    case ZYDIS_MNEMONIC_VPSLLQ:
    case ZYDIS_MNEMONIC_VPSRLQ:
    case ZYDIS_MNEMONIC_VPSLLW:
    case ZYDIS_MNEMONIC_VPSRLW:
    case ZYDIS_MNEMONIC_VPSRAW: {
        int count = visible - 1;
        if (operands[count].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            return false;
        }
        std::string name = Mnemonic(instruction);
        if (name[0] == 'v') name = name.substr(1);
        std::string element = name == "psrad" ? "i32" : name == "psraw" ? "i16"
            : (name.back() == 'd' ? "u32" : name.back() == 'q' ? "u64" : "u16");
        unsigned bits = operands[0].size;
        unsigned shift = static_cast<unsigned>(operands[count].imm.value.u);
        unsigned elementBits = ElementBits(element);
        auto type = VectorType(element, bits);
        std::string source = ReadVector(decoded, vex ? 1 : 0, type);
        std::string value;
        bool left = name.find("psll") == 0;
        if (shift >= elementBits) {
            value = element[0] == 'i' ? "(" + source + " >> " + std::to_string(elementBits - 1) + ")" : "(" + type + "){}";
        } else {
            value = "(" + source + (left ? " << " : " >> ") + std::to_string(shift) + ")";
        }
        out << "    " << writeFull(type, value) << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_VPSLLVD:
    case ZYDIS_MNEMONIC_VPSLLVQ:
    case ZYDIS_MNEMONIC_VPSRLVD:
    case ZYDIS_MNEMONIC_VPSRLVQ:
    case ZYDIS_MNEMONIC_VPSRAVD: {
        std::string name = Mnemonic(instruction);
        bool quad = name.back() == 'q';
        int direction = name.find("vpsll") == 0 ? -1 : 1;
        std::string element = name.find("vpsra") == 0 ? "i32" : quad ? "u64" : "u32";
        auto type = VectorType(element, operands[0].size);
        out << "    " << writeFull(type, "ShiftVariable(" + ReadVector(decoded, 1, type) + ", " + ReadVector(decoded, 2, type) +
            ", " + std::to_string(direction) + ")") << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_PSLLDQ:
    case ZYDIS_MNEMONIC_PSRLDQ: {
        unsigned shift = std::min<unsigned>(static_cast<unsigned>(operands[1].imm.value.u), 16);
        bool left = instruction.mnemonic == ZYDIS_MNEMONIC_PSLLDQ;
        std::ostringstream lanes;
        for (unsigned i = 0; i < 16; ++i) {
            int source = left ? static_cast<int>(i) - static_cast<int>(shift) : static_cast<int>(i + shift);
            lanes << (i ? ", " : "") << (source >= 0 && source < 16 ? source : 16);
        }
        out << "    { Vec<u8, 16> s = ReadVec<Vec<u8, 16>>(c, " << destinationIndex() << "); Vec<u8, 16> z = {}; "
            << writeFull("Vec<u8, 16>", "__builtin_shufflevector(s, z, " + lanes.str() + ")") << " }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_MOVMSKPS:
    case ZYDIS_MNEMONIC_MOVMSKPD:
    case ZYDIS_MNEMONIC_PMOVMSKB:
    case ZYDIS_MNEMONIC_VMOVMSKPS:
    case ZYDIS_MNEMONIC_VPMOVMSKB: {
        std::string helper = instruction.mnemonic == ZYDIS_MNEMONIC_MOVMSKPD ? "MoveMask64"
            : (instruction.mnemonic == ZYDIS_MNEMONIC_MOVMSKPS || instruction.mnemonic == ZYDIS_MNEMONIC_VMOVMSKPS) ? "MoveMask32"
            : "MoveMask8";
        out << "    " << Write(decoded, 0, helper + "<" + std::to_string(operands[1].size / 8) + ">(c, " +
            std::to_string(VectorIndex(operands[1].reg.value)) + ")") << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_VZEROUPPER:
        out << "    for (int i = 0; i < 16; ++i) std::memset(c->ymm[i] + 16, 0, 16);\n";
        return true;
    case ZYDIS_MNEMONIC_VBROADCASTSS:
    case ZYDIS_MNEMONIC_VBROADCASTSD:
    case ZYDIS_MNEMONIC_VPBROADCASTB:
    case ZYDIS_MNEMONIC_VPBROADCASTW:
    case ZYDIS_MNEMONIC_VPBROADCASTD:
    case ZYDIS_MNEMONIC_VPBROADCASTQ: {
        unsigned bits = operands[0].size;
        unsigned elementBits = instruction.mnemonic == ZYDIS_MNEMONIC_VPBROADCASTB ? 8
            : instruction.mnemonic == ZYDIS_MNEMONIC_VPBROADCASTW ? 16
            : (instruction.mnemonic == ZYDIS_MNEMONIC_VBROADCASTSD || instruction.mnemonic == ZYDIS_MNEMONIC_VPBROADCASTQ) ? 64 : 32;
        auto element = UnsignedType(elementBits);
        auto type = VectorType(element, bits);
        std::string scalar = operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY
            ? "Ld<" + element + ">(" + Address(decoded, operands[1]) + ")"
            : "ReadVec<" + element + ">(c, " + std::to_string(VectorIndex(operands[1].reg.value)) + ")";
        out << "    " << writeFull(type, "(" + type + "){} + (" + element + ")" + scalar) << "\n";
        return true;
    }
    case ZYDIS_MNEMONIC_VINSERTF128:
    case ZYDIS_MNEMONIC_VINSERTI128: {
        auto lane = operands[3].imm.value.u & 1;
        out << "    { Vec<u8, 32> v = ReadVec<Vec<u8, 32>>(c, " << VectorIndex(operands[1].reg.value) << "); Vec<u8, 16> s = "
            << ReadVector(decoded, 2, "Vec<u8, 16>") << "; std::memcpy((u8*)&v + " << lane * 16
            << ", &s, 16); WriteVec<Vec<u8, 32>>(c, " << destinationIndex() << ", v, true); }\n";
        return true;
    }
    case ZYDIS_MNEMONIC_VEXTRACTF128:
    case ZYDIS_MNEMONIC_VEXTRACTI128: {
        auto lane = operands[2].imm.value.u & 1;
        std::string value = "ReadVec<Vec<u8, 32>>(c, " + std::to_string(VectorIndex(operands[1].reg.value)) + ")";
        if (operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            out << "    { Vec<u8, 32> v = " << value << "; Vec<u8, 16> h; std::memcpy(&h, (u8*)&v + " << lane * 16 << ", 16); St<Vec<u8, 16>>("
                << Address(decoded, operands[0]) << ", h); }\n";
        } else {
            out << "    { Vec<u8, 32> v = " << value << "; Vec<u8, 16> h; std::memcpy(&h, (u8*)&v + " << lane * 16
                << ", 16); WriteVec<Vec<u8, 16>>(c, " << destinationIndex() << ", h, true); }\n";
        }
        return true;
    }
    case ZYDIS_MNEMONIC_VFMADD231PS:
    case ZYDIS_MNEMONIC_VFMADD213PS:
    case ZYDIS_MNEMONIC_VFMADD132PS:
    case ZYDIS_MNEMONIC_VFMADD231PD:
    case ZYDIS_MNEMONIC_VFMADD213PD:
    case ZYDIS_MNEMONIC_VFMADD132PD:
    case ZYDIS_MNEMONIC_VFMADD231SS:
    case ZYDIS_MNEMONIC_VFMADD213SS:
    case ZYDIS_MNEMONIC_VFMADD132SS:
    case ZYDIS_MNEMONIC_VFMADD231SD:
    case ZYDIS_MNEMONIC_VFMADD213SD:
    case ZYDIS_MNEMONIC_VFMADD132SD: {
        std::string name = Mnemonic(instruction);
        std::string suffix = name.substr(name.size() - 2);
        bool scalar = suffix == "ss" || suffix == "sd";
        bool isDouble = name.back() == 'd';
        std::string order = name.substr(6, 3);
        std::string element = isDouble ? "double" : "float";
        if (scalar) {
            std::string third = operands[2].type == ZYDIS_OPERAND_TYPE_MEMORY
                ? "Ld<" + element + ">(" + Address(decoded, operands[2]) + ")"
                : "ReadVec<" + element + ">(c, " + std::to_string(VectorIndex(operands[2].reg.value)) + ")";
            std::string x1 = "ReadVec<" + element + ">(c, " + destinationIndex() + ")";
            std::string x2 = "ReadVec<" + element + ">(c, " + std::to_string(VectorIndex(operands[1].reg.value)) + ")";
            std::string expression = order == "231" ? "std::fma(" + x2 + ", " + third + ", " + x1 + ")"
                : order == "213" ? "std::fma(" + x2 + ", " + x1 + ", " + third + ")"
                : "std::fma(" + x1 + ", " + third + ", " + x2 + ")";
            out << "    { Vec<u8, 16> base = ReadVec<Vec<u8, 16>>(c, " << destinationIndex() << "); " << element << " r = "
                << expression << "; std::memcpy(&base, &r, sizeof(r)); WriteVec<Vec<u8, 16>>(c, " << destinationIndex()
                << ", base, true); }\n";
            return true;
        }
        unsigned bits = operands[0].size;
        auto type = VectorType(isDouble ? "f64" : "f32", bits);
        std::string x1 = "ReadVec<" + type + ">(c, " + destinationIndex() + ")";
        std::string x2 = "ReadVec<" + type + ">(c, " + std::to_string(VectorIndex(operands[1].reg.value)) + ")";
        std::string x3 = ReadVector(decoded, 2, type);
        std::string expression = order == "231" ? "FmaVec(" + x2 + ", " + x3 + ", " + x1 + ")"
            : order == "213" ? "FmaVec(" + x2 + ", " + x1 + ", " + x3 + ")" : "FmaVec(" + x1 + ", " + x3 + ", " + x2 + ")";
        out << "    " << writeFull(type, expression) << "\n";
        return true;
    }
    default:
        return false;
    }
}

}
