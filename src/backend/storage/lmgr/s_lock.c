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
 *	  $PostgreSQL: pgsql/src/backend/storage/lmgr/s_lock.c,v 1.28 2004/06/19 20:31:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>
#include <unistd.h>

#include "storage/s_lock.h"
#include "miscadmin.h"

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
	 * We loop tightly for awhile, then delay using pg_usleep() and try
	 * again. Preferably, "awhile" should be a small multiple of the
	 * maximum time we expect a spinlock to be held.  100 iterations seems
	 * about right.  In most multi-CPU scenarios, the spinlock is probably
	 * held by a process on another CPU and will be released before we
	 * finish 100 iterations.  However, on a uniprocessor, the tight loop
	 * is just a waste of cycles, so don't iterate thousands of times.
	 *
	 * Once we do decide to block, we use randomly increasing pg_usleep()
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
	 * The pg_usleep() delays are measured in centiseconds (0.01 sec) because 10
	 * msec is a common resolution limit at the OS level.
	 */
#define SPINS_PER_DELAY		100
#define NUM_DELAYS			1000
#define MIN_DELAY_CSEC		1
#define MAX_DELAY_CSEC		100

	int			spins = 0;
	int			delays = 0;
	int			cur_delay = MIN_DELAY_CSEC;

	while (TAS(lock))
	{
		/* CPU-specific delay each time through the loop */
		SPIN_DELAY();

		/* Block the process every SPINS_PER_DELAY tries */
		if (++spins > SPINS_PER_DELAY)
		{
			if (++delays > NUM_DELAYS)
				s_lock_stuck(lock, file, line);

			pg_usleep(cur_delay * 10000L);

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
 *
 * If you change something here, you will likely need to modify s_lock.h too,
 * because the definitions for these are split between this file and s_lock.h.
 */


#ifdef HAVE_SPINLOCKS	/* skip spinlocks if requested */


#if defined(__GNUC__)

/*
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

/*
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


#if defined(__sparc__) || defined(__sparc)
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
#endif   /* __sparc || __sparc__ */


#endif   /* not __GNUC__ */

#endif /* HAVE_SPINLOCKS */



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
