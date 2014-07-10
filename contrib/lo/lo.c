/*
 *	PostgreSQL definitions for managed Large Objects.
 *
 *	contrib/lo/lo.c
 *
 */

#include "postgres.h"

#include "commands/trigger.h"
#include "executor/spi.h"
#include "libpq/be-fsstubs.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

#define atooid(x)  ((Oid) strtoul((x), NULL, 10))


/*
 * This is the trigger that protects us from orphaned large objects
 */
PG_FUNCTION_INFO_V1(lo_manage);

Datum
lo_manage(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	int			attnum;			/* attribute number to monitor	*/
	char	  **args;			/* Args containing attr name	*/
	TupleDesc	tupdesc;		/* Tuple Descriptor				*/
	HeapTuple	rettuple;		/* Tuple to be returned			*/
	bool		isdelete;		/* are we deleting?				*/
	HeapTuple	newtuple;		/* The new value for tuple		*/
	HeapTuple	trigtuple;		/* The original value of tuple	*/

	if (!CALLED_AS_TRIGGER(fcinfo))		/* internal error */
		elog(ERROR, "%s: not fired by trigger manager",
			 trigdata->tg_trigger->tgname);

	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))		/* internal error */
		elog(ERROR, "%s: must be fired for row",
			 trigdata->tg_trigger->tgname);

	/*
	 * Fetch some values from trigdata
	 */
	newtuple = trigdata->tg_newtuple;
	trigtuple = trigdata->tg_trigtuple;
	tupdesc = trigdata->tg_relation->rd_att;
	args = trigdata->tg_trigger->tgargs;

	if (args == NULL)			/* internal error */
		elog(ERROR, "%s: no column name provided in the trigger definition",
			 trigdata->tg_trigger->tgname);

	/* tuple to return to Executor */
	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		rettuple = newtuple;
	else
		rettuple = trigtuple;

	/* Are we deleting the row? */
	isdelete = TRIGGER_FIRED_BY_DELETE(trigdata->tg_event);

	/* Get the column we're interested in */
	attnum = SPI_fnumber(tupdesc, args[0]);

	if (attnum <= 0)
		elog(ERROR, "%s: column \"%s\" does not exist",
			 trigdata->tg_trigger->tgname, args[0]);

	/*
	 * Handle updates
	 *
	 * Here, if the value of the monitored attribute changes, then the large
	 * object associated with the original value is unlinked.
	 */
	if (newtuple != NULL)
	{
		char	   *orig = SPI_getvalue(trigtuple, tupdesc, attnum);
		char	   *newv = SPI_getvalue(newtuple, tupdesc, attnum);

		if (orig != NULL && (newv == NULL || strcmp(orig, newv) != 0))
			DirectFunctionCall1(lo_unlink,
								ObjectIdGetDatum(atooid(orig)));

		if (newv)
			pfree(newv);
		if (orig)
			pfree(orig);
	}

	/*
	 * Handle deleting of rows
	 *
	 * Here, we unlink the large object associated with the managed attribute
	 */
	if (isdelete)
	{
		char	   *orig = SPI_getvalue(trigtuple, tupdesc, attnum);

		if (orig != NULL)
		{
			DirectFunctionCall1(lo_unlink,
								ObjectIdGetDatum(atooid(orig)));

			pfree(orig);
		}
	}

	return PointerGetDatum(rettuple);
}
