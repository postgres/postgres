/*--------------------------------------------------------------------------
 *
 * test_tam_options.c
 *		Test code for table access method reloptions.
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_tam_options/test_tam_options.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "access/tableam.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(heap_alter_options_tam_handler);

/* An alternative relation options for heap */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	bool		enable_parallel;	/* enable parallel scans? */
} HeapAlterRdOptions;

static bytea *
heap_alter_reloptions(char relkind, Datum reloptions,
					  CommonRdOptions *common, bool validate)
{
	local_relopts relopts;
	HeapAlterRdOptions *result;

	Assert(relkind == RELKIND_RELATION ||
		   relkind == RELKIND_TOASTVALUE ||
		   relkind == RELKIND_MATVIEW);

	init_local_reloptions(&relopts, sizeof(HeapAlterRdOptions));
	add_local_bool_reloption(&relopts, "enable_parallel",
							 "enable parallel scan", true,
							 offsetof(HeapAlterRdOptions, enable_parallel));

	result = (HeapAlterRdOptions *) build_local_reloptions(&relopts,
														   reloptions,
														   validate);

	if (result != NULL && common != NULL)
	{
		common->parallel_workers = result->enable_parallel ? -1 : 0;
	}

	return (bytea *) result;
}

Datum
heap_alter_options_tam_handler(PG_FUNCTION_ARGS)
{
	static TableAmRoutine tam_routine;

	tam_routine = *GetHeapamTableAmRoutine();
	tam_routine.reloptions = heap_alter_reloptions;

	PG_RETURN_POINTER(&tam_routine);
}
