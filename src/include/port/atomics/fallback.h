/*-------------------------------------------------------------------------
 *
 * fallback.h
 *    Fallback for platforms without 64 bit atomics support. Slower
 *    than native atomics support, but not unusably slow.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/port/atomics/fallback.h
 *
 *-------------------------------------------------------------------------
 */

/* intentionally no include guards, should only be included by atomics.h */
#ifndef INSIDE_ATOMICS_H
#	error "should be included via atomics.h"
#endif

#ifndef pg_memory_barrier_impl
/*
 * If we have no memory barrier implementation for this architecture, we
 * fall back to acquiring and releasing a spinlock.
 *
 * It's not self-evident that every possible legal implementation of a
 * spinlock acquire-and-release would be equivalent to a full memory barrier.
 * For example, I'm not sure that Itanium's acq and rel add up to a full
 * fence.  But all of our actual implementations seem OK in this regard.
 */
#define PG_HAVE_MEMORY_BARRIER_EMULATION

extern void pg_spinlock_barrier(void);
#define pg_memory_barrier_impl pg_spinlock_barrier
#endif


#if !defined(PG_HAVE_ATOMIC_U64_SUPPORT)

#define PG_HAVE_ATOMIC_U64_SIMULATION

#define PG_HAVE_ATOMIC_U64_SUPPORT
typedef struct pg_atomic_uint64
{
	int			sema;
	volatile uint64 value;
} pg_atomic_uint64;

#define PG_HAVE_ATOMIC_INIT_U64
extern void pg_atomic_init_u64_impl(volatile pg_atomic_uint64 *ptr, uint64 val_);

#define PG_HAVE_ATOMIC_COMPARE_EXCHANGE_U64
extern bool pg_atomic_compare_exchange_u64_impl(volatile pg_atomic_uint64 *ptr,
												uint64 *expected, uint64 newval);

#define PG_HAVE_ATOMIC_FETCH_ADD_U64
extern uint64 pg_atomic_fetch_add_u64_impl(volatile pg_atomic_uint64 *ptr, int64 add_);

#endif /* PG_HAVE_ATOMIC_U64_SUPPORT */
