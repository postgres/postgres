/*--------------------------------------------------------------------------
 *
 * test_lwlock_tranches.c
 *		Test code for LWLock tranches allocated by extensions.
 *
 * Copyright (c) 2025-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_lwlock_tranches/test_lwlock_tranches.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/wait_classes.h"

PG_MODULE_MAGIC;

static shmem_request_hook_type prev_shmem_request_hook;
static void test_lwlock_tranches_shmem_request(void);

void
_PG_init(void)
{
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = test_lwlock_tranches_shmem_request;
}

static void
test_lwlock_tranches_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	/*
	 * Request some tranches with RequestNamedLWLockTranche() for
	 * test_startup_lwlocks()
	 */
	RequestNamedLWLockTranche("test_lwlock_tranches_startup", 1);
	RequestNamedLWLockTranche("test_lwlock_tranches_startup10", 10);
}

/*
 * Test locks requested with RequestNamedLWLockTranche
 */
PG_FUNCTION_INFO_V1(test_startup_lwlocks);
Datum
test_startup_lwlocks(PG_FUNCTION_ARGS)
{
	LWLockPadded *lwlock_startup;
	LWLockPadded *lwlock_startup10;

	/* Check that the locks can be used */
	lwlock_startup = GetNamedLWLockTranche("test_lwlock_tranches_startup");
	lwlock_startup10 = GetNamedLWLockTranche("test_lwlock_tranches_startup10");

	LWLockAcquire(&lwlock_startup->lock, LW_EXCLUSIVE);
	for (int i = 0; i < 10; i++)
		LWLockAcquire(&lwlock_startup10[i].lock, LW_EXCLUSIVE);

	LWLockRelease(&lwlock_startup->lock);
	for (int i = 0; i < 10; i++)
		LWLockRelease(&lwlock_startup10[i].lock);

	/*
	 * Check that GetLWLockIdentifier() returns the expected value for
	 * tranches
	 */
	if (strcmp("test_lwlock_tranches_startup",
			   GetLWLockIdentifier(PG_WAIT_LWLOCK, LWTRANCHE_FIRST_USER_DEFINED)) != 0)
		elog(ERROR, "incorrect startup lock tranche name");
	if (strcmp("test_lwlock_tranches_startup10",
			   GetLWLockIdentifier(PG_WAIT_LWLOCK, LWTRANCHE_FIRST_USER_DEFINED + 1)) != 0)
		elog(ERROR, "incorrect startup lock tranche name");

	PG_RETURN_VOID();
}

/*
 * Wrapper for LWLockNewTrancheId().
 */
PG_FUNCTION_INFO_V1(test_lwlock_tranche_create);
Datum
test_lwlock_tranche_create(PG_FUNCTION_ARGS)
{
	char	   *tranche_name = PG_ARGISNULL(0) ? NULL : TextDatumGetCString(PG_GETARG_DATUM(0));
	int			tranche_id;

	tranche_id = LWLockNewTrancheId(tranche_name);

	PG_RETURN_INT32(tranche_id);
}

/*
 * Wrapper for GetNamedLWLockTranche().
 */
PG_FUNCTION_INFO_V1(test_lwlock_tranche_lookup);
Datum
test_lwlock_tranche_lookup(PG_FUNCTION_ARGS)
{
	char	   *tranche_name = TextDatumGetCString(PG_GETARG_DATUM(0));

	(void) GetNamedLWLockTranche(tranche_name);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(test_lwlock_get_lwlock_identifier);
Datum
test_lwlock_get_lwlock_identifier(PG_FUNCTION_ARGS)
{
	int			eventId = PG_GETARG_INT32(0);
	const char *tranche_name;

	tranche_name = GetLWLockIdentifier(PG_WAIT_LWLOCK, eventId);

	if (tranche_name)
		PG_RETURN_TEXT_P(cstring_to_text(tranche_name));
	else
		PG_RETURN_NULL();
}

/*
 * Wrapper for LWLockInitialize().
 */
PG_FUNCTION_INFO_V1(test_lwlock_initialize);
Datum
test_lwlock_initialize(PG_FUNCTION_ARGS)
{
	int			tranche_id = PG_GETARG_INT32(0);
	LWLock		lock;

	LWLockInitialize(&lock, tranche_id);

	PG_RETURN_VOID();
}
