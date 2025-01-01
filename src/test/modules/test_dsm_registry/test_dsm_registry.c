/*--------------------------------------------------------------------------
 *
 * test_dsm_registry.c
 *	  Test the dynamic shared memory registry.
 *
 * Copyright (c) 2024-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_dsm_registry/test_dsm_registry.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "storage/dsm_registry.h"
#include "storage/lwlock.h"

PG_MODULE_MAGIC;

typedef struct TestDSMRegistryStruct
{
	int			val;
	LWLock		lck;
} TestDSMRegistryStruct;

static TestDSMRegistryStruct *tdr_state;

static void
tdr_init_shmem(void *ptr)
{
	TestDSMRegistryStruct *state = (TestDSMRegistryStruct *) ptr;

	LWLockInitialize(&state->lck, LWLockNewTrancheId());
	state->val = 0;
}

static void
tdr_attach_shmem(void)
{
	bool		found;

	tdr_state = GetNamedDSMSegment("test_dsm_registry",
								   sizeof(TestDSMRegistryStruct),
								   tdr_init_shmem,
								   &found);
	LWLockRegisterTranche(tdr_state->lck.tranche, "test_dsm_registry");
}

PG_FUNCTION_INFO_V1(set_val_in_shmem);
Datum
set_val_in_shmem(PG_FUNCTION_ARGS)
{
	tdr_attach_shmem();

	LWLockAcquire(&tdr_state->lck, LW_EXCLUSIVE);
	tdr_state->val = PG_GETARG_UINT32(0);
	LWLockRelease(&tdr_state->lck);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(get_val_in_shmem);
Datum
get_val_in_shmem(PG_FUNCTION_ARGS)
{
	int			ret;

	tdr_attach_shmem();

	LWLockAcquire(&tdr_state->lck, LW_SHARED);
	ret = tdr_state->val;
	LWLockRelease(&tdr_state->lck);

	PG_RETURN_UINT32(ret);
}
