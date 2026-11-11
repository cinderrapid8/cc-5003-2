#include "Analyzer.h"

#include <algorithm>
#include <cmath>

namespace mix {

double Analyzer::goertzelMagnitude(const float* data, size_t n,
                                   double freqHz, double sampleRate) {
    if (n == 0 || sampleRate <= 0.0) return 0.0;
    const double w = 2.0 * M_PI * freqHz / sampleRate;
    const double cw = std::cos(w);
    const double coeff = 2.0 * cw;
    double sPrev = 0.0, sPrev2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double s = static_cast<double>(data[i]) + coeff * sPrev - sPrev2;
        sPrev2 = sPrev;
        sPrev = s;
    }
    // Standard normalized magnitude: a pure tone at the bin frequency
    // yields ~1.0 independent of length; absent energy stays near zero.
    double raw = sPrev * sPrev + sPrev2 * sPrev2 - coeff * sPrev * sPrev2;
    return std::sqrt(std::max(0.0, raw)) / static_cast<double>(n);
}

double Analyzer::detectPulseRate(const std::vector<float>& data,
                                 double sampleRate) {
    // Rectify then smooth over a few carrier cycles to recover the slow
    // gating envelope; carrier zero-crossings must not count as pulses.
    const size_t win = std::max<size_t>(8, sampleRate / 1000);
    std::vector<double> env(data.size(), 0.0);
    double run = 0.0;
    for (size_t i = 0; i < data.size(); ++i) {
        run += std::fabs(static_cast<double>(data[i]));
        if (i >= win) run -= std::fabs(static_cast<double>(data[i - win]));
        env[i] = run / static_cast<double>(win);
    }
    double maxEnv = 0.0;
    for (double v : env) maxEnv = std::max(maxEnv, v);
    if (maxEnv <= 0.0) return 0.0;
    const double on = maxEnv * 0.45;
    const double off = maxEnv * 0.2;
    std::vector<size_t> edges;
    bool above = false;
    for (size_t i = 0; i < env.size(); ++i) {
        if (!above && env[i] >= on) {
            edges.push_back(i);
            above = true;
        } else if (above && env[i] <= off) {
            above = false;
        }
    }
    if (edges.size() < 2) return 0.0;
    double total = 0.0;
    for (size_t i = 1; i < edges.size(); ++i)
        total += static_cast<double>(edges[i] - edges[i - 1]);
    return sampleRate / (total / (edges.size() - 1));
}

float Analyzer::peak(const std::vector<float>& data) {
    float p = 0.0f;
    for (float v : data) p = std::max(p, std::fabs(v));
    return p;
}

MatrixReport verifyRouting(
    const std::vector<std::vector<float>>& busRecordings,
    const std::vector<double>& sourceFreqs,
    const std::vector<std::vector<char>>& expected, double sampleRate,
    double detectThreshold) {
    MatrixReport rep;
    const size_t nBuses = busRecordings.size();
    const size_t nSrc = sourceFreqs.size();
    rep.expected = expected;
    rep.detected.assign(nBuses, std::vector<char>(nSrc, 0));

    for (size_t b = 0; b < nBuses; ++b) {
        const auto& rec = busRecordings[b];
        for (size_t s = 0; s < nSrc; ++s) {
            // Goertzel works for both continuous tones and the narrow
            // pulses because a pulse train's fundamental sits at its rate.
            double mag = Analyzer::goertzelMagnitude(rec, sourceFreqs[s],
                                                     sampleRate);
            rep.detected[b][s] = mag >= detectThreshold ? 1 : 0;
        }
    }

    rep.pass = rep.detected.size() == expected.size();
    if (rep.pass) {
        for (size_t b = 0; b < nBuses && rep.pass; ++b) {
            if (expected[b].size() != nSrc) {
                rep.pass = false;
                break;
            }
            for (size_t s = 0; s < nSrc; ++s)
                if (rep.detected[b][s] != expected[b][s]) {
                    rep.pass = false;
                    break;
                }
        }
    }
    return rep;
}

std::string MatrixReport::toString() const {
    std::string out = "bus\\src";
    const size_t nSrc = detected.empty() ? 0 : detected[0].size();
    for (size_t s = 0; s < nSrc; ++s) out += "  S" + std::to_string(s);
    out += "\n";
    for (size_t b = 0; b < detected.size(); ++b) {
        out += "bus " + std::to_string(b) + " ";
        for (size_t s = 0; s < detected[b].size(); ++s) {
            char mark = detected[b][s] ? 'Y' : '.';
            if (b < expected.size() && s < expected[b].size() &&
                detected[b][s] != expected[b][s])
                mark = '!';
            out += "  " + std::string(1, mark) + " ";
        }
        out += "\n";
    }
    out += pass ? "matrix: PASS" : "matrix: FAIL";
    return out;
}

}  // namespace mix
