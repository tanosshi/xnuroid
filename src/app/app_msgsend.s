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
