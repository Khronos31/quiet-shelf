/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Widen read/write handler candidate for the CECH-ZD1J main CPU.
 *
 * This is original FR30 assembly written for torne-userland. The control flow
 * follows the adopted contract v0.2.2 (§6.1-6.3) and the fixed reference
 * repos/coax commit b118c14f85e09ed0f98340a60dec0ae70aed3ea5,
 * internal/coaxd/usb/bridge/mb86h47/addressing.go:44-159. It does not reuse
 * that firmware or its generated image.
 *
 * Entry ABI (a candidate taken from the fixed reference, not independently
 * proven on this ROM): the ROM handler passes the 24-bit address in R4 and the
 * request state in R8 (read) or R10 (write). State +0x40 is wValue's low byte
 * and state +0x44 is the address the transfer uses. Only R0, R1, R4 and R12
 * may be changed. The handler returns to the fixed ROM resumes.
 *
 * Not implemented here: EXEC_READ (wValue low byte 0xF0 and above). The host
 * does not send those values in the initial capability.
 */
    .section .text

    .global read_handler
    .global write_handler

    .balign 4
read_handler:
    mov r8, r1                  /* r1 = request state */
    ldi:8 #0x40, r0
    add r0, r1                  /* r1 = &state[0x40] */
    ldub @r1, r0                /* r0 = wValue low byte */
    cmp #0, r0
    beq 1f
    lsl #8, r0                  /* high byte to bits 24-31 */
    lsl #8, r0
    lsl #8, r0
    or r0, r4                   /* address = R4 | (high << 24) */
    add #4, r1                  /* r1 = &state[0x44] */
    st r4, @r1
    bra 9f
1:
    ldi:20 #0x80000, r0
    cmp r0, r4
    blt 9f
    ldi:20 #0xA0000, r0
    cmp r0, r4
    bge 9f
    ldi:20 #0x3FFFF, r0
    and r0, r4                  /* keep the 18-bit window */
    ldi:20 #0xF0000, r0
    lsl #4, r0                  /* 0xF00000 */
    or r0, r4
    add #4, r1
    st r4, @r1
9:
    ldi:20 #0x0E9AE6, r12
    jmp @r12

    .balign 4
write_handler:
    mov r10, r1                 /* r1 = request state */
    ldi:8 #0x40, r0
    add r0, r1                  /* r1 = &state[0x40] */
    ldub @r1, r0                /* r0 = wValue low byte */
    cmp #0, r0
    beq 1f
    lsl #8, r0                  /* high byte to bits 24-31 */
    lsl #8, r0
    lsl #8, r0
    or r0, r4                   /* address = R4 | (high << 24) */
    ldi:8 #0, r0
    stb r0, @r1                 /* write path consumes the high byte */
    add #4, r1                  /* r1 = &state[0x44] */
    st r4, @r1
    bra 9f
1:
    ldi:20 #0x80000, r0
    cmp r0, r4
    blt 9f
    ldi:20 #0xA0000, r0
    cmp r0, r4
    bge 9f
    ldi:20 #0x3FFFF, r0
    and r0, r4                  /* keep the 18-bit window */
    ldi:20 #0xF0000, r0
    lsl #4, r0                  /* 0xF00000 */
    or r0, r4
    add #4, r1
    st r4, @r1
9:
    ldi:20 #0x0E9E2E, r12
    jmp @r12
