#include "Discovery.hpp"
#include <algorithm>
#include <deque>
#include <sstream>
#include <stdexcept>

namespace translator {

namespace {

constexpr std::uint8_t SttFunc = 2;
constexpr std::uint32_t RX8664Relative = 8;

std::string Hex(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << value;
    return stream.str();
}

ZydisRegister Enclosing(ZydisRegister reg) {
    return ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
}

bool IsUnconditionalJump(const ZydisDecodedInstruction& instruction) {
    return instruction.mnemonic == ZYDIS_MNEMONIC_JMP;
}

bool IsConditionalJump(const ZydisDecodedInstruction& instruction) {
    return instruction.meta.category == ZYDIS_CATEGORY_COND_BR;
}

std::optional<std::uint64_t> DirectTarget(const DecodedInstruction& decoded) {
    const auto& operand = decoded.operands[0];
    if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative) {
        return decoded.Next() + operand.imm.value.u;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> RipRelativeAddress(const DecodedInstruction& decoded, const ZydisDecodedOperand& operand) {
    if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_RIP &&
        operand.mem.index == ZYDIS_REGISTER_NONE) {
        return decoded.Next() + operand.mem.disp.value;
    }
    return std::nullopt;
}

}

bool IsBlockTerminator(const ZydisDecodedInstruction& instruction) {
    switch (instruction.mnemonic) {
    case ZYDIS_MNEMONIC_JMP:
    case ZYDIS_MNEMONIC_RET:
    case ZYDIS_MNEMONIC_UD2:
    case ZYDIS_MNEMONIC_HLT:
    case ZYDIS_MNEMONIC_INT3:
        return true;
    default:
        return instruction.meta.category == ZYDIS_CATEGORY_COND_BR;
    }
}

Discovery::Discovery(const ElfImage& image) : _image(image), _exceptions(ReadExceptionTables(image)) {
    ZydisDecoderInit(&_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
}

bool Discovery::Decode(std::uint64_t address, DecodedInstruction& out) const {
    std::size_t available = 0;
    const auto* code = _image.CodeAt(address, available);
    if (code == nullptr) {
        return false;
    }
    out.address = address;
    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&_decoder, code, available, &out.instruction, out.operands));
}

std::set<std::uint64_t> Discovery::InitialEntries() const {
    std::set<std::uint64_t> entries;
    if (_image.IsExecutable(_image.Entry())) {
        entries.insert(_image.Entry());
    }
    for (const auto& symbol : _image.Symbols()) {
        if (symbol.defined && symbol.type == SttFunc && _image.IsExecutable(symbol.value)) {
            entries.insert(symbol.value);
        }
    }
    for (const auto& symbol : _image.DynamicSymbols()) {
        if (symbol.defined && symbol.value != 0 && (symbol.type == SttFunc || symbol.type == 0) && _image.IsExecutable(symbol.value)) {
            entries.insert(symbol.value);
        }
    }
    if (_image.Init() != 0 && _image.IsExecutable(_image.Init())) {
        entries.insert(_image.Init());
    }
    entries.insert(_exceptions.functionStarts.begin(), _exceptions.functionStarts.end());
    const auto& dynamic = _image.DynamicSymbols();
    for (const auto& relocation : _image.Relocations()) {
        if (relocation.type == RX8664Relative && _image.IsExecutable(static_cast<std::uint64_t>(relocation.addend))) {
            entries.insert(static_cast<std::uint64_t>(relocation.addend));
        } else if (relocation.symbolIndex != 0 && relocation.symbolIndex < dynamic.size()) {
            const auto& symbol = dynamic[relocation.symbolIndex];
            const auto target = symbol.value + static_cast<std::uint64_t>(relocation.addend);
            if (symbol.defined && symbol.value != 0 && _image.IsExecutable(target)) {
                entries.insert(target);
            }
        }
    }
    return entries;
}

Program Discovery::Run() {
    Program program;
    program.functionEntries = InitialEntries();
    std::set<std::uint64_t> pending = program.functionEntries;
    while (!pending.empty()) {
        std::set<std::uint64_t> discovered;
        std::vector<std::string> ignored;
        for (auto entry : pending) {
            ExploreFunction(entry, program.functionEntries, discovered, ignored);
        }
        pending.clear();
        for (auto entry : discovered) {
            if (program.functionEntries.insert(entry).second) {
                pending.insert(entry);
            }
        }
    }
    std::set<std::uint64_t> unused;
    for (auto entry : program.functionEntries) {
        program.functions.emplace(entry, ExploreFunction(entry, program.functionEntries, unused, program.warnings));
    }
    std::map<std::uint64_t, std::string> names;
    for (const auto& symbol : _image.Symbols()) {
        if (symbol.defined && symbol.type == SttFunc && !symbol.name.empty()) {
            names.emplace(symbol.value, symbol.name);
        }
    }
    for (auto& [entry, function] : program.functions) {
        auto it = names.find(entry);
        function.name = it != names.end() ? it->second : "sub_" + Hex(entry);
    }
    return program;
}

TranslatedFunctionInfo Discovery::ExploreFunction(std::uint64_t entry, const std::set<std::uint64_t>& knownEntries,
    std::set<std::uint64_t>& newEntries, std::vector<std::string>& warnings) const {
    std::map<std::uint64_t, DecodedInstruction> instructions;
    std::set<std::uint64_t> leaders{entry};
    std::map<std::uint64_t, std::vector<std::uint64_t>> tables;
    std::deque<std::uint64_t> worklist{entry};
    std::set<std::uint64_t> landingPads;
    if (auto pads = _exceptions.landingPads.find(entry); pads != _exceptions.landingPads.end()) {
        for (auto pad : pads->second) {
            if (_image.IsExecutable(pad)) {
                landingPads.insert(pad);
                leaders.insert(pad);
                worklist.push_back(pad);
            }
        }
    }
    while (!worklist.empty()) {
        auto address = worklist.front();
        worklist.pop_front();
        while (instructions.count(address) == 0) {
            DecodedInstruction decoded;
            if (!Decode(address, decoded)) {
                warnings.push_back("Cannot decode instruction at 0x" + Hex(address) + " in function 0x" + Hex(entry));
                break;
            }
            instructions.emplace(address, decoded);
            const auto& instruction = decoded.instruction;
            if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL) {
                if (auto target = DirectTarget(decoded); target && _image.IsExecutable(*target)) {
                    newEntries.insert(*target);
                }
                leaders.insert(decoded.Next());
            }
            if (instruction.mnemonic == ZYDIS_MNEMONIC_LEA) {
                if (auto target = RipRelativeAddress(decoded, decoded.operands[1]); target && _image.IsExecutable(*target)) {
                    newEntries.insert(*target);
                }
            }
            if (!IsBlockTerminator(instruction)) {
                address = decoded.Next();
                continue;
            }
            if (IsConditionalJump(instruction)) {
                auto target = DirectTarget(decoded);
                for (auto next : {*target, decoded.Next()}) {
                    leaders.insert(next);
                    worklist.push_back(next);
                }
            } else if (IsUnconditionalJump(instruction)) {
                if (auto target = DirectTarget(decoded)) {
                    if (!knownEntries.count(*target) || *target == entry) {
                        leaders.insert(*target);
                        worklist.push_back(*target);
                    }
                } else {
                    std::vector<DecodedInstruction> path;
                    for (auto it = instructions.find(address); path.size() < 16; --it) {
                        path.push_back(it->second);
                        if (it == instructions.begin()) {
                            break;
                        }
                    }
                    std::vector<std::uint64_t> targets;
                    if (RecoverJumpTable(path, decoded, instructions, targets)) {
                        tables[address] = targets;
                        for (auto target : targets) {
                            leaders.insert(target);
                            worklist.push_back(target);
                        }
                    }
                }
            }
            break;
        }
    }

    TranslatedFunctionInfo function;
    function.entry = entry;
    function.landingPads = landingPads;
    BasicBlock* current = nullptr;
    std::uint64_t expected = 0;
    for (const auto& [address, decoded] : instructions) {
        if (current == nullptr || leaders.count(address) != 0 || address != expected) {
            if (current != nullptr && address == expected && !current->instructions.empty() &&
                !IsBlockTerminator(current->instructions.back().instruction)) {
                current->successors.push_back(address);
            }
            current = &function.blocks.emplace(address, BasicBlock{address, {}, {}, {}, {}, false}).first->second;
        }
        current->addresses.push_back(address);
        current->instructions.push_back(decoded);
        expected = decoded.Next();
        const auto& instruction = decoded.instruction;
        if (IsBlockTerminator(instruction)) {
            if (IsConditionalJump(instruction)) {
                current->successors.push_back(*DirectTarget(decoded));
                current->successors.push_back(decoded.Next());
            } else if (IsUnconditionalJump(instruction)) {
                auto target = DirectTarget(decoded);
                if (target && leaders.count(*target) != 0) {
                    current->successors.push_back(*target);
                }
                if (auto table = tables.find(address); table != tables.end()) {
                    current->jumpTableRecovered = true;
                    current->jumpTableTargets = table->second;
                    current->successors.insert(current->successors.end(), table->second.begin(), table->second.end());
                }
            }
            current = nullptr;
        }
    }
    for (auto& [start, block] : function.blocks) {
        block.instructions.clear();
        block.instructions.shrink_to_fit();
    }
    return function;
}

TranslatedFunctionInfo Discovery::Materialize(const TranslatedFunctionInfo& function) const {
    TranslatedFunctionInfo copy = function;
    for (auto& [start, block] : copy.blocks) {
        block.instructions.reserve(block.addresses.size());
        for (auto address : block.addresses) {
            DecodedInstruction decoded;
            if (!Decode(address, decoded)) {
                throw std::runtime_error("Cannot decode instruction at 0x" + Hex(address));
            }
            block.instructions.push_back(decoded);
        }
    }
    return copy;
}

bool Discovery::RecoverJumpTable(const std::vector<DecodedInstruction>& path, const DecodedInstruction& jump,
    const std::map<std::uint64_t, DecodedInstruction>& function, std::vector<std::uint64_t>& targets) const {
    const auto& operand = jump.operands[0];
    ZydisRegister indexRegister = ZYDIS_REGISTER_NONE;
    std::uint64_t table = 0;
    bool relative = false;
    std::size_t searchFrom = 1;
    if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_NONE && operand.mem.scale == 8) {
        indexRegister = Enclosing(operand.mem.index);
        table = static_cast<std::uint64_t>(operand.mem.disp.value);
    } else if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        auto jumpRegister = Enclosing(operand.reg.value);
        ZydisRegister baseRegister = ZYDIS_REGISTER_NONE;
        for (; searchFrom < path.size(); ++searchFrom) {
            const auto& candidate = path[searchFrom];
            if (candidate.instruction.mnemonic == ZYDIS_MNEMONIC_MOVSXD &&
                Enclosing(candidate.operands[0].reg.value) == jumpRegister &&
                candidate.operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && candidate.operands[1].mem.scale == 4) {
                baseRegister = Enclosing(candidate.operands[1].mem.base);
                indexRegister = Enclosing(candidate.operands[1].mem.index);
                break;
            }
        }
        if (baseRegister == ZYDIS_REGISTER_NONE) {
            return false;
        }
        for (std::size_t i = 1; i < path.size(); ++i) {
            const auto& candidate = path[i];
            if (candidate.instruction.mnemonic == ZYDIS_MNEMONIC_LEA &&
                Enclosing(candidate.operands[0].reg.value) == baseRegister) {
                auto address = RipRelativeAddress(candidate, candidate.operands[1]);
                if (!address) {
                    return false;
                }
                table = *address;
                relative = true;
                break;
            }
        }
        if (!relative) {
            std::set<std::uint64_t> bases;
            for (const auto& [address, candidate] : function) {
                if (candidate.instruction.mnemonic != ZYDIS_MNEMONIC_LEA || Enclosing(candidate.operands[0].reg.value) != baseRegister) {
                    continue;
                }
                if (auto lea = RipRelativeAddress(candidate, candidate.operands[1])) bases.insert(*lea);
            }
            if (bases.size() != 1) {
                return false;
            }
            table = *bases.begin();
            relative = true;
        }
    } else {
        return false;
    }
    std::optional<std::uint64_t> count;
    for (std::size_t i = searchFrom; i < path.size(); ++i) {
        const auto& candidate = path[i];
        if ((candidate.instruction.mnemonic == ZYDIS_MNEMONIC_MOV || candidate.instruction.mnemonic == ZYDIS_MNEMONIC_MOVZX) &&
            candidate.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            candidate.operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            Enclosing(candidate.operands[0].reg.value) == indexRegister) {
            indexRegister = Enclosing(candidate.operands[1].reg.value);
            continue;
        }
        if (candidate.instruction.mnemonic == ZYDIS_MNEMONIC_CMP &&
            candidate.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            Enclosing(candidate.operands[0].reg.value) == indexRegister &&
            candidate.operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            count = candidate.operands[1].imm.value.u + 1;
            break;
        }
        if (candidate.instruction.mnemonic == ZYDIS_MNEMONIC_BEXTR && candidate.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            Enclosing(candidate.operands[0].reg.value) == indexRegister) {
            std::optional<std::uint64_t> control;
            if (candidate.operands[2].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                control = candidate.operands[2].imm.value.u;
            } else if (candidate.operands[2].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                const auto controlRegister = Enclosing(candidate.operands[2].reg.value);
                for (std::size_t j = i + 1; j < path.size(); ++j) {
                    const auto& source = path[j];
                    if (source.instruction.mnemonic == ZYDIS_MNEMONIC_MOV && source.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                        Enclosing(source.operands[0].reg.value) == controlRegister) {
                        if (source.operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) control = source.operands[1].imm.value.u;
                        break;
                    }
                }
            }
            if (control) {
                const auto length = (*control >> 8) & 0xff;
                if (length > 0 && length <= 12) {
                    count = std::uint64_t(1) << length;
                    break;
                }
            }
            return false;
        }
        if (candidate.instruction.mnemonic == ZYDIS_MNEMONIC_AND &&
            candidate.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            Enclosing(candidate.operands[0].reg.value) == indexRegister &&
            candidate.operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            const auto mask = candidate.operands[1].imm.value.u & 0xffffffffull;
            if (mask != 0 && (mask & (mask + 1)) == 0) {
                count = mask + 1;
                break;
            }
        }
    }
    if (!count && relative) {
        auto range = _exceptions.functionRanges.upper_bound(jump.address);
        if (range == _exceptions.functionRanges.begin() || jump.address >= std::prev(range)->second) {
            return false;
        }
        --range;
        std::set<ZydisRegister> copies{indexRegister};
        for (bool grew = true; grew;) {
            grew = false;
            for (const auto& [address, candidate] : function) {
                const auto mnemonic = candidate.instruction.mnemonic;
                if ((mnemonic != ZYDIS_MNEMONIC_MOV && mnemonic != ZYDIS_MNEMONIC_MOVZX) ||
                    candidate.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER || candidate.operands[1].type != ZYDIS_OPERAND_TYPE_REGISTER) {
                    continue;
                }
                const auto destination = Enclosing(candidate.operands[0].reg.value);
                const auto source = Enclosing(candidate.operands[1].reg.value);
                if (copies.count(destination) != 0 && copies.insert(source).second) grew = true;
                if (copies.count(source) != 0 && copies.insert(destination).second) grew = true;
            }
        }
        std::optional<std::uint64_t> bound;
        for (auto it = function.begin(); it != function.end() && it->first < jump.address; ++it) {
            const auto& candidate = it->second;
            if (candidate.instruction.mnemonic != ZYDIS_MNEMONIC_CMP || candidate.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                candidate.operands[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || copies.count(Enclosing(candidate.operands[0].reg.value)) == 0) {
                continue;
            }
            const auto next = function.find(candidate.Next());
            if (next == function.end()) continue;
            const auto immediate = candidate.operands[1].imm.value.u & 0xffffffffull;
            const auto branch = DirectTarget(next->second);
            const bool intoTable = branch && *branch <= jump.address && jump.address - *branch <= 32;
            if (next->second.instruction.mnemonic == ZYDIS_MNEMONIC_JNBE) bound = immediate + 1;
            else if (next->second.instruction.mnemonic == ZYDIS_MNEMONIC_JNB) bound = immediate;
            else if (next->second.instruction.mnemonic == ZYDIS_MNEMONIC_JBE && intoTable) bound = immediate + 1;
            else if (next->second.instruction.mnemonic == ZYDIS_MNEMONIC_JB && intoTable) bound = immediate;
        }
        if (!bound || *bound < 2 || *bound > 4096) {
            return false;
        }
        for (std::uint64_t i = 0; i < *bound; ++i) {
            auto entry = _image.ReadDword(table + i * 4);
            DecodedInstruction decoded;
            if (!entry) return false;
            const auto target = table + static_cast<std::int64_t>(*entry);
            if (target < range->first || target >= range->second || !Decode(target, decoded)) return false;
        }
        count = bound;
    }
    if (!count || *count > 4096) {
        return false;
    }
    std::map<std::uint64_t, std::int64_t> relocated;
    for (const auto& relocation : _image.Relocations()) {
        if (relocation.type == RX8664Relative) {
            relocated.emplace(relocation.offset, relocation.addend);
        }
    }
    for (std::uint64_t i = 0; i < *count; ++i) {
        std::uint64_t target = 0;
        if (relative) {
            auto entry = _image.ReadDword(table + i * 4);
            if (!entry) {
                return false;
            }
            target = table + static_cast<std::int64_t>(*entry);
        } else if (auto it = relocated.find(table + i * 8); it != relocated.end()) {
            target = static_cast<std::uint64_t>(it->second);
        } else if (auto entry = _image.ReadQword(table + i * 8)) {
            target = *entry;
        } else {
            return false;
        }
        if (!_image.IsExecutable(target)) {
            return false;
        }
        targets.push_back(target);
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    return true;
}

}
