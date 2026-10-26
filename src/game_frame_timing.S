// Pinned outer callbacks consume no stack arguments. Use a real CALL with valid
// Win64 unwind metadata; do not rewrite a caller's return slot. Observe normal
// returns only, and retain the opaque RAX/XMM0 return rather than inventing void.
.text
.macro save_volatile
    movq %rax, 32(%rbp)
    movq %rcx, 40(%rbp)
    movq %rdx, 48(%rbp)
    movq %r8, 56(%rbp)
    movq %r9, 64(%rbp)
    movq %r10, 72(%rbp)
    movq %r11, 80(%rbp)
    movdqu %xmm0, 96(%rbp)
    movdqu %xmm1, 112(%rbp)
    movdqu %xmm2, 128(%rbp)
    movdqu %xmm3, 144(%rbp)
    movdqu %xmm4, 160(%rbp)
    movdqu %xmm5, 176(%rbp)
.endm
.macro restore_volatile
    movq 32(%rbp), %rax
    movq 40(%rbp), %rcx
    movq 48(%rbp), %rdx
    movq 56(%rbp), %r8
    movq 64(%rbp), %r9
    movq 72(%rbp), %r10
    movq 80(%rbp), %r11
    movdqu 96(%rbp), %xmm0
    movdqu 112(%rbp), %xmm1
    movdqu 128(%rbp), %xmm2
    movdqu 144(%rbp), %xmm3
    movdqu 160(%rbp), %xmm4
    movdqu 176(%rbp), %xmm5
.endm
.macro post_entry name, index
.p2align 4
.globl gft_\name\()_entry
.def gft_\name\()_entry; .scl 2; .type 32; .endef
.seh_proc gft_\name\()_entry
gft_\name\()_entry:
    pushq %rbp
    .seh_pushreg %rbp
    pushfq
    .seh_stackalloc 8
    subq $232, %rsp
    .seh_stackalloc 232
    movq %rsp, %rbp
    .seh_setframe %rbp, 0
    .seh_endprologue
    // 0..31 shadow, 32..87 GPRs, 96..191 XMM, 192 post flags,
    // 200..223 PostSnapshot, 232 incoming flags, 240 saved RBP.
    save_volatile
    movq 40(%rbp), %rdx
    movl $\index, %ecx
    leaq 200(%rbp), %r8
    call gft_before
    pushq 232(%rbp)
    popfq
    restore_volatile
    call *gft_\name\()_original(%rip)
    pushfq
    popq 192(%rbp)
    save_volatile
    movl $\index, %ecx
    leaq 200(%rbp), %rdx
    call gft_after
    pushq 192(%rbp)
    popfq
    restore_volatile
    leaq 240(%rbp), %rsp
    popq %rbp
    ret
.seh_endproc
.endm

post_entry sim_start, 0
post_entry render_start, 2
post_entry render_end, 3
post_entry present_start, 4
post_entry present_end, 5

// Actual five-argument enqueue ABI. Observe before the original can signal its
// worker. A tail jump keeps caller shadow/stack arg five and native unwind.
.p2align 4
.globl gft_enqueue_entry
.def gft_enqueue_entry; .scl 2; .type 32; .endef
.seh_proc gft_enqueue_entry
gft_enqueue_entry:
    pushq %rbp
    .seh_pushreg %rbp
    pushfq
    .seh_stackalloc 8
    subq $200, %rsp
    .seh_stackalloc 200
    movq %rsp, %rbp
    .seh_setframe %rbp, 0
    .seh_endprologue
    save_volatile
    call gft_enqueue_before
    pushq 200(%rbp)
    popfq
    restore_volatile
    leaq 208(%rbp), %rsp
    popq %rbp
    jmp *gft_enqueue_original(%rip)
.seh_endproc

// SIM_END instead observes the common helper entry after the original marker's
// gate converges. A tail jump retains this helper's real return address/stack,
// AL result, and all argument registers even for unrelated callers.
.p2align 4
.globl gft_sim_end_entry
.def gft_sim_end_entry; .scl 2; .type 32; .endef
.seh_proc gft_sim_end_entry
gft_sim_end_entry:
    pushq %rbp
    .seh_pushreg %rbp
    pushfq
    .seh_stackalloc 8
    subq $200, %rsp
    .seh_stackalloc 200
    movq %rsp, %rbp
    .seh_setframe %rbp, 0
    .seh_endprologue
    save_volatile
    movq 216(%rbp), %rcx
    movq %rbx, %rdx
    call gft_sim_end_before
    pushq 200(%rbp)
    popfq
    restore_volatile
    leaq 208(%rbp), %rsp
    popq %rbp
    jmp *gft_sim_end_original(%rip)
.seh_endproc

.data
.p2align 3
.macro trampoline name
.globl gft_\name\()_original
gft_\name\()_original:
    .quad 0
.endm
trampoline sim_start
trampoline sim_end
trampoline render_start
trampoline render_end
trampoline present_start
trampoline present_end
trampoline enqueue
