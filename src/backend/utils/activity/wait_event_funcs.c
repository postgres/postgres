/*------------------------------------------------------------------------
 *
 * wait_event_funcs.c
 *	  Functions for accessing wait event data.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/wait_event_funcs.c
 *
 *------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/wait_event.h"

/*
 * Each wait event has one corresponding entry in this structure, fed to
 * the SQL function of this file.
 */
static const struct
{
	const char *type;
	const char *name;
	const char *description;
}

			waitEventData[] =
{
#include "wait_event_funcs_data.c"
	/* end of list */
	{NULL, NULL, NULL}
};


/*
 * pg_get_wait_events
 *
 * List information about wait events (type, name and description).
 */
Datum
pg_get_wait_events(PG_FUNCTION_ARGS)
{
#define PG_GET_WAIT_EVENTS_COLS 3
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	  **waiteventnames;
	int			nbwaitevents;

	/* Build tuplestore to hold the result rows */
	InitMaterializedSRF(fcinfo, 0);

	/* Iterate over the list of wait events */
	for (int idx = 0; waitEventData[idx].type != NULL; idx++)
	{
		Datum		values[PG_GET_WAIT_EVENTS_COLS] = {0};
		bool		nulls[PG_GET_WAIT_EVENTS_COLS] = {0};

		values[0] = CStringGetTextDatum(waitEventData[idx].type);
		values[1] = CStringGetTextDatum(waitEventData[idx].name);
		values[2] = CStringGetTextDatum(waitEventData[idx].description);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	/* Handle custom wait events for extensions */
	waiteventnames = GetWaitEventCustomNames(PG_WAIT_EXTENSION,
											 &nbwaitevents);

	for (int idx = 0; idx < nbwaitevents; idx++)
	{
		StringInfoData buf;
		Datum		values[PG_GET_WAIT_EVENTS_COLS] = {0};
		bool		nulls[PG_GET_WAIT_EVENTS_COLS] = {0};


		values[0] = CStringGetTextDatum("Extension");
		values[1] = CStringGetTextDatum(waiteventnames[idx]);

		initStringInfo(&buf);
		appendStringInfo(&buf,
						 "Waiting for custom wait event \"%s\" defined by extension module",
						 waiteventnames[idx]);

		values[2] = CStringGetTextDatum(buf.data);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	/* Likewise for injection points */
	waiteventnames = GetWaitEventCustomNames(PG_WAIT_INJECTIONPOINT,
											 &nbwaitevents);

	for (int idx = 0; idx < nbwaitevents; idx++)
	{
		StringInfoData buf;
		Datum		values[PG_GET_WAIT_EVENTS_COLS] = {0};
		bool		nulls[PG_GET_WAIT_EVENTS_COLS] = {0};


		values[0] = CStringGetTextDatum("InjectionPoint");
		values[1] = CStringGetTextDatum(waiteventnames[idx]);

		initStringInfo(&buf);
		appendStringInfo(&buf,
						 "Waiting for injection point \"%s\"",
						 waiteventnames[idx]);

		values[2] = CStringGetTextDatum(buf.data);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}
