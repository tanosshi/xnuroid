# Calculator 3.24 on SM-S926B — long-term plan

Status: living document. Update the Status log at the end of every work session; it is the source of truth when a
session starts cold.

## 1. Target (what "fully working" means, measurably)

- **T1** The app process runs 60 s on the device with no abort, no SIGSEGV, no unresolved-symbol trap.
- **T2** A first frame that the app itself draws (not the storyboard background) is visible in the app's window.
- **T3** The calculator UI is visible: keys/display, correct layout, at least one theme.
- **T4** Interaction: a tap on a key changes the display; the app's own arithmetic produces the result.
- **T5** Lifecycle: background/resume, rotation if supported, no crash after 10 minutes of use.

T1 is already met headlessly. T2 is the hard gate: it needs the app's SwiftUI/SceneKit view tree to exist.

## 2. Established facts (do not re-litigate without new evidence)

- The IPA is arm64, Swift, SwiftUI-first: 1127 SwiftUI imports, 721 `libswiftCore`, 506 Foundation, 302 StoreKit,
  112 `libswift_Concurrency`, 60 Combine, 40 CryptoKit, plus SceneKit (20), QuartzCore (12), Metal/AVFoundation.
- The storyboard defines only a navigation controller and an empty `UIView`; no controls exist in any nib.
- The loader has no Swift runtime: Swift imports resolve to no-ops, so no Swift code with runtime dependencies runs.
- Result of that combination: the maximum loader-side render is the storyboard background (white 1080x1920 frame,
  `build/ui_frame.png`).
- Therefore T2+ is a **runtime project**, not a bug-fix project. Any plan that says "patch a few more UIKit shims and
  the calculator appears" is wrong.

## 3. Work tracks

### P0 — Baseline lock (short, first)

- Keep the arm64 build independent of the other agent's in-progress `armv7.cpp`: link `build/armv7_stub.cpp`
  (`build/build_apk_stub.sh`, and the manual CLI link in the session log). Never edit `armv7.cpp`,
  `armv7_bridge.hpp`, `docs/armv7-support.md`, `tools/host*`.
- Keep the invalid-free fix in `dealloc_object`: objects from `low_alloc` (0x50000000..0x6fffffff) must not reach
  `free()`. Without it every arm64 dealloc aborts.
- Add a repeatable harness: push loader, run N seconds with `LOADER_UI_DUMP`, assert exit 137 (timeout, not abort),
  assert frame exists, log hash + painted-pixel count.
- Deliverable: green harness run; white frame reproduced from a clean build.

### P1 — arm64 Swift runtime bring-up (the real engineering)

Goal: the app's own Swift code executes, including `@main`/`App.main` up to the point where SwiftUI is required.

1. **Symbol inventory.** Classify every imported symbol into: Swift runtime ABI (allocation, metadata, retain/release,
   demangling, `swift_once`, protocol conformance, value witnesses, concurrency), Foundation/ObjC bridging, SwiftUI
   framework API, SceneKit/Metal, vendored libs. Produce `build/calc324-symbols.tsv` and counts per class.
2. **Runtime acquisition.** Obtain an arm64 `libswiftCore.dylib` (+ `libswift_Concurrency`, `libswiftFoundation`-family
   as needed) matching a modern iOS. Candidate sources: an arm64 IPSW `dyld_shared_cache` extraction, or a simulator
   runtime image. Verify loadable by our ELF-side loader (Mach-O arm64 dylib mapping, chained fixups, exports trie).
3. **Side-image loading.** Generalize the loader from "one main image" to "main image + framework images", with a
   shared symbol table so the app's binds resolve into the side image. The armv7 agent already prototyped this for
   32-bit `libswiftCore`; port the idea, not the code.
4. **ABI shims.** Implement the runtime entry points the app actually calls, driven by crashes: `swift_allocObject`,
   metadata accessors/caches, `swift_retain/release`, `swift_getTypeByMangledName*`, witness-table lookups,
   `swift_once`, `swift_demangle`, ObjC bridging (`_bridgeToObjectiveC`, `String` bridging).
5. **Class metadata.** Register the app's Swift classes with real metadata (currently parsed for the ObjC side), so
   `class_getName`, `respondsToSelector`, and ObjC message dispatch agree with Swift's view of them.
6. **Verification.** Instrumented runs: global initializers complete; `App.main` entered; the first SwiftUI call
   identified by name. Exit gate **G1**: the app reaches a SwiftUI framework call, and the remaining blocker is
   SwiftUI itself, proven by a symbol-level trace.

### P2 — SwiftUI / SceneKit strategy (decision gate G2)

Three mutually exclusive routes. Choose with evidence from P1, not optimism.

- **A. Real Apple frameworks.** Load Apple's arm64 SwiftUI/UIKit/SceneKit. Dependency closure includes CoreAnimation,
  Metal, Accessibility, text/rendering services, and XPC to system daemons. Expected verdict: not self-contained on
  Android. Gate question: does a minimal SwiftUI view tree render with only frameworks we can host?
- **B. Compatibility layer.** Implement the SwiftUI ABI subset this binary uses (ViewBuilder, View protocol, layout,
  environment, text, shapes) rendering into the loader compositor, plus SceneKit scene loading for the keys.
  This is a multi-month project. Gate question: how many distinct SwiftUI generic instantiations/metadata symbols
  does the binary require? Produce the count before committing.
- **C. Replica.** Rebuild the calculator natively for Android from the app's assets/scenes/behavior. Fast, visually
  faithful, but it is **not the IPA running** and must be labelled that way.

P2 ends with a written verdict per route (evidence, effort, risk) and a chosen route. Route C is never presented as
success for the loader goal.

### P3 — Execution of the chosen route

Milestones: first app-drawn frame (T2), full keypad (T3), input/arithmetic (T4), lifecycle (T5). Each milestone has a
harness check and a saved frame.

### P4 — Hardening

Themes, launch animation (`FirstLaunchIntro.mp4`), audio, StoreKit stubs, long-run stability, APK packaging with the
normal `build_apk.sh` once `armv7.cpp` compiles again.

## 4. Coordination with the other agent

- Their work: armv7 interpreter, VFP/NEON, 32-bit ObjC bridge, 32-bit `libswiftCore` side image, docs/armv7-support.md.
- Shared files they touch: `src/app/app.hpp` (`send()` routing), `src/app/app_runtime.cpp` (`low_alloc`), `src/app/app_uikit.cpp`
  (bridge registration), `src/objc/objc_runtime.hpp` (`register_placeholder`).
- Rules: re-read shared files immediately before editing; never revert their changes; rebuild after their saves;
  if a build error points into their half-written file, build with the stub and continue.

## 5. Verification harness (every session)

```
export MSYS_NO_PATHCONV=1
adb push xnuroid_loader /data/local/tmp/xnuroid_loader && adb shell chmod 755 /data/local/tmp/xnuroid_loader
adb shell "cd /data/local/tmp/calc324 && LOADER_APP=1 LOADER_ALLOW_UNRESOLVED=1 LOADER_AUDIO=0 \
  LOADER_GAME_DIR=/data/local/tmp/calc324 LOADER_UI_DUMP=/data/local/tmp/ui.ppm timeout -s KILL 20 \
  /data/local/tmp/xnuroid_loader" > run.log 2>&1
```

Pass: exit 137, no `CRASH`/`Aborted`, `ui dump: wrote`, painted pixels recorded. Fail: any abort, missing frame.
No foreground launches on the user's screen.

## 6. Risks

- SwiftUI is closed and deeply tied to iOS userland; routes A and B may both prove infeasible. G2 exists so that is
  discovered with numbers, not opinions.
- Runtime dylib provenance/licensing for side-loading Apple binaries is the user's call.
- Concurrent edits by the other agent can break the build at any moment.
- The app is a moving target only if it updates; 3.24 is fixed.

## 7. G2 evidence so far (2026-11-13)

- Lifecycle: `_$s5UIKit17UIApplicationMain...` + `_$s7SwiftUI19UIHostingControllerC8rootViewACyxGx_tcfc` — the app is a
  UIKit app hosting SwiftUI, not a pure SwiftUI `App`.
- 1,114 SwiftUI symbols in the app's mangled imports (`docs/calc-3.24-symbols.md`), 407 Foundation, 288 StoreKit, 60 Combine.
- SceneKit is used through ObjC classes (`SCNView`, `SCNAction`, `SCNAnimationPlayer`, `SCNSceneRenderer`, ...), with
  129 named geometries in one theme scene alone — the themes are animated 3D characters, not a static keypad.
- Theme JSONs hold colors and character-pair kerning only; no key layout. Nothing in the bundle can be rasterised into
  the calculator UI without the app's code.
- The 3.23 IPA is encrypted (`SC_Info`, cryptid 0) and unusable as a fallback.
- The only Swift runtime present on the device (`/data/local/tmp/fw/libswiftCore.dylib`) is armv7; the arm64 Swift
  runtime would have to be sourced separately.
- Verdict for loader-native: requires an arm64 Swift runtime plus functioning SwiftUI and SceneKit — no incremental
  loader change produces the calculator UI.

## 8. Loader capability work (2026-11-13)

- **Fixed:** Swift classes' relative method lists point their name field at a *selector slot* holding the string, not
  at the string itself. The runtime read the slot bytes as a name, so Swift `@objc` methods were registered under
  garbage selectors and never dispatched (garbage also passed the old class-name validation by luck of ASLR).
  `read_method_list_locked` now validates selector bytes as ASCII and decodes the slot (including an unapplied
  chained-fixup rebase).
- **Result:** the app's `AppDelegate` now exposes `application:didFinishLaunchingWithOptions:` etc., and the loader calls
  into the app's Swift code for the first time. Deterministic across runs.
- **Wall:** the delegate's Swift body calls Swift concurrency runtime entry points (`_$sScMMa` — `Swift.Actor` metadata
  accessor, then `_$sScM6sharedScMvgZ` — `Actor.shared` getter). The loader has no Swift runtime, so the guest traps.
- Next: side-load an arm64 Swift runtime (see P1.2) or implement the ABI subset; after that, SwiftUI remains.

## 9. Launch path after the method-list fix

With three concurrency records answered (`ScMMa`, `ScM6sharedScMvgZ`, `ScA15unownedExecutorScevgTj`), the launch path
now completes: `application:didFinishLaunchingWithOptions:` runs, `ViewController viewDidLoad` runs, appearance
callbacks run, and the loader enters the main run loop with no crash.

The live view tree at that point (LOADER_UI_TREE=1):

```
UIWindow (320x568)
  UIView (UINavigationController's view)
    UIView white 320x568   <- the storyboard's empty view, no subviews
```

No `UIHostingController` host view exists, and no SwiftUI/SceneKit call is recorded by the unresolved-symbol shim during
launch. The app's own Swift code therefore runs its launch overrides but never materialises its SwiftUI content; the
frame remains the storyboard background. This is the SwiftUI boundary, now observed rather than inferred.

The three concurrency records are deliberate placeholders; they keep the launch path alive for runtime work and must be
replaced by real implementations once an arm64 Swift runtime is available.

## 10. UIScene lifecycle (2026-11-14)

- Added `UIScene`, `UIWindowScene`, `UISceneSession`, `UISceneConfiguration`, `UISceneConnectionOptions` stand-ins,
  `UIWindow initWithWindowScene:`, and the scene-delegate call
  (`scene:willConnectToSession:options:`, then `sceneWillEnterForeground:`/`sceneDidBecomeActive:`).
- Reading `UIApplicationSceneManifest` from Info.plist maps `module.Class` to the runtime name
  (`_TtC<len>module<len>Class`).
- Result on the calculator: `scene: andyworks_calculator.SceneDelegate` runs, no crash, still no UI. The delegate's
  Swift body returns without creating a window or hosting controller (no `UIWindow initWithWindowScene:` log), so the
  view tree and white frame are unchanged. The next wall inside the delegate is Swift/ObjC metadata and SwiftUI, not
  the scene plumbing.

## 11. Status log

- 2026-11-14: Scene lifecycle implemented; SceneDelegate executes; no window/UI created; frame stays white.
- 2026-11-14: Launch path completes (delegate, viewDidLoad, appearance) with the concurrency placeholders; view tree has
  no SwiftUI host; white frame. No GUI yet.
- 2026-11-13: Swift method dispatch fixed; delegate executes; stops on missing Swift concurrency symbols. No UI yet.
- 2026-11-13: G2 evidence collected (see section 7). Route A (host Apple frameworks) is not self-contained on Android;
  route B (SwiftUI compatibility) is a multi-month runtime project; route C (replica) is not the IPA. Decision needed
  before spending the next block of work.
- 2026-11-13: T1 met headlessly (25 s, 0 crashes, 2,070,360 px frame). Invalid-free fixed. APK with compositor
  installed, unlaunched. T2..T5 blocked on SwiftUI/Swift runtime. Plan written.
