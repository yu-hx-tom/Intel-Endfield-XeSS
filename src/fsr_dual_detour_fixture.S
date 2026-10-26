// Self-owned two-target fixture. No game addresses or instructions are used.
.text
.p2align 4
.globl dual_target_a
.def dual_target_a; .scl 2; .type 32; .endef
dual_target_a:
    .byte 0xe9
    .long dual_original_a - . - 4
    .fill 11, 1, 0xcc
.p2align 4
.globl dual_target_b
.def dual_target_b; .scl 2; .type 32; .endef
dual_target_b:
    .byte 0xe9
    .long dual_original_b - . - 4
    .fill 11, 1, 0xcc
