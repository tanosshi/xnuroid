# armv7 support plan

Companion to [`armv7-support.md`](armv7-support.md) (design) and [`armv7-abi.md`](armv7-abi.md) (data model).
This is the work breakdown: phases, files, acceptance criteria and estimates. Estimates assume one developer
working only on this, and they are deliberately pessimistic — the translator and the shim ABI are the two places
where "looks done" and "works" are far apart.

## Ground rules

1. **arm64 stays green.** Every phase ends with the existing arm64 loader booting an arm64 bundle and
   `tools/ipa_preflight.py` producing byte-identical verdicts for arm64 inputs. No phase may change an arm64
   code path without a test that proves it.
2. **No new dependencies** unless a phase explicitly asks for one. The translator is in-tree so the guest ABI
   stays under our control; an emulator library would fight the low-arena memory model.
3. **Every guest symbol miss is logged.** The existing `UNRESOLVED` machinery is the debugging tool; it stays on
   for all phases.
4. **Stop points are real.** Phases 1-2 are useful on their own (they can run hand-written armv7 test images);
   phases 4-5 are where the sample actually needs everything to line up.

## Phase 0 — foundation (done)

| item | state |
| --- | --- |
| loader recognises `MH_MAGIC`, refuses with a specific message, exit 1 | done, device-verified |
| `tools/ipa_preflight.py` parses 32-bit Mach-O, reports `unsupported`, exit 5 | done, verified on the sample |
| device capability measured: no AArch32, no 32-bit ELF, no `/system/lib` | done |
| low-address `mmap` proven at 0x10000000-0x70000000 | done |
| 32-bit import census (indirect symbol table, avoids bind decoding) | done in `build/32census.py` (scratch) |
| sample identified: armv7, iOS 9.3, **Swift 2**, private `Calculate`/`AppSupport`, 512 bound symbols | done |

Phase-0 correction: the preflight tool had been handing `classic_imports` the rebase/lazy pair instead of the
bind/lazy pair, so every classic-binary census before this was wrong. With the offsets fixed, the sample yields
512 bound symbols (73 `_swift_*`, 67 `_OBJC_CLASS_$_`, 22 dylibs), and both bind streams decode to their exact
lengths with no `BIND_OPCODE_THREADED` opcodes.

## Phase 1 — 32-bit image loader and relocations (2-3 weeks)

Status: the core is implemented in `src/loader/armv7.cpp` and runs on the device. The sample maps at a low arena,
applies 6651 rebases and 1068 binds into 512 import stubs, and its entry point executes 22 ARM instructions before
calling `_swift_once`. What remains for this phase is the arm64-side decoder parameterisation (below) and the
malformed-image tests; the interpreter in the same file is a first slice of phase 2, not the phase-2 deliverable.

Deliverable: an armv7 Mach-O loads into the low arena, every rebase/bind is applied with 4-byte slots, and the
loader prints a complete import census before refusing to execute (no translator yet).

Tasks:

1. `src/loader/macho32.hpp` — 32-bit header/segment/load-command reader, mirroring `chained_fixups.hpp` style.
2. `src/loader/dyld_opcodes.hpp` — shared rebase/bind decoder with a `pointer_size` parameter (4 for armv7,
   8 for arm64). No threaded-bind support is needed for this sample, but the decoder must reject unknown opcodes
   loudly rather than walk past them.
3. `src/loader/low_arena.*` — reserve 0x10000000-0x80000000 with guard pages, fixed mappings, a bump allocator
   for images and a heap for guest objects.
4. Wire the 32-bit branch into `android_loader.cpp` behind `MH_MAGIC`: map segments, apply fixups, log
   `[*] armv7 image ... (N rebases, M binds)`.
5. Preflight tool: report the same counts for 32-bit bundles (done: 512 symbols; the loader's on-device number
   must match).

Acceptance:

- the sample reports 512 bound symbols and 22 dylibs on device, matching the indirect-symbol census (255 GOT
  slots) plus the direct data binds;
- a deliberately malformed 32-bit image fails with a named error, not a segfault;
- arm64 regression: sample arm64 bundle still reaches the Swift preflight refusal with identical output.

## Phase 2 — translator core (4-6 weeks)

Deliverable: hand-written armv7 test Mach-Os execute and return values through the translator on the device.

Tasks:

1. `src/armv7/decoder.*` — ARM and Thumb-2 decode to an IR; unknown encodings trap with PC, opcode and a
   symbolised address.
2. `src/armv7/interp.*` — integer, branch, load/store, VFP (s/d registers), `MRS`/`MSR`, barriers.
3. `src/armv7/frame.*` — guest register file, per-thread state, host->guest call frames.
4. Bridge stubs: one 8-byte region per host symbol in the trampoline page; the interpreter intercepts execution
   there and calls the host function.
5. Instruction-coverage harness: a table of encodings the sample's `__text` actually contains, extracted offline,
   so "coverage" is measured against this app rather than against ARM's manual.

Acceptance:

- a test image that computes and returns 42 runs on the phone;
- a test image that calls a host shim (`NSLog`, `strlen`) prints and returns correctly;
- the sample's first 1,000 instructions execute without an unknown-encoding trap.

Estimate risk: Thumb-2 is large. The mitigation is frequency-ordered coverage: the sample's own instruction
histogram (phase 1 tooling) drives the decoder backlog.

## Phase 3 — 32-bit ObjC runtime and message send (4-6 weeks)

Deliverable: a hand-written armv7 ObjC test image allocates a class, sends messages and prints through a shim.

Tasks:

1. 32-bit metadata readers (`class_t`, `class_ro_t`, `method_t`, `ivar_t`, `category_t`, `protocol_t`).
2. `objc_msgSend`/`_stret` over the guest register file, with cache, superclass walk, forwarding and
   `respondsToSelector:` fast paths.
3. `call_guest()` typed helpers for int/pointer/float/double/struct returns, used by every shim that calls an IMP.
4. Class realisation: resolve superclasses, install methods/categories/protocols, handle `+load` and
   `__objc_classlist`.
5. Tagged pointers, associated objects, weak references (32-bit layout).

Acceptance:

- the ObjC test image's `-[Greeter greet:]` returns a guest-built NSString and the shim logs it;
- `class_getInstanceSize`, `object_getClass`, `method_getImplementation` agree with the guest's own numbers;
- 200 consecutive guest<->host round trips leak nothing (allocation counters flat).

## Phase 4 — guest system libraries from an IPSW (4-6 weeks)

Deliverable: `libswiftCore.dylib` (armv7) runs under the translator and the sample's 73 `_swift_*` imports bind
to it.

Tasks:

1. IPSW acquisition and `dyld_shared_cache_armv7` extraction (documented tooling, not checked in).
2. Cache splitter: produce individual armv7 dylibs with their fixups normalised to binds.
3. Load them as guest images (phase 1 loader) at fixed low addresses; resolve cross-dylib binds inside the arena.
4. Swift 2 metadata checks: allocate a class via `swift_allocObject`, run `swift_getGenericMetadata1`, and
   exercise `swift_dynamicCast` between two guest classes.
5. Decide the `libobjc.A.dylib` boundary: keep our runtime for the app's ObjC surface, load Apple's only if the
   Swift runtime demands internals we do not implement (message refs, `objc_opt_*` internals).

Acceptance:

- a Swift test binary (built for armv7 with an old toolchain, or a hand-assembled equivalent) prints from Swift;
- the sample's Swift initialisers run to completion without an unresolved `_swift_*` symbol;
- no host `dlsym` fallback is used for any `_swift_*` import.

Gate: if a suitable armv7 `libswiftCore` cannot be obtained or does not run, the sample is out of reach; the
project should then stop at "armv7 ObjC apps" and document that limit.

## Phase 5 — shims with a 32-bit data model (6-8 weeks)

Deliverable: UIKit/Foundation/CoreGraphics shims accept armv7 structs and drive the Android surface.

Tasks:

1. `CGFloat` becomes word-size aware in `app.hpp`; geometry types derive from it; `NSNotFound` fixed.
2. Every shim entry that takes/returns a guest struct uses `guest_read`/`guest_write` helpers that know the
   guest word size, instead of casting.
3. Audit the shim tables for arm64-only assumptions: `uintptr_t` truncation, `NSInteger` widths, `BOOL` return
   widening, selector/IMP widths.
4. Font/text path: CoreText shims take 32-bit `CGRect`/`CGPoint`; verify with a CGRect round-trip per family.
5. Run the existing arm64 bundles after every batch; the word-size branch must be dead for arm64.

Acceptance:

- a 32-bit UIKit test app shows a window, a label and a button, and receives a touch;
- the calculator renders its first frame;
- arm64 bundles render identically (frame hash or screenshot comparison).

## Phase 6 — performance and packaging (4 weeks)

Deliverable: interactive frame rates for the sample, one APK that carries the translator.

Tasks:

1. Block translator (AArch32 -> AArch64) with a per-block instruction budget and invalidation on code writes.
2. Profile: guest instruction count per frame, host time per guest call, shim time.
3. APK: no new ABI directory; the same arm64 `libxnuroid.so` gains `LOADER_ARMV7=1` and automatic detection by
   magic. Bundle the IPSW-derived dylibs as optional assets with a licence note.
4. Regression suite: arm64 bundles, armv7 test images, sample boot.

Acceptance:

- sample is usable (>= 30 fps on the test device, measured, not estimated);
- APK size grows by the dylib assets only;
- `LOADER_ARMV7=0` restores today's behaviour bit-for-bit.

## Schedule

| phase | estimate | cumulative |
| --- | --- | --- |
| 0 foundation | done | — |
| 1 loader + relocations | 2-3 wk | 3 wk |
| 2 translator core | 4-6 wk | 9 wk |
| 3 ObjC 32 | 4-6 wk | 15 wk |
| 4 guest Swift runtime | 4-6 wk | 21 wk |
| 5 shims 32-bit | 6-8 wk | 29 wk |
| 6 performance + packaging | 4 wk | 33 wk |

Roughly **7-8 months** to a booting Swift app, with the honest caveat that phases 4 and 5 can each discover a
blocker that adds weeks. The first device-visible milestone (an armv7 Mach-O executing `return 42` through the
translator) arrives at the end of phase 2, about nine weeks in.

## Alternatives considered

| route | verdict |
| --- | --- |
| ship an `armeabi-v7a` build | impossible on this phone: no 32-bit linker or libraries (measured) |
| run a separate emulator app | the guest needs the host's GPU, audio, files and UIKit shims; a second process cannot share them without a protocol as large as the loader |
| embed Unicorn/Dynarmic | viable CPU cores, but they own the address space; the low-arena model and the shim bridge would be rebuilt around them, and armv7 support in either is a dependency with its own bugs |
| in-process translator (chosen) | keeps the existing shim and graphics stacks, keeps addresses real, and makes arm64 regressions impossible by construction |

## Immediate next tasks

1. Parameterise the loader's classic rebase/bind loops (`android_loader.cpp`, ~line 3950 and ~line 4040) with the
   guest pointer size, and verify the armv7 4-byte write path on a test image.
2. Add `src/loader/macho32.hpp` and the low-arena allocator; load the sample's segments and print the fixup
   counts. No execution yet.
3. Extract the sample's instruction histogram from `__text` to drive the Thumb-2 decoder backlog.
