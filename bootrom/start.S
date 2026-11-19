/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Boot ROM entry: the CPU resets to 0; 0x10 is the (unused) interrupt
 * vector, kept as a trap.
 */
	.section .text.start
	.global _start
_start:
	j	reset
	.balign 16
irq:
	j	irq
reset:
	li	sp, 0x16000
	call	boot_main
1:	j	1b
