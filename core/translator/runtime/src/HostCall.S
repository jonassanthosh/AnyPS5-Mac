    .text
    .p2align 2
    .globl _aps5_host_call_sysv
_aps5_host_call_sysv:
    .cfi_startproc
    stp x29, x30, [sp, #-32]!
    .cfi_def_cfa_offset 32
    .cfi_offset w30, -24
    .cfi_offset w29, -32
    mov x29, sp
    .cfi_def_cfa w29, 32
    str x19, [sp, #16]
    .cfi_offset w19, -16
    sub sp, sp, #64
    mov x19, x0
    mov x9, x1
    ldr x0, [x19, #56]
    ldr x1, [x19, #48]
    ldr x2, [x19, #16]
    ldr x3, [x19, #8]
    ldr x4, [x19, #64]
    ldr x5, [x19, #72]
    ldr x10, [x19, #32]
    ldr x6, [x10, #8]
    ldr x7, [x10, #16]
    ldp x11, x12, [x10, #24]
    stp x11, x12, [sp, #0]
    ldp x11, x12, [x10, #40]
    stp x11, x12, [sp, #16]
    ldp x11, x12, [x10, #56]
    stp x11, x12, [sp, #32]
    ldp x11, x12, [x10, #72]
    stp x11, x12, [sp, #48]
    ldr q0, [x19, #128]
    ldr q1, [x19, #160]
    ldr q2, [x19, #192]
    ldr q3, [x19, #224]
    ldr q4, [x19, #256]
    ldr q5, [x19, #288]
    ldr q6, [x19, #320]
    ldr q7, [x19, #352]
    blr x9
    str x0, [x19, #0]
    str x1, [x19, #16]
    str q0, [x19, #128]
    str q1, [x19, #160]
    add sp, sp, #64
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
    .cfi_endproc
