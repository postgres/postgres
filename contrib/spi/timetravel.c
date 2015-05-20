/*
 * contrib/spi/timetravel.c
 *
 *
 * timetravel.c --	function to get time travel feature
 *		using general triggers.
 *
 * Modified by BÖJTHE Zoltán, Hungary, mailto:urdesobt@axelero.hu
 */
#include "postgres.h"

#include <ctype.h>

#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/nabstime.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

/* AbsoluteTime currabstime(void); */

typedef struct
{
	char	   *ident;
	SPIPlanPtr	splan;
} EPlan;

static EPlan *Plans = NULL;		/* for UPDATE/DELETE */
static int	nPlans = 0;

typedef struct _TTOffList
{
	struct _TTOffList *next;
	char		name[FLEXIBLE_ARRAY_MEMBER];
} TTOffList;

static TTOffList *TTOff = NULL;

static int	findTTStatus(char *name);
static EPlan *find_plan(char *ident, EPlan **eplan, int *nplans);

/*
 * timetravel () --
 *		1.  IF an update affects tuple with stop_date eq INFINITY
 *			then form (and return) new tuple with start_date eq current date
 *			and stop_date eq INFINITY [ and update_user eq current user ]
 *			and all other column values as in new tuple, and insert tuple
 *			with old data and stop_date eq current date
 *			ELSE - skip updation of tuple.
 *		2.  IF a delete affects tuple with stop_date eq INFINITY
 *			then insert the same tuple with stop_date eq current date
 *			[ and delete_user eq current user ]
 *			ELSE - skip deletion of tuple.
 *		3.  On INSERT, if start_date is NULL then current date will be
 *			inserted, if stop_date is NULL then INFINITY will be inserted.
 *			[ and insert_user eq current user, update_user and delete_user
 *			eq NULL ]
 *
 * In CREATE TRIGGER you are to specify start_date and stop_date column
 * names:
 * EXECUTE PROCEDURE
 * timetravel ('date_on', 'date_off' [,'insert_user', 'update_user', 'delete_user' ] ).
 */

#define MaxAttrNum	5
#define MinAttrNum	2

#define a_time_on	0
#define a_time_off	1
#define a_ins_user	2
#define a_upd_user	3
#define a_del_user	4

PG_FUNCTION_INFO_V1(timetravel);

Datum							/* have to return HeapTuple to Executor */
timetravel(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	Trigger    *trigger;		/* to get trigger name */
	int			argc;
	char	  **args;			/* arguments */
	int			attnum[MaxAttrNum];		/* fnumbers of start/stop columns */
	Datum		oldtimeon,
				oldtimeoff;
	Datum		newtimeon,
				newtimeoff,
				newuser,
				nulltext;
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
	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		elog(ERROR, "timetravel: must be fired for row");

	/* Should be called BEFORE */
	if (!TRIGGER_FIRED_BEFORE(trigdata->tg_event))
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
	if (0 == findTTStatus(relname))
	{
		/* OFF - nothing to do */
		pfree(relname);
		return PointerGetDatum((newtuple != NULL) ? newtuple : trigtuple);
	}

	trigger = trigdata->tg_trigger;

	argc = trigger->tgnargs;
	if (argc != MinAttrNum && argc != MaxAttrNum)
		elog(ERROR, "timetravel (%s): invalid (!= %d or %d) number of arguments %d",
			 relname, MinAttrNum, MaxAttrNum, trigger->tgnargs);

	args = trigger->tgargs;
	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	for (i = 0; i < MinAttrNum; i++)
	{
		attnum[i] = SPI_fnumber(tupdesc, args[i]);
		if (attnum[i] < 0)
			elog(ERROR, "timetravel (%s): there is no attribute %s", relname, args[i]);
		if (SPI_gettypeid(tupdesc, attnum[i]) != ABSTIMEOID)
			elog(ERROR, "timetravel (%s): attribute %s must be of abstime type",
				 relname, args[i]);
	}
	for (; i < argc; i++)
	{
		attnum[i] = SPI_fnumber(tupdesc, args[i]);
		if (attnum[i] < 0)
			elog(ERROR, "timetravel (%s): there is no attribute %s", relname, args[i]);
		if (SPI_gettypeid(tupdesc, attnum[i]) != TEXTOID)
			elog(ERROR, "timetravel (%s): attribute %s must be of text type",
				 relname, args[i]);
	}

	/* create fields containing name */
	newuser = CStringGetTextDatum(GetUserNameFromId(GetUserId(), false));

	nulltext = (Datum) NULL;

	if (isinsert)
	{							/* INSERT */
		int			chnattrs = 0;
		int			chattrs[MaxAttrNum];
		Datum		newvals[MaxAttrNum];
		char		newnulls[MaxAttrNum];

		oldtimeon = SPI_getbinval(trigtuple, tupdesc, attnum[a_time_on], &isnull);
		if (isnull)
		{
			newvals[chnattrs] = GetCurrentAbsoluteTime();
			newnulls[chnattrs] = ' ';
			chattrs[chnattrs] = attnum[a_time_on];
			chnattrs++;
		}

		oldtimeoff = SPI_getbinval(trigtuple, tupdesc, attnum[a_time_off], &isnull);
		if (isnull)
		{
			if ((chnattrs == 0 && DatumGetInt32(oldtimeon) >= NOEND_ABSTIME) ||
				(chnattrs > 0 && DatumGetInt32(newvals[a_time_on]) >= NOEND_ABSTIME))
				elog(ERROR, "timetravel (%s): %s is infinity", relname, args[a_time_on]);
			newvals[chnattrs] = NOEND_ABSTIME;
			newnulls[chnattrs] = ' ';
			chattrs[chnattrs] = attnum[a_time_off];
			chnattrs++;
		}
		else
		{
			if ((chnattrs == 0 && DatumGetInt32(oldtimeon) > DatumGetInt32(oldtimeoff)) ||
				(chnattrs > 0 && DatumGetInt32(newvals[a_time_on]) > DatumGetInt32(oldtimeoff)))
				elog(ERROR, "timetravel (%s): %s gt %s", relname, args[a_time_on], args[a_time_off]);
		}

		pfree(relname);
		if (chnattrs <= 0)
			return PointerGetDatum(trigtuple);

		if (argc == MaxAttrNum)
		{
			/* clear update_user value */
			newvals[chnattrs] = nulltext;
			newnulls[chnattrs] = 'n';
			chattrs[chnattrs] = attnum[a_upd_user];
			chnattrs++;
			/* clear delete_user value */
			newvals[chnattrs] = nulltext;
			newnulls[chnattrs] = 'n';
			chattrs[chnattrs] = attnum[a_del_user];
			chnattrs++;
			/* set insert_user value */
			newvals[chnattrs] = newuser;
			newnulls[chnattrs] = ' ';
			chattrs[chnattrs] = attnum[a_ins_user];
			chnattrs++;
		}
		rettuple = SPI_modifytuple(rel, trigtuple, chnattrs, chattrs, newvals, newnulls);
		return PointerGetDatum(rettuple);
		/* end of INSERT */
	}

	/* UPDATE/DELETE: */
	oldtimeon = SPI_getbinval(trigtuple, tupdesc, attnum[a_time_on], &isnull);
	if (isnull)
		elog(ERROR, "timetravel (%s): %s must be NOT NULL", relname, args[a_time_on]);

	oldtimeoff = SPI_getbinval(trigtuple, tupdesc, attnum[a_time_off], &isnull);
	if (isnull)
		elog(ERROR, "timetravel (%s): %s must be NOT NULL", relname, args[a_time_off]);

	/*
	 * If DELETE/UPDATE of tuple with stop_date neq INFINITY then say upper
	 * Executor to skip operation for this tuple
	 */
	if (newtuple != NULL)
	{							/* UPDATE */
		newtimeon = SPI_getbinval(newtuple, tupdesc, attnum[a_time_on], &isnull);
		if (isnull)
			elog(ERROR, "timetravel (%s): %s must be NOT NULL", relname, args[a_time_on]);

		newtimeoff = SPI_getbinval(newtuple, tupdesc, attnum[a_time_off], &isnull);
		if (isnull)
			elog(ERROR, "timetravel (%s): %s must be NOT NULL", relname, args[a_time_off]);

		if (oldtimeon != newtimeon || oldtimeoff != newtimeoff)
			elog(ERROR, "timetravel (%s): you cannot change %s and/or %s columns (use set_timetravel)",
				 relname, args[a_time_on], args[a_time_off]);
	}
	if (oldtimeoff != NOEND_ABSTIME)
	{							/* current record is a deleted/updated record */
		pfree(relname);
		return PointerGetDatum(NULL);
	}

	newtimeoff = GetCurrentAbsoluteTime();

	/* Connect to SPI manager */
	if ((ret = SPI_connect()) < 0)
		elog(ERROR, "timetravel (%s): SPI_connect returned %d", relname, ret);

	/* Fetch tuple values and nulls */
	cvals = (Datum *) palloc(natts * sizeof(Datum));
	cnulls = (char *) palloc(natts * sizeof(char));
	for (i = 0; i < natts; i++)
	{
		cvals[i] = SPI_getbinval(trigtuple, tupdesc, i + 1, &isnull);
		cnulls[i] = (isnull) ? 'n' : ' ';
	}

	/* change date column(s) */
	cvals[attnum[a_time_off] - 1] = newtimeoff; /* stop_date eq current date */
	cnulls[attnum[a_time_off] - 1] = ' ';

	if (!newtuple)
	{							/* DELETE */
		if (argc == MaxAttrNum)
		{
			cvals[attnum[a_del_user] - 1] = newuser;	/* set delete user */
			cnulls[attnum[a_del_user] - 1] = ' ';
		}
	}

	/*
	 * Construct ident string as TriggerName $ TriggeredRelationId and try to
	 * find prepared execution plan.
	 */
	snprintf(ident, sizeof(ident), "%s$%u", trigger->tgname, rel->rd_id);
	plan = find_plan(ident, &Plans, &nPlans);

	/* if there is no plan ... */
	if (plan->splan == NULL)
	{
		SPIPlanPtr	pplan;
		Oid		   *ctypes;
		char		sql[8192];
		char		separ = ' ';

		/* allocate ctypes for preparation */
		ctypes = (Oid *) palloc(natts * sizeof(Oid));

		/*
		 * Construct query: INSERT INTO _relation_ VALUES ($1, ...)
		 */
		snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES (", relname);
		for (i = 1; i <= natts; i++)
		{
			ctypes[i - 1] = SPI_gettypeid(tupdesc, i);
			if (!(tupdesc->attrs[i - 1]->attisdropped)) /* skip dropped columns */
			{
				snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), "%c$%d", separ, i);
				separ = ',';
			}
		}
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), ")");

		elog(DEBUG4, "timetravel (%s) update: sql: %s", relname, sql);

		/* Prepare plan for query */
		pplan = SPI_prepare(sql, natts, ctypes);
		if (pplan == NULL)
			elog(ERROR, "timetravel (%s): SPI_prepare returned %d", relname, SPI_result);

		/*
		 * Remember that SPI_prepare places plan in current memory context -
		 * so, we have to save plan in Top memory context for later use.
		 */
		if (SPI_keepplan(pplan))
			elog(ERROR, "timetravel (%s): SPI_keepplan failed", relname);

		plan->splan = pplan;
	}

	/*
	 * Ok, execute prepared plan.
	 */
	ret = SPI_execp(plan->splan, cvals, cnulls, 0);

	if (ret < 0)
		elog(ERROR, "timetravel (%s): SPI_execp returned %d", relname, ret);

	/* Tuple to return to upper Executor ... */
	if (newtuple)
	{							/* UPDATE */
		int			chnattrs = 0;
		int			chattrs[MaxAttrNum];
		Datum		newvals[MaxAttrNum];
		char		newnulls[MaxAttrNum];

		newvals[chnattrs] = newtimeoff;
		newnulls[chnattrs] = ' ';
		chattrs[chnattrs] = attnum[a_time_on];
		chnattrs++;

		newvals[chnattrs] = NOEND_ABSTIME;
		newnulls[chnattrs] = ' ';
		chattrs[chnattrs] = attnum[a_time_off];
		chnattrs++;

		if (argc == MaxAttrNum)
		{
			/* set update_user value */
			newvals[chnattrs] = newuser;
			newnulls[chnattrs] = ' ';
			chattrs[chnattrs] = attnum[a_upd_user];
			chnattrs++;
			/* clear delete_user value */
			newvals[chnattrs] = nulltext;
			newnulls[chnattrs] = 'n';
			chattrs[chnattrs] = attnum[a_del_user];
			chnattrs++;
			/* set insert_user value */
			newvals[chnattrs] = nulltext;
			newnulls[chnattrs] = 'n';
			chattrs[chnattrs] = attnum[a_ins_user];
			chnattrs++;
		}

		rettuple = SPI_modifytuple(rel, newtuple, chnattrs, chattrs, newvals, newnulls);

		/*
		 * SPI_copytuple allocates tmptuple in upper executor context - have
		 * to free allocation using SPI_pfree
		 */
		/* SPI_pfree(tmptuple); */
	}
	else
		/* DELETE case */
		rettuple = trigtuple;

	SPI_finish();				/* don't forget say Bye to SPI mgr */

	pfree(relname);
	return PointerGetDatum(rettuple);
}

/*
 * set_timetravel (relname, on) --
 *					turn timetravel for specified relation ON/OFF
 */
PG_FUNCTION_INFO_V1(set_timetravel);

Datum
set_timetravel(PG_FUNCTION_ARGS)
{
	Name		relname = PG_GETARG_NAME(0);
	int32		on = PG_GETARG_INT32(1);
	char	   *rname;
	char	   *d;
	char	   *s;
	int32		ret;
	TTOffList  *prev,
			   *pp;

	prev = NULL;
	for (pp = TTOff; pp; prev = pp, pp = pp->next)
	{
		if (namestrcmp(relname, pp->name) == 0)
			break;
	}
	if (pp)
	{
		/* OFF currently */
		if (on != 0)
		{
			/* turn ON */
			if (prev)
				prev->next = pp->next;
			else
				TTOff = pp->next;
			free(pp);
		}
		ret = 0;
	}
	else
	{
		/* ON currently */
		if (on == 0)
		{
			/* turn OFF */
			s = rname = DatumGetCString(DirectFunctionCall1(nameout, NameGetDatum(relname)));
			if (s)
			{
				pp = malloc(offsetof(TTOffList, name) +strlen(rname) + 1);
				if (pp)
				{
					pp->next = NULL;
					d = pp->name;
					while (*s)
						*d++ = tolower((unsigned char) *s++);
					*d = '\0';
					if (prev)
						prev->next = pp;
					else
						TTOff = pp;
				}
				pfree(rname);
			}
		}
		ret = 1;
	}
	PG_RETURN_INT32(ret);
}

/*
 * get_timetravel (relname) --
 *	get timetravel status for specified relation (ON/OFF)
 */
PG_FUNCTION_INFO_V1(get_timetravel);

Datum
get_timetravel(PG_FUNCTION_ARGS)
{
	Name		relname = PG_GETARG_NAME(0);
	TTOffList  *pp;

	for (pp = TTOff; pp; pp = pp->next)
	{
		if (namestrcmp(relname, pp->name) == 0)
			PG_RETURN_INT32(0);
	}
	PG_RETURN_INT32(1);
}

static int
findTTStatus(char *name)
{
	TTOffList  *pp;

	for (pp = TTOff; pp; pp = pp->next)
		if (pg_strcasecmp(name, pp->name) == 0)
			return 0;
	return 1;
}

/*
AbsoluteTime
currabstime()
{
	return (GetCurrentAbsoluteTime());
}
*/

static EPlan *
find_plan(char *ident, EPlan **eplan, int *nplans)
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

	newp->ident = strdup(ident);
	newp->splan = NULL;
	(*nplans)++;

	return (newp);
}
