# Rendering investigation log (durable notes)

Observations, measurements and reproducible procedures for getting iOS games to draw through the
Metal -> GLES renderer. Everything here was measured on the device described below; numbers are kept so a
later session can compare instead of re-deriving.

Companion docs: [`graphics-pipeline.md`](graphics-pipeline.md) (architecture), [`environment-variables.md`](environment-variables.md) (switches).

> Last updated 2026-11-12. The renderer changes described in section 4 are in the working tree and are not
> committed; `git diff --stat` shows `src/gfx/mtl_gles.hpp`, `src/gfx/msl_glsl.cpp`,
> `src/objc/fake_uikit.hpp`, `src/objc/fake_foundation.hpp`, `src/loader/android_loader.cpp`,
> `src/loader/apple_string.cpp`, `build_apk.sh`.

---

## 0. Workstation / device

| Item | Value |
| :--- | :--- |
| Repo | `C:\Users\tanos\Desktop\ipa-loader` (bash runs as Git Bash; `MSYS_NO_PATHCONV=1` is required for `adb shell` paths) |
| Device | arm64 Android phone, wireless adb serial `adb-R5CX32HFAPV-JGeAPq._adb-tls-connect._tcp` |
| NDK | `C:/Users/tanos/AppData/Local/Android/Sdk/ndk/28.2.13676358` (clang 19.0.1) |
| Symbolizer | `$LOCALAPPDATA/Android/Sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-addr2line.exe -f -C -e xnuroid_loader <offset>` |
| CLI binary | `xnuroid_loader` (38-39 MB), pushed to `/data/local/tmp/xnuroid_loader` |
| Game bundles on device | `/data/local/tmp/jcgame` (title A, ~592 MB), `/data/local/tmp/game` (title B) |
| Local game bundles | `games/TitleA-v2-2-0` (selected in `tools/game_config_local.py`, git-ignored) |
| Saved captures | `device_logs/diag/` |

### Build / run / collect (verified commands)

```bash
# build (1-3 min). If clang reports "frontend command failed due to signal", rerun: it is an intermittent
# Windows crash in the NDK driver, the same sources build on the next invocation.
./build_cli.sh > build/diag_build.log 2>&1; test -s build/diag_build.log && tail -20 build/diag_build.log

export MSYS_NO_PATHCONV=1
adb push xnuroid_loader /data/local/tmp/xnuroid_loader
adb shell 'chmod 755 /data/local/tmp/xnuroid_loader'        # otherwise exit code 126

adb shell 'rm -rf /data/local/tmp/jc; mkdir -p /data/local/tmp/jc/tmp /data/local/tmp/jc/frames; \
  cd /data/local/tmp/jc; \
  LOADER_ENGINE=8 LOADER_FDE=all LOADER_FRAMES=300 \
  LOADER_GAME_DIR=/data/local/tmp/jcgame LOADER_TMP_DIR=/data/local/tmp/jc/tmp \
  LOADER_FRAME_DIR=/data/local/tmp/jc/frames LOADER_FRAME_EVERY=100 \
  LOADER_WATCHDOG=110 timeout 260 /data/local/tmp/xnuroid_loader > run.out 2> run.err; echo EXIT=$?'

adb pull /data/local/tmp/jc/frames/frame_00300.png device_logs/diag/x.png
python -c "from PIL import Image; im=Image.open('device_logs/diag/x.png').convert('RGB'); \
c=im.getcolors(4000000); c.sort(reverse=True); print(len(c), c[:3])"
```

Notes that save time:

* `LOADER_TMP_DIR` must not contain the logs; stage `run.out`/`run.err` in the parent directory.
* All engine frames run from one `adb shell` invocation; a watchdog (`LOADER_WATCHDOG=<seconds>`) plus
  `timeout` prevents an orphaned guest from holding the GPU.
* `adb devices` can come back empty after a long detach; `adb kill-server; adb start-server; sleep 5` restores
  the wireless device without re-pairing.
* A pixel histogram with `len(colors) == 1` means the frame is one flat colour; `len(colors)` in the
  thousands means real content (`title B`'s offline dialog measures 2766 colours).

---

## 1. Title profiles

### 1.1 title B (reference title, engine + renderer known good)

| Measurement | Value |
| :--- | :--- |
| Engine path | `LOADER_ENGINE=8`, warmup + `UnityAppController` launch |
| `__mod_init_func` entries run | 449 |
| Ticks / presents | 400 ticks -> 401 drawables, 401 presents in 7.9 s (1200 ticks -> 1201 presents earlier) |
| Present shape | `[commandBuffer presentDrawable:]` (commit path) |
| Frame dump | `frame_00350.png`, 1080x2340, 2766 distinct colours (yellow/white offline dialog on black) |
| Dropped draws (`ready=0`) | 0 |
| Uniform blocks in traced draws | `vs_UnityPerDraw_block`, `vs_VGlobals_block` |

This title is the regression check for every renderer change: the same histogram (1756344 black + 486133
white, 2766 colours) reproduces after unrelated fixes.

### 1.2 title A (`TitleA-v2-2-0.ipa`, Unity 2021.3.33f1)

| Measurement | Value |
| :--- | :--- |
| Guest payload | `UnityFramework` 65 MB loaded as engine-mode image 0; app stub links it via `LC_LOAD_DYLIB @rpath/UnityFramework.framework/UnityFramework` (never `dlopen`) |
| `__mod_init_func` entries run | 589 (counted from the section table, not a hardcoded address) |
| Managed side | logs `Initialize engine version: 2021.3.33f1`, `initializing default ECS World manually...`, `Unloaded 6 Unused Serialized files`, `Loaded Objects now: 3690` -> managed code and asset loading run |
| Assets opened | `unity_builtin_extra`, `sharedassets0`, `sharedassets1`, `level0`, `level1`, `level1.resS`, `resources.assets`, `resources.resource` |
| Present shape | `[CAMetalDrawable present]` (`g_presented_drawable` path); newer Unity never sends `presentDrawable:` |
| Render passes per frame | screen-sized blit pass (colour, no depth), 864x1872 scene pass (colour + depth), game blit pass into the drawable |
| Draws per frame | 3 after the fixes in section 4 (Unity blit 864x1872 -> 864x1872, Unity blit -> 1080x2340 with depth, game `vprog`/`fshader` blit 1080x2340 -> drawable) |
| Scene render target | 864x1872 BGRA8 with a depth attachment (= 1080x2340 x 0.8 render scale) |
| Drawables | three pooled 1080x2340 BGRA8 textures; `renders` counter reaches 400+ over a 1200-frame run |
| Shader names | Unity HLSLcc: `xlatMtlMain`/`xlatMtlMain`; game: `vprog`/`fshader` with a file-scope `constexpr sampler blit_tex_sampler(address::clamp_to_edge, filter::linear)` |
| Dropped draws | 0 after the fixes; before them, 2 per pass (`prog == 0`) |
| Shader translation failures | 0 after the fixes; before them, one per library (`unparsed global constant`, then `'VProgOutput' : undeclared identifier`) |
| Long run | 1200 frames, all presents, no crash, watchdog clean |

---

## 2. Diagnostic recipes (each one produced a concrete answer)

Every technique below is a build-and-run cycle of ~2-3 minutes; each is worth reaching for before
speculating.

### 2.1 "Is a pipeline actually usable?" — count drops, not successes

`draw_indexed` logs `[MTLGL] draw skipped: ready=0 ... pipe=... prog=... vs=<function name>` for the first 6
drops. `ready(e)` is `e->valid && e->pipe && e->pipe->prog`, so `prog=0` means the pipeline built from that
descriptor produced no GL program. Pair it with the creation log `[MTLGL] make_pipeline #N vf=... ff=...`
(first 12 calls) and `[MTLGL] pipeline <vs> / <fs> -> prog N`; a pipeline that logs a creation line but never
a `-> prog` line stopped in translation or compilation.

### 2.2 "Why was a shader rejected?" — read the dumped pair

Every rejection writes `<LOADER_FRAME_DIR>/failed_<n>.metal` (the guest MSL) and `.glsl` (what the translator
emitted) and prints the reason to stderr. Reading the emitted GLSL is usually faster than reading the
translator: `VProgOutput out = {...}` in the output means a stage-output struct survived, an empty
`gl_Position` means the return value was not rewritten.

### 2.3 "Does the geometry cover anything at all?" — force the vertex stage

`LOADER_GL_SOLID_VS=1` replaces `gl_Position` at the end of every vertex main with a full-target quad built
from `gl_VertexID`; `LOADER_GL_SOLID=1` forces the fragment out to magenta. Combine with
`LOADER_DRAW_DUMP=1`, which reads the pass's own FBO back immediately after the draw
(`frames/dbg_after_draw.png`). A magenta result proves rasterisation happened for that draw, independently of
the guest's matrices, sampler contents or blend state.

### 2.4 "Is the viewport what the guest asked for?" — log the setter and the GL state

`setViewport:` logs the six doubles it receives (`[MTLGL] setViewport: x y w h near far`, first 8), and the
trace block prints the live GL state after each traced draw:
`gl: fbo ... prog ... scissor[...] en ... viewport[...] depthtest ...`.
A zero-sized viewport with a correct scissor is the signature of a guest that sized its viewport from a zero
host metric, not of a broken pass.

### 2.5 "Which texture does the frame really land in?" — compare draw target with presented drawable

`dump_presented` prints the presented drawable's tex id and the id of the last full-size render target
(`g_last_big_target_obj`). When they differ, the guest is presenting a buffer from an earlier frame; the dump
then follows the render target so it captures what was drawn this frame.

### 2.6 "Is the draw count plausible?" — draws per dump window

`dump_presented` reports `N draws since last dump`, drained from `g_draws_frame`. With `FRAME_EVERY=100`, a
value of 200 means 2 draws/frame, 300 means 3, and a 3D scene normally reads in the thousands per frame.
`LOADER_DRAW_STATS=1` prints the same idea periodically without dumps.

### 2.7 "Are the uniforms real?" — trace one frame

`LOADER_GL_TRACE_AT=<n>` traces one presented frame: per draw it prints the pipeline's function names, index
count, `maxv`, attribute count, block count, sampler count, the sampled texture's size, the first 3 floats of
each vertex attribute, the first 4 floats of each uniform block with the buffer index and available bytes, the
blend/depth/stencil state *and* the live GL viewport/scissor/FBO/program. This single output distinguishes
missing uniforms, missing attributes, wrong textures and wrong viewport.

### 2.8 "Which textures are being realized 1x1?" — creation census

`texture()` prints `[MTLGL] texture realized 1x1: pixelFormat N, M properties, class ...` for the first few,
including every non-`#` property value. Unity's 1x1 defaults show identical `width=1 height=1` plus a real
`pixelFormat`; a 1x1 *render target* instead shows up in `begin_pass` as `[MTLGL] tiny pass target: color tex
... 1x1 pf ...`.

### 2.9 "Are the guest's own logs available?" — filter the run output

Unity's `Debug.Log` lands on stdout without a prefix. `grep -v '^\[' run.out` separates guest/engine text from
the loader's own `[TAG]` lines and shows startup banners, ECS/asset messages and, when a guest writes MSL to
the log, the shader source itself (that is how `vprog`/`fshader` and the file-scope sampler were found).

### 2.10 "Which selectors does this title actually use?" — unique-sorted selector dump

First use of every selector is logged once, so this gives the complete API surface of a title:

```bash
adb shell 'grep -a -o "\[0x[0-9a-f]* [a-zA-Z:]*[^]]*\]" run.out | sed "s/.*] //" | sort -u'
```

Useful filters: `encoder` (render/blit/parallel), `pipeline`, `draw`, `copy` (blits), `Heap` (heaps).

### 2.11 "What is the shape of a frame?" — pass census with load/store actions

`begin_pass` logs (first 24 passes) each pass's size, attachments, `colorLA`/`colorST`/clear colour/`depthLA`, and
prints `pass N issued M draws` when the next pass begins. On title A this shows the whole frame:

| pass | size | attachments | colour load | draws |
| :--- | :--- | :--- | :--- | :--- |
| camera | 864x1872 | colour + depth | Clear (0,0,0,1) | **0** |
| blit | 864x1872 | colour | Load | 1 (camera target -> 864x1872 target) |
| blit | 1080x2340 | colour + depth | Load | 1 (-> drawable) |
| present | 1080x2340 | colour | DontCare | 1 (game `vprog`/`fshader` blit) |
| scratch | 1x1 | depth only | - | 0 |

A camera pass that clears to a constant colour and issues zero draws means the engine had a camera and an empty
renderer list; the rest of the frame is copies of that empty target.

### 2.12 "Does the guest rely on NSNotificationCenter?" — observer table

Engine mode now keeps a real observer table (`[NOTE] observe '<name>' object ... block ...` and
`[NOTE] post <name> -> N observer(s)`). Unity's iOS view/display lifecycle runs through these names:
`kUnityWillFinishLaunchingWithOptions`, `kUnityViewWillAppear`, `kUnityViewDidAppear`,
`kUnityViewWillLayoutSubviews`, `kUnityViewDidLayoutSubviews`, plus
`UIApplicationWillEnterForegroundNotification` / `UIApplicationDidBecomeActiveNotification`. `LOADER_REPOST_NOTES=1`
re-delivers the view notifications every 120 frames (used to test whether the display manager read a size too
early).

---

## 3. Facts established about host-side metrics (screen, layer, view)

Unity's Metal backend derives its render viewport from host surface metrics, and a zero in any of them
propagates all the way to `setViewport:` with width/height 0 (rasterisation then produces no fragments, and
the frame is the clear colour).

* `-[UIScreen bounds]` returns points (360 x `screen::g_height`); `-[UIScreen nativeBounds]` returns
  pixels (`kScreenW * kScreenScale`); `scale`/`nativeScale` return `kScreenScale` (3.0). Values live in
  `src/objc/screen_metrics.hpp`; the app host updates the height from the window aspect ratio.
* `-[UIView bounds]`/`frame` return the view's rect, and fall back to the screen when the view was created
  without a frame (autoresized later). `-[CALayer bounds]`/`frame` do the same through the owning view.
* `-[CAMetalLayer drawableSize]` and `CAMetalLayer.nextDrawable` share `layer_drawable_pixels()`: the
  explicitly set `drawableSize` if positive, else view bounds x `contentsScale`, else the screen in pixels.
* Observed effect on title A: viewports `0 0 0 0` before, `0 0 1080 2340` for the drawable pass and
  `0 0 864 1872` for the scene pass after; the scene render target appeared at 864x1872 (0.8 render scale)
  instead of 1x1 scratch textures.

## 4. Working-tree changes from this session (uncommitted)

Facts about the edit, not conclusions about the titles.

### `src/objc/fake_uikit.hpp`
* Added `layer_drawable_pixels(FakeObjcObj*, double& w, double& h)`; `next_drawable` and
  `-[CAMetalLayer drawableSize]` both use it (single source of truth for the layer pixel size).
* `-[UIView frame]`/`-[UIView bounds]` fall back to `kScreenW` x `screen::g_height` when the view's rect is
  zero.
* `-[CAMetalDrawable present]`, `presentAtTime:`, `presentAfterMinimumDuration:` store the receiver in
  `mtlgl::g_presented_drawable` and increment `g_presents`.

### `src/gfx/msl_glsl.cpp`
* `parse_global_const` accepts a declaration with no `=` when the type is `sampler` (file-scope
  `constexpr sampler name(address::..., filter::...)`) and registers the name in `samplers_` with unit 0.
  Failure message for other unparsable globals now includes the type and name.
* New statement pass: `StageStruct local = { a, b, ... };` expands to per-field assignments through
  `io_name(true, field)` (so `gl_Position`, `v_<field>`, `o_<field>` are written), remembers the mapping in
  `aggregate_return_`, and `return local;` replays it (`return_value`).
* `dump_failed_shader` reasons for vertex/fragment compilation now print the compiler log to stderr.

### `src/gfx/mtl_gles.hpp`
* SOLID injections are generic: `LOADER_GL_SOLID` locates the fragment `vec4` out variable instead of
  requiring `o_SV_Target0`; `LOADER_GL_SOLID_VS` rewrites `gl_Position` from `gl_VertexID`.
* New counters: `g_draws_frame`, `g_presented_drawable`, `g_last_big_target`,
  `g_last_big_target_obj` (set in `begin_pass` for full-size color attachments).
* `dump_presented` prefers the last full-size render target over the presented drawable when they differ,
  and reports tex id, size, `renders`, draws since last dump and the drawing thread.
* Bounded diagnostics (first N occurrences): `MTLGL pass`, `MTLGL attach color`, `MTLGL tiny pass target`,
  `MTLGL texture realized 1x1` (+ property values), `MTLGL make_pipeline #`, `MTLGL pipeline X -> prog N`,
  `MTLGL draw:` (prog/count/maxv/attrs/rt/tex/err/thread), `MTLGL draw skipped` (ready/pipe/prog/vs),
  `MTLGL setViewport:`, `MTLGL debug dump after draw`, `MTLGL debug dump after clear`.
* `LOADER_DEBUG_CLEAR` paints full-size colour clears magenta; `LOADER_DEBUG_CLEAR_DUMP` reads the FBO back
  right after the clears; `LOADER_DRAW_DUMP` reads it back right after a full-size draw.

Earlier session work still present in the same file set: engine-mode ObjC bootstrap
(`+[UnityFramework getInstance]`, `-frameworkWarmup:argv:`, `-setDataBundleId:`) in
`src/loader/android_loader.cpp`, `__mod_init_func` taken from the section table, availability-version-check
shim, app-runtime symbol fallback with unresolved functions answering nil
(`LOADER_ENGINE_STRICT_UNRESOLVED=1` restores abort), lenient `native_thread_get_state`, and the repaired
`A(sym)` macro in `src/loader/apple_string.cpp`.

## 5. Open questions (with the evidence that frames them)

1. **Where do a title's scene draws originate when the camera pass is empty?** For title A the
   864x1872 pass with depth exists, is complete, and receives zero draws while the managed side logs asset
   and ECS startup; the three per-frame draws are all full-quad blits. Useful next probes: count passes per
   frame with/without depth, dump the scene target on every frame for the first 100 frames, and check whether
   the guest ever calls `setRenderPipelineState:` with a scene pipeline (pipeline census by function name).
2. **Which host metric does a given Unity version use for each size?** Observed inputs: `UIScreen bounds`
   (points), `nativeBounds` (pixels), `UIView bounds/frame`, `CALayer bounds`, `CAMetalLayer drawableSize`,
   `MTLTexture width/height` of the drawable. title A 2021.3 responded to the view/layer path; a title that
   still reports 0 is worth checking against its own `setViewport:` log before changing more shims.
3. **Presentation ordering.** Guests present either through `presentDrawable:` or `-[CAMetalDrawable
   present]`, and the drawable presented can belong to an earlier frame (observed on title A: draw target 93
   vs presented drawable 87 in the same window). Any frame-capture code should follow the render target, and
   any present-side effect (screenshot, band detection) should state which buffer it used.
4. **Aggregate and struct-returning shader shapes.** The translator now handles the specific pattern
   `Struct local = { ... }; return local;` plus member-wise writes and `return (Struct){ .f = ... };`. Other
   shapes worth collecting as test cases: multiple locals, nested structs, `return` inside branches, and
   fragment shaders returning a struct with several `[[color(n)]]` members.
5. **Blit coverage.** `blitCommandEncoder` is created by these titles, but no `copyFromTexture:`/`copyToTexture:`
   selector has been logged; the renderer's blit entry points are `begin_pass`/`draw_indexed` only. If a title
   is observed calling a `copy...` selector, that is the point to implement `glCopyImageSubData` /
   `glBlitFramebuffer` rather than a render pass.

## 6. Update 2026-11-12, after the renderer fixes

* **Pipeline census** (`[MTLGL] pipeline new #N: <vs> / <fs> -> prog P`, one line per distinct pair, uncapped):
  title A creates exactly **two** pipelines for a 1200-6000 frame run — `vprog / fshader` (the game's own
  blit) and `xlatMtlMain / xlatMtlMain` (Unity's blit). No scene shader is ever compiled, so no draw with scene
  geometry exists to trace.
* **Draw census** (`[MTLGL] census: passes depth N nodepth M | draws depth X nodepth Y`, drained by the frame
  dump): per frame 4 depth-attached passes + 2 without, and 1 depth draw + 2 non-depth draws. Camera pass draws:
  **0**.
* **Camera pass detail**: 864x1872, colour+depth, `colorLA=2` (Clear) with clear colour `(0,0,0,1)`,
  `depthLA=2`, and zero draws. `colorST` reads as 4 (MTLStoreActionUnknown) on several passes.
* **Presentation chain confirmed** on the same frames: camera target -> 864x1872 -> drawable (Unity, with depth)
  -> drawable-sized target (game's `vprog`). Every one of those draws samples the empty camera target.
* **Input is not the gate**: `LOADER_TOUCH=120:540,1170;300:540,1600;600:540,1170` produced `[TOUCH] began ...`
  events and left the draw census, pipeline set and frame colour unchanged over 900 frames.
* **Long-run stability**: 6000 ticks in 102 s, 6001 drawables/presents, no crash, no new pipelines, census flat.
* **Engine-mode notification gap closed**: `NSNotificationCenter` registrations used to be dropped (the center
  object existed, `addObserverForName:object:queue:usingBlock:` returned nil and stored nothing), so Unity's own
  `kUnityView*` lifecycle notifications were posted into an empty table. `src/objc/fake_uikit.hpp` now keeps an
  observer table, posts after `applicationDidBecomeActive:`, and invokes observer blocks. Effect observed: every
  Unity view notification now reports `-> 1 observer(s)`; pipeline set, draw census and frame colour are
  unchanged by that fix alone, so it is a correctness fix for engine-mode hosting rather than the trigger for
  scene rendering.
* **No heap sub-allocations**: the only heap selector is `newHeapWithDescriptor:`; no
  `newTextureWithDescriptor:offset:` / `newBufferWithLength:options:offset:` appear, so heap-backed resources are
  not a missing path here.
* **Sandbox writes**: after a 2400-frame run the sandbox contains only title B's
  `the other title's preferences plist` (from the other title); title A writes no preferences and no Documents
  files, i.e. it has not reached its first-run/user-data init.
* **Thread state**: the main thread is inside guest code driven by `loader_main`'s run loop
  (`android_loader.cpp:4334`) with no shim at the top of stack; worker threads sit in
  `native_dispatch_semaphore_wait` (`android_loader.cpp:567`) via `guest_thread_tramp` (`:634`). Guest return
  addresses are not reliably walkable (optimized Unity frames), so only the top PC is trustworthy.
* **Watchdog usage**: `LOADER_WATCHDOG=<s>` samples every `<s>` seconds, `LOADER_WATCHDOG_COUNT=N` for N samples
  (e.g. 15/3 gives +15/+30/+45 s), then the process exits 124.

### 6.1 The pass shape that carries a working scene (title B as the control)

title B, same binary and same census code, 400 frames: **3 passes per frame**, each 1080x2340 with
colour+depth, `colorLA=2` (Clear) to `(0,0,0,1)`, `depthLA=2`, and **2 draws per pass**; 2 distinct pipelines
over the whole run. `frame_00350.png` from that run is a full splash screen (blue gradient field, white star with
rays, lens flares; 7784 distinct colours).

So a frame that renders correctly is: a colour+depth pass that clears to black and then receives *draws*. The
clear itself is not the picture, and two pipelines are enough for a rich image. title A runs the same
pass shape (864x1872, colour+depth, `colorLA=2`, `(0,0,0,1)`, `depthLA=2`) but the pass receives **0 draws**, and
every later pass in the frame is a copy of that empty target. The renderer, the shader translator, the drawable
pool and the presentation path are therefore all exercised successfully by the control title; the open gap for
title A is that the engine's camera submits nothing, i.e. its culling result is empty, not a failure to execute
a submitted command.

### 6.2 Is the guest looping, and what does it do per frame?

Two instruments answer "is this title stuck or just empty?":

* **Host activity meter** — the frame dump reports `N guest msgs since last dump` (every guest -> host message).
  title A: 237720 and 234663 messages per 600 frames (about 396 per frame), steady across intervals, so
  its frame loop runs continuously and is not spinning inside one blocked call.
* **Selector profile** (`LOADER_SELECTOR_COUNTS=1`) — per 600 frames the hot calls are `pixelFormat` 22841,
  `storeAction` 14403, `storageMode` 10832, `conformsToProtocol:` 10803, `new` 6087, then `setTexture:`, `setSlice:`,
  `setLevel:`, `setLoadAction:`, `setStoreAction:` and `setResolveTexture:` about 6001 each. The per-frame cost is
  therefore render-pass and attachment **descriptor construction** (about 10 descriptor objects per frame, each
  queried and re-set), not a stall in any single call.

### 6.3 Metal objects must answer `conformsToProtocol:`

Unity asks `conformsToProtocol:` on every Metal object it binds (10803 calls per 600 frames on title A, all of them
`mtl / MTLRenderCommandEncoder`). `src/objc/fake_foundation.hpp` used to answer 0 for every protocol, so a fake
render command encoder reported that it does *not* conform to `MTLRenderCommandEncoder`. It now answers YES for
`MTL*`, `NSObject` and `NSCopying`, and logs each distinct `class / protocol` pair once
(`[OBJC] conformsToProtocol: mtl / MTLRenderCommandEncoder -> 1`). Pipeline set, draw census and frame colour are
unchanged by that fix, so it is a host-correctness fix, not the trigger for scene content.

### 6.4 Paths ruled out on title A

Checked directly in the guest's selector log and absent: `NSURLSession` / `NSURLConnection` (no network fetch is
pending), `AVPlayer` / `AVPlayerItem` (no video is waiting to finish), `NSOperationQueue`, heap sub-allocations
(`newTextureWithDescriptor:offset:`), and `presentViewController:` / `dismissViewController...` (no native dialog is
waiting for a completion). Present selectors: `MTLDrawable present` and `MTLCommandBuffer presentDrawable:`; motion
queries are `CMMotionManager accelerometerData` / `gyroData` only.

### 6.5 What a title A frame actually contains (device measurements)

* **The camera pass binds nothing.** `[PASSTRACE] pass 4 864x1872 color=1 depth=1 valid=1 pipelineSets=0`, `vbuf none`,
  `fbuf none`, `vtex none`, `ftex none`, `viewport (never set; pass rect used)`, `draws 0: (no draw call)`,
  `end pass 4: 864x1872 draws=0 rejected=0`. Nothing was submitted, so nothing was dropped.
* **The game's own composite samples black.** Its `vprog`/`fshader` pass (1080x2340, no depth) reads tex 87 and
  reports `1 distinct colours, top: 000000ff 100.00%`; the dumped frame PNGs are the same single colour.
* **The surface is sound.** One `UnityView` on a `CAMetalLayer` with 3 drawables at 1080x2340, pixelFormat 80,
  presented 702 times over 700 ticks. The layer's own `bounds` are 0x0 and `position` (0,0) because no guest code
  ever writes them; `drawableSize` is synthesised from the view, so guests that size themselves from `layer.bounds`
  would see 0x0 while the render target is correct.
* **The title is in steady state.** 3600 frames are byte-identical (md5 `3da952ef9e7b12529521498a13522697`), all 89
  textures are created in frames 0-1 and none afterwards, and every frame has 3 draws (all 6-index, 3-vertex
  fullscreen quads) and 6 passes.

### 6.6 Managed side: a healthy URP frame with no game code in it

With `LOADER_IL2CPP_SYMS` loaded and `LOADER_PC_PROFILE=2 LOADER_PC_SAMPLES=9000` (jittered sampling, so it cannot
phase-lock to the ~5 ms frame), the main thread resolves 52 distinct managed methods out of 9000 samples (8916 land
outside managed code). The managed set is a complete, ordinary URP frame:
`TryGetCullingParameters`, `CreateCameraRenderTarget`, `MainLightShadowCasterPass`/`AdditionalLightsShadowCasterPass`
setup, `ScriptableRenderer::SetRenderPassAttachments`, `DrawObjectsPass::Execute`,
`ScriptableRenderPass::CreateDrawingSettings`, `CopyColorPass::Setup`, `FinalBlitPass::Execute`,
`RenderTargetBufferSystem::Swap`.

No method from the title's own assemblies is sampled at all. Dense thread snapshots do catch
`UnityEngine.Entities.SystemState`/`ComponentSystemGroup::UpdateAllSystems` and `Rewired.InputManager_Base::DoUpdate`,
so the player loop is alive; but nothing in the game's own code runs, no renderers reach the camera pass, and no
shadow casters reach the shadow passes. The picture that fits every measurement is Unity rendering an **empty
scene** (a camera, a clear colour, URP's normal pass chain and nothing else), not a broken renderer.

Caveat on one log line: `[Physics::Module] Initialized MultithreadedJobDispatcher with {0} workers.` keeps Unity's
un-substituted placeholder, so the actual worker count is not readable from it. `hw.ncpu` answers 8 and
`NSProcessInfo activeProcessorCount` answers 8, and the process runs ~55 threads.

### 6.7 Telemetry fixes, and a per-frame allocation leak

* `[MEM]` was unreachable for titles on the legacy `[CAMetalDrawable present]` path, because `commit()` returns at
  `if (!drawable) { glFlush(); return; }` before the block. It now also prints there, as
  `[MEM] frame N (legacy present): ...`.
* That immediately shows a leak: 22352 live fake objects at frame 600 and 216752 at frame 6000, exactly **+36 per
  frame**, while GL texture memory is flat at 105.5 MB. This is the source of the +3.4 MB/s RSS growth measured on
  the host (all of it in `scudo_primary`, guest arena flat) and matches the per-frame render-pass descriptor
  construction in the selector profile (about 10 `new` + 10 `colorAttachments` + attachment setters per frame).
* `LOADER_PC_PROFILE` now waits 0.5x-1.5x its interval (jitter) and takes its sample count from
  `LOADER_PC_SAMPLES`, since a fixed 2-5 ms interval aliases against a 5 ms frame and re-samples one phase.

Next experiments that follow from this: (a) log the guest's `drawIndexedPrimitives` call sites by return address
(limited symbolisation) to confirm which engine function would submit scene draws; (b) enumerate the engine's
renderer list indirectly by counting `newBufferWithLength:` allocations before the first camera pass; (c) test a
title that is known to draw a UI in engine mode (title B) with the same census to compare pass counts;
(d) read managed state from the host through the exported IL2CPP API (`il2cpp_domain_get`,
`il2cpp_class_from_name`, `il2cpp_runtime_invoke`) to report the active scene name and object counts, which is the
only remaining way to distinguish "scene failed to load" from "scene loaded but empty" for this title.

### 6.8 The black screen was a splash video, not a renderer fault

Managed introspection settled it. The renderer had been drawing correctly the whole time; title A never left its
splash scene, so there was nothing to draw:

```
scene 0 name="splash" roots=2
FindObjectsOfType(Object) = 56, Renderer = 0, Camera = 1
VideoPlayer instances = 1
video[0] url="" prepared=0 playing=? length=3.067 time=0.000
live object types: Storefront.SplashScreen.SplashScreenPlayer, StorefrontSplashSkipper,
                   UnityEngine.Video.VideoPlayer, 5 GameObject, 5 Transform, URP volume components
```

* The build's scene order (`Data/globalgamemanagers`) is `earlyboot → splash → splash_walaber → boot → menus → …`.
  `splash` is an storefront splash *video*; `splash_walaber` is the studio logo video; `boot` is the note-card
  loading screen that then loads `menus`.
* `VideoPlayer.isPrepared` is false and stays false: the video backend needs AVFoundation, the loader's fake object
  layer answers those classes with placeholders, so the clip can never prepare and the 3.067 s clip never ends.
  A splash screen that waits for the clip therefore waits forever.
* Loading the next scene by hand from the host proved the rest of the pipeline: 220 objects, 13 renderers, 2 cameras,
  1 canvas, and the boot screen renders the drawn title A logo on lined paper with 8 488 distinct colours. Loading
  `menus` from there gives 1 234 objects, 58 renderers, 4 canvases and the real main menu (graph paper, "Tap to Play",
  12 432 distinct colours). Input works too: scripted taps dismiss the update notice and press the title button.

The fix is a host-side video-completion fallback rather than a video decoder (`src/loader/il2cpp_query.hpp`):

* When a `VideoPlayer` exists and is neither prepared nor playing, the run loop looks for a live component that
  declares `OnVideoCompleted` and invokes it with the scene's `VideoPlayer`, which is exactly the callback the clip's
  end would deliver. `Storefront.SplashScreen.SplashScreenPlayer` and `LogoVideoPlayer` both have it, so the game's
  own startup code advances: `splash → splash_walaber → boot` happen without any scene being forced.
* If no such component exists, the loader falls back to the next scene in build order, read from
  `Data/globalgamemanagers` (no title-specific names anywhere).
* `LOADER_NO_VIDEO_SKIP=1` disables the whole path.

`boot` still does not advance on its own. Breakpoint tracing (`LOADER_TRACE_HIT`, which patches the first instruction
of a method with `udf #0` so the SIGILL dump names it) shows `Bootup::Start` is reached while `_bootupStepTwo`,
`_bootupStepThree` and `_bootupStepFour` never are, and `Application.persistentDataPath` stays empty, so the step chain
that ends in `SceneManager.LoadSceneAsync(menuScenePath)` never starts. Its first step builds an iCloud file provider
over `Apple.CloudKit.CKContainer`, which cannot complete on a host without those services. title B's `Loading`
scene sits in the same place for the same kind of reason (its title then reports "game is offline").

So the run loop also has a startup escape hatch: when one scene has been the active scene for `LOADER_STALL_SECONDS`
(default 15) without the player loading anything, it continues into the next scene of the build, at most once per
launch (`LOADER_STALL_ADVANCE=0` disables it). With it, title A goes splash → splash_walaber → boot → menus and
stays on its menu unattended, and title B goes `Loading → Title`. Frames from those runs:
`device_logs/diag/jc_final_2100.png` (menu) and `device_logs/diag/sld_title_1800.png` (title, offline notice).

Every run above was made with `LOADER_AUDIO=0`, which keeps the mixer running on a timer thread with no AAudio stream,
so nothing is played out loud while a title is being tested.
