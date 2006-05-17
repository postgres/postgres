/=======================================================================
/ solaris_i386.s -- compare and swap for solaris_i386
/=======================================================================

/ Fortunately the Sun compiler can process cpp conditionals with -P

/ '/' is the comment for x86, while '!' is the comment for Sparc

	.file   "tas.s"

#if defined(__amd64)
	.code64
#endif

	.globl pg_atomic_cas
	.type pg_atomic_cas, @function

	.section .text, "ax"
	.align 16

pg_atomic_cas:
#if defined(__amd64)
	movl       %edx,%eax
	lock
	cmpxchgl   %esi,(%rdi)
#else
	movl    4(%esp), %edx
	movl    8(%esp), %ecx
	movl    12(%esp), %eax
	lock
	cmpxchgl %ecx, (%edx)
#endif
	ret
	.size pg_atomic_cas, . - pg_atomic_cas
