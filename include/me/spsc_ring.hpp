#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace me {

// Bounded single producer / single consumer queue.
//
// Producer and consumer each own one index and only read the other's with
// acquire loads. Each side also keeps a cached copy of the other side's
// index, so in steady state a push or pop touches only its own cache line
// and the shared index line is read only when the cached value says the
// queue looks full (or empty).
template <class T, std::size_t N>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>);

public:
    bool try_push(const T& v) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_cache_ == N) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t - head_cache_ == N) return false;
        }
        buf_[t & (N - 1)] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) return false;
        }
        out = buf_[h & (N - 1)];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    std::size_t size_approx() const {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kLine = 64;

    alignas(kLine) std::atomic<std::size_t> head_{0};  // written by consumer
    alignas(kLine) std::size_t tail_cache_ = 0;         // consumer's copy of tail
    alignas(kLine) std::atomic<std::size_t> tail_{0};  // written by producer
    alignas(kLine) std::size_t head_cache_ = 0;         // producer's copy of head
    alignas(kLine) T buf_[N];
};

}  // namespace me
