/*
 * Copyright (c) 2010 Apple Inc. All rights reserved.
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
 
 
 
#if __x86_64__
	// returns address of TLV in %rax, all other registers preserved
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
	subq	$592,%rsp
	movq	%rdi,-8(%rbp)
	movq	%rsi,-16(%rbp)
	movq	%rdx,-24(%rbp)
	movq	%rcx,-32(%rbp)
	movq	%r8,-40(%rbp)
	movq	%r9,-48(%rbp)
	movq	%r10,-56(%rbp)
	movq	%r11,-64(%rbp)
	fxsave  -592(%rbp)
	movq	8(%rdi),%rdi			// get key from descriptor
	call	_tlv_allocate_and_initialize_for_key  
	fxrstor -592(%rbp)  
	movq	-64(%rbp),%r11
	movq	-56(%rbp),%r10
	movq	-48(%rbp),%r9
	movq	-40(%rbp),%r8
	movq	-32(%rbp),%rcx
	movq	-24(%rbp),%rdx
	movq	-16(%rbp),%rsi
	movq	-8(%rbp),%rdi
 	addq	16(%rdi),%rax			// result = buffer + offset
	addq	$592,%rsp
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


