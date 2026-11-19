/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Firmware entry. The boot ROM jumps here with a0 = the flash address
 * it found the firmware at (docs/boot.md).
 */
	.section .text.start
	.global _start
_start:
	li	sp, 0x16000
	mv	s0, a0
	la	t0, _bss_start
	la	t1, _bss_end
1:	bgeu	t0, t1, 2f
	sw	zero, 0(t0)
	addi	t0, t0, 4
	j	1b
2:	mv	a0, s0
	call	main
3:	j	3b
