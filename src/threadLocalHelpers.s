/*
 * Copyright (c) 2010-2013 Apple Inc. All rights reserved.
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
 
#include <System/machine/cpu_capabilities.h>

// bool save_xxm = (*((uint32_t*)_COMM_PAGE_CPU_CAPABILITIES) & kHasAVX1_0) != 0;

#if __x86_64__
	// returns address of TLV in %rax, all other registers preserved
	#define FP_SAVE			-192
	#define VECTOR_SAVE		-704
	#define STACK_SIZE		704

	.globl _tlv_get_addr
	.private_extern _tlv_get_addr
_tlv_get_addr:
	movq	8(%rdi),%rax			// get key from descriptor
	movq	%gs:0x0(,%rax,8),%rax	// get thread value
	testq	%rax,%rax				// if NULL, lazily allocate
	je		LlazyAllocate
	addq	16(%rdi),%rax			// add offset from descriptor
	ret
LlazyAllocate:
	pushq	%rbp
	movq	%rsp, %rbp
	subq	$STACK_SIZE,%rsp				// fxsave uses 512 bytes of store, xsave uses 
	movq	%rdi,-8(%rbp)
	movq	%rsi,-16(%rbp)
	movq	%rdx,-24(%rbp)
	movq	%rcx,-32(%rbp)
	movq	%r8,-40(%rbp)
	movq	%r9,-48(%rbp)
	movq	%r10,-56(%rbp)
	movq	%r11,-64(%rbp)
	fnsave	FP_SAVE(%rbp)
	movq    $(_COMM_PAGE_CPU_CAPABILITIES), %rcx
	movl    (%rcx), %ecx
	testl   $kHasAVX1_0, %ecx
	jne     L2
	movdqa	%xmm0, VECTOR_SAVE+0x00(%rbp)
	movdqa	%xmm1, VECTOR_SAVE+0x10(%rbp)
	movdqa	%xmm2, VECTOR_SAVE+0x20(%rbp)
	movdqa	%xmm3, VECTOR_SAVE+0x30(%rbp)
	movdqa	%xmm4, VECTOR_SAVE+0x40(%rbp)
	movdqa	%xmm5, VECTOR_SAVE+0x50(%rbp)
	movdqa	%xmm6, VECTOR_SAVE+0x60(%rbp)
	movdqa	%xmm7, VECTOR_SAVE+0x70(%rbp)
	movdqa	%xmm8, VECTOR_SAVE+0x80(%rbp)
	movdqa	%xmm9, VECTOR_SAVE+0x90(%rbp)
	movdqa	%xmm10,VECTOR_SAVE+0xA0(%rbp)
	movdqa	%xmm11,VECTOR_SAVE+0xB0(%rbp)
	movdqa	%xmm12,VECTOR_SAVE+0xC0(%rbp)
	movdqa	%xmm13,VECTOR_SAVE+0xD0(%rbp)
	movdqa	%xmm14,VECTOR_SAVE+0xE0(%rbp)
	movdqa	%xmm15,VECTOR_SAVE+0xF0(%rbp)
	jmp		L3
L2:	vmovdqu	%ymm0, VECTOR_SAVE+0x00(%rbp)
	vmovdqu	%ymm1, VECTOR_SAVE+0x20(%rbp)
	vmovdqu	%ymm2, VECTOR_SAVE+0x40(%rbp)
	vmovdqu	%ymm3, VECTOR_SAVE+0x60(%rbp)
	vmovdqu	%ymm4, VECTOR_SAVE+0x80(%rbp)
	vmovdqu	%ymm5, VECTOR_SAVE+0xA0(%rbp)
	vmovdqu	%ymm6, VECTOR_SAVE+0xC0(%rbp)
	vmovdqu	%ymm7, VECTOR_SAVE+0xE0(%rbp)
	vmovdqu	%ymm8, VECTOR_SAVE+0x100(%rbp)
	vmovdqu	%ymm9, VECTOR_SAVE+0x120(%rbp)
	vmovdqu	%ymm10,VECTOR_SAVE+0x140(%rbp)
	vmovdqu	%ymm11,VECTOR_SAVE+0x160(%rbp)
	vmovdqu	%ymm12,VECTOR_SAVE+0x180(%rbp)
	vmovdqu	%ymm13,VECTOR_SAVE+0x1A0(%rbp)
	vmovdqu	%ymm14,VECTOR_SAVE+0x1C0(%rbp)
	vmovdqu	%ymm15,VECTOR_SAVE+0x1E0(%rbp)
L3:	movq	-32(%rbp),%rcx
	movq	8(%rdi),%rdi			// get key from descriptor
	call	_tlv_allocate_and_initialize_for_key

	frstor	FP_SAVE(%rbp)
	movq    $(_COMM_PAGE_CPU_CAPABILITIES), %rcx
	movl    (%rcx), %ecx
	testl   $kHasAVX1_0, %ecx
	jne     L4
	movdqa	VECTOR_SAVE+0x00(%rbp), %xmm0
	movdqa	VECTOR_SAVE+0x10(%rbp), %xmm1
	movdqa	VECTOR_SAVE+0x20(%rbp), %xmm2
	movdqa	VECTOR_SAVE+0x30(%rbp), %xmm3
	movdqa	VECTOR_SAVE+0x40(%rbp), %xmm4
	movdqa	VECTOR_SAVE+0x50(%rbp), %xmm5
	movdqa	VECTOR_SAVE+0x60(%rbp), %xmm6
	movdqa	VECTOR_SAVE+0x70(%rbp), %xmm7
	movdqa	VECTOR_SAVE+0x80(%rbp), %xmm8
	movdqa	VECTOR_SAVE+0x90(%rbp), %xmm9
	movdqa	VECTOR_SAVE+0xA0(%rbp), %xmm10
	movdqa	VECTOR_SAVE+0xB0(%rbp), %xmm11
	movdqa	VECTOR_SAVE+0xC0(%rbp), %xmm12
	movdqa	VECTOR_SAVE+0xD0(%rbp), %xmm13
	movdqa	VECTOR_SAVE+0xE0(%rbp), %xmm14
	movdqa	VECTOR_SAVE+0xF0(%rbp), %xmm15
	jmp		L5
L4: vmovdqu	VECTOR_SAVE+0x00(%rbp),  %ymm0
	vmovdqu VECTOR_SAVE+0x20(%rbp),  %ymm1
	vmovdqu	VECTOR_SAVE+0x40(%rbp),  %ymm2
	vmovdqu	VECTOR_SAVE+0x60(%rbp),  %ymm3
	vmovdqu	VECTOR_SAVE+0x80(%rbp),  %ymm4
	vmovdqu	VECTOR_SAVE+0xA0(%rbp),  %ymm5
	vmovdqu	VECTOR_SAVE+0xC0(%rbp),  %ymm6
	vmovdqu	VECTOR_SAVE+0xE0(%rbp),  %ymm7
	vmovdqu	VECTOR_SAVE+0x100(%rbp), %ymm8
	vmovdqu	VECTOR_SAVE+0x120(%rbp), %ymm9
	vmovdqu VECTOR_SAVE+0x140(%rbp), %ymm10
	vmovdqu	VECTOR_SAVE+0x160(%rbp), %ymm11
	vmovdqu	VECTOR_SAVE+0x180(%rbp), %ymm12
	vmovdqu	VECTOR_SAVE+0x1A0(%rbp), %ymm13
	vmovdqu	VECTOR_SAVE+0x1C0(%rbp), %ymm14
	vmovdqu	VECTOR_SAVE+0x1E0(%rbp), %ymm15
L5:	movq	-64(%rbp),%r11
	movq	-56(%rbp),%r10
	movq	-48(%rbp),%r9
	movq	-40(%rbp),%r8
	movq	-32(%rbp),%rcx
	movq	-24(%rbp),%rdx
	movq	-16(%rbp),%rsi
	movq	-8(%rbp),%rdi
 	addq	16(%rdi),%rax			// result = buffer + offset
	addq	$STACK_SIZE,%rsp
	popq	%rbp
	ret
#endif



#if __i386__
	// returns address of TLV in %eax, all other registers (except %ecx) preserved
	.globl _tlv_get_addr
	.private_extern _tlv_get_addr
_tlv_get_addr:
	movl	4(%eax),%ecx			// get key from descriptor
	movl	%gs:0x0(,%ecx,4),%ecx	// get thread value
	testl	%ecx,%ecx				// if NULL, lazily allocate
	je		LlazyAllocate
	movl	8(%eax),%eax			// add offset from descriptor
	addl	%ecx,%eax
	ret
LlazyAllocate:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%edx					// save edx
	subl	$548,%esp
	movl	%eax,-8(%ebp)		    // save descriptor
	lea		-528(%ebp),%ecx		    // get 512 byte buffer in frame
	and		$-16, %ecx			    // 16-byte align buffer for fxsave
	fxsave  (%ecx)
	movl	4(%eax),%ecx			// get key from descriptor
	movl	%ecx,(%esp)	            // push key parameter, also leaves stack aligned properly
	call	_tlv_allocate_and_initialize_for_key
	movl	-8(%ebp),%ecx			// get descriptor
	movl	8(%ecx),%ecx			// get offset from descriptor
	addl	%ecx,%eax				// add offset to buffer
	lea 	-528(%ebp),%ecx
	and 	$-16, %ecx              // 16-byte align buffer for fxrstor
	fxrstor (%ecx)
	addl	$548,%esp
	popl	%edx	                // restore edx
	popl	%ebp
	ret
#endif


#if 0
#if __arm__
	// returns address of TLV in r0, all other registers preserved
	.globl _tlv_get_addr
	.private_extern _tlv_get_addr
_tlv_get_addr:
	push	{r1,r2,r3,r7,lr}				
	mov		r7,r0							// save descriptor in r7
	ldr		r0, [r7, #4]					// get key from descriptor
	bl		_pthread_getspecific			// get thread value
	cmp		r0, #0
	bne		L2								// if NULL, lazily allocate
	ldr		r0, [r7, #4]					// get key from descriptor
	bl		_tlv_allocate_and_initialize_for_key
L2:	ldr		r1, [r7, #8]					// get offset from descriptor
	add		r0, r1, r0						// add offset into allocation block
	pop		{r1,r2,r3,r7,pc}
#endif
#endif

	.subsections_via_symbols
	

