/*-------------------------------------------------------------------------
 *
 * s_lock.h--
 *	   This file contains the implementation (if any) for spinlocks.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/include/storage/s_lock.h,v 1.14 1997/12/30 04:01:28 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 DESCRIPTION
 *		The following code fragment should be written (in assembly
 *		language) on machines that have a native test-and-set instruction:
 *
 *		void
 *		S_LOCK(char_address)
 *			char *char_address;
 *		{
 *			while (test_and_set(char_address))
 *				;
 *		}
 *
 *		If this is not done, POSTGRES will default to using System V
 *		semaphores (and take a large performance hit -- around 40% of
 *		its time on a DS5000/240 is spent in semop(3)...).
 *
 *	 NOTES
 *		AIX has a test-and-set but the recommended interface is the cs(3)
 *		system call.  This provides an 8-instruction (plus system call
 *		overhead) uninterruptible compare-and-set operation.  True
 *		spinlocks might be faster but using cs(3) still speeds up the
 *		regression test suite by about 25%.  I don't have an assembler
 *		manual for POWER in any case.
 *
 */
#ifndef S_LOCK_H
#define S_LOCK_H

#include "storage/ipc.h"

#if defined(HAS_TEST_AND_SET)

#if defined (nextstep)
/*
 * NEXTSTEP (mach)
 * slock_t is defined as a struct mutex.
 */
#define	S_LOCK(lock)	mutex_lock(lock)

#define	S_UNLOCK(lock)	mutex_unlock(lock)

#define	S_INIT_LOCK(lock)	mutex_init(lock)

 /* S_LOCK_FREE should return 1 if lock is free; 0 if lock is locked */
/* For Mach, we have to delve inside the entrails of `struct mutex'.  Ick! */
#define	S_LOCK_FREE(alock)	((alock)->lock == 0)

#endif							/* next */



#if defined(irix5)
/*
 * SGI IRIX 5
 * slock_t is defined as a struct abilock_t, which has a single unsigned long
 * member.
 *
 * This stuff may be supplemented in the future with Masato Kataoka's MIPS-II
 * assembly from his NECEWS SVR4 port, but we probably ought to retain this
 * for the R3000 chips out there.
 */
#define	S_LOCK(lock)	do \
						{ \
							while (!acquire_lock(lock)) \
								; \
						} while (0)

#define S_UNLOCK(lock)	release_lock(lock)

#define	S_INIT_LOCK(lock)	init_lock(lock)

/* S_LOCK_FREE should return 1 if lock is free; 0 if lock is locked */

#define	S_LOCK_FREE(lock)	(stat_lock(lock) == UNLOCKED)

#endif							/* irix5 */


/*
 * OSF/1 (Alpha AXP)
 *
 * Note that slock_t on the Alpha AXP is msemaphore instead of char
 * (see storage/ipc.h).
 */

#if (defined(__alpha__) || defined(__alpha)) && !defined(linux)

#define	S_LOCK(lock)	do \
						{ \
							while (msem_lock((lock), MSEM_IF_NOWAIT) < 0) \
								; \
						} while (0)

#define	S_UNLOCK(lock)	msem_unlock((lock), 0)

#define	S_INIT_LOCK(lock)	msem_init((lock), MSEM_UNLOCKED)

#define	S_LOCK_FREE(lock)	(!(lock)->msem_state)

#endif							/* alpha */

/*
 * Solaris 2
 */

#if defined(i386_solaris) || \
	defined(sparc_solaris)
/* for xxxxx_solaris, this is defined in port/.../tas.s */

static int	tas(slock_t *lock);

#define	S_LOCK(lock)	do \
						{ \
							while (tas(lock)) \
								; \
						} while (0)

#define	S_UNLOCK(lock)	(*(lock) = 0)

#define	S_INIT_LOCK(lock)	S_UNLOCK(lock)

#endif							/* i86pc_solaris || sparc_solaris */

/*
 * AIX (POWER)
 *
 * Note that slock_t on POWER/POWER2/PowerPC is int instead of char
 * (see storage/ipc.h).
 */

#if defined(aix)

#define	S_LOCK(lock)	do \
						{ \
							while (cs((int *) (lock), 0, 1)) \
								; \
						} while (0)

#define	S_UNLOCK(lock)	(*(lock) = 0)

#define	S_INIT_LOCK(lock)	S_UNLOCK(lock)

#endif							/* aix */

/*
 * HP-UX (PA-RISC)
 *
 * Note that slock_t on PA-RISC is a structure instead of char
 * (see storage/ipc.h).
 */

#if defined(hpux)

/*
* a "set" slock_t has a single word cleared.  a "clear" slock_t has
* all words set to non-zero.
*/
static slock_t clear_lock = {-1, -1, -1, -1};

static int	tas(slock_t *lock);

#define	S_LOCK(lock)	do \
						{ \
							while (tas(lock)) \
								; \
						} while (0)

#define	S_UNLOCK(lock)	(*(lock) = clear_lock)			/* struct assignment */

#define	S_INIT_LOCK(lock)	S_UNLOCK(lock)

#define	S_LOCK_FREE(lock)	( *(int *) (((long) (lock) + 15) & ~15) != 0)

#endif							/* hpux */

/*
 * sun3
 */

#if defined(sun3)

static int	tas(slock_t *lock);

#define S_LOCK(lock)	do \
						{ \
							while (tas(lock)) \
								; \
						} while (0)

#define	S_UNLOCK(lock)	(*(lock) = 0)

#define	S_INIT_LOCK(lock)	S_UNLOCK(lock)

static int
tas_dummy()
{
	asm("LLA0:");
	asm("	.data");
	asm("	.text");
	asm("|#PROC# 04");
	asm("	.globl	_tas");
	asm("_tas:");
	asm("|#PROLOGUE# 1");
	asm("	movel   sp@(0x4),a0");
	asm("	tas	a0@");
	asm("	beq	LLA1");
	asm("	moveq   #-128,d0");
	asm("	rts");
	asm("LLA1:");
	asm("	moveq   #0,d0");
	asm("	rts");
	asm("	.data");
}

#endif							/* sun3 */

/*
 * sparc machines
 */

#if defined(NEED_SPARC_TAS_ASM)

/* if we're using -ansi w/ gcc, use __asm__ instead of asm */
#if defined(__STRICT_ANSI__)
#define asm(x)	__asm__(x)
#endif

static int	tas(slock_t *lock);

static int
tas_dummy()
{
	asm(".seg \"data\"");
	asm(".seg \"text\"");
	asm(".global _tas");
	asm("_tas:");

	/*
	 * Sparc atomic test and set (sparc calls it "atomic load-store")
	 */

	asm("ldstub [%r8], %r8");

	/*
	 * Did test and set actually do the set?
	 */

	asm("tst %r8");

	asm("be,a ReturnZero");

	/*
	 * otherwise, just return.
	 */

	asm("clr %r8");
	asm("mov 0x1, %r8");
	asm("ReturnZero:");
	asm("retl");
	asm("nop");
}

#define	S_LOCK(addr)	do \
						{ \
							while (tas(addr)) \
								; \
						} while (0)

/*
 * addr should be as in the above S_LOCK routine
 */
#define	S_UNLOCK(addr)	(*(addr) = 0)

#define	S_INIT_LOCK(addr)	(*(addr) = 0)

#endif							/* NEED_SPARC_TAS_ASM */

/*
 * i386 based things
 */

#if defined(NEED_I386_TAS_ASM)

#define	S_LOCK(lock)	do \
						{ \
							slock_t		_res; \
							do \
							{ \
				__asm__("xchgb %0,%1": "=q"(_res), "=m"(*lock):"0"(0x1)); \
							} while (_res != 0); \
						} while (0)

#define	S_UNLOCK(lock)	(*(lock) = 0)

#define	S_INIT_LOCK(lock)	S_UNLOCK(lock)

#endif							/* NEED_I386_TAS_ASM */


#if defined(__alpha__) && defined(linux)

void S_LOCK(slock_t* lock);

#define S_UNLOCK(lock) { __asm__("mb"); *(lock) = 0; }

#define	S_INIT_LOCK(lock)	S_UNLOCK(lock)

#endif							/* defined(__alpha__) && defined(linux) */

#if (defined(linux) || defined(__NetBSD__)) && defined(sparc)

#define S_LOCK(lock)	do \
						{ \
							slock_t		_res; \
							slock_t		*tmplock = lock ; \
							do \
							{ \
								__asm__("ldstub [%1], %0" \
						:		"=&r"(_res), "=r"(tmplock) \
						:		"1"(tmplock)); \
							} while (_res != 0); \
						} while (0)

#define	S_UNLOCK(lock)	(*(lock) = 0)

#define	S_INIT_LOCK(lock)	S_UNLOCK(lock)

#endif							/* defined(linux) && defined(sparc) */

#if defined(linux) && defined(PPC)

static int
tas_dummy()
{
	__asm__("	\n\
tas:			\n\
	lwarx	5,0,3	\n\
	cmpwi	5,0	\n\
	bne	fail	\n\
	addi	5,5,1	\n\
        stwcx.  5,0,3	\n\
        beq	success	\n\
fail:	li	3,1	\n\
	blr		\n\
success:		\n\
	li 3,0		\n\
        blr		\n\
	");
}

#define	S_LOCK(lock)	do \
						{ \
							while (tas(lock)) \
								; \
						} while (0)

#define	S_UNLOCK(lock)	(*(lock) = 0)

#define	S_INIT_LOCK(lock)	S_UNLOCK(lock)

#endif							/* defined(linux) && defined(PPC) */

#ifndef S_LOCK_FREE		/* for those who have not already defined it */
#define S_LOCK_FREE(lock)		((*lock) == 0)
#endif

#endif							/* HAS_TEST_AND_SET */

#endif							/* S_LOCK_H */ 
