/*
 * Copyright (c) 1999-2008 Apple Inc. All rights reserved.
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



	// Hack to make _offset_to_dyld_all_image_infos work
	// Without this local symbol, assembler will error out about in subtraction expression
	// The real _dyld_all_image_infos (non-weak) _dyld_all_image_infos is defined in dyld_gdb.o
	// and the linker with throw this one away and use the real one instead.
	.section __DATA,__datacoal_nt,coalesced
	.globl _dyld_all_image_infos
	.weak_definition _dyld_all_image_infos
_dyld_all_image_infos:	.long 0



	.globl __dyld_start

#ifdef __i386__
	.data
__dyld_start_static_picbase: 
	.long   L__dyld_start_picbase
Lmh:	.long	___dso_handle

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
	nop
	nop
	nop
_offset_to_dyld_all_image_infos:
	.long	_dyld_all_image_infos - . + 0x1010 
	.long	0
	# space for future stable entry points
	.space	16

	
	
	.text
	.align	4, 0x90
	.globl __dyld_start
__dyld_start:
	pushl	$0		# push a zero for debugger end of frames marker
	movl	%esp,%ebp	# pointer to base of kernel frame
	andl    $-16,%esp       # force SSE alignment
	
	# call dyldbootstrap::start(app_mh, argc, argv, slide, dyld_mh)
	subl	$12,%esp
	call    L__dyld_start_picbase
L__dyld_start_picbase:	
	popl	%ebx		# set %ebx to runtime value of picbase
   	movl	Lmh-L__dyld_start_picbase(%ebx), %ecx # ecx = prefered load address
   	movl	__dyld_start_static_picbase-L__dyld_start_picbase(%ebx), %eax
	subl    %eax, %ebx      # ebx = slide = L__dyld_start_picbase - [__dyld_start_static_picbase]
	addl	%ebx, %ecx	# ecx = actual load address
	pushl   %ecx		# param5 = actual load address
	pushl   %ebx		# param4 = slide
	lea     12(%ebp),%ebx	
	pushl   %ebx		# param3 = argv
	movl	8(%ebp),%ebx	
	pushl   %ebx		# param2 = argc
	movl	4(%ebp),%ebx	
	pushl   %ebx		# param1 = mh
	call	__ZN13dyldbootstrap5startEPK12macho_headeriPPKclS2_	

    	# clean up stack and jump to result
	movl	%ebp,%esp	# restore the unaligned stack pointer
	addl	$8,%esp		# remove the mh argument, and debugger end
				#  frame marker
	movl	$0,%ebp		# restore ebp back to zero
	jmp	*%eax		# jump to the entry point


	.globl dyld_stub_binding_helper
dyld_stub_binding_helper:
	hlt
L_end:
#endif /* __i386__ */



#if __x86_64__
	.data
	.align 3
__dyld_start_static: 
	.quad   __dyld_start

# stable entry points into dyld
	.text
	.align 2
	.globl	_stub_binding_helper
_stub_binding_helper:
	jmp	_stub_binding_helper_interface
	nop
	nop
	nop
	.globl	_dyld_func_lookup
_dyld_func_lookup:
	jmp	__Z18lookupDyldFunctionPKcPm
	nop
	nop
	nop
_offset_to_dyld_all_image_infos:
	.long	_dyld_all_image_infos - . + 0x1010 
	.long	0
	# space for future stable entry points
	.space	16


	.text
	.align 2,0x90
	.globl __dyld_start
__dyld_start:
	pushq	$0		# push a zero for debugger end of frames marker
	movq	%rsp,%rbp	# pointer to base of kernel frame
	andq    $-16,%rsp       # force SSE alignment
	
	# call dyldbootstrap::start(app_mh, argc, argv, slide, dyld_mh)
	movq	8(%rbp),%rdi	# param1 = mh into %rdi
	movl	16(%rbp),%esi	# param2 = argc into %esi
	leaq	24(%rbp),%rdx	# param3 = &argv[0] into %rdx
	movq	__dyld_start_static(%rip), %r8
	leaq	__dyld_start(%rip), %rcx
	subq	 %r8, %rcx	# param4 = slide into %rcx
	leaq	___dso_handle(%rip),%r8 # param5 = dyldsMachHeader
	call	__ZN13dyldbootstrap5startEPK12macho_headeriPPKclS2_	

    	# clean up stack and jump to result
	movq	%rbp,%rsp	# restore the unaligned stack pointer
	addq	$16,%rsp	# remove the mh argument, and debugger end frame marker
	movq	$0,%rbp		# restore ebp back to zero
	jmp	*%rax		# jump to the entry point
	
#endif /* __x86_64__ */


#if __ppc__ || __ppc64__
#include <architecture/ppc/mode_independent_asm.h>

	.data
	.align 2
__dyld_start_static_picbase: 
	.g_long	    L__dyld_start_picbase
Lmh:	.g_long	    ___dso_handle

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
	nop 
_offset_to_dyld_all_image_infos:
	.long	_dyld_all_image_infos - . + 0x1010 
	.long	0
	# space for future stable entry points
	.space	16
	
	
	.text
	.align 2
__dyld_start:
	mr	r26,r1		; save original stack pointer into r26
	subi	r1,r1,GPR_BYTES	; make space for linkage
	clrrgi	r1,r1,5		; align to 32 bytes
	addi	r0,0,0		; load 0 into r0
	stg	r0,0(r1)	; terminate initial stack frame
	stgu	r1,-SF_MINSIZE(r1); allocate minimal stack frame
		
	# call dyldbootstrap::start(app_mh, argc, argv, slide, dyld_mh)
	lg	r3,L_mh_offset(r26)	; r3 = mach_header
	lwz	r4,L_argc_offset(r26)	; r4 = argc (int == 4 bytes)
	addi	r5,r26,L_argv_offset	; r5 = argv
	bcl	20,31,L__dyld_start_picbase	
L__dyld_start_picbase:	
	mflr	r31		; put address of L__dyld_start_picbase in r31
	addis   r6,r31,ha16(__dyld_start_static_picbase-L__dyld_start_picbase)
	lg      r6,lo16(__dyld_start_static_picbase-L__dyld_start_picbase)(r6)
	subf    r6,r6,r31       ; r6 = slide
	addis   r7,r31,ha16(Lmh-L__dyld_start_picbase)
	lg      r7,lo16(Lmh-L__dyld_start_picbase)(r7)
	add	r7,r6,r7	; r7 = dyld_mh
	bl	__ZN13dyldbootstrap5startEPK12macho_headeriPPKclS2_	
	
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

#if __arm__
	.data
	.align 2
__dyld_start_static_picbase: 
	.long	L__dyld_start_picbase

	.text
	.align 2
	.globl	_stub_binding_helper
_stub_binding_helper:
	b	_stub_binding_helper_interface
	nop 
	
	.globl	_dyld_func_lookup
_dyld_func_lookup:
	b       _branch_to_lookupDyldFunction
	nop
	
_offset_to_dyld_all_image_infos:
	.long	_dyld_all_image_infos - . + 0x1010 
	.long	0
	# space for future stable entry points
	.space	16
    
    
	// Hack to make ___dso_handle work
	// Without this local symbol, assembler will error out about in subtraction expression
	// The real ___dso_handle (non-weak) sythesized by the linker
	// Since this one is weak, the linker will throw this one away and use the real one instead.
	.data
	.globl ___dso_handle
	.weak_definition ___dso_handle
___dso_handle:	.long 0

	.text
	.align 2
__dyld_start:
	mov	r8, sp		// save stack pointer
	sub	sp, #8		// make room for outgoing dyld_mh parameter
	bic     sp, sp, #7	// force 8-byte alignment

	// call dyldbootstrap::start(app_mh, argc, argv, slide, dyld_mh)

	ldr	r3, L__dyld_start_picbase_ptr
L__dyld_start_picbase:
	sub	r0, pc, #8	// load actual PC
	ldr	r3, [r0, r3]	// load expected PC
	sub	r3, r0, r3	// r3 = slide

	ldr	r0, [r8]	// r0 = mach_header
	ldr	r1, [r8, #4]	// r1 = argc
	add	r2, r8, #8	// r2 = argv

	ldr	r4, Lmh
L3:	add	r4, r4, pc	// r4 = dyld_mh
	str	r4, [sp, #0]
       
	bl	__ZN13dyldbootstrap5startEPK12macho_headeriPPKclS2_
       
	// clean up stack and jump to result
	add	sp, r8, #4	// remove the mach_header argument.
	bx	r0		// jump to the program's entry point


	.align 2
L__dyld_start_picbase_ptr:
	.long	__dyld_start_static_picbase-L__dyld_start_picbase
Lmh:	.long   ___dso_handle-L3-8
	
	.text
	.align 2
_branch_to_lookupDyldFunction:
	// arm has no "bx label" instruction, so need this island in case lookupDyldFunction() is in thumb
	ldr ip, L2
L1:	ldr pc, [pc, ip]
L2:	.long   _lookupDyldFunction_ptr-8-L1
       
 	.data
	.align 2
_lookupDyldFunction_ptr:
	.long	__Z18lookupDyldFunctionPKcPm
	
      
	.text
	.globl dyld_stub_binding_helper
dyld_stub_binding_helper:
	trap

L_end:
#endif /* __arm__ */

/*
 * dyld calls this function to terminate a process.
 * It has a label so that CrashReporter can distinguish this
 * termination from a random crash.  rdar://problem/4764143
 */
	.text
	.align 2
	.globl	_dyld_fatal_error
_dyld_fatal_error:
#if __ppc__ || __ppc64__ || __arm__
    trap
    nop
#elif __x86_64__ || __i386__
    int3
    nop
#else
    #error unknown architecture
#endif

#if __arm__
	// work around for:  <rdar://problem/6530727> gdb-1109: notifier in dyld does not work if it is in thumb
 	.text
	.align 2
	.globl	_gdb_image_notifier
	.private_extern _gdb_image_notifier
_gdb_image_notifier:
	bx  lr
#endif




