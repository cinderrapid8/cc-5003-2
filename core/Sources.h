#pragma once

#include "Types.h"

#include <atomic>
#include <cstddef>
#include <memory>

namespace mix {

// Planar audio source. pull() writes exactly nframes samples per channel
// and returns the number of frames actually available:
//   nframes -> normal
//   0       -> input interrupted: engine must fill silence itself,
//              count an underrun and never replay the previous block.
class ISource {
public:
    virtual ~ISource() = default;
    virtual unsigned id() const = 0;
    virtual int channels() const = 0;
    virtual size_t pull(float* const* dst, size_t nframes) = 0;
};

// Always-silent source, also used as the safe default before any source
// is attached to an engine input.
class SilenceSource final : public ISource {
public:
    explicit SilenceSource(unsigned id = 0, int channels = 1)
        : id_(id), channels_(channels) {}
    unsigned id() const override { return id_; }
    int channels() const override { return channels_; }
    size_t pull(float* const* dst, size_t nframes) override;

private:
    unsigned id_;
    int channels_;
};

// Virtual sound-card input. Every source gets an individual frequency or
// a unique pulse rate so the analyzer can prove which sources reach a bus.
class VirtualSource final : public ISource {
public:
    enum class Mode { Tone, Pulse };

    VirtualSource(unsigned id, Mode mode, double frequencyHz, float amplitude,
                  double sampleRate, int channels);

    unsigned id() const override { return id_; }
    int channels() const override { return channels_; }
    double frequency() const { return freq_; }
    Mode mode() const { return mode_; }

    // Simulate an input being unplugged / starved: while false, pull()
    // returns 0 without touching the destination buffer.
    void setAvailable(bool on) { available_.store(on ? 1 : 0); }
    bool available() const { return available_.load() != 0; }

    size_t pull(float* const* dst, size_t nframes) override;

private:
    unsigned id_;
    Mode mode_;
    double freq_;
    float amp_;
    double sr_;
    int channels_;
    uint64_t phase_ = 0;
    uint64_t pipPhase_ = 0;
    uint64_t totalSamples_ = 0;
    std::atomic<int> available_{1};
};

}  // namespace mix
