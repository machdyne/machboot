/* machboot firmware, Lakritz and Konfekt: as fw/link.ld, and the pages as
   overlays (fw/ovl.h, docs/overlays.md). Each fw/page_*.c (with diag.c
   and selfinst.c) is compiled on its own as build/ovl_<name>_*.o, linked
   to run in one window after .data, and copied out by objcopy; mkfw.py
   puts them in flash after the firmware. Their data stays resident. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x400, LENGTH = 0x15C00 }
SECTIONS {
	.text : { *(.text.start) EXCLUDE_FILE(*ovl_*.o) *(.text .text.*) } > RAM
	.rodata : { EXCLUDE_FILE(*ovl_*.o) *(.rodata .rodata.* .srodata .srodata.*) } > RAM
	.data : { *(.data .data.* .sdata .sdata.*) } > RAM
	_image_end = .;
	. = ALIGN(4);
	__ovl_start = .;
	OVERLAY : NOCROSSREFS AT (0x01000000) {
		.ovl_start    { *ovl_start_*.o(.text .text.* .rodata .rodata.* .srodata .srodata.*) . = ALIGN(4); }
		.ovl_update   { *ovl_update_*.o(.text .text.* .rodata .rodata.* .srodata .srodata.*) . = ALIGN(4); }
		.ovl_card     { *ovl_card_*.o(.text .text.* .rodata .rodata.* .srodata .srodata.*) . = ALIGN(4); }
		.ovl_settings { *ovl_settings_*.o(.text .text.* .rodata .rodata.* .srodata .srodata.*) . = ALIGN(4); }
		.ovl_diag     { *ovl_diag_*.o(.text .text.* .rodata .rodata.* .srodata .srodata.*) . = ALIGN(4); }
		.ovl_board    { *ovl_board_*.o(.text .text.* .rodata .rodata.* .srodata .srodata.*) . = ALIGN(4); }
		.ovl_instmode { *ovl_instmode_*.o(.text .text.* .rodata .rodata.* .srodata .srodata.*) . = ALIGN(4); }
	} > RAM
	__ovl_size = . - __ovl_start;
	.bss (NOLOAD) : ALIGN(4) { _bss_start = .; *(.bss .bss.* .sbss .sbss.* COMMON) . = ALIGN(4); _bss_end = .; } > RAM
	_end = .;
	ASSERT(_end <= 0x14800, "firmware + bss leave less than 6 KB of stack")
}
