#!/bin/bash
# Builds the command-line loader (xnuroid_loader) run from adb shell.
set -euo pipefail
cd "$(dirname "$0")"
SDK=$(cygpath -m "${ANDROID_SDK:-/c/Users/${USERNAME}/AppData/Local/Android/Sdk}")
CXX=$SDK/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android31-clang++.cmd
mkdir -p build
# hot pixel loops are built optimized on their own
"$CXX" -std=c++17 -O3 -c src/gfx/pvrtc.cpp -o build/pvrtc_cli.o
"$CXX" -std=c++17 -O1 -g -fno-omit-frame-pointer \
    -Isrc/loader -Isrc/objc -Isrc/gfx -Isrc/platform \
    src/loader/android_loader.cpp src/loader/apple_string.cpp src/loader/trampolines.s src/gfx/gles_backend.cpp src/gfx/msl_glsl.cpp src/platform/audio_unit.cpp src/platform/haptics.cpp src/loader/darwin_pthread.cpp \
    build/pvrtc_cli.o \
    -static-libstdc++ -ldl -lEGL -lGLESv3 -lz -laaudio -o xnuroid_loader
