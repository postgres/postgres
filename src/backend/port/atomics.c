/*-------------------------------------------------------------------------
 *
 * atomics.c
 *	   Non-Inline parts of the atomics implementation
 *
 * Portions Copyright (c) 2013-2024, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/port/atomics.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/spin.h"


#ifdef PG_HAVE_ATOMIC_U64_SIMULATION

void
pg_atomic_init_u64_impl(volatile pg_atomic_uint64 *ptr, uint64 val_)
{
	StaticAssertDecl(sizeof(ptr->sema) >= sizeof(slock_t),
					 "size mismatch of atomic_uint64 vs slock_t");

	SpinLockInit((slock_t *) &ptr->sema);
	ptr->value = val_;
}

bool
pg_atomic_compare_exchange_u64_impl(volatile pg_atomic_uint64 *ptr,
									uint64 *expected, uint64 newval)
{
	bool		ret;

	/*
	 * Do atomic op under a spinlock. It might look like we could just skip
	 * the cmpxchg if the lock isn't available, but that'd just emulate a
	 * 'weak' compare and swap. I.e. one that allows spurious failures. Since
	 * several algorithms rely on a strong variant and that is efficiently
	 * implementable on most major architectures let's emulate it here as
	 * well.
	 */
	SpinLockAcquire((slock_t *) &ptr->sema);

	/* perform compare/exchange logic */
	ret = ptr->value == *expected;
	*expected = ptr->value;
	if (ret)
		ptr->value = newval;

	/* and release lock */
	SpinLockRelease((slock_t *) &ptr->sema);

	return ret;
}

uint64
pg_atomic_fetch_add_u64_impl(volatile pg_atomic_uint64 *ptr, int64 add_)
{
	uint64		oldval;

	SpinLockAcquire((slock_t *) &ptr->sema);
	oldval = ptr->value;
	ptr->value += add_;
	SpinLockRelease((slock_t *) &ptr->sema);
	return oldval;
}

#endif							/* PG_HAVE_ATOMIC_U64_SIMULATION */
