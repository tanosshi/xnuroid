.text
.align 4
.extern native_mmap_impl
.extern callframe_dispatch

// Generic C++ Shim Trampoline with ARMv8.5+ BTI support.
// Incoming: x17 = &TrampolineEntry { handler, symbol_name }, x0-x8 = guest args.
// Loads handler from [x17]. Does not touch x0-x8 before they are spilled.
.globl callframe_trampoline
.type callframe_trampoline, %function
callframe_trampoline:
    hint #34

    sub sp, sp, #192

    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]

    stp d0, d1, [sp, #64]
    stp d2, d3, [sp, #80]
    stp d4, d5, [sp, #96]
    stp d6, d7, [sp, #112]

    add x8, sp, #192
    stp x8, lr, [sp, #128]

    stp x0, x1, [sp, #144]
    str x8, [sp, #160]

    // callframe_dispatch(CallFrame*, TrampolineEntry*) records the call in the
    // shim ring buffer, then invokes entry->handler(frame, entry).
    adrp x16, callframe_dispatch
    add x16, x16, :lo12:callframe_dispatch
    mov x0, sp
    mov x1, x17
    blr x16

    ldp x0, x1, [sp, #144]
    ldr lr, [sp, #136]
    add sp, sp, #192
    ret

.size callframe_trampoline, .-callframe_trampoline

// Raw entry capture for guest mmap. Spills x0-x7/lr/sp into a ring, then
// tail-calls the C implementation with the original argument registers.
.globl native_mmap
.type native_mmap, %function
native_mmap:
    hint #34

    adrp x16, g_mmap_capture_count
    add x16, x16, :lo12:g_mmap_capture_count
    ldr x17, [x16]
    add x17, x17, #1
    str x17, [x16]
    sub x17, x17, #1
    and x17, x17, #15
    lsl x16, x17, #6
    lsl x17, x17, #4
    add x17, x16, x17
    adrp x16, g_mmap_capture
    add x16, x16, :lo12:g_mmap_capture
    add x16, x16, x17
    stp x0, x1, [x16, #0]
    stp x2, x3, [x16, #16]
    stp x4, x5, [x16, #32]
    stp x6, x7, [x16, #48]
    str lr, [x16, #64]
    mov x17, sp
    str x17, [x16, #72]
    b native_mmap_impl

.size native_mmap, .-native_mmap

.bss
.align 4
.globl g_mmap_capture
.hidden g_mmap_capture
g_mmap_capture:
    .zero 1280

.globl g_mmap_capture_count
.hidden g_mmap_capture_count
g_mmap_capture_count:
    .zero 8
