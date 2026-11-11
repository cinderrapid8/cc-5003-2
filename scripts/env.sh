# Source the local, rootless Qt5 toolchain prefix (downloaded into
# .localdeb by fetch_qt.py) so the UI binary finds its libs and plugins.
PREFIX="${PREFIX:-/workspace/.localdeb/prefix}"
export LD_LIBRARY_PATH="$PREFIX/usr/lib/aarch64-linux-gnu:$PREFIX/lib/aarch64-linux-gnu:$LD_LIBRARY_PATH"
export QT_PLUGIN_PATH="$PREFIX/usr/lib/aarch64-linux-gnu/qt5/plugins"
# Headless environments use the offscreen platform; override on a real desktop.
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
export QT_QPA_PLATFORM_PLUGIN_PATH="$PREFIX/usr/lib/aarch64-linux-gnu/qt5/plugins/platforms"
