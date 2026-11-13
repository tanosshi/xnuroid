# Environment variables

See [`graphics-pipeline.md`](graphics-pipeline.md) for how the GL switches map onto render passes and
[`rendering-investigation.md`](rendering-investigation.md) for the procedures that use them.

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
| `LOADER_NO_VIDEO_SKIP` | Any value except `0` leaves a splash scene that waits on a VideoPlayer alone instead of loading the next scene. `KEEP_SPLASH=1 ./build_apk.sh` (or unticking the splash box in the helper) bakes that default into the APK; `LOADER_NO_VIDEO_SKIP=0` in `loader.env` turns skipping back on. |
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
| `LOADER_GL_NO_DEPTH` | Debug: disable the depth test for every draw. |
| `LOADER_GL_NO_STENCIL` | Debug: disable the stencil test for every draw. |
| `LOADER_GL_SOLID` | Force a solid magenta fragment colour in every translated fragment shader (rewrites the vec4 output at the end of main). Proves draw coverage independently of the guest's state. |
| `LOADER_GL_SOLID_VS` | Force `gl_Position` from `gl_VertexID` so every draw covers the whole target (geometry debug). |
| `LOADER_DRAW_DUMP` | Read the pass's framebuffer back immediately after a full-size draw and write `frames/dbg_after_draw.png` (first 3 draws). |
| `LOADER_DEBUG_CLEAR` | Paint full-size colour clears magenta, so a frame that stays black shows whether any pass cleared colour at all. |
| `LOADER_DEBUG_CLEAR_DUMP` | Read the framebuffer back right after the pass clears and write `frames/dbg_after_clear.png` (first 2 passes). |
| `LOADER_DRAW_STATS` | Print draws per presented frame every 60 frames. |
| `LOADER_ENGINE_STRICT_UNRESOLVED` | Make engine mode abort on an unresolved function import instead of answering nil. |
| `LOADER_ENGINE_LEGACY_OFFSETS` | Use the legacy hand-replayed ObjC bootstrap offsets instead of the framework's own entry points. |
| `LOADER_LOG_PVRTC` | Log PVRTC decode timing. |
| `LOADER_TEX_PROFILE` | Texture profiler. Logs `[TEXPROF]` texture creation, a per-second summary, and anomalies: textures sampled with no upload or render, missing or wrong-target bindings, render feedback, draws whose texture swaps between a real texture and a tiny single-colour placeholder (Unity default textures), textures destroyed within 120 frames, re-uploads, dropped or unimplemented blits, zero-filled uniform blocks, short vertex buffers, dropped draws and incomplete framebuffers. Also logs each pipeline's vertex attributes and blend state once. With `LOADER_FRAME_DIR`, traces and dumps the frame after each of the first N anomalies. N is the value, default 6. |
| `LOADER_MEM_STATS` | Every 600 frames: live fake objects (top kinds) and GL texture memory (largest textures). Printed from the frame-dump point, so it needs `LOADER_FRAME_DIR` and `LOADER_FRAME_EVERY` to divide 600 for output. |
| `LOADER_REPOST_NOTES` | Re-deliver Unity's view lifecycle notifications (`kUnityViewWillLayoutSubviews` and friends) every 120 frames, to test whether the engine's display manager read the surface size before its observers existed. |
| `LOADER_SELECTOR_COUNTS` | At each frame dump, the 14 most-called Objective-C selectors since the previous dump (`[SEL] count name`). Shows where a title's per-frame host calls go; the totals are dominated by Metal render-pass descriptor construction. |
| `LOADER_PC_SAMPLES` | Sample count for `LOADER_PC_PROFILE` (default 4000). The sampler waits a jittered 0.5x-1.5x of the interval so it does not lock onto one phase of a repeating frame. |
| `LOADER_PASS_TRACE` | One compact block per render pass (first 12 passes): pipeline sets with vs/fs names, bound buffers/textures with handles and GL ids, viewport/scissor, depth-stencil state, every draw entry point with counts, and every draw rejected before rasterising with the code's reason. |
| `LOADER_PASS_TEX_DUMP=<frame>` | At that frame, dump the texture sampled by the 1080x2340 no-depth pass and print its colour histogram (`[PASSTEX]`, PNG into `LOADER_FRAME_DIR`). |
| `LOADER_UI_AUDIT` | Print the host surface state at startup, frame 300 and frame 600: application state, key window, root view controller and view geometry, every view owning a `CAMetalLayer`, that layer's bounds/drawableSize/contentsScale/pixelFormat, screen metrics, orientation, and a list of zero or inconsistent values. |
| `LOADER_FAKE_LIFE` | Track freed fake objects and log `[DEAD]` when a message reaches one; adds creating selectors to `LOADER_MEM_STATS`. Never reuses freed handles. |
| `LOADER_LOG_DEFAULTS` | Log NSUserDefaults (PlayerPrefs) saves to <sandbox>/Library/Preferences. |
| `LOADER_TOUCH` | Scripted taps: <frame>:<x>,<y>[,<frames held>];... in screen pixels. |
| `LOADER_NO_VIDEO_SKIP` | Disable the splash-video completion fallback. Without video decoding a splash that waits for its clip never ends; the loader instead delivers the scene's `OnVideoCompleted` callback (or, failing that, loads the next scene in build order). |
| `LOADER_VIDEO_SKIP_FRAME` | First frame the splash-video fallback may act on. Default 150. |
| `LOADER_STALL_ADVANCE` | 0 disables the startup escape hatch that continues into the next build scene when one scene has been active for `LOADER_STALL_SECONDS` without the player loading anything. Retried at most once per launch. |
| `LOADER_STALL_SECONDS` | Seconds a scene may stay active before the escape hatch fires. Default 15. |
| `LOADER_MANAGED_QUERY` | Comma list of frames at which to print managed state through the guest's exported IL2CPP API: `Time` fields, `SceneManager` scene names and root counts, `FindObjectsOfType` counts, `VideoPlayer` state, and a live object-type histogram. |
| `LOADER_MANAGED_CMD` | `<frame>:scene:<name>` or `<frame>:sceneindex:<n>` — load a scene through the guest's own `SceneManager` (`LOADER_MANAGED_CMD_FRAME` sets the frame when the frame is not given in the value). |
| `LOADER_MANAGED_INSPECT` | `<frame>:<Type>[,<Type>...]` — dump every field of the live instances of those managed types, decoding strings and primitive values. |
| `LOADER_MANAGED_STATIC` | `<frame>:<Type>::<Method>` — call one parameterless static method (e.g. `UnityEngine.Application::get_persistentDataPath`) and print its string result. |
| `LOADER_MANAGED_CALL` | `<frame>:<Type>::<Method>` — invoke one parameterless method on the first live instance of a type. |
| `LOADER_TRACE_HIT` | `<frame>:<Type>::<Method>[,<Type>::<Method>...]` — overwrite the first instruction of each method with `udf #0`. The first execution raises SIGILL, whose dump names the PC, so one run shows whether (and which) of a set of methods is ever reached. Diagnostic only: it stops the process. |
| `LOADER_AUDIO` | 0 disables AAudio and discards rendered samples, so a title runs silently. Worth setting for every test run when the device cannot be muted. |
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
| `LOADER_ENGINE_ARGS` | Extra engine command-line switches handed to the framework, space- or comma-separated. |
