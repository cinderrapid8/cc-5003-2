#include "MainWindow.h"

#include "../core/Analyzer.h"
#include "../core/Sources.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace mix {

MainWindow::MainWindow(RoutingGraph& graph, MixerEngine& engine,
                       VirtualDevice& device, QWidget* parent)
    : QMainWindow(parent), graph_(graph), engine_(engine), device_(device) {
    setWindowTitle("会议音控混音矩阵 / Conference Mixing Matrix");
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* root = new QVBoxLayout(central);

    // ---- routing matrix ----------------------------------------------
    auto* matrixBox = new QGroupBox("路由矩阵 (源 → 总线)", central);
    auto* matrixLay = new QVBoxLayout(matrixBox);
    table_ = new QTableWidget(matrixBox);
    matrixLay->addWidget(table_);
    root->addWidget(matrixBox);

    // ---- bus mutes + input interrupts --------------------------------
    auto* ctrlBox = new QGroupBox("总线静音 / 输入中断模拟", central);
    auto* ctrlLay = new QGridLayout(ctrlBox);
    root->addWidget(ctrlBox);

    // ---- self check + metrics ----------------------------------------
    auto* checkBtn = new QPushButton("运行虚拟声卡路由自检", central);
    connect(checkBtn, &QPushButton::clicked, this,
            &MainWindow::runSelfCheck);
    root->addWidget(checkBtn);
    selfCheck_ = new QLabel("自检: 尚未运行", central);
    root->addWidget(selfCheck_);
    metrics_ = new QLabel(central);
    metrics_->setStyleSheet("font-family: monospace;");
    root->addWidget(metrics_);

    buildMatrix();

    // Bus mute controls.
    const auto& buses = graph_.buses();
    for (size_t b = 0; b < buses.size(); ++b) {
        auto* cb = new QCheckBox(
            QString("静音 %1").arg(QString::fromStdString(buses[b].name)),
            ctrlBox);
        cb->setChecked(buses[b].muted);
        cb->setProperty("busId", static_cast<int>(buses[b].id));
        connect(cb, &QCheckBox::toggled, this, &MainWindow::onBusMuteToggled);
        ctrlLay->addWidget(cb, 0, static_cast<int>(b));
        busMutes_.push_back(cb);
    }
    // Input interruption switches (virtual sound card sources only).
    for (size_t s = 0; s < graph_.sources().size(); ++s) {
        auto* cb = new QCheckBox(
            QString("中断 %1").arg(
                QString::fromStdString(graph_.sources()[s].name)),
            ctrlBox);
        cb->setProperty("sourceSlot", static_cast<int>(s));
        connect(cb, &QCheckBox::toggled, this,
                &MainWindow::onInterruptToggled);
        ctrlLay->addWidget(cb, 1, static_cast<int>(s));
        interrupts_.push_back(cb);
    }

    // Publish the initial frozen config and start polling observability.
    publishConfig();
    connect(&timer_, &QTimer::timeout, this, &MainWindow::refreshMetrics);
    timer_.start(100);  // 10 Hz GUI refresh, decoupled from the audio clock
}

int MainWindow::sourceRow(unsigned id) const {
    for (size_t i = 0; i < graph_.sources().size(); ++i)
        if (graph_.sources()[i].id == id) return static_cast<int>(i);
    return -1;
}

int MainWindow::busColumn(unsigned id) const {
    for (size_t i = 0; i < graph_.buses().size(); ++i)
        if (graph_.buses()[i].id == id) return static_cast<int>(i);
    return -1;
}

void MainWindow::buildMatrix() {
    const auto& srcs = graph_.sources();
    const auto& buses = graph_.buses();
    table_->setRowCount(static_cast<int>(srcs.size()));
    table_->setColumnCount(static_cast<int>(buses.size()));
    table_->setVerticalHeaderLabels([&] {
        QStringList labels;
        for (const auto& s : srcs)
            labels << QString::fromStdString(s.name);
        return labels;
    }());
    table_->setHorizontalHeaderLabels([&] {
        QStringList labels;
        for (const auto& b : buses)
            labels << QString::fromStdString(b.name);
        return labels;
    }());

    cells_.assign(srcs.size(),
                  std::vector<CellWidgets>(buses.size()));
    for (size_t r = 0; r < srcs.size(); ++r) {
        for (size_t c = 0; c < buses.size(); ++c) {
            auto* holder = new QWidget();
            auto* lay = new QGridLayout(holder);
            lay->setContentsMargins(2, 2, 2, 2);

            auto* present = new QCheckBox("连接", holder);
            auto* gain = new QDoubleSpinBox(holder);
            gain->setRange(-60.0, 12.0);
            gain->setSuffix(" dB");
            gain->setSingleStep(0.5);
            gain->setValue(0.0);
            auto* mute = new QCheckBox("静音", holder);
            auto* mapL = new QComboBox(holder);
            auto* mapR = new QComboBox(holder);
            for (auto* combo : {mapL, mapR}) {
                combo->addItem("静音(-1)", -1);
                combo->addItem("源声道 0", 0);
                combo->addItem("源声道 1", 1);
            }

            lay->addWidget(present, 0, 0);
            lay->addWidget(gain, 0, 1);
            lay->addWidget(mute, 0, 2);
            lay->addWidget(new QLabel("L", holder), 0, 3);
            lay->addWidget(mapL, 0, 4);
            lay->addWidget(new QLabel("R", holder), 0, 5);
            lay->addWidget(mapR, 0, 6);
            table_->setCellWidget(static_cast<int>(r), static_cast<int>(c),
                                  holder);

            CellWidgets cw;
            cw.present = present;
            cw.gain = gain;
            cw.mute = mute;
            cw.mapL = mapL;
            cw.mapR = mapR;
            cells_[r][c] = cw;

            const EdgeParams* e =
                graph_.findEdge(srcs[r].id, buses[c].id);
            present->setChecked(e != nullptr);
            if (e) {
                gain->setValue(e->gainDb);
                mute->setChecked(e->muted);
                mapL->setCurrentIndex(mapL->findData(e->channelMap[0]));
                mapR->setCurrentIndex(mapR->findData(e->channelMap[1]));
            }
            gain->setEnabled(e != nullptr);
            mute->setEnabled(e != nullptr);
            mapL->setEnabled(e != nullptr);
            mapR->setEnabled(e != nullptr);

            connect(present, &QCheckBox::toggled, this,
                    &MainWindow::onConnectToggled);
            connect(gain,
                    QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                    &MainWindow::onEdgeParamChanged);
            connect(mute, &QCheckBox::toggled, this,
                    &MainWindow::onEdgeParamChanged);
            connect(mapL, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, &MainWindow::onEdgeParamChanged);
            connect(mapR, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, &MainWindow::onEdgeParamChanged);
        }
    }
    table_->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    table_->verticalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
}

void MainWindow::onConnectToggled() {
    auto* present = qobject_cast<QCheckBox*>(sender());
    if (!present) return;
    // Locate the cell this widget belongs to.
    for (size_t r = 0; r < cells_.size(); ++r) {
        for (size_t c = 0; c < cells_[r].size(); ++c) {
            if (cells_[r][c].present != present) continue;
            unsigned src = graph_.sources()[r].id;
            unsigned bus = graph_.buses()[c].id;
            if (present->isChecked()) {
                EdgeParams p;
                p.gainDb = static_cast<float>(cells_[r][c].gain->value());
                p.muted = cells_[r][c].mute->isChecked();
                p.channelMap[0] =
                    cells_[r][c].mapL->currentData().toInt();
                p.channelMap[1] =
                    cells_[r][c].mapR->currentData().toInt();
                if (!graph_.setEdge(src, bus, p)) {
                    // Rejected: feedback loop or illegal destination.
                    QMessageBox::warning(
                        this, "路由被拒绝",
                        "该连接会形成反馈环或指向非法节点，已回滚。");
                    QSignalBlocker b(present);
                    present->setChecked(false);
                    return;
                }
                cells_[r][c].gain->setEnabled(true);
                cells_[r][c].mute->setEnabled(true);
                cells_[r][c].mapL->setEnabled(true);
                cells_[r][c].mapR->setEnabled(true);
            } else {
                graph_.removeEdge(src, bus);
                cells_[r][c].gain->setEnabled(false);
                cells_[r][c].mute->setEnabled(false);
                cells_[r][c].mapL->setEnabled(false);
                cells_[r][c].mapR->setEnabled(false);
            }
            publishConfig();
            return;
        }
    }
}

void MainWindow::onEdgeParamChanged() {
    for (size_t r = 0; r < cells_.size(); ++r) {
        for (size_t c = 0; c < cells_[r].size(); ++c) {
            const auto& cw = cells_[r][c];
            if (cw.gain == sender() || cw.mute == sender() ||
                cw.mapL == sender() || cw.mapR == sender()) {
                if (!cw.present->isChecked()) return;
                EdgeParams p;
                p.gainDb = static_cast<float>(cw.gain->value());
                p.muted = cw.mute->isChecked();
                p.channelMap[0] = cw.mapL->currentData().toInt();
                p.channelMap[1] = cw.mapR->currentData().toInt();
                graph_.updateEdge(graph_.sources()[r].id,
                                  graph_.buses()[c].id, p);
                publishConfig();
                return;
            }
        }
    }
}

void MainWindow::onBusMuteToggled() {
    auto* cb = qobject_cast<QCheckBox*>(sender());
    if (!cb) return;
    unsigned bus = static_cast<unsigned>(cb->property("busId").toInt());
    graph_.setBusMuted(bus, cb->isChecked());
    publishConfig();
}

void MainWindow::onInterruptToggled() {
    auto* cb = qobject_cast<QCheckBox*>(sender());
    if (!cb) return;
    int slot = cb->property("sourceSlot").toInt();
    // The GUI owns the virtual sources created in ui_main; toggle their
    // availability flag, which makes pull() report an underrun.
    auto src = guiSource(slot);
    if (src) src->setAvailable(!cb->isChecked());
}

void MainWindow::publishConfig() {
    // Every GUI edit builds a brand-new immutable snapshot. The audio
    // thread swaps it once per block, so dragging a slider can never leave
    // one block reading half-old, half-new parameters.
    engine_.updateConfig(graph_.snapshot());
}

void MainWindow::refreshMetrics() {
    EngineStats st = engine_.stats();
    QString peak;
    for (size_t i = 0; i < st.busPeak.size(); ++i)
        peak += QString(" B%1=%2")
                    .arg(i)
                    .arg(st.busPeak[i], 0, 'f', 3);
    metrics_->setText(
        QString("块数 %1 | 回调 avg %2 us / max %3 us (预算 %4 us)\n"
                "欠载 %5 | 削波样本 %6 | 拒绝块 %7 | 配置应用 %8 (代 %9)\n峰值:%10")
            .arg(st.blocksProcessed)
            .arg(st.callbackAvgUs, 0, 'f', 1)
            .arg(st.callbackMaxUs, 0, 'f', 1)
            .arg(st.callbackBudgetUs, 0, 'f', 0)
            .arg(st.underruns)
            .arg(st.clippedSamples)
            .arg(st.rejectedBlocks)
            .arg(st.configApplies)
            .arg(st.lastConfigGeneration)
            .arg(peak));
}

void MainWindow::runSelfCheck() {
    // Render offline against the current graph and verify, from the actual
    // output audio, which sources each bus contains.
    const auto& srcs = graph_.sources();
    const auto& buses = graph_.buses();
    const int blocks = 400;
    std::vector<std::vector<float>> rec(buses.size());
    VirtualDevice probe(engine_, engine_.sampleRate(), engine_.blockSize());
    probe.runBlocks(static_cast<size_t>(blocks),
                    [&](const MixerEngine& e) {
                        for (size_t b = 0; b < buses.size(); ++b) {
                            const float* const* o =
                                e.busOutput(static_cast<int>(b));
                            for (int i = 0; i < engine_.blockSize(); ++i)
                                rec[b].push_back(o[0][i]);
                        }
                    });

    std::vector<double> freqs;
    for (const auto& s : srcs) freqs.push_back(200.0 + 60.0 * s.id);
    auto cfg = graph_.snapshot();
    std::vector<std::vector<char>> expected(
        buses.size(), std::vector<char>(srcs.size(), 0));
    for (const SrcEdge& e : cfg->srcEdges) {
        int bi = busColumn(e.busId), si = sourceRow(e.sourceId);
        if (bi >= 0 && si >= 0) expected[bi][si] = 1;
    }
    MatrixReport rep = verifyRouting(rec, freqs, expected,
                                     engine_.sampleRate());
    selfCheck_->setText(QString::fromStdString(
        (rep.pass ? "自检通过 ✓  " : "自检失败 ✗  ") + rep.toString()));
}

}  // namespace mix
