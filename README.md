# xnudroid

xnudroid runs a decrypted iOS arm64 Mach-O: a Unity IL2CPP game inside a normal Android arm64
process. The iOS image is mapped, rebound, and executed natively. Calls into Darwin libc, dyld, Mach,
pthread, Foundation, UIKit, Metal, and AudioUnit are handled by assembly trampolines, shims, fake
objects, or translation layers.

This is **not a general-purpose iOS emulator**. Development has targeted a single commercial Unity
IL2CPP title on one physical arm64 device. The Android app is the playable host; the command-line
build is the diagnostic host. Status below reflects the repository history and code; no device run
was performed during this documentation pass.

## Status

| Area | State | Evidence |
| :--- | :--- | :--- |
| Mach-O load | VERIFIED | The tested image maps, rebases, and binds on Android arm64; headless boot run logs exist. |
| IL2CPP boot | VERIFIED | `_il2cpp_init` returns 1; assemblies and classes enumerate; `il2cpp_string_new` works. |
| Managed invoke | VERIFIED | `Math.Max(7, 42)` through `il2cpp_runtime_invoke` returns 42. |
| Unity engine startup | VERIFIED | `LOADER_ENGINE=8` launches `UnityAppController` and ticks the main run loop. Commit history records the loading screen, title screen, and gameplay. |
| Rendering | VERIFIED | Metal is emulated on OpenGL ES 3.2; runtime MSL is translated to GLSL ES 3.20; drawables are presented. |
| Input | VERIFIED | NativeActivity touch events become `UITouch` events; scripted taps are supported. |
| Audio | VERIFIED | FMOD output is played through AAudio. |
| Haptics | VERIFIED | UIKit feedback generators drive the Android vibrator through JNI. |
| Managed exceptions | PARTIAL | Compact-unwind FDE synthesis and culture-free throw tests exist, but the captured logs in this repo predate that work. |
| General iOS support | TODO | Only this title and device have been exercised. |

- **VERIFIED** means observed in a real run or recorded in the project's commit history.
- **INFERRED** means a theory that has not been confirmed.
- **TODO** means not implemented.

## How it works

**Loading.** The loader parses the thin arm64 Mach-O, maps its segments, applies rebases and binds,
and executes `__mod_init_func`. The image and the generated trampoline pool live in one combined
reservation so that branch instructions and TLS veneers stay within +/-128 MB.

**Calls and ABI.** Each import gets a trampoline that loads a `TrampolineEntry*` into `x17` and the
target into `x16`, leaving `x0`-`x7` intact for the shim. Shims cover Darwin/bionic differences:
variadic arguments, 192-byte `jmp_buf`, `stat`/`dirent` layouts, `open`/`fcntl`/`ioctl` flags,
`qsort_r` argument order, pthread attributes, Darwin TSD, dyld, Mach, and GCD calls.

**Engine and frameworks.** In engine mode the guest’s own Objective-C classes and categories are
registered, so messages reach real method implementations. Foundation and UIKit are stateful fakes:
views keep frames and layers, windows keep root controllers, `CAMetalLayer` hands out drawables,
`CADisplayLink` fires, containers store objects, and the main run loop delivers timers, queued
`dispatch_async` blocks, and touches. Blocks and `__block` variables are copied to the heap.

**Graphics.** Metal objects are emulated on GLES 3.2: devices, queues, buffers, textures, samplers,
depth-stencil states, render pipelines, encoders, and render passes. Runtime-compiled MSL is
translated to GLSL ES 3.20. ASTC, ETC2, and PVRTC textures are supported; PVRTC decodes on the GPU
with a compute shader and falls back to multithreaded CPU decoding. Presenting a drawable blits it to
the NativeActivity window.

**Audio and haptics.** AudioUnit output is routed to AAudio. If no device is available, or
`LOADER_AUDIO=0` is set, a timed thread drives the callback and discards samples. UIKit impact,
notification, and selection feedback generators call the Android vibrator.

**Unwinding.** The guest relies mostly on compact unwind. The loader parses `__unwind_info`,
synthesizes DWARF FDEs, and registers them with bionic’s LLVM libunwind. Each function gets one
coarse CFI row; the guest’s own `__eh_frame` FDEs are registered in place. Registration can cover
every function or only offsets listed in a file.

## Requirements

- Windows with Git Bash/MSYS (the build scripts use `cygpath`) and a working `adb`
- Android NDK `28.2.13676358`
- Android SDK build-tools `36.1.0` and platform `android-36`
- An arm64 Android device, API 31 or newer
- A **decrypted** IPA containing an arm64 Unity IL2CPP build (FairPlay encryption is not handled)

The build scripts hardcode the SDK location as `C:\Users\user\AppData\Local\Android\Sdk`
(overridable with `ANDROID_SDK`). Edit the version variables if your SDK differs.

### Pointing the analysis scripts at your IPA

The scripts in the repository root read the guest bundle location from
`game_config.py`, which ships empty. Extract your decrypted IPA and create a
local, git-ignored `game_config_local.py` beside it:

```python
GAME_ROOT = "path/to/extracted-ipa"   # folder that contains Payload/
APP_NAME = "YourApp"                  # YourApp.app lives inside Payload/
```

You can also set the `LOADER_GAME_DIR` and `LOADER_APP_NAME` environment
variables instead. `run_phases.ps1` accepts the same folder through `-GameDir`.

## Build

### Build the command-line loader

```bash
./build_cli.sh
```

This produces `xnuroid_loader` in the repository root. It links EGL, GLESv3, zlib, and AAudio.

### Build the Android app

```bash
./build_apk.sh
./build_apk.sh install
```

The first command builds `build/xnuroid.apk`. The `install` command builds, installs, copies
game files if missing, and launches the app.

The APK package is `com.tanosshi.xnuroid`, the native library is `libxnuroid.so`, and the
activity is `android.app.NativeActivity`. The install script pulls the device's
`/system/lib64/libc++.so` and ships it as `libsysc++.so`, because an app linker namespace cannot
open the system C++ library directly.

> **Note:** `CMakeLists.txt` is stale. It builds only the host-side `macho-inspect` tool from
> `src/main.cpp` and `src/chained_fixups.cpp`; it does not build either loader. Use the shell scripts.

## Running

### Command-line loader

Copy the app bundle to `/data/local/tmp/game` on the device:

```bash
adb push xnuroid_loader /data/local/tmp/
adb push <app>/UnityFramework /data/local/tmp/game/
adb push <app>/Info.plist /data/local/tmp/game/
adb push <app>/Data /data/local/tmp/game/Data
adb shell chmod 755 /data/local/tmp/xnuroid_loader
adb shell /data/local/tmp/xnuroid_loader
```

Override the bundle root with `LOADER_GAME_DIR`. The default writable container is
`/data/local/tmp/sandbox`; override it with `LOADER_SANDBOX_DIR`.

### Android app

`./build_apk.sh install` installs the APK, copies `/data/local/tmp/game` into the app's internal
files directory with `run-as` if needed, and starts the activity.

The app uses this layout:

| Path | Contents |
| --- | --- |
| `<internalDataPath>/game/` | `UnityFramework`, `Info.plist`, and `Data/` |
| `<internalDataPath>/sandbox/` | Guest-visible `Documents`, `Library`, and `Caches` tree |
| `<externalDataPath>/loader.env` | Optional `KEY=VALUE` environment overrides |
| `<externalDataPath>/out.txt`, `err.txt` | Loader standard output and error |
| `<externalDataPath>/exit.txt` | `EXIT=<code>` after the loader thread returns |

Pull logs with:

```bash
adb pull /sdcard/Android/data/com.tanosshi.xnuroid/files/out.txt
adb pull /sdcard/Android/data/com.tanosshi.xnuroid/files/err.txt
```

Example `loader.env`:

```dotenv
LOADER_FRAMES=300
LOADER_FRAME_DIR=/sdcard/Android/data/com.tanosshi.xnuroid/files/frames
LOADER_GL_TRACE=1
```

The app sets `LOADER_ENGINE=8`, `LOADER_FDE=all`, and a very large `LOADER_FRAMES` before reading
`loader.env`, so values in that file override those defaults.

### Launch stages

Without `LOADER_ENGINE`, the diagnostic path runs these stages:

1. Register the IL2CPP log callback.
2. Set the data directory.
3. Call `_il2cpp_init`.
4. Enumerate assemblies and classes.
5. Call `il2cpp_string_new`.
6. Invoke a managed method.
7. Optionally run exception tests with `LOADER_EXCEPTION_TEST=1` or `LOADER_EXCEPTION_TEST=parse`.

With `LOADER_ENGINE` set, the headless sequence is skipped and the engine path runs instead.
`LOADER_ENGINE=8` performs the full `UIApplicationMain`-style launch and enters the run loop.

## Environment variables

| Variable | Meaning |
| --- | --- |
| `LOADER_GAME_DIR` | App bundle root. Default /data/local/tmp/game. |
| `LOADER_TMP_DIR` | Temporary directory used by shims. |
| `LOADER_SANDBOX_DIR` | Guest container root. Default /data/local/tmp/sandbox. |
| `LOADER_LIBCXX` | Path to the Android libc++ used for the guest’s std::__1 ABI. |
| `LOADER_ENGINE` | Enable engine mode. 1 no-graphics init, 2 startup helper first, 5 Unity runtime warmup, 6 graphics init, 7 first scene, 8 full app launch and run loop. Any value also enables nil-object behavior and GLES unless disabled. |
| `LOADER_FRAMES` | Run-loop ticks for engine 8. CLI default 300; the APK sets a very large value. |
| `LOADER_ENGINE_ARGS` | Extra engine command-line switches, space- or comma-separated. |
| `LOADER_ENGINE_SKIP_1B304` | Skip the engine registration step at image+0x1b304. |
| `LOADER_NO_OBJC_RT` | Do not register the guest Objective-C runtime. |
| `LOADER_OBJC_NIL` | Enable nil-object behavior without entering engine mode. |
| `LOADER_PEEK` | Comma-separated hex image offsets to print at startup. |
| `LOADER_FDE` | Any non-empty value registers FDEs for all guest functions; all is conventional. |
| `LOADER_FDE_ONLY` | File containing 0x... image offsets; register only containing functions. |
| `LOADER_FDE_DUMP` | Log every registered FDE. |
| `LOADER_EXCEPTION_TEST` | 1 runs culture-free throw tests; parse runs the Int32.Parse("abc") test. Also enables unwind tracing. |
| `LOADER_TRACE_UNWIND` | Log calls to C++ exception/unwind imports. |
| `LOADER_EXCEPTION_BENCH` | With exception test 1, run 100 timed throw/catch round trips. |
| `LOADER_GLES` | 0 disables the GLES renderer even in engine mode. |
| `LOADER_MSL_TEST` | Translate every <dir>/*.metal to GLSL, compile it, print a report, and exit. |
| `LOADER_DUMP_MSL` | Write runtime MSL sources as <dir>/<n>.metal. |
| `LOADER_FRAME_DIR` | Save presented frames as PNG files in this directory. |
| `LOADER_FRAME_EVERY` | Save every Nth frame. Default 60. |
| `LOADER_GL_TRACE` | Log render passes and draws for the first N presented frames. |
| `LOADER_GL_TRACE_AT` | Log one specific presented frame. |
| `LOADER_GL_SKIP_DRAW` | Drop the Nth draw of each pass (debug). |
| `LOADER_GL_SOLID` | Force a solid magenta fragment color for shaders writing o_SV_Target0. |
| `LOADER_DRAW_STATS` | Print draws per presented frame every 60 frames. |
| `LOADER_LOG_PVRTC` | Log PVRTC decode timing. |
| `LOADER_MEM_STATS` | Print live fake-object and GL texture memory every 600 frames. |
| `LOADER_TOUCH` | Scripted taps: <frame>:<x>,<y>[,<frames held>];... in screen pixels. |
| `LOADER_AUDIO` | 0 disables AAudio and discards rendered samples. |
| `LOADER_AUDIO_WAV` | Record rendered audio to this WAV path. |
| `LOADER_HAPTICS` | 0 disables vibration. |
| `LOADER_LOG_THREADS` | Log thread creation and exit. |
| `LOADER_LOG_BLOCKS` | Log block copies and invocations. |
| `LOADER_WATCHDOG` | Start a watchdog that dumps all thread stacks after N seconds, then exits 124. |
| `LOADER_WATCHDOG_COUNT` | Number of watchdog dumps. Default 1. |
| `LOADER_POISON_STRICT` | Fault on the first access to an unresolved data import instead of logging and continuing. |
| `LOADER_TRACE_DIRECT` | 1 logs the first calls per directly bound symbol, all logs every call, list logs the symbol list. |
| `BISECT_NO_VARIADIC_SHIMS` | Disable variadic shims for A/B comparison. |
| `LOADER_FAKE_HANDLE_SIZE` | Bytes allocated behind each fake object handle. Default 32. |
## Test harness
run_matrix.txt defines device runs as name|environment. The current entries are the default run
and an engine-7/FDE run.
run_phases.ps1 builds, deploys, runs the matrix, and writes device_logs\<timestamp>\SUMMARY.txt.
Its compile command predates the current source list. Prefer building with `build_cli.sh`, then run:

```powershell
powershell -ExecutionPolicy Bypass -File run_phases.ps1 -SkipBuild
```

`-Push` uploads logs to a `device-logs` branch using a throwaway clone. `watch_runner.ps1` polls
`main`, fast-forwards on new commits, and runs the same quick matrix.
## Debugging
- The signal handler covers SIGSEGV, SIGBUS, SIGABRT, and SIGILL. It prints a frame-pointer
walk, classifies addresses as image/trampoline/loader/libc/libc++, and previews string arguments.
- A ring buffer keeps the last 32 shim calls with registers and return addresses.
- The watchdog sends SIGUSR2 to every thread to dump stacks when the engine hangs.
- Unresolved data imports go to poison pages; LOADER_POISON_STRICT=1 turns the first touch into a
fault instead of continuing.
- FDE registration, GL traces, draw statistics, and frame PNGs can be enabled independently.
## Known limitations
- One title, one device. This is not evidence of general iOS or Unity compatibility.
- Partial Metal. The MSL-to-GLSL translator and Metal object model cover the paths this game
uses. Unsupported shader features or pipeline state can fail or render incorrectly.
- Fake frameworks. Foundation, UIKit, QuartzCore, AVFoundation, and CoreFoundation exist only to
the extent this title exercises them. Unknown selectors may return nil or an inert object.
- Approximate unwinding. Synthesized FDEs use one CFI row per function, which is sufficient for
call-site unwinding but not a full replacement for compact unwind. Managed exceptions remain
PARTIAL.
- Unresolved imports remain. Calls to unresolved functions abort through shim_unresolved;
unresolved data imports read from poison pages.
- No FairPlay decryption. The input executable must already be decrypted.
- No arm64e or pointer authentication support. The tested guest is arm64.
- The CLI path is diagnostic. The playable experience is the APK host.
## Repository layout
- src/android_loader.cpp — Mach-O loader, shims, engine stages, crash handling, IL2CPP tests
- src/android_app.cpp — NativeActivity host, window, input, logging, JNI vibration
- src/trampolines.s, src/shim_types.hpp — trampoline ABI and CallFrame
- src/apple_string.cpp — libc++ std::__1 string ABI
- src/objc_runtime.hpp — guest Objective-C runtime
- src/fake_foundation.hpp, src/fake_uikit.hpp — stateful Foundation/UIKit fakes and run loop
- src/mtl_gles.hpp, src/gles_backend.*, src/msl_glsl.* — Metal-on-GLES and MSL translation
- src/pvrtc.*, src/pvrtc_gpu.hpp — PVRTC decoding
- src/audio_unit.cpp, src/haptics.cpp, src/darwin_pthread.cpp — audio, vibration, pthread ABI
- src/guest_fde.hpp — compact-unwind to DWARF FDE synthesis
- src/main.cpp, src/chained_fixups.* — host-side Mach-O inspector (macho-inspect)
- build_cli.sh, build_apk.sh — authoritative loader builds
- android/AndroidManifest.xml — NativeActivity manifest
- run_phases.ps1, watch_runner.ps1, run_matrix.txt — device test harness
- CMakeLists.txt — inspector-only build; not used for the loader
