#ifndef CORE_TRANSLATOR_RUNTIME_INCLUDE_TRANSLATOR_GUESTJUMP_HPP
#define CORE_TRANSLATOR_RUNTIME_INCLUDE_TRANSLATOR_GUESTJUMP_HPP

#include "translator/Runtime.hpp"
#include <string>
#include <utility>
#include <vector>

namespace translator {

void GuestSetjmp(GuestContext* c);
void GuestLongjmp(GuestContext* c);

const std::vector<std::pair<std::string, ContextImport>>& RuntimeContextImports();

}

#endif
