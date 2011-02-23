/*
 * insert_username.c
 * $Modified: Thu Oct 16 08:13:42 1997 by brook $
 * contrib/spi/insert_username.c
 *
 * insert user name in response to a trigger
 * usage:  insert_username (column_name)
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

extern Datum insert_username(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(insert_username);

Datum
insert_username(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	Trigger    *trigger;		/* to get trigger name */
	int			nargs;			/* # of arguments */
	Datum		newval;			/* new value of column */
	char	  **args;			/* arguments */
	char	   *relname;		/* triggered relation name */
	Relation	rel;			/* triggered relation */
	HeapTuple	rettuple = NULL;
	TupleDesc	tupdesc;		/* tuple description */
	int			attnum;

	/* sanity checks from autoinc.c */
	if (!CALLED_AS_TRIGGER(fcinfo))
		/* internal error */
		elog(ERROR, "insert_username: not fired by trigger manager");
	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		/* internal error */
		elog(ERROR, "insert_username: must be fired for row");
	if (!TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		/* internal error */
		elog(ERROR, "insert_username: must be fired before event");

	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		rettuple = trigdata->tg_trigtuple;
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		rettuple = trigdata->tg_newtuple;
	else
		/* internal error */
		elog(ERROR, "insert_username: cannot process DELETE events");

	rel = trigdata->tg_relation;
	relname = SPI_getrelname(rel);

	trigger = trigdata->tg_trigger;

	nargs = trigger->tgnargs;
	if (nargs != 1)
		/* internal error */
		elog(ERROR, "insert_username (%s): one argument was expected", relname);

	args = trigger->tgargs;
	tupdesc = rel->rd_att;

	attnum = SPI_fnumber(tupdesc, args[0]);

	if (attnum < 0)
		ereport(ERROR,
				(errcode(ERRCODE_TRIGGERED_ACTION_EXCEPTION),
				 errmsg("\"%s\" has no attribute \"%s\"", relname, args[0])));

	if (SPI_gettypeid(tupdesc, attnum) != TEXTOID)
		ereport(ERROR,
				(errcode(ERRCODE_TRIGGERED_ACTION_EXCEPTION),
				 errmsg("attribute \"%s\" of \"%s\" must be type TEXT",
						args[0], relname)));

	/* create fields containing name */
	newval = CStringGetTextDatum(GetUserNameFromId(GetUserId()));

	/* construct new tuple */
	rettuple = SPI_modifytuple(rel, rettuple, 1, &attnum, &newval, NULL);
	if (rettuple == NULL)
		/* internal error */
		elog(ERROR, "insert_username (\"%s\"): %d returned by SPI_modifytuple",
			 relname, SPI_result);

	pfree(relname);

	return PointerGetDatum(rettuple);
}
