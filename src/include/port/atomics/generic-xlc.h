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

#endif /* PG_HAVE_ATOMIC_U64_SUPPORT */

#endif /* defined(HAVE_ATOMICS) */
