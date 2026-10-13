#ifndef CORE_TRANSLATOR_SRC_EXCEPTIONTABLES_HPP
#define CORE_TRANSLATOR_SRC_EXCEPTIONTABLES_HPP

#include "ElfImage.hpp"
#include <cstdint>
#include <map>
#include <set>

namespace translator {

struct ExceptionTables {
    std::set<std::uint64_t> functionStarts;
    std::map<std::uint64_t, std::uint64_t> functionRanges;
    std::map<std::uint64_t, std::set<std::uint64_t>> landingPads;
};

ExceptionTables ReadExceptionTables(const ElfImage& image);

}

#endif
