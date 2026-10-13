#ifdef __APPLE__
#include "HostVnode.hpp"

#include <cerrno>
#include <sys/event.h>
#include <unistd.h>

namespace HostVnode {

namespace {

constexpr std::uintptr_t StopIdent = 1;

}

int OpenQueue() {
    const int queue = ::kqueue();
    if (queue < 0) return -1;
    struct kevent stop {};
    EV_SET(&stop, StopIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(queue, &stop, 1, nullptr, 0, nullptr) != 0) {
        const int error = errno;
        ::close(queue);
        errno = error;
        return -1;
    }
    return queue;
}

void Stop(int queue) {
    struct kevent stop {};
    EV_SET(&stop, StopIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    static_cast<void>(::kevent(queue, &stop, 1, nullptr, 0, nullptr));
}

int Watch(int queue, int target, std::uint32_t notes) {
    struct kevent watch {};
    EV_SET(&watch, static_cast<std::uintptr_t>(target), EVFILT_VNODE, EV_ADD | EV_CLEAR, notes, 0, nullptr);
    return ::kevent(queue, &watch, 1, nullptr, 0, nullptr) == 0 ? 0 : errno;
}

int Wait(int queue, std::uint32_t& notes, bool& stopped) {
    notes = 0;
    stopped = false;
    struct kevent events[8];
    for (;;) {
        const int count = ::kevent(queue, nullptr, 0, events, 8, nullptr);
        if (count < 0) {
            if (errno == EINTR) continue;
            return errno;
        }
        for (int index = 0; index < count; ++index) {
            if (events[index].filter == EVFILT_USER) stopped = true;
            if (events[index].filter == EVFILT_VNODE) notes |= events[index].fflags;
        }
        return 0;
    }
}

}
#endif
