/* machboot firmware: loaded by the boot ROM to 0x400, runs from RAM */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x400, LENGTH = 0x15C00 }
SECTIONS {
	.text : { *(.text.start) *(.text .text.*) } > RAM
	.rodata : { *(.rodata .rodata.* .srodata .srodata.*) } > RAM
	.data : { *(.data .data.* .sdata .sdata.*) } > RAM
	_image_end = .;
	.bss (NOLOAD) : ALIGN(4) { _bss_start = .; *(.bss .bss.* .sbss .sbss.* COMMON) . = ALIGN(4); _bss_end = .; } > RAM
	_end = .;
	ASSERT(_end <= 0x14800, "firmware + bss leave less than 6 KB of stack")
}
