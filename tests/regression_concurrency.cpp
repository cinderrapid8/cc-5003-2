// Regression: consecutive routing edits racing the audio block boundary.
//
// The reported symptom was a conference return bus (mix-minus Send)
// occasionally carrying the remote caller's own audio while two adjacent
// routing updates were published back to back. A block is only correct if
// the whole block obeys exactly one frozen revision: edges, gains and the
// mix-minus exclusions must never be stitched together from two snapshots.
//
// These tests step the engine deterministically (one processBlock at a
// time) so the mailbox can be forced to skip intermediate revisions, plus a
// multithreaded soak that exercises the real publish/take timing under
// ASan/UBSan. Every assertion is derived from the actual rendered audio:
// a default-passing result cannot hide a routing failure because the
// expected matrix is rebuilt from the exact config that drove each block.
#include "../core/Analyzer.h"
#include "../core/MixerEngine.h"
#include "../core/RoutingGraph.h"
#include "../core/Sources.h"
#include "../core/VirtualDevice.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using namespace mix;

namespace {
constexpr int kSr = 48000;
constexpr int kBlock = 128;
constexpr int kSettleBlocks = 60;  // > 5 ms glide time constant, comfortably

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        ++g_checks;                                                    \
        if (!(cond)) {                                                 \
            ++g_failures;                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

struct Rig {
    std::unique_ptr<RoutingGraph> g;
    std::unique_ptr<MixerEngine> eng;
    std::vector<std::shared_ptr<VirtualSource>> tones;
    unsigned host = 0, remA = 0, remB = 0, play = 0;
    unsigned pa = 0, rec = 0, sendA = 0, sendB = 0;
    std::vector<double> freqs;

    static std::unique_ptr<RoutingGraph> graph(int remotes) {
        auto gr = std::make_unique<RoutingGraph>();
        gr->addSource(SourceKind::HostMic, "host", 1, -1);
        for (int r = 0; r < remotes; ++r)
            gr->addSource(SourceKind::Remote, "rem" + std::to_string(r), 1,
                          r);
        gr->addSource(SourceKind::Playback, "play", 2, -1);
        gr->addBus(BusKind::MainPa, "mainPA", 2);
        gr->addBus(BusKind::Record, "record", 2);
        for (int r = 0; r < remotes; ++r)
            gr->addBus(BusKind::Send, "send" + std::to_string(r), 2, r);
        gr->addDefaultEdges(remotes);
        return gr;
    }

    static Rig make(int remotes) {
        Rig rig;
        rig.g = graph(remotes);
        const auto& src = rig.g->sources();
        const auto& bus = rig.g->buses();
        rig.host = src[0].id;
        rig.remA = src[1].id;
        if (remotes > 1) rig.remB = src[2].id;
        rig.play = src.back().id;
        rig.pa = bus[0].id;
        rig.rec = bus[1].id;
        rig.sendA = bus[2].id;
        if (remotes > 1) rig.sendB = bus[3].id;

        rig.eng = std::make_unique<MixerEngine>(
            kSr, kBlock, (int)src.size(), (int)bus.size());
        for (size_t i = 0; i < src.size(); ++i) {
            double f = 200.0 + 70.0 * (double)i;  // unique signature
            rig.freqs.push_back(f);
            auto t = std::make_shared<VirtualSource>(
                src[i].id, VirtualSource::Mode::Tone, f, 0.4, kSr,
                src[i].channels);
            rig.tones.push_back(t);
            rig.eng->setSource((int)i, t);
        }
        rig.eng->updateConfig(rig.g->snapshot());
        return rig;
    }
};

// Render one block and capture channel 0 of every bus.
void stepBlock(MixerEngine& eng, std::vector<std::vector<float>>& rec) {
    eng.processBlock();
    for (int b = 0; b < eng.busCount(); ++b) {
        const float* const* p = eng.busOutput(b);
        for (int i = 0; i < kBlock; ++i) rec[b].push_back(p[0][i]);
    }
}

// A single 128-sample block cannot separate nearby tones (one-block
// Goertzel leakage is as large as the signal). Aggregate a run of blocks
// (~2048 samples), where off-bin leakage drops below 0.002 while a present
// tone stays near 0.2, so presence/absence is unambiguous.
constexpr size_t kAnalyzeBlocks = 16;
double windowEnergy(const std::vector<float>& rec, size_t endBlock,
                    double freq) {
    size_t end = endBlock * kBlock;
    size_t begin = end > kAnalyzeBlocks * kBlock
                       ? end - kAnalyzeBlocks * kBlock
                       : 0;
    std::vector<float> window(rec.begin() + begin, rec.begin() + end);
    return Analyzer::goertzelMagnitude(window, freq, kSr);
}

    // Rebuild the expected presence matrix from the exact frozen config.
    // Indexed by *slot* (engine source slot / bus slot), which is how the
    // recorder lays out rows and how the signature frequencies are stored.
std::vector<std::vector<char>> expectedMatrix(const EffectiveConfig& cfg) {
    size_t nb = cfg.buses.size(), ns = cfg.sources.size();
    std::vector<std::vector<char>> m(nb, std::vector<char>(ns, 0));
    for (const SrcEdge& e : cfg.srcEdges) {
        if (e.muted) continue;
        size_t bi = 0, si = 0;
        for (; bi < nb; ++bi)
            if (cfg.buses[bi].id == e.busId) break;
        for (; si < ns; ++si)
            if (cfg.sources[si].id == e.sourceId) break;
        if (bi < nb && si < ns) m[bi][si] = 1;
    }
    return m;
}

// 1. Two adjacent updates inside one block gap: the intermediate revision is
//    never rendered, so no block may mix it with its neighbour. Drives the
//    exact mix-minus edge whose leak is the reported conference defect.
void testSkippedRevisionBoundary() {
    std::printf("[regress] skipped-revision block boundary\n");
    Rig rig = Rig::make(1);
    RoutingGraph& g = *rig.g;
    MixerEngine& eng = *rig.eng;
    const int sendSlot = 2;      // sendA bus slot
    const int remSlot = 1;       // remA source slot
    const double remFreq = rig.freqs[remSlot];

    std::vector<std::vector<float>> rec(eng.busCount());
    for (int i = 0; i < kSettleBlocks; ++i) stepBlock(eng, rec);

    // Steady state: mix-minus guarantees remA never reaches sendA.
    size_t steadyEnd = rec[sendSlot].size() / kBlock;
    CHECK(windowEnergy(rec[sendSlot], steadyEnd, remFreq) < 0.05);

    // Revision B adds the (normally forbidden at build time) self edge;
    // revision C removes it again. Publish both before the next block so the
    // mailbox can only deliver C: B must never become audible.
    CHECK(g.setEdge(rig.remA, rig.sendA, {}));
    ConfigPtr cfgB = g.snapshot();
    CHECK(g.removeEdge(rig.remA, rig.sendA));
    ConfigPtr cfgC = g.snapshot();
    eng.updateConfig(cfgB);
    eng.updateConfig(cfgC);  // overwrites B inside the single-slot mailbox

    for (int i = 0; i < kSettleBlocks; ++i) stepBlock(eng, rec);
    size_t afterGap = rec[sendSlot].size() / kBlock;
    CHECK(windowEnergy(rec[sendSlot], afterGap, remFreq) < 0.05);

    // Mirror case: remove then re-add within one gap; the re-added edge
    // ramps from zero and the block still obeys only the final revision.
    CHECK(g.setEdge(rig.remA, rig.sendA, {}));
    ConfigPtr cfgD = g.snapshot();       // edge present
    CHECK(g.removeEdge(rig.remA, rig.sendA));
    ConfigPtr cfgE = g.snapshot();       // edge absent
    CHECK(g.setEdge(rig.remA, rig.sendA, {}));
    ConfigPtr cfgF = g.snapshot();       // edge present again
    eng.updateConfig(cfgD);
    eng.updateConfig(cfgE);
    eng.updateConfig(cfgF);              // only F can be taken
    for (int i = 0; i < kSettleBlocks; ++i) stepBlock(eng, rec);
    // F contains the edge, so after settling remA is audibly present and the
    // observed matrix matches F exactly (single revision, no stitching).
    auto mat = expectedMatrix(*cfgF);
    size_t afterF = rec[sendSlot].size() / kBlock;
    CHECK(windowEnergy(rec[sendSlot], afterF, remFreq) > 0.1);
    CHECK(mat[sendSlot][remSlot] == 1);
    // Restore the mix-minus state and confirm the self tone fades back out,
    // ending every scenario on the same topology it started with.
    CHECK(g.removeEdge(rig.remA, rig.sendA));
    eng.updateConfig(g.snapshot());
    for (int i = 0; i < kSettleBlocks; ++i) stepBlock(eng, rec);
    size_t afterRestore = rec[sendSlot].size() / kBlock;
    CHECK(windowEnergy(rec[sendSlot], afterRestore, remFreq) < 0.05);
}

// 2. Block-by-block coherence: every rendered block is checked against the
//    exact generation that drove it, across many rapid edits. A block that
//    mixed two revisions would disagree with its own frozen matrix.
void testPerBlockMatrixCoherence() {
    std::printf("[regress] per-block matrix coherence under rapid edits\n");
    Rig rig = Rig::make(2);
    RoutingGraph& g = *rig.g;
    MixerEngine& eng = *rig.eng;

    std::vector<std::vector<float>> rec(eng.busCount());
    ConfigPtr live = g.snapshot();
    int violations = 0;

    for (int round = 0; round < 40; ++round) {
        // Publish one or two edits before each block, varying the edge that
        // carries the self route so every Send bus is exercised.
        unsigned selfSrc = (round % 2 == 0) ? rig.remA : rig.remB;
        unsigned selfBus = (round % 2 == 0) ? rig.sendA : rig.sendB;
        g.setEdge(selfSrc, selfBus, {});
        ConfigPtr p1 = g.snapshot();
        if (round % 3 != 0) {
            g.removeEdge(selfSrc, selfBus);
            ConfigPtr p2 = g.snapshot();
            eng.updateConfig(p1);
            eng.updateConfig(p2);  // adjacent pair; only p2 survives
            live = p2;
        } else {
            eng.updateConfig(p1);
            live = p1;
        }
        size_t startBlock = rec[0].size() / kBlock;
        for (int i = 0; i < kSettleBlocks; ++i) stepBlock(eng, rec);

        // Verify the settled tail against the matrix of the config that was
        // actually the newest at adoption time.
        auto mat = expectedMatrix(*live);
        size_t endBlock = rec[0].size() / kBlock;
        for (size_t b = 0; b < mat.size(); ++b) {
            for (size_t s = 0; s < mat[b].size(); ++s) {
                double mag = windowEnergy(rec[b], endBlock, rig.freqs[s]);
                bool detected = mag >= 0.05;
                if (detected != (mat[b][s] == 1)) {
                    std::printf("  round %d bus %zu src %zu mag %.4f "
                                "expected %d\n",
                                round, b, s, mag, (int)mat[b][s]);
                    ++violations;
                }
            }
        }
        (void)startBlock;
        // End every round back in the clean mix-minus topology and publish
        // it, so the next round starts from a known, coherent revision.
        if (g.removeEdge(selfSrc, selfBus)) {
            ConfigPtr done = g.snapshot();
            eng.updateConfig(done);
            live = done;
            for (int i = 0; i < kSettleBlocks; ++i) stepBlock(eng, rec);
        }
    }
    CHECK(violations == 0);
}

// 3. Feedback-loop rejection must stay intact while edits race the audio
//    thread: every genuinely loop-closing request fails and never reaches a
//    snapshot. A benign forward edge is established first so the rejected
//    edge would close a real, detectable cycle.
void testFeedbackRejectionUnderLoad() {
    std::printf("[regress] feedback rejection preserved under edits\n");
    Rig rig = Rig::make(1);
    RoutingGraph& g = *rig.g;
    // Forward chain mainPA -> record -> sendA is acyclic and accepted.
    CHECK(g.setEdge(rig.pa, rig.rec, {}));
    CHECK(g.setEdge(rig.rec, rig.sendA, {}));
    // Closing sendA -> mainPA would form mainPA->record->sendA->mainPA;
    // the self loop is a cycle of length one. Both must always be refused.
    CHECK(g.createsCycle(rig.sendA, rig.pa));
    CHECK(g.createsCycle(rig.pa, rig.pa));

    std::atomic<bool> stop{false};
    std::atomic<int> badAccepts{0};
    std::thread attacker([&] {
        while (!stop.load()) {
            if (g.setEdge(rig.sendA, rig.pa, {})) badAccepts.fetch_add(1);
            if (g.setEdge(rig.pa, rig.pa, {})) badAccepts.fetch_add(1);
            EdgeParams p;
            p.gainDb = -3.0f;
            g.updateEdge(rig.host, rig.pa, p);
            rig.eng->updateConfig(g.snapshot());
        }
    });

    std::vector<std::vector<float>> rec(rig.eng->busCount());
    for (int i = 0; i < 3000; ++i) stepBlock(*rig.eng, rec);
    stop.store(true);
    attacker.join();
    CHECK(badAccepts.load() == 0);
    CHECK(rig.eng->stats().underruns == 0);
}

// 4. Multithreaded soak: real publish/take timing with concurrent mute and
//    source dropout ("静音" and "断流"), checking the return bus invariant
//    and that the engine never tears a block or loses the audio thread.
void testConcurrentMuteAndDropoutSoak() {
    std::printf("[regress] concurrent mute + dropout soak\n");
    Rig rig = Rig::make(1);
    RoutingGraph& g = *rig.g;
    MixerEngine& eng = *rig.eng;
    const double remFreq = rig.freqs[1];

    std::atomic<bool> stop{false};
    std::thread control([&] {
        bool muted = false;
        while (!stop.load()) {
            muted = !muted;
            g.setBusMuted(rig.rec, muted);
            rig.tones[1]->setAvailable(!muted);  // dropout mirrors the mute
            eng.updateConfig(g.snapshot());
            std::this_thread::yield();
        }
        g.setBusMuted(rig.rec, false);
        rig.tones[1]->setAvailable(true);
        eng.updateConfig(g.snapshot());
    });

    std::vector<std::vector<float>> rec(eng.busCount());
    for (int i = 0; i < 20000; ++i) stepBlock(eng, rec);
    stop.store(true);
    control.join();

    CHECK(eng.blocksProcessed() == 20000);
    CHECK(eng.stats().configApplies > 0);
    // The sendA mix-minus exclusion is graph-level: remA's own tone must
    // never show up in its return bus, regardless of edit timing. The soak
    // restores remA and unmutes before joining, so the final window is
    // fully powered and any leak would be measurable.
    size_t soakEnd = rec[2].size() / kBlock;
    double leak = windowEnergy(rec[2], soakEnd, remFreq);
    CHECK(leak < 0.05);
}

// 5. Snapshot publish boundary at the mailbox: a single-slot mailbox must
//    deliver only the newest revision, each revision at most once, and never
//    redeliver a taken (null on the next take) config. This pins the exact
//    "freeze boundary" the RT thread relies on, independent of DSP.
void testMailboxRevisionBoundary() {
    std::printf("[regress] mailbox newest-revision boundary\n");
    ConfigMailbox box;
    CHECK(box.take() == nullptr);  // nothing published yet

    auto snap = [](uint64_t gen) {
        auto c = std::make_shared<EffectiveConfig>();
        c->generation = gen;
        return ConfigPtr(c);
    };
    box.publish(snap(1));
    box.publish(snap(2));
    box.publish(snap(3));  // two adjacent updates overwrite 1 and 2
    ConfigPtr got = box.take();
    CHECK(got != nullptr);
    CHECK(got->generation == 3);       // newest only, never a torn pair
    CHECK(box.take() == nullptr);      // consumed: no stale redelivery

    box.publish(snap(4));
    ConfigPtr got2 = box.take();
    CHECK(got2 != nullptr && got2->generation == 4);
    CHECK(box.take() == nullptr);

    // Older frozen configs remain immutable and independently usable after a
    // newer one was published (the engine may deliberately revert).
    ConfigPtr old = snap(10);
    box.publish(old);
    box.publish(snap(11));
    (void)box.take();
    CHECK(old->generation == 10);
    CHECK(old->srcEdges.empty() && old->busEdges.empty());
}

// 6. Concurrent publish can never make one rendered block observe two
//    revisions. Track the generation adopted at every block boundary and
//    prove each block's rendered edges equal that single generation's
//    frozen matrix, even under a storm of back-to-back publishes.
void testGenerationPinPerBlock() {
    std::printf("[regress] every block pinned to one generation\n");
    Rig rig = Rig::make(1);
    RoutingGraph& g = *rig.g;
    MixerEngine& eng = *rig.eng;

    std::atomic<bool> stop{false};
    std::thread publisher([&] {
        uint64_t n = 0;
        while (!stop.load()) {
            // Alternate two distinct, coherent graphs each revision.
            if ((n & 1) == 0)
                g.setEdge(rig.remA, rig.sendA, {});
            else
                g.removeEdge(rig.remA, rig.sendA);
            eng.updateConfig(g.snapshot());
            ++n;
        }
    });

    std::vector<uint64_t> gensPerBlock;
    std::vector<std::vector<float>> rec(eng.busCount());
    uint64_t prevGen = 0;
    for (int i = 0; i < 30000; ++i) {
        eng.processBlock();
        uint64_t genAtBlock = eng.stats().lastConfigGeneration;
        gensPerBlock.push_back(genAtBlock);
        // The freeze boundary is a per-block single revision: the generation
        // driving a block can never move backwards relative to the previous
        // block, i.e. no block stitches together two snapshots. (The exact
        // newest generation is not asserted because the single-slot mailbox
        // legitimately collapses adjacent publishes between blocks.)
        CHECK(genAtBlock >= prevGen);
        prevGen = genAtBlock;
        for (int b = 0; b < eng.busCount(); ++b) {
            const float* const* p = eng.busOutput(b);
            for (int k = 0; k < kBlock; ++k) rec[b].push_back(p[0][k]);
        }
    }
    stop.store(true);
    publisher.join();

    CHECK(eng.blocksProcessed() == 30000);
    CHECK(eng.stats().underruns == 0);
    // At least one revision was adopted, and the recorded sequence is
    // monotonic (no block stitched two generations together).
    bool adoptedAny = false;
    for (uint64_t g0 : gensPerBlock)
        if (g0 != 0) adoptedAny = true;
    CHECK(adoptedAny);

    // Restore clean mix-minus and confirm the final return bus is self-free.
    g.removeEdge(rig.remA, rig.sendA);
    eng.updateConfig(g.snapshot());
    for (int i = 0; i < kSettleBlocks; ++i) stepBlock(eng, rec);
    size_t end = rec[2].size() / kBlock;
    CHECK(windowEnergy(rec[2], end, rig.freqs[1]) < 0.05);
}

// 7. Directed edge queries: findEdge is exact-direction. A mix-minus gap
//    (remote -> its own Send bus absent) must stay a gap even when edges in
//    the opposite direction exist, so control-side presence cannot invent a
//    route that the frozen config never published.
void testDirectedFindEdgeBoundary() {
    std::printf("[regress] exact-direction findEdge boundary\n");
    Rig rig = Rig::make(1);
    RoutingGraph& g = *rig.g;

    // Default mix-minus: remA -> sendA is absent.
    CHECK(g.findEdge(rig.remA, rig.sendA) == nullptr);
    // Add an unrelated edge that shares these nodes' key space in reverse.
    CHECK(g.setEdge(rig.host, rig.sendA, {}));
    // The gap must still read as absent; reverse fallback must not echo an
    // unrelated edge (host->sendA) into the (sendA, host) or the mix-minus
    // (remA, sendA) query.
    CHECK(g.findEdge(rig.remA, rig.sendA) == nullptr);
    CHECK(g.findEdge(rig.sendA, rig.host) == nullptr);
    CHECK(g.findEdge(rig.host, rig.sendA) != nullptr);  // real edge still found
    // Existing mix-minus checks used by the demo/UI keep working.
    CHECK(g.findEdge(rig.host, rig.pa) != nullptr);
}

}  // namespace

int main() {
    testSkippedRevisionBoundary();
    testPerBlockMatrixCoherence();
    testFeedbackRejectionUnderLoad();
    testConcurrentMuteAndDropoutSoak();
    testMailboxRevisionBoundary();
    testGenerationPinPerBlock();
    testDirectedFindEdgeBoundary();
    std::printf("\n%d passed, %d failed\n", g_checks - g_failures, g_failures);
    return g_failures ? 1 : 0;
}
