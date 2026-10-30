/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Core 1 candidate image. Code and constants occupy A0000-A2FFF. The stack
 * (A3000-A37FF), shared data (A3800-A3FFF) and reset vector (AFFFFC) are
 * outside this image and are owned by the host/contract. __core1_build_id is
 * supplied by the generator after hashing the same image with that field zero.
 */
ENTRY(_start)
__core1_base = 0x000A0000;
__core1_stack_start = 0x000A3000;
__core1_data_base = 0x000A3800;
PROVIDE(__core1_capacity_limit = 0x3000);

SECTIONS {
  . = __core1_base;
  .text : { *(.text) . = ALIGN(4); } =0x9FA0
  __core1_image_end = .;
  __core1_image_bytes = __core1_image_end - __core1_base;

  ASSERT(__core1_image_bytes > 0, "Core 1 image is empty")
  ASSERT(__core1_image_bytes <= __core1_capacity_limit, "Core 1 code exceeds capacity")
  ASSERT(__core1_image_bytes <= 0x3000, "Core 1 code exceeds 12 KiB")
  ASSERT((__core1_image_bytes & 3) == 0, "Core 1 image is not 4-byte aligned")
  ASSERT(__core1_image_end <= __core1_stack_start, "Core 1 image overlaps the stack")
  ASSERT(__core1_image_end <= __core1_data_base, "Core 1 image overlaps shared data")
  ASSERT(_start == __core1_base, "Core 1 entry is not A0000")
  ASSERT((_start & 3) == 0, "Core 1 entry is not 4-byte aligned")

  /DISCARD/ : { *(.comment) *(.note*) }
}
