/*--------------------------------------------------------------------------
 *
 * test_checksums.c
 *		Test data checksums
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_checksums/test_checksums.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/datachecksum_state.h"
#include "storage/latch.h"
#include "utils/injection_point.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC;

extern PGDLLEXPORT void dc_delay_barrier(const char *name, const void *private_data, void *arg);

/*
 * Test for delaying emission of procsignalbarriers.
 */
void
dc_delay_barrier(const char *name, const void *private_data, void *arg)
{
	(void) name;
	(void) private_data;

	(void) WaitLatch(MyLatch,
					 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					 (3 * 1000),
					 WAIT_EVENT_PG_SLEEP);
}

PG_FUNCTION_INFO_V1(dcw_inject_delay_barrier);
Datum
dcw_inject_delay_barrier(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	bool		attach = PG_GETARG_BOOL(0);

	if (attach)
		InjectionPointAttach("datachecksums-enable-checksums-delay",
							 "test_checksums",
							 "dc_delay_barrier",
							 NULL,
							 0);
	else
		InjectionPointDetach("datachecksums-enable-checksums-delay");
#else
	elog(ERROR,
		 "test is not working as intended when injection points are disabled");
#endif
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(dcw_inject_launcher_delay);
Datum
dcw_inject_launcher_delay(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	bool		attach = PG_GETARG_BOOL(0);

	if (attach)
		InjectionPointAttach("datachecksumsworker-launcher-delay",
							 "test_checksums",
							 "dc_delay_barrier",
							 NULL,
							 0);
	else
		InjectionPointDetach("datachecksumsworker-launcher-delay");
#else
	elog(ERROR,
		 "test is not working as intended when injection points are disabled");
#endif
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(dcw_inject_startup_delay);
Datum
dcw_inject_startup_delay(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	bool		attach = PG_GETARG_BOOL(0);

	if (attach)
		InjectionPointAttach("datachecksumsworker-startup-delay",
							 "test_checksums",
							 "dc_delay_barrier",
							 NULL,
							 0);
	else
		InjectionPointDetach("datachecksumsworker-startup-delay");
#else
	elog(ERROR,
		 "test is not working as intended when injection points are disabled");
#endif
	PG_RETURN_VOID();
}
