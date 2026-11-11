// Headless smoke test: instantiate the real Qt MainWindow on the offscreen
// platform and exercise the same control paths a human operator would use.
#include "../core/MixerEngine.h"
#include "../core/RoutingGraph.h"
#include "../core/Sources.h"
#include "../core/VirtualDevice.h"
#include "../ui/MainWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QTimer>
#include <QWidget>
#include <cstdio>

using namespace mix;

namespace mix {
// The smoke harness owns its sources directly; no shared GUI registry.
std::shared_ptr<VirtualSource> guiSource(int) { return nullptr; }
}  // namespace mix

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    RoutingGraph graph;
    unsigned host = graph.addSource(SourceKind::HostMic, "host", 1);
    unsigned remA = graph.addSource(SourceKind::Remote, "remA", 1, 0);
    graph.addSource(SourceKind::Playback, "play", 2);
    unsigned pa = graph.addBus(BusKind::MainPa, "pa", 2);
    unsigned rec = graph.addBus(BusKind::Record, "rec", 2);
    graph.addBus(BusKind::Send, "sendA", 2, 0);
    graph.addDefaultEdges(1);

    MixerEngine engine(48000, 128, 3, 3);
    engine.setSource(0, std::make_shared<VirtualSource>(
                            host, VirtualSource::Mode::Tone, 200, 0.2,
                            48000, 1));
    engine.setSource(1, std::make_shared<VirtualSource>(
                            remA, VirtualSource::Mode::Tone, 260, 0.2,
                            48000, 1));
    engine.setSource(2, std::make_shared<VirtualSource>(
                            2, VirtualSource::Mode::Tone, 320, 0.15,
                            48000, 2));

    VirtualDevice device(engine, 48000, 128);
    device.start([](const MixerEngine&) {}, true);

    MainWindow window(graph, engine, device);
    window.show();

    int failures = 0;
    // Drive a sequence of GUI mutations across event-loop iterations.
    QTimer::singleShot(50, [&] {
        // Reject a self loop on the PA bus via the graph directly (the UI
        // path shows a modal dialog, which offscreen cannot dismiss; the
        // underlying rejection is what guarantees safety).
        if (graph.setEdge(pa, pa, {})) {
            std::printf("FAIL: self loop accepted\n");
            ++failures;
        }
        // Toggle the record bus mute through the live control.
        graph.setBusMuted(rec, true);
        engine.updateConfig(graph.snapshot());
    });
    QTimer::singleShot(120, [&] {
        EngineStats st = engine.stats();
        if (st.blocksProcessed == 0) {
            std::printf("FAIL: audio thread produced no blocks\n");
            ++failures;
        }

        // Exercise the per-edge channel-mapping combos through the real Qt
        // signal path: every matrix cell owns two (destination L/R).
        const auto combos = window.findChildren<QComboBox*>();
        if (combos.size() < 2) {
            std::printf("FAIL: channel-mapping combos missing (got %d)\n",
                        static_cast<int>(combos.size()));
            ++failures;
        } else {
            const auto& playInfo = graph.sources()[2];  // stereo playback
            const auto& paInfo = graph.buses()[0];
            const EdgeParams* before =
                graph.findEdge(playInfo.id, paInfo.id);
            if (!before) {
                std::printf("FAIL: expected default play->pa edge\n");
                ++failures;
            } else {
                // Matrix cells are created row-major, each with mapL then
                // mapR: row 2 (play), column 0 (pa) holds this mapL combo.
                const int playPaMapL = 2 * 3 * 2;  // 3 buses, 2 combos/cell
                if (combos.size() <= playPaMapL) {
                    std::printf("FAIL: not enough combos for play->pa cell\n");
                    ++failures;
                } else {
                    QComboBox* mapL = combos[playPaMapL];
                    int idx = mapL->findData(1);
                    if (idx < 0) {
                        std::printf("FAIL: combo lacks source-channel-1 item\n");
                        ++failures;
                    } else {
                        mapL->setCurrentIndex(
                            idx);  // fires currentIndexChanged
                        const EdgeParams* after =
                            graph.findEdge(playInfo.id, paInfo.id);
                        if (!after || after->channelMap[0] != 1 ||
                            after->channelMap[1] != before->channelMap[1]) {
                            std::printf("FAIL: channel-map edit did not reach "
                                        "the routing graph\n");
                            ++failures;
                        }
                        // The published frozen config must carry the new map.
                        auto snap = graph.snapshot();
                        bool found = false;
                        for (const SrcEdge& e : snap->srcEdges)
                            if (e.sourceId == playInfo.id &&
                                e.busId == paInfo.id && e.channelMap[0] == 1)
                                found = true;
                        if (!found) {
                            std::printf("FAIL: frozen config missing channel "
                                        "map\n");
                            ++failures;
                        }
                    }
                }
            }
        }

        // Simulate an input interruption and confirm underrun accounting.
        guiSource(0);  // null in this harness (sources created locally)
        auto src0 = std::make_shared<VirtualSource>(
            host, VirtualSource::Mode::Tone, 200, 0.2, 48000, 1);
        engine.setSource(0, src0);
        uint64_t before = engine.stats().underruns;
        src0->setAvailable(false);
        QTimer::singleShot(80, [&, src0, before]() {
            src0->setAvailable(true);
            if (engine.stats().underruns <= before) {
                std::printf("FAIL: interruption did not register underrun\n");
                ++failures;
            }
            std::printf("smoke: blocks=%llu underruns=%llu applies=%llu\n",
                        static_cast<unsigned long long>(
                            engine.stats().blocksProcessed),
                        static_cast<unsigned long long>(
                            engine.stats().underruns),
                        static_cast<unsigned long long>(
                            engine.stats().configApplies));
        });
    });
    QTimer::singleShot(300, [&] {
        device.stop();
        QApplication::quit();
    });

    int rc = app.exec();
    std::printf(failures == 0 ? "UI smoke test: PASS\n"
                              : "UI smoke test: FAIL\n");
    return rc | (failures ? 1 : 0);
}
