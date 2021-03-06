#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/asm-offsets.h>
#include "abort-macro.S"
/*
 * Function: v5tj_early_abort
 *
 * Params  : r2 = address of aborted instruction
 *         : r3 = saved SPSR
 *
 * Returns : r0 = address of abort
 *	   : r1 = FSR, bit 11 = write
 *	   : r2-r8 = corrupted
 *	   : r9 = preserved
 *	   : sp = pointer to registers
 *
 * Purpose : obtain information about current aborted instruction.
 * Note: we read user space.  This means we might cause a data
 * abort here if the I-TLB and D-TLB aren't seeing the same
 * picture.  Unfortunately, this does happen.  We live with it.
 */
	.align	5
ENTRY(v5tj_early_abort)
	mrc	p15, 0, r1, c5, c0, 0		@ get FSR
	mrc	p15, 0, r0, c6, c0, 0		@ get FAR
	bic	r1, r1, #1 << 11 | 1 << 10	@ clear bits 11 and 10 of FSR
	tst	r3, #PSR_J_BIT			@ Java?
	bne	jazelle_abort
	do_thumb_abort
	ldreq	r3, [r2]			@ read aborted ARM instruction
	do_ldrd_abort
	tst	r3, #1 << 20			@ L = 0 -> write
	orreq	r1, r1, #1 << 11		@ yes.
	mov	pc, lr

jazelle_abort:
	mov	r2, sp, lsr #13			@ get the top of the stack
	mov	r2, r2, lsl #13			@ containing the thread_info structure
	ldr	r3, [r2, #TI_JABORT_ADDR]	@ get the last Jazelle abort address
	cmp	r0, r3				@ is it the same address
	orreq	r1, r1, #1 << 11		@ mark it as a write
	str	r0, [r2, #TI_JABORT_ADDR]	@ always store the last Jazelle abort
	mov	pc, lr

