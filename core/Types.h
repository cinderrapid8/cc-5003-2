#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mix {

constexpr int kMaxChannels = 2;

enum class SourceKind {
    HostMic = 0,   // 主持话筒
    Remote = 1,    // 远端连线
    Playback = 2,  // 现场播放
};

enum class BusKind {
    MainPa = 0,    // 主扩
    Record = 1,    // 录制
    Send = 2,      // 远端返送 (mix-minus)
};

// Explicit overload policy applied when a mixed bus would exceed full scale.
enum class LimiterPolicy {
    HardClip = 0,  // saturate at +/-1 and count every clipped sample
    Reject = 1,    // emit silence for any block that would clip, count it
};

inline const char* sourceKindName(SourceKind k) {
    switch (k) {
        case SourceKind::HostMic: return "host";
        case SourceKind::Remote: return "remote";
        case SourceKind::Playback: return "playback";
    }
    return "?";
}

inline const char* busKindName(BusKind k) {
    switch (k) {
        case BusKind::MainPa: return "mainPA";
        case BusKind::Record: return "record";
        case BusKind::Send: return "send";
    }
    return "?";
}

// One edge = one routing rule. A source edge mixes a source into a bus;
// a bus edge forwards one bus into another.
struct EdgeParams {
    float gainDb = 0.0f;
    bool muted = false;
    // dst[ch] receives src[channelMap[ch]]; -1 means that destination
    // channel gets silence on this edge. Size 0 means identity mapping.
    int channelMap[kMaxChannels] = {0, 1};
};

struct SourceInfo {
    unsigned id = 0;
    SourceKind kind = SourceKind::HostMic;
    std::string name;
    int channels = 1;
    int remoteIndex = -1;  // >=0 only for Remote sources, used by mix-minus
};

struct BusInfo {
    unsigned id = 0;
    BusKind kind = BusKind::MainPa;
    std::string name;
    int channels = 2;
    bool muted = false;
    int remoteIndex = -1;  // >=0 only for Send buses: whose feed to exclude
};

struct EngineStats {
    uint64_t blocksProcessed = 0;
    uint64_t underruns = 0;          // source pull failures / short reads
    uint64_t clippedSamples = 0;     // samples that hit the hard limiter
    uint64_t rejectedBlocks = 0;     // blocks dropped by the Reject policy
    uint64_t configApplies = 0;      // number of frozen configs adopted
    uint64_t lastConfigGeneration = 0;
    double callbackAvgUs = 0.0;
    double callbackMaxUs = 0.0;
    double callbackBudgetUs = 0.0;
    std::vector<float> busPeak;      // pre-limiter peak per output bus/ch
};

}  // namespace mix
