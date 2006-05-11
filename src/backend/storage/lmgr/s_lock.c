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
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/s_lock.c,v 1.16.4.1 2006/05/11 21:59:47 tgl Exp $
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
			"\nStuck spinlock (%p) detected at %s:%d.\n",
			lock, file, line);
	exit(1);
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
	/*
	 * We loop tightly for awhile, then delay using select() and try
	 * again. Preferably, "awhile" should be a small multiple of the
	 * maximum time we expect a spinlock to be held.  100 iterations seems
	 * about right.  In most multi-CPU scenarios, the spinlock is probably
	 * held by a process on another CPU and will be released before we
	 * finish 100 iterations.  However, on a uniprocessor, the tight loop
	 * is just a waste of cycles, so don't iterate thousands of times.
	 *
	 * Once we do decide to block, we use randomly increasing select()
	 * delays. The first delay is 10 msec, then the delay randomly
	 * increases to about one second, after which we reset to 10 msec and
	 * start again.  The idea here is that in the presence of heavy
	 * contention we need to increase the delay, else the spinlock holder
	 * may never get to run and release the lock.  (Consider situation
	 * where spinlock holder has been nice'd down in priority by the
	 * scheduler --- it will not get scheduled until all would-be
	 * acquirers are sleeping, so if we always use a 10-msec sleep, there
	 * is a real possibility of starvation.)  But we can't just clamp the
	 * delay to an upper bound, else it would take a long time to make a
	 * reasonable number of tries.
	 *
	 * We time out and declare error after NUM_DELAYS delays (thus, exactly
	 * that many tries).  With the given settings, this will usually take
	 * 3 or so minutes.  It seems better to fix the total number of tries
	 * (and thus the probability of unintended failure) than to fix the
	 * total time spent.
	 *
	 * The select() delays are measured in centiseconds (0.01 sec) because 10
	 * msec is a common resolution limit at the OS level.
	 */
#define SPINS_PER_DELAY		100
#define NUM_DELAYS			1000
#define MIN_DELAY_CSEC		1
#define MAX_DELAY_CSEC		100

	int			spins = 0;
	int			delays = 0;
	int			cur_delay = MIN_DELAY_CSEC;
	struct timeval delay;

	while (TAS(lock))
	{
		if (++spins > SPINS_PER_DELAY)
		{
			if (++delays > NUM_DELAYS)
				s_lock_stuck(lock, file, line);

			delay.tv_sec = cur_delay / 100;
			delay.tv_usec = (cur_delay % 100) * 10000;
			(void) select(0, NULL, NULL, NULL, &delay);

#if defined(S_LOCK_TEST)
			fprintf(stdout, "*");
			fflush(stdout);
#endif

			/* increase delay by a random fraction between 1X and 2X */
			cur_delay += (int) (cur_delay *
			  (((double) random()) / ((double) MAX_RANDOM_VALUE)) + 0.5);
			/* wrap back to minimum delay when max is exceeded */
			if (cur_delay > MAX_DELAY_CSEC)
				cur_delay = MIN_DELAY_CSEC;

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
 * test program for verifying a port's spinlock support.
 */

volatile slock_t test_lock;

int
main()
{
	srandom((unsigned int) time(NULL));

	S_INIT_LOCK(&test_lock);

	if (!S_LOCK_FREE(&test_lock))
	{
		printf("S_LOCK_TEST: failed, lock not initialized\n");
		return 1;
	}

	S_LOCK(&test_lock);

	if (S_LOCK_FREE(&test_lock))
	{
		printf("S_LOCK_TEST: failed, lock not locked\n");
		return 1;
	}

	S_UNLOCK(&test_lock);

	if (!S_LOCK_FREE(&test_lock))
	{
		printf("S_LOCK_TEST: failed, lock not unlocked\n");
		return 1;
	}

	S_LOCK(&test_lock);

	if (S_LOCK_FREE(&test_lock))
	{
		printf("S_LOCK_TEST: failed, lock not re-locked\n");
		return 1;
	}

	printf("S_LOCK_TEST: this will print %d stars and then\n", NUM_DELAYS);
	printf("             exit with a 'stuck spinlock' message\n");
	printf("             if S_LOCK() and TAS() are working.\n");
	fflush(stdout);

	s_lock(&test_lock, __FILE__, __LINE__);

	printf("S_LOCK_TEST: failed, lock not locked\n");
	return 1;
}

#endif   /* S_LOCK_TEST */
