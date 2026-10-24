#pragma once
// Cheap per command timestamps. On x86 this reads the TSC (rdtscp) and
// converts with a ratio calibrated against steady_clock; elsewhere it falls
// back to steady_clock.
#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace me::bench {

class Clock {
public:
    Clock() {
#if defined(__x86_64__)
        auto t0 = std::chrono::steady_clock::now();
        std::uint64_t c0 = raw();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::uint64_t c1 = raw();
        auto t1 = std::chrono::steady_clock::now();
        ns_per_tick_ = std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(c1 - c0);
#endif
    }

    std::uint64_t raw() const {
#if defined(__x86_64__)
        unsigned aux;
        return __rdtscp(&aux);
#else
        return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }

    double to_ns(std::uint64_t ticks) const { return static_cast<double>(ticks) * ns_per_tick_; }

private:
    double ns_per_tick_ = 1.0;
};

}  // namespace me::bench
