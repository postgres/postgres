/*-------------------------------------------------------------------------
 *
 * s_lock.c--
 *	  buffer manager interface routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/buffer/Attic/s_lock.c,v 1.11 1998/09/18 05:36:23 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <sys/time.h>

#include "config.h"
#include "c.h"
#include "storage/s_lock.h"


/*
 * Each time we busy spin we select the next element of this array as the
 * number of microseconds to wait. This accomplishes pseudo random back-off.
 * Values are not critical but 10 milliseconds is a common platform
 * granularity.
 * note: total time to cycle through all 16 entries might be about .07 sec.
 */
#define S_NSPINCYCLE	20
#define S_MAX_BUSY		500 * S_NSPINCYCLE

int			s_spincycle[S_NSPINCYCLE] =
{0, 0, 0, 0, 10000, 0, 0, 0, 10000, 0,
	0, 10000, 0, 0, 10000, 0, 10000, 0, 10000, 10000
};


/*
 * s_lock_stuck(lock) - complain about a stuck spinlock
 */
static void
s_lock_stuck(volatile slock_t *lock, const char *file, const int line)
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
 * s_lock(lock) - take a spinlock with backoff
 */
void
s_lock(volatile slock_t *lock, const char *file, const int line)
{
	int			spins = 0;

	while (TAS(lock))
	{
		struct timeval delay;

		delay.tv_sec = 0;
		delay.tv_usec = s_spincycle[spins % S_NSPINCYCLE];
		(void) select(0, NULL, NULL, NULL, &delay);
		if (++spins > S_MAX_BUSY)
		{
			/* It's been over a minute...  */
			s_lock_stuck(lock, file, line);
		}
	}
}




/*
 * Various TAS implementations that cannot live in s_lock.h as no inline
 * definition exists (yet).
 * In the future, get rid of tas.[cso] and fold it into this file.
 */


#if defined(__GNUC__)
/*************************************************************************
 * All the gcc flavors that are not inlined
 */



#if defined(PPC)
/* Note: need a nice gcc constrained asm version so it can be inlined */
static void
tas_dummy()
{
	__asm__("		\n\
.global		tas		\n\
tas:				\n\
		lwarx	5,0,3	\n\
		cmpwi	5,0	\n\
		bne	fail	\n\
		addi	5,5,1	\n\
        	stwcx.  5,0,3	\n\
     		beq	success	\n\
fail:		li	3,1	\n\
		blr		\n\
success:			\n\
		li 3,0		\n\
        	blr		\n\
	");
}

#endif	 /* PPC */


#if defined(__ns32k__)
int
tas(volatile slock_t *lock)
{
  int res;
  __asm__("sbitb 0, %0" : "=m"(*lock));
  __asm__("sprb us, %0" : "=r"(res));
  res = (res >> 5) & 1;
  return res;
}
#endif

#else							/* defined(__GNUC__) */
/***************************************************************************
 * All non gcc
 */



#if defined(sun3)
static void
tas_dummy()						/* really means: extern int tas(slock_t
								 * *lock); */
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

#endif	 /* sun3 */



#if defined(NEED_SPARC_TAS_ASM)
/*
 * sparc machines not using gcc
 */
static void
tas_dummy()						/* really means: extern int tas(slock_t
								 * *lock); */
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

#endif	 /* NEED_SPARC_TAS_ASM */




#if defined(NEED_I386_TAS_ASM)
/* non gcc i386 based things */
#endif	 /* NEED_I386_TAS_ASM */



#endif	 /* not __GNUC__ */




/*****************************************************************************/
#if defined(S_LOCK_TEST)

/*
 * test program for verifying a port.
 */

volatile slock_t test_lock;

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
	s_lock(&test_lock, __FILE__, __LINE__);

	printf("S_LOCK_TEST: failed, lock not locked~\n");
	exit(3);

}

#endif	 /* S_LOCK_TEST */
