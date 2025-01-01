/*-------------------------------------------------------------------------
 *
 * generic-sunpro.h
 *	  Atomic operations for solaris' CC
 *
 * Portions Copyright (c) 2013-2025, PostgreSQL Global Development Group
 *
 * NOTES:
 *
 * Documentation:
 * * manpage for atomic_cas(3C)
 *   http://www.unix.com/man-page/opensolaris/3c/atomic_cas/
 *   http://docs.oracle.com/cd/E23824_01/html/821-1465/atomic-cas-3c.html
 *
 * src/include/port/atomics/generic-sunpro.h
 *
 * -------------------------------------------------------------------------
 */

#ifdef HAVE_MBARRIER_H
#include <mbarrier.h>

#define pg_compiler_barrier_impl()	__compiler_barrier()

#ifndef pg_memory_barrier_impl
/*
 * Despite the name this is actually a full barrier. Expanding to mfence/
 * membar #StoreStore | #LoadStore | #StoreLoad | #LoadLoad on x86/sparc
 * respectively.
 */
#	define pg_memory_barrier_impl()		__machine_rw_barrier()
#endif
#ifndef pg_read_barrier_impl
#	define pg_read_barrier_impl()		__machine_r_barrier()
#endif
#ifndef pg_write_barrier_impl
#	define pg_write_barrier_impl()		__machine_w_barrier()
#endif

#endif /* HAVE_MBARRIER_H */

/* Older versions of the compiler don't have atomic.h... */
#ifdef HAVE_ATOMIC_H

#include <atomic.h>

#define PG_HAVE_ATOMIC_U32_SUPPORT
typedef struct pg_atomic_uint32
{
	volatile uint32 value;
} pg_atomic_uint32;

#define PG_HAVE_ATOMIC_U64_SUPPORT
typedef struct pg_atomic_uint64
{
	/*
	 * Syntax to enforce variable alignment should be supported by versions
	 * supporting atomic.h, but it's hard to find accurate documentation. If
	 * it proves to be a problem, we'll have to add more version checks for 64
	 * bit support.
	 */
	volatile uint64 value pg_attribute_aligned(8);
} pg_atomic_uint64;

#endif /* HAVE_ATOMIC_H */


#ifdef HAVE_ATOMIC_H

#define PG_HAVE_ATOMIC_COMPARE_EXCHANGE_U32
static inline bool
pg_atomic_compare_exchange_u32_impl(volatile pg_atomic_uint32 *ptr,
									uint32 *expected, uint32 newval)
{
	bool	ret;
	uint32	current;

	current = atomic_cas_32(&ptr->value, *expected, newval);
	ret = current == *expected;
	*expected = current;
	return ret;
}

#define PG_HAVE_ATOMIC_EXCHANGE_U32
static inline uint32
pg_atomic_exchange_u32_impl(volatile pg_atomic_uint32 *ptr, uint32 newval)
{
	return atomic_swap_32(&ptr->value, newval);
}

#define PG_HAVE_ATOMIC_COMPARE_EXCHANGE_U64
static inline bool
pg_atomic_compare_exchange_u64_impl(volatile pg_atomic_uint64 *ptr,
									uint64 *expected, uint64 newval)
{
	bool	ret;
	uint64	current;

	AssertPointerAlignment(expected, 8);
	current = atomic_cas_64(&ptr->value, *expected, newval);
	ret = current == *expected;
	*expected = current;
	return ret;
}

#define PG_HAVE_ATOMIC_EXCHANGE_U64
static inline uint64
pg_atomic_exchange_u64_impl(volatile pg_atomic_uint64 *ptr, uint64 newval)
{
	return atomic_swap_64(&ptr->value, newval);
}

#endif /* HAVE_ATOMIC_H */
