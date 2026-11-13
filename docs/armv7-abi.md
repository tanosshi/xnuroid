# armv7 ABI and data model

Companion to [`armv7-support.md`](armv7-support.md). Everything here is what the translator, the 32-bit ObjC
runtime and the shared shims must agree on. Values are for iOS armv7 (AAPCS32), not for Android.

## Data model

| type | bytes | notes |
| --- | --- | --- |
| `id`, `Class`, `SEL`, `IMP`, any pointer | 4 | guest pointers live in the low arena, so host `void*` fits |
| `NSInteger`, `NSUInteger`, `CFIndex` | 4 | `long` on armv7 |
| `BOOL` | 1 (`signed char` in ObjC) | returned zero-extended in r0 |
| `CGFloat` | **4** | `float`, unlike arm64's `double` |
| `CGPoint` / `CGSize` | 8 | two floats |
| `CGRect` | **16** | four floats |
| `CGAffineTransform` | 24 | six floats |
| `NSRange` | 8 | two `NSUInteger` |
| `NSDecimal` | 20 | unchanged by bitness |
| `CFTimeInterval`, `NSTimeInterval` | 8 | `double` on both |
| `mach_port_t`, `pthread_t`, `int`-like handles | 4 | |

Consequences for `src/app/app.hpp`: `CGPoint`, `CGSize`, `CGRect`, `CGAffineTransform` currently hardcode
`double`; they must be built from a `CGFloat` typedef that is `float` in the 32-bit data model. `NSInteger`
already follows the compiler's `long`, but `NSNotFound` is written as a 64-bit constant and truncates to
`0xFFFFFFFF` on a 32-bit guest; it must be `NSIntegerMax`.

## Calling convention (AAPCS32)

| role | registers |
| --- | --- |
| integer args 1-4 | r0-r3 |
| further args | stacked, 4-byte aligned, caller-cleans |
| return (integer, pointer, <=4-byte struct) | r0 (r1 for 8-byte pairs) |
| return (struct >4 bytes) | caller passes a buffer address in r0, callee writes it (no `sret` register) |
| float args | s0-s15 (VFP), also prototyped-only; iOS builds use hardfp |
| float return | s0 (float), d0 (double) |
| callee-saved | r4-r11, s16-s31/d8-d15, sp |
| special | r12 ip, r13 sp, r14 lr, r15 pc |
| stack alignment | 8 bytes at every public interface |

The host side keeps a `CallFrame` with `r[16]`, `s[32]`/`d[16]` and a flags word. Host shims are ordinary C++
functions; the bridge fills a frame from the guest registers before calling one and copies results back.

Guest code that calls a host shim does so through a slot that the translator recognises as a bridge address.
The translator must not execute those addresses; it intercepts them, including `BLX` through a register.

## `objc_msgSend` on armv7

- `r0` = receiver, `r1` = selector, `r2`, `r3` and the stack carry the method arguments.
- `objc_msgSend_stret` is used when the return type is a struct too large for r0-r3; the buffer address is the
  first argument.
- `objc_msgSend_fpret` exists for x86 only; armv7 returns floats in s0/d0 through the normal entry point.
- Dispatch is implemented in C++ against the saved register file: look up the class from the receiver's isa
  (tagged pointers first), find the IMP, then `call_guest()` with the same argument layout. The result lands in
  the frame's r0/r1 or s0/d0 and is returned to the guest.

This avoids writing armv7 assembly for message send entirely; the cost is a frame copy per message, which the
interpreter path pays anyway.

## Objective-C 32-bit metadata

All pointers below are 4 bytes. The runtime reads the guest's structures in place.

```
objc_object         isa                                      4
class_t             isa, superclass, cache, vtable, data      20   (data's low bits carry the class bits)
class_ro_t          flags, instanceStart, instanceSize,
                    ivarLayout, name, baseMethods,
                    baseProtocols, ivars, weakIvarLayout,
                    baseProperties                              40
method_t            name, types, imp                         12
ivar_t              offset, name, type, alignment, size       20
category_t          name, cls, instanceMethods, classMethods,
                    protocols, instanceProperties,
                    classProperties (newer)                 24-28
protocol_t          isa, name, protocols, instanceMethods,
                    classMethods, optionalInstanceMethods,
                    optionalClassMethods, instanceProperties,
                    size, flags, extendedMethodTypes        44+
property_t          name, attributes                          8
```

Layout rules that differ from arm64 and must not be copied from the 64-bit path:

- `class_ro_t.instanceStart` and `instanceSize` are 32-bit; ivar offsets are 32-bit.
- `method_t.imp` is a 32-bit guest address, resolved through the 32-bit image's rebases.
- the class bits (`RW_REALIZED`, `RW_META`, ...) live in the low bits of `data`, as on 64-bit pre-optimized
  runtimes.
- tagged pointers use the low bit; the payload occupies the remaining 31 bits.
- `objc_msgSend` cache buckets store `SEL`/`IMP` pairs of 4 bytes each.

## Guest memory

| region | address | notes |
| --- | --- | --- |
| guest images | `0x1000_0000` upwards | each `__TEXT` page-aligned, `__DATA` writable |
| guest heap | `0x2000_0000` upwards | bump allocator plus free lists; all ObjC/Swift objects |
| translator trampolines | `0x3000_0000` | one page per imported host symbol, `BRK`-style trap |
| guest stacks | `0x7000_0000` downwards | main stack 1 MB, each guest thread 512 KB |
| guest dyld cache dylibs | allocated in the images area | `libswiftCore` and friends |

Measured: fixed mappings at 0x10000000, 0x20000000, 0x40000000 and 0x70000000 all succeed on the test device, so
the whole model fits below 4 GB with room to spare. Every region gets guard pages; the 32-bit runtime validates
that a guest pointer lies inside a mapped region before dereferencing it in the debug build.

## Threads and TLS

- armv7 guest TLS uses `TPIDRURW` (cp15 c13, c0, 2) and `TPIDRURO` (c0, 3); the translator has to service the
  corresponding `MRC`/`MCR` encodings and keep a per-thread block in the low arena.
- `pthread_*` imports are already mapped to bionic; the guest's `pthread_t` is 4 bytes, the host's is a pointer,
  so the bridge keeps a small handle table in the low arena rather than truncating host pointers.
- `LDREX`/`STREX` are executed as ordinary loads/stores; a lock prefix is added when the guest actually
  contends on a lock (single-core guest execution is the default and matches what the shims assume).

## Swift 2 runtime boundary

The sample's 73 `_swift_*` imports are Swift 2-era entry points. They are not shimmed; the real 32-bit
`libswiftCore.dylib` runs under the translator (see the parent document). The boundary therefore only needs:

- correct armv7 calling convention for the imported functions (covered above);
- guest metadata produced by the app's compiler to be readable by that runtime — which it is, because both are
  armv7 code from the same era;
- ObjC bridging (`swift_bridgeObjectRetain`, `swift_convertStringToNSString`) to reach the shimmed Foundation
  classes through the 32-bit ObjC runtime.

`swift_once` maps to the host's `pthread_once` semantics; `swift_slowAlloc` must return low-arena memory.

## Error and exception conventions

- Guest `@throw` unwinds through guest frames only; the translator keeps a per-thread handler chain and restores
  sp/lr from the frame records.
- A host shim that cannot continue raises a guest-visible ObjC exception by calling the 32-bit
  `objc_exception_throw` with a shim-created object; it must never `longjmp` across translator frames.
- `errno` for guest libc calls is the host's per-thread errno; armv7 and arm64 both use `__errno`.
