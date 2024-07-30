/*-------------------------------------------------------------------------
 *
 * s_lock.h
 *	   Implementation of spinlocks.
 *
 *	NOTE: none of the macros in this file are intended to be called directly.
 *	Call them through the macros in spin.h.
 *
 *	The following hardware-dependent macros must be provided for each
 *	supported platform:
 *
 *	void S_INIT_LOCK(slock_t *lock)
 *		Initialize a spinlock (to the unlocked state).
 *
 *	int S_LOCK(slock_t *lock)
 *		Acquire a spinlock, waiting if necessary.
 *		Time out and abort() if unable to acquire the lock in a
 *		"reasonable" amount of time --- typically ~ 1 minute.
 *		Should return number of "delays"; see s_lock.c
 *
 *	void S_UNLOCK(slock_t *lock)
 *		Unlock a previously acquired lock.
 *
 *	bool S_LOCK_FREE(slock_t *lock)
 *		Tests if the lock is free. Returns true if free, false if locked.
 *		This does *not* change the state of the lock.
 *
 *	void SPIN_DELAY(void)
 *		Delay operation to occur inside spinlock wait loop.
 *
 *	Note to implementors: there are default implementations for all these
 *	macros at the bottom of the file.  Check if your platform can use
 *	these or needs to override them.
 *
 *  Usually, S_LOCK() is implemented in terms of even lower-level macros
 *	TAS() and TAS_SPIN():
 *
 *	int TAS(slock_t *lock)
 *		Atomic test-and-set instruction.  Attempt to acquire the lock,
 *		but do *not* wait.	Returns 0 if successful, nonzero if unable
 *		to acquire the lock.
 *
 *	int TAS_SPIN(slock_t *lock)
 *		Like TAS(), but this version is used when waiting for a lock
 *		previously found to be contended.  By default, this is the
 *		same as TAS(), but on some architectures it's better to poll a
 *		contended lock using an unlocked instruction and retry the
 *		atomic test-and-set only when it appears free.
 *
 *	TAS() and TAS_SPIN() are NOT part of the API, and should never be called
 *	directly.
 *
 *	CAUTION: on some platforms TAS() and/or TAS_SPIN() may sometimes report
 *	failure to acquire a lock even when the lock is not locked.  For example,
 *	on Alpha TAS() will "fail" if interrupted.  Therefore a retry loop must
 *	always be used, even if you are certain the lock is free.
 *
 *	It is the responsibility of these macros to make sure that the compiler
 *	does not re-order accesses to shared memory to precede the actual lock
 *	acquisition, or follow the lock release.  Prior to PostgreSQL 9.5, this
 *	was the caller's responsibility, which meant that callers had to use
 *	volatile-qualified pointers to refer to both the spinlock itself and the
 *	shared data being accessed within the spinlocked critical section.  This
 *	was notationally awkward, easy to forget (and thus error-prone), and
 *	prevented some useful compiler optimizations.  For these reasons, we
 *	now require that the macros themselves prevent compiler re-ordering,
 *	so that the caller doesn't need to take special precautions.
 *
 *	On platforms with weak memory ordering, the TAS(), TAS_SPIN(), and
 *	S_UNLOCK() macros must further include hardware-level memory fence
 *	instructions to prevent similar re-ordering at the hardware level.
 *	TAS() and TAS_SPIN() must guarantee that loads and stores issued after
 *	the macro are not executed until the lock has been obtained.  Conversely,
 *	S_UNLOCK() must guarantee that loads and stores issued before the macro
 *	have been executed before the lock is released.
 *
 *	On most supported platforms, TAS() uses a tas() function written
 *	in assembly language to execute a hardware atomic-test-and-set
 *	instruction.  Equivalent OS-supplied mutex routines could be used too.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	  src/include/storage/s_lock.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef S_LOCK_H
#define S_LOCK_H

#ifdef FRONTEND
#error "s_lock.h may not be included from frontend code"
#endif

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
	slock_t		_res = 1;

	/*
	 * Use a non-locking test before asserting the bus lock.  Note that the
	 * extra test appears to be a small loss on some x86 platforms and a small
	 * win on others; it's by no means clear that we should keep it.
	 *
	 * When this was last tested, we didn't have separate TAS() and TAS_SPIN()
	 * macros.  Nowadays it probably would be better to do a non-locking test
	 * in TAS_SPIN() but not in TAS(), like on x86_64, but no-one's done the
	 * testing to verify that.  Without some empirical evidence, better to
	 * leave it alone.
	 */
	__asm__ __volatile__(
		"	cmpb	$0,%1	\n"
		"	jne		1f		\n"
		"	lock			\n"
		"	xchgb	%0,%1	\n"
		"1: \n"
:		"+q"(_res), "+m"(*lock)
:		/* no inputs */
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

/*
 * On Intel EM64T, it's a win to use a non-locking test before the xchg proper,
 * but only when spinning.
 *
 * See also Implementing Scalable Atomic Locks for Multi-Core Intel(tm) EM64T
 * and IA32, by Michael Chynoweth and Mary R. Lee. As of this writing, it is
 * available at:
 * http://software.intel.com/en-us/articles/implementing-scalable-atomic-locks-for-multi-core-intel-em64t-and-ia32-architectures
 */
#define TAS_SPIN(lock)    (*(lock) ? 1 : TAS(lock))

static __inline__ int
tas(volatile slock_t *lock)
{
	slock_t		_res = 1;

	__asm__ __volatile__(
		"	lock			\n"
		"	xchgb	%0,%1	\n"
:		"+q"(_res), "+m"(*lock)
:		/* no inputs */
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


/*
 * On ARM and ARM64, we use __sync_lock_test_and_set(int *, int) if available.
 *
 * We use the int-width variant of the builtin because it works on more chips
 * than other widths.
 */
#if defined(__arm__) || defined(__arm) || defined(__aarch64__)
#ifdef HAVE_GCC__SYNC_INT32_TAS
#define HAS_TEST_AND_SET

#define TAS(lock) tas(lock)

typedef int slock_t;

static __inline__ int
tas(volatile slock_t *lock)
{
	return __sync_lock_test_and_set(lock, 1);
}

#define S_UNLOCK(lock) __sync_lock_release(lock)

/*
 * Using an ISB instruction to delay in spinlock loops appears beneficial on
 * high-core-count ARM64 processors.  It seems mostly a wash for smaller gear,
 * and ISB doesn't exist at all on pre-v7 ARM chips.
 */
#if defined(__aarch64__)

#define SPIN_DELAY() spin_delay()

static __inline__ void
spin_delay(void)
{
	__asm__ __volatile__(
		" isb;				\n");
}

#endif	 /* __aarch64__ */
#endif	 /* HAVE_GCC__SYNC_INT32_TAS */
#endif	 /* __arm__ || __arm || __aarch64__ */


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
/*
 * Solaris has always run sparc processors in TSO (total store) mode, but
 * linux didn't use to and the *BSDs still don't. So, be careful about
 * acquire/release semantics. The CPU will treat superfluous members as
 * NOPs, so it's just code space.
 */
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;

#define TAS(lock) tas(lock)

static __inline__ int
tas(volatile slock_t *lock)
{
	slock_t		_res;

	/*
	 *	See comment in src/backend/port/tas/sunstudio_sparc.s for why this
	 *	uses "ldstub", and that file uses "cas".  gcc currently generates
	 *	sparcv7-targeted binaries, so "cas" use isn't possible.
	 */
	__asm__ __volatile__(
		"	ldstub	[%2], %0	\n"
:		"=r"(_res), "+m"(*lock)
:		"r"(lock)
:		"memory");
#if defined(__sparcv7) || defined(__sparc_v7__)
	/*
	 * No stbar or membar available, luckily no actually produced hardware
	 * requires a barrier.
	 */
#elif defined(__sparcv8) || defined(__sparc_v8__)
	/* stbar is available (and required for both PSO, RMO), membar isn't */
	__asm__ __volatile__ ("stbar	 \n":::"memory");
#else
	/*
	 * #LoadStore (RMO) | #LoadLoad (RMO) together are the appropriate acquire
	 * barrier for sparcv8+ upwards.
	 */
	__asm__ __volatile__ ("membar #LoadStore | #LoadLoad \n":::"memory");
#endif
	return (int) _res;
}

#if defined(__sparcv7) || defined(__sparc_v7__)
/*
 * No stbar or membar available, luckily no actually produced hardware
 * requires a barrier.  We fall through to the default gcc definition of
 * S_UNLOCK in this case.
 */
#elif defined(__sparcv8) || defined(__sparc_v8__)
/* stbar is available (and required for both PSO, RMO), membar isn't */
#define S_UNLOCK(lock)	\
do \
{ \
	__asm__ __volatile__ ("stbar	 \n":::"memory"); \
	*((volatile slock_t *) (lock)) = 0; \
} while (0)
#else
/*
 * #LoadStore (RMO) | #StoreStore (RMO, PSO) together are the appropriate
 * release barrier for sparcv8+ upwards.
 */
#define S_UNLOCK(lock)	\
do \
{ \
	__asm__ __volatile__ ("membar #LoadStore | #StoreStore \n":::"memory"); \
	*((volatile slock_t *) (lock)) = 0; \
} while (0)
#endif

#endif	 /* __sparc__ */


/* PowerPC */
#if defined(__ppc__) || defined(__powerpc__) || defined(__ppc64__) || defined(__powerpc64__)
#define HAS_TEST_AND_SET

typedef unsigned int slock_t;

#define TAS(lock) tas(lock)

/* On PPC, it's a win to use a non-locking test before the lwarx */
#define TAS_SPIN(lock)	(*(lock) ? 1 : TAS(lock))

/*
 * The second operand of addi can hold a constant zero or a register number,
 * hence constraint "=&b" to avoid allocating r0.  "b" stands for "address
 * base register"; most operands having this register-or-zero property are
 * address bases, e.g. the second operand of lwax.
 *
 * NOTE: per the Enhanced PowerPC Architecture manual, v1.0 dated 7-May-2002,
 * an isync is a sufficient synchronization barrier after a lwarx/stwcx loop.
 * But if the spinlock is in ordinary memory, we can use lwsync instead for
 * better performance.
 */
static __inline__ int
tas(volatile slock_t *lock)
{
	slock_t _t;
	int _res;

	__asm__ __volatile__(
"	lwarx   %0,0,%3,1	\n"
"	cmpwi   %0,0		\n"
"	bne     1f			\n"
"	addi    %0,%0,1		\n"
"	stwcx.  %0,0,%3		\n"
"	beq     2f			\n"
"1: \n"
"	li      %1,1		\n"
"	b       3f			\n"
"2: \n"
"	lwsync				\n"
"	li      %1,0		\n"
"3: \n"
:	"=&b"(_t), "=r"(_res), "+m"(*lock)
:	"r"(lock)
:	"memory", "cc");
	return _res;
}

/*
 * PowerPC S_UNLOCK is almost standard but requires a "sync" instruction.
 * But we can use lwsync instead for better performance.
 */
#define S_UNLOCK(lock)	\
do \
{ \
	__asm__ __volatile__ ("	lwsync \n" ::: "memory"); \
	*((volatile slock_t *) (lock)) = 0; \
} while (0)

#endif /* powerpc */


#if defined(__mips__) && !defined(__sgi)	/* non-SGI MIPS */
#define HAS_TEST_AND_SET

typedef unsigned int slock_t;

#define TAS(lock) tas(lock)

/*
 * Original MIPS-I processors lacked the LL/SC instructions, but if we are
 * so unfortunate as to be running on one of those, we expect that the kernel
 * will handle the illegal-instruction traps and emulate them for us.  On
 * anything newer (and really, MIPS-I is extinct) LL/SC is the only sane
 * choice because any other synchronization method must involve a kernel
 * call.  Unfortunately, many toolchains still default to MIPS-I as the
 * codegen target; if the symbol __mips shows that that's the case, we
 * have to force the assembler to accept LL/SC.
 *
 * R10000 and up processors require a separate SYNC, which has the same
 * issues as LL/SC.
 */
#if __mips < 2
#define MIPS_SET_MIPS2	"       .set mips2          \n"
#else
#define MIPS_SET_MIPS2
#endif

static __inline__ int
tas(volatile slock_t *lock)
{
	volatile slock_t *_l = lock;
	int			_res;
	int			_tmp;

	__asm__ __volatile__(
		"       .set push           \n"
		MIPS_SET_MIPS2
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
:		/* no inputs */
:		"memory");
	return _res;
}

/* MIPS S_UNLOCK is almost standard but requires a "sync" instruction */
#define S_UNLOCK(lock)	\
do \
{ \
	__asm__ __volatile__( \
		"       .set push           \n" \
		MIPS_SET_MIPS2 \
		"       .set noreorder      \n" \
		"       .set nomacro        \n" \
		"       sync                \n" \
		"       .set pop              " \
:		/* no outputs */ \
:		/* no inputs */	\
:		"memory"); \
	*((volatile slock_t *) (lock)) = 0; \
} while (0)

#endif /* __mips__ && !__sgi */



/*
 * If we have no platform-specific knowledge, but we found that the compiler
 * provides __sync_lock_test_and_set(), use that.  Prefer the int-width
 * version over the char-width version if we have both, on the rather dubious
 * grounds that that's known to be more likely to work in the ARM ecosystem.
 * (But we dealt with ARM above.)
 */
#if !defined(HAS_TEST_AND_SET)

#if defined(HAVE_GCC__SYNC_INT32_TAS)
#define HAS_TEST_AND_SET

#define TAS(lock) tas(lock)

typedef int slock_t;

static __inline__ int
tas(volatile slock_t *lock)
{
	return __sync_lock_test_and_set(lock, 1);
}

#define S_UNLOCK(lock) __sync_lock_release(lock)

#elif defined(HAVE_GCC__SYNC_CHAR_TAS)
#define HAS_TEST_AND_SET

#define TAS(lock) tas(lock)

typedef char slock_t;

static __inline__ int
tas(volatile slock_t *lock)
{
	return __sync_lock_test_and_set(lock, 1);
}

#define S_UNLOCK(lock) __sync_lock_release(lock)

#endif	 /* HAVE_GCC__SYNC_INT32_TAS */

#endif	/* !defined(HAS_TEST_AND_SET) */


/*
 * Default implementation of S_UNLOCK() for gcc/icc.
 *
 * Note that this implementation is unsafe for any platform that can reorder
 * a memory access (either load or store) after a following store.  That
 * happens not to be possible on x86 and most legacy architectures (some are
 * single-processor!), but many modern systems have weaker memory ordering.
 * Those that do must define their own version of S_UNLOCK() rather than
 * relying on this one.
 */
#if !defined(S_UNLOCK)
#define S_UNLOCK(lock)	\
	do { __asm__ __volatile__("" : : : "memory");  *(lock) = 0; } while (0)
#endif

#endif	/* defined(__GNUC__) || defined(__INTEL_COMPILER) */


/*
 * ---------------------------------------------------------------------
 * Platforms that use non-gcc inline assembly:
 * ---------------------------------------------------------------------
 */

#if !defined(HAS_TEST_AND_SET)	/* We didn't trigger above, let's try here */

/* These are in sunstudio_(sparc|x86).s */

#if defined(__SUNPRO_C) && (defined(__i386) || defined(__x86_64__) || defined(__sparc__) || defined(__sparc))
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


#ifdef _MSC_VER
typedef LONG slock_t;

#define HAS_TEST_AND_SET
#define TAS(lock) (InterlockedCompareExchange(lock, 1, 0))

#define SPIN_DELAY() spin_delay()

/* If using Visual C++ on Win64, inline assembly is unavailable.
 * Use a _mm_pause intrinsic instead of rep nop.
 */
#if defined(_WIN64)
static __forceinline void
spin_delay(void)
{
	_mm_pause();
}
#else
static __forceinline void
spin_delay(void)
{
	/* See comment for gcc code. Same code, MASM syntax */
	__asm rep nop;
}
#endif

#include <intrin.h>
#pragma intrinsic(_ReadWriteBarrier)

#define S_UNLOCK(lock)	\
	do { _ReadWriteBarrier(); (*(lock)) = 0; } while (0)

#endif


#endif	/* !defined(HAS_TEST_AND_SET) */


/* Blow up if we didn't have any way to do spinlocks */
#ifndef HAS_TEST_AND_SET
#error PostgreSQL does not have spinlock support on this platform.  Please report this to pgsql-bugs@lists.postgresql.org.
#endif


/*
 * Default Definitions - override these above as needed.
 */

#if !defined(S_LOCK)
#define S_LOCK(lock) \
	(TAS(lock) ? s_lock((lock), __FILE__, __LINE__, __func__) : 0)
#endif	 /* S_LOCK */

#if !defined(S_LOCK_FREE)
#define S_LOCK_FREE(lock)	(*(lock) == 0)
#endif	 /* S_LOCK_FREE */

#if !defined(S_UNLOCK)
/*
 * Our default implementation of S_UNLOCK is essentially *(lock) = 0.  This
 * is unsafe if the platform can reorder a memory access (either load or
 * store) after a following store; platforms where this is possible must
 * define their own S_UNLOCK.  But CPU reordering is not the only concern:
 * if we simply defined S_UNLOCK() as an inline macro, the compiler might
 * reorder instructions from inside the critical section to occur after the
 * lock release.  Since the compiler probably can't know what the external
 * function s_unlock is doing, putting the same logic there should be adequate.
 * A sufficiently-smart globally optimizing compiler could break that
 * assumption, though, and the cost of a function call for every spinlock
 * release may hurt performance significantly, so we use this implementation
 * only for platforms where we don't know of a suitable intrinsic.  For the
 * most part, those are relatively obscure platform/compiler combinations to
 * which the PostgreSQL project does not have access.
 */
#define USE_DEFAULT_S_UNLOCK
extern void s_unlock(volatile slock_t *lock);
#define S_UNLOCK(lock)		s_unlock(lock)
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

#if !defined(TAS_SPIN)
#define TAS_SPIN(lock)	TAS(lock)
#endif	 /* TAS_SPIN */


/*
 * Platform-independent out-of-line support routines
 */
extern int s_lock(volatile slock_t *lock, const char *file, int line, const char *func);

/* Support for dynamic adjustment of spins_per_delay */
#define DEFAULT_SPINS_PER_DELAY  100

extern void set_spins_per_delay(int shared_spins_per_delay);
extern int	update_spins_per_delay(int shared_spins_per_delay);

/*
 * Support for spin delay which is useful in various places where
 * spinlock-like procedures take place.
 */
typedef struct
{
	int			spins;
	int			delays;
	int			cur_delay;
	const char *file;
	int			line;
	const char *func;
} SpinDelayStatus;

static inline void
init_spin_delay(SpinDelayStatus *status,
				const char *file, int line, const char *func)
{
	status->spins = 0;
	status->delays = 0;
	status->cur_delay = 0;
	status->file = file;
	status->line = line;
	status->func = func;
}

#define init_local_spin_delay(status) init_spin_delay(status, __FILE__, __LINE__, __func__)
extern void perform_spin_delay(SpinDelayStatus *status);
extern void finish_spin_delay(SpinDelayStatus *status);

#endif	 /* S_LOCK_H */
