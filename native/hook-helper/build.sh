#!/bin/sh
# Builds the hook helper and its tests: on macOS with Apple clang (Xcode Command Line Tools), on Linux with the
# system C++ compiler and the libevdev headers.
set -eu
cd "$(dirname "$0")"
mkdir -p build
if [ "$(uname -s)" = Linux ]; then
  FLAGS="-std=c++20 -Wall -Wextra"
  c++ $FLAGS -O2 src/main_posix.cpp src/hooks_linux.cpp src/linuxinput.cpp src/simulate.cpp src/commands.cpp \
    src/output.cpp src/json.cpp src/decision.cpp \
    $(pkg-config --cflags --libs libevdev) -pthread -o build/hook-helper
  c++ $FLAGS -O0 tests/helper_tests.cpp src/decision.cpp src/commands.cpp src/macinput.cpp src/linuxinput.cpp \
    src/json.cpp src/output.cpp -pthread -o build/helper_tests
else
  FLAGS="-std=c++20 -arch arm64 -mmacosx-version-min=12.0 -Wall -Wextra"
  clang++ $FLAGS -O2 src/main_posix.cpp src/hooks_mac.cpp src/macinput.cpp src/simulate.cpp src/commands.cpp \
    src/output.cpp src/json.cpp src/decision.cpp \
    -framework ApplicationServices -framework CoreFoundation -o build/hook-helper
  clang++ $FLAGS -O0 tests/helper_tests.cpp src/decision.cpp src/commands.cpp src/macinput.cpp src/linuxinput.cpp \
    src/json.cpp src/output.cpp -o build/helper_tests
fi
echo "Built build/hook-helper and build/helper_tests"
