// Ordinary Win64 graph/pass/source -> RAX only. New call continuation is real.
// This file never writes/replaces the caller's return address.
#ifdef GRAPH_REGISTRATION_BRIDGE_SELF_TEST
.section .rdata,"dr"
.p2align 3
.globl grb_unwind_points
grb_unwind_points:
#endif
.text
.macro point phase
#ifdef GRAPH_REGISTRATION_BRIDGE_SELF_TEST
.Lpoint\@:
    .pushsection .rdata,"dr"
    .quad .Lpoint\@
    .long \phase
    .long 0
    .popsection
#endif
.endm
.macro entry name, route
.p2align 4
.globl \name
.def \name; .scl 2; .type 32; .endef
.seh_proc \name
\name:
    point 0
    pushq %rbp
    .seh_pushreg %rbp
    point 1
    subq $96, %rsp
    .seh_stackalloc 96
    point 2
    movq %rsp, %rbp
    .seh_setframe %rbp, 0
    .seh_endprologue
    // [0,32) home area; [32,88) owned EntrySnapshot; saved RBP at 96.
    // Entry RSP = RBP+104; body RSP is 16-byte aligned before CALL.
    point 3
    leaq 104(%rbp), %rax
    point 3
    movq %rax, 32(%rbp)
    point 3
    movq 96(%rbp), %rax
    point 3
    movq %rax, 40(%rbp)
    point 3
    movq 104(%rbp), %rax
    point 3
    movq %rax, 48(%rbp)
    point 3
    movq %rcx, 56(%rbp)
    point 3
    movq %rdx, 64(%rbp)
    point 3
    movq %r8, 72(%rbp)
    point 3
    movq $0, 80(%rbp)
    point 3
    leaq 32(%rbp), %rcx
    point 3
    leaq \route(%rip), %rdx
    point 3
    call grb_dispatch
    point 3
    leaq 96(%rbp), %rsp
    point 4
    popq %rbp
    point 5
    ret
.seh_endproc
.endm
#ifdef GRAPH_REGISTRATION_SR_ONLY
entry grb_sr_accumulate_entry, grb_sr_accumulate_route
entry grb_sr_rcas_entry, grb_sr_rcas_route
#define SR_NATIVE_PASS(name, pass) entry grb_sr_ ## name ## _entry, grb_sr_ ## name ## _route
#include "sr_native_passes.inc"
#undef SR_NATIVE_PASS
#else
entry grb_fi_prepare_entry, grb_fi_prepare_route
entry grb_fi_copy_entry, grb_fi_copy_route
entry grb_upscale_prepare_entry, grb_upscale_prepare_route
#endif
#ifdef GRAPH_REGISTRATION_BRIDGE_SELF_TEST
.section .rdata,"dr"
.globl grb_unwind_points_end
grb_unwind_points_end:
#endif
