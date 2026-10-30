/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fixture placement for Core 1's local instruction RAM. The address is the
 * fixed-coax candidate from the generation preflight; the core-local limit is
 * passed by the generator because the product contract does not fix it yet.
 */
ENTRY(_start)
SECTIONS {
  . = 0x000a0000;
  .text : { *(.text) ; . = ALIGN(4);
    ASSERT(. - ADDR(.text) <= __fr30_text_limit, "FR30 .text exceeds limit"); } =0x9fa0
  /DISCARD/ : { *(.comment) *(.note*) }
}
