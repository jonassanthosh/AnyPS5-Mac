#ifndef CORE_TRANSLATOR_SRC_EMITTER_HPP
#define CORE_TRANSLATOR_SRC_EMITTER_HPP

#include "Discovery.hpp"
#include "ElfImage.hpp"
#include <map>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace translator {

struct EmitOptions {
    bool conservativeFlags = false;
};

struct EmitStatistics {
    std::size_t instructions = 0;
    std::size_t translated = 0;
    std::map<std::string, std::size_t> unsupported;
    std::size_t indirectJumps = 0;
    std::size_t recoveredJumpTables = 0;
    std::size_t indirectThroughRipMemory = 0;
    std::size_t indirectThroughOtherMemory = 0;
    std::size_t indirectThroughRegister = 0;
    std::size_t indirectTableShaped = 0;
    std::size_t flagWrites = 0;
    std::size_t flagWritesElided = 0;
    std::size_t coveredBytes = 0;
    std::size_t executableBytes = 0;
};

class Emitter {
public:
    Emitter(const ElfImage& image, const Discovery& discovery, const Program& program, EmitOptions options);

    void Emit(const std::vector<std::ostream*>& shards);
    const EmitStatistics& Statistics() const { return _statistics; }

private:
    using FlagMask = std::uint32_t;

    void EmitFunction(std::ostream& out, const TranslatedFunctionInfo& function);
    std::map<std::uint64_t, std::vector<FlagMask>> ComputeLiveFlags(const TranslatedFunctionInfo& function) const;
    bool EmitInstruction(std::ostream& out, const TranslatedFunctionInfo& function, const BasicBlock& block,
        const DecodedInstruction& decoded, FlagMask liveAfter);
    bool EmitInteger(std::ostream& out, const DecodedInstruction& decoded, FlagMask liveAfter);
    bool EmitVector(std::ostream& out, const DecodedInstruction& decoded);
    bool EmitVectorExtended(std::ostream& out, const DecodedInstruction& decoded);
    bool EmitControl(std::ostream& out, const TranslatedFunctionInfo& function, const BasicBlock& block,
        const DecodedInstruction& decoded);

    std::string Address(const DecodedInstruction& decoded, const ZydisDecodedOperand& operand) const;
    std::string Read(const DecodedInstruction& decoded, int index) const;
    std::string Read(const DecodedInstruction& decoded, int index, unsigned bits) const;
    std::string Write(const DecodedInstruction& decoded, int index, const std::string& value) const;
    std::string ReadVector(const DecodedInstruction& decoded, int index, const std::string& type) const;
    std::string Load(const ZydisDecodedOperand& operand, const std::string& type) const;
    std::string Store(const ZydisDecodedOperand& operand, const std::string& type) const;
    std::string FunctionName(std::uint64_t entry) const;
    std::string Absolute(std::uint64_t vaddr) const;
    std::string CallTo(const std::string& target, std::uint64_t returnAddress) const;

    const ElfImage& _image;
    const Discovery& _discovery;
    const Program& _program;
    EmitOptions _options;
    EmitStatistics _statistics;
};

}

#endif
