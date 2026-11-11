#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace mix {

// Offline analysis helpers used by the virtual sound-card self test.
// All routines are allocation-light and never touch the audio thread.
class Analyzer {
public:
    // Single-bin Goertzel magnitude: how strongly `freqHz` is present.
    static double goertzelMagnitude(const float* data, size_t n,
                                    double freqHz, double sampleRate);
    static double goertzelMagnitude(const std::vector<float>& data,
                                    double freqHz, double sampleRate) {
        return goertzelMagnitude(data.data(), data.size(), freqHz,
                                 sampleRate);
    }

    // Estimate the pulse repetition rate of a VirtualSource::Pulse stream
    // by locating its narrow pulses and averaging their spacing.
    static double detectPulseRate(const std::vector<float>& data,
                                  double sampleRate);

    static float peak(const std::vector<float>& data);
};

// Result of the virtual sound-card routing self check: for every output
// bus, which input sources were actually detected in the rendered audio.
struct MatrixReport {
    // detected[b][s] = source s is audibly present in bus b.
    std::vector<std::vector<char>> detected;
    std::vector<std::vector<char>> expected;
    bool pass = false;
    std::string toString() const;
};

// busRecordings: per bus, per channel planar samples of the rendered output
// (concatenated blocks). sourceFreqs: signature tone/pulse frequency of
// each source. expected: routing matrix the config claims to implement.
MatrixReport verifyRouting(
    const std::vector<std::vector<float>>& busRecordings,
    const std::vector<double>& sourceFreqs,
    const std::vector<std::vector<char>>& expected,
    double sampleRate, double detectThreshold = 0.05);

}  // namespace mix
