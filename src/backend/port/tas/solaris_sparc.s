	!!
	!! $PostgreSQL: pgsql/src/backend/port/tas/solaris_sparc.s,v 1.2 2003/11/29 19:51:54 pgsql Exp $
	!!
	!! this would be a piece of inlined assembler but it appears
	!! to be easier to just write the assembler than to try to 
	!! figure out how to make sure that in/out registers are kept
	!! straight in the asm's.
	!!
	.file	"tas.c"
.section	".text"
	.align 4
	.global tas
	.type	 tas,#function
	.proc	04
tas:
	!!
	!! this is a leaf procedure - no need to save windows and 
	!! diddle the CWP.
	!!
	!#PROLOGUE# 0
	!#PROLOGUE# 1
	
	!!
	!! write 0xFF into the lock address, saving the old value in %o0.
	!! this is an atomic action, even on multiprocessors.
	!!
	ldstub [%o0],%o0
	
	!!
	!! if it was already set when we set it, somebody else already
	!! owned the lock -- return 1.
	!!
	cmp %o0,0
	bne .LL2
	mov 1,%o0
		
	!!
	!! otherwise, it was clear and we now own the lock -- return 0.
	!!
	mov 0,%o0
.LL2:
	!!
	!! this is a leaf procedure - no need to restore windows and 
	!! diddle the CWP.
	!!
	retl
	nop
.LLfe1:
	.size	 tas,.LLfe1-tas
	.ident	"GCC: (GNU) 2.5.8"
