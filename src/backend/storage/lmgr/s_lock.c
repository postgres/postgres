/*-------------------------------------------------------------------------
 *
 * s_lock.c
 *	   Hardware-dependent implementation of spinlocks.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/s_lock.c,v 1.13 2003/08/04 02:40:03 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/time.h>
#include <unistd.h>

#include "storage/s_lock.h"


/*
 * s_lock_stuck() - complain about a stuck spinlock
 */
static void
s_lock_stuck(volatile slock_t *lock, const char *file, int line)
{
#if defined(S_LOCK_TEST)
	fprintf(stderr,
			"\nFATAL: stuck spinlock (%p) detected at %s:%d.\n",
			lock, file, line);
	abort();
#else
	elog(PANIC, "stuck spinlock (%p) detected at %s:%d",
		 lock, file, line);
#endif
}


/*
 * s_lock(lock) - platform-independent portion of waiting for a spinlock.
 */
void
s_lock(volatile slock_t *lock, const char *file, int line)
{
	unsigned	spins = 0;
	unsigned	delays = 0;
	struct timeval delay;

	/*
	 * We loop tightly for awhile, then delay using select() and try
	 * again. Preferably, "awhile" should be a small multiple of the
	 * maximum time we expect a spinlock to be held.  100 iterations seems
	 * about right.
	 *
	 * We use a 10 millisec select delay because that is the lower limit on
	 * many platforms.	The timeout is figured on this delay only, and so
	 * the nominal 1 minute is a lower bound.
	 */
#define SPINS_PER_DELAY		100
#define DELAY_MSEC			10
#define TIMEOUT_MSEC		(60 * 1000)

	while (TAS(lock))
	{
		if (++spins > SPINS_PER_DELAY)
		{
			if (++delays > (TIMEOUT_MSEC / DELAY_MSEC))
				s_lock_stuck(lock, file, line);

			delay.tv_sec = 0;
			delay.tv_usec = DELAY_MSEC * 1000;
			(void) select(0, NULL, NULL, NULL, &delay);

			spins = 0;
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


#if defined(__m68k__)
static void
tas_dummy()						/* really means: extern int tas(slock_t
								 * **lock); */
{
	__asm__		__volatile__(
										 "\
.global		_tas				\n\
_tas:							\n\
			movel	sp@(0x4),a0	\n\
			tas 	a0@			\n\
			beq 	_success	\n\
			moveq 	#-128,d0	\n\
			rts					\n\
_success:						\n\
			moveq 	#0,d0		\n\
			rts					\n\
");
}
#endif   /* __m68k__ */

#if defined(__mips__) && !defined(__sgi)
static void
tas_dummy()
{
	__asm__		__volatile__(
										 "\
.global	tas						\n\
tas:							\n\
			.frame	$sp, 0, $31	\n\
			.set push		\n\
			.set mips2		\n\
			ll		$14, 0($4)	\n\
			or		$15, $14, 1	\n\
			sc		$15, 0($4)	\n\
			.set pop			\n\
			beq		$15, 0, fail\n\
			bne		$14, 0, fail\n\
			li		$2, 0		\n\
			.livereg 0x2000FF0E,0x00000FFF	\n\
			j		$31			\n\
fail:							\n\
			li		$2, 1		\n\
			j   	$31			\n\
");
}
#endif   /* __mips__ && !__sgi */

#else							/* not __GNUC__ */
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
#endif   /* sun3 */



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
#endif   /* NEED_SPARC_TAS_ASM */




#if defined(NEED_I386_TAS_ASM)
/* non gcc i386 based things */
#endif   /* NEED_I386_TAS_ASM */
#endif   /* not __GNUC__ */




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

#endif   /* S_LOCK_TEST */
