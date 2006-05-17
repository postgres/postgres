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
	
	! "cas" only works on sparcv9 chips, and requies a compiler
	! that is targeting sparcv9.  It will fail on a compiler
	! targeting sparcv8, and of course will not be understood
	! by a sparcv8 CPU.  If this fails on existing Solaris
	! systems, we need to use a !defined(__sparcv9) test
	! to fall back to the old "ldstub" call for sparcv8 compiles.
	! gcc continues to use "ldstub" because there is no indication
	! which sparc version it is targeting.
	!
	! There actually is a trick for embedding "cas" for a compiler
	! that is targeting sparcv8:
	!
	!   http://cvs.opensolaris.org/source/xref/on/usr/src/lib/libc/sparc/threads/sparc.il
	!
	! This might work for sparc8:
	! ldstub [%o0],%o1	! moves only a byte

	cas     [%o0],%o2,%o1
	mov     %o1,%o0
	retl
	nop
	.type   pg_atomic_cas,2
	.size   pg_atomic_cas,(.-pg_atomic_cas)
#endif
