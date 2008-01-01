/*-------------------------------------------------------------------------
 *
 * s_lock.h
 *	   Hardware-dependent implementation of spinlocks.
 *
 *	NOTE: none of the macros in this file are intended to be called directly.
 *	Call them through the hardware-independent macros in spin.h.
 *
 *	The following hardware-dependent macros must be provided for each
 *	supported platform:
 *
 *	void S_INIT_LOCK(slock_t *lock)
 *		Initialize a spinlock (to the unlocked state).
 *
 *	void S_LOCK(slock_t *lock)
 *		Acquire a spinlock, waiting if necessary.
 *		Time out and abort() if unable to acquire the lock in a
 *		"reasonable" amount of time --- typically ~ 1 minute.
 *
 *	void S_UNLOCK(slock_t *lock)
 *		Unlock a previously acquired lock.
 *
 *	bool S_LOCK_FREE(slock_t *lock)
 *		Tests if the lock is free. Returns TRUE if free, FALSE if locked.
 *		This does *not* change the state of the lock.
 *
 *	void SPIN_DELAY(void)
 *		Delay operation to occur inside spinlock wait loop.
 *
 *	Note to implementors: there are default implementations for all these
 *	macros at the bottom of the file.  Check if your platform can use
 *	these or needs to override them.
 *
 *  Usually, S_LOCK() is implemented in terms of an even lower-level macro
 *	TAS():
 *
 *	int TAS(slock_t *lock)
 *		Atomic test-and-set instruction.  Attempt to acquire the lock,
 *		but do *not* wait.	Returns 0 if successful, nonzero if unable
 *		to acquire the lock.
 *
 *	TAS() is NOT part of the API, and should never be called directly.
 *
 *	CAUTION: on some platforms TAS() may sometimes report failure to acquire
 *	a lock even when the lock is not locked.  For example, on Alpha TAS()
 *	will "fail" if interrupted.  Therefore TAS() should always be invoked
 *	in a retry loop, even if you are certain the lock is free.
 *
 *	ANOTHER CAUTION: be sure that TAS() and S_UNLOCK() represent sequence
 *	points, ie, loads and stores of other values must not be moved across
 *	a lock or unlock.  In most cases it suffices to make the operation be
 *	done through a "volatile" pointer.
 *
 *	On most supported platforms, TAS() uses a tas() function written
 *	in assembly language to execute a hardware atomic-test-and-set
 *	instruction.  Equivalent OS-supplied mutex routines could be used too.
 *
 *	If no system-specific TAS() is available (ie, HAVE_SPINLOCKS is not
 *	defined), then we fall back on an emulation that uses SysV semaphores
 *	(see spin.c).  This emulation will be MUCH MUCH slower than a proper TAS()
 *	implementation, because of the cost of a kernel call per lock or unlock.
 *	An old report is that Postgres spends around 40% of its time in semop(2)
 *	when using the SysV semaphore code.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	  $PostgreSQL: pgsql/src/include/storage/s_lock.h,v 1.164 2008/01/01 19:45:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef S_LOCK_H
#define S_LOCK_H

#include "storage/pg_sema.h"

#ifdef HAVE_SPINLOCKS	/* skip spinlocks if requested */


#if defined(__GNUC__) || defined(__INTEL_COMPILER)
/*************************************************************************
 * All the gcc inlines
 * Gcc consistently defines the CPU as __cpu__.
 * Other compilers use __cpu or __cpu__ so we test for both in those cases.
 */

/*----------
 * Standard gcc asm format (assuming "volatile slock_t *lock"):

	__asm__ __volatile__(
		"	instruction	\n"
		"	instruction	\n"
		"	instruction	\n"
:		"=r"(_res), "+m"(*lock)		// return register, in/out lock value
:		"r"(lock)					// lock pointer, in input register
:		"memory", "cc");			// show clobbered registers here

 * The output-operands list (after first colon) should always include
 * "+m"(*lock), whether or not the asm code actually refers to this
 * operand directly.  This ensures that gcc believes the value in the
 * lock variable is used and set by the asm code.  Also, the clobbers
 * list (after third colon) should always include "memory"; this prevents
 * gcc from thinking it can cache the values of shared-memory fields
 * across the asm code.  Add "cc" if your asm code changes the condition
 * code register, and also list any temp registers the code uses.
 *----------
 */


#ifdef __i386__		/* 32-bit i386 */
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;

#define TAS(lock) tas(lock)

static __inline__ int
tas(volatile slock_t *lock)
{
	register slock_t _res = 1;

	/*
	 * Use a non-locking test before asserting the bus lock.  Note that the
	 * extra test appears to be a small loss on some x86 platforms and a small
	 * win on others; it's by no means clear that we should keep it.
	 */
	__asm__ __volatile__(
		"	cmpb	$0,%1	\n"
		"	jne		1f		\n"
		"	lock			\n"
		"	xchgb	%0,%1	\n"
		"1: \n"
:		"+q"(_res), "+m"(*lock)
:
:		"memory", "cc");
	return (int) _res;
}

#define SPIN_DELAY() spin_delay()

static __inline__ void
spin_delay(void)
{
	/*
	 * This sequence is equivalent to the PAUSE instruction ("rep" is
	 * ignored by old IA32 processors if the following instruction is
	 * not a string operation); the IA-32 Architecture Software
	 * Developer's Manual, Vol. 3, Section 7.7.2 describes why using
	 * PAUSE in the inner loop of a spin lock is necessary for good
	 * performance:
	 *
	 *     The PAUSE instruction improves the performance of IA-32
	 *     processors supporting Hyper-Threading Technology when
	 *     executing spin-wait loops and other routines where one
	 *     thread is accessing a shared lock or semaphore in a tight
	 *     polling loop. When executing a spin-wait loop, the
	 *     processor can suffer a severe performance penalty when
	 *     exiting the loop because it detects a possible memory order
	 *     violation and flushes the core processor's pipeline. The
	 *     PAUSE instruction provides a hint to the processor that the
	 *     code sequence is a spin-wait loop. The processor uses this
	 *     hint to avoid the memory order violation and prevent the
	 *     pipeline flush. In addition, the PAUSE instruction
	 *     de-pipelines the spin-wait loop to prevent it from
	 *     consuming execution resources excessively.
	 */
	__asm__ __volatile__(
		" rep; nop			\n");
}

#endif	 /* __i386__ */


#ifdef __x86_64__		/* AMD Opteron, Intel EM64T */
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;

#define TAS(lock) tas(lock)

static __inline__ int
tas(volatile slock_t *lock)
{
	register slock_t _res = 1;

	/*
	 * On Opteron, using a non-locking test before the locking instruction
	 * is a huge loss.  On EM64T, it appears to be a wash or small loss,
	 * so we needn't bother to try to distinguish the sub-architectures.
	 */
	__asm__ __volatile__(
		"	lock			\n"
		"	xchgb	%0,%1	\n"
:		"+q"(_res), "+m"(*lock)
:
:		"memory", "cc");
	return (int) _res;
}

#define SPIN_DELAY() spin_delay()

static __inline__ void
spin_delay(void)
{
	/*
	 * Adding a PAUSE in the spin delay loop is demonstrably a no-op on
	 * Opteron, but it may be of some use on EM64T, so we keep it.
	 */
	__asm__ __volatile__(
		" rep; nop			\n");
}

#endif	 /* __x86_64__ */


#if defined(__ia64__) || defined(__ia64)	/* Intel Itanium */
#define HAS_TEST_AND_SET

typedef unsigned int slock_t;

#define TAS(lock) tas(lock)

#ifndef __INTEL_COMPILER

static __inline__ int
tas(volatile slock_t *lock)
{
	long int	ret;

	__asm__ __volatile__(
		"	xchg4 	%0=%1,%2	\n"
:		"=r"(ret), "+m"(*lock)
:		"r"(1)
:		"memory");
	return (int) ret;
}

#else /* __INTEL_COMPILER */

static __inline__ int
tas(volatile slock_t *lock)
{
	int		ret;

	ret = _InterlockedExchange(lock,1);	/* this is a xchg asm macro */

	return ret;
}

#endif /* __INTEL_COMPILER */
#endif	 /* __ia64__ || __ia64 */


#if defined(__arm__) || defined(__arm)
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;

#define TAS(lock) tas(lock)

static __inline__ int
tas(volatile slock_t *lock)
{
	register slock_t _res = 1;

	__asm__ __volatile__(
		"	swpb 	%0, %0, [%2]	\n"
:		"+r"(_res), "+m"(*lock)
:		"r"(lock)
:		"memory");
	return (int) _res;
}

#endif	 /* __arm__ */


/* S/390 and S/390x Linux (32- and 64-bit zSeries) */
#if defined(__s390__) || defined(__s390x__)
#define HAS_TEST_AND_SET

typedef unsigned int slock_t;

#define TAS(lock)	   tas(lock)

static __inline__ int
tas(volatile slock_t *lock)
{
	int			_res = 0;

	__asm__	__volatile__(
		"	cs 	%0,%3,0(%2)		\n"
:		"+d"(_res), "+m"(*lock)
:		"a"(lock), "d"(1)
:		"memory", "cc");
	return _res;
}

#endif	 /* __s390__ || __s390x__ */


#if defined(__sparc__)		/* Sparc */
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;

#define TAS(lock) tas(lock)

static __inline__ int
tas(volatile slock_t *lock)
{
	register slock_t _res;

	/*
	 *	See comment in /pg/backend/port/tas/solaris_sparc.s for why this
	 *	uses "ldstub", and that file uses "cas".  gcc currently generates
	 *	sparcv7-targeted binaries, so "cas" use isn't possible.
	 */
	__asm__ __volatile__(
		"	ldstub	[%2], %0	\n"
:		"=r"(_res), "+m"(*lock)
:		"r"(lock)
:		"memory");
	return (int) _res;
}

#endif	 /* __sparc__ */


/* PowerPC */
#if defined(__ppc__) || defined(__powerpc__) || defined(__ppc64__) || defined(__powerpc64__)
#define HAS_TEST_AND_SET

#if defined(__ppc64__) || defined(__powerpc64__)
typedef unsigned long slock_t;
#else
typedef unsigned int slock_t;
#endif

#define TAS(lock) tas(lock)
/*
 * NOTE: per the Enhanced PowerPC Architecture manual, v1.0 dated 7-May-2002,
 * an isync is a sufficient synchronization barrier after a lwarx/stwcx loop.
 */
static __inline__ int
tas(volatile slock_t *lock)
{
	slock_t _t;
	int _res;

	__asm__ __volatile__(
"	lwarx   %0,0,%3		\n"
"	cmpwi   %0,0		\n"
"	bne     1f			\n"
"	addi    %0,%0,1		\n"
"	stwcx.  %0,0,%3		\n"
"	beq     2f         	\n"
"1:	li      %1,1		\n"
"	b		3f			\n"
"2:						\n"
"	isync				\n"
"	li      %1,0		\n"
"3:						\n"

:	"=&r"(_t), "=r"(_res), "+m"(*lock)
:	"r"(lock)
:	"memory", "cc");
	return _res;
}

/* PowerPC S_UNLOCK is almost standard but requires a "sync" instruction */
#define S_UNLOCK(lock)	\
do \
{ \
	__asm__ __volatile__ ("	sync \n"); \
	*((volatile slock_t *) (lock)) = 0; \
} while (0)

#endif /* powerpc */


/* Linux Motorola 68k */
#if (defined(__mc68000__) || defined(__m68k__)) && defined(__linux__)
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;

#define TAS(lock) tas(lock)

static __inline__ int
tas(volatile slock_t *lock)
{
	register int rv;

	__asm__	__volatile__(
		"	clrl	%0		\n"
		"	tas		%1		\n"
		"	sne		%0		\n"
:		"=d"(rv), "+m"(*lock)
:
:		"memory", "cc");
	return rv;
}

#endif	 /* (__mc68000__ || __m68k__) && __linux__ */


/*
 * VAXen -- even multiprocessor ones
 * (thanks to Tom Ivar Helbekkmo)
 */
#if defined(__vax__)
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;

#define TAS(lock) tas(lock)

static __inline__ int
tas(volatile slock_t *lock)
{
	register int	_res;

	__asm__ __volatile__(
		"	movl 	$1, %0			\n"
		"	bbssi	$0, (%2), 1f	\n"
		"	clrl	%0				\n"
		"1: \n"
:		"=&r"(_res), "+m"(*lock)
:		"r"(lock)
:		"memory");
	return _res;
}

#endif	 /* __vax__ */


#if defined(__ns32k__)		/* National Semiconductor 32K */
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;

#define TAS(lock) tas(lock)

static __inline__ int
tas(volatile slock_t *lock)
{
	register int	_res;

	__asm__ __volatile__(
		"	sbitb	0, %1	\n"
		"	sfsd	%0		\n"
:		"=r"(_res), "+m"(*lock)
:
:		"memory");
	return _res;
}

#endif	 /* __ns32k__ */


#if defined(__alpha) || defined(__alpha__)	/* Alpha */
/*
 * Correct multi-processor locking methods are explained in section 5.5.3
 * of the Alpha AXP Architecture Handbook, which at this writing can be
 * found at ftp://ftp.netbsd.org/pub/NetBSD/misc/dec-docs/index.html.
 * For gcc we implement the handbook's code directly with inline assembler.
 */
#define HAS_TEST_AND_SET

typedef unsigned long slock_t;

#define TAS(lock)  tas(lock)

static __inline__ int
tas(volatile slock_t *lock)
{
	register slock_t _res;

	__asm__	__volatile__(
		"	ldq		$0, %1	\n"
		"	bne		$0, 2f	\n"
		"	ldq_l	%0, %1	\n"
		"	bne		%0, 2f	\n"
		"	mov		1,  $0	\n"
		"	stq_c	$0, %1	\n"
		"	beq		$0, 2f	\n"
		"	mb				\n"
		"	br		3f		\n"
		"2:	mov		1, %0	\n"
		"3:					\n"
:		"=&r"(_res), "+m"(*lock)
:
:		"memory", "0");
	return (int) _res;
}

#define S_UNLOCK(lock)	\
do \
{\
	__asm__ __volatile__ ("	mb \n"); \
	*((volatile slock_t *) (lock)) = 0; \
} while (0)

#endif /* __alpha || __alpha__ */


#if defined(__mips__) && !defined(__sgi)	/* non-SGI MIPS */
/* Note: on SGI we use the OS' mutex ABI, see below */
/* Note: R10000 processors require a separate SYNC */
#define HAS_TEST_AND_SET

typedef unsigned int slock_t;

#define TAS(lock) tas(lock)

static __inline__ int
tas(volatile slock_t *lock)
{
	register volatile slock_t *_l = lock;
	register int _res;
	register int _tmp;

	__asm__ __volatile__(
		"       .set push           \n"
		"       .set mips2          \n"
		"       .set noreorder      \n"
		"       .set nomacro        \n"
		"       ll      %0, %2      \n"
		"       or      %1, %0, 1   \n"
		"       sc      %1, %2      \n"
		"       xori    %1, 1       \n"
		"       or      %0, %0, %1  \n"
		"       sync                \n"
		"       .set pop              "
:		"=&r" (_res), "=&r" (_tmp), "+R" (*_l)
:
:		"memory");
	return _res;
}

/* MIPS S_UNLOCK is almost standard but requires a "sync" instruction */
#define S_UNLOCK(lock)	\
do \
{ \
	__asm__ __volatile__( \
		"       .set push           \n" \
		"       .set mips2          \n" \
		"       .set noreorder      \n" \
		"       .set nomacro        \n" \
		"       sync                \n" \
		"       .set pop              "); \
	*((volatile slock_t *) (lock)) = 0; \
} while (0)

#endif /* __mips__ && !__sgi */


#if defined(__m32r__) && defined(HAVE_SYS_TAS_H)	/* Renesas' M32R */
#define HAS_TEST_AND_SET

#include <sys/tas.h>

typedef int slock_t;

#define TAS(lock) tas(lock)

#endif /* __m32r__ */


/* These live in s_lock.c, but only for gcc */


#if defined(__m68k__) && !defined(__linux__)	/* non-Linux Motorola 68k */
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;
#endif


#endif	/* __GNUC__ */



/*
 * ---------------------------------------------------------------------
 * Platforms that use non-gcc inline assembly:
 * ---------------------------------------------------------------------
 */

#if !defined(HAS_TEST_AND_SET)	/* We didn't trigger above, let's try here */


#if defined(USE_UNIVEL_CC)		/* Unixware compiler */
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;

#define TAS(lock)	tas(lock)

asm int
tas(volatile slock_t *s_lock)
{
/* UNIVEL wants %mem in column 1, so we don't pg_indent this file */
%mem s_lock
	pushl %ebx
	movl s_lock, %ebx
	movl $255, %eax
	lock
	xchgb %al, (%ebx)
	popl %ebx
}

#endif	 /* defined(USE_UNIVEL_CC) */


#if defined(__alpha) || defined(__alpha__)	/* Tru64 Unix Alpha compiler */
/*
 * The Tru64 compiler doesn't support gcc-style inline asm, but it does
 * have some builtin functions that accomplish much the same results.
 * For simplicity, slock_t is defined as long (ie, quadword) on Alpha
 * regardless of the compiler in use.  LOCK_LONG and UNLOCK_LONG only
 * operate on an int (ie, longword), but that's OK as long as we define
 * S_INIT_LOCK to zero out the whole quadword.
 */
#define HAS_TEST_AND_SET

typedef unsigned long slock_t;

#include <alpha/builtins.h>
#define S_INIT_LOCK(lock)  (*(lock) = 0)
#define TAS(lock)		   (__LOCK_LONG_RETRY((lock), 1) == 0)
#define S_UNLOCK(lock)	   __UNLOCK_LONG(lock)

#endif	 /* __alpha || __alpha__ */


#if defined(__hppa) || defined(__hppa__)	/* HP PA-RISC, GCC and HP compilers */
/*
 * HP's PA-RISC
 *
 * See src/backend/port/hpux/tas.c.template for details about LDCWX.  Because
 * LDCWX requires a 16-byte-aligned address, we declare slock_t as a 16-byte
 * struct.  The active word in the struct is whichever has the aligned address;
 * the other three words just sit at -1.
 *
 * When using gcc, we can inline the required assembly code.
 */
#define HAS_TEST_AND_SET

typedef struct
{
	int			sema[4];
} slock_t;

#define TAS_ACTIVE_WORD(lock)	((volatile int *) (((long) (lock) + 15) & ~15))

#if defined(__GNUC__)

static __inline__ int
tas(volatile slock_t *lock)
{
	volatile int *lockword = TAS_ACTIVE_WORD(lock);
	register int lockval;

	__asm__ __volatile__(
		"	ldcwx	0(0,%2),%0	\n"
:		"=r"(lockval), "+m"(*lockword)
:		"r"(lockword)
:		"memory");
	return (lockval == 0);
}

#endif /* __GNUC__ */

#define S_UNLOCK(lock)	(*TAS_ACTIVE_WORD(lock) = -1)

#define S_INIT_LOCK(lock) \
	do { \
		volatile slock_t *lock_ = (lock); \
		lock_->sema[0] = -1; \
		lock_->sema[1] = -1; \
		lock_->sema[2] = -1; \
		lock_->sema[3] = -1; \
	} while (0)

#define S_LOCK_FREE(lock)	(*TAS_ACTIVE_WORD(lock) != 0)

#endif	 /* __hppa || __hppa__ */


#if defined(__hpux) && defined(__ia64) && !defined(__GNUC__)

#define HAS_TEST_AND_SET

typedef unsigned int slock_t;

#include <ia64/sys/inline.h>
#define TAS(lock) _Asm_xchg(_SZ_W, lock, 1, _LDHINT_NONE)

#endif	/* HPUX on IA64, non gcc */


#if defined(__sgi)	/* SGI compiler */
/*
 * SGI IRIX 5
 * slock_t is defined as a unsigned long. We use the standard SGI
 * mutex API.
 *
 * The following comment is left for historical reasons, but is probably
 * not a good idea since the mutex ABI is supported.
 *
 * This stuff may be supplemented in the future with Masato Kataoka's MIPS-II
 * assembly from his NECEWS SVR4 port, but we probably ought to retain this
 * for the R3000 chips out there.
 */
#define HAS_TEST_AND_SET

typedef unsigned long slock_t;

#include "mutex.h"
#define TAS(lock)	(test_and_set(lock,1))
#define S_UNLOCK(lock)	(test_then_and(lock,0))
#define S_INIT_LOCK(lock)	(test_then_and(lock,0))
#define S_LOCK_FREE(lock)	(test_then_add(lock,0) == 0)
#endif	 /* __sgi */


#if defined(sinix)		/* Sinix */
/*
 * SINIX / Reliant UNIX
 * slock_t is defined as a struct abilock_t, which has a single unsigned long
 * member. (Basically same as SGI)
 */
#define HAS_TEST_AND_SET

#include "abi_mutex.h"
typedef abilock_t slock_t;

#define TAS(lock)	(!acquire_lock(lock))
#define S_UNLOCK(lock)	release_lock(lock)
#define S_INIT_LOCK(lock)	init_lock(lock)
#define S_LOCK_FREE(lock)	(stat_lock(lock) == UNLOCKED)
#endif	 /* sinix */


#if defined(_AIX)	/* AIX */
/*
 * AIX (POWER)
 */
#define HAS_TEST_AND_SET

#include <sys/atomic_op.h>

typedef int slock_t;

#define TAS(lock)			_check_lock((slock_t *) (lock), 0, 1)
#define S_UNLOCK(lock)		_clear_lock((slock_t *) (lock), 0)
#endif	 /* _AIX */


#if defined (nextstep)		/* Nextstep */
#define HAS_TEST_AND_SET

typedef struct mutex slock_t;

#define S_LOCK(lock)	mutex_lock(lock)
#define S_UNLOCK(lock)	mutex_unlock(lock)
#define S_INIT_LOCK(lock)	mutex_init(lock)
/* For Mach, we have to delve inside the entrails of `struct mutex'.  Ick! */
#define S_LOCK_FREE(alock)	((alock)->lock == 0)
#endif	 /* nextstep */


/* These are in s_lock.c */


#if defined(sun3)		/* Sun3 */
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;
#endif


#if defined(__sun) && (defined(__i386) || defined(__x86_64__) || defined(__sparc__) || defined(__sparc))
#define HAS_TEST_AND_SET

#if defined(__i386) || defined(__x86_64__) || defined(__sparcv9) || defined(__sparcv8plus)
typedef unsigned int slock_t;
#else
typedef unsigned char slock_t;
#endif

extern slock_t pg_atomic_cas(volatile slock_t *lock, slock_t with,
									  slock_t cmp);

#define TAS(a) (pg_atomic_cas((a), 1, 0) != 0)
#endif


#ifdef WIN32_ONLY_COMPILER
typedef LONG slock_t;

#define HAS_TEST_AND_SET
#define TAS(lock) (InterlockedCompareExchange(lock, 1, 0))

#define SPIN_DELAY() spin_delay()

static __forceinline void
spin_delay(void)
{
	/* See comment for gcc code. Same code, MASM syntax */
	__asm rep nop;
}

#endif

  
#endif	/* !defined(HAS_TEST_AND_SET) */


/* Blow up if we didn't have any way to do spinlocks */
#ifndef HAS_TEST_AND_SET
#error PostgreSQL does not have native spinlock support on this platform.  To continue the compilation, rerun configure using --disable-spinlocks.  However, performance will be poor.  Please report this to pgsql-bugs@postgresql.org.
#endif


#else	/* !HAVE_SPINLOCKS */


/*
 * Fake spinlock implementation using semaphores --- slow and prone
 * to fall foul of kernel limits on number of semaphores, so don't use this
 * unless you must!  The subroutines appear in spin.c.
 */
typedef PGSemaphoreData slock_t;

extern bool s_lock_free_sema(volatile slock_t *lock);
extern void s_unlock_sema(volatile slock_t *lock);
extern void s_init_lock_sema(volatile slock_t *lock);
extern int	tas_sema(volatile slock_t *lock);

#define S_LOCK_FREE(lock)	s_lock_free_sema(lock)
#define S_UNLOCK(lock)	 s_unlock_sema(lock)
#define S_INIT_LOCK(lock)	s_init_lock_sema(lock)
#define TAS(lock)	tas_sema(lock)


#endif	/* HAVE_SPINLOCKS */


/*
 * Default Definitions - override these above as needed.
 */

#if !defined(S_LOCK)
#define S_LOCK(lock) \
	do { \
		if (TAS(lock)) \
			s_lock((lock), __FILE__, __LINE__); \
	} while (0)
#endif	 /* S_LOCK */

#if !defined(S_LOCK_FREE)
#define S_LOCK_FREE(lock)	(*(lock) == 0)
#endif	 /* S_LOCK_FREE */

#if !defined(S_UNLOCK)
#define S_UNLOCK(lock)		(*((volatile slock_t *) (lock)) = 0)
#endif	 /* S_UNLOCK */

#if !defined(S_INIT_LOCK)
#define S_INIT_LOCK(lock)	S_UNLOCK(lock)
#endif	 /* S_INIT_LOCK */

#if !defined(SPIN_DELAY)
#define SPIN_DELAY()	((void) 0)
#endif	 /* SPIN_DELAY */

#if !defined(TAS)
extern int	tas(volatile slock_t *lock);		/* in port/.../tas.s, or
												 * s_lock.c */

#define TAS(lock)		tas(lock)
#endif	 /* TAS */


/*
 * Platform-independent out-of-line support routines
 */
extern void s_lock(volatile slock_t *lock, const char *file, int line);

/* Support for dynamic adjustment of spins_per_delay */
#define DEFAULT_SPINS_PER_DELAY  100

extern void set_spins_per_delay(int shared_spins_per_delay);
extern int	update_spins_per_delay(int shared_spins_per_delay);

#endif	 /* S_LOCK_H */
