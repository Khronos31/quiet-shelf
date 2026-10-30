/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fixture placement for Widen bank 0. The address and the 0x800 B limit are
 * the fixed-coax candidate from the generation preflight; neither is a
 * confirmed product contract yet.
 */
ENTRY(_start)
SECTIONS {
  . = 0x000e1000;
  .text : { *(.text) ; . = ALIGN(4);
    ASSERT(. - ADDR(.text) <= __fr30_text_limit, "FR30 .text exceeds limit"); } =0x9fa0
  /DISCARD/ : { *(.comment) *(.note*) }
}
