/*
 * insert_username.c
 * $Modified: Thu Oct 16 08:13:42 1997 by brook $
 *
 * insert user name in response to a trigger
 * usage:  insert_username (column_name)
 */

#include "executor/spi.h"		/* this is what you need to work with SPI */
#include "commands/trigger.h"	/* -"- and triggers */
#include "miscadmin.h"			/* for GetPgUserName() */

extern Datum	insert_username(PG_FUNCTION_ARGS);

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
		elog(ERROR, "insert_username: not fired by trigger manager");
	if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		elog(ERROR, "insert_username: can't process STATEMENT events");
	if (TRIGGER_FIRED_AFTER(trigdata->tg_event))
		elog(ERROR, "insert_username: must be fired before event");

	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		rettuple = trigdata->tg_trigtuple;
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		rettuple = trigdata->tg_newtuple;
	else
		elog(ERROR, "insert_username: can't process DELETE events");

	rel = trigdata->tg_relation;
	relname = SPI_getrelname(rel);

	trigger = trigdata->tg_trigger;

	nargs = trigger->tgnargs;
	if (nargs != 1)
		elog(ERROR, "insert_username (%s): one argument was expected", relname);

	args = trigger->tgargs;
	tupdesc = rel->rd_att;

	attnum = SPI_fnumber(tupdesc, args[0]);

	if (attnum < 0)
		elog(ERROR, "insert_username (%s): there is no attribute %s", relname, args[0]);
	if (SPI_gettypeid(tupdesc, attnum) != TEXTOID)
		elog(ERROR, "insert_username (%s): attribute %s must be of TEXT type",
			 relname, args[0]);

	/* create fields containing name */
	newval = DirectFunctionCall1(textin, CStringGetDatum(GetPgUserName()));

	/* construct new tuple */
	rettuple = SPI_modifytuple(rel, rettuple, 1, &attnum, &newval, NULL);
	if (rettuple == NULL)
		elog(ERROR, "insert_username (%s): %d returned by SPI_modifytuple",
			 relname, SPI_result);

	pfree(relname);

	return PointerGetDatum(rettuple);
}
