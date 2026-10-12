#ifndef CORE_TRANSLATOR_SRC_ELFIMAGE_HPP
#define CORE_TRANSLATOR_SRC_ELFIMAGE_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace translator {

struct ElfSegment {
    std::uint64_t vaddr;
    std::uint64_t memorySize;
    std::uint64_t fileOffset;
    std::uint64_t fileSize;
    std::uint32_t flags;
};

struct ElfSymbol {
    std::string name;
    std::uint64_t value;
    std::uint64_t size;
    std::uint8_t type;
    bool defined;
};

struct ElfRelocation {
    std::uint64_t offset;
    std::uint32_t type;
    std::uint32_t symbolIndex;
    std::int64_t addend;
};

class ElfImage {
public:
    static ElfImage Load(const std::filesystem::path& path);

    const std::vector<std::uint8_t>& Bytes() const { return _bytes; }
    const std::vector<ElfSegment>& Segments() const { return _segments; }
    const std::vector<ElfSymbol>& Symbols() const { return _symbols; }
    const std::vector<ElfSymbol>& DynamicSymbols() const { return _dynamicSymbols; }
    const std::vector<ElfRelocation>& Relocations() const { return _relocations; }
    std::uint64_t Entry() const { return _entry; }
    std::uint64_t ImageSize() const;

    bool IsExecutable(std::uint64_t vaddr) const;
    std::optional<std::uint64_t> FileOffsetOf(std::uint64_t vaddr) const;
    const std::uint8_t* CodeAt(std::uint64_t vaddr, std::size_t& available) const;
    std::optional<std::uint64_t> ReadQword(std::uint64_t vaddr) const;
    std::optional<std::int32_t> ReadDword(std::uint64_t vaddr) const;

private:
    void ReadRelocations(std::uint64_t fileOffset, std::uint64_t size);

    std::vector<std::uint8_t> _bytes;
    std::vector<ElfSegment> _segments;
    std::vector<ElfSymbol> _symbols;
    std::vector<ElfSymbol> _dynamicSymbols;
    std::vector<ElfRelocation> _relocations;
    std::uint64_t _entry = 0;
};

}

#endif
