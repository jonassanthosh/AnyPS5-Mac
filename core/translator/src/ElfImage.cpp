#include "ElfImage.hpp"
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>

namespace translator {

namespace {

struct Elf64Header {
    std::uint8_t ident[16];
    std::uint16_t type;
    std::uint16_t machine;
    std::uint32_t version;
    std::uint64_t entry;
    std::uint64_t phoff;
    std::uint64_t shoff;
    std::uint32_t flags;
    std::uint16_t ehsize;
    std::uint16_t phentsize;
    std::uint16_t phnum;
    std::uint16_t shentsize;
    std::uint16_t shnum;
    std::uint16_t shstrndx;
};

struct Elf64ProgramHeader {
    std::uint32_t type;
    std::uint32_t flags;
    std::uint64_t offset;
    std::uint64_t vaddr;
    std::uint64_t paddr;
    std::uint64_t filesz;
    std::uint64_t memsz;
    std::uint64_t align;
};

struct Elf64SectionHeader {
    std::uint32_t name;
    std::uint32_t type;
    std::uint64_t flags;
    std::uint64_t addr;
    std::uint64_t offset;
    std::uint64_t size;
    std::uint32_t link;
    std::uint32_t info;
    std::uint64_t addralign;
    std::uint64_t entsize;
};

struct Elf64Sym {
    std::uint32_t name;
    std::uint8_t info;
    std::uint8_t other;
    std::uint16_t shndx;
    std::uint64_t value;
    std::uint64_t size;
};

struct Elf64Rela {
    std::uint64_t offset;
    std::uint64_t info;
    std::int64_t addend;
};

constexpr std::uint32_t PtLoad = 1;
constexpr std::uint32_t PtDynamic = 2;
constexpr std::uint32_t PtSceDynlibData = 0x61000000;
constexpr std::int64_t DtNull = 0;
constexpr std::int64_t DtPltRelSz = 2;
constexpr std::int64_t DtStrTab = 5;
constexpr std::int64_t DtSymTab = 6;
constexpr std::int64_t DtRela = 7;
constexpr std::int64_t DtRelaSz = 8;
constexpr std::int64_t DtStrSz = 10;
constexpr std::int64_t DtJmpRel = 23;
constexpr std::int64_t DtOsJmpRel = 0x61000029;
constexpr std::int64_t DtOsPltRelSz = 0x6100002d;
constexpr std::int64_t DtOsRela = 0x6100002f;
constexpr std::int64_t DtOsRelaSz = 0x61000031;
constexpr std::int64_t DtOsStrTab = 0x61000035;
constexpr std::int64_t DtOsStrSz = 0x61000037;
constexpr std::int64_t DtOsSymTab = 0x61000039;
constexpr std::int64_t DtOsSymTabSz = 0x6100003f;
constexpr std::uint32_t ShtSymtab = 2;
constexpr std::uint32_t ShtRela = 4;
constexpr std::uint32_t ShtDynsym = 11;
constexpr std::uint16_t EmX8664 = 62;
constexpr std::uint32_t PfExecute = 1;

template <typename T>
T ReadAt(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    if (offset + sizeof(T) > bytes.size()) {
        throw std::runtime_error("ELF read out of range at offset " + std::to_string(offset));
    }
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

std::vector<ElfSymbol> ReadSymbols(const std::vector<std::uint8_t>& bytes, const Elf64SectionHeader& table,
    const Elf64SectionHeader& strings) {
    std::vector<ElfSymbol> symbols;
    for (std::uint64_t offset = 0; offset + sizeof(Elf64Sym) <= table.size; offset += sizeof(Elf64Sym)) {
        auto raw = ReadAt<Elf64Sym>(bytes, table.offset + offset);
        std::string name;
        if (raw.name < strings.size) {
            name = reinterpret_cast<const char*>(bytes.data() + strings.offset + raw.name);
        }
        symbols.push_back({std::move(name), raw.value, raw.size, static_cast<std::uint8_t>(raw.info & 0xf), raw.shndx != 0});
    }
    return symbols;
}

}

ElfImage ElfImage::Load(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open " + path.string());
    }
    ElfImage image;
    image._bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    auto header = ReadAt<Elf64Header>(image._bytes, 0);
    if (std::memcmp(header.ident, "\x7f" "ELF", 4) != 0 || header.ident[4] != 2 || header.machine != EmX8664) {
        throw std::runtime_error(path.string() + " is not an x86-64 ELF64 image");
    }
    image._entry = header.entry;
    for (std::uint16_t i = 0; i < header.phnum; ++i) {
        auto ph = ReadAt<Elf64ProgramHeader>(image._bytes, header.phoff + i * sizeof(Elf64ProgramHeader));
        if (ph.type == PtLoad) {
            image._segments.push_back({ph.vaddr, ph.memsz, ph.offset, ph.filesz, ph.flags});
        }
    }
    std::optional<std::uint64_t> dynamicOffset;
    std::uint64_t dynamicSize = 0;
    std::optional<std::uint64_t> dynlibOffset;
    for (std::uint16_t i = 0; i < header.phnum; ++i) {
        auto ph = ReadAt<Elf64ProgramHeader>(image._bytes, header.phoff + i * sizeof(Elf64ProgramHeader));
        if (ph.type == PtDynamic) {
            dynamicOffset = ph.offset;
            dynamicSize = ph.filesz;
        } else if (ph.type == PtSceDynlibData) {
            dynlibOffset = ph.offset;
        }
    }
    bool sectionsPresent = header.shnum != 0 &&
        header.shoff + static_cast<std::uint64_t>(header.shnum) * sizeof(Elf64SectionHeader) <= image._bytes.size();
    if (sectionsPresent) {
        std::vector<Elf64SectionHeader> sections;
        for (std::uint16_t i = 0; i < header.shnum; ++i) {
            sections.push_back(ReadAt<Elf64SectionHeader>(image._bytes, header.shoff + i * sizeof(Elf64SectionHeader)));
        }
        for (const auto& section : sections) {
            if ((section.type == ShtSymtab || section.type == ShtDynsym) && section.link < sections.size()) {
                auto symbols = ReadSymbols(image._bytes, section, sections[section.link]);
                (section.type == ShtSymtab ? image._symbols : image._dynamicSymbols) = std::move(symbols);
            }
        }
        for (const auto& section : sections) {
            if (section.type != ShtRela) {
                continue;
            }
            image.ReadRelocations(section.offset, section.size);
        }
    } else if (dynamicOffset) {
        std::map<std::int64_t, std::uint64_t> tags;
        for (std::uint64_t offset = 0; offset + 16 <= dynamicSize; offset += 16) {
            auto tag = ReadAt<std::int64_t>(image._bytes, *dynamicOffset + offset);
            auto value = ReadAt<std::uint64_t>(image._bytes, *dynamicOffset + offset + 8);
            if (tag == DtNull) {
                break;
            }
            tags.emplace(tag, value);
        }
        bool sce = tags.count(DtOsRela) != 0 || tags.count(DtOsSymTab) != 0;
        if (sce && !dynlibOffset) {
            throw std::runtime_error("PS5 dynamic tags without a PT_SCE_DYNLIBDATA segment");
        }
        auto fileOffset = [&](std::uint64_t value) -> std::uint64_t {
            if (sce) {
                return *dynlibOffset + value;
            }
            auto offset = image.FileOffsetOf(value);
            if (!offset) {
                throw std::runtime_error("Dynamic table address is outside the loaded segments");
            }
            return *offset;
        };
        auto tag = [&](std::int64_t sceTag, std::int64_t standardTag) -> std::optional<std::uint64_t> {
            if (auto it = tags.find(sce ? sceTag : standardTag); it != tags.end()) {
                return it->second;
            }
            return std::nullopt;
        };
        auto symbolTable = tag(DtOsSymTab, DtSymTab);
        auto stringTable = tag(DtOsStrTab, DtStrTab);
        auto stringSize = tag(DtOsStrSz, DtStrSz);
        auto symbolSize = tags.count(DtOsSymTabSz) != 0 ? std::optional<std::uint64_t>(tags.at(DtOsSymTabSz)) : std::nullopt;
        if (symbolTable && stringTable && stringSize && symbolSize) {
            Elf64SectionHeader table{};
            table.offset = fileOffset(*symbolTable);
            table.size = *symbolSize;
            Elf64SectionHeader strings{};
            strings.offset = fileOffset(*stringTable);
            strings.size = *stringSize;
            image._dynamicSymbols = ReadSymbols(image._bytes, table, strings);
        }
        if (auto rela = tag(DtOsRela, DtRela)) {
            image.ReadRelocations(fileOffset(*rela), tag(DtOsRelaSz, DtRelaSz).value_or(0));
        }
        if (auto jmprel = tag(DtOsJmpRel, DtJmpRel)) {
            image.ReadRelocations(fileOffset(*jmprel), tag(DtOsPltRelSz, DtPltRelSz).value_or(0));
        }
    }
    return image;
}

void ElfImage::ReadRelocations(std::uint64_t fileOffset, std::uint64_t size) {
    for (std::uint64_t offset = 0; offset + sizeof(Elf64Rela) <= size; offset += sizeof(Elf64Rela)) {
        auto rela = ReadAt<Elf64Rela>(_bytes, fileOffset + offset);
        _relocations.push_back({rela.offset, static_cast<std::uint32_t>(rela.info & 0xffffffff),
            static_cast<std::uint32_t>(rela.info >> 32), rela.addend});
    }
}

std::uint64_t ElfImage::ImageSize() const {
    std::uint64_t end = 0;
    for (const auto& segment : _segments) {
        end = std::max(end, segment.vaddr + segment.memorySize);
    }
    return end;
}

bool ElfImage::IsExecutable(std::uint64_t vaddr) const {
    for (const auto& segment : _segments) {
        if (vaddr >= segment.vaddr && vaddr < segment.vaddr + segment.fileSize) {
            return (segment.flags & PfExecute) != 0;
        }
    }
    return false;
}

std::optional<std::uint64_t> ElfImage::FileOffsetOf(std::uint64_t vaddr) const {
    for (const auto& segment : _segments) {
        if (vaddr >= segment.vaddr && vaddr < segment.vaddr + segment.fileSize) {
            return segment.fileOffset + (vaddr - segment.vaddr);
        }
    }
    return std::nullopt;
}

const std::uint8_t* ElfImage::CodeAt(std::uint64_t vaddr, std::size_t& available) const {
    for (const auto& segment : _segments) {
        if (vaddr >= segment.vaddr && vaddr < segment.vaddr + segment.fileSize && (segment.flags & PfExecute) != 0) {
            available = segment.vaddr + segment.fileSize - vaddr;
            return _bytes.data() + segment.fileOffset + (vaddr - segment.vaddr);
        }
    }
    available = 0;
    return nullptr;
}

std::optional<std::uint64_t> ElfImage::ReadQword(std::uint64_t vaddr) const {
    auto offset = FileOffsetOf(vaddr);
    if (!offset || !FileOffsetOf(vaddr + 7)) {
        return std::nullopt;
    }
    return ReadAt<std::uint64_t>(_bytes, *offset);
}

std::optional<std::int32_t> ElfImage::ReadDword(std::uint64_t vaddr) const {
    auto offset = FileOffsetOf(vaddr);
    if (!offset || !FileOffsetOf(vaddr + 3)) {
        return std::nullopt;
    }
    return ReadAt<std::int32_t>(_bytes, *offset);
}

}
