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
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/injection_point.h"


PG_MODULE_MAGIC;

typedef struct TestShmemData
{
	bool		initialized;
	int			attach_count;
	char		dummy_data[FLEXIBLE_ARRAY_MEMBER];
} TestShmemData;

static TestShmemData *TestShmem;

#define DEFAULT_TEST_AREA_BYTES sizeof(TestShmemData)
#define MAX_TEST_AREA_BYTES 1000000

static bool attached_or_initialized = false;
static int	test_shmem_area_size = DEFAULT_TEST_AREA_BYTES;
static bool test_shmem_guc_defined = false;

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
					   .size = test_shmem_area_size,
					   .ptr = (void **) &TestShmem);
}

static void
test_shmem_init(void *arg)
{
	elog(LOG, "init callback called");

	INJECTION_POINT("test-shmem-init", NULL);

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

	if (!test_shmem_guc_defined)
	{
		/*
		 * The minimum size that makes sense is sizeof(TestShmemData), but we
		 * allow -1 so that we can test passing SHMEM_ATTACH_UNKNOWN_SIZE.
		 */
		DefineCustomIntVariable("test_shmem.area_size",
								"Size of the shmem area to request.",
								NULL,
								&test_shmem_area_size,
								DEFAULT_TEST_AREA_BYTES,
								-1,
								MAX_TEST_AREA_BYTES,
								PGC_USERSET,
								GUC_UNIT_BYTE,
								NULL, NULL, NULL);
		MarkGUCPrefixReserved("test_shmem");
		test_shmem_guc_defined = true;
	}
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


/*
 * Callback for test_shmem_register().  test_shmem_register() provides the
 * options, we just pass them through to ShmemRequestStruct.
 */
static void
test_shmem_after_startup_request(void *arg)
{
	ShmemStructOpts *opts = (ShmemStructOpts *) arg;

	elog(LOG, "test_shmem_after_startup_request callback called");

	ShmemRequestStructWithOpts(opts);
}

/*
 * Allocate or attach to a shmem structure, with the caller-supplied name and
 * size.
 *
 * The given integer 'new_value' is stored in the area, and the old value
 * is returned.
 */
PG_FUNCTION_INFO_V1(test_shmem_register);
Datum
test_shmem_register(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int64		size = PG_GETARG_INT64(1);
	int			new_value = PG_GETARG_INT32(2);
	int			old_value;
	int		   *attached = NULL;

	ShmemStructOpts opts = {
		.name = name,
		.size = size,
		.ptr = (void **) &attached,
	};

	ShmemCallbacks callbacks = {
		.flags = SHMEM_CALLBACKS_ALLOW_AFTER_STARTUP,
		.request_fn = test_shmem_after_startup_request,
		.opaque_arg = &opts,
	};

	RegisterShmemCallbacks(&callbacks);
	if (attached == NULL)
		elog(ERROR, "could not attach to shared memory");

	old_value = *attached;
	*attached = new_value;

	PG_RETURN_INT32(old_value);
}
