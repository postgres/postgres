/*-------------------------------------------------------------------------
 *
 * tcn.c
 *	  triggered change notification support for PostgreSQL
 *
 * Portions Copyright (c) 2011-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  contrib/tcn/tcn.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "commands/async.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "utils/rel.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC_EXT(
					.name = "tcn",
					.version = PG_VERSION
);

/*
 * Copy from s (for source) to r (for result), wrapping with q (quote)
 * characters and doubling any quote characters found.
 */
static void
strcpy_quoted(StringInfo r, const char *s, const char q)
{
	appendStringInfoCharMacro(r, q);
	while (*s)
	{
		if (*s == q)
			appendStringInfoCharMacro(r, q);
		appendStringInfoCharMacro(r, *s);
		s++;
	}
	appendStringInfoCharMacro(r, q);
}

/*
 * triggered_change_notification
 *
 * This trigger function will send a notification of data modification with
 * primary key values.  The channel will be "tcn" unless the trigger is
 * created with a parameter, in which case that parameter will be used.
 */
PG_FUNCTION_INFO_V1(triggered_change_notification);

Datum
triggered_change_notification(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	Trigger    *trigger;
	int			nargs;
	HeapTuple	trigtuple;
	Relation	rel;
	TupleDesc	tupdesc;
	char	   *channel;
	char		operation;
	StringInfo	payload = makeStringInfo();
	bool		foundPK;

	List	   *indexoidlist;
	ListCell   *indexoidscan;

	/* make sure it's called as a trigger */
	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("triggered_change_notification: must be called as trigger")));

	/* and that it's called after the change */
	if (!TRIGGER_FIRED_AFTER(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("triggered_change_notification: must be called after the change")));

	/* and that it's called for each row */
	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("triggered_change_notification: must be called for each row")));

	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		operation = 'I';
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		operation = 'U';
	else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
		operation = 'D';
	else
	{
		elog(ERROR, "triggered_change_notification: trigger fired by unrecognized operation");
		operation = 'X';		/* silence compiler warning */
	}

	trigger = trigdata->tg_trigger;
	nargs = trigger->tgnargs;
	if (nargs > 1)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("triggered_change_notification: must not be called with more than one parameter")));

	if (nargs == 0)
		channel = "tcn";
	else
		channel = trigger->tgargs[0];

	/* get tuple data */
	trigtuple = trigdata->tg_trigtuple;
	rel = trigdata->tg_relation;
	tupdesc = rel->rd_att;

	foundPK = false;

	/*
	 * Get the list of index OIDs for the table from the relcache, and look up
	 * each one in the pg_index syscache until we find one marked primary key
	 * (hopefully there isn't more than one such).
	 */
	indexoidlist = RelationGetIndexList(rel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		HeapTuple	indexTuple;
		Form_pg_index index;

		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		if (!HeapTupleIsValid(indexTuple))	/* should not happen */
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		index = (Form_pg_index) GETSTRUCT(indexTuple);
		/* we're only interested if it is the primary key and valid */
		if (index->indisprimary && index->indisvalid)
		{
			int			indnkeyatts = index->indnkeyatts;

			if (indnkeyatts > 0)
			{
				int			i;

				foundPK = true;

				strcpy_quoted(payload, RelationGetRelationName(rel), '"');
				appendStringInfoCharMacro(payload, ',');
				appendStringInfoCharMacro(payload, operation);

				for (i = 0; i < indnkeyatts; i++)
				{
					int			colno = index->indkey.values[i];
					Form_pg_attribute attr = TupleDescAttr(tupdesc, colno - 1);

					appendStringInfoCharMacro(payload, ',');
					strcpy_quoted(payload, NameStr(attr->attname), '"');
					appendStringInfoCharMacro(payload, '=');
					strcpy_quoted(payload, SPI_getvalue(trigtuple, tupdesc, colno), '\'');
				}

				Async_Notify(channel, payload->data);
			}
			ReleaseSysCache(indexTuple);
			break;
		}
		ReleaseSysCache(indexTuple);
	}

	list_free(indexoidlist);

	if (!foundPK)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("triggered_change_notification: must be called on a table with a primary key")));

	return PointerGetDatum(NULL);	/* after trigger; value doesn't matter */
}
