#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace mix {

// Bounded single-producer/single-consumer ring. Used to hand per-block
// metrics from the audio thread to the GUI without locks or allocation.
template <typename T, size_t N>
class SpscQueue {
public:
    bool push(const T& v) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % N;
        if (next == tail_.load(std::memory_order_acquire))
            return false;  // full: oldest data stays, caller counts drops
        slots_[head] = v;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        out = slots_[tail];
        tail_.store((tail + 1) % N, std::memory_order_release);
        return true;
    }

private:
    std::array<T, N> slots_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

}  // namespace mix
