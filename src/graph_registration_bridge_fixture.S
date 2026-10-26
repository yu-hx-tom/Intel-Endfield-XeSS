// Self-owned CPU fixture. No hook, detour, game code, or executable-page write.
.text
.p2align 4
.globl grb_fixture_invoke
.def grb_fixture_invoke; .scl 2; .type 32; .endef
.seh_proc grb_fixture_invoke
grb_fixture_invoke:
    pushq %rbp
    .seh_pushreg %rbp
    pushq %rbx
    .seh_pushreg %rbx
    subq $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
    movq %rcx, %rbx
    movq %rdx, %rcx
    movq %r8, %rdx
    movq %r9, %r8
    leaq -8(%rsp), %rax
    movq %rax, grb_fixture_expected_rsp(%rip)
    movabsq $0x123456789abcdef0, %rbp
    call *%rbx
.globl grb_fixture_return
grb_fixture_return:
    movq %rbp, grb_fixture_returned_rbp(%rip)
    addq $40, %rsp
    popq %rbx
    popq %rbp
    ret
.seh_endproc

.p2align 4
.globl grb_fixture_original
.def grb_fixture_original; .scl 2; .type 32; .endef
grb_fixture_original:
    cmpl $0, grb_fixture_enabled(%rip)
    je .Lforward
    movq %rsp, grb_fixture_seen_rsp(%rip)
    movq (%rsp), %rax
    movq %rax, grb_fixture_seen_return(%rip)
    movq %rcx, grb_fixture_seen_rcx(%rip)
    movq %rdx, grb_fixture_seen_rdx(%rip)
    movq %r8, grb_fixture_seen_r8(%rip)
.Lforward:
    jmp grb_test_original0
