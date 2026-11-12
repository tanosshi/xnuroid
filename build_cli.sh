#!/bin/bash
# Builds the command-line loader (xnuroid_loader) run from adb shell.
set -euo pipefail
cd "$(dirname "$0")"
SDK=$(cygpath -m "${ANDROID_SDK:-/c/Users/${USERNAME}/AppData/Local/Android/Sdk}")
CXX=$SDK/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android31-clang++.cmd
CC=${CXX%++.cmd}.cmd
mkdir -p build
# hot pixel loops are built optimized on their own
"$CXX" -std=c++17 -O3 -c src/gfx/pvrtc.cpp -o build/pvrtc_cli.o
# iOS ships libsqlite3; the amalgamation is built in and exported so guest imports bind to it
[ build/sqlite3_cli.o -nt src/third_party/sqlite/sqlite3.c ] || "$CC" -c -O2 -fPIC -DSQLITE_THREADSAFE=1 -DSQLITE_TEMP_STORE=3 -DSQLITE_OMIT_LOAD_EXTENSION=1 -DHAVE_USLEEP=1 src/third_party/sqlite/sqlite3.c -o build/sqlite3_cli.o
"$CXX" -std=c++17 -O1 -g -fno-omit-frame-pointer \
    -Isrc/loader -Isrc/objc -Isrc/gfx -Isrc/platform -Isrc/app \
    src/loader/android_loader.cpp src/loader/apple_string.cpp src/loader/trampolines.s src/gfx/gles_backend.cpp src/gfx/msl_glsl.cpp src/platform/audio_unit.cpp src/platform/haptics.cpp src/loader/darwin_pthread.cpp \
    src/app/app_runtime.cpp src/app/app_foundation.cpp src/app/app_foundation2.cpp src/app/app_plist.cpp src/app/app_uikit.cpp src/app/app_nib.cpp src/app/app_gl.cpp src/app/app_audio.cpp src/app/app_cg.cpp src/app/app_misc.cpp src/app/app_patches.cpp src/app/app_darwin.cpp src/app/app_crypto.cpp src/app/app_av.cpp src/app/app_main.cpp src/app/app_msgsend.s \
    build/pvrtc_cli.o build/sqlite3_cli.o -Wl,--export-dynamic-symbol='sqlite3_*' \
    -static-libstdc++ -ldl -lEGL -lGLESv3 -lz -laaudio -landroid -ljnigraphics -o xnuroid_loader
