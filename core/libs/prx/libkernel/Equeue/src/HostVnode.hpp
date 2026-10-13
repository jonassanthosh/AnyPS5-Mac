#ifndef CORE_LIBS_PRX_LIBKERNEL_EQUEUE_SRC_HOSTVNODE_HPP
#define CORE_LIBS_PRX_LIBKERNEL_EQUEUE_SRC_HOSTVNODE_HPP

#include <cstdint>

namespace HostVnode {

int OpenQueue();
void Stop(int queue);
int Watch(int queue, int target, std::uint32_t notes);
int Wait(int queue, std::uint32_t& notes, bool& stopped);

}

#endif
