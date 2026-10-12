    .text
    .p2align 2
    .globl _aps5_host_call_sysv
_aps5_host_call_sysv:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x19, [sp, #16]
    mov x19, x0
    mov x9, x1
    ldr x0, [x19, #56]
    ldr x1, [x19, #48]
    ldr x2, [x19, #16]
    ldr x3, [x19, #8]
    ldr x4, [x19, #64]
    ldr x5, [x19, #72]
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
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
