/*--------------------------------------------------------------------------
 *
 * test_dsa.c
 *		Test dynamic shared memory areas (DSAs)
 *
 * Copyright (c) 2022-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_dsa/test_dsa.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "storage/dsm_registry.h"
#include "storage/lwlock.h"
#include "utils/dsa.h"
#include "utils/resowner.h"

PG_MODULE_MAGIC;

static void
init_tranche(void *ptr)
{
	int		   *tranche_id = (int *) ptr;

	*tranche_id = LWLockNewTrancheId("test_dsa");
}

/* Test basic DSA functionality */
PG_FUNCTION_INFO_V1(test_dsa_basic);
Datum
test_dsa_basic(PG_FUNCTION_ARGS)
{
	int		   *tranche_id;
	bool		found;
	dsa_area   *a;
	dsa_pointer p[100];

	tranche_id = GetNamedDSMSegment("test_dsa", sizeof(int),
									init_tranche, &found);

	a = dsa_create(*tranche_id);
	for (int i = 0; i < 100; i++)
	{
		p[i] = dsa_allocate(a, 1000);
		snprintf(dsa_get_address(a, p[i]), 1000, "foobar%d", i);
	}

	for (int i = 0; i < 100; i++)
	{
		char		buf[100];

		snprintf(buf, 100, "foobar%d", i);
		if (strcmp(dsa_get_address(a, p[i]), buf) != 0)
			elog(ERROR, "no match");
	}

	for (int i = 0; i < 100; i++)
	{
		dsa_free(a, p[i]);
	}

	dsa_detach(a);

	PG_RETURN_VOID();
}

/* Test using DSA across different resource owners */
PG_FUNCTION_INFO_V1(test_dsa_resowners);
Datum
test_dsa_resowners(PG_FUNCTION_ARGS)
{
	int		   *tranche_id;
	bool		found;
	dsa_area   *a;
	dsa_pointer p[10000];
	ResourceOwner oldowner;
	ResourceOwner childowner;

	tranche_id = GetNamedDSMSegment("test_dsa", sizeof(int),
									init_tranche, &found);

	/* Create DSA in parent resource owner */
	a = dsa_create(*tranche_id);

	/*
	 * Switch to child resource owner, and do a bunch of allocations in the
	 * DSA
	 */
	oldowner = CurrentResourceOwner;
	childowner = ResourceOwnerCreate(oldowner, "test_dsa temp owner");
	CurrentResourceOwner = childowner;

	for (int i = 0; i < 10000; i++)
	{
		p[i] = dsa_allocate(a, 1000);
		snprintf(dsa_get_address(a, p[i]), 1000, "foobar%d", i);
	}

	/* Also test freeing, by freeing some of the allocations. */
	for (int i = 0; i < 500; i++)
		dsa_free(a, p[i]);

	/* Release the child resource owner */
	CurrentResourceOwner = oldowner;
	ResourceOwnerRelease(childowner,
						 RESOURCE_RELEASE_BEFORE_LOCKS,
						 true, false);
	ResourceOwnerRelease(childowner,
						 RESOURCE_RELEASE_LOCKS,
						 true, false);
	ResourceOwnerRelease(childowner,
						 RESOURCE_RELEASE_AFTER_LOCKS,
						 true, false);
	ResourceOwnerDelete(childowner);

	dsa_detach(a);

	PG_RETURN_VOID();
}
