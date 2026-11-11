#include "VirtualDevice.h"

#include <ctime>
#include <pthread.h>
#include <sched.h>

namespace mix {

VirtualDevice::VirtualDevice(MixerEngine& engine, int sampleRate,
                             int blockSize)
    : engine_(engine), sampleRate_(sampleRate), blockSize_(blockSize) {}

VirtualDevice::~VirtualDevice() { stop(); }

void VirtualDevice::start(std::function<void(const MixerEngine&)> onBlock,
                          bool realtime) {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&VirtualDevice::threadLoop, this,
                          std::move(onBlock), realtime);
}

void VirtualDevice::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void VirtualDevice::runBlocks(
    size_t nBlocks,
    const std::function<void(const MixerEngine&)>& onBlock, bool) {
    for (size_t i = 0; i < nBlocks; ++i) {
        engine_.processBlock();
        if (onBlock) onBlock(engine_);
    }
}

void VirtualDevice::threadLoop(
    std::function<void(const MixerEngine&)> onBlock, bool realtime) {
    if (realtime) {
        sched_param p{};
        p.sched_priority = 80;  // harmless if unprivileged: stays normal
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
    }

    const long blockNs =
        static_cast<long>(1e9) * blockSize_ / sampleRate_;
    timespec next{};
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (running_.load(std::memory_order_acquire)) {
        engine_.processBlock();
        if (onBlock) onBlock(engine_);

        next.tv_nsec += blockNs;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            ++next.tv_sec;
        }
        // Absolute-time sleep: wakeup jitter does not accumulate.
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
    }
}

}  // namespace mix
