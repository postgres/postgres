/*
 * noup.c --	functions to remove update permission from a column
 */

#include "executor/spi.h"		/* this is what you need to work with SPI */
#include "commands/trigger.h"	/* -"- and triggers */
#include <ctype.h>				/* tolower () */

extern Datum noup(PG_FUNCTION_ARGS);

/*
 * noup () -- revoke permission on column
 *
 * Though it's called without args You have to specify referenced
 * table/column while creating trigger:
 * EXECUTE PROCEDURE noup ('col').
 */

Datum
noup(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	Trigger    *trigger;		/* to get trigger name */
	int			nargs;			/* # of args specified in CREATE TRIGGER */
	char	  **args;			/* arguments: column names and table name */
	int			nkeys;			/* # of key columns (= nargs / 2) */
	Datum	   *kvals;			/* key values */
	Relation	rel;			/* triggered relation */
	HeapTuple	tuple = NULL;	/* tuple to return */
	TupleDesc	tupdesc;		/* tuple description */
	bool		isnull;			/* to know is some column NULL or not */
	int			ret;
	int			i;

	/*
	 * Some checks first...
	 */

	/* Called by trigger manager ? */
	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "noup: not fired by trigger manager");

	/* Should be called for ROW trigger */
	if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		elog(ERROR, "noup: can't process STATEMENT events");

	/* Not should be called for INSERT */
	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		elog(ERROR, "noup: can't process INSERT events");

	/* Not should be called for DELETE */
	else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
		elog(ERROR, "noup: can't process DELETE events");

	/* check new Tuple */
	tuple = trigdata->tg_newtuple;

	trigger = trigdata->tg_trigger;
	nargs = trigger->tgnargs;
	args = trigger->tgargs;

	nkeys = nargs;
	rel = trigdata->tg_relation;
	tupdesc = rel->rd_att;

	/* Connect to SPI manager */
	if ((ret = SPI_connect()) < 0)
		elog(ERROR, "noup: SPI_connect returned %d", ret);

	/*
	 * We use SPI plan preparation feature, so allocate space to place key
	 * values.
	 */
	kvals = (Datum *) palloc(nkeys * sizeof(Datum));

	/* For each column in key ... */
	for (i = 0; i < nkeys; i++)
	{
		/* get index of column in tuple */
		int			fnumber = SPI_fnumber(tupdesc, args[i]);

		/* Bad guys may give us un-existing column in CREATE TRIGGER */
		if (fnumber < 0)
			elog(ERROR, "noup: there is no attribute %s in relation %s",
				 args[i], SPI_getrelname(rel));

		/* Well, get binary (in internal format) value of column */
		kvals[i] = SPI_getbinval(tuple, tupdesc, fnumber, &isnull);

		/*
		 * If it's NOT NULL then cancel update
		 */
		if (!isnull)
		{

			elog(NOTICE, "%s: update not allowed", args[i]);
			SPI_finish();
			return PointerGetDatum(NULL);
		}

	}

	SPI_finish();
	return PointerGetDatum(tuple);
}
