!=======================================================================
! solaris_sparc.s -- compare and swap for solaris_sparc
!=======================================================================

! Fortunately the Sun compiler can process cpp conditionals with -P

! '/' is the comment for x86, while '!' is the comment for Sparc

#if defined(__sparcv9) || defined(__sparc)

	.section        ".text"
	.align  8
	.skip   24
	.align  4

	.global pg_atomic_cas
pg_atomic_cas:
	
	cas     [%o0],%o2,%o1
	mov     %o1,%o0
	retl
	nop
	.type   pg_atomic_cas,2
	.size   pg_atomic_cas,(.-pg_atomic_cas)
#endif
