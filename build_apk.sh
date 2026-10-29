#!/bin/bash
# Builds build/xnuroid.apk: the loader as libxnuroid.so behind a NativeActivity (android/AndroidManifest.xml).
#   ./build_apk.sh            build
#   ./build_apk.sh install    build, install, push game files if missing, launch
set -euo pipefail
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")"

SDK=$(cygpath -m "${ANDROID_SDK:-/c/Users/${USERNAME}/AppData/Local/Android/Sdk}")
NDK=$SDK/ndk/28.2.13676358
BT=$SDK/build-tools/36.1.0
PLATFORM=$SDK/platforms/android-36/android.jar
CXX=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android31-clang++.cmd
CC=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android31-clang.cmd
ADB=$SDK/platform-tools/adb.exe
GLUE=$NDK/sources/android/native_app_glue
PKG=com.tanosshi.xnuroid
OUT=build/apk

rm -rf "$OUT"
mkdir -p "$OUT/lib/arm64-v8a"

"$CC" -c -O2 -fPIC -I"$GLUE" "$GLUE/android_native_app_glue.c" -o build/native_app_glue.o
"$CXX" -std=c++17 -O3 -fPIC -fvisibility=hidden -c src/gfx/pvrtc.cpp -o build/pvrtc_apk.o
"$CXX" -std=c++17 -O1 -g -fno-omit-frame-pointer -fPIC -fvisibility=hidden -shared -DLOADER_LIBRARY -I"$GLUE" \
    -Isrc/loader -Isrc/objc -Isrc/gfx -Isrc/platform \
    src/loader/android_loader.cpp src/loader/apple_string.cpp src/loader/trampolines.s src/gfx/gles_backend.cpp src/gfx/msl_glsl.cpp src/platform/audio_unit.cpp \
    src/platform/haptics.cpp src/loader/darwin_pthread.cpp src/platform/android_app.cpp build/native_app_glue.o build/pvrtc_apk.o \
    -u ANativeActivity_onCreate -static-libstdc++ -landroid -llog -lEGL -lGLESv3 -lz -laaudio -ldl \
    -o "$OUT/lib/arm64-v8a/libxnuroid.so"

# The app's linker namespace cannot load /system/lib64/libc++.so; ship the phone's own copy under another name.
if [ ! -f build/libsysc++.so ]; then "$ADB" pull /system/lib64/libc++.so build/libsysc++.so >/dev/null; fi
cp build/libsysc++.so "$OUT/lib/arm64-v8a/libsysc++.so"

"$BT/aapt2.exe" link -o build/base.apk --manifest android/AndroidManifest.xml -I "$PLATFORM" \
    --min-sdk-version 31 --target-sdk-version 33
python -I - build/base.apk "$OUT" <<'EOF'
import os, sys, zipfile
apk, root = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk, "a", zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(os.path.join(root, "lib")):
        for f in files:
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, root).replace(os.sep, "/"))
EOF
"$BT/zipalign.exe" -f -P 16 4 build/base.apk build/aligned.apk
"$BT/apksigner.bat" sign --ks "$(cygpath -m "$HOME/.android/debug.keystore")" --ks-pass pass:android --key-pass pass:android \
    --out build/xnuroid.apk build/aligned.apk
echo "built build/xnuroid.apk"

if [ "${1:-}" = "install" ]; then
    "$ADB" install -r build/xnuroid.apk
    # The app cannot read files adb writes into its dirs; stream the shell-run layout in as the app user instead.
    if ! "$ADB" shell "run-as $PKG test -f files/game/UnityFramework"; then
        "$ADB" shell "cd /data/local/tmp/game && tar -chf - . | run-as $PKG sh -c 'mkdir -p files/game && tar -C files/game -xf -'" || true
    fi
    "$ADB" shell "rm -f /sdcard/Android/data/$PKG/files/exit.txt; am start -S -n $PKG/android.app.NativeActivity"
fi
