/*------------------------------------------------------------------------
 *
 * regress.c
 *	 Code for various C-language functions defined as part of the
 *	 regression tests.
 *
 * This code is released under the terms of the PostgreSQL License.
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/test/regress/regress.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <float.h>
#include <math.h>
#include <signal.h>

#include "access/htup_details.h"
#include "access/transam.h"
#include "access/tuptoaster.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/sequence.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "utils/builtins.h"
#include "utils/geo_decls.h"
#include "utils/rel.h"
#include "utils/typcache.h"
#include "utils/memutils.h"


#define LDELIM			'('
#define RDELIM			')'
#define DELIM			','

static void regress_lseg_construct(LSEG *lseg, Point *pt1, Point *pt2);

PG_MODULE_MAGIC;


/* return the point where two paths intersect, or NULL if no intersection. */
PG_FUNCTION_INFO_V1(interpt_pp);

Datum
interpt_pp(PG_FUNCTION_ARGS)
{
	PATH	   *p1 = PG_GETARG_PATH_P(0);
	PATH	   *p2 = PG_GETARG_PATH_P(1);
	int			i,
				j;
	LSEG		seg1,
				seg2;
	bool		found;			/* We've found the intersection */

	found = false;				/* Haven't found it yet */

	for (i = 0; i < p1->npts - 1 && !found; i++)
	{
		regress_lseg_construct(&seg1, &p1->p[i], &p1->p[i + 1]);
		for (j = 0; j < p2->npts - 1 && !found; j++)
		{
			regress_lseg_construct(&seg2, &p2->p[j], &p2->p[j + 1]);
			if (DatumGetBool(DirectFunctionCall2(lseg_intersect,
												 LsegPGetDatum(&seg1),
												 LsegPGetDatum(&seg2))))
				found = true;
		}
	}

	if (!found)
		PG_RETURN_NULL();

	/*
	 * Note: DirectFunctionCall2 will kick out an error if lseg_interpt()
	 * returns NULL, but that should be impossible since we know the two
	 * segments intersect.
	 */
	PG_RETURN_DATUM(DirectFunctionCall2(lseg_interpt,
										LsegPGetDatum(&seg1),
										LsegPGetDatum(&seg2)));
}


/* like lseg_construct, but assume space already allocated */
static void
regress_lseg_construct(LSEG *lseg, Point *pt1, Point *pt2)
{
	lseg->p[0].x = pt1->x;
	lseg->p[0].y = pt1->y;
	lseg->p[1].x = pt2->x;
	lseg->p[1].y = pt2->y;
}

PG_FUNCTION_INFO_V1(overpaid);

Datum
overpaid(PG_FUNCTION_ARGS)
{
	HeapTupleHeader tuple = PG_GETARG_HEAPTUPLEHEADER(0);
	bool		isnull;
	int32		salary;

	salary = DatumGetInt32(GetAttributeByName(tuple, "salary", &isnull));
	if (isnull)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(salary > 699);
}

/* New type "widget"
 * This used to be "circle", but I added circle to builtins,
 *	so needed to make sure the names do not collide. - tgl 97/04/21
 */

typedef struct
{
	Point		center;
	double		radius;
} WIDGET;

PG_FUNCTION_INFO_V1(widget_in);
PG_FUNCTION_INFO_V1(widget_out);

#define NARGS	3

Datum
widget_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	char	   *p,
			   *coord[NARGS];
	int			i;
	WIDGET	   *result;

	for (i = 0, p = str; *p && i < NARGS && *p != RDELIM; p++)
	{
		if (*p == DELIM || (*p == LDELIM && i == 0))
			coord[i++] = p + 1;
	}

	if (i < NARGS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type widget: \"%s\"",
						str)));

	result = (WIDGET *) palloc(sizeof(WIDGET));
	result->center.x = atof(coord[0]);
	result->center.y = atof(coord[1]);
	result->radius = atof(coord[2]);

	PG_RETURN_POINTER(result);
}

Datum
widget_out(PG_FUNCTION_ARGS)
{
	WIDGET	   *widget = (WIDGET *) PG_GETARG_POINTER(0);
	char	   *str = psprintf("(%g,%g,%g)",
							   widget->center.x, widget->center.y, widget->radius);

	PG_RETURN_CSTRING(str);
}

PG_FUNCTION_INFO_V1(pt_in_widget);

Datum
pt_in_widget(PG_FUNCTION_ARGS)
{
	Point	   *point = PG_GETARG_POINT_P(0);
	WIDGET	   *widget = (WIDGET *) PG_GETARG_POINTER(1);

	PG_RETURN_BOOL(point_dt(point, &widget->center) < widget->radius);
}

PG_FUNCTION_INFO_V1(reverse_name);

Datum
reverse_name(PG_FUNCTION_ARGS)
{
	char	   *string = PG_GETARG_CSTRING(0);
	int			i;
	int			len;
	char	   *new_string;

	new_string = palloc0(NAMEDATALEN);
	for (i = 0; i < NAMEDATALEN && string[i]; ++i)
		;
	if (i == NAMEDATALEN || !string[i])
		--i;
	len = i;
	for (; i >= 0; --i)
		new_string[len - i] = string[i];
	PG_RETURN_CSTRING(new_string);
}

PG_FUNCTION_INFO_V1(trigger_return_old);

Datum
trigger_return_old(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	HeapTuple	tuple;

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "trigger_return_old: not fired by trigger manager");

	tuple = trigdata->tg_trigtuple;

	return PointerGetDatum(tuple);
}

#define TTDUMMY_INFINITY	999999

static SPIPlanPtr splan = NULL;
static bool ttoff = false;

PG_FUNCTION_INFO_V1(ttdummy);

Datum
ttdummy(PG_FUNCTION_ARGS)
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
	bool		isnull;			/* to know is some column NULL or not */
	int			ret;
	int			i;

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "ttdummy: not fired by trigger manager");
	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		elog(ERROR, "ttdummy: must be fired for row");
	if (!TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		elog(ERROR, "ttdummy: must be fired before event");
	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		elog(ERROR, "ttdummy: cannot process INSERT event");
	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		newtuple = trigdata->tg_newtuple;

	trigtuple = trigdata->tg_trigtuple;

	rel = trigdata->tg_relation;
	relname = SPI_getrelname(rel);

	/* check if TT is OFF for this relation */
	if (ttoff)					/* OFF - nothing to do */
	{
		pfree(relname);
		return PointerGetDatum((newtuple != NULL) ? newtuple : trigtuple);
	}

	trigger = trigdata->tg_trigger;

	if (trigger->tgnargs != 2)
		elog(ERROR, "ttdummy (%s): invalid (!= 2) number of arguments %d",
			 relname, trigger->tgnargs);

	args = trigger->tgargs;
	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	for (i = 0; i < 2; i++)
	{
		attnum[i] = SPI_fnumber(tupdesc, args[i]);
		if (attnum[i] <= 0)
			elog(ERROR, "ttdummy (%s): there is no attribute %s",
				 relname, args[i]);
		if (SPI_gettypeid(tupdesc, attnum[i]) != INT4OID)
			elog(ERROR, "ttdummy (%s): attribute %s must be of integer type",
				 relname, args[i]);
	}

	oldon = SPI_getbinval(trigtuple, tupdesc, attnum[0], &isnull);
	if (isnull)
		elog(ERROR, "ttdummy (%s): %s must be NOT NULL", relname, args[0]);

	oldoff = SPI_getbinval(trigtuple, tupdesc, attnum[1], &isnull);
	if (isnull)
		elog(ERROR, "ttdummy (%s): %s must be NOT NULL", relname, args[1]);

	if (newtuple != NULL)		/* UPDATE */
	{
		newon = SPI_getbinval(newtuple, tupdesc, attnum[0], &isnull);
		if (isnull)
			elog(ERROR, "ttdummy (%s): %s must be NOT NULL", relname, args[0]);
		newoff = SPI_getbinval(newtuple, tupdesc, attnum[1], &isnull);
		if (isnull)
			elog(ERROR, "ttdummy (%s): %s must be NOT NULL", relname, args[1]);

		if (oldon != newon || oldoff != newoff)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("ttdummy (%s): you cannot change %s and/or %s columns (use set_ttdummy)",
							relname, args[0], args[1])));

		if (newoff != TTDUMMY_INFINITY)
		{
			pfree(relname);		/* allocated in upper executor context */
			return PointerGetDatum(NULL);
		}
	}
	else if (oldoff != TTDUMMY_INFINITY)	/* DELETE */
	{
		pfree(relname);
		return PointerGetDatum(NULL);
	}

	newoff = DirectFunctionCall1(nextval, CStringGetTextDatum("ttdummy_seq"));
	/* nextval now returns int64; coerce down to int32 */
	newoff = Int32GetDatum((int32) DatumGetInt64(newoff));

	/* Connect to SPI manager */
	if ((ret = SPI_connect()) < 0)
		elog(ERROR, "ttdummy (%s): SPI_connect returned %d", relname, ret);

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
		cvals[attnum[1] - 1] = TTDUMMY_INFINITY;	/* stop_date eq INFINITY */
		cnulls[attnum[1] - 1] = ' ';
	}
	else
		/* DELETE */
	{
		cvals[attnum[1] - 1] = newoff;	/* stop_date eq current date */
		cnulls[attnum[1] - 1] = ' ';
	}

	/* if there is no plan ... */
	if (splan == NULL)
	{
		SPIPlanPtr	pplan;
		Oid		   *ctypes;
		char	   *query;

		/* allocate space in preparation */
		ctypes = (Oid *) palloc(natts * sizeof(Oid));
		query = (char *) palloc(100 + 16 * natts);

		/*
		 * Construct query: INSERT INTO _relation_ VALUES ($1, ...)
		 */
		sprintf(query, "INSERT INTO %s VALUES (", relname);
		for (i = 1; i <= natts; i++)
		{
			sprintf(query + strlen(query), "$%d%s",
					i, (i < natts) ? ", " : ")");
			ctypes[i - 1] = SPI_gettypeid(tupdesc, i);
		}

		/* Prepare plan for query */
		pplan = SPI_prepare(query, natts, ctypes);
		if (pplan == NULL)
			elog(ERROR, "ttdummy (%s): SPI_prepare returned %s", relname, SPI_result_code_string(SPI_result));

		if (SPI_keepplan(pplan))
			elog(ERROR, "ttdummy (%s): SPI_keepplan failed", relname);

		splan = pplan;
	}

	ret = SPI_execp(splan, cvals, cnulls, 0);

	if (ret < 0)
		elog(ERROR, "ttdummy (%s): SPI_execp returned %d", relname, ret);

	/* Tuple to return to upper Executor ... */
	if (newtuple)				/* UPDATE */
		rettuple = SPI_modifytuple(rel, trigtuple, 1, &(attnum[1]), &newoff, NULL);
	else						/* DELETE */
		rettuple = trigtuple;

	SPI_finish();				/* don't forget say Bye to SPI mgr */

	pfree(relname);

	return PointerGetDatum(rettuple);
}

PG_FUNCTION_INFO_V1(set_ttdummy);

Datum
set_ttdummy(PG_FUNCTION_ARGS)
{
	int32		on = PG_GETARG_INT32(0);

	if (ttoff)					/* OFF currently */
	{
		if (on == 0)
			PG_RETURN_INT32(0);

		/* turn ON */
		ttoff = false;
		PG_RETURN_INT32(0);
	}

	/* ON currently */
	if (on != 0)
		PG_RETURN_INT32(1);

	/* turn OFF */
	ttoff = true;

	PG_RETURN_INT32(1);
}


/*
 * Type int44 has no real-world use, but the regression tests use it
 * (under the alias "city_budget").  It's a four-element vector of int4's.
 */

/*
 *		int44in			- converts "num, num, ..." to internal form
 *
 *		Note: Fills any missing positions with zeroes.
 */
PG_FUNCTION_INFO_V1(int44in);

Datum
int44in(PG_FUNCTION_ARGS)
{
	char	   *input_string = PG_GETARG_CSTRING(0);
	int32	   *result = (int32 *) palloc(4 * sizeof(int32));
	int			i;

	i = sscanf(input_string,
			   "%d, %d, %d, %d",
			   &result[0],
			   &result[1],
			   &result[2],
			   &result[3]);
	while (i < 4)
		result[i++] = 0;

	PG_RETURN_POINTER(result);
}

/*
 *		int44out		- converts internal form to "num, num, ..."
 */
PG_FUNCTION_INFO_V1(int44out);

Datum
int44out(PG_FUNCTION_ARGS)
{
	int32	   *an_array = (int32 *) PG_GETARG_POINTER(0);
	char	   *result = (char *) palloc(16 * 4);

	snprintf(result, 16 * 4, "%d,%d,%d,%d",
			 an_array[0],
			 an_array[1],
			 an_array[2],
			 an_array[3]);

	PG_RETURN_CSTRING(result);
}

PG_FUNCTION_INFO_V1(make_tuple_indirect);
Datum
make_tuple_indirect(PG_FUNCTION_ARGS)
{
	HeapTupleHeader rec = PG_GETARG_HEAPTUPLEHEADER(0);
	HeapTupleData tuple;
	int			ncolumns;
	Datum	   *values;
	bool	   *nulls;

	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;

	HeapTuple	newtup;

	int			i;

	MemoryContext old_context;

	/* Extract type info from the tuple itself */
	tupType = HeapTupleHeaderGetTypeId(rec);
	tupTypmod = HeapTupleHeaderGetTypMod(rec);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	ncolumns = tupdesc->natts;

	/* Build a temporary HeapTuple control structure */
	tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = rec;

	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));

	heap_deform_tuple(&tuple, tupdesc, values, nulls);

	old_context = MemoryContextSwitchTo(TopTransactionContext);

	for (i = 0; i < ncolumns; i++)
	{
		struct varlena *attr;
		struct varlena *new_attr;
		struct varatt_indirect redirect_pointer;

		/* only work on existing, not-null varlenas */
		if (TupleDescAttr(tupdesc, i)->attisdropped ||
			nulls[i] ||
			TupleDescAttr(tupdesc, i)->attlen != -1)
			continue;

		attr = (struct varlena *) DatumGetPointer(values[i]);

		/* don't recursively indirect */
		if (VARATT_IS_EXTERNAL_INDIRECT(attr))
			continue;

		/* copy datum, so it still lives later */
		if (VARATT_IS_EXTERNAL_ONDISK(attr))
			attr = heap_tuple_fetch_attr(attr);
		else
		{
			struct varlena *oldattr = attr;

			attr = palloc0(VARSIZE_ANY(oldattr));
			memcpy(attr, oldattr, VARSIZE_ANY(oldattr));
		}

		/* build indirection Datum */
		new_attr = (struct varlena *) palloc0(INDIRECT_POINTER_SIZE);
		redirect_pointer.pointer = attr;
		SET_VARTAG_EXTERNAL(new_attr, VARTAG_INDIRECT);
		memcpy(VARDATA_EXTERNAL(new_attr), &redirect_pointer,
			   sizeof(redirect_pointer));

		values[i] = PointerGetDatum(new_attr);
	}

	newtup = heap_form_tuple(tupdesc, values, nulls);
	pfree(values);
	pfree(nulls);
	ReleaseTupleDesc(tupdesc);

	MemoryContextSwitchTo(old_context);

	/*
	 * We intentionally don't use PG_RETURN_HEAPTUPLEHEADER here, because that
	 * would cause the indirect toast pointers to be flattened out of the
	 * tuple immediately, rendering subsequent testing irrelevant.  So just
	 * return the HeapTupleHeader pointer as-is.  This violates the general
	 * rule that composite Datums shouldn't contain toast pointers, but so
	 * long as the regression test scripts don't insert the result of this
	 * function into a container type (record, array, etc) it should be OK.
	 */
	PG_RETURN_POINTER(newtup->t_data);
}

PG_FUNCTION_INFO_V1(regress_putenv);

Datum
regress_putenv(PG_FUNCTION_ARGS)
{
	MemoryContext oldcontext;
	char	   *envbuf;

	if (!superuser())
		elog(ERROR, "must be superuser to change environment variables");

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	envbuf = text_to_cstring((text *) PG_GETARG_POINTER(0));
	MemoryContextSwitchTo(oldcontext);

	if (putenv(envbuf) != 0)
		elog(ERROR, "could not set environment variable: %m");

	PG_RETURN_VOID();
}

/* Sleep until no process has a given PID. */
PG_FUNCTION_INFO_V1(wait_pid);

Datum
wait_pid(PG_FUNCTION_ARGS)
{
	int			pid = PG_GETARG_INT32(0);

	if (!superuser())
		elog(ERROR, "must be superuser to check PID liveness");

	while (kill(pid, 0) == 0)
	{
		CHECK_FOR_INTERRUPTS();
		pg_usleep(50000);
	}

	if (errno != ESRCH)
		elog(ERROR, "could not check PID %d liveness: %m", pid);

	PG_RETURN_VOID();
}

static void
test_atomic_flag(void)
{
	pg_atomic_flag flag;

	pg_atomic_init_flag(&flag);

	if (!pg_atomic_unlocked_test_flag(&flag))
		elog(ERROR, "flag: unexpectedly set");

	if (!pg_atomic_test_set_flag(&flag))
		elog(ERROR, "flag: couldn't set");

	if (pg_atomic_unlocked_test_flag(&flag))
		elog(ERROR, "flag: unexpectedly unset");

	if (pg_atomic_test_set_flag(&flag))
		elog(ERROR, "flag: set spuriously #2");

	pg_atomic_clear_flag(&flag);

	if (!pg_atomic_unlocked_test_flag(&flag))
		elog(ERROR, "flag: unexpectedly set #2");

	if (!pg_atomic_test_set_flag(&flag))
		elog(ERROR, "flag: couldn't set");

	pg_atomic_clear_flag(&flag);
}

static void
test_atomic_uint32(void)
{
	pg_atomic_uint32 var;
	uint32		expected;
	int			i;

	pg_atomic_init_u32(&var, 0);

	if (pg_atomic_read_u32(&var) != 0)
		elog(ERROR, "atomic_read_u32() #1 wrong");

	pg_atomic_write_u32(&var, 3);

	if (pg_atomic_read_u32(&var) != 3)
		elog(ERROR, "atomic_read_u32() #2 wrong");

	if (pg_atomic_fetch_add_u32(&var, 1) != 3)
		elog(ERROR, "atomic_fetch_add_u32() #1 wrong");

	if (pg_atomic_fetch_sub_u32(&var, 1) != 4)
		elog(ERROR, "atomic_fetch_sub_u32() #1 wrong");

	if (pg_atomic_sub_fetch_u32(&var, 3) != 0)
		elog(ERROR, "atomic_sub_fetch_u32() #1 wrong");

	if (pg_atomic_add_fetch_u32(&var, 10) != 10)
		elog(ERROR, "atomic_add_fetch_u32() #1 wrong");

	if (pg_atomic_exchange_u32(&var, 5) != 10)
		elog(ERROR, "pg_atomic_exchange_u32() #1 wrong");

	if (pg_atomic_exchange_u32(&var, 0) != 5)
		elog(ERROR, "pg_atomic_exchange_u32() #0 wrong");

	/* test around numerical limits */
	if (pg_atomic_fetch_add_u32(&var, INT_MAX) != 0)
		elog(ERROR, "pg_atomic_fetch_add_u32() #2 wrong");

	if (pg_atomic_fetch_add_u32(&var, INT_MAX) != INT_MAX)
		elog(ERROR, "pg_atomic_add_fetch_u32() #3 wrong");

	pg_atomic_fetch_add_u32(&var, 1);	/* top up to UINT_MAX */

	if (pg_atomic_read_u32(&var) != UINT_MAX)
		elog(ERROR, "atomic_read_u32() #2 wrong");

	if (pg_atomic_fetch_sub_u32(&var, INT_MAX) != UINT_MAX)
		elog(ERROR, "pg_atomic_fetch_sub_u32() #2 wrong");

	if (pg_atomic_read_u32(&var) != (uint32) INT_MAX + 1)
		elog(ERROR, "atomic_read_u32() #3 wrong: %u", pg_atomic_read_u32(&var));

	expected = pg_atomic_sub_fetch_u32(&var, INT_MAX);
	if (expected != 1)
		elog(ERROR, "pg_atomic_sub_fetch_u32() #3 wrong: %u", expected);

	pg_atomic_sub_fetch_u32(&var, 1);

	/* fail exchange because of old expected */
	expected = 10;
	if (pg_atomic_compare_exchange_u32(&var, &expected, 1))
		elog(ERROR, "atomic_compare_exchange_u32() changed value spuriously");

	/* CAS is allowed to fail due to interrupts, try a couple of times */
	for (i = 0; i < 1000; i++)
	{
		expected = 0;
		if (!pg_atomic_compare_exchange_u32(&var, &expected, 1))
			break;
	}
	if (i == 1000)
		elog(ERROR, "atomic_compare_exchange_u32() never succeeded");
	if (pg_atomic_read_u32(&var) != 1)
		elog(ERROR, "atomic_compare_exchange_u32() didn't set value properly");

	pg_atomic_write_u32(&var, 0);

	/* try setting flagbits */
	if (pg_atomic_fetch_or_u32(&var, 1) & 1)
		elog(ERROR, "pg_atomic_fetch_or_u32() #1 wrong");

	if (!(pg_atomic_fetch_or_u32(&var, 2) & 1))
		elog(ERROR, "pg_atomic_fetch_or_u32() #2 wrong");

	if (pg_atomic_read_u32(&var) != 3)
		elog(ERROR, "invalid result after pg_atomic_fetch_or_u32()");

	/* try clearing flagbits */
	if ((pg_atomic_fetch_and_u32(&var, ~2) & 3) != 3)
		elog(ERROR, "pg_atomic_fetch_and_u32() #1 wrong");

	if (pg_atomic_fetch_and_u32(&var, ~1) != 1)
		elog(ERROR, "pg_atomic_fetch_and_u32() #2 wrong: is %u",
			 pg_atomic_read_u32(&var));
	/* no bits set anymore */
	if (pg_atomic_fetch_and_u32(&var, ~0) != 0)
		elog(ERROR, "pg_atomic_fetch_and_u32() #3 wrong");
}

static void
test_atomic_uint64(void)
{
	pg_atomic_uint64 var;
	uint64		expected;
	int			i;

	pg_atomic_init_u64(&var, 0);

	if (pg_atomic_read_u64(&var) != 0)
		elog(ERROR, "atomic_read_u64() #1 wrong");

	pg_atomic_write_u64(&var, 3);

	if (pg_atomic_read_u64(&var) != 3)
		elog(ERROR, "atomic_read_u64() #2 wrong");

	if (pg_atomic_fetch_add_u64(&var, 1) != 3)
		elog(ERROR, "atomic_fetch_add_u64() #1 wrong");

	if (pg_atomic_fetch_sub_u64(&var, 1) != 4)
		elog(ERROR, "atomic_fetch_sub_u64() #1 wrong");

	if (pg_atomic_sub_fetch_u64(&var, 3) != 0)
		elog(ERROR, "atomic_sub_fetch_u64() #1 wrong");

	if (pg_atomic_add_fetch_u64(&var, 10) != 10)
		elog(ERROR, "atomic_add_fetch_u64() #1 wrong");

	if (pg_atomic_exchange_u64(&var, 5) != 10)
		elog(ERROR, "pg_atomic_exchange_u64() #1 wrong");

	if (pg_atomic_exchange_u64(&var, 0) != 5)
		elog(ERROR, "pg_atomic_exchange_u64() #0 wrong");

	/* fail exchange because of old expected */
	expected = 10;
	if (pg_atomic_compare_exchange_u64(&var, &expected, 1))
		elog(ERROR, "atomic_compare_exchange_u64() changed value spuriously");

	/* CAS is allowed to fail due to interrupts, try a couple of times */
	for (i = 0; i < 100; i++)
	{
		expected = 0;
		if (!pg_atomic_compare_exchange_u64(&var, &expected, 1))
			break;
	}
	if (i == 100)
		elog(ERROR, "atomic_compare_exchange_u64() never succeeded");
	if (pg_atomic_read_u64(&var) != 1)
		elog(ERROR, "atomic_compare_exchange_u64() didn't set value properly");

	pg_atomic_write_u64(&var, 0);

	/* try setting flagbits */
	if (pg_atomic_fetch_or_u64(&var, 1) & 1)
		elog(ERROR, "pg_atomic_fetch_or_u64() #1 wrong");

	if (!(pg_atomic_fetch_or_u64(&var, 2) & 1))
		elog(ERROR, "pg_atomic_fetch_or_u64() #2 wrong");

	if (pg_atomic_read_u64(&var) != 3)
		elog(ERROR, "invalid result after pg_atomic_fetch_or_u64()");

	/* try clearing flagbits */
	if ((pg_atomic_fetch_and_u64(&var, ~2) & 3) != 3)
		elog(ERROR, "pg_atomic_fetch_and_u64() #1 wrong");

	if (pg_atomic_fetch_and_u64(&var, ~1) != 1)
		elog(ERROR, "pg_atomic_fetch_and_u64() #2 wrong: is " UINT64_FORMAT,
			 pg_atomic_read_u64(&var));
	/* no bits set anymore */
	if (pg_atomic_fetch_and_u64(&var, ~0) != 0)
		elog(ERROR, "pg_atomic_fetch_and_u64() #3 wrong");
}


PG_FUNCTION_INFO_V1(test_atomic_ops);
Datum
test_atomic_ops(PG_FUNCTION_ARGS)
{
	test_atomic_flag();

	test_atomic_uint32();

	test_atomic_uint64();

	PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(test_fdw_handler);
Datum
test_fdw_handler(PG_FUNCTION_ARGS)
{
	elog(ERROR, "test_fdw_handler is not implemented");
	PG_RETURN_NULL();
}
