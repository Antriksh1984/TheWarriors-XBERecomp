// The MCPX APU's global sample counter (NV_PAPU_XGSCNT at 0xFE80200C), a 48 kHz count the console's audio
// processor keeps. XACT reads it directly for its timing (0x3D4D0D and others), so a host thread keeps it running.
#include <atomic>
#include <chrono>
#include <thread>

#include "kernel/memory.h"

namespace warriors {

namespace {

constexpr uint32_t kSampleCounter = 0xFE80200C;
constexpr uint64_t kSampleRate = 48000;

}  // namespace

void StartApuClock() {
    mem::Commit(kSampleCounter & ~(mem::kPage - 1), mem::kPage);
    auto* counter = reinterpret_cast<std::atomic<uint32_t>*>(mem::Base() + kSampleCounter);
    counter->store(0);
    std::thread([counter] {
        auto start = std::chrono::steady_clock::now();
        for (;;) {
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
            counter->store(uint32_t(uint64_t(us.count()) * kSampleRate / 1000000), std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
}

}  // namespace warriors
