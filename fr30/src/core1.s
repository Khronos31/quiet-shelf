/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Core 1 I2C candidate for CECH-ZD1J.
 *
 * Original FR30 assembly for torne-userland. The shared layout, ring, tag,
 * HALTED/resume rules and result codes follow contract v0.2.2 sections 2, 3,
 * 5, 7 and 9. The byte-level I2C control values and the poll budget are the
 * candidate hardware knowledge from the fixed coax commit
 * b118c14f85e09ed0f98340a60dec0ae70aed3ea5
 * (internal/coaxd/usb/bridge/mb86h47/core1prog.go:28-113 and regs/usb.go).
 * This is not a device-verified program.
 *
 * The host performs S4 (I2C master initialisation) before RUN. The host also
 * zeroes A3800-A3FFF, writes CTRL.session and loads this image at A0000.
 */
    .section .text

    /* FR30 conditional branches are 8-bit; use an absolute jump when a
       helper/error label is farther away. R1 is scratch at every such site. */
    .macro JMPABS label
      ldi:20 #\label, r1
      jmp @r1
    .endm
    .macro BEQ_LONG label
      bne 1f
      JMPABS \label
1:
    .endm
    .macro BNE_LONG label
      beq 1f
      JMPABS \label
1:
    .endm
    .macro BHI_LONG label
      bls 1f
      JMPABS \label
1:
    .endm
    .macro BLT_LONG label
      bge 1f
      JMPABS \label
1:
    .endm

    .equ INFO_BASE,  0x000A3800
    .equ CTRL_BASE,  0x000A3840
    .equ STAT_BASE,  0x000A3880
    .equ REQ_BASE,   0x000A3900
    .equ DONE_BASE,  0x000A3B00
    .equ SCR_BASE,   0x000A3000

    .equ SCR_TAG,    0x000A3000
    .equ SCR_CONS,   0x000A3004
    .equ SCR_ACKED,  0x000A3008
    .equ SCR_RLEN,   0x000A300C
    .equ SCR_W,      0x000A3010
    .equ SCR_R,      0x000A3014
    .equ SCR_ADDR,   0x000A3018
    .equ SCR_OP,     0x000A301C
    .equ SCR_CODE,   0x000A3020
    .equ SCR_STAGE,  0x000A3024
    .equ SCR_IDLE,   0x000A3028
    .equ SCR_RESUME, 0x000A302C
    .equ SCR_END,    0x000A3030

    .equ I2C_STATUS, 0x0FD8FE0
    .equ I2C_CTRL,   0x0FD8FE4
    .equ I2C_DATA,   0x0FD8FF0

    .global _start
    .balign 4
_start:
    /* Initial stack is the contract value, below the shared data. */
    ldi:20 #0x000A37FC, r15

    /* Candidate I2C master registers and constant masks. */
    ldi:32 #I2C_STATUS, r12
    ldi:32 #I2C_CTRL, r13
    ldi:32 #I2C_DATA, r14
    ldi:8 #0x80, r4
    ldi:8 #0x01, r3
    ldi:8 #0x10, r2

    /* CTRL.session must have a non-zero high half. */
    ldi:20 #CTRL_BASE, r1
    ld @r1, r0
    ldi:32 #0xFFFF0000, r1
    and r1, r0
    cmp #0, r0
    beq boot_fault
    mov r0, r8                  /* session high half */

    /* INFO: ABI, build_id, capability, session, image_bytes, data, param. */
    ldi:20 #INFO_BASE+0x04, r1
    ldi:32 #0x00010000, r0
    st r0, @r1
    ldi:20 #INFO_BASE+0x08, r1
    ldi:32 #__core1_build_id, r0
    st r0, @r1
    ldi:20 #INFO_BASE+0x0C, r1
    ldi:8 #0x01, r0
    st r0, @r1
    ldi:20 #CTRL_BASE, r1
    ld @r1, r0
    ldi:20 #INFO_BASE+0x10, r1
    st r0, @r1
    ldi:20 #INFO_BASE+0x14, r1
    ldi:32 #__core1_image_bytes, r0
    st r0, @r1
    ldi:20 #INFO_BASE+0x18, r1
    ldi:20 #0x000A3800, r0
    st r0, @r1
    ldi:20 #INFO_BASE+0x1C, r1
    ldi:32 #0x00040000, r0
    st r0, @r1

    /* STAT: BOOT, zero counters/progress, ack fields. */
    ldi:20 #STAT_BASE+0x00, r1
    ldi:8 #0x01, r0
    st r0, @r1
    ldi:20 #STAT_BASE+0x04, r1
    ldi:8 #0x00, r0
    st r0, @r1
    ldi:20 #STAT_BASE+0x08, r1
    ldi:8 #0x00, r0
    st r0, @r1
    ldi:20 #STAT_BASE+0x0C, r1
    ldi:8 #0x00, r0
    st r0, @r1
    ldi:20 #STAT_BASE+0x10, r1
    ldi:8 #0x00, r0
    st r0, @r1
    ldi:20 #STAT_BASE+0x14, r1
    ldi:8 #0x00, r0
    st r0, @r1
    ldi:20 #STAT_BASE+0x18, r1
    ldi:8 #0x00, r0
    st r0, @r1
    ldi:20 #STAT_BASE+0x1C, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x20, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x24, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x28, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x2C, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x30, r1
    st r0, @r1

    /* T_ret and progress start at the session's base wire value. */
    ldi:20 #CTRL_BASE+0x10, r1
    ld @r1, r0
    ldi:20 #STAT_BASE+0x18, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x1C, r1
    st r0, @r1

    /* READY, then publish INFO.magic last. */
    ldi:20 #STAT_BASE+0x00, r1
    ldi:8 #0x02, r0
    st r0, @r1
    ldi:20 #INFO_BASE+0x00, r1
    ldi:32 #0x54493243, r0
    st r0, @r1
    JMPABS main_loop

boot_fault:
    ldi:20 #STAT_BASE+0x00, r1
    ldi:8 #0xE0, r0
    st r0, @r1
    ldi:20 #STAT_BASE+0x08, r1
    ldi:8 #0x01, r0
    st r0, @r1
    JMPABS main_loop

/* ------------------------------------------------------------------ */
/* Main loop                                                          */
/* ------------------------------------------------------------------ */
main_loop:
    /* heartbeat += 1 */
    ldi:20 #STAT_BASE+0x04, r1
    ld @r1, r0
    add #1, r0
    st r0, @r1

    /* NOP command: process a changed cmd_seq. */
    ldi:20 #CTRL_BASE+0x04, r1
    ld @r1, r7
    ldi:20 #STAT_BASE+0x10, r1
    ld @r1, r5
    cmp r5, r7
    beq cmd_done
    ldi:20 #CTRL_BASE+0x08, r1
    ld @r1, r5
    ldi:8 #0x00, r0
    cmp #0, r5
    beq cmd_store
    ldi:8 #0x01, r0
cmd_store:
    ldi:20 #STAT_BASE+0x14, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x10, r1
    st r7, @r1
cmd_done:

    /* Resume handling: only a changed value is considered. The ack and the
       HALTED release are published only after CONTINUE/DISCARD processing, so
       a host cannot publish outside a fixed DISCARD range while it is running. */
    ldi:20 #CTRL_BASE+0x18, r1
    ld @r1, r7
    ldi:20 #STAT_BASE+0x30, r1
    ld @r1, r5
    cmp r5, r7
    BEQ_LONG resume_done
    ldi:20 #SCR_RESUME, r1
    st r7, @r1
    ldi:20 #STAT_BASE+0x00, r1
    ld @r1, r0
    cmp #4, r0
    BNE_LONG resume_ack_now
    ldi:20 #STAT_BASE+0x20, r1
    ld @r1, r5
    mov r7, r0
    ldi:32 #0xFFFF, r1
    and r1, r0
    cmp r0, r5
    BNE_LONG resume_ack_now
    mov r7, r0
    lsr #8, r0
    lsr #8, r0
    ldi:8 #0xFF, r1
    and r1, r0
    cmp #1, r0
    BEQ_LONG resume_continue
    cmp #2, r0
    BEQ_LONG resume_discard
    JMPABS resume_ack_now

resume_ack_now:
    ldi:20 #SCR_RESUME, r1
    ld @r1, r5
    ldi:20 #STAT_BASE+0x30, r1
    st r5, @r1
    JMPABS resume_done

resume_continue:
    ldi:20 #STAT_BASE+0x00, r1
    ldi:8 #0x02, r0
    st r0, @r1
    ldi:20 #STAT_BASE+0x18, r1
    ld @r1, r0
    ldi:20 #STAT_BASE+0x1C, r1
    st r0, @r1
    ldi:20 #SCR_RESUME, r1
    ld @r1, r5
    ldi:20 #STAT_BASE+0x30, r1
    st r5, @r1
    JMPABS resume_done

resume_discard:
    /* Freeze the producer that defines D at acceptance time. */
    ldi:20 #CTRL_BASE+0x10, r1
    ld @r1, r0
    ldi:20 #SCR_END, r1
    st r0, @r1
discard_loop:
    ldi:20 #SCR_END, r1
    ld @r1, r0
    ldi:20 #STAT_BASE+0x18, r1
    ld @r1, r5
    sub r5, r0
    ldi:32 #0xFFFF, r1
    and r1, r0
    cmp #0, r0
    BEQ_LONG discard_done
    cmp #8, r0
    BHI_LONG discard_bad
    mov r5, r0
    ldi:8 #0x07, r1
    and r1, r0
    lsl #6, r0
    ldi:20 #DONE_BASE, r1
    add r0, r1
    mov r1, r9
    mov r8, r0
    or r5, r0
    mov r0, r7
    ldi:8 #0x11, r0
    add #4, r9
    st r0, @r9
    add #4, r9
    ldi:8 #0x00, r0
    st r0, @r9
    add #4, r9
    st r0, @r9
    ldi:8 #0x30, r0
    add r0, r9
    st r7, @r9
    ldi:8 #0x3C, r0
    sub r0, r9
    st r7, @r9
    add r3, r5
    ldi:32 #0xFFFF, r1
    and r1, r5
    ldi:20 #STAT_BASE+0x18, r1
    st r5, @r1
    ldi:32 #0x80000000, r0
    or r5, r0
    ldi:20 #STAT_BASE+0x1C, r1
    st r0, @r1
    JMPABS discard_loop
discard_bad:
    ldi:20 #STAT_BASE+0x00, r1
    ldi:8 #0xE0, r0
    st r0, @r1
    ldi:20 #STAT_BASE+0x08, r1
    ldi:8 #0x01, r0
    st r0, @r1
    ldi:20 #SCR_RESUME, r1
    ld @r1, r5
    ldi:20 #STAT_BASE+0x30, r1
    st r5, @r1
    JMPABS resume_done
discard_done:
    /* Release only after every fixed-range SKIPPED completion is published. */
    ldi:20 #STAT_BASE+0x00, r1
    ldi:8 #0x02, r0
    st r0, @r1
    ldi:20 #STAT_BASE+0x18, r1
    ld @r1, r0
    ldi:20 #STAT_BASE+0x1C, r1
    st r0, @r1
    ldi:20 #SCR_RESUME, r1
    ld @r1, r5
    ldi:20 #STAT_BASE+0x30, r1
    st r5, @r1
resume_done:

    /* A HALTED or FAULT core does not take new requests. */
    ldi:20 #STAT_BASE+0x00, r1
    ld @r1, r0
    cmp #3, r0
    BHI_LONG main_loop

    /* pending = dist(prod, cons) */
    ldi:20 #CTRL_BASE+0x10, r1
    ld @r1, r0
    ldi:20 #STAT_BASE+0x18, r1
    ld @r1, r5
    sub r5, r0
    ldi:32 #0xFFFF, r1
    and r1, r0
    cmp #0, r0
    BEQ_LONG main_loop
    cmp #8, r0
    BHI_LONG bad_ctrl

    /* Save cons and tag, then locate the request/completion slots. */
    ldi:20 #SCR_CONS, r1
    st r5, @r1
    mov r8, r0
    or r5, r0
    ldi:20 #SCR_TAG, r1
    st r0, @r1
    mov r0, r7
    mov r5, r0
    ldi:8 #0x07, r1
    and r1, r0
    lsl #6, r0
    ldi:20 #REQ_BASE, r1
    add r0, r1
    mov r1, r10
    ldi:20 #DONE_BASE, r1
    add r0, r1
    mov r1, r9

    /* Clear per-request diagnostics before tag validation so TORN/INVALID
       still publish zero acked/rlen/polls/idle. */
    ldi:20 #SCR_ACKED, r1
    ldi:8 #0x00, r0
    st r0, @r1
    ldi:20 #SCR_RLEN, r1
    st r0, @r1
    ldi:20 #SCR_CODE, r1
    st r0, @r1
    ldi:20 #SCR_STAGE, r1
    st r0, @r1
    ldi:20 #SCR_IDLE, r1
    st r0, @r1
    ldi:8 #0x00, r11

    /* Validate both TWDN-style tags before any bus activity. */
    ld @r10, r0
    cmp r7, r0
    BNE_LONG torn
    mov r10, r1
    ldi:8 #0x3C, r0
    add r0, r1
    ld @r1, r0
    cmp r7, r0
    BNE_LONG torn

    /* op / address / lengths */
    mov r10, r1
    add #4, r1
    ld @r1, r7
    mov r7, r0
    ldi:8 #0xFF, r1
    and r1, r0
    ldi:20 #SCR_OP, r1
    st r0, @r1
    mov r7, r0
    lsr #8, r0
    lsr #8, r0
    ldi:8 #0xFF, r1
    and r1, r0
    mov r0, r5
    ldi:8 #0x01, r1
    and r1, r5
    BNE_LONG invalid
    ldi:20 #SCR_ADDR, r1
    st r0, @r1
    mov r7, r0
    lsr #8, r0
    ldi:8 #0xFF, r1
    and r1, r0
    cmp #0, r0
    BNE_LONG invalid
    mov r7, r0
    lsr #8, r0
    lsr #8, r0
    lsr #8, r0
    cmp #0, r0
    BNE_LONG invalid

    mov r10, r1
    add #8, r1
    ld @r1, r7
    mov r7, r0
    ldi:8 #0xFF, r1
    and r1, r0
    ldi:20 #SCR_W, r1
    st r0, @r1
    mov r7, r0
    lsr #8, r0
    ldi:8 #0xFF, r1
    and r1, r0
    ldi:20 #SCR_R, r1
    st r0, @r1
    mov r7, r0
    lsr #8, r0
    lsr #8, r0
    cmp #0, r0
    BNE_LONG invalid

    /* Valid opcode/length combinations. */
    ldi:20 #SCR_OP, r1
    ld @r1, r0
    cmp #1, r0
    beq valid_write
    cmp #2, r0
    beq valid_read
    cmp #3, r0
    beq valid_write_read
    JMPABS invalid
valid_write:
    ldi:20 #SCR_W, r1
    ld @r1, r5
    cmp #1, r5
    BLT_LONG invalid
    ldi:8 #0x20, r1
    cmp r1, r5
    BHI_LONG invalid
    ldi:20 #SCR_R, r1
    ld @r1, r0
    cmp #0, r0
    BNE_LONG invalid
    JMPABS valid
valid_read:
    ldi:20 #SCR_W, r1
    ld @r1, r5
    cmp #0, r5
    BNE_LONG invalid
    ldi:20 #SCR_R, r1
    ld @r1, r5
    cmp #1, r5
    BLT_LONG invalid
    ldi:8 #0x20, r1
    cmp r1, r5
    BHI_LONG invalid
    JMPABS valid
valid_write_read:
    ldi:20 #SCR_W, r1
    ld @r1, r5
    cmp #1, r5
    BLT_LONG invalid
    ldi:8 #0x20, r1
    cmp r1, r5
    BHI_LONG invalid
    ldi:20 #SCR_R, r1
    ld @r1, r5
    cmp #1, r5
    BLT_LONG invalid
    ldi:8 #0x20, r1
    cmp r1, r5
    BHI_LONG invalid
valid:
    /* Clear per-request diagnostics. */
    ldi:20 #SCR_ACKED, r1
    ldi:8 #0x00, r0
    st r0, @r1
    ldi:20 #SCR_RLEN, r1
    st r0, @r1
    ldi:20 #SCR_CODE, r1
    st r0, @r1
    ldi:20 #SCR_STAGE, r1
    st r0, @r1
    ldi:20 #SCR_IDLE, r1
    st r0, @r1
    ldi:8 #0x00, r11

    call start_free
    cmp #0, r0
    beq bus_free
    ldi:8 #0x05, r0
    mov r0, r5
    ldi:8 #0x05, r5
    JMPABS no_stop_error
bus_free:

    ldi:20 #SCR_OP, r1
    ld @r1, r0
    cmp #2, r0
    beq start_read

    /* WRITE / WRITE_READ: address even, START+WRITE. */
    ldi:20 #SCR_ADDR, r1
    ld @r1, r5
    ldi:8 #0x50, r7
    call send_byte
    cmp #0, r0
    beq write_data
    cmp #1, r0
    beq nack_addr1
    mov r0, r5
    ldi:8 #0x01, r5
    bra error_stop

write_data:
    ldi:20 #SCR_W, r1
    ld @r1, r6
    mov r10, r1
    ldi:8 #0x10, r0
    add r0, r1
write_loop:
    cmp #0, r6
    beq write_done
    ldub @r1, r5
    ldi:8 #0x50, r7
    st r1, @-r15
    call send_byte
    ld @r15+, r1
    cmp #0, r0
    beq write_ok
    cmp #1, r0
    beq nack_data
    mov r0, r5
    ldi:8 #0x02, r5
    bra error_stop
write_ok:
    add #1, r1
    sub r3, r6
    ldi:20 #SCR_ACKED, r0
    ld @r0, r5
    add r3, r5
    st r5, @r0
    bra write_loop
write_done:
    ldi:20 #SCR_OP, r1
    ld @r1, r0
    cmp #1, r0
    beq success
    /* WRITE_READ: repeated START and read address. */
    ldi:20 #SCR_ADDR, r1
    ld @r1, r5
    or r3, r5
    ldi:8 #0x70, r7
    call send_byte
    cmp #0, r0
    beq start_read_loop
    cmp #1, r0
    beq nack_addr3
    mov r0, r5
    ldi:8 #0x03, r5
    bra error_stop

start_read:
    ldi:20 #SCR_ADDR, r1
    ld @r1, r5
    or r3, r5
    ldi:8 #0x50, r7
    call send_byte
    cmp #0, r0
    beq start_read_loop
    cmp #1, r0
    beq nack_addr3
    mov r0, r5
    ldi:8 #0x03, r5
    bra error_stop

start_read_loop:
    ldi:20 #SCR_R, r1
    ld @r1, r6
    mov r9, r1
    ldi:8 #0x10, r0
    add r0, r1
read_loop:
    cmp #0, r6
    beq success
    cmp #1, r6
    beq read_last
    ldi:8 #0x58, r7
    bra read_do
read_last:
    ldi:8 #0x50, r7
read_do:
    st r1, @-r15
    call read_byte
    ld @r15+, r1
    cmp #0, r0
    beq read_ok
    mov r0, r5
    ldi:8 #0x04, r5
    bra error_stop
read_ok:
    stb r5, @r1
    add #1, r1
    sub r3, r6
    ldi:20 #SCR_RLEN, r0
    ld @r0, r5
    add r3, r5
    st r5, @r0
    bra read_loop

success:
    ldi:20 #SCR_CODE, r1
    ldi:8 #0x00, r0
    st r0, @r1
    ldi:20 #SCR_STAGE, r1
    st r0, @r1
    call stop_bus
    ldi:20 #SCR_IDLE, r1
    st r0, @r1
    JMPABS complete

nack_addr1:
    ldi:8 #0x01, r0
    ldi:8 #0x01, r5
    bra error_stop
nack_addr3:
    ldi:8 #0x01, r0
    ldi:8 #0x03, r5
    bra error_stop
nack_data:
    ldi:8 #0x02, r0
    ldi:8 #0x02, r5
    bra error_stop

error_stop:
    ldi:20 #SCR_CODE, r1
    st r0, @r1
    ldi:20 #SCR_STAGE, r1
    st r5, @r1
    call stop_bus
    ldi:20 #SCR_IDLE, r1
    st r0, @r1
    JMPABS complete

invalid:
    ldi:8 #0x10, r0
    ldi:8 #0x00, r5
    JMPABS no_stop_error
torn:
    ldi:8 #0x12, r0
    ldi:8 #0x00, r5
no_stop_error:
    ldi:20 #SCR_CODE, r1
    st r0, @r1
    ldi:20 #SCR_STAGE, r1
    st r5, @r1
    JMPABS complete

/* ------------------------------------------------------------------ */
/* Completion publication                                             */
/* ------------------------------------------------------------------ */
complete:
    /* status = code | stage<<8 | acked<<16 | bus_idle<<24 */
    ldi:20 #SCR_CODE, r1
    ld @r1, r0
    ldi:20 #SCR_STAGE, r1
    ld @r1, r5
    lsl #8, r5
    or r5, r0
    ldi:20 #SCR_ACKED, r1
    ld @r1, r5
    lsl #8, r5
    lsl #8, r5
    or r5, r0
    ldi:20 #SCR_IDLE, r1
    ld @r1, r5
    lsl #8, r5
    lsl #8, r5
    lsl #8, r5
    or r5, r0
    mov r9, r1
    add #4, r1
    st r0, @r1
    ldi:20 #SCR_RLEN, r1
    ld @r1, r0
    mov r9, r1
    add #8, r1
    st r0, @r1
    mov r9, r1
    ldi:8 #0x0C, r0
    add r0, r1
    st r11, @r1
    ldi:20 #SCR_TAG, r1
    ld @r1, r7
    mov r9, r1
    ldi:8 #0x3C, r0
    add r0, r1
    st r7, @r1
    st r7, @r9

    ldi:20 #SCR_CONS, r1
    ld @r1, r5
    add r3, r5
    ldi:32 #0xFFFF, r1
    and r1, r5
    ldi:20 #STAT_BASE+0x18, r1
    st r5, @r1

    ldi:20 #SCR_CODE, r1
    ld @r1, r0
    cmp #0, r0
    bne complete_error
    ldi:20 #STAT_BASE+0x1C, r1
    st r5, @r1
    ldi:20 #STAT_BASE+0x28, r1
    ld @r1, r0
    add r3, r0
    st r0, @r1
    JMPABS main_loop

complete_error:
    ldi:20 #SCR_CONS, r1
    ld @r1, r7
    ldi:20 #STAT_BASE+0x20, r1
    st r7, @r1
    mov r9, r1
    add #4, r1
    ld @r1, r7
    ldi:20 #STAT_BASE+0x24, r1
    st r7, @r1
    ldi:20 #STAT_BASE+0x00, r1
    ldi:8 #0x04, r0
    st r0, @r1
    ldi:32 #0x80000000, r0
    or r5, r0
    ldi:20 #STAT_BASE+0x1C, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x2C, r1
    ld @r1, r0
    add r3, r0
    st r0, @r1
    JMPABS main_loop

bad_ctrl:
    ldi:20 #STAT_BASE+0x00, r1
    ldi:8 #0xE0, r0
    st r0, @r1
    ldi:20 #STAT_BASE+0x08, r1
    ldi:8 #0x01, r0
    st r0, @r1
    JMPABS main_loop

/* ------------------------------------------------------------------ */
/* I2C byte helpers                                                   */
/* ------------------------------------------------------------------ */

/* wait for the bus to become free; R0=0 free, R0=5 BUS_BUSY */
start_free:
    ldi:32 #0x00040000, r1
sf_loop:
    ld @r12, r0
    and r4, r0
    beq sf_free
    add r3, r11
    sub r3, r1
    beq sf_busy
    bra sf_loop
sf_free:
    ldi:8 #0x00, r0
    ret
sf_busy:
    ldi:8 #0x05, r0
    ret

/* send R5 with control R7; R0=0 OK, 1 NACK, 3 BUS_ERROR, 4 TIMEOUT */
send_byte:
    st r5, @r14
    st r7, @r13
    ldi:32 #0x00040000, r1
sb_loop:
    ld @r13, r0
    and r4, r0
    bne sb_bus_error
    ld @r13, r0
    and r3, r0
    bne sb_done
    add r3, r11
    sub r3, r1
    beq sb_timeout
    bra sb_loop
sb_done:
    ld @r12, r0
    and r2, r0
    bne sb_nack
    ldi:8 #0x00, r0
    ret
sb_nack:
    ldi:8 #0x01, r0
    ret
sb_bus_error:
    ldi:8 #0x03, r0
    ret
sb_timeout:
    ldi:8 #0x04, r0
    ret

/* read one byte with control R7; R5=byte, R0=0 OK, 3 BUS_ERROR, 4 TIMEOUT */
read_byte:
    st r7, @r13
    ldi:32 #0x00040000, r1
rb_loop:
    ld @r13, r0
    and r4, r0
    bne rb_bus_error
    ld @r13, r0
    and r3, r0
    bne rb_done
    add r3, r11
    sub r3, r1
    beq rb_timeout
    bra rb_loop
rb_done:
    ld @r14, r5
    ldi:8 #0x00, r0
    ret
rb_bus_error:
    ldi:8 #0x03, r0
    ret
rb_timeout:
    ldi:8 #0x04, r0
    ret

/* STOP and wait; R0=1 idle, R0=0 not idle */
stop_bus:
    ldi:8 #0x40, r0
    st r0, @r13
    ldi:32 #0x00040000, r1
st_loop:
    ld @r12, r0
    and r4, r0
    beq st_idle
    add r3, r11
    sub r3, r1
    beq st_busy
    bra st_loop
st_idle:
    ldi:8 #0x01, r0
    ret
st_busy:
    ldi:8 #0x00, r0
    ret
