#pragma once

#include "../core/MixerEngine.h"
#include "../core/RoutingGraph.h"
#include "../core/VirtualDevice.h"

#include <QMainWindow>
#include <QTimer>
#include <memory>
#include <vector>

class QTableWidget;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QGroupBox;

namespace mix {

// Defined in the GUI entry point; gives the window access to the virtual
// sound-card inputs so it can simulate temporary input interruptions.
std::shared_ptr<VirtualSource> guiSource(int slot);

// Matrix cell: per-edge gain (dB), mute and per-output channel mapping.
struct CellWidgets {
    QDoubleSpinBox* gain = nullptr;
    QCheckBox* mute = nullptr;
    QCheckBox* present = nullptr;  // edge exists / connect toggle
    QComboBox* mapL = nullptr;     // source channel feeding destination L
    QComboBox* mapR = nullptr;     // source channel feeding destination R
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(RoutingGraph& graph, MixerEngine& engine,
               VirtualDevice& device, QWidget* parent = nullptr);

private slots:
    void onConnectToggled();
    void onEdgeParamChanged();
    void onBusMuteToggled();
    void onInterruptToggled();
    void refreshMetrics();
    void runSelfCheck();

private:
    void buildMatrix();
    void publishConfig();
    int sourceRow(unsigned id) const;
    int busColumn(unsigned id) const;

    RoutingGraph& graph_;
    MixerEngine& engine_;
    VirtualDevice& device_;

    QTableWidget* table_ = nullptr;
    std::vector<std::vector<CellWidgets>> cells_;
    std::vector<QCheckBox*> busMutes_;
    std::vector<QCheckBox*> interrupts_;
    QLabel* metrics_ = nullptr;
    QLabel* selfCheck_ = nullptr;
    QTimer timer_;
};

}  // namespace mix
