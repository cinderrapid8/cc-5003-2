#pragma once

#include "EffectiveConfig.h"
#include "SpscQueue.h"
#include "Sources.h"
#include "Types.h"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>

namespace mix {

struct MetricsSample {
    uint64_t blocksProcessed = 0;
    uint64_t underruns = 0;
    uint64_t clippedSamples = 0;
    double callbackUs = 0.0;
    float busPeak[8] = {};
};

class MixerEngine {
public:
    MixerEngine(int sampleRate, int blockSize, int maxSources,
                int maxBuses = 4);
    ~MixerEngine();

    // Control thread: attach an input source. Real audio backends would
    // call this from their open routine; the virtual card uses it directly.
    void setSource(int slot, std::shared_ptr<ISource> source);

    // Control thread: publish a frozen config via the mailbox.
    void updateConfig(ConfigPtr cfg);

    // Select the overload policy. Safe to call anytime; the audio thread
    // reads the atomic value once per block.
    void setLimiterPolicy(LimiterPolicy policy) {
        policy_.store(static_cast<int>(policy), std::memory_order_relaxed);
    }

    // Audio thread entry. One call = one audio block; never blocks on
    // file I/O or unbounded locks (all synchronization is lock-free).
    void processBlock();

    // Audio thread: latest rendered output for bus slot, planar pointers
    // valid until the next processBlock() call.
    const float* const* busOutput(int busSlot) const;
    int busCount() const { return maxBuses_; }

    EngineStats stats() const;
    uint64_t blocksProcessed() const {
        return blocks_.load(std::memory_order_relaxed);
    }
    void resetPeaks();

    // GUI polls this at ~10-20 Hz; drops are counted as skipped samples,
    // never blocking the audio thread.
    bool pollMetrics(MetricsSample& out) { return metricsQueue_.pop(out); }

    int sampleRate() const { return sampleRate_; }
    int blockSize() const { return blockSize_; }

private:
    struct SrcEdgeRt {
        unsigned sourceId = 0;
        unsigned busId = 0;
        float targetGain = 1.0f;
        float currentGain = 0.0f;
        bool muted = false;
        bool retired = false;  // absent from newest config; ramps to zero
        int channelMap[kMaxChannels] = {0, 1};
    };
    struct BusEdgeRt {
        unsigned fromBus = 0;
        unsigned toBus = 0;
        float targetGain = 1.0f;
        float currentGain = 0.0f;
        bool muted = false;
        bool retired = false;
        int channelMap[kMaxChannels] = {0, 1};
    };

    void adoptConfig(ConfigPtr cfg);
    void reclaimRetired();

    static unsigned long long edgeKey(unsigned a, unsigned b) {
        return (static_cast<unsigned long long>(a) << 32) | b;
    }

    int sampleRate_;
    int blockSize_;
    int maxSources_;
    int maxBuses_;

    ConfigMailbox mailbox_;
    // Config currently driving the DSP; held by shared_ptr so retirement
    // frees it only once every in-flight block is done.
    ConfigPtr active_;

    std::unordered_map<unsigned long long, SrcEdgeRt> srcRt_;
    std::unordered_map<unsigned long long, BusEdgeRt> busRt_;
    std::vector<ConfigPtr> retired_;

    // source slot -> source; nullptr means silence input
    std::vector<std::shared_ptr<ISource>> sources_;
    // Planar per-block buffers.
    std::vector<std::vector<float>> srcBufs_;   // [slot*ch + ch][n]
    std::vector<std::vector<float>> busBufs_;   // [bus*ch + ch][n]
    std::vector<std::vector<float>> mixAccum_;  // bus accumulator
    std::vector<std::array<float*, kMaxChannels>> busPtrStorage_;
    std::vector<float* const*> busPtrs_;

    std::atomic<uint64_t> blocks_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> clipped_{0};
    std::atomic<uint64_t> rejected_{0};
    std::atomic<uint64_t> configApplies_{0};
    std::atomic<uint64_t> lastGen_{0};
    std::atomic<int> policy_{static_cast<int>(LimiterPolicy::HardClip)};

    std::vector<float> peakBus_;  // pre-limiter peak per bus (max of ch)
    double avgUs_ = 0.0;
    double maxUs_ = 0.0;
    double budgetUs_;

    SpscQueue<MetricsSample, 64> metricsQueue_;
};

}  // namespace mix
