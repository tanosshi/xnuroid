# xnuroid

xnuroid runs a decrypted iOS arm64 Mach-O: a Unity IL2CPP game inside a normal Android arm64
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

The build scripts default the SDK location to `%LOCALAPPDATA%\Android\Sdk`
(overridable with `ANDROID_SDK`). Edit the version variables if your SDK differs.

### Pointing the analysis scripts at your IPA

The scripts in `tools/` read the guest bundle location from
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

For the Android app host (build and run), see the [Android app host](docs/android-app.md) document.

> **Note:** `CMakeLists.txt` is stale. It builds only the host-side `macho-inspect` tool from
> `src/host/main.cpp` and `src/host/chained_fixups.cpp`; it does not build either loader. Use the shell scripts.

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

See the [environment variables](docs/environment-variables.md) reference.

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
