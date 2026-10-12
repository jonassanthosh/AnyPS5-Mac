    .text
    .p2align 2
    .globl _aps5_guest_callback_bridge
_aps5_guest_callback_bridge:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #224
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp q0, q1, [sp, #64]
    stp q2, q3, [sp, #96]
    stp q4, q5, [sp, #128]
    stp q6, q7, [sp, #160]
    mov x0, x16
    mov x1, sp
    add x2, sp, #64
    add x3, sp, #192
    bl _Aps5GuestCallbackFromNative
    cbnz x0, 1f
    ldr x0, [sp, #192]
    ldr q0, [sp, #208]
    add sp, sp, #224
    ldp x29, x30, [sp], #16
    ret
1:
    mov x16, x0
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp q0, q1, [sp, #64]
    ldp q2, q3, [sp, #96]
    ldp q4, q5, [sp, #128]
    ldp q6, q7, [sp, #160]
    add sp, sp, #224
    ldp x29, x30, [sp], #16
    br x16
