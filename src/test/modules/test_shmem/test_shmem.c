/*-------------------------------------------------------------------------
 *
 * test_shmem.c
 *		Helpers to test shmem allocation routines
 *
 * Test basic memory allocation in an extension module. One notable feature
 * that is not exercised by any other module in the repository is the
 * allocating (non-DSM) shared memory after postmaster startup.
 *
 * Copyright (c) 2020-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_shmem/test_shmem.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/shmem.h"


PG_MODULE_MAGIC;

typedef struct TestShmemData
{
	int			value;
	bool		initialized;
	int			attach_count;
} TestShmemData;

static TestShmemData *TestShmem;

static bool attached_or_initialized = false;

static void test_shmem_request(void *arg);
static void test_shmem_init(void *arg);
static void test_shmem_attach(void *arg);

static const ShmemCallbacks TestShmemCallbacks = {
	.flags = SHMEM_CALLBACKS_ALLOW_AFTER_STARTUP,
	.request_fn = test_shmem_request,
	.init_fn = test_shmem_init,
	.attach_fn = test_shmem_attach,
};

static void
test_shmem_request(void *arg)
{
	elog(LOG, "test_shmem_request callback called");

	ShmemRequestStruct(.name = "test_shmem area",
					   .size = sizeof(TestShmemData),
					   .ptr = (void **) &TestShmem);
}

static void
test_shmem_init(void *arg)
{
	elog(LOG, "init callback called");
	if (TestShmem->initialized)
		elog(ERROR, "shmem area already initialized");
	TestShmem->initialized = true;

	if (attached_or_initialized)
		elog(ERROR, "attach or initialize already called in this process");
	attached_or_initialized = true;
}

static void
test_shmem_attach(void *arg)
{
	elog(LOG, "test_shmem_attach callback called");
	if (!TestShmem->initialized)
		elog(ERROR, "shmem area not yet initialized");
	TestShmem->attach_count++;

	if (attached_or_initialized)
		elog(ERROR, "attach or initialize already called in this process");
	attached_or_initialized = true;
}

void
_PG_init(void)
{
	elog(LOG, "test_shmem module's _PG_init called");
	RegisterShmemCallbacks(&TestShmemCallbacks);
}

PG_FUNCTION_INFO_V1(get_test_shmem_attach_count);
Datum
get_test_shmem_attach_count(PG_FUNCTION_ARGS)
{
	if (!attached_or_initialized)
		elog(ERROR, "shmem area not attached or initialized in this process");
	if (!TestShmem->initialized)
		elog(ERROR, "shmem area not yet initialized");
	PG_RETURN_INT32(TestShmem->attach_count);
}
