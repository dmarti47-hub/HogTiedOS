/*
 * HogTiedOS boot shim: the "startup" code the stock QNX IPL jumps to.
 *
 * Entry state (measured by running the stock IPL, see
 * docs/findings/CROSS_CHECKS.md sec. 8): SVC mode, IRQ/FIQ masked,
 * MMU/D-cache/I-cache off. The IPL has copied the header plus this code to
 * image_paddr and patched header+0x28 (imagefs_paddr) to point at the
 * payload it left at 0x84000000 + startup_size.
 *
 * Job: find the payload table, print one line on UART3, and enter the Linux
 * zImage with the ARM boot protocol registers:
 *   r0 = 0, r1 = 0xffffffff (DT-only boot), r2 = physical address of the DTB.
 *
 * This code never touches the DRAM controller, clocks, pads or NAND.
 * Position-independent: it only uses PC-relative addressing.
 */
	.syntax unified
	.arm
	.text

	.equ	HDR_SIZE,	0x100
	.equ	HDR_IMAGEFS,	0x28		/* patched by the IPL */
	.equ	UART3_THR,	0x49020000
	.equ	UART3_LSR,	0x49020014
	.equ	LSR_THRE,	0x20

	/* payload table (see mkifs.py) */
	.equ	PT_MAGIC,	0x584c5448	/* "HTLX" */
	.equ	PT_KERNEL_OFF,	8
	.equ	PT_DTB_OFF,	16

	.global	_start
_start:
	adr	r4, _start
	sub	r4, r4, #HDR_SIZE		@ r4 = header copy
	ldr	r5, [r4, #HDR_IMAGEFS]		@ r5 = payload table

	ldr	r0, [r5]
	ldr	r1, =PT_MAGIC
	cmp	r0, r1
	bne	bad_payload

	adr	r0, msg_boot
	bl	puts

	ldr	r6, [r5, #PT_KERNEL_OFF]
	add	r6, r5, r6			@ r6 = zImage entry
	ldr	r2, [r5, #PT_DTB_OFF]
	add	r2, r5, r2			@ r2 = DTB
	mov	r0, #0
	mvn	r1, #0				@ r1 = 0xffffffff: machine from DT
	bx	r6

bad_payload:
	adr	r0, msg_bad
	bl	puts
hang:
	wfi
	b	hang

/* puts(r0): write a NUL-terminated string to UART3 (already set up by the IPL) */
puts:
	ldr	r2, =UART3_THR
1:	ldrb	r1, [r0], #1
	cmp	r1, #0
	bxeq	lr
2:	ldr	r3, [r2, #(UART3_LSR - UART3_THR)]
	tst	r3, #LSR_THRE
	beq	2b
	str	r1, [r2]
	b	1b

	.ltorg
msg_boot:
	.asciz	"\r\nHogTiedOS shim: starting Linux\r\n"
msg_bad:
	.asciz	"\r\nHogTiedOS shim: bad payload table, halting\r\n"
	.balign	4
