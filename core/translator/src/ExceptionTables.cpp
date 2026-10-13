#include "ExceptionTables.hpp"
#include <optional>
#include <string>

namespace translator {

namespace {

constexpr std::uint32_t PtGnuEhFrame = 0x6474e550;
constexpr std::uint8_t EncodingOmit = 0xff;

class Reader {
public:
    Reader(const ElfImage& image, std::uint64_t address) : _image(image), _address(address) {}

    std::uint64_t Address() const { return _address; }
    void Seek(std::uint64_t address) { _address = address; }
    bool Valid() const { return _valid; }

    std::uint64_t Unsigned(unsigned bytes) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < bytes; ++i) {
            value |= std::uint64_t(Byte()) << (8 * i);
        }
        return value;
    }

    std::int64_t Signed(unsigned bytes) {
        const auto value = Unsigned(bytes);
        const unsigned shift = 64 - bytes * 8;
        return static_cast<std::int64_t>(value << shift) >> shift;
    }

    std::uint64_t Uleb() {
        std::uint64_t value = 0;
        for (unsigned shift = 0; _valid && shift < 64; shift += 7) {
            const auto byte = Byte();
            value |= std::uint64_t(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) break;
        }
        return value;
    }

    std::int64_t Sleb() {
        std::int64_t value = 0;
        unsigned shift = 0;
        std::uint8_t byte = 0;
        do {
            byte = Byte();
            value |= std::int64_t(byte & 0x7f) << shift;
            shift += 7;
        } while (_valid && (byte & 0x80) != 0 && shift < 64);
        if (shift < 64 && (byte & 0x40) != 0) value |= -(std::int64_t(1) << shift);
        return value;
    }

    std::optional<std::uint64_t> Encoded(std::uint8_t encoding, std::uint64_t functionBase = 0) {
        if (encoding == EncodingOmit) return std::nullopt;
        const auto start = _address;
        std::uint64_t value = 0;
        switch (encoding & 0x0f) {
        case 0x00: value = Unsigned(8); break;
        case 0x01: value = Uleb(); break;
        case 0x02: value = Unsigned(2); break;
        case 0x03: value = Unsigned(4); break;
        case 0x04: value = Unsigned(8); break;
        case 0x09: value = static_cast<std::uint64_t>(Sleb()); break;
        case 0x0a: value = static_cast<std::uint64_t>(Signed(2)); break;
        case 0x0b: value = static_cast<std::uint64_t>(Signed(4)); break;
        case 0x0c: value = static_cast<std::uint64_t>(Signed(8)); break;
        default: _valid = false; return std::nullopt;
        }
        switch (encoding & 0x70) {
        case 0x00: break;
        case 0x10: value += start; break;
        case 0x40: value += functionBase; break;
        default: _valid = false; return std::nullopt;
        }
        if ((encoding & 0x80) != 0) {
            const auto target = _image.ReadQword(value);
            if (!target) {
                _valid = false;
                return std::nullopt;
            }
            value = *target;
        }
        return value;
    }

private:
    std::uint8_t Byte() {
        const auto offset = _image.FileOffsetOf(_address);
        if (!offset || *offset >= _image.Bytes().size()) {
            _valid = false;
            return 0;
        }
        ++_address;
        return _image.Bytes()[*offset];
    }

    const ElfImage& _image;
    std::uint64_t _address;
    bool _valid = true;
};

struct Cie {
    std::uint8_t pointerEncoding = 0;
    std::uint8_t lsdaEncoding = EncodingOmit;
    bool augmented = false;
};

std::optional<Cie> ReadCie(const ElfImage& image, std::uint64_t address) {
    Reader reader(image, address);
    const auto length = reader.Unsigned(4);
    if (length == 0 || length == 0xffffffff || reader.Unsigned(4) != 0) return std::nullopt;
    Cie cie;
    const auto version = reader.Unsigned(1);
    std::string augmentation;
    for (auto c = reader.Unsigned(1); c != 0 && reader.Valid(); c = reader.Unsigned(1)) {
        augmentation.push_back(static_cast<char>(c));
    }
    reader.Uleb();
    reader.Sleb();
    if (version == 1) reader.Unsigned(1);
    else reader.Uleb();
    if (augmentation.empty() || augmentation[0] != 'z') return reader.Valid() ? std::optional<Cie>(cie) : std::nullopt;
    cie.augmented = true;
    reader.Uleb();
    for (std::size_t i = 1; i < augmentation.size() && reader.Valid(); ++i) {
        switch (augmentation[i]) {
        case 'R': cie.pointerEncoding = static_cast<std::uint8_t>(reader.Unsigned(1)); break;
        case 'L': cie.lsdaEncoding = static_cast<std::uint8_t>(reader.Unsigned(1)); break;
        case 'P': reader.Encoded(static_cast<std::uint8_t>(reader.Unsigned(1))); break;
        case 'S': case 'B': break;
        default: return std::nullopt;
        }
    }
    return reader.Valid() ? std::optional<Cie>(cie) : std::nullopt;
}

void ReadLsda(const ElfImage& image, std::uint64_t lsda, std::uint64_t functionStart, std::set<std::uint64_t>& pads) {
    Reader reader(image, lsda);
    const auto lpStartEncoding = static_cast<std::uint8_t>(reader.Unsigned(1));
    const auto lpStart = lpStartEncoding != EncodingOmit ? reader.Encoded(lpStartEncoding).value_or(functionStart) : functionStart;
    if (static_cast<std::uint8_t>(reader.Unsigned(1)) != EncodingOmit) reader.Uleb();
    const auto callSiteEncoding = static_cast<std::uint8_t>(reader.Unsigned(1));
    const auto tableLength = reader.Uleb();
    const auto end = reader.Address() + tableLength;
    while (reader.Valid() && reader.Address() < end) {
        reader.Encoded(callSiteEncoding);
        reader.Encoded(callSiteEncoding);
        const auto pad = reader.Encoded(callSiteEncoding);
        reader.Uleb();
        if (reader.Valid() && pad && *pad != 0) pads.insert(lpStart + *pad);
    }
}

}

ExceptionTables ReadExceptionTables(const ElfImage& image) {
    ExceptionTables tables;
    const ElfProgramHeader* header = nullptr;
    for (const auto& programHeader : image.ProgramHeaders()) {
        if (programHeader.type == PtGnuEhFrame) header = &programHeader;
    }
    if (header == nullptr) return tables;
    Reader hdr(image, header->vaddr);
    if (hdr.Unsigned(1) != 1) return tables;
    const auto framePointerEncoding = static_cast<std::uint8_t>(hdr.Unsigned(1));
    hdr.Unsigned(2);
    const auto frames = hdr.Encoded(framePointerEncoding);
    if (!frames || !hdr.Valid()) return tables;
    std::map<std::uint64_t, Cie> cies;
    Reader reader(image, *frames);
    while (reader.Valid()) {
        const auto length = reader.Unsigned(4);
        if (!reader.Valid() || length == 0 || length == 0xffffffff) break;
        const auto body = reader.Address();
        const auto next = body + length;
        const auto cieOffset = reader.Unsigned(4);
        if (cieOffset != 0) {
            const auto cieAddress = body - cieOffset;
            auto cie = cies.find(cieAddress);
            if (cie == cies.end()) {
                if (auto parsed = ReadCie(image, cieAddress)) cie = cies.emplace(cieAddress, *parsed).first;
            }
            if (cie != cies.end()) {
                const auto start = reader.Encoded(cie->second.pointerEncoding);
                const auto range = reader.Encoded(cie->second.pointerEncoding & 0x0f);
                if (start && range && reader.Valid() && image.IsExecutable(*start)) {
                    tables.functionStarts.insert(*start);
                    tables.functionRanges[*start] = *start + *range;
                    if (cie->second.augmented) {
                        reader.Uleb();
                        if (const auto lsda = reader.Encoded(cie->second.lsdaEncoding); lsda && *lsda != 0 && reader.Valid()) {
                            ReadLsda(image, *lsda, *start, tables.landingPads[*start]);
                        }
                    }
                }
            }
        }
        reader.Seek(next);
    }
    return tables;
}

}
