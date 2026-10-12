#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GENERAL_ATOMICSHAREDPTR_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GENERAL_ATOMICSHAREDPTR_HPP

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

#if defined(__cpp_lib_atomic_shared_ptr)

template <typename T>
using AtomicSharedPtr = std::atomic<std::shared_ptr<T>>;

#else

template <typename T>
class AtomicSharedPtr {
public:
    AtomicSharedPtr() noexcept = default;
    AtomicSharedPtr(std::shared_ptr<T> value) noexcept : _value(std::move(value)) {}
    AtomicSharedPtr(const AtomicSharedPtr&) = delete;
    AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;

    std::shared_ptr<T> load(std::memory_order = std::memory_order_seq_cst) const {
        std::lock_guard lock(_lock);
        return _value;
    }

    void store(std::shared_ptr<T> value, std::memory_order = std::memory_order_seq_cst) {
        std::lock_guard lock(_lock);
        _value.swap(value);
    }

    std::shared_ptr<T> exchange(std::shared_ptr<T> value, std::memory_order = std::memory_order_seq_cst) {
        std::lock_guard lock(_lock);
        _value.swap(value);
        return value;
    }

    bool compare_exchange_strong(std::shared_ptr<T>& expected, std::shared_ptr<T> desired,
        std::memory_order = std::memory_order_seq_cst, std::memory_order = std::memory_order_seq_cst) {
        std::lock_guard lock(_lock);
        if (_value == expected) {
            _value.swap(desired);
            return true;
        }
        expected = _value;
        return false;
    }

    operator std::shared_ptr<T>() const {
        return load();
    }

private:
    mutable std::mutex _lock;
    std::shared_ptr<T> _value;
};

#endif

#endif
