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

#define STARTUP_TRANCHE_NAME "test_lwlock_tranches_startup"
#define DYNAMIC_TRANCHE_NAME "test_lwlock_tranches_dynamic"

#define NUM_STARTUP_TRANCHES (32)
#define NUM_DYNAMIC_TRANCHES (256 - NUM_STARTUP_TRANCHES)

#define GET_TRANCHE_NAME(a) GetLWLockIdentifier(PG_WAIT_LWLOCK, (a))

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

	for (int i = 0; i < NUM_STARTUP_TRANCHES; i++)
		RequestNamedLWLockTranche(STARTUP_TRANCHE_NAME, 1);
}

/*
 * Checks that GetLWLockIdentifier() returns the expected value for tranches
 * registered via RequestNamedLWLockTranche() and LWLockNewTrancheId().
 */
PG_FUNCTION_INFO_V1(test_lwlock_tranches);
Datum
test_lwlock_tranches(PG_FUNCTION_ARGS)
{
	int			dynamic_tranches[NUM_DYNAMIC_TRANCHES];

	for (int i = 0; i < NUM_DYNAMIC_TRANCHES; i++)
		dynamic_tranches[i] = LWLockNewTrancheId(DYNAMIC_TRANCHE_NAME);

	for (int i = 0; i < NUM_STARTUP_TRANCHES; i++)
	{
		if (strcmp(GET_TRANCHE_NAME(LWTRANCHE_FIRST_USER_DEFINED + i),
				   STARTUP_TRANCHE_NAME) != 0)
			elog(ERROR, "incorrect startup lock tranche name");
	}

	for (int i = 0; i < NUM_DYNAMIC_TRANCHES; i++)
	{
		if (strcmp(GET_TRANCHE_NAME(dynamic_tranches[i]),
				   DYNAMIC_TRANCHE_NAME) != 0)
			elog(ERROR, "incorrect dynamic lock tranche name");
	}

	PG_RETURN_VOID();
}

/*
 * Wrapper for LWLockNewTrancheId().
 */
PG_FUNCTION_INFO_V1(test_lwlock_tranche_creation);
Datum
test_lwlock_tranche_creation(PG_FUNCTION_ARGS)
{
	char	   *tranche_name = PG_ARGISNULL(0) ? NULL : TextDatumGetCString(PG_GETARG_DATUM(0));

	(void) LWLockNewTrancheId(tranche_name);

	PG_RETURN_VOID();
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
