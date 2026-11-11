#!/usr/bin/env bash
# Headless UI smoke build under ASan/UBSan (not part of the normal Makefile).
set -euo pipefail
cd "$(dirname "$0")/.."

PREFIX=/workspace/.localdeb/prefix
QT_INC="$PREFIX/usr/include/aarch64-linux-gnu/qt5
$PREFIX/usr/include/aarch64-linux-gnu/qt5/QtCore
$PREFIX/usr/include/aarch64-linux-gnu/qt5/QtGui
$PREFIX/usr/include/aarch64-linux-gnu/qt5/QtWidgets"
QT_LIBDIR=$PREFIX/usr/lib/aarch64-linux-gnu
UI_FLAGS="$(echo "$QT_INC" | sed 's|^|-I|;s|\n| -I|g')"
SAN="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"
O=build/obj

g++ -std=c++17 -Wall -Wextra -Wpedantic $SAN $UI_FLAGS -Icore \
    -c ui/MainWindow.cpp -o $O/MainWindow_asan.o
"$QT_LIBDIR/qt5/bin/moc" $UI_FLAGS ui/MainWindow.h -o $O/moc_MainWindow_asan.cpp
g++ -std=c++17 -Wall -Wextra -Wpedantic $SAN $UI_FLAGS \
    -c $O/moc_MainWindow_asan.cpp -o $O/moc_MainWindow_asan.o
for f in Analyzer MixerEngine RoutingGraph Sources VirtualDevice WavWriter; do
    g++ -std=c++17 -Wall -Wextra -Wpedantic $SAN -pthread -Icore \
        -c core/$f.cpp -o $O/${f}_asan.o
done
g++ -std=c++17 -Wall -Wextra -Wpedantic $SAN $UI_FLAGS -Icore \
    tests/ui_smoke.cpp \
    $O/MainWindow_asan.o $O/moc_MainWindow_asan.o \
    $O/Analyzer_asan.o $O/MixerEngine_asan.o $O/RoutingGraph_asan.o \
    $O/Sources_asan.o $O/VirtualDevice_asan.o $O/WavWriter_asan.o \
    -o build/ui_smoke_asan -L$QT_LIBDIR -lQt5Widgets -lQt5Gui -lQt5Core \
    -Wl,-rpath,$QT_LIBDIR -pthread

# LeakSanitizer is disabled: the only leaks at exit are inside Qt's offscreen
# platform plugin (QScreen/QPlatformCursor teardown), not in project code.
ASAN_OPTIONS=detect_leaks=0 \
LD_LIBRARY_PATH=$QT_LIBDIR:$PREFIX/lib/aarch64-linux-gnu \
QT_QPA_PLATFORM=offscreen \
QT_QPA_PLATFORM_PLUGIN_PATH=$QT_LIBDIR/qt5/plugins/platforms \
./build/ui_smoke_asan
