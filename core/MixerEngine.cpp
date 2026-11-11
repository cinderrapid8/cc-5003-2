#include "MixerEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mix {

namespace {
// Gain glide: ~5 ms time constant. A slider drag therefore changes level
// across many blocks, with no zipper noise and no per-sample parameter
// reads — one coefficient per block per edge.
constexpr float kSmoothTauSeconds = 0.005f;
}  // namespace

MixerEngine::MixerEngine(int sampleRate, int blockSize, int maxSources,
                         int maxBuses)
    : sampleRate_(sampleRate),
      blockSize_(blockSize),
      maxSources_(maxSources),
      maxBuses_(maxBuses),
      sources_(maxSources, nullptr),
      budgetUs_(1e6 * blockSize / static_cast<double>(sampleRate)) {
    srcBufs_.resize(static_cast<size_t>(maxSources) * kMaxChannels,
                    std::vector<float>(blockSize, 0.0f));
    busBufs_.resize(static_cast<size_t>(maxBuses) * kMaxChannels,
                    std::vector<float>(blockSize, 0.0f));
    mixAccum_.resize(static_cast<size_t>(maxBuses) * kMaxChannels,
                     std::vector<float>(blockSize, 0.0f));
    busPtrStorage_.resize(maxBuses);
    busPtrs_.resize(maxBuses);
    for (int b = 0; b < maxBuses; ++b) {
        for (int ch = 0; ch < kMaxChannels; ++ch)
            busPtrStorage_[b][ch] =
                busBufs_[static_cast<size_t>(b) * kMaxChannels + ch].data();
        busPtrs_[b] = busPtrStorage_[b].data();
    }
    peakBus_.assign(maxBuses, 0.0f);
}

MixerEngine::~MixerEngine() = default;

void MixerEngine::setSource(int slot, std::shared_ptr<ISource> source) {
    if (slot < 0 || slot >= maxSources_) return;
    sources_[slot] = std::move(source);
}

void MixerEngine::updateConfig(ConfigPtr cfg) {
    mailbox_.publish(std::move(cfg));
}

void MixerEngine::adoptConfig(ConfigPtr cfg) {
    // Runs on the audio thread when a new frozen config is available.
    // Existing edges keep their smoothing state; new edges ramp from zero
    // so an added route fades in instead of clicking.
    struct K {
        static unsigned long long of(unsigned a, unsigned b) {
            return (static_cast<unsigned long long>(a) << 32) | b;
        }
    };

    auto mapFrom = [](const int in[kMaxChannels], int out[kMaxChannels]) {
        out[0] = in[0];
        out[1] = in[1];
    };

    // Edges retired at the previous adoption have since glided to zero for
    // at least one whole block; drop them now.
    for (auto it = srcRt_.begin(); it != srcRt_.end();)
        it = it->second.retired ? srcRt_.erase(it) : std::next(it);
    for (auto it = busRt_.begin(); it != busRt_.end();)
        it = it->second.retired ? busRt_.erase(it) : std::next(it);

    // Mark every live RT edge retired; edges still present in the new
    // config clear the flag below. Anything marked stays in the table one
    // more adoption cycle with target 0, so removal fades out silently.
    for (auto& entry : srcRt_) {
        entry.second.retired = true;
        entry.second.targetGain = 0.0f;
    }
    for (auto& entry : busRt_) {
        entry.second.retired = true;
        entry.second.targetGain = 0.0f;
    }

    for (const SrcEdge& e : cfg->srcEdges) {
        auto k = K::of(e.sourceId, e.busId);
        auto [it, inserted] = srcRt_.try_emplace(k);
        SrcEdgeRt& rt = it->second;
        if (inserted) {
            rt = SrcEdgeRt{};
            rt.sourceId = e.sourceId;
            rt.busId = e.busId;
            mapFrom(e.channelMap, rt.channelMap);
        }
        rt.retired = false;
        rt.muted = e.muted;
        rt.targetGain = e.muted ? 0.0f : e.gainLinear;
        mapFrom(e.channelMap, rt.channelMap);
    }

    for (const BusEdge& e : cfg->busEdges) {
        auto k = K::of(e.fromBus, e.toBus);
        auto [it, inserted] = busRt_.try_emplace(k);
        BusEdgeRt& rt = it->second;
        if (inserted) {
            rt = BusEdgeRt{};
            rt.fromBus = e.fromBus;
            rt.toBus = e.toBus;
            mapFrom(e.channelMap, rt.channelMap);
        }
        rt.retired = false;
        rt.muted = e.muted;
        rt.targetGain = e.muted ? 0.0f : e.gainLinear;
        mapFrom(e.channelMap, rt.channelMap);
    }

    const uint64_t generation = cfg->generation;
    if (active_) retired_.push_back(active_);
    active_ = std::move(cfg);
    lastGen_.store(generation, std::memory_order_relaxed);
    configApplies_.fetch_add(1, std::memory_order_relaxed);
    reclaimRetired();
}

void MixerEngine::reclaimRetired() {
    // Keep the most recent retired config for one extra block in case a
    // downstream reader still holds pointers; drop everything older.
    while (retired_.size() > 1) retired_.erase(retired_.begin());
}

void MixerEngine::processBlock() {
    const auto t0 = std::chrono::steady_clock::now();
    const int n = blockSize_;

    if (ConfigPtr fresh = mailbox_.take()) adoptConfig(fresh);

    const EffectiveConfig* cfg = active_.get();
    const int nBuses = std::min(maxBuses_, cfg ? (int)cfg->buses.size() : 0);
    const int nSrc = std::min(maxSources_, cfg ? (int)cfg->sources.size() : 0);

    // 1) Pull every source. A short/zero read is an underrun: the block is
    //    filled with silence and counted. We never reuse the old buffer,
    //    which would disguise the interruption as continuous audio.
    for (int s = 0; s < nSrc; ++s) {
        const auto& info = cfg->sources[s];
        float* planar[kMaxChannels];
        for (int ch = 0; ch < kMaxChannels; ++ch)
            planar[ch] = srcBufs_[(size_t)s * kMaxChannels + ch].data();
        size_t got = 0;
        if (s < (int)sources_.size() && sources_[s])
            got = sources_[s]->pull(planar, (size_t)n);
        if (got < (size_t)n) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            size_t fromCh = std::min(info.channels, kMaxChannels);
            for (size_t ch = 0; ch < fromCh; ++ch)
                std::memset(planar[ch] + got, 0,
                            (n - got) * sizeof(float));
            for (int ch = (int)fromCh; ch < kMaxChannels; ++ch)
                std::memset(planar[ch], 0, n * sizeof(float));
        }
    }

    // 2) Clear bus accumulators and outputs.
    for (int b = 0; b < nBuses; ++b)
        for (int ch = 0; ch < kMaxChannels; ++ch) {
            std::memset(mixAccum_[(size_t)b * kMaxChannels + ch].data(), 0,
                        n * sizeof(float));
            std::memset(busBufs_[(size_t)b * kMaxChannels + ch].data(), 0,
                        n * sizeof(float));
        }

    // One glide coefficient per block, shared by all edges.
    const float coeff =
        1.0f - std::exp(-static_cast<float>(n) /
                        (kSmoothTauSeconds * (float)sampleRate_));

    if (cfg) {
        // 3) Source -> bus contributions.
        for (auto& entry : srcRt_) {
            SrcEdgeRt& e = entry.second;
            if (e.retired) continue;
            int s = (int)e.sourceId;
            if (s >= nSrc) continue;
            int b = -1;
            for (int i = 0; i < nBuses; ++i)
                if (cfg->buses[i].id == e.busId) { b = i; break; }
            if (b < 0) continue;
            const int srcCh = std::min(cfg->sources[s].channels, kMaxChannels);
            // Per-sample linear glide between the block's start and end
            // gains: no discontinuity at the block boundary, and the
            // coefficient itself is still computed once per block.
            const float gStart = e.currentGain;
            const float gEnd = gStart + (e.targetGain - gStart) * coeff;
            e.currentGain = gEnd;
            if (gEnd <= 1e-9f && gStart <= 1e-9f) continue;
            const float gStep = (gEnd - gStart) / static_cast<float>(n);
            for (int dch = 0; dch < kMaxChannels; ++dch) {
                int sch = e.channelMap[dch];
                if (sch < 0 || sch >= srcCh) continue;
                const float* in =
                    srcBufs_[(size_t)s * kMaxChannels + sch].data();
                float* acc = mixAccum_[(size_t)b * kMaxChannels + dch].data();
                float g = gStart;
                for (int i = 0; i < n; ++i) {
                    acc[i] += in[i] * g;
                    g += gStep;
                }
            }
        }

        // 4) Bus -> bus forwarding in the topological order baked into the
        //    frozen config, so forwarded audio is complete before the
        //    destination bus is finalized.
        for (const BusEdge& ce : cfg->busEdges) {
            auto it = busRt_.find(edgeKey(ce.fromBus, ce.toBus));
            if (it == busRt_.end() || it->second.retired) continue;
            BusEdgeRt& e = it->second;
            int from = -1, to = -1;
            for (int i = 0; i < nBuses; ++i) {
                if (cfg->buses[i].id == e.fromBus) from = i;
                if (cfg->buses[i].id == e.toBus) to = i;
            }
            if (from < 0 || to < 0) continue;
            const float gStart = e.currentGain;
            const float gEnd = gStart + (e.targetGain - gStart) * coeff;
            e.currentGain = gEnd;
            if (gEnd <= 1e-9f && gStart <= 1e-9f) continue;
            const float gStep = (gEnd - gStart) / static_cast<float>(n);
            for (int dch = 0; dch < kMaxChannels; ++dch) {
                int sch = e.channelMap[dch];
                if (sch < 0 || sch >= kMaxChannels) continue;
                const float* in =
                    mixAccum_[(size_t)from * kMaxChannels + sch].data();
                float* acc =
                    mixAccum_[(size_t)to * kMaxChannels + dch].data();
                float g = gStart;
                for (int i = 0; i < n; ++i) {
                    acc[i] += in[i] * g;
                    g += gStep;
                }
            }
        }

        // 5) Finalize each bus: bus mute, pre-limiter peak metering, then
        //    the explicit overload policy selected on the control thread.
        const LimiterPolicy policy =
            static_cast<LimiterPolicy>(policy_.load(std::memory_order_relaxed));
        for (int b = 0; b < nBuses; ++b) {
            const bool muted = cfg->buses[b].muted;
            float peak = 0.0f;
            for (int ch = 0; ch < kMaxChannels; ++ch) {
                float* acc = mixAccum_[(size_t)b * kMaxChannels + ch].data();
                for (int i = 0; i < n; ++i)
                    peak = std::max(peak, std::fabs(acc[i]));
            }
            peakBus_[b] = std::max(peakBus_[b], peak);

            const bool wouldClip = peak > 1.0f && !muted;
            if (policy == LimiterPolicy::Reject && wouldClip) {
                // Refuse the whole over-level block rather than distort it.
                rejected_.fetch_add(1, std::memory_order_relaxed);
                for (int ch = 0; ch < kMaxChannels; ++ch)
                    std::memset(
                        busBufs_[(size_t)b * kMaxChannels + ch].data(), 0,
                        n * sizeof(float));
                continue;
            }
            for (int ch = 0; ch < kMaxChannels; ++ch) {
                float* acc = mixAccum_[(size_t)b * kMaxChannels + ch].data();
                float* out = busBufs_[(size_t)b * kMaxChannels + ch].data();
                for (int i = 0; i < n; ++i) {
                    if (muted) {
                        out[i] = 0.0f;
                        continue;
                    }
                    if (acc[i] > 1.0f) {
                        out[i] = 1.0f;
                        clipped_.fetch_add(1, std::memory_order_relaxed);
                    } else if (acc[i] < -1.0f) {
                        out[i] = -1.0f;
                        clipped_.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        out[i] = acc[i];
                    }
                }
            }
        }
    }

    const auto t1 = std::chrono::steady_clock::now();
    double us =
        std::chrono::duration<double, std::micro>(t1 - t0).count();
    maxUs_ = std::max(maxUs_, us);
    avgUs_ = avgUs_ == 0.0 ? us : avgUs_ * 0.99 + us * 0.01;

    const uint64_t done = blocks_.fetch_add(1, std::memory_order_relaxed) + 1;
    MetricsSample m;
    m.blocksProcessed = done;
    m.underruns = underruns_.load(std::memory_order_relaxed);
    m.clippedSamples = clipped_.load(std::memory_order_relaxed);
    m.callbackUs = us;
    for (int b = 0; b < nBuses && b < 8; ++b) m.busPeak[b] = peakBus_[b];
    metricsQueue_.push(m);  // lock-free; a full queue just drops a sample
}

const float* const* MixerEngine::busOutput(int busSlot) const {
    return busPtrs_[busSlot];
}

EngineStats MixerEngine::stats() const {
    EngineStats s;
    s.blocksProcessed = blocks_.load(std::memory_order_relaxed);
    s.underruns = underruns_.load(std::memory_order_relaxed);
    s.clippedSamples = clipped_.load(std::memory_order_relaxed);
    s.rejectedBlocks = rejected_.load(std::memory_order_relaxed);
    s.configApplies = configApplies_.load(std::memory_order_relaxed);
    s.lastConfigGeneration = lastGen_.load(std::memory_order_relaxed);
    s.callbackAvgUs = avgUs_;
    s.callbackMaxUs = maxUs_;
    s.callbackBudgetUs = budgetUs_;
    s.busPeak = peakBus_;
    return s;
}

void MixerEngine::resetPeaks() {
    for (float& p : peakBus_) p = 0.0f;
}

}  // namespace mix
