# 32-bit (armv7) iOS bundles on a 64-bit-only phone

Status: first stage implemented and device-verified. The loader now parses a 32-bit Mach-O, maps its segments into
an arena below 4 GB, applies the classic dyld rebases and binds with 4-byte slots, and executes the ARM entry point
under a small interpreter until guest code calls an imported symbol. For the sample it reports 6651 rebases, 1068
binds and 512 import stubs, then Apple's `main` runs 22 instructions and calls `_swift_once` with
`_globalinit_33_1BDF70FFC18749BAB495A73B459ED2F0_token5`/`_func5`, the Swift global-initializer pair.
`tools/ipa_preflight.py` still reports armv7 bundles as `unsupported` (exit 5), which is the honest verdict for the
loader's app-mode path: there is no 32-bit UIKit/Swift runtime behind the interpreter yet.

## Why this is not a build flag

The loader runs guest code natively: the iOS binary's instructions execute on the device CPU inside the host
process. That works for arm64 because the guest ABI and the host ABI are the same. For armv7 there is no host ABI
to borrow: a 64-bit Linux process cannot drop to AArch32 at EL0, and the phone in question has no 32-bit userland
at all.

Measured on the test device (SM-S926B, Android SDK 37, ARMv9 SoC):

```
ro.zygote               = zygote64
ro.product.cpu.abilist  = arm64-v8a
ro.product.cpu.abilist32= (empty)
/system/bin/linker      = missing
/system/lib             = missing
/proc/cpuinfo           = no "aarch32"
```

A trivial 32-bit ELF built with the NDK is rejected by the kernel before `main`:

```
$ /data/local/tmp/t32
/system/bin/sh: /data/local/tmp/t32: not executable: 32-bit ELF file
```

So on this device "32-bit support" can only mean **executing armv7 instructions through a translator inside the
existing arm64 process**. Shipping an `armeabi-v7a` build (the cheaper option for older phones) is impossible here,
because the OS has no 32-bit linker or libraries to load it.

## What does work: the low address space

A 32-bit guest pointer is 32 bits wide, so all guest-visible memory must be reachable below 4 GB. Android does not
provide `MAP_32BIT`, but fixed mappings in the low range are allowed. Measured on the device:

```
fixed 0x10000000 -> 0x10000000 (OK)
fixed 0x20000000 -> 0x20000000 (OK)
fixed 0x40000000 -> 0x40000000 (OK)
fixed 0x70000000 -> 0x70000000 (OK)
```

That is the foundation of the design: **one reserved arena below 4 GB holds every guest object, image and stack**,
so a guest `uint32_t` pointer is a valid host pointer and the existing shim code can keep using real addresses.
No handle table, no pointer translation on every dereference.

## Architecture

```
+-------------------------------------------------------------------+
| libxnuroid.so (arm64 process)                                      |
|                                                                    |
|  app shims (src/app)      guest dylibs (libswiftCore, ...)         |
|  arm64 host code          armv7 code, run by the translator        |
|         \                        /                                 |
|          \                      /                                  |
|        +------------------------------+                            |
|        | ABI bridge (armv7 <-> host)   |                           |
|        +------------------------------+                            |
|                     |                                              |
|        +------------------------------+       +-----------------+  |
|        | AArch32 translator            |<----->| low arena       |  |
|        | (interp, later block JIT)     |       | < 4 GB          |  |
|        +------------------------------+       +-----------------+  |
|                     |                                              |
|        +------------------------------+                            |
|        | 32-bit image loader           |                            |
|        | Mach-O 32, dyld opcodes       |                            |
|        +------------------------------+                            |
+-------------------------------------------------------------------+
```

### 1. 32-bit image loader

New sibling of the existing 64-bit path (`android_loader.cpp`, `chained_fixups.hpp`), selected by
`MH_MAGIC` (`0xfeedface`):

- `LC_SEGMENT` (0x1) instead of `LC_SEGMENT_64`; segments reserved in the low arena.
- `LC_ENCRYPTION_INFO` (0x21); `cryptid` check as today.
- `LC_DYLD_INFO(_ONLY)` rebase/bind with **4-byte pointer slots**. The current loops hardcode 8-byte strides and
  `uint64_t` stores; they need a pointer-size parameter.
- Bind streams decode cleanly for this sample: regular bind (259 symbols, 458 binds) and lazy bind
  (254 symbols) contain no `BIND_OPCODE_THREADED` (0xD0) opcodes, so no ordinal table is involved. The slots are
  4 bytes; the existing loops are hardcoded to 8.
- `LC_MAIN` and `LC_UNIXTHREAD` entry points; iOS 9 images often use the latter.
- ObjC 2 metadata fixups (selrefs, classrefs, classlist, categorylist, protocolrefs), 4-byte slots.

### 2. Translator

The guest CPU model is ARMv7-A + Thumb-2 + VFPv3/NEON, which is what iOS 9 armv7 apps were built for.

- **Phase A: interpreter.** Decode and execute one instruction at a time against the guest register file.
  Simple to make correct, roughly 10-50 MIPS on a modern phone — enough to boot a UI and prove the stack.
- **Phase B: block translator.** Cache decoded basic blocks as arm64 code. Needed for anything interactive;
  optional for the calculator.
- Interrupt point: a per-block instruction counter so the host can service timers, audio callbacks and the run
  loop. Guest code is otherwise run to a yield.
- Guest exceptions (ObjC `@throw`, Swift traps) unwind through translator frames; every host->guest call
  records a recoverable frame.

### 3. ABI bridge

Two directions, described in detail in [`armv7-abi.md`](armv7-abi.md):

- **guest -> host shim**: the guest's `__got`/`__la_symbol_ptr` entries point at a stub in the translator's
  address range. The translator recognises the target as a host function, marshals r0-r3/stack and s0-s15/d0-d7
  into a `CallFrame`, calls the C++ shim, and writes the result back.
- **host -> guest**: an IMP is a guest address. `call_guest()` pushes a translator frame, sets r0/r1, runs until
  the return address, and hands back r0/r1 or s0/d0. `objc_msgSend` is a host function that dispatches with the
  guest's register file and calls the guest IMP through this route.

### 4. 32-bit Objective-C runtime

Apple's 32-bit metadata layouts differ from arm64 in every structure that embeds a pointer:
`class_t`, `class_ro_t`, `method_t`, `ivar_t`, `category_t`, `protocol_t`. The runtime walks the guest's own
metadata; it does not convert it. Tagged pointers use the 32-bit encoding. `objc_msgSend` is implemented in C++
against the guest register file rather than in assembly, because a C function can read the armv7 argument
registers from the frame the translator saved.

### 5. Guest system libraries

Shims cannot reimplement Swift. The sample imports 73 `_swift_*` runtime entry points
(`swift_allocObject`, `swift_getGenericMetadata1`, `swift_dynamicCast`, the value-witness helpers, the weak
reference family). Those semantics — metadata allocation, generic instantiation, existential layout, refcounting —
are not something to clone by hand.

Instead, run **Apple's own 32-bit dylibs** through the translator:

- extract `dyld_shared_cache_armv7` from an iOS 9.x IPSW for an armv7 device;
- split out `libswiftCore.dylib`, `libswiftFoundation.dylib`, the other `libswift*` overlays, and
  `libobjc.A.dylib` if the guest needs its internals;
- load them as guest images at fixed low addresses, with the cache's shared-cache fixups converted to normal
  binds.

This is the same technique as running the app's own code: the translator does not care whether the armv7
instructions came from the app or from Apple's runtime. It does require an IPSW, which is a public download.

UIKit/Foundation/CoreGraphics stay shimmed (as today) because they must ultimately drive Android's surface,
audio and text stack.

### 6. Shim ABI for 32-bit

The shim sources compile once, for arm64, but must read guest structures with 32-bit layouts: `CGFloat` is
`float`, `CGRect` is 16 bytes, `NSRange` is 8, `NSUInteger` is 4. Two rules keep this tractable:

- every geometry type derives from `CGFloat` (`app.hpp` currently hardcodes `double` in `CGPoint`, `CGSize`,
  `CGRect`, `CGAffineTransform`);
- every shim that takes or returns a guest struct goes through a reader/writer that knows the guest word size,
  instead of casting a host pointer.

## The sample bundle, measured

`Calculator.app.ipa` (the 32-bit one):

| property | value |
| --- | --- |
| magic / cpu | `feedface`, cputype 12 subtype 9 → **armv7** |
| min OS | 9.3.0 (32-bit iOS ends at 10.3.3) |
| encryption | no `LC_ENCRYPTION_INFO`; code readable |
| executable | 1,233,120 bytes, entry `0x5204` |
| dylibs | 22, including `Calculate.framework`, `libswiftCore`, `libswiftUIKit`, `AppSupport`, `MobileGestalt` |
| imports | 512 bound symbols (255 of them indirect/GOT slots): 73 `_swift_*`, 67 `_OBJC_CLASS_$_`, 22 dylibs |
| bind format | classic dyld: rebase + regular bind + lazy bind, no threaded opcodes |

Two consequences:

1. It is a **Swift 2 app**, so the guest Swift runtime is mandatory; it cannot be shimmed the way UIKit is.
2. It is a system app using private frameworks (`Calculate`, `AppSupport`, `MobileGestalt`). Those are not on
   Android and are not in the loader's tables; each symbol has to be provided by a shim or by loading the
   framework from the IPSW cache.

## What stays shared with arm64

- The arm64 loader path is untouched: the 32-bit path is selected by the Mach-O magic, and all new code is
  reachable only for `MH_MAGIC` images.
- The shim implementations are shared; only their struct readers and the `CGFloat` typedef are word-size aware.
- The preflight tool keeps classifying both bitnesses; arm64 verdicts must not change.

## Testing

| level | test |
| --- | --- |
| unit (host) | translator decodes/executes known armv7 sequences against a reference model |
| unit (host) | 32-bit rebase/bind writes 4-byte slots |
| device | a hand-written armv7 Mach-O that returns 42 through the translator |
| device | the sample loads, runs `main`, installs its first UIWindow, draws one frame |
| regression | every existing arm64 bundle still boots; `tools/ipa_preflight.py` output for arm64 unchanged |

## Risks

| risk | impact | mitigation |
| --- | --- | --- |
| Thumb-2 decoder gaps | wrong execution, hard-to-find crashes | cover instructions by frequency; trap unknown encodings with PC + opcode |
| Swift 2 runtime needs cache-era dylibs | app cannot start | validate with the IPSW cache early (phase 4 gate) |
| private frameworks (`Calculate`, `AppSupport`) | missing classes/selectors | stub the small surface the app actually calls; log every miss |
| shim struct ABI (CGFloat=float) | geometry garbage, silent misbehaviour | word-size readers + a CGRect round-trip test per shim family |
| interpreter speed | unusable UI | block JIT in phase 6; measure with a frame-time budget |
| scope | months of work | milestones below are independently useful; stop points are explicit |

## Non-goals

- Running 32-bit *Android* apps (the OS handles that, or cannot).
- ARMv7 games with heavy NEON/JIT workloads; the interpreter targets UI-class apps first.
- 32-bit binaries on the APK path: the shipped app remains arm64-only; armv7 support lives in the same
  arm64 binary via the translator.
