/*
 * timetravel.c --	function to get time travel feature
 *		using general triggers.
 */

#include "executor/spi.h"		/* this is what you need to work with SPI */
#include "commands/trigger.h"	/* -"- and triggers */
#include <ctype.h>				/* tolower () */

#define ABSTIMEOID	702			/* it should be in pg_type.h */

AbsoluteTime currabstime(void);
Datum		timetravel(PG_FUNCTION_ARGS);
Datum		set_timetravel(PG_FUNCTION_ARGS);

typedef struct
{
	char	   *ident;
	void	   *splan;
}			EPlan;

static EPlan *Plans = NULL;		/* for UPDATE/DELETE */
static int	nPlans = 0;

static char **TTOff = NULL;
static int	nTTOff = 0;

static EPlan *find_plan(char *ident, EPlan ** eplan, int *nplans);

/*
 * timetravel () --
 *		1.	IF an update affects tuple with stop_date eq INFINITY
 *			then form (and return) new tuple with stop_date eq current date
 *			and all other column values as in old tuple, and insert tuple
 *			with new data and start_date eq current date and
 *			stop_date eq INFINITY
 *			ELSE - skip updation of tuple.
 *		2.	IF an delete affects tuple with stop_date eq INFINITY
 *			then insert the same tuple with stop_date eq current date
 *			ELSE - skip deletion of tuple.
 *		3.	On INSERT, if start_date is NULL then current date will be
 *			inserted, if stop_date is NULL then INFINITY will be inserted.
 *
 * In CREATE TRIGGER you are to specify start_date and stop_date column
 * names:
 * EXECUTE PROCEDURE
 * timetravel ('date_on', 'date_off').
 */

Datum
timetravel(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	Trigger    *trigger;		/* to get trigger name */
	char	  **args;			/* arguments */
	int			attnum[2];		/* fnumbers of start/stop columns */
	Datum		oldon,
				oldoff;
	Datum		newon,
				newoff;
	Datum	   *cvals;			/* column values */
	char	   *cnulls;			/* column nulls */
	char	   *relname;		/* triggered relation name */
	Relation	rel;			/* triggered relation */
	HeapTuple	trigtuple;
	HeapTuple	newtuple = NULL;
	HeapTuple	rettuple;
	TupleDesc	tupdesc;		/* tuple description */
	int			natts;			/* # of attributes */
	EPlan	   *plan;			/* prepared plan */
	char		ident[2 * NAMEDATALEN];
	bool		isnull;			/* to know is some column NULL or not */
	bool		isinsert = false;
	int			ret;
	int			i;

	/*
	 * Some checks first...
	 */

	/* Called by trigger manager ? */
	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "timetravel: not fired by trigger manager");

	/* Should be called for ROW trigger */
	if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		elog(ERROR, "timetravel: can't process STATEMENT events");

	/* Should be called BEFORE */
	if (TRIGGER_FIRED_AFTER(trigdata->tg_event))
		elog(ERROR, "timetravel: must be fired before event");

	/* INSERT ? */
	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		isinsert = true;

	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		newtuple = trigdata->tg_newtuple;

	trigtuple = trigdata->tg_trigtuple;

	rel = trigdata->tg_relation;
	relname = SPI_getrelname(rel);

	/* check if TT is OFF for this relation */
	for (i = 0; i < nTTOff; i++)
		if (strcasecmp(TTOff[i], relname) == 0)
			break;
	if (i < nTTOff)				/* OFF - nothing to do */
	{
		pfree(relname);
		return PointerGetDatum((newtuple != NULL) ? newtuple : trigtuple);
	}

	trigger = trigdata->tg_trigger;

	if (trigger->tgnargs != 2)
		elog(ERROR, "timetravel (%s): invalid (!= 2) number of arguments %d",
			 relname, trigger->tgnargs);

	args = trigger->tgargs;
	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	for (i = 0; i < 2; i++)
	{
		attnum[i] = SPI_fnumber(tupdesc, args[i]);
		if (attnum[i] < 0)
			elog(ERROR, "timetravel (%s): there is no attribute %s", relname, args[i]);
		if (SPI_gettypeid(tupdesc, attnum[i]) != ABSTIMEOID)
			elog(ERROR, "timetravel (%s): attributes %s and %s must be of abstime type",
				 relname, args[0], args[1]);
	}

	if (isinsert)				/* INSERT */
	{
		int			chnattrs = 0;
		int			chattrs[2];
		Datum		newvals[2];

		oldon = SPI_getbinval(trigtuple, tupdesc, attnum[0], &isnull);
		if (isnull)
		{
			newvals[chnattrs] = GetCurrentAbsoluteTime();
			chattrs[chnattrs] = attnum[0];
			chnattrs++;
		}

		oldoff = SPI_getbinval(trigtuple, tupdesc, attnum[1], &isnull);
		if (isnull)
		{
			if ((chnattrs == 0 && DatumGetInt32(oldon) >= NOEND_ABSTIME) ||
			(chnattrs > 0 && DatumGetInt32(newvals[0]) >= NOEND_ABSTIME))
				elog(ERROR, "timetravel (%s): %s ge %s",
					 relname, args[0], args[1]);
			newvals[chnattrs] = NOEND_ABSTIME;
			chattrs[chnattrs] = attnum[1];
			chnattrs++;
		}
		else
		{
			if ((chnattrs == 0 && DatumGetInt32(oldon) >=
				 DatumGetInt32(oldoff)) ||
				(chnattrs > 0 && DatumGetInt32(newvals[0]) >=
				 DatumGetInt32(oldoff)))
				elog(ERROR, "timetravel (%s): %s ge %s",
					 relname, args[0], args[1]);
		}

		pfree(relname);
		if (chnattrs <= 0)
			return PointerGetDatum(trigtuple);

		rettuple = SPI_modifytuple(rel, trigtuple, chnattrs,
								   chattrs, newvals, NULL);
		return PointerGetDatum(rettuple);
	}

	oldon = SPI_getbinval(trigtuple, tupdesc, attnum[0], &isnull);
	if (isnull)
		elog(ERROR, "timetravel (%s): %s must be NOT NULL", relname, args[0]);

	oldoff = SPI_getbinval(trigtuple, tupdesc, attnum[1], &isnull);
	if (isnull)
		elog(ERROR, "timetravel (%s): %s must be NOT NULL", relname, args[1]);

	/*
	 * If DELETE/UPDATE of tuple with stop_date neq INFINITY then say
	 * upper Executor to skip operation for this tuple
	 */
	if (newtuple != NULL)		/* UPDATE */
	{
		newon = SPI_getbinval(newtuple, tupdesc, attnum[0], &isnull);
		if (isnull)
			elog(ERROR, "timetravel (%s): %s must be NOT NULL", relname, args[0]);
		newoff = SPI_getbinval(newtuple, tupdesc, attnum[1], &isnull);
		if (isnull)
			elog(ERROR, "timetravel (%s): %s must be NOT NULL", relname, args[1]);

		if (oldon != newon || oldoff != newoff)
			elog(ERROR, "timetravel (%s): you can't change %s and/or %s columns (use set_timetravel)",
				 relname, args[0], args[1]);

		if (newoff != NOEND_ABSTIME)
		{
			pfree(relname);		/* allocated in upper executor context */
			return PointerGetDatum(NULL);
		}
	}
	else if (oldoff != NOEND_ABSTIME)	/* DELETE */
	{
		pfree(relname);
		return PointerGetDatum(NULL);
	}

	newoff = GetCurrentAbsoluteTime();

	/* Connect to SPI manager */
	if ((ret = SPI_connect()) < 0)
		elog(ERROR, "timetravel (%s): SPI_connect returned %d", relname, ret);

	/* Fetch tuple values and nulls */
	cvals = (Datum *) palloc(natts * sizeof(Datum));
	cnulls = (char *) palloc(natts * sizeof(char));
	for (i = 0; i < natts; i++)
	{
		cvals[i] = SPI_getbinval((newtuple != NULL) ? newtuple : trigtuple,
								 tupdesc, i + 1, &isnull);
		cnulls[i] = (isnull) ? 'n' : ' ';
	}

	/* change date column(s) */
	if (newtuple)				/* UPDATE */
	{
		cvals[attnum[0] - 1] = newoff;	/* start_date eq current date */
		cnulls[attnum[0] - 1] = ' ';
		cvals[attnum[1] - 1] = NOEND_ABSTIME;	/* stop_date eq INFINITY */
		cnulls[attnum[1] - 1] = ' ';
	}
	else
/* DELETE */
	{
		cvals[attnum[1] - 1] = newoff;	/* stop_date eq current date */
		cnulls[attnum[1] - 1] = ' ';
	}

	/*
	 * Construct ident string as TriggerName $ TriggeredRelationId and try
	 * to find prepared execution plan.
	 */
	sprintf(ident, "%s$%u", trigger->tgname, rel->rd_id);
	plan = find_plan(ident, &Plans, &nPlans);

	/* if there is no plan ... */
	if (plan->splan == NULL)
	{
		void	   *pplan;
		Oid		   *ctypes;
		char		sql[8192];

		/* allocate ctypes for preparation */
		ctypes = (Oid *) palloc(natts * sizeof(Oid));

		/*
		 * Construct query: INSERT INTO _relation_ VALUES ($1, ...)
		 */
		sprintf(sql, "INSERT INTO %s VALUES (", relname);
		for (i = 1; i <= natts; i++)
		{
			sprintf(sql + strlen(sql), "$%d%s",
					i, (i < natts) ? ", " : ")");
			ctypes[i - 1] = SPI_gettypeid(tupdesc, i);
		}

		/* Prepare plan for query */
		pplan = SPI_prepare(sql, natts, ctypes);
		if (pplan == NULL)
			elog(ERROR, "timetravel (%s): SPI_prepare returned %d", relname, SPI_result);

		/*
		 * Remember that SPI_prepare places plan in current memory context
		 * - so, we have to save plan in Top memory context for latter
		 * use.
		 */
		pplan = SPI_saveplan(pplan);
		if (pplan == NULL)
			elog(ERROR, "timetravel (%s): SPI_saveplan returned %d", relname, SPI_result);

		plan->splan = pplan;
	}

	/*
	 * Ok, execute prepared plan.
	 */
	ret = SPI_execp(plan->splan, cvals, cnulls, 0);

	if (ret < 0)
		elog(ERROR, "timetravel (%s): SPI_execp returned %d", relname, ret);

	/* Tuple to return to upper Executor ... */
	if (newtuple)				/* UPDATE */
	{
		HeapTuple	tmptuple;

		tmptuple = SPI_copytuple(trigtuple);
		rettuple = SPI_modifytuple(rel, tmptuple, 1, &(attnum[1]), &newoff, NULL);

		/*
		 * SPI_copytuple allocates tmptuple in upper executor context -
		 * have to free allocation using SPI_pfree
		 */
		SPI_pfree(tmptuple);
	}
	else
/* DELETE */
		rettuple = trigtuple;

	SPI_finish();				/* don't forget say Bye to SPI mgr */

	pfree(relname);

	return PointerGetDatum(rettuple);
}

/*
 * set_timetravel (relname, on) --
 *					turn timetravel for specified relation ON/OFF
 */
Datum
set_timetravel(PG_FUNCTION_ARGS)
{
	Name		relname = PG_GETARG_NAME(0);
	int32		on = PG_GETARG_INT32(1);
	char	   *rname;
	char	   *d;
	char	   *s;
	int			i;

	for (i = 0; i < nTTOff; i++)
		if (namestrcmp(relname, TTOff[i]) == 0)
			break;

	if (i < nTTOff)				/* OFF currently */
	{
		if (on == 0)
			PG_RETURN_INT32(0);

		/* turn ON */
		free(TTOff[i]);
		if (nTTOff == 1)
			free(TTOff);
		else
		{
			if (i < nTTOff - 1)
				memcpy(&(TTOff[i]), &(TTOff[i + 1]), (nTTOff - i) * sizeof(char *));
			TTOff = realloc(TTOff, (nTTOff - 1) * sizeof(char *));
		}
		nTTOff--;
		PG_RETURN_INT32(0);
	}

	/* ON currently */
	if (on != 0)
		PG_RETURN_INT32(1);

	/* turn OFF */
	if (nTTOff == 0)
		TTOff = malloc(sizeof(char *));
	else
		TTOff = realloc(TTOff, (nTTOff + 1) * sizeof(char *));
	s = rname = DatumGetCString(DirectFunctionCall1(nameout,
													NameGetDatum(relname)));
	d = TTOff[nTTOff] = malloc(strlen(rname) + 1);
	while (*s)
		*d++ = tolower(*s++);
	*d = 0;
	pfree(rname);
	nTTOff++;

	PG_RETURN_INT32(1);
}

AbsoluteTime
currabstime()
{
	return (GetCurrentAbsoluteTime());
}

static EPlan *
find_plan(char *ident, EPlan ** eplan, int *nplans)
{
	EPlan	   *newp;
	int			i;

	if (*nplans > 0)
	{
		for (i = 0; i < *nplans; i++)
		{
			if (strcmp((*eplan)[i].ident, ident) == 0)
				break;
		}
		if (i != *nplans)
			return (*eplan + i);
		*eplan = (EPlan *) realloc(*eplan, (i + 1) * sizeof(EPlan));
		newp = *eplan + i;
	}
	else
	{
		newp = *eplan = (EPlan *) malloc(sizeof(EPlan));
		(*nplans) = i = 0;
	}

	newp->ident = (char *) malloc(strlen(ident) + 1);
	strcpy(newp->ident, ident);
	newp->splan = NULL;
	(*nplans)++;

	return (newp);
}
