#pragma once

#include "Types.h"

#include <atomic>
#include <memory>
#include <vector>

namespace mix {

struct SrcEdge {
    unsigned sourceId = 0;
    unsigned busId = 0;
    float gainLinear = 1.0f;
    bool muted = false;
    int channelMap[kMaxChannels] = {0, 1};
};

struct BusEdge {
    unsigned fromBus = 0;
    unsigned toBus = 0;
    float gainLinear = 1.0f;
    bool muted = false;
    int channelMap[kMaxChannels] = {0, 1};
};

// Immutable routing snapshot. The GUI builds a new instance for every
// change and publishes a shared_ptr; the audio thread swaps its pointer
// once per block, so a block never observes a half-updated configuration.
struct EffectiveConfig {
    uint64_t generation = 0;
    std::vector<SourceInfo> sources;
    std::vector<BusInfo> buses;
    std::vector<SrcEdge> srcEdges;
    std::vector<BusEdge> busEdges;

    const BusInfo* findBus(unsigned id) const {
        for (const auto& b : buses)
            if (b.id == id) return &b;
        return nullptr;
    }
    const SourceInfo* findSource(unsigned id) const {
        for (const auto& s : sources)
            if (s.id == id) return &s;
        return nullptr;
    }
};

using ConfigPtr = std::shared_ptr<const EffectiveConfig>;

// Publish/take of frozen configs. Only one publisher (control thread) and
// one subscriber (audio thread) are allowed; the exchange itself is a
// single atomic shared_ptr operation (lock-free on mainstream toolchains).
class ConfigMailbox {
public:
    void publish(ConfigPtr cfg) {
        std::atomic_store_explicit(&current_, std::move(cfg),
                                   std::memory_order_release);
    }

    // Returns the newest config since the last take, or nullptr.
    ConfigPtr take() {
        return std::atomic_exchange_explicit(&current_, ConfigPtr{},
                                             std::memory_order_acquire);
    }

private:
    ConfigPtr current_;
};

}  // namespace mix
