.macro SNAP symbol
    .set pos, 0
    .irp reg,rax,rbx,rcx,rdx,rbp,rsi,rdi,r8,r9,r10,r11,r12,r13,r14,r15,rsp
        movq %\reg, \symbol+pos(%rip)
        .set pos, pos+8
    .endr
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        movdqu %xmm\n, \symbol+128+16*\n(%rip)
    .endr
    pushfq
    popq \symbol+384(%rip)
.endm
.macro FIXTURE n
.text
.p2align 4
.globl fst_invoke\n, fst_cmp\n, fst_resume\n
.def fst_invoke\n; .scl 2; .type 32; .endef
.seh_proc fst_invoke\n
fst_invoke\n:
    movq %rbx, 24(%rsp)
    pushq %rbp
    .seh_pushreg %rbp
    pushq %rsi
    .seh_pushreg %rsi
    pushq %rdi
    .seh_pushreg %rdi
    pushq %r12
    .seh_pushreg %r12
    pushq %r13
    .seh_pushreg %r13
    pushq %r14
    .seh_pushreg %r14
    pushq %r15
    .seh_pushreg %r15
    movl $0xb350, %eax
    call ___chkstk_ms
    subq %rax, %rsp
    .seh_stackalloc 0xb350
    .seh_savereg %rbx, 0xb3a0
    movdqu %xmm6, 0xb340(%rsp)
    .seh_savexmm %xmm6, 0xb340
    movdqu %xmm7, 0xb330(%rsp)
    .seh_savexmm %xmm7, 0xb330
    movdqu %xmm8, 0xb320(%rsp)
    .seh_savexmm %xmm8, 0xb320
    movdqu %xmm9, 0xb310(%rsp)
    .seh_savexmm %xmm9, 0xb310
    movdqu %xmm10, 0xb300(%rsp)
    .seh_savexmm %xmm10, 0xb300
    movdqu %xmm11, 0xb2f0(%rsp)
    .seh_savexmm %xmm11, 0xb2f0
    movdqu %xmm12, 0xb2e0(%rsp)
    .seh_savexmm %xmm12, 0xb2e0
    .seh_endprologue
    movq %rcx, %r13
    .irp reg,rax,rbx,rcx,rdx,rbp,rsi,rdi,r8,r9,r10,r11,r12,r14,r15
        movabsq $0x12345678abcdef01, %\reg
    .endr
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12
        movdqu fst_pattern(%rip), %xmm\n
    .endr
    SNAP fst_before
fst_cmp\n:
    .byte 0x41,0x80,0x7d,0x3b,0x00
fst_resume\n:
.if \n == 0
    .byte 0x75,0x14
    movl $0, fst_taken(%rip)
    jmp 2f
    .space 8, 0x90
.else
    .byte 0x0f,0x85,0x4b,0x01,0x00,0x00
    movl $0, fst_taken(%rip)
    jmp 2f
    .space 316, 0x90
.endif
1:  movl $1, fst_taken(%rip)
2:  SNAP fst_after
    movdqu 0xb340(%rsp), %xmm6
    movdqu 0xb330(%rsp), %xmm7
    movdqu 0xb320(%rsp), %xmm8
    movdqu 0xb310(%rsp), %xmm9
    movdqu 0xb300(%rsp), %xmm10
    movdqu 0xb2f0(%rsp), %xmm11
    movdqu 0xb2e0(%rsp), %xmm12
    movq 0xb3a0(%rsp), %rbx
    addq $0xb350, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rdi
    popq %rsi
    popq %rbp
    ret
.seh_endproc
.endm
FIXTURE 0
FIXTURE 1
.section .rdata,"dr"
fst_pattern: .quad 0x1020304050607080,0x90a0b0c0d0e0f001
