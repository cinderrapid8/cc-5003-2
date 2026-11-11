// Offline, audible end-to-end demo: virtual sound card -> mixing matrix
// -> WAV files, exercising every acceptance scenario deterministically.
#include "../core/Analyzer.h"
#include "../core/MixerEngine.h"
#include "../core/RoutingGraph.h"
#include "../core/Sources.h"
#include "../core/VirtualDevice.h"
#include "../core/WavWriter.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace mix;

namespace {
constexpr int kSr = 48000;
constexpr int kBlock = 128;

struct Recorder {
    std::vector<std::vector<float>> ch;  // per bus, interleaved L/R
};

}  // namespace

int main() {
    std::printf("=== conference mixing matrix demo (virtual sound card) ===\n");

    // ---- topology: host mic, two remote callers, playback; main PA,
    // record, one mix-minus return bus per remote ---------------------
    RoutingGraph g;
    unsigned host = g.addSource(SourceKind::HostMic, "host", 1);
    unsigned remA = g.addSource(SourceKind::Remote, "remA", 1, 0);
    unsigned remB = g.addSource(SourceKind::Remote, "remB", 1, 1);
    unsigned play = g.addSource(SourceKind::Playback, "play", 2);
    unsigned pa = g.addBus(BusKind::MainPa, "mainPA", 2);
    unsigned rec = g.addBus(BusKind::Record, "record", 2);
    unsigned sendA = g.addBus(BusKind::Send, "sendA", 2, 0);
    unsigned sendB = g.addBus(BusKind::Send, "sendB", 2, 1);
    g.addDefaultEdges(2);

    MixerEngine eng(kSr, kBlock, 4, 4);
    // Signature frequencies: continuous tones stay below 300 Hz, the pulsed
    // source uses a 520 Hz carrier so the two encodings never overlap.
    eng.setSource(0, std::make_shared<VirtualSource>(
                         host, VirtualSource::Mode::Tone, 200.0, 0.12, kSr, 1));
    eng.setSource(1, std::make_shared<VirtualSource>(
                         remA, VirtualSource::Mode::Tone, 260.0, 0.12, kSr, 1));
    eng.setSource(2, std::make_shared<VirtualSource>(
                         remB, VirtualSource::Mode::Pulse, 9.0, 0.25, kSr, 1));
    struct StereoPlay : public ISource {
        unsigned id_;
        uint64_t ph = 0;
        explicit StereoPlay(unsigned id) : id_(id) {}
        unsigned id() const override { return id_; }
        int channels() const override { return 2; }
        size_t pull(float* const* d, size_t n) override {
            for (size_t i = 0; i < n; ++i) {
                double t = static_cast<double>(ph++) / kSr;
                d[0][i] = 0.09f * std::sin(2 * M_PI * 150.0 * t);
                d[1][i] = 0.09f * std::sin(2 * M_PI * 300.0 * t);
            }
            return n;
        }
    };
    eng.setSource(3, std::make_shared<StereoPlay>(play));
    eng.updateConfig(g.snapshot());

    // ---- mix-minus verification --------------------------------------
    const char* mmA = g.findEdge(sendA, remA) ? "YES (bad)" : "no (ok)";
    const char* mmB = g.findEdge(sendB, remB) ? "YES (bad)" : "no (ok)";
    std::printf("mix-minus check: sendA has remA edge? %s | sendB has remB "
                "edge? %s\n",
                mmA, mmB);

    // ---- loop requests ------------------------------------------------
    // First create a genuine bus chain pa -> record -> sendA, then prove
    // that closing it (sendA -> pa) and a self loop are both rejected.
    bool chain1 = g.setEdge(pa, rec, {});
    bool chain2 = g.setEdge(rec, sendA, {});
    bool closesLoop = g.setEdge(sendA, pa, {});
    bool selfLoop = g.setEdge(pa, pa, {});
    std::printf("loop requests:\n");
    std::printf("  chain pa->record (%s), record->sendA (%s)\n",
                chain1 ? "ok" : "bad", chain2 ? "ok" : "bad");
    std::printf("  closing sendA->mainPA: %s\n",
                closesLoop ? "ACCEPTED (bad)" : "rejected (ok)");
    std::printf("  mainPA->mainPA self-loop: %s\n",
                selfLoop ? "ACCEPTED (bad)" : "rejected (ok)");
    // The temporary bus chain served only the loop test; remove it so the
    // rendered matrix reflects the plain source->bus defaults and the
    // mix-minus exclusions stay audibly verifiable.
    g.removeEdge(pa, rec);
    g.removeEdge(rec, sendA);

    std::printf("routing edges (src -> bus, gain):\n");
    for (const auto& e : g.edges()) {
        auto nameOf = [&](unsigned id) -> std::string {
            for (const auto& s : g.sources())
                if (s.id == id) return s.name;
            for (const auto& b : g.buses())
                if (b.id == id) return b.name;
            return "?";
        };
        std::printf("  %-8s -> %-8s gain=%.2f%s\n", nameOf(e.from).c_str(),
                    nameOf(e.to).c_str(), e.params.gainDb,
                    e.params.muted ? " [muted]" : "");
    }

    // ---- 4 s timeline with interruptions and mutes -------------------
    const size_t totalBlocks = 4 * kSr / kBlock;
    std::vector<Recorder> recs(4);
    for (auto& r : recs) r.ch.resize(2);
    // Keep ownership of the remote A source so we can simulate its dropout.
    auto remAHolder = std::make_shared<VirtualSource>(
        remA, VirtualSource::Mode::Tone, 260.0, 0.12, kSr, 1);
    eng.setSource(1, remAHolder);

    VirtualDevice dev(eng, kSr, kBlock);
    size_t block = 0;
    dev.runBlocks(totalBlocks, [&](const MixerEngine& e) {
        if (block == static_cast<size_t>(1.2 * kSr / kBlock)) {
            g.setBusMuted(rec, true);
            eng.updateConfig(g.snapshot());
        }
        if (block == static_cast<size_t>(2.0 * kSr / kBlock)) {
            g.setBusMuted(rec, false);
            eng.updateConfig(g.snapshot());
        }
        if (block == static_cast<size_t>(2.95 * kSr / kBlock))
            remAHolder->setAvailable(false);  // remote A drops
        if (block == static_cast<size_t>(3.35 * kSr / kBlock))
            remAHolder->setAvailable(true);
        for (int b = 0; b < 4; ++b) {
            const float* const* o = e.busOutput(b);
            for (int i = 0; i < kBlock; ++i) {
                recs[b].ch[0].push_back(o[0][i]);
                recs[b].ch[1].push_back(o[1][i]);
            }
        }
        ++block;
    });

    auto windowPeak = [&](int bus, double t0, double t1) {
        size_t a = static_cast<size_t>(t0 * kSr),
               b = static_cast<size_t>(t1 * kSr);
        float p = 0.f;
        for (size_t i = a; i < b && i < recs[bus].ch[0].size(); ++i)
            p = std::max(p, std::fabs(recs[bus].ch[0][i]));
        return p;
    };
    std::printf("-- window 0.1-0.9 s (steady state) --\n");
    std::printf("  mainPA peak %.3f, record peak %.3f, sendA peak %.3f\n",
                windowPeak(0, 0.1, 0.9), windowPeak(1, 0.1, 0.9),
                windowPeak(2, 0.1, 0.9));
    std::printf("-- window 1.2-1.8 s (record bus muted) --\n");
    std::printf("  record peak %.6f, mainPA still %.3f\n",
                windowPeak(1, 1.25, 1.8), windowPeak(0, 1.25, 1.8));
    std::printf("-- window 2.95-3.35 s (remote A interrupted) --\n");
    std::printf("  mainPA peak %.3f (remB/host/playback keep it alive)\n",
                windowPeak(0, 3.0, 3.3));

    // ---- write audible WAV results -----------------------------------
    const char* names[4] = {"mainPA", "record", "sendA", "sendB"};
    for (int b = 0; b < 4; ++b) {
        std::vector<float> interleaved;
        interleaved.reserve(recs[b].ch[0].size() * 2);
        for (size_t i = 0; i < recs[b].ch[0].size(); ++i) {
            interleaved.push_back(recs[b].ch[0][i]);
            interleaved.push_back(recs[b].ch[1][i]);
        }
        std::string path = std::string("out/") + names[b] + ".wav";
        bool ok = writeWav(path, interleaved, 2, kSr);
        std::printf("wrote %s: %s\n", path.c_str(), ok ? "ok" : "FAILED");
    }

    // ---- routing self-check from the actual rendered audio -----------
    // Goertzel verifies each source's signature: tones at 200/260/150 and
    // the pulsed carrier at 520 Hz. The pulse repetition rate is checked
    // separately below on an isolated channel, free of tone interference.
    std::vector<double> freqs = {200.0, 260.0, 520.0, 150.0};
    std::vector<std::vector<float>> mono(4);
    for (int b = 0; b < 4; ++b) mono[b] = recs[b].ch[0];
    auto cfg = g.snapshot();
    std::vector<std::vector<char>> expected(
        4, std::vector<char>(4, 0));
    for (const SrcEdge& e : cfg->srcEdges) {
        size_t bi = 0, si = 0;
        for (; bi < 4; ++bi)
            if (cfg->buses[bi].id == e.busId) break;
        for (; si < 4; ++si)
            if (cfg->sources[si].id == e.sourceId) break;
        if (bi < 4 && si < 4) expected[bi][si] = 1;
    }
    MatrixReport rep = verifyRouting(mono, freqs, expected, kSr, 0.03);
    for (size_t b = 0; b < mono.size(); ++b) {
        std::printf("bus %zu magnitudes:", b);
        for (double f : freqs)
            std::printf(" %.4f", Analyzer::goertzelMagnitude(mono[b], f, kSr));
        std::printf("\n");
    }
    std::printf("%s\n", rep.toString().c_str());

    // Independent pulse-rate recovery on an isolated channel: a fresh
    // pulsed source is recorded without any tone interference, proving the
    // pulse coding carries a unique, measurable repetition rate (9 Hz)
    // distinct from its 520 Hz carrier.
    VirtualSource pulseProbe(99, VirtualSource::Mode::Pulse, 9.0, 0.25,
                             kSr, 1);
    std::vector<float> probe(kSr * 4);
    float* probePlanar[1] = {probe.data()};
    pulseProbe.pull(probePlanar, probe.size());
    double recoveredRate = Analyzer::detectPulseRate(probe, kSr);
    std::printf("pulse coding: isolated rate %.2f Hz (expect 9.0), "
                "carrier 520 Hz carries presence in the matrix above\n",
                recoveredRate);

    // ---- observability ------------------------------------------------
    EngineStats st = eng.stats();
    std::printf("-- engine stats --\n");
    std::printf("blocks processed : %llu\n",
                static_cast<unsigned long long>(st.blocksProcessed));
    std::printf("callback time    : avg %.1f us, max %.1f us (budget %.0f us)\n",
                st.callbackAvgUs, st.callbackMaxUs, st.callbackBudgetUs);
    std::printf("underruns        : %llu\n",
                static_cast<unsigned long long>(st.underruns));
    std::printf("clipped samples  : %llu\n",
                static_cast<unsigned long long>(st.clippedSamples));
    std::printf("config applies   : %llu (generation %llu)\n",
                static_cast<unsigned long long>(st.configApplies),
                static_cast<unsigned long long>(st.lastConfigGeneration));
    return rep.pass ? 0 : 1;
}
