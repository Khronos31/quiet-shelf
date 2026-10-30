/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Core 0 TS carrier candidate for CECH-ZD1J.
 *
 * Original FR30 assembly for torne-userland. The INFO/CTRL/STAT layout,
 * ring/epoch/heartbeat rules and fault codes follow contract v0.2.2 sections
 * 2, 4, 5, 7 and 9.1. The internal DMA/FIFO/packetizer register values and
 * the event bits are the candidate hardware knowledge from the fixed coax
 * commit b118c14f85e09ed0f98340a60dec0ae70aed3ea5
 * (internal/coaxd/usb/bridge/mb86h47/core0prog.go:25-137 and stream.go:160-173).
 * This is not a device-verified program.
 *
 * The host zeroes A3800-A3FFF, writes CTRL, loads this image at A0000 and
 * then releases the core. The host owns the TS engine and USB endpoint; this
 * image only moves written ring records through DMA ch2, FIFO and packetizer,
 * and publishes the writer limit.
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
    .macro BC_LONG label
      bnc 1f
      JMPABS \label
1:
    .endm
    .macro BNC_LONG label
      bc 1f
      JMPABS \label
1:
    .endm

    .equ INFO_BASE,  0x000A3800
    .equ CTRL_BASE,  0x000A3840
    .equ STAT_BASE,  0x000A3880

    .equ UNIT_SIZE,  192
    .equ FIFO_CAP,   0x840

    .equ DMA_READ_POS,   0x1048
    .equ DMA_READ_LIMIT, 0x1038
    .equ DMA_CONTROL,    0x1068
    .equ DMA_REFILL_LEN, 0x1078
    .equ FIFO_HEAD,      0x10068
    .equ FIFO_TAIL,      0x10058

    .global _start
    .balign 4
_start:
    /* Initial stack is the contract value, below the shared data. */
    ldi:20 #0x000A37FC, r15

    /* STAT: BOOT and all diagnostic fields zero. The host also zeroes the
       data area before RUN; writing the defined fields keeps a warm restart
       from exposing stale values. */
    ldi:20 #STAT_BASE+0x00, r1
    ldi:8 #0x01, r0
    st r0, @r1
    ldi:8 #0x00, r0
    ldi:20 #STAT_BASE+0x04, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x08, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x0C, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x10, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x14, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x18, r1
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
    ldi:20 #STAT_BASE+0x34, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x38, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x3C, r1
    st r0, @r1

    /* CTRL.session must have a non-zero high half. */
    ldi:20 #CTRL_BASE+0x00, r1
    ld @r1, r0
    ldi:32 #0xFFFF0000, r1
    and r1, r0
    BEQ_LONG fault_bad_ctrl

    /* CTRL.ring_size: non-zero, a multiple of 192, and at least one unit. */
    ldi:20 #CTRL_BASE+0x18, r1
    ld @r1, r6
    cmp #0, r6
    BEQ_LONG fault_bad_ctrl
    ldi:20 #CTRL_BASE+0x1C, r1
    ld @r1, r0
    ldi:20 #UNIT_SIZE, r1
    cmp r1, r0
    BNE_LONG fault_bad_ctrl

    mov r6, r4
    ldi:8 #0x3F, r1
    and r1, r4
    BNE_LONG fault_bad_ctrl

    /* n mod 3 = byte-sum mod 3, since 256 == 1 (mod 3). */
    mov r6, r5
    lsr #6, r5
    mov r5, r0
    ldi:8 #0xFF, r1
    and r1, r0
    mov r5, r4
    lsr #8, r4
    and r1, r4
    add r4, r0
    mov r5, r4
    lsr #8, r4
    lsr #8, r4
    and r1, r4
    add r4, r0
    mov r5, r4
    lsr #8, r4
    lsr #8, r4
    lsr #8, r4
    and r1, r4
    add r4, r0
mod3_loop:
    cmp #3, r0
    bc mod3_done
    ldi:8 #0x03, r1
    sub r1, r0
    bra mod3_loop
mod3_done:
    cmp #0, r0
    BNE_LONG fault_bad_ctrl

    /* CTRL.margin: below the ring size, and leaves room for one unit. */
    ldi:20 #CTRL_BASE+0x28, r1
    ld @r1, r4
    cmp r6, r4
    BNC_LONG fault_bad_ctrl
    mov r6, r5
    sub r4, r5
    ldi:20 #UNIT_SIZE, r1
    cmp r1, r5
    BC_LONG fault_bad_ctrl

    /* CTRL.report_every must make the limit update reachable. */
    ldi:20 #CTRL_BASE+0x2C, r1
    ld @r1, r4
    cmp #0, r4
    BEQ_LONG fault_bad_ctrl
    mov r4, r10

    /* The two bus registers must be usable, aligned addresses. A zero value
       would otherwise write the core's own local register 0. */
    ldi:20 #CTRL_BASE+0x20, r1
    ld @r1, r14
    cmp #0, r14
    BEQ_LONG fault_bad_ctrl
    mov r14, r0
    ldi:8 #0x03, r1
    and r1, r0
    BNE_LONG fault_bad_ctrl
    ldi:20 #CTRL_BASE+0x24, r1
    ld @r1, r13
    cmp #0, r13
    BEQ_LONG fault_bad_ctrl
    mov r13, r0
    ldi:8 #0x03, r1
    and r1, r0
    BNE_LONG fault_bad_ctrl

    mov r6, r12                  /* ring_size */
    ldi:20 #CTRL_BASE+0x28, r1
    ld @r1, r11                  /* margin */
    mov r6, r7
    sub r11, r7
    ldi:20 #UNIT_SIZE, r1
    sub r1, r7                   /* near-full threshold */
    ldi:8 #0x00, r9              /* units since the last limit update */
    ldi:8 #0x00, r8              /* no DMA refill in flight */

    /* INFO, with magic written last. */
    ldi:20 #INFO_BASE+0x04, r1
    ldi:32 #0x00010000, r0
    st r0, @r1
    ldi:20 #INFO_BASE+0x08, r1
    ldi:32 #__core0_build_id, r0
    st r0, @r1
    ldi:20 #INFO_BASE+0x0C, r1
    ldi:8 #0x01, r0
    st r0, @r1
    ldi:20 #CTRL_BASE+0x00, r1
    ld @r1, r0
    ldi:20 #INFO_BASE+0x10, r1
    st r0, @r1
    ldi:20 #INFO_BASE+0x14, r1
    ldi:32 #__core0_image_bytes, r0
    st r0, @r1
    ldi:20 #INFO_BASE+0x18, r1
    ldi:20 #0x000A3800, r0
    st r0, @r1
    ldi:20 #INFO_BASE+0x1C, r1
    ldi:20 #FIFO_CAP, r0
    st r0, @r1
    ldi:20 #INFO_BASE+0x00, r1
    ldi:32 #0x54545330, r0
    st r0, @r1

    /* Release the core's blocks and request an internal-DMA stop. */
    ldi:20 #0x0000, r1
    ldi:8 #0x77, r0
    st r0, @r1
    ldi:8 #0x00, r0
    st r0, @r1
    ldi:20 #0x0004, r1
    ldi:32 #0xFFFFFFFF, r0
    st r0, @r1
    ldi:20 #0x0030, r1
    ldi:32 #0x004041FF, r0
    st r0, @r1
    ldi:20 #0x0050, r1
    st r0, @r1
    ldi:20 #0x0090, r1
    ldi:32 #0xFFFFFFFF, r0
    st r0, @r1
    ldi:20 #0x00B8, r1
    st r0, @r1
    ldi:20 #0x00C8, r1
    st r0, @r1
    ldi:20 #0x0010, r1
    st r0, @r1
    ldi:20 #0x0020, r1
    st r0, @r1
    ldi:20 #0x0040, r1
    ldi:8 #0x00, r0
    st r0, @r1
    ldi:20 #0x1060, r1
    ldi:20 #0x0300, r0
    st r0, @r1
    ldi:20 #0x1064, r1
    st r0, @r1
    ldi:20 #0x1068, r1
    st r0, @r1
    ldi:20 #0x106C, r1
    st r0, @r1
    ldi:20 #0x10B0, r1
    st r0, @r1
    ldi:20 #0x10B4, r1
    st r0, @r1

    /* Wait for the DMA stop with an implementation bound. The bound is not a
       measured hardware deadline; a real stop normally clears 0x300 quickly. */
    ldi:32 #0x00040000, r5
dma_wait:
    ldi:20 #DMA_CONTROL, r1
    ld @r1, r0
    ldi:20 #0x0300, r4
    and r4, r0
    beq dma_stopped
    ldi:8 #0x01, r4
    sub r4, r5
    cmp #0, r5
    BEQ_LONG fault_dma_quiesce
    bra dma_wait
dma_stopped:

    /* DMA ch2 reads the ring from the engine's current write offset. */
    ldi:20 #CTRL_BASE+0x14, r1
    ld @r1, r0
    ldi:20 #0x1018, r1
    st r0, @r1
    ldi:20 #0x1028, r1
    st r12, @r1
    ld @r14, r5
    ldi:20 #DMA_READ_POS, r1
    st r5, @r1
    ldi:20 #DMA_READ_LIMIT, r1
    st r5, @r1
    ldi:20 #STAT_BASE+0x28, r1
    st r5, @r1
    ldi:20 #STAT_BASE+0x2C, r1
    st r5, @r1
    ldi:20 #0x1000, r1
    ldi:32 #0xFFF00000, r0
    st r0, @r1

    /* Local FIFO layout. These are halfword registers in the fixed source. */
    ldi:20 #0x10010, r1
    ldi:20 #0x0000, r0
    sth r0, @r1
    ldi:20 #0x10030, r1
    sth r0, @r1
    ldi:20 #0x10034, r1
    ldi:20 #0x0800, r0
    sth r0, @r1
    ldi:20 #0x10038, r1
    ldi:20 #0x1000, r0
    sth r0, @r1
    ldi:20 #0x1003C, r1
    ldi:20 #0x1840, r0
    sth r0, @r1
    ldi:20 #0x10040, r1
    sth r0, @r1
    ldi:20 #0x10080, r1
    ldi:20 #0x00B8, r0
    sth r0, @r1
    ldi:20 #0x10084, r1
    sth r0, @r1
    ldi:20 #0x10088, r1
    ldi:20 #0x00C0, r0
    sth r0, @r1
    ldi:20 #0x1008C, r1
    ldi:20 #0x00B8, r0
    sth r0, @r1
    ldi:20 #0x100B0, r1
    ldi:20 #0x1880, r0
    sth r0, @r1
    ldi:20 #0x100B4, r1
    ldi:20 #0x1900, r0
    sth r0, @r1

    /* Packetizer and the 192-byte output port. */
    ldi:20 #0x2020, r1
    ldi:20 #0x0000, r0
    st r0, @r1
    ldi:20 #0x2024, r1
    st r0, @r1
    ldi:20 #0x202C, r1
    ldi:32 #0x03000400, r0
    st r0, @r1
    ldi:20 #0x2030, r1
    ldi:32 #0x05010101, r0
    st r0, @r1
    ldi:20 #0x3008, r1
    ldi:8 #0x00, r0
    st r0, @r1
    ldi:20 #0x5000, r1
    ldi:32 #0x411080C0, r0
    st r0, @r1
    ldi:20 #0x5008, r1
    ldi:32 #0x10000000, r0
    st r0, @r1
    ldi:20 #0x5004, r1
    ldi:32 #0x01000000, r0
    st r0, @r1
    ldi:20 #0x200C, r1
    ldi:32 #0x01000000, r0
    st r0, @r1

    /* Start one replenishment without counting it in STAT.refills, matching
       the fixed source's debug counter. */
    ldi:20 #DMA_REFILL_LEN, r1
    ldi:20 #0x0460, r0
    st r0, @r1
    ldi:20 #DMA_CONTROL, r1
    ldi:32 #0x01000000, r0
    st r0, @r1
    ldi:8 #0x01, r8

    /* epoch_echo before RUNNING. */
    ldi:20 #CTRL_BASE+0x10, r1
    ld @r1, r0
    ldi:20 #STAT_BASE+0x18, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x00, r1
    ldi:8 #0x03, r0
    st r0, @r1
    JMPABS main_loop

fault_bad_ctrl:
    ldi:20 #STAT_BASE+0x00, r1
    ldi:8 #0xE0, r0
    st r0, @r1
    ldi:20 #STAT_BASE+0x08, r1
    ldi:8 #0x01, r0
    st r0, @r1
    JMPABS main_loop
fault_dma_quiesce:
    ldi:20 #STAT_BASE+0x00, r1
    ldi:8 #0xE0, r0
    st r0, @r1
    ldi:20 #STAT_BASE+0x08, r1
    ldi:8 #0x02, r0
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
    ld @r1, r5
    ldi:20 #STAT_BASE+0x10, r1
    ld @r1, r4
    cmp r4, r5
    beq nop_done
    ldi:20 #CTRL_BASE+0x08, r1
    ld @r1, r4
    ldi:8 #0x00, r0
    cmp #0, r4
    beq nop_store
    ldi:8 #0x01, r0
nop_store:
    ldi:20 #STAT_BASE+0x14, r1
    st r0, @r1
    ldi:20 #STAT_BASE+0x10, r1
    st r5, @r1
nop_done:

    /* Only RUNNING moves data or touches the DMA registers. */
    ldi:20 #STAT_BASE+0x00, r1
    ld @r1, r0
    cmp #3, r0
    BNE_LONG main_loop

    /* Event word and its diagnostics copy. */
    ldi:20 #0x0020, r1
    ld @r1, r6
    ldi:20 #STAT_BASE+0x24, r1
    st r6, @r1

    /* Event bit 29: the replenishment finished; acknowledge it. */
    ldi:32 #0x20000000, r4
    mov r6, r0
    and r4, r0
    beq no_refill_done
    ldi:20 #0x0020, r1
    st r4, @r1
    ldi:8 #0x00, r8
no_refill_done:

    /* The DMA may read up to the engine's current write offset. */
    ld @r14, r5
    ldi:20 #STAT_BASE+0x28, r1
    st r5, @r1
    ldi:20 #DMA_READ_LIMIT, r1
    st r5, @r1

    /* Current read cursor, fill level, maximum and near-full count. */
    ldi:20 #DMA_READ_POS, r1
    ld @r1, r4
    ldi:20 #STAT_BASE+0x2C, r1
    st r4, @r1
    mov r5, r0
    sub r4, r0
    cmp r4, r5
    bnc fill_ready
    add r12, r0
fill_ready:
    ldi:20 #STAT_BASE+0x30, r1
    ld @r1, r4
    cmp r4, r0
    bhi max_update
    bra max_done
max_update:
    ldi:20 #STAT_BASE+0x30, r1
    st r0, @r1
max_done:
    cmp r7, r0
    bnc near_full_update
    bra near_full_done
near_full_update:
    ldi:20 #STAT_BASE+0x34, r1
    ld @r1, r4
    add #1, r4
    st r4, @r1
near_full_done:

    /* Refill when no transfer is in flight and the FIFO has room. */
    cmp #0, r8
    bne no_refill
    ldi:20 #FIFO_HEAD, r1
    lduh @r1, r0
    ldi:20 #FIFO_TAIL, r1
    lduh @r1, r4
    sub r4, r0
    cmp #0, r0
    bne free_nonzero
    ldi:20 #FIFO_CAP, r0
free_nonzero:
    cmp #0, r0
    bge free_positive
    ldi:20 #FIFO_CAP, r4
    add r4, r0
free_positive:
    ldi:20 #0x0470, r4
    cmp r4, r0
    bls no_refill
    ldi:20 #DMA_REFILL_LEN, r1
    ldi:20 #0x0460, r0
    st r0, @r1
    ldi:20 #DMA_CONTROL, r1
    ldi:32 #0x01000000, r0
    st r0, @r1
    ldi:8 #0x01, r8
    ldi:20 #STAT_BASE+0x20, r1
    ld @r1, r0
    add #1, r0
    st r0, @r1
no_refill:

    /* Event bit 5 with the packetizer idle: send the FIFO head as one
       192-byte record. */
    ldi:20 #0x0020, r4
    mov r6, r0
    and r4, r0
    beq no_packet
    ldi:20 #0x2000, r1
    ld @r1, r0
    ldi:8 #0x07, r4
    and r4, r0
    bne no_packet
    ldi:20 #0x0020, r1
    ldi:8 #0x08, r0
    st r0, @r1
    ldi:20 #0x200C, r1
    ldi:32 #0x01000000, r0
    st r0, @r1
    ldi:20 #0x2004, r1
    ldi:20 #0x00BF, r0
    st r0, @r1
    ldi:20 #0x2000, r1
    ldi:32 #0x0D000041, r0
    st r0, @r1
    ldi:20 #STAT_BASE+0x1C, r1
    ld @r1, r0
    add #1, r0
    st r0, @r1
    add #1, r9

    /* Every CTRL.report_every units, hand the read cursor less the margin
       back to the engine as the writer limit. */
    cmp r10, r9
    bne no_report
    ldi:8 #0x00, r9
    ldi:20 #DMA_READ_POS, r1
    ld @r1, r0
    cmp r11, r0
    bnc report_subtract
    mov r12, r4
    sub r11, r4
    add r4, r0
    bra report_store
report_subtract:
    sub r11, r0
report_store:
    st r0, @r13
no_report:

no_packet:
    JMPABS main_loop
