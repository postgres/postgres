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

HeapTuple	insert_username(void);

HeapTuple
insert_username()
{
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
	if (!CurrentTriggerData)
		elog(ERROR, "insert_username: triggers are not initialized");
	if (TRIGGER_FIRED_FOR_STATEMENT(CurrentTriggerData->tg_event))
		elog(ERROR, "insert_username: can't process STATEMENT events");
	if (TRIGGER_FIRED_AFTER(CurrentTriggerData->tg_event))
		elog(ERROR, "insert_username: must be fired before event");

	if (TRIGGER_FIRED_BY_INSERT(CurrentTriggerData->tg_event))
		rettuple = CurrentTriggerData->tg_trigtuple;
	else if (TRIGGER_FIRED_BY_UPDATE(CurrentTriggerData->tg_event))
		rettuple = CurrentTriggerData->tg_newtuple;
	else
		elog(ERROR, "insert_username: can't process DELETE events");

	rel = CurrentTriggerData->tg_relation;
	relname = SPI_getrelname(rel);

	trigger = CurrentTriggerData->tg_trigger;

	nargs = trigger->tgnargs;
	if (nargs != 1)
		elog(ERROR, "insert_username (%s): one argument was expected", relname);

	args = trigger->tgargs;
	tupdesc = rel->rd_att;

	CurrentTriggerData = NULL;

	attnum = SPI_fnumber(tupdesc, args[0]);

	if (attnum < 0)
		elog(ERROR, "insert_username (%s): there is no attribute %s", relname, args[0]);
	if (SPI_gettypeid(tupdesc, attnum) != TEXTOID)
		elog(ERROR, "insert_username (%s): attribute %s must be of TEXT type",
			 relname, args[0]);

	/* create fields containing name */
	newval = PointerGetDatum(textin(GetPgUserName()));

	/* construct new tuple */
	rettuple = SPI_modifytuple(rel, rettuple, 1, &attnum, &newval, NULL);
	if (rettuple == NULL)
		elog(ERROR, "insert_username (%s): %d returned by SPI_modifytuple",
			 relname, SPI_result);

	pfree(relname);

	return (rettuple);
}
