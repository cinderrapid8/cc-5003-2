#pragma once

#include "MixerEngine.h"
#include "Types.h"

#include <atomic>
#include <functional>
#include <thread>

namespace mix {

// Virtual sound card. A high-priority thread wakes at the audio block
// rate and invokes engine.processBlock(), emulating a real device
// callback. `onBlock` lets a recorder/analyzer observe each block; it runs
// on the audio thread and must stay allocation-free.
class VirtualDevice {
public:
    VirtualDevice(MixerEngine& engine, int sampleRate, int blockSize);
    ~VirtualDevice();

    // realtime=true schedules with SCHED_FIFO when permitted and paces
    // blocks with clock_nanosleep (absolute wakeups, no drift accumulation).
    void start(std::function<void(const MixerEngine&)> onBlock,
               bool realtime = true);
    void stop();

    // Deterministic, offline variant for tests/demos: render exactly
    // nBlocks back-to-back without sleeping or spawning a thread.
    void runBlocks(size_t nBlocks,
                   const std::function<void(const MixerEngine&)>& onBlock,
                   bool realtime = false);

    bool running() const { return running_.load(); }

private:
    void threadLoop(std::function<void(const MixerEngine&)> onBlock,
                    bool realtime);

    MixerEngine& engine_;
    int sampleRate_;
    int blockSize_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace mix
