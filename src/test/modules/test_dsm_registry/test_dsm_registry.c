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
#include "utils/builtins.h"

PG_MODULE_MAGIC;

typedef struct TestDSMRegistryStruct
{
	int			val;
	LWLock		lck;
} TestDSMRegistryStruct;

typedef struct TestDSMRegistryHashEntry
{
	char		key[64];
	dsa_pointer val;
} TestDSMRegistryHashEntry;

static TestDSMRegistryStruct *tdr_dsm;
static dsa_area *tdr_dsa;
static dshash_table *tdr_hash;

static const dshash_parameters dsh_params = {
	offsetof(TestDSMRegistryHashEntry, val),
	sizeof(TestDSMRegistryHashEntry),
	dshash_strcmp,
	dshash_strhash,
	dshash_strcpy
};

static void
init_tdr_dsm(void *ptr)
{
	TestDSMRegistryStruct *dsm = (TestDSMRegistryStruct *) ptr;

	LWLockInitialize(&dsm->lck, LWLockNewTrancheId());
	dsm->val = 0;
}

static void
tdr_attach_shmem(void)
{
	bool		found;

	tdr_dsm = GetNamedDSMSegment("test_dsm_registry_dsm",
								 sizeof(TestDSMRegistryStruct),
								 init_tdr_dsm,
								 &found);
	LWLockRegisterTranche(tdr_dsm->lck.tranche, "test_dsm_registry");

	if (tdr_dsa == NULL)
		tdr_dsa = GetNamedDSA("test_dsm_registry_dsa", &found);

	if (tdr_hash == NULL)
		tdr_hash = GetNamedDSHash("test_dsm_registry_hash", &dsh_params, &found);
}

PG_FUNCTION_INFO_V1(set_val_in_shmem);
Datum
set_val_in_shmem(PG_FUNCTION_ARGS)
{
	tdr_attach_shmem();

	LWLockAcquire(&tdr_dsm->lck, LW_EXCLUSIVE);
	tdr_dsm->val = PG_GETARG_INT32(0);
	LWLockRelease(&tdr_dsm->lck);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(get_val_in_shmem);
Datum
get_val_in_shmem(PG_FUNCTION_ARGS)
{
	int			ret;

	tdr_attach_shmem();

	LWLockAcquire(&tdr_dsm->lck, LW_SHARED);
	ret = tdr_dsm->val;
	LWLockRelease(&tdr_dsm->lck);

	PG_RETURN_INT32(ret);
}

PG_FUNCTION_INFO_V1(set_val_in_hash);
Datum
set_val_in_hash(PG_FUNCTION_ARGS)
{
	TestDSMRegistryHashEntry *entry;
	char	   *key = TextDatumGetCString(PG_GETARG_DATUM(0));
	char	   *val = TextDatumGetCString(PG_GETARG_DATUM(1));
	bool		found;

	if (strlen(key) >= offsetof(TestDSMRegistryHashEntry, val))
		ereport(ERROR,
				(errmsg("key too long")));

	tdr_attach_shmem();

	entry = dshash_find_or_insert(tdr_hash, key, &found);
	if (found)
		dsa_free(tdr_dsa, entry->val);

	entry->val = dsa_allocate(tdr_dsa, strlen(val) + 1);
	strcpy(dsa_get_address(tdr_dsa, entry->val), val);

	dshash_release_lock(tdr_hash, entry);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(get_val_in_hash);
Datum
get_val_in_hash(PG_FUNCTION_ARGS)
{
	TestDSMRegistryHashEntry *entry;
	char	   *key = TextDatumGetCString(PG_GETARG_DATUM(0));
	text	   *val = NULL;

	tdr_attach_shmem();

	entry = dshash_find(tdr_hash, key, false);
	if (entry == NULL)
		PG_RETURN_NULL();

	val = cstring_to_text(dsa_get_address(tdr_dsa, entry->val));

	dshash_release_lock(tdr_hash, entry);

	PG_RETURN_TEXT_P(val);
}
