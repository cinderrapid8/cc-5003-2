# Conference mixing matrix — local-prefix Qt5 build (no system install needed)
CXX ?= g++
CXXSTD := -std=c++17
WARN := -Wall -Wextra -Wpedantic
OPT := -O2 -g

PREFIX ?= /workspace/.localdeb/prefix
QT_INC := $(PREFIX)/usr/include/aarch64-linux-gnu/qt5 \
          $(PREFIX)/usr/include/aarch64-linux-gnu/qt5/QtCore \
          $(PREFIX)/usr/include/aarch64-linux-gnu/qt5/QtGui \
          $(PREFIX)/usr/include/aarch64-linux-gnu/qt5/QtWidgets
QT_LIBDIR := $(PREFIX)/usr/lib/aarch64-linux-gnu
MOC := $(PREFIX)/usr/lib/qt5/bin/moc

CORE_CXX := $(CXXSTD) $(WARN) $(OPT) -pthread -Icore
CORE_SRCS := $(wildcard core/*.cpp)
CORE_OBJS := $(patsubst core/%.cpp,build/obj/%.o,$(CORE_SRCS))

.PHONY: all clean test demo ui run-test run-demo regress
all: build/mixtests build/regress_conc build/mixdemo build/mixui

build/obj:
	mkdir -p build/obj

build/obj/%.o: core/%.cpp | build/obj
	$(CXX) $(CORE_CXX) -c $< -o $@

build/mixtests: tests/tests.cpp $(CORE_OBJS) | build/obj
	$(CXX) $(CORE_CXX) $^ -o $@ -pthread

# Routing snapshot / concurrency boundary regression (matrix competition).
build/regress_conc: tests/regression_concurrency.cpp $(CORE_OBJS) | build/obj
	$(CXX) $(CORE_CXX) $^ -o $@ -pthread

build/mixdemo: app/main.cpp $(CORE_OBJS) | build/obj
	$(CXX) $(CORE_CXX) $^ -o $@ -pthread

# ---- Qt UI ----
UI_FLAGS := $(addprefix -I,$(QT_INC))
UI_LIBS := -L$(QT_LIBDIR) -lQt5Widgets -lQt5Gui -lQt5Core -Wl,-rpath,$(QT_LIBDIR)

build/obj/moc_MainWindow.cpp: ui/MainWindow.h | build/obj
	$(MOC) $(UI_FLAGS) $< -o $@

build/obj/moc_MainWindow.o: build/obj/moc_MainWindow.cpp
	$(CXX) $(CXXSTD) $(WARN) $(OPT) $(UI_FLAGS) -c $< -o $@

build/obj/MainWindow.o: ui/MainWindow.cpp ui/MainWindow.h
	$(CXX) $(CXXSTD) $(WARN) $(OPT) $(UI_FLAGS) -Icore -c $< -o $@

build/obj/ui_main.o: app/ui_main.cpp
	$(CXX) $(CXXSTD) $(WARN) $(OPT) $(UI_FLAGS) -Icore -c $< -o $@

build/mixui: build/obj/ui_main.o build/obj/MainWindow.o build/obj/moc_MainWindow.o $(CORE_OBJS)
	$(CXX) $(CXXSTD) $(WARN) $(OPT) $^ -o $@ $(UI_LIBS) -pthread

build/ui_smoke: tests/ui_smoke.cpp build/obj/MainWindow.o build/obj/moc_MainWindow.o $(CORE_OBJS)
	$(CXX) $(CXXSTD) $(WARN) $(OPT) $(UI_FLAGS) -Icore $< build/obj/MainWindow.o build/obj/moc_MainWindow.o $(CORE_OBJS) -o $@ $(UI_LIBS) -pthread

regress: build/regress_conc
	./build/regress_conc

test: build/mixtests build/regress_conc
	./build/mixtests
	./build/regress_conc

demo: build/mixdemo
	./build/mixdemo

# Headless UI exercise; uses the offscreen platform plugin from the prefix.
ui-smoke: build/ui_smoke
	LD_LIBRARY_PATH=$(QT_LIBDIR):$(PREFIX)/lib/aarch64-linux-gnu:$$LD_LIBRARY_PATH \
	QT_QPA_PLATFORM=offscreen \
	QT_QPA_PLATFORM_PLUGIN_PATH=$(QT_LIBDIR)/qt5/plugins/platforms \
	./build/ui_smoke

run-test: test
run-demo: demo

clean:
	rm -rf build/obj build/mixtests build/regress_conc build/mixdemo build/mixui build/ui_smoke build/mixtests_asan build/mixtests_tsan build/regress_conc_asan
