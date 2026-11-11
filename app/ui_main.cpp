// Qt GUI entry: builds the conference topology, wires virtual sound-card
// inputs, starts the real-time device thread, and opens the matrix window.
#include "../core/MixerEngine.h"
#include "../core/RoutingGraph.h"
#include "../core/Sources.h"
#include "../core/VirtualDevice.h"
#include "../ui/MainWindow.h"

#include <QApplication>
#include <memory>
#include <vector>

using namespace mix;

namespace {
constexpr int kSampleRate = 48000;
constexpr int kBlock = 128;

std::vector<std::shared_ptr<VirtualSource>> g_sources;
}  // namespace

namespace mix {
// Gives MainWindow access to the virtual sound-card inputs so it can
// simulate temporary input interruptions and observe underruns.
std::shared_ptr<VirtualSource> guiSource(int slot) {
    if (slot < 0 || slot >= static_cast<int>(g_sources.size()))
        return nullptr;
    return g_sources[slot];
}
}  // namespace mix

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    RoutingGraph graph;
    unsigned host = graph.addSource(SourceKind::HostMic, "主持话筒", 1);
    unsigned remA = graph.addSource(SourceKind::Remote, "远端A", 1, 0);
    unsigned remB = graph.addSource(SourceKind::Remote, "远端B", 1, 1);
    unsigned play = graph.addSource(SourceKind::Playback, "现场播放", 2);
    graph.addBus(BusKind::MainPa, "主扩", 2);
    graph.addBus(BusKind::Record, "录制", 2);
    graph.addBus(BusKind::Send, "返送A", 2, 0);
    graph.addBus(BusKind::Send, "返送B", 2, 1);
    graph.addDefaultEdges(2);

    MixerEngine engine(kSampleRate, kBlock, 4, 4);

    // Distinct signature frequencies per input for the routing self-check.
    g_sources = {
        std::make_shared<VirtualSource>(host, VirtualSource::Mode::Tone,
                                        200.0, 0.2, kSampleRate, 1),
        std::make_shared<VirtualSource>(remA, VirtualSource::Mode::Tone,
                                        260.0, 0.2, kSampleRate, 1),
        std::make_shared<VirtualSource>(remB, VirtualSource::Mode::Pulse,
                                        9.0, 0.25, kSampleRate, 1),
        std::make_shared<VirtualSource>(play, VirtualSource::Mode::Tone,
                                        320.0, 0.15, kSampleRate, 2),
    };
    for (int i = 0; i < static_cast<int>(g_sources.size()); ++i)
        engine.setSource(i, g_sources[i]);
    engine.updateConfig(graph.snapshot());

    VirtualDevice device(engine, kSampleRate, kBlock);
    device.start([](const MixerEngine&) { /* metrics polled via SPSC */ },
                 /*realtime=*/true);

    mix::MainWindow window(graph, engine, device);
    window.resize(1100, 600);
    window.show();

    int rc = app.exec();
    device.stop();
    return rc;
}
