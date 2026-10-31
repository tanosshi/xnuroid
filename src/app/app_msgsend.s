// Objective-C message dispatch for app mode (src/app/app_runtime.cpp).
//   app_objc_msgSend:        x0 = receiver, x1 = selector; all argument registers (x0-x8, q0-q7) reach the IMP intact.
//   app_objc_msgSendSuper2:  x0 = &objc_super{receiver, current_class}; the search starts above current_class.
//   app_objc_msgSendSuper:   x0 = &objc_super{receiver, class_to_search}.
//   app_nil_imp:             what a message to nil (or an unimplemented selector) returns: zero in x0/x1 and d0-d3.
//   app_forward_thunk:       re-sends the message to the object app_take_forward_target() hands back.
    .text

.macro SAVE_ARGS
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
.endm

.macro RESTORE_ARGS
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
.endm

    .globl app_objc_msgSend
    .type app_objc_msgSend, %function
    .p2align 4
app_objc_msgSend:
    cbz x0, 1f
    SAVE_ARGS
    bl app_lookup_c
    mov x16, x0
    RESTORE_ARGS
    br x16
1:  mov x1, #0
    movi d0, #0
    movi d1, #0
    movi d2, #0
    movi d3, #0
    ret
    .size app_objc_msgSend, .-app_objc_msgSend

    .globl app_objc_msgSendSuper2
    .type app_objc_msgSendSuper2, %function
    .p2align 4
app_objc_msgSendSuper2:
    SAVE_ARGS
    bl app_lookup_super2_c
    mov x16, x0
    RESTORE_ARGS
    ldr x0, [x0]
    br x16
    .size app_objc_msgSendSuper2, .-app_objc_msgSendSuper2

    .globl app_objc_msgSendSuper
    .type app_objc_msgSendSuper, %function
    .p2align 4
app_objc_msgSendSuper:
    SAVE_ARGS
    bl app_lookup_super_c
    mov x16, x0
    RESTORE_ARGS
    ldr x0, [x0]
    br x16
    .size app_objc_msgSendSuper, .-app_objc_msgSendSuper

    .globl app_nil_imp
    .type app_nil_imp, %function
    .p2align 4
app_nil_imp:
    mov x0, #0
    mov x1, #0
    movi d0, #0
    movi d1, #0
    movi d2, #0
    movi d3, #0
    ret
    .size app_nil_imp, .-app_nil_imp

    .globl app_forward_thunk
    .type app_forward_thunk, %function
    .p2align 4
app_forward_thunk:
    SAVE_ARGS
    bl app_take_forward_target
    str x0, [sp, #16]
    RESTORE_ARGS
    b app_objc_msgSend
    .size app_forward_thunk, .-app_forward_thunk

// Darwin thread-local variable thunk (bound to _tlv_bootstrap): compiled code calls it through the variable's
// TLVDescriptor with x0 = &descriptor and expects the address for this thread back in x0, every other register
// intact. app_tlv_resolve (app_darwin.cpp) does the work; everything it may clobber is saved around it.
    .globl app_tlv_get_addr
    .type app_tlv_get_addr, %function
    .p2align 4
app_tlv_get_addr:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #528
    stp x1, x2, [sp, #0]
    stp x3, x4, [sp, #16]
    stp x5, x6, [sp, #32]
    stp x7, x8, [sp, #48]
    stp x9, x10, [sp, #64]
    stp x11, x12, [sp, #80]
    stp x13, x14, [sp, #96]
    stp x15, x16, [sp, #112]
    stp x17, x18, [sp, #128]
    stp q0, q1, [sp, #144]
    stp q2, q3, [sp, #176]
    stp q4, q5, [sp, #208]
    stp q6, q7, [sp, #240]
    stp q16, q17, [sp, #272]
    stp q18, q19, [sp, #304]
    stp q20, q21, [sp, #336]
    stp q22, q23, [sp, #368]
    stp q24, q25, [sp, #400]
    stp q26, q27, [sp, #432]
    stp q28, q29, [sp, #464]
    stp q30, q31, [sp, #496]
    bl app_tlv_resolve
    ldp q30, q31, [sp, #496]
    ldp q28, q29, [sp, #464]
    ldp q26, q27, [sp, #432]
    ldp q24, q25, [sp, #400]
    ldp q22, q23, [sp, #368]
    ldp q20, q21, [sp, #336]
    ldp q18, q19, [sp, #304]
    ldp q16, q17, [sp, #272]
    ldp q6, q7, [sp, #240]
    ldp q4, q5, [sp, #208]
    ldp q2, q3, [sp, #176]
    ldp q0, q1, [sp, #144]
    ldp x17, x18, [sp, #128]
    ldp x15, x16, [sp, #112]
    ldp x13, x14, [sp, #96]
    ldp x11, x12, [sp, #80]
    ldp x9, x10, [sp, #64]
    ldp x7, x8, [sp, #48]
    ldp x5, x6, [sp, #32]
    ldp x3, x4, [sp, #16]
    ldp x1, x2, [sp, #0]
    add sp, sp, #528
    ldp x29, x30, [sp], #16
    ret
    .size app_tlv_get_addr, .-app_tlv_get_addr

// ___chkstk_darwin: the stack probe called (x15 = size / 16) before frames over a page. Android stacks are mapped up
// front, so there is nothing to touch; it only has to leave every register alone.
    .globl app_chkstk_darwin
    .type app_chkstk_darwin, %function
    .p2align 2
app_chkstk_darwin:
    ret
    .size app_chkstk_darwin, .-app_chkstk_darwin
