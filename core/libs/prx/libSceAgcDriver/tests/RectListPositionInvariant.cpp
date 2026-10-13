#include "Recompiler.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <spirv/unified1/spirv.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ShaderRecompiler;

void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool PositionIsInvariant(std::span<const std::uint32_t> words) {
    Check(words.size() >= 5 && words[0] == spv::MagicNumber, "rect-list invariant: missing SPIR-V module");
    std::vector<std::pair<std::uint32_t, std::uint32_t>> positionMembers;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> invariantMembers;
    for (std::size_t offset = 5; offset < words.size();) {
        const auto count = words[offset] >> spv::WordCountShift;
        Check(count != 0 && count <= words.size() - offset, "rect-list invariant: malformed SPIR-V instruction");
        const auto opcode = words[offset] & spv::OpCodeMask;
        if (opcode == spv::OpMemberDecorate && count == 5 && words[offset + 3] == spv::DecorationBuiltIn && words[offset + 4] == spv::BuiltInPosition) {
            positionMembers.emplace_back(words[offset + 1], words[offset + 2]);
        }
        if (opcode == spv::OpMemberDecorate && count == 4 && words[offset + 3] == spv::DecorationInvariant) {
            invariantMembers.emplace_back(words[offset + 1], words[offset + 2]);
        }
        offset += count;
    }
    Check(!positionMembers.empty(), "rect-list invariant: shader has no gl_Position");
    for (const auto& member : positionMembers) {
        if (std::find(invariantMembers.begin(), invariantMembers.end(), member) == invariantMembers.end()) return false;
    }
    return true;
}

std::uint32_t MemberDistanceCount(std::span<const std::uint32_t> words, spv::BuiltIn builtin) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> members;
    std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>> structs;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> arrays;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> constants;
    for (std::size_t offset = 5; offset < words.size();) {
        const auto count = words[offset] >> spv::WordCountShift;
        Check(count != 0 && count <= words.size() - offset, "rect-list distances: malformed SPIR-V instruction");
        const auto opcode = words[offset] & spv::OpCodeMask;
        if (opcode == spv::OpMemberDecorate && count == 5 && words[offset + 3] == spv::DecorationBuiltIn && words[offset + 4] == builtin) members.emplace_back(words[offset + 1], words[offset + 2]);
        if (opcode == spv::OpTypeStruct) structs.emplace_back(words[offset + 1], std::vector<std::uint32_t>(words.begin() + offset + 2, words.begin() + offset + count));
        if (opcode == spv::OpTypeArray && count == 4) arrays.emplace_back(words[offset + 1], words[offset + 3]);
        if (opcode == spv::OpConstant && count == 4) constants.emplace_back(words[offset + 2], words[offset + 3]);
        offset += count;
    }
    if (members.empty()) return 0;
    const auto find = [](const auto& list, std::uint32_t id) {
        const auto it = std::find_if(list.begin(), list.end(), [&](const auto& entry) { return entry.first == id; });
        Check(it != list.end(), "rect-list distances: missing SPIR-V declaration");
        return it->second;
    };
    const auto& [structId, member] = members.front();
    return find(constants, find(arrays, find(structs, structId).at(member)));
}

std::vector<std::uint32_t> VertexWithDistances() {
    const auto op = [](spv::Op opcode, std::uint32_t words) { return (words << spv::WordCountShift) | opcode; };
    return {
        spv::MagicNumber, 0x00010400u, 0u, 11u, 0u,
        op(spv::OpDecorate, 4), 9u, spv::DecorationBuiltIn, spv::BuiltInClipDistance,
        op(spv::OpDecorate, 4), 10u, spv::DecorationBuiltIn, spv::BuiltInCullDistance,
        op(spv::OpTypeFloat, 3), 1u, 32u,
        op(spv::OpTypeInt, 4), 2u, 32u, 0u,
        op(spv::OpConstant, 4), 2u, 3u, 2u,
        op(spv::OpConstant, 4), 2u, 4u, 1u,
        op(spv::OpTypeArray, 4), 5u, 1u, 3u,
        op(spv::OpTypeArray, 4), 6u, 1u, 4u,
        op(spv::OpTypePointer, 4), 7u, spv::StorageClassOutput, 5u,
        op(spv::OpTypePointer, 4), 8u, spv::StorageClassOutput, 6u,
        op(spv::OpVariable, 4), 7u, 9u, spv::StorageClassOutput,
        op(spv::OpVariable, 4), 8u, 10u, spv::StorageClassOutput,
    };
}

}

int main() {
    try {
        static constexpr std::array<std::uint32_t, 2> capabilities{spv::CapabilityShader, spv::CapabilityTessellation};
        SpirvTarget target{};
        target.vulkanVersion = 0x00401000u;
        target.spirvVersion = 0x00010400u;
        target.supportedCapabilities = capabilities;
        target.tessellation = TessellationTargetLimits{32, 128, 128, 120, 4096, 128, 128};
        RecompileResult vertex;
        RecompileResult fragment;
        const auto shaders = BuildRectListShaders(vertex, fragment, target);
        Check(PositionIsInvariant(shaders.control.spirv.Words()), "rect-list control gl_Position is not Invariant");
        Check(PositionIsInvariant(shaders.evaluation.spirv.Words()), "rect-list evaluation gl_Position is not Invariant");
        Check(MemberDistanceCount(shaders.control.spirv.Words(), spv::BuiltInClipDistance) == 0 && MemberDistanceCount(shaders.control.spirv.Words(), spv::BuiltInCullDistance) == 0, "rect-list control declares distances its vertex shader does not export");
        vertex.spirv = VertexWithDistances();
        bool rejected = false;
        try {
            static_cast<void>(BuildRectListShaders(vertex, fragment, target));
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        Check(rejected, "rect-list shaders were built without the clip and cull distance capabilities");
        static constexpr std::array<std::uint32_t, 4> distanceCapabilities{spv::CapabilityShader, spv::CapabilityTessellation, spv::CapabilityClipDistance, spv::CapabilityCullDistance};
        target.supportedCapabilities = distanceCapabilities;
        const auto distances = BuildRectListShaders(vertex, fragment, target);
        for (const auto& stage : {distances.control.spirv.Words(), distances.evaluation.spirv.Words()}) {
            Check(MemberDistanceCount(stage, spv::BuiltInClipDistance) == 2, "rect-list stage does not pass the vertex's two clip distances through");
            Check(MemberDistanceCount(stage, spv::BuiltInCullDistance) == 1, "rect-list stage does not pass the vertex's cull distance through");
        }
        std::printf("rect-list position invariant tests passed\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
