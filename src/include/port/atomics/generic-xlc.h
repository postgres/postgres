/*-------------------------------------------------------------------------
 *
 * generic-xlc.h
 *	  Atomic operations for IBM's CC
 *
 * Portions Copyright (c) 2013-2016, PostgreSQL Global Development Group
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
	/*
	 * __fetch_and_add() emits a leading "sync" and trailing "isync", thereby
	 * providing sequential consistency.  This is undocumented.
	 */
	return __fetch_and_add((volatile int *)&ptr->value, add_);
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
	return __fetch_and_addlp((volatile long *)&ptr->value, add_);
}

#endif /* PG_HAVE_ATOMIC_U64_SUPPORT */

#endif /* defined(HAVE_ATOMICS) */
