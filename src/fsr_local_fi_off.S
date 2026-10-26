.section .rdata,"dr"
.p2align 4
.globl fsr_local_fi_off_template, fsr_local_fi_off_template_end
.globl fsr_local_fi_off_phase_disp, fsr_local_fi_off_one_disp
.globl fsr_local_fi_off_active_disp0, fsr_local_fi_off_active_disp1
.globl fsr_local_fi_off_inactive_disp0, fsr_local_fi_off_inactive_disp1
fsr_local_fi_off_template:
    .byte 0x83, 0x3d
fsr_local_fi_off_phase_disp: .long 0
    .byte 1                         // cmp dword [phase],1
    jne 1f
    .byte 0x80, 0x3d
fsr_local_fi_off_one_disp: .long 0
    .byte 0                         // cmp byte [one],0
    .byte 0x0f, 0x84
fsr_local_fi_off_active_disp0: .long 0
    .byte 0x0f, 0x85
fsr_local_fi_off_active_disp1: .long 0
1:
    .byte 0x0f, 0x84
fsr_local_fi_off_inactive_disp0: .long 0
    .byte 0x0f, 0x85
fsr_local_fi_off_inactive_disp1: .long 0
fsr_local_fi_off_template_end:
    .byte 1                         // immutable comparison operand

// Every instruction runs with the original Unity BODY stack. Complementary
// Jcc pairs preserve flags and are not Win64 epilogue tail-JMP encodings.
.p2align 2
.globl fsr_local_fi_off_unwind
fsr_local_fi_off_unwind:
    .byte 1, 0, 25, 0
    .byte 0, 0xc8; .short 0x0b2e
    .byte 0, 0xb8; .short 0x0b2f
    .byte 0, 0xa8; .short 0x0b30
    .byte 0, 0x98; .short 0x0b31
    .byte 0, 0x88; .short 0x0b32
    .byte 0, 0x68; .short 0x0b34
    .byte 0, 0x78; .short 0x0b33
    .byte 0, 0x34; .short 0x1674
    .byte 0, 0x01; .short 0x166a
    .byte 0, 0xf0, 0, 0xe0, 0, 0xd0, 0, 0xc0
    .byte 0, 0x70, 0, 0x60, 0, 0x50
    .short 0
