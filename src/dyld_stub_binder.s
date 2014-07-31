/*
 * Copyright (c) 2008 Apple Inc. All rights reserved.
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

#define MH_PARAM_OUT			0
#define LP_PARAM_OUT			4
#define XMMM0_SAVE			16	/* 16-byte align */
#define XMMM1_SAVE			32
#define XMMM2_SAVE			48
#define XMMM3_SAVE			64
#define EAX_SAVE			84
#define ECX_SAVE			88
#define EDX_SAVE			92
#define LP_LOCAL			96
#define MH_LOCAL			100
#define STACK_SIZE			100	/* must be 4 mod 16 so that stack winds up 16-byte aliged  */
#define LP_OLD_BP_SAVE			104

/*    
 * sp+4	lazy binding info offset
 * sp+0	address of ImageLoader cache
 */
    .text
    .align 4,0x90
    .globl dyld_stub_binder
    .globl _misaligned_stack_error
dyld_stub_binder:
	subl		$STACK_SIZE,%esp	    # makes stack 16-byte aligned
	movl		%eax,EAX_SAVE(%esp)	
	movl		LP_OLD_BP_SAVE(%esp),%eax   # get lazy-pointer meta-parameter
	movl		%eax,LP_LOCAL(%esp)	
	movl		%ebp,LP_OLD_BP_SAVE(%esp)   # store epb back chain
	movl		%esp,%ebp		    # set epb to be this frame
	add		$LP_OLD_BP_SAVE,%ebp
	movl		%ecx,ECX_SAVE(%esp)
	movl		%edx,EDX_SAVE(%esp)
	.align 0,0x90
_misaligned_stack_error_:
	movdqa		%xmm0,XMMM0_SAVE(%esp)
	movdqa		%xmm1,XMMM1_SAVE(%esp)
	movdqa		%xmm2,XMMM2_SAVE(%esp)
	movdqa		%xmm3,XMMM3_SAVE(%esp)
dyld_stub_binder_:
	movl		MH_LOCAL(%esp),%eax	# call dyld::fastBindLazySymbol(loadercache, lazyinfo)
	movl		%eax,MH_PARAM_OUT(%esp)
	movl		LP_LOCAL(%esp),%eax
	movl		%eax,LP_PARAM_OUT(%esp)
	call		__Z21_dyld_fast_stub_entryPvl
	movdqa		XMMM0_SAVE(%esp),%xmm0	# restore registers
	movdqa		XMMM1_SAVE(%esp),%xmm1
	movdqa		XMMM2_SAVE(%esp),%xmm2
	movdqa		XMMM3_SAVE(%esp),%xmm3
	movl		ECX_SAVE(%esp),%ecx
	movl		EDX_SAVE(%esp),%edx
	movl		%eax,%ebp		# move target address to epb
	movl		EAX_SAVE(%esp),%eax	# restore eax
	addl		$STACK_SIZE+4,%esp	# cut back stack
	xchg		%ebp, (%esp)		# restore ebp and set target to top of stack
	ret					# jump to target
    

#endif /* __i386__ */


#if __x86_64__

#define MH_PARAM_BP			8
#define LP_PARAM_BP			16

#define RDI_SAVE			0
#define RSI_SAVE			8
#define RDX_SAVE			16
#define RCX_SAVE			24
#define R8_SAVE				32
#define R9_SAVE				40
#define RAX_SAVE			48
#define XMMM0_SAVE			64    /* 16-byte align */
#define XMMM1_SAVE			80
#define XMMM2_SAVE			96
#define XMMM3_SAVE			112
#define XMMM4_SAVE			128
#define XMMM5_SAVE			144
#define XMMM6_SAVE			160
#define XMMM7_SAVE			176
#define STACK_SIZE			192 /*  (XMMM7_SAVE+16) must be 16 byte aligned too */
    

 /*    
 * sp+4	lazy binding info offset
 * sp+0	address of ImageLoader cache
 */
    .align 2,0x90
    .globl dyld_stub_binder
dyld_stub_binder:
	pushq		%rbp
	movq		%rsp,%rbp
	subq		$STACK_SIZE,%rsp	# at this point stack is 16-byte aligned because two meta-parameters where pushed
	movq		%rdi,RDI_SAVE(%rsp)	# save registers that might be used as parameters
	movq		%rsi,RSI_SAVE(%rsp)
	movq		%rdx,RDX_SAVE(%rsp)
	movq		%rcx,RCX_SAVE(%rsp)
	movq		%r8,R8_SAVE(%rsp)
	movq		%r9,R9_SAVE(%rsp)
	movq		%rax,RAX_SAVE(%rsp)
	movdqa		%xmm0,XMMM0_SAVE(%rsp)
	movdqa		%xmm1,XMMM1_SAVE(%rsp)
	movdqa		%xmm2,XMMM2_SAVE(%rsp)
	movdqa		%xmm3,XMMM3_SAVE(%rsp)
	movdqa		%xmm4,XMMM4_SAVE(%rsp)
	movdqa		%xmm5,XMMM5_SAVE(%rsp)
	movdqa		%xmm6,XMMM6_SAVE(%rsp)
	movdqa		%xmm7,XMMM7_SAVE(%rsp)
	movq		MH_PARAM_BP(%rbp),%rdi	# call fastBindLazySymbol(loadercache, lazyinfo)
	movq		LP_PARAM_BP(%rbp),%rsi
	call		__Z21_dyld_fast_stub_entryPvl
	movq		%rax,%r11		# save target
	movdqa		XMMM0_SAVE(%rsp),%xmm0	# restore registers
	movdqa		XMMM1_SAVE(%rsp),%xmm1
	movdqa		XMMM2_SAVE(%rsp),%xmm2
	movdqa		XMMM3_SAVE(%rsp),%xmm3
	movdqa		XMMM4_SAVE(%rsp),%xmm4
	movdqa		XMMM5_SAVE(%rsp),%xmm5
	movdqa		XMMM6_SAVE(%rsp),%xmm6
	movdqa		XMMM7_SAVE(%rsp),%xmm7
	movq		RDI_SAVE(%rsp),%rdi
	movq		RSI_SAVE(%rsp),%rsi
	movq		RDX_SAVE(%rsp),%rdx
	movq		RCX_SAVE(%rsp),%rcx
	movq		R8_SAVE(%rsp),%r8
	movq		R9_SAVE(%rsp),%r9
	movq		RAX_SAVE(%rsp),%rax
	addq		$STACK_SIZE,%rsp
	popq		%rbp
	addq		$16,%rsp		# remove meta-parameters
	jmp		*%r11			# jmp to target

#endif



