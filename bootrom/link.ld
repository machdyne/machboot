/* machboot boot ROM: the first 1 KB of RAM (its initial contents) */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { ROM (rwx) : ORIGIN = 0x0, LENGTH = 0x400 }
SECTIONS {
	.text : { *(.text.start) *(.text .text.*) *(.rodata .rodata.* .srodata .srodata.*) *(.data .data.* .sdata .sdata.*) *(.bss .bss.* .sbss .sbss.* COMMON) } > ROM
}
