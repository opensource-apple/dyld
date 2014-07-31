/*
 * Copyright (c) 1999-2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * C runtime startup for i386 and ppc interface to the dynamic linker.
 * This is the same as the entry point in crt0.o with the addition of the
 * address of the mach header passed as the an extra first argument.
 *
 * Kernel sets up stack frame to look like:
 *
 *	| STRING AREA |
 *	+-------------+
 *	|      0      |	
*	+-------------+
 *	|  apple[n]   |
 *	+-------------+
 *	       :
 *	+-------------+
 *	|  apple[0]   | 
 *	+-------------+ 
 *	|      0      |
 *	+-------------+
 *	|    env[n]   |
 *	+-------------+
 *	       :
 *	       :
 *	+-------------+
 *	|    env[0]   |
 *	+-------------+
 *	|      0      |
 *	+-------------+
 *	| arg[argc-1] |
 *	+-------------+
 *	       :
 *	       :
 *	+-------------+
 *	|    arg[0]   |
 *	+-------------+
 *	|     argc    |
 *	+-------------+
 * sp->	|      mh     | address of where the a.out's file offset 0 is in memory
 *	+-------------+
 *
 *	Where arg[i] and env[i] point into the STRING AREA
 */

	.globl __dyld_start


#ifdef __i386__
	.data
__dyld_start_static_picbase: 
	.long   L__dyld_start_picbase


	.text
	.align 2
# stable entry points into dyld
	.globl	_stub_binding_helper
_stub_binding_helper:
	jmp	_stub_binding_helper_interface
	nop
	nop
	nop
	.globl	_dyld_func_lookup
_dyld_func_lookup:
	jmp	__Z18lookupDyldFunctionPKcPm

	.text
	.align	4, 0x90
	.globl __dyld_start
__dyld_start:
	pushl	$0		# push a zero for debugger end of frames marker
	movl	%esp,%ebp	# pointer to base of kernel frame
	andl    $-16,%esp       # force SSE alignment
	
	# call dyldbootstrap::start(app_mh, argc, argv, slide)
	call    L__dyld_start_picbase
L__dyld_start_picbase:	
	popl	%ebx		# set %ebx to runtime value of picbase
    	movl	__dyld_start_static_picbase-L__dyld_start_picbase(%ebx), %eax
	subl    %eax, %ebx      # slide = L__dyld_start_picbase - [__dyld_start_static_picbase]
	pushl   %ebx		# param4 = slide
	lea     12(%ebp),%ebx	
	pushl   %ebx		# param3 = argv
	movl	8(%ebp),%ebx	
	pushl   %ebx		# param2 = argc
	movl	4(%ebp),%ebx	
	pushl   %ebx		# param1 = mh
	call	__ZN13dyldbootstrap5startEPK11mach_headeriPPKcl	

    	# clean up stack and jump to result
	movl	%ebp,%esp	# restore the unaligned stack pointer
	addl	$8,%esp		# remove the mh argument, and debugger end
				#  frame marker
	movl	$0,%ebp		# restore ebp back to zero
	jmp	%eax		# jump to the entry point


	.globl dyld_stub_binding_helper
dyld_stub_binding_helper:
	hlt
L_end:
#endif /* __i386__ */



#if __ppc__ || __ppc64__
#include <architecture/ppc/mode_independent_asm.h>

	.data
	.align 2
__dyld_start_static_picbase: 
	.g_long   L__dyld_start_picbase

#if __ppc__	
	.set L_mh_offset,0
	.set L_argc_offset,4
	.set L_argv_offset,8
#else
	.set L_mh_offset,0
	.set L_argc_offset,8	; stack is 8-byte aligned and there is a 4-byte hole between argc and argv
	.set L_argv_offset,16
#endif

	.text
	.align 2
; stable entry points into dyld
	.globl	_stub_binding_helper
_stub_binding_helper:
	b	_stub_binding_helper_interface
	nop 
	.globl	_dyld_func_lookup
_dyld_func_lookup:
	b	__Z18lookupDyldFunctionPKcPm
	
	
	
	.text
	.align 2
__dyld_start:
	mr	r26,r1		; save original stack pointer into r26
	subi	r1,r1,GPR_BYTES	; make space for linkage
	clrrgi	r1,r1,5		; align to 32 bytes
	addi	r0,0,0		; load 0 into r0
	stg	r0,0(r1)	; terminate initial stack frame
	stgu	r1,-SF_MINSIZE(r1); allocate minimal stack frame
		
	; call dyldbootstrap::start(app_mh, argc, argv, slide)
	lg	r3,L_mh_offset(r26)	; r3 = mach_header
	lwz	r4,L_argc_offset(r26)	; r4 = argc (int == 4 bytes)
	addi	r5,r26,L_argv_offset	; r5 = argv
	bcl	20,31,L__dyld_start_picbase	
L__dyld_start_picbase:	
	mflr	r31		; put address of L__dyld_start_picbase in r31
	addis   r6,r31,ha16(__dyld_start_static_picbase-L__dyld_start_picbase)
	lg      r6,lo16(__dyld_start_static_picbase-L__dyld_start_picbase)(r6)
	subf    r6,r6,r31       ; r6 = slide
	bl	__ZN13dyldbootstrap5startEPK11mach_headeriPPKcl	
	
	; clean up stack and jump to result
	mtctr	r3		; Put entry point in count register
	mr	r12,r3		;  also put in r12 for ABI convention.
	addi	r1,r26,GPR_BYTES; Restore the stack pointer and remove the
				;  mach_header argument.
	bctr			; jump to the program's entry point

	.globl dyld_stub_binding_helper
dyld_stub_binding_helper:
	trap
L_end:
#endif /* __ppc__ */


