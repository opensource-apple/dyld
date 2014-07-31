
		# _a is zerofill global TLV
		.tbss _a$tlv$init,4,2

		# _b is an initialized global TLV
		.tdata
_b$tlv$init:    
		.long   5


#if __x86_64__

		# _a is global TLV
		.tlv
		.globl _a
_a:		.quad	__tlv_bootstrap
		.quad	0
		.quad	_a$tlv$init

		# _b is a global TLV
		.tlv
		.globl _b
_b:		.quad	__tlv_bootstrap
		.quad	0
		.quad	_b$tlv$init

		# _myinit sets up TLV content
		.thread_init_func
		.quad	_myinit


	.text
	.globl	_get_a
_get_a:
	pushq	%rbp
	movq	%rsp, %rbp
	movq	_a@TLVP(%rip), %rdi
	call	*(%rdi)
	popq	%rbp
	ret

	.globl	_get_b
_get_b:
	pushq	%rbp
	movq	%rsp, %rbp
	movq	_b@TLVP(%rip), %rdi
	call	*(%rdi)
	popq	%rbp
	ret

#endif

#if __i386__

		# _a is global TLV
		.tlv
		.globl _a
_a:		.long	__tlv_bootstrap
		.long	0
		.long	_a$tlv$init

		# _b is a global TLV
		.tlv
		.globl _b
_b:		.long	__tlv_bootstrap
		.long	0
		.long	_b$tlv$init

		# _myinit sets up TLV content
		.thread_init_func
		.long	_myinit


	.text
	.globl	_get_a
_get_a:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$8, %esp
	movl	_a@TLVP, %eax
	call	*(%eax)    
	movl	%ebp, %esp
	popl	%ebp
	ret

	.globl	_get_b
_get_b:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$8, %esp
	movl	_b@TLVP, %eax
	call	*(%eax)    
	movl	%ebp, %esp
	popl	%ebp
	ret

#endif

.subsections_via_symbols
