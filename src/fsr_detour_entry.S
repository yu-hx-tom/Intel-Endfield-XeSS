// Win64 pre-observation entry. No target is selected or patched by this file.
// Preserve the unknown target's volatile argument/return-capable registers and
// RFLAGS, then tail-jump so the target sees its original return address and stack.
// The helper must preserve LastError and must not let an exception escape.
#ifdef FSR_DETOUR_SELF_TEST
.section .rdata,"dr"
.p2align 3
.globl fsr_observer_unwind_points
fsr_observer_unwind_points:
#endif
.text
.macro point phase
#ifdef FSR_DETOUR_SELF_TEST
.Lpoint\@:
    .pushsection .rdata,"dr"
    .quad .Lpoint\@
    .long \phase
    .long 0
    .popsection
#endif
.endm
.p2align 4
.globl fsr_observer_entry
.def fsr_observer_entry; .scl 2; .type 32; .endef
.seh_proc fsr_observer_entry
fsr_observer_entry:
    point 0
    pushq %rbp
    .seh_pushreg %rbp
    point 1
    pushfq                         // Save flags BEFORE sub changes them.
    .seh_stackalloc 8
    point 2
    subq $200, %rsp
    .seh_stackalloc 200
    point 3
    movq %rsp, %rbp
    .seh_setframe %rbp, 0
    .seh_endprologue
    // RBP = original RSP - 216; caller return address at RBP + 216.
    // [0,32) shadow space, [32,88) GPRs, [96,192) XMM0..5,
    // [200,208) flags, [208,216) saved RBP. RSP is 16-byte aligned.
    point 4
    movq %rax, 32(%rbp)
    point 4
    movq %rcx, 40(%rbp)
    point 4
    movq %rdx, 48(%rbp)
    point 4
    movq %r8, 56(%rbp)
    point 4
    movq %r9, 64(%rbp)
    point 4
    movq %r10, 72(%rbp)
    point 4
    movq %r11, 80(%rbp)
    point 4
    movdqu %xmm0, 96(%rbp)
    point 4
    movdqu %xmm1, 112(%rbp)
    point 4
    movdqu %xmm2, 128(%rbp)
    point 4
    movdqu %xmm3, 144(%rbp)
    point 4
    movdqu %xmm4, 160(%rbp)
    point 4
    movdqu %xmm5, 176(%rbp)
    point 4
    call fsr_observe_before
    // Temporary body push is unwindable through the fixed RBP frame. Do not
    // restore flags inside the epilogue: POPFQ is not a legal Win64 epilogue op.
    point 4
    pushq 200(%rbp)
    point 5
    popfq
    point 4
    movq 32(%rbp), %rax
    point 4
    movq 40(%rbp), %rcx
    point 4
    movq 48(%rbp), %rdx
    point 4
    movq 56(%rbp), %r8
    point 4
    movq 64(%rbp), %r9
    point 4
    movq 72(%rbp), %r10
    point 4
    movq 80(%rbp), %r11
    point 4
    movdqu 96(%rbp), %xmm0
    point 4
    movdqu 112(%rbp), %xmm1
    point 4
    movdqu 128(%rbp), %xmm2
    point 4
    movdqu 144(%rbp), %xmm3
    point 4
    movdqu 160(%rbp), %xmm4
    point 4
    movdqu 176(%rbp), %xmm5
    // Legal epilogue: LEA + POP nonvolatile + indirect JMP (ModRM.mod=00).
    // LEA/POP/MOV/JMP leave the restored arithmetic flags intact.
    point 4
    leaq 208(%rbp), %rsp
    point 6
    popq %rbp
    point 7
    jmp *fsr_original_trampoline(%rip)
.seh_endproc
.globl fsr_observer_entry_end
fsr_observer_entry_end:

#ifdef FSR_DETOUR_SELF_TEST
.section .rdata,"dr"
.globl fsr_observer_unwind_points_end
fsr_observer_unwind_points_end:

.text
// A real fixed E9 rel32 thunk, matching the instruction class under evaluation.
// No game bytes, addresses, or function bodies are copied into this fixture.
.p2align 4
.globl fsr_test_thunk
.def fsr_test_thunk; .scl 2; .type 32; .endef
fsr_test_thunk:
    .byte 0xe9
    .long fsr_test_original - . - 4
    .fill 11, 1, 0x90
.globl fsr_test_float_thunk
.set fsr_test_float_thunk, fsr_test_thunk

.p2align 4
.globl fsr_test_original
fsr_test_original:
    // Store incoming flags in this caller-provided home slot, without touching
    // argument registers. Only single-thread raw mode writes the snapshot.
    pushfq
    popq 8(%rsp)
    cmpl $0, fsr_test_mode(%rip)
    je .Lcapture
    cmpl $1, fsr_test_mode(%rip)
    je .Lint
    jmp fsr_test_float_original
.Lint:
    jmp fsr_test_int_original
.Lcapture:
    movq %rax, fsr_test_seen+0(%rip)
    movq %rcx, fsr_test_seen+8(%rip)
    movq %rdx, fsr_test_seen+16(%rip)
    movq %r8, fsr_test_seen+24(%rip)
    movq %r9, fsr_test_seen+32(%rip)
    movq %r10, fsr_test_seen+40(%rip)
    movq %r11, fsr_test_seen+48(%rip)
    movdqu %xmm0, fsr_test_seen+56(%rip)
    movdqu %xmm1, fsr_test_seen+72(%rip)
    movdqu %xmm2, fsr_test_seen+88(%rip)
    movdqu %xmm3, fsr_test_seen+104(%rip)
    movdqu %xmm4, fsr_test_seen+120(%rip)
    movdqu %xmm5, fsr_test_seen+136(%rip)
    movq 8(%rsp), %rax
    movq %rax, fsr_test_seen+152(%rip)
    movq %rsp, fsr_test_seen+160(%rip)
    movq 40(%rsp), %rax
    movq %rax, fsr_test_seen+168(%rip)
    movq 48(%rsp), %rax
    movq %rax, fsr_test_seen+176(%rip)
    movq (%rsp), %rax
    movq %rax, fsr_test_seen+184(%rip)
    movabsq $0xabcdef0123456789, %rax
    ret

.p2align 4
.globl fsr_test_invoke_registers
.def fsr_test_invoke_registers; .scl 2; .type 32; .endef
.seh_proc fsr_test_invoke_registers
fsr_test_invoke_registers:
    subq $56, %rsp
    .seh_stackalloc 56
    .seh_endprologue
    movabsq $0x5555666677778888, %rax
    movq %rax, 32(%rsp)
    movabsq $0x9999aaaabbbbcccc, %rax
    movq %rax, 40(%rsp)
    leaq -8(%rsp), %rax
    movq %rax, fsr_test_expected_rsp(%rip)
    movdqu fsr_test_xmm+0(%rip), %xmm0
    movdqu fsr_test_xmm+16(%rip), %xmm1
    movdqu fsr_test_xmm+32(%rip), %xmm2
    movdqu fsr_test_xmm+48(%rip), %xmm3
    movdqu fsr_test_xmm+64(%rip), %xmm4
    movdqu fsr_test_xmm+80(%rip), %xmm5
    movabsq $0x0123456789abcdef, %rax
    movabsq $0x1023456789abcdef, %rcx
    movabsq $0x2023456789abcdef, %rdx
    movabsq $0x3023456789abcdef, %r8
    movabsq $0x4023456789abcdef, %r9
    movabsq $0x5023456789abcdef, %r10
    movabsq $0x6023456789abcdef, %r11
    pushq $0x247                 // CF/PF/ZF set; DF clear, Win64 ABI conforming.
    popfq
    call fsr_test_thunk
.globl fsr_test_raw_return
fsr_test_raw_return:
    addq $56, %rsp
    ret
.seh_endproc

.p2align 4
.globl fsr_test_clobber
.def fsr_test_clobber; .scl 2; .type 32; .endef
fsr_test_clobber:
    xorq %rax, %rax
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %r8, %r8
    xorq %r9, %r9
    xorq %r10, %r10
    xorq %r11, %r11
    pxor %xmm0, %xmm0
    pxor %xmm1, %xmm1
    pxor %xmm2, %xmm2
    pxor %xmm3, %xmm3
    pxor %xmm4, %xmm4
    pxor %xmm5, %xmm5
    ret
#endif

.data
.p2align 3
.globl fsr_original_trampoline
fsr_original_trampoline:
    .quad 0
