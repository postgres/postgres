
#include "executor/spi.h"		/* this is what you need to work with SPI */
#include "commands/trigger.h"	/* -"- and triggers */
#include "commands/sequence.h"	/* for nextval() */

extern Datum autoinc(PG_FUNCTION_ARGS);

Datum
autoinc(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	Trigger    *trigger;		/* to get trigger name */
	int			nargs;			/* # of arguments */
	int		   *chattrs;		/* attnums of attributes to change */
	int			chnattrs = 0;	/* # of above */
	Datum	   *newvals;		/* vals of above */
	char	  **args;			/* arguments */
	char	   *relname;		/* triggered relation name */
	Relation	rel;			/* triggered relation */
	HeapTuple	rettuple = NULL;
	TupleDesc	tupdesc;		/* tuple description */
	bool		isnull;
	int			i;

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "autoinc: not fired by trigger manager");
	if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		elog(ERROR, "autoinc: can't process STATEMENT events");
	if (TRIGGER_FIRED_AFTER(trigdata->tg_event))
		elog(ERROR, "autoinc: must be fired before event");

	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		rettuple = trigdata->tg_trigtuple;
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		rettuple = trigdata->tg_newtuple;
	else
		elog(ERROR, "autoinc: can't process DELETE events");

	rel = trigdata->tg_relation;
	relname = SPI_getrelname(rel);

	trigger = trigdata->tg_trigger;

	nargs = trigger->tgnargs;
	if (nargs <= 0 || nargs % 2 != 0)
		elog(ERROR, "autoinc (%s): even number gt 0 of arguments was expected", relname);

	args = trigger->tgargs;
	tupdesc = rel->rd_att;

	chattrs = (int *) palloc(nargs / 2 * sizeof(int));
	newvals = (Datum *) palloc(nargs / 2 * sizeof(Datum));

	for (i = 0; i < nargs;)
	{
		int			attnum = SPI_fnumber(tupdesc, args[i]);
		int32		val;
		Datum		seqname;

		if (attnum < 0)
			elog(ERROR, "autoinc (%s): there is no attribute %s",
				 relname, args[i]);
		if (SPI_gettypeid(tupdesc, attnum) != INT4OID)
			elog(ERROR, "autoinc (%s): attribute %s must be of INT4 type",
				 relname, args[i]);

		val = DatumGetInt32(SPI_getbinval(rettuple, tupdesc, attnum, &isnull));

		if (!isnull && val != 0)
		{
			i += 2;
			continue;
		}

		i++;
		chattrs[chnattrs] = attnum;
		seqname = DirectFunctionCall1(textin,
									  CStringGetDatum(args[i]));
		newvals[chnattrs] = DirectFunctionCall1(nextval, seqname);
		if (DatumGetInt32(newvals[chnattrs]) == 0)
			newvals[chnattrs] = DirectFunctionCall1(nextval, seqname);
		pfree(DatumGetTextP(seqname));
		chnattrs++;
		i++;
	}

	if (chnattrs > 0)
	{
		rettuple = SPI_modifytuple(rel, rettuple, chnattrs, chattrs, newvals, NULL);
		if (rettuple == NULL)
			elog(ERROR, "autoinc (%s): %d returned by SPI_modifytuple",
				 relname, SPI_result);
	}

	pfree(relname);
	pfree(chattrs);
	pfree(newvals);

	return PointerGetDatum(rettuple);
}
