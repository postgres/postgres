/*-------------------------------------------------------------------------
 *
 * generic-xlc.h
 *	  Atomic operations for IBM's CC
 *
 * Portions Copyright (c) 2013-2019, PostgreSQL Global Development Group
 *
 * NOTES:
 *
 * Documentation:
 * * Synchronization and atomic built-in functions
 *   http://www-01.ibm.com/support/knowledgecenter/SSGH3R_13.1.2/com.ibm.xlcpp131.aix.doc/compiler_ref/bifs_sync_atomic.html
 *
 * src/include/port/atomics/generic-xlc.h
 *
 * -------------------------------------------------------------------------
 */

#if defined(HAVE_ATOMICS)

#define PG_HAVE_ATOMIC_U32_SUPPORT
typedef struct pg_atomic_uint32
{
	volatile uint32 value;
} pg_atomic_uint32;


/* 64bit atomics are only supported in 64bit mode */
#ifdef __64BIT__
#define PG_HAVE_ATOMIC_U64_SUPPORT
typedef struct pg_atomic_uint64
{
	volatile uint64 value pg_attribute_aligned(8);
} pg_atomic_uint64;

#endif /* __64BIT__ */

#define PG_HAVE_ATOMIC_COMPARE_EXCHANGE_U32
static inline bool
pg_atomic_compare_exchange_u32_impl(volatile pg_atomic_uint32 *ptr,
									uint32 *expected, uint32 newval)
{
	bool		ret;

	/*
	 * atomics.h specifies sequential consistency ("full barrier semantics")
	 * for this interface.  Since "lwsync" provides acquire/release
	 * consistency only, do not use it here.  GCC atomics observe the same
	 * restriction; see its rs6000_pre_atomic_barrier().
	 */
	__asm__ __volatile__ ("	sync \n" ::: "memory");

	/*
	 * XXX: __compare_and_swap is defined to take signed parameters, but that
	 * shouldn't matter since we don't perform any arithmetic operations.
	 */
	ret = __compare_and_swap((volatile int*)&ptr->value,
							 (int *)expected, (int)newval);

	/*
	 * xlc's documentation tells us:
	 * "If __compare_and_swap is used as a locking primitive, insert a call to
	 * the __isync built-in function at the start of any critical sections."
	 *
	 * The critical section begins immediately after __compare_and_swap().
	 */
	__isync();

	return ret;
}

#define PG_HAVE_ATOMIC_FETCH_ADD_U32
static inline uint32
pg_atomic_fetch_add_u32_impl(volatile pg_atomic_uint32 *ptr, int32 add_)
{
	uint32 _t;
	uint32 res;

	/*
	 * xlc has a no-longer-documented __fetch_and_add() intrinsic.  In xlc
	 * 12.01.0000.0000, it emits a leading "sync" and trailing "isync".  In
	 * xlc 13.01.0003.0004, it emits neither.  Hence, using the intrinsic
	 * would add redundant syncs on xlc 12.
	 */
	__asm__ __volatile__(
		"	sync				\n"
		"	lwarx   %1,0,%4		\n"
		"	add     %0,%1,%3	\n"
		"	stwcx.  %0,0,%4		\n"
		"	bne     $-12		\n"		/* branch to lwarx */
		"	isync				\n"
:		"=&r"(_t), "=&r"(res), "+m"(ptr->value)
:		"r"(add_), "r"(&ptr->value)
:		"memory", "cc");

	return res;
}

#ifdef PG_HAVE_ATOMIC_U64_SUPPORT

#define PG_HAVE_ATOMIC_COMPARE_EXCHANGE_U64
static inline bool
pg_atomic_compare_exchange_u64_impl(volatile pg_atomic_uint64 *ptr,
									uint64 *expected, uint64 newval)
{
	bool		ret;

	__asm__ __volatile__ ("	sync \n" ::: "memory");

	ret = __compare_and_swaplp((volatile long*)&ptr->value,
							   (long *)expected, (long)newval);

	__isync();

	return ret;
}

#define PG_HAVE_ATOMIC_FETCH_ADD_U64
static inline uint64
pg_atomic_fetch_add_u64_impl(volatile pg_atomic_uint64 *ptr, int64 add_)
{
	uint64 _t;
	uint64 res;

	/* Like u32, but s/lwarx/ldarx/; s/stwcx/stdcx/ */
	__asm__ __volatile__(
		"	sync				\n"
		"	ldarx   %1,0,%4		\n"
		"	add     %0,%1,%3	\n"
		"	stdcx.  %0,0,%4		\n"
		"	bne     $-12		\n"		/* branch to ldarx */
		"	isync				\n"
:		"=&r"(_t), "=&r"(res), "+m"(ptr->value)
:		"r"(add_), "r"(&ptr->value)
:		"memory", "cc");

	return res;
}

#endif /* PG_HAVE_ATOMIC_U64_SUPPORT */

#endif /* defined(HAVE_ATOMICS) */
