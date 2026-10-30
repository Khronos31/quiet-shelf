/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Original torne-userland FR30 fixture. It performs no device I/O and is
 * assembled only to check the generation path; it is not a loadable product
 * firmware image.
 */
    .section .text
    .global _start
_start:
    ldi:8 #0x2a,r0
    ldi:20 #0x12345,r1
    nop
    ret
