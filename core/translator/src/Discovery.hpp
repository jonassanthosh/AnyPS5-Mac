#ifndef CORE_TRANSLATOR_SRC_DISCOVERY_HPP
#define CORE_TRANSLATOR_SRC_DISCOVERY_HPP

#include "ElfImage.hpp"
#include <Zydis/Zydis.h>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace translator {

struct DecodedInstruction {
    std::uint64_t address;
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

    std::uint64_t Next() const { return address + instruction.length; }
};

struct BasicBlock {
    std::uint64_t start;
    std::vector<std::uint64_t> addresses;
    std::vector<DecodedInstruction> instructions;
    std::vector<std::uint64_t> successors;
    std::vector<std::uint64_t> jumpTableTargets;
    bool jumpTableRecovered = false;
};

struct TranslatedFunctionInfo {
    std::uint64_t entry;
    std::string name;
    std::map<std::uint64_t, BasicBlock> blocks;
};

struct Program {
    std::map<std::uint64_t, TranslatedFunctionInfo> functions;
    std::set<std::uint64_t> functionEntries;
    std::vector<std::string> warnings;
};

class Discovery {
public:
    explicit Discovery(const ElfImage& image);

    Program Run();
    TranslatedFunctionInfo Materialize(const TranslatedFunctionInfo& function) const;

private:
    bool Decode(std::uint64_t address, DecodedInstruction& out) const;
    TranslatedFunctionInfo ExploreFunction(std::uint64_t entry, const std::set<std::uint64_t>& knownEntries,
        std::set<std::uint64_t>& newEntries, std::vector<std::string>& warnings) const;
    bool RecoverJumpTable(const std::vector<DecodedInstruction>& path, const DecodedInstruction& jump,
        std::vector<std::uint64_t>& targets) const;
    std::set<std::uint64_t> InitialEntries() const;

    const ElfImage& _image;
    ZydisDecoder _decoder;
};

bool IsBlockTerminator(const ZydisDecodedInstruction& instruction);

}

#endif
