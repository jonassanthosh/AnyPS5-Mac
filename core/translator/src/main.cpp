#include "Discovery.hpp"
#include "ElfImage.hpp"
#include "Emitter.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void PrintUsage() {
    std::cerr << "usage: x64a64 <input.elf> -o <output.cpp> [--shards <n>] [--conservative-flags] [--report]\n";
}

void PrintReport(const translator::Program& program, const translator::EmitStatistics& statistics) {
    std::size_t blocks = 0;
    for (const auto& [entry, function] : program.functions) {
        blocks += function.blocks.size();
    }
    double coverage = statistics.instructions == 0 ? 100.0
        : 100.0 * static_cast<double>(statistics.translated) / static_cast<double>(statistics.instructions);
    std::printf("functions:            %zu\n", program.functions.size());
    std::printf("basic blocks:         %zu\n", blocks);
    std::printf("instructions:         %zu\n", statistics.instructions);
    std::printf("translated:           %zu (%.2f%%)\n", statistics.translated, coverage);
    std::printf("code reached:         %zu of %zu executable bytes (%.2f%%)\n", statistics.coveredBytes,
        statistics.executableBytes, statistics.executableBytes == 0 ? 0.0
            : 100.0 * static_cast<double>(statistics.coveredBytes) / static_cast<double>(statistics.executableBytes));
    std::printf("indirect jumps:       %zu (%zu with recovered jump tables)\n", statistics.indirectJumps,
        statistics.recoveredJumpTables);
    std::printf("  unresolved via [rip]: %zu, via other memory: %zu, via register: %zu (%zu shaped like jump tables)\n",
        statistics.indirectThroughRipMemory, statistics.indirectThroughOtherMemory, statistics.indirectThroughRegister,
        statistics.indirectTableShaped);
    std::printf("flag writes elided:   %zu of %zu\n", statistics.flagWritesElided, statistics.flagWrites);
    std::vector<std::pair<std::string, std::size_t>> unsupported(statistics.unsupported.begin(), statistics.unsupported.end());
    std::sort(unsupported.begin(), unsupported.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (!unsupported.empty()) {
        std::printf("untranslated mnemonics:\n");
        for (const auto& [mnemonic, count] : unsupported) {
            std::printf("  %-20s %zu\n", mnemonic.c_str(), count);
        }
    }
    for (const auto& warning : program.warnings) {
        std::printf("warning: %s\n", warning.c_str());
    }
}

}

int main(int argc, char** argv) {
    std::string input;
    std::string output;
    bool report = false;
    unsigned shardCount = 1;
    translator::EmitOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string argument = argv[i];
        if (argument == "-o" && i + 1 < argc) {
            output = argv[++i];
        } else if (argument == "--report") {
            report = true;
        } else if (argument == "--shards" && i + 1 < argc) {
            shardCount = static_cast<unsigned>(std::stoul(argv[++i]));
        } else if (argument == "--conservative-flags") {
            options.conservativeFlags = true;
        } else if (input.empty()) {
            input = argument;
        } else {
            PrintUsage();
            return 2;
        }
    }
    if (input.empty() || output.empty()) {
        PrintUsage();
        return 2;
    }
    try {
        auto image = translator::ElfImage::Load(input);
        translator::Discovery discovery(image);
        auto program = discovery.Run();
        translator::Emitter emitter(image, discovery, program, options);
        std::vector<std::unique_ptr<std::ofstream>> streams;
        std::vector<std::ostream*> shards;
        for (unsigned i = 0; i < shardCount; ++i) {
            std::string path = shardCount == 1 ? output : output + "." + std::to_string(i) + ".cpp";
            streams.push_back(std::make_unique<std::ofstream>(path));
            if (!*streams.back()) {
                throw std::runtime_error("Cannot write " + path);
            }
            shards.push_back(streams.back().get());
        }
        emitter.Emit(shards);
        if (report) {
            PrintReport(program, emitter.Statistics());
        }
    } catch (const std::exception& error) {
        std::cerr << "x64a64: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
