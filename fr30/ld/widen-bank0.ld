/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Widen bank 0 candidate image. The header occupies base+0x00..0x1F and the
 * code starts at base+0x20. __widen_build_id is supplied by the two-pass
 * generator after hashing the same image with that field zero.
 */
ENTRY(read_handler)
__widen_base = 0x000E1000;
PROVIDE(__widen_build_id = 0);
PROVIDE(__widen_capacity_limit = 0x800);
SECTIONS {
  . = __widen_base;
  .header : {
    LONG(0x5457444E)                 /* "TWDN" */
    LONG(0x00010000)                 /* ABI v1.0 */
    LONG(__widen_build_id)
    LONG(0x00000001)                 /* WIDEN_RW */
    LONG(__widen_image_end - __widen_base)
    LONG(read_handler)
    LONG(write_handler)
    LONG(0)
  }
  . = __widen_base + 0x20;
  .text : { *(.text) . = ALIGN(4); } =0x9FA0
  __widen_image_end = .;
  ASSERT(__widen_image_end - __widen_base > 0x20, "Widen image has no code")
  ASSERT(__widen_image_end - __widen_base <= __widen_capacity_limit, "Widen image exceeds capacity")
  ASSERT(__widen_image_end - __widen_base <= 0x800, "Widen image exceeds bank")
  ASSERT(((__widen_image_end - __widen_base) & 3) == 0, "Widen image is not 4-byte aligned")
  ASSERT(read_handler >= __widen_base + 0x20, "read handler is below the header")
  ASSERT(read_handler < __widen_image_end, "read handler is outside the image")
  ASSERT((read_handler & 3) == 0, "read handler is not 4-byte aligned")
  ASSERT(write_handler >= __widen_base + 0x20, "write handler is below the header")
  ASSERT(write_handler < __widen_image_end, "write handler is outside the image")
  ASSERT((write_handler & 3) == 0, "write handler is not 4-byte aligned")
  /DISCARD/ : { *(.comment) *(.note*) }
}
