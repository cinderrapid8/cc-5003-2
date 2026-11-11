#include "Sources.h"

#include <cmath>

namespace mix {

namespace {
constexpr double kTwoPi = 6.283185307179586;
}  // namespace

size_t SilenceSource::pull(float* const* dst, size_t nframes) {
    for (int ch = 0; ch < channels_; ++ch)
        for (size_t i = 0; i < nframes; ++i) dst[ch][i] = 0.0f;
    return nframes;
}

VirtualSource::VirtualSource(unsigned id, Mode mode, double frequencyHz,
                             float amplitude, double sampleRate, int channels)
    : id_(id),
      mode_(mode),
      freq_(frequencyHz),
      amp_(amplitude),
      sr_(sampleRate),
      channels_(channels) {}

size_t VirtualSource::pull(float* const* dst, size_t nframes) {
    if (available_.load(std::memory_order_acquire) == 0)
        return 0;  // interrupted: do not write, do not advance phase

    // 64-bit phase accumulator with an implicit 2^32 modulo: one full
    // 32-bit turn equals one signal period, so wrap rate == freq_. Block
    // boundaries do not disturb the timing, no floating phase drift.
    const uint64_t inc =
        static_cast<uint64_t>(freq_ * 4294967296.0 / sr_);
    // Pulse mode emits a band-limited burst train: freq_ gated envelopes
    // per second, each opening a 500 Hz carrier for ~45% of the period.
    // The envelope gives a unique repetition rate (detectPulseRate on the
    // rectified signal); the continuous-phase carrier retains coherent
    // Goertzel energy even when several sources are mixed onto one bus.
    const double carrierHz = 520.0;
    const double gateFraction = 0.6;
    for (size_t i = 0; i < nframes; ++i) {
        uint32_t prev32 = static_cast<uint32_t>(phase_);
        phase_ += inc;
        uint32_t now32 = static_cast<uint32_t>(phase_);
        double t = static_cast<double>(now32) / 4294967296.0;
        float v;
        if (mode_ == Mode::Tone) {
            v = static_cast<float>(amp_ * std::sin(t * kTwoPi));
        } else {
            bool wrapped = now32 < prev32;
            if (wrapped) pipPhase_ = 0;
            double frac = static_cast<double>(pipPhase_) * freq_ / sr_;
            if (frac < gateFraction) {
                double env =
                    0.5 * (1.0 - std::cos(kTwoPi * frac / gateFraction));
                double carrierPhase =
                    kTwoPi * carrierHz *
                    static_cast<double>(totalSamples_ + i) / sr_;
                v = static_cast<float>(amp_ * env * std::sin(carrierPhase));
                ++pipPhase_;
            } else {
                v = 0.0f;
            }
        }
        for (int ch = 0; ch < channels_; ++ch) dst[ch][i] = v;
    }
    totalSamples_ += nframes;
    return nframes;
}

}  // namespace mix
