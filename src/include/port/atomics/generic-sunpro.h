/*-------------------------------------------------------------------------
 *
 * generic-sunpro.h
 *	  Atomic operations for solaris' CC
 *
 * Portions Copyright (c) 2013-2014, PostgreSQL Global Development Group
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

#if defined(HAVE_ATOMICS)

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
	volatile uint64 value;
} pg_atomic_uint64;

#endif /* HAVE_ATOMIC_H */

#endif /* defined(HAVE_ATOMICS) */


#if defined(PG_USE_INLINE) || defined(ATOMICS_INCLUDE_DEFINITIONS)

#if defined(HAVE_ATOMICS)

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

#define PG_HAVE_ATOMIC_COMPARE_EXCHANGE_U64
static inline bool
pg_atomic_compare_exchange_u64_impl(volatile pg_atomic_uint64 *ptr,
									uint64 *expected, uint64 newval)
{
	bool	ret;
	uint64	current;

	current = atomic_cas_64(&ptr->value, *expected, newval);
	ret = current == *expected;
	*expected = current;
	return ret;
}

#endif /* HAVE_ATOMIC_H */

#endif /* defined(HAVE_ATOMICS) */

#endif /* defined(PG_USE_INLINE) || defined(ATOMICS_INCLUDE_DEFINITIONS) */
