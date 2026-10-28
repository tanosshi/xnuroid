.text
.align 4
.extern native_mmap_impl
.extern callframe_dispatch
.extern direct_trace_log

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

// Logging forwarder for imports bound directly to bionic (debug mode only).
// Incoming: x17 = &DirectTraceEntry { target, name, calls }, x0-x8/q0-q7 = guest args.
// Saves every argument register, calls direct_trace_log(entry, guest_lr, &saved_x0),
// restores everything and TAIL-JUMPS (br) to the target, so the target sees the
// guest's original lr, sp and stack arguments.
.globl direct_trace_forward
.type direct_trace_forward, %function
direct_trace_forward:
    hint #34
    stp x29, x30, [sp, #-224]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    stp x8, x17, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    mov x0, x17
    mov x1, x30
    add x2, sp, #16
    bl direct_trace_log
    ldp q6, q7, [sp, #192]
    ldp q4, q5, [sp, #160]
    ldp q2, q3, [sp, #128]
    ldp q0, q1, [sp, #96]
    ldp x8, x17, [sp, #80]
    ldp x6, x7, [sp, #64]
    ldp x4, x5, [sp, #48]
    ldp x2, x3, [sp, #32]
    ldp x0, x1, [sp, #16]
    ldp x29, x30, [sp], #224
    ldr x16, [x17]
    br x16

.size direct_trace_forward, .-direct_trace_forward

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

// Darwin-ABI setjmp / longjmp. The guest's jmp_buf is only 192 bytes (int[48]);
// bionic's is 256, so the bionic routines would overrun it. These are bound directly
// as the import targets (no C shim, no extra frame), so lr and sp stay the guest's.
// Layout inside the 192-byte buffer (168 bytes used):
//   +0   x19,x20   +16 x21,x22   +32 x23,x24   +48 x25,x26   +64 x27,x28
//   +80  x29,x30   +96 sp        +104 d8..d15 (64 bytes)
// Signal mask is not saved or restored (sigsetjmp savemask is ignored).
.globl darwin_setjmp
.type darwin_setjmp, %function
darwin_setjmp:
    hint #34
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x2, sp
    str x2, [x0, #96]
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mov w0, #0
    ret
.size darwin_setjmp, .-darwin_setjmp

.globl darwin_longjmp
.type darwin_longjmp, %function
darwin_longjmp:
    hint #34
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x2, [x0, #96]
    mov sp, x2
    ldp d8, d9, [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]
    cmp w1, #0
    csinc w0, w1, wzr, ne
    ret
.size darwin_longjmp, .-darwin_longjmp

// int darwin_jmp_fp_selftest(void* buf192): returns 1 if d8-d15 are restored by longjmp.
// Written in assembly so the compiler cannot touch the callee-saved FP registers.
// Loads known values into d8-d15, setjmp, zeroes d8-d15, longjmp, then verifies them.
.globl darwin_jmp_fp_selftest
.type darwin_jmp_fp_selftest, %function
darwin_jmp_fp_selftest:
    hint #34
    stp x29, x30, [sp, #-112]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp d8, d9, [sp, #32]
    stp d10, d11, [sp, #48]
    stp d12, d13, [sp, #64]
    stp d14, d15, [sp, #80]
    mov x19, x0
    .irp n, 8, 9, 10, 11, 12, 13, 14, 15
    mov x1, #(0x1100 + \n)
    movk x1, #0xabcd, lsl #48
    fmov d\n, x1
    .endr
    mov x0, x19
    bl darwin_setjmp
    cbnz w0, .Lfp_after_longjmp
    .irp n, 8, 9, 10, 11, 12, 13, 14, 15
    movi d\n, #0
    .endr
    mov x0, x19
    mov w1, #5
    bl darwin_longjmp
.Lfp_after_longjmp:
    cmp w0, #5
    b.ne .Lfp_fail
    .irp n, 8, 9, 10, 11, 12, 13, 14, 15
    fmov x1, d\n
    mov x2, #(0x1100 + \n)
    movk x2, #0xabcd, lsl #48
    cmp x1, x2
    b.ne .Lfp_fail
    .endr
    mov w0, #1
    b .Lfp_done
.Lfp_fail:
    mov w0, #0
.Lfp_done:
    ldp d14, d15, [sp, #80]
    ldp d12, d13, [sp, #64]
    ldp d10, d11, [sp, #48]
    ldp d8, d9, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #112
    ret
.size darwin_jmp_fp_selftest, .-darwin_jmp_fp_selftest

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

// ---------------------------------------------------------------------------
// Objective-C message dispatch for guest classes (see src/objc_runtime.hpp).
//   objc_msgSend_guest:      x0 = receiver, x1 = selector. If the receiver is a guest class/instance with a matching
//                            method, tail-jump to its IMP (all argument registers and lr untouched). Otherwise
//                            tail-call objc_fallback_c(x0..x7), the fake-Foundation / nil-message path.
//   objc_msgSendSuper2_guest: x0 = &objc_super{receiver, current_class}; lookup starts at the current class's super.
// Both are bound directly as the imports, so the guest's lr/sp stay in place for the IMP.
// ---------------------------------------------------------------------------
.text
.balign 16
.globl objc_msgSend_guest
.type objc_msgSend_guest, %function
objc_msgSend_guest:
    hint #34
    cbz x0, 1f
    stp x29, x30, [sp, #-224]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    bl objc_lookup_imp_c
    mov x16, x0
    ldp q6, q7, [sp, #192]
    ldp q4, q5, [sp, #160]
    ldp q2, q3, [sp, #128]
    ldp q0, q1, [sp, #96]
    ldr x8, [sp, #80]
    ldp x6, x7, [sp, #64]
    ldp x4, x5, [sp, #48]
    ldp x2, x3, [sp, #32]
    ldp x0, x1, [sp, #16]
    ldp x29, x30, [sp], #224
    cbz x16, 2f
    br x16
2:  b objc_fallback_asm
1:  mov x0, #0
    mov x1, #0
    movi v0.2d, #0
    movi v1.2d, #0
    ret
.size objc_msgSend_guest, .-objc_msgSend_guest

.globl objc_msgSendSuper2_guest
.type objc_msgSendSuper2_guest, %function
objc_msgSendSuper2_guest:
    hint #34
    stp x29, x30, [sp, #-224]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    bl objc_lookup_super_imp_c
    mov x16, x0
    ldp q6, q7, [sp, #192]
    ldp q4, q5, [sp, #160]
    ldp q2, q3, [sp, #128]
    ldp q0, q1, [sp, #96]
    ldr x8, [sp, #80]
    ldp x6, x7, [sp, #64]
    ldp x4, x5, [sp, #48]
    ldp x2, x3, [sp, #32]
    ldp x0, x1, [sp, #16]
    ldp x29, x30, [sp], #224
    ldr x0, [x0]            // receiver out of the objc_super struct
    cbz x16, 2f
    br x16
2:  b objc_fallback_asm
.size objc_msgSendSuper2_guest, .-objc_msgSendSuper2_guest

// Calls the C fallback with the original argument registers, then loads d0-d3 from the thread-local return area
// the fallback may have filled (CGFloat / CGRect / float answers) while keeping x0 as its integer answer.
.globl objc_fallback_asm
.type objc_fallback_asm, %function
objc_fallback_asm:
    hint #34
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    bl objc_fallback_c
    str x0, [sp, #16]
    bl objc_ret_d_ptr
    ldp d0, d1, [x0]
    ldp d2, d3, [x0, #16]
    ldr x0, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
.size objc_fallback_asm, .-objc_fallback_asm
