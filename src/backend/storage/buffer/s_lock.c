/*-------------------------------------------------------------------------
 *
 * s_lock.c--
 *	  buffer manager interface routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/buffer/Attic/s_lock.c,v 1.6 1998/05/04 16:58:38 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>

#include "config.h"
#include "c.h"
#include "storage/s_lock.h"


/*
 * Each time we busy spin we select the next element of this array as the
 * number of microseconds to wait. This accomplishes pseudo random back-off.
 * Values are not critical and are weighted to the low end of the range. They
 * were chosen to work even with different select() timer resolutions on
 * different platforms.
 * note: total time to cycle through all 16 entries might be about .1 second.
 */
int			s_spincycle[S_NSPINCYCLE] =
{0, 0, 0, 1000, 5000, 0, 10000, 3000,
 0, 10000, 0, 15000, 9000, 21000, 6000, 30000
};


#if defined(S_LOCK_DEBUG)
/*
 * s_lock(lock) - take a spinlock
 * add intrumentation code to this and define S_LOCK_DEBUG
 * instead of hacking up the macro in s_lock.h
 */
void
s_lock(slock_t *lock, char *file, int line)
{
	int			spins = 0;

	while (TAS(lock))
	{
		struct timeval delay;

		delay.tv_sec = 0;
		delay.tv_usec = s_spincycle[spins++ % S_NSPINCYCLE];
		(void) select(0, NULL, NULL, NULL, &delay);
		if (spins > S_MAX_BUSY)
		{
			/* It's been well over a minute...  */
			s_lock_stuck(lock, file, line);
		}
	}
}
#endif /* S_LOCK_DEBUG */


/*
 * s_lock_stuck(lock) - deal with stuck spinlock
 */
void
s_lock_stuck(slock_t *lock, char *file, int line)
{
	fprintf(stderr,
			"\nFATAL: s_lock(%08x) at %s:%d, stuck spinlock. Aborting.\n",
			(unsigned int) lock, file, line);
	fprintf(stdout,
			"\nFATAL: s_lock(%08x) at %s:%d, stuck spinlock. Aborting.\n",
			(unsigned int) lock, file, line);
	abort();
}



/*
 * Various TAS implementations moved from s_lock.h to avoid redundant
 * definitions of the same routine.
 * RESOLVE: move this to tas.c. Alternatively get rid of tas.[cso] and fold
 * all that into this file.
 */


#if defined(linux)
/*************************************************************************
 * All the Linux flavors
 */


#if defined(__alpha__)
int
tas(slock_t *lock)
{
	slock_t		_res;

  __asm__("      ldq   $0, %0              \n\
                 bne   $0, already_set     \n\
                 ldq_l $0, %0	           \n\
                 bne   $0, already_set     \n\
                 or    $31, 1, $0          \n\
                 stq_c $0, %0	           \n\
                 beq   $0, stqc_fail       \n\
        success: bis   $31, $31, %1        \n\
                 mb		                   \n\
                 jmp   $31, end	           \n\
      stqc_fail: or    $31, 1, $0	       \n\
    already_set: bis   $0, $0, %1	       \n\
            end: nop      ": "=m"(*lock), "=r"(_res): :"0");

	return (_res != 0);
}
#endif /* __alpha__ */



#if defined(i386)
int
tas(slock_t *lock)
{
	slock_t		_res = 1;

  __asm__("lock; xchgb %0,%1": "=q"(_res), "=m"(*lock):"0"(0x1));
	return (_res != 0);
}
#endif /* i386 */



#if defined(sparc)

int
tas(slock_t *lock)
{
	slock_t		_res;
	slock_t    *tmplock = lock;

  __asm__("ldstub [%1], %0" \
  :			"=&r"(_res), "=r"(tmplock) \
  :			"1"(tmplock));
	return (_res != 0);
}

#endif /* sparc */



#if defined(PPC)

static int
tas_dummy()
{
	__asm__("				\n\
tas:						\n\
			lwarx	5,0,3	\n\
			cmpwi	5,0		\n\
			bne		fail	\n\
			addi	5,5,1	\n\
        	stwcx.  5,0,3	\n\
     		beq		success	\n\
fail:		li		3,1		\n\
			blr				\n\
success:					\n\
			li 3,0			\n\
        	blr				\n\
	");
}

#endif /* PPC */



#else /* defined(linux) */
/***************************************************************************
 * All Non-Linux
 */



#if defined(sun3)
static void
tas_dummy()						/* really means: extern int tas(slock_t *lock); */
{
	asm("LLA0:");
	asm("   .data");
	asm("   .text");
	asm("|#PROC# 04");
	asm("   .globl  _tas");
	asm("_tas:");
	asm("|#PROLOGUE# 1");
	asm("   movel   sp@(0x4),a0");
	asm("   tas a0@");
	asm("   beq LLA1");
	asm("   moveq   #-128,d0");
	asm("   rts");
	asm("LLA1:");
	asm("   moveq   #0,d0");
	asm("   rts");
	asm("   .data");
}
#endif /* sun3 */



#if defined(NEED_SPARC_TAS_ASM)
/*
 * bsd and bsdi sparc machines
 */

/* if we're using -ansi w/ gcc, use __asm__ instead of asm */
#if defined(__STRICT_ANSI__)
#define asm(x)	__asm__(x)
#endif /* __STRICT_ANSI__ */

static void
tas_dummy()						/* really means: extern int tas(slock_t *lock); */
{
	asm(".seg \"data\"");
	asm(".seg \"text\"");
	asm("_tas:");

	/*
	 * Sparc atomic test and set (sparc calls it "atomic load-store")
	 */
	asm("ldstub [%r8], %r8");

	asm("retl");
	asm("nop");
}

#endif /* NEED_SPARC_TAS_ASM */




#if defined(NEED_VAX_TAS_ASM)
/*
 * VAXen -- even multiprocessor ones
 * (thanks to Tom Ivar Helbekkmo)
 */
typedef unsigned char slock_t;

int
tas(slock_t *lock)
{
	register	ret;

	asm("	movl $1, r0
		bbssi $0, (%1), 1f
		clrl r0
  1:	movl r0, %0 "
  :		"=r"(ret)				/* return value, in register */
  :		"r"(lock)				/* argument, 'lock pointer', in register */
  :		"r0");					/* inline code uses this register */

	return ret;
}

#endif /* NEED_VAX_TAS_ASM */



#if defined(NEED_I386_TAS_ASM)
/*
 * i386 based things
 */

#if defined(USE_UNIVEL_CC)
asm int
tas(slock_t *s_lock)
{
	%lab locked;
/* Upon entry, %eax will contain the pointer to the lock byte */
	pushl % ebx
	xchgl % eax, %ebx
	xor % eax, %eax
	movb $255, %al
	lock
	xchgb % al, (%ebx)
	popl % ebx
}


#else /* USE_UNIVEL_CC */

int
tas(slock_t *lock)
{
	slock_t		_res = 1;

  __asm__("lock; xchgb %0,%1": "=q"(_res), "=m"(*lock):"0"(0x1));
	return (_res != 0);
}

#endif /* USE_UNIVEL_CC */

#endif /* NEED_I386_TAS_ASM */


#endif /* linux */


#if defined(S_LOCK_TEST)

slock_t		test_lock;

void
main()
{
	S_INIT_LOCK(&test_lock);

	if (!S_LOCK_FREE(&test_lock))
	{
		printf("S_LOCK_TEST: failed, lock not initialized.\n");
		exit(1);
	}

	S_LOCK(&test_lock);

	if (S_LOCK_FREE(&test_lock))
	{
		printf("S_LOCK_TEST: failed, lock not locked\n");
		exit(2);
	}

	printf("S_LOCK_TEST: this will hang for a few minutes and then abort\n");
	printf("             with a 'stuck spinlock' message if S_LOCK()\n");
	printf("             and TAS() are working.\n");
	S_LOCK(&test_lock);

	printf("S_LOCK_TEST: failed, lock not locked~\n");
	exit(3);

}

#endif /* S_LOCK_TEST */
