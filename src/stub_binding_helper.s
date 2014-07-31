/*
 * Copyright (c) 1999-2006 Apple Computer, Inc. All rights reserved.
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
 
 

#ifdef __i386__
/*
 * This is the interface for the stub_binding_helper for i386:
 * The caller has pushed the address of the a lazy pointer to be filled in with
 * the value for the defined symbol and pushed the address of the the mach
 * header this pointer comes from.
 *
 * sp+4	address of lazy pointer
 * sp+0	address of mach header
 *
 * Some inter-image function calls pass parameters in registers EAX, ECX, EDX, or XXM0-3,
 * Therefore those registers need to be preserved during the lazy binding.
 * 
 * After the symbol has been resolved and the pointer filled in this is to pop
 * these arguments off the stack and jump to the address of the defined symbol.
 */
#define MH_PARAM_BP			4
#define LP_PARAM_BP			8
#define RESULT_BP			8    /* in order to trash no registers, the target is stored back on the stack then ret it done to it */

#define MH_PARAM_OUT        0
#define LP_PARAM_OUT        4
#define EAX_SAVE			8
#define ECX_SAVE			12
#define EDX_SAVE			16
#define XMMM0_SAVE			32    /* 16-byte align */
#define XMMM1_SAVE			48
#define XMMM2_SAVE			64
#define XMMM3_SAVE			80
#define STACK_SIZE			96 /*  (XMMM3_SAVE+16) must be 16 byte aligned too */


    .text
    .align 4,0x90
	.globl _fast_stub_binding_helper_interface
_fast_stub_binding_helper_interface:
	pushl		$0
    .globl _stub_binding_helper_interface
_stub_binding_helper_interface:
	pushl		%ebp
	movl		%esp,%ebp
	subl		$STACK_SIZE,%esp		# at this point stack is 16-byte aligned because two meta-parameters where pushed
	movl		%eax,EAX_SAVE(%esp)		# save registers that might be used as parameters
	movl		%ecx,ECX_SAVE(%esp)
	movl		%edx,EDX_SAVE(%esp)
	movdqa		%xmm0,XMMM0_SAVE(%esp)
	movdqa		%xmm1,XMMM1_SAVE(%esp)
	movdqa		%xmm2,XMMM2_SAVE(%esp)
	movdqa		%xmm3,XMMM3_SAVE(%esp)
	movl		MH_PARAM_BP(%ebp),%eax	# call dyld::bindLazySymbol(mh, lazy_ptr)
	movl		%eax,MH_PARAM_OUT(%esp)
	movl		LP_PARAM_BP(%ebp),%eax
	movl		%eax,LP_PARAM_OUT(%esp)
	call		__ZN4dyld14bindLazySymbolEPK11mach_headerPm
	movl		%eax,RESULT_BP(%ebp)	# store target for ret
	movdqa		XMMM0_SAVE(%esp),%xmm0	# restore registers
	movdqa		XMMM1_SAVE(%esp),%xmm1
	movdqa		XMMM2_SAVE(%esp),%xmm2
	movdqa		XMMM3_SAVE(%esp),%xmm3
	movl		EAX_SAVE(%esp),%eax
	movl		ECX_SAVE(%esp),%ecx
	movl		EDX_SAVE(%esp),%edx
	addl		$STACK_SIZE,%esp
	popl		%ebp
	addl		$4,%esp					# remove meta-parameter, other meta-parmaeter now holds target for ret
	ret
#endif /* __i386__ */




#if __ppc__ || __ppc64__
#include <architecture/ppc/mode_independent_asm.h>
/*
 * This is the interface for the stub_binding_helper for the ppc:
 * The caller has placed in r11 the address of the a lazy pointer to be filled
 * in with the value for the defined symbol and placed in r12 the address of
 * the the mach header this pointer comes from.
 *
 * r11 address of lazy pointer
 * r12 address of mach header
 */
#define LRSAVE		MODE_CHOICE(8,16)
#define STACK_SIZE	MODE_CHOICE(144,288)
#define R3SAVE          MODE_CHOICE(56,112)
#define R4SAVE          MODE_CHOICE(60,120)
#define R5SAVE          MODE_CHOICE(64,128)
#define R6SAVE          MODE_CHOICE(68,136)
#define R7SAVE          MODE_CHOICE(72,144)
#define R8SAVE          MODE_CHOICE(76,152)
#define R9SAVE          MODE_CHOICE(80,160)
#define R10SAVE         MODE_CHOICE(84,168)

  
	.text
	.align 2
	.globl _stub_binding_helper_interface
_stub_binding_helper_interface:
	mflr	r0		    ; get link register value
	stg	r0,LRSAVE(r1)	    ; save link register value in the linkage area
	stgu	r1,-STACK_SIZE(r1)  ; save stack pointer and update it

	stg	r3,R3SAVE(r1)	; save all registers that could contain
	stg	r4,R4SAVE(r1)	;  parameters to the routine that is being
	stg	r5,R5SAVE(r1)	;  bound.
	stg	r6,R6SAVE(r1)
	stg	r7,R7SAVE(r1)
	stg	r8,R8SAVE(r1)
	stg	r9,R9SAVE(r1)
	stg	r10,R10SAVE(r1)

	mr	r3,r12		; move address of mach header to 1st parameter
	mr	r4,r11		; move address of lazy pointer to 2nd parameter
	; call dyld::bindLazySymbol(mh, lazy_symbol_pointer_address)
	bl	__ZN4dyld14bindLazySymbolEPK11mach_headerPm
	mr	r12,r3		; move the symbol`s address into r12
	mtctr	r12		; move the symbol`s address into count register

	lg	r0,STACK_SIZE+LRSAVE(r1)	; get old link register value

	lg	r3,R3SAVE(r1)	; restore all registers that could contain
	lg	r4,R4SAVE(r1)	;  parameters to the routine that was bound.
	lg	r5,R5SAVE(r1)
	lg	r6,R6SAVE(r1)
	lg	r7,R7SAVE(r1)
	lg	r8,R8SAVE(r1)
	lg	r9,R9SAVE(r1)
	lg	r10,R10SAVE(r1)

	addi	r1,r1,STACK_SIZE; restore old stack pointer
	mtlr	r0		; restore link register

	bctr			; jump to the symbol`s address that was bound

#endif /* __ppc__ */











