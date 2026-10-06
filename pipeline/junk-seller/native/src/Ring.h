#pragma once
// Bounded value-only ring. Producers never wait: a failed try_lock or a full
// buffer increments a drop counter instead of blocking an engine thread.
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
namespace js {
template <class T, std::size_t N>
class Ring {
public:
    static constexpr std::size_t Capacity = N;
    bool Push(const T& value) noexcept {
        std::unique_lock lock(_mutex, std::try_to_lock);
        if (!lock || _count == _limit) { ++drops; return false; }
        _slots[_head] = value;
        _head = (_head + 1) % N;
        ++_count; ++available; ++pushed;
        return true;
    }
    bool Pop(T& value) noexcept {
        std::unique_lock lock(_mutex, std::try_to_lock);
        if (!lock || !_count) return false;
        value = _slots[_tail];
        _tail = (_tail + 1) % N;
        --_count; --available; ++popped;
        return true;
    }
    // The INI may shrink the usable window; it can never grow past N.
    void SetLimit(std::size_t limit) noexcept {
        std::lock_guard lock(_mutex);
        _limit = limit == 0 || limit > N ? N : limit;
    }
    std::size_t Limit() const noexcept { return _limit; }
    std::atomic<std::uint64_t> pushed{}, popped{}, drops{};
    std::atomic<unsigned> available{};
private:
    std::array<T, N> _slots{};
    mutable std::mutex _mutex;
    std::size_t _head{}, _tail{}, _count{}, _limit{N};
};
}
