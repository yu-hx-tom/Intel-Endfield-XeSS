.text
.p2align 4
.globl gft_test_invoke_sim_end
.def gft_test_invoke_sim_end; .scl 2; .type 32; .endef
.seh_proc gft_test_invoke_sim_end
gft_test_invoke_sim_end:
    pushq %rbx
    .seh_pushreg %rbx
    subq $32, %rsp
    .seh_stackalloc 32
    .seh_endprologue
    movq %rcx, %rbx
    call gft_sim_end_entry
.globl gft_test_sim_end_return
gft_test_sim_end_return:
    addq $32, %rsp
    popq %rbx
    ret
.seh_endproc

.p2align 4
.globl gft_test_sim_end_original
gft_test_sim_end_original:
    movq %rbx, gft_test_helper_rbx(%rip)
    movq (%rsp), %rax
    movq %rax, gft_test_helper_return(%rip)
    incq gft_test_helper_calls(%rip)
    movb $1, %al
    ret

.p2align 4
.globl gft_test_clobber
gft_test_clobber:
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

.data
.p2align 3
.globl gft_test_helper_calls, gft_test_helper_rbx, gft_test_helper_return
gft_test_helper_calls:
    .quad 0
gft_test_helper_rbx:
    .quad 0
gft_test_helper_return:
    .quad 0

.text
.globl gft_test_invoke_enqueue
.def gft_test_invoke_enqueue; .scl 2; .type 32; .endef
.seh_proc gft_test_invoke_enqueue
gft_test_invoke_enqueue:
    subq $56, %rsp
    .seh_stackalloc 56
    .seh_endprologue
    movq 96(%rsp), %rax
    movq %rax, 32(%rsp)
    stc
    call gft_enqueue_entry
    addq $56, %rsp
    ret
.seh_endproc

.globl gft_test_enqueue_original
gft_test_enqueue_original:
    pushfq
    popq gft_test_enqueue_flags(%rip)
    movq %rcx, gft_test_enqueue_args(%rip)
    movq %rdx, gft_test_enqueue_args+8(%rip)
    movq %r8, gft_test_enqueue_args+16(%rip)
    movq %xmm3, gft_test_enqueue_args+24(%rip)
    movq 40(%rsp), %rax
    movq %rax, gft_test_enqueue_args+32(%rip)
    incq gft_test_enqueue_original_calls(%rip)
    movabsq $0xABCDEF1234567890, %rax
    ret

.data
.p2align 3
.globl gft_test_enqueue_flags, gft_test_enqueue_args, gft_test_enqueue_original_calls
gft_test_enqueue_original_calls:
    .quad 0
gft_test_enqueue_flags:
    .quad 0
gft_test_enqueue_args:
    .zero 40
