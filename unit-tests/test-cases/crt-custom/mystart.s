


	.text
	.globl _mystart
_mystart:
#if __i386__ 
	movl	$2, _flag
	jmp		start
#elif __x86_64__
	movl	$2, _flag(%rip)
	jmp		start
#elif __ppc__ || __ppc64__
	li r0,2
	lis r2,ha16(_flag)
	stw r0,lo16(_flag)(r2)
	b		start
#endif


