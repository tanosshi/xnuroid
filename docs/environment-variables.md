# Environment variables

The loader reads all of its runtime switches from the environment. Set them in the shell for the
command-line loader, or in `loader.env` on the device for the [Android app](android-app.md).

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
| `LOADER_GL_TRACE_TRIGGER` | File path checked every 5 frames. When it appears, the next frame is traced and the file is removed. An `x,y` line in the file sets the probe pixel. With `LOADER_FRAME_DIR`, the traced frame, the first texture of each draw, and each program's GLSL (`prog_<id>.glsl`) are saved. |
| `LOADER_GL_TRACE_PROBE` | `x,y` render-target pixel (Metal coordinates) printed after each traced draw. |
| `LOADER_GL_SKIP_DRAW` | Drop the Nth draw of each pass (debug). |
| `LOADER_GL_SOLID` | Force a solid magenta fragment color for shaders writing o_SV_Target0. |
| `LOADER_DRAW_STATS` | Print draws per presented frame every 60 frames. |
| `LOADER_LOG_PVRTC` | Log PVRTC decode timing. |
| `LOADER_MEM_STATS` | Every 600 frames: live fake objects (top kinds) and GL texture memory (largest textures). |
| `LOADER_FAKE_LIFE` | Track freed fake objects and log `[DEAD]` when a message reaches one; adds creating selectors to `LOADER_MEM_STATS`. Never reuses freed handles. |
| `LOADER_LOG_DEFAULTS` | Log NSUserDefaults (PlayerPrefs) saves to <sandbox>/Library/Preferences. |
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
