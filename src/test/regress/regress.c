/*
 * $Header: /cvsroot/pgsql/src/test/regress/regress.c,v 1.40 2000/06/13 07:35:34 tgl Exp $
 */

#include <float.h>				/* faked on sunos */

#include "postgres.h"

#include "utils/geo_decls.h"	/* includes <math.h> */
#include "executor/executor.h"	/* For GetAttributeByName */
#include "commands/sequence.h"	/* for nextval() */

#define P_MAXDIG 12
#define LDELIM			'('
#define RDELIM			')'
#define DELIM			','

typedef TupleTableSlot *TUPLE;

extern double *regress_dist_ptpath(Point *pt, PATH *path);
extern double *regress_path_dist(PATH *p1, PATH *p2);
extern PATH *poly2path(POLYGON *poly);
extern Point *interpt_pp(PATH *p1, PATH *p2);
extern void regress_lseg_construct(LSEG *lseg, Point *pt1, Point *pt2);
extern Datum overpaid(PG_FUNCTION_ARGS);
extern Datum boxarea(PG_FUNCTION_ARGS);
extern char *reverse_name(char *string);

/*
** Distance from a point to a path
*/
double *
regress_dist_ptpath(pt, path)
Point	   *pt;
PATH	   *path;
{
	double	   *result;
	double	   *tmp;
	int			i;
	LSEG		lseg;

	switch (path->npts)
	{
		case 0:
			result = palloc(sizeof(double));
			*result = Abs((double) DBL_MAX);	/* +infinity */
			break;
		case 1:
			result = point_distance(pt, &path->p[0]);
			break;
		default:

			/*
			 * the distance from a point to a path is the smallest
			 * distance from the point to any of its constituent segments.
			 */
			Assert(path->npts > 1);
			result = palloc(sizeof(double));
			for (i = 0; i < path->npts - 1; ++i)
			{
				regress_lseg_construct(&lseg, &path->p[i], &path->p[i + 1]);
				tmp = dist_ps(pt, &lseg);
				if (i == 0 || *tmp < *result)
					*result = *tmp;
				pfree(tmp);

			}
			break;
	}
	return result;
}

/* this essentially does a cartesian product of the lsegs in the
   two paths, and finds the min distance between any two lsegs */
double *
regress_path_dist(p1, p2)
PATH	   *p1;
PATH	   *p2;
{
	double	   *min,
			   *tmp;
	int			i,
				j;
	LSEG		seg1,
				seg2;

	regress_lseg_construct(&seg1, &p1->p[0], &p1->p[1]);
	regress_lseg_construct(&seg2, &p2->p[0], &p2->p[1]);
	min = lseg_distance(&seg1, &seg2);

	for (i = 0; i < p1->npts - 1; i++)
		for (j = 0; j < p2->npts - 1; j++)
		{
			regress_lseg_construct(&seg1, &p1->p[i], &p1->p[i + 1]);
			regress_lseg_construct(&seg2, &p2->p[j], &p2->p[j + 1]);

			if (*min < *(tmp = lseg_distance(&seg1, &seg2)))
				*min = *tmp;
			pfree(tmp);
		}

	return min;
}

PATH *
poly2path(poly)
POLYGON    *poly;
{
	int			i;
	char	   *output = (char *) palloc(2 * (P_MAXDIG + 1) * poly->npts + 64);
	char		buf[2 * (P_MAXDIG) + 20];

	sprintf(output, "(1, %*d", P_MAXDIG, poly->npts);

	for (i = 0; i < poly->npts; i++)
	{
		sprintf(buf, ",%*g,%*g", P_MAXDIG, poly->p[i].x, P_MAXDIG, poly->p[i].y);
		strcat(output, buf);
	}

	sprintf(buf, "%c", RDELIM);
	strcat(output, buf);
	return path_in(output);
}

/* return the point where two paths intersect.	Assumes that they do. */
Point *
interpt_pp(p1, p2)
PATH	   *p1;
PATH	   *p2;
{

	Point	   *retval;
	int			i,
				j;
	LSEG		seg1,
				seg2;

#ifdef NOT_USED
	LINE	   *ln;

#endif
	bool		found;			/* We've found the intersection */

	found = false;				/* Haven't found it yet */

	for (i = 0; i < p1->npts - 1 && !found; i++)
		for (j = 0; j < p2->npts - 1 && !found; j++)
		{
			regress_lseg_construct(&seg1, &p1->p[i], &p1->p[i + 1]);
			regress_lseg_construct(&seg2, &p2->p[j], &p2->p[j + 1]);
			if (lseg_intersect(&seg1, &seg2))
				found = true;
		}

#ifdef NOT_USED
	ln = line_construct_pp(&seg2.p[0], &seg2.p[1]);
	retval = interpt_sl(&seg1, ln);
#endif
	retval = lseg_interpt(&seg1, &seg2);

	return retval;
}


/* like lseg_construct, but assume space already allocated */
void
regress_lseg_construct(lseg, pt1, pt2)
LSEG	   *lseg;
Point	   *pt1;
Point	   *pt2;
{
	lseg->p[0].x = pt1->x;
	lseg->p[0].y = pt1->y;
	lseg->p[1].x = pt2->x;
	lseg->p[1].y = pt2->y;
	lseg->m = point_sl(pt1, pt2);
}

Datum
overpaid(PG_FUNCTION_ARGS)
{
	TUPLE		tuple = (TUPLE) PG_GETARG_POINTER(0);
	bool		isnull;
	long		salary;

	salary = (long) GetAttributeByName(tuple, "salary", &isnull);
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
}			WIDGET;

WIDGET	   *widget_in(char *str);
char	   *widget_out(WIDGET * widget);
extern Datum pt_in_widget(PG_FUNCTION_ARGS);

#define NARGS	3

WIDGET *
widget_in(str)
char	   *str;
{
	char	   *p,
			   *coord[NARGS],
				buf2[1000];
	int			i;
	WIDGET	   *result;

	if (str == NULL)
		return NULL;
	for (i = 0, p = str; *p && i < NARGS && *p != RDELIM; p++)
		if (*p == ',' || (*p == LDELIM && !i))
			coord[i++] = p + 1;
	if (i < NARGS - 1)
		return NULL;
	result = (WIDGET *) palloc(sizeof(WIDGET));
	result->center.x = atof(coord[0]);
	result->center.y = atof(coord[1]);
	result->radius = atof(coord[2]);

	sprintf(buf2, "widget_in: read (%f, %f, %f)\n", result->center.x,
			result->center.y, result->radius);
	return result;
}

char *
widget_out(widget)
WIDGET	   *widget;
{
	char	   *result;

	if (widget == NULL)
		return NULL;

	result = (char *) palloc(60);
	sprintf(result, "(%g,%g,%g)",
			widget->center.x, widget->center.y, widget->radius);
	return result;
}

Datum
pt_in_widget(PG_FUNCTION_ARGS)
{
	Point	   *point = PG_GETARG_POINT_P(0);
	WIDGET	   *widget = (WIDGET *) PG_GETARG_POINTER(1);

	PG_RETURN_BOOL(point_dt(point, &widget->center) < widget->radius);
}

#define ABS(X) ((X) >= 0 ? (X) : -(X))

Datum
boxarea(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);
	double		width,
				height;

	width = ABS(box->high.x - box->low.x);
	height = ABS(box->high.y - box->low.y);
	PG_RETURN_FLOAT8(width * height);
}

char *
reverse_name(string)
char	   *string;
{
	int			i;
	int			len;
	char	   *new_string;

	if (!(new_string = palloc(NAMEDATALEN)))
	{
		fprintf(stderr, "reverse_name: palloc failed\n");
		return NULL;
	}
	MemSet(new_string, 0, NAMEDATALEN);
	for (i = 0; i < NAMEDATALEN && string[i]; ++i)
		;
	if (i == NAMEDATALEN || !string[i])
		--i;
	len = i;
	for (; i >= 0; --i)
		new_string[len - i] = string[i];
	return new_string;
}

#include "executor/spi.h"		/* this is what you need to work with SPI */
#include "commands/trigger.h"	/* -"- and triggers */

static TransactionId fd17b_xid = InvalidTransactionId;
static TransactionId fd17a_xid = InvalidTransactionId;
static int	fd17b_level = 0;
static int	fd17a_level = 0;
static bool fd17b_recursion = true;
static bool fd17a_recursion = true;
extern Datum funny_dup17(PG_FUNCTION_ARGS);

Datum
funny_dup17(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	TransactionId *xid;
	int		   *level;
	bool	   *recursion;
	Relation	rel;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	char	   *query,
			   *fieldval,
			   *fieldtype;
	char	   *when;
	int			inserted;
	int			selected = 0;
	int			ret;

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "funny_dup17: not fired by trigger manager");

	tuple = trigdata->tg_trigtuple;
	rel = trigdata->tg_relation;
	tupdesc = rel->rd_att;
	if (TRIGGER_FIRED_BEFORE(trigdata->tg_event))
	{
		xid = &fd17b_xid;
		level = &fd17b_level;
		recursion = &fd17b_recursion;
		when = "BEFORE";
	}
	else
	{
		xid = &fd17a_xid;
		level = &fd17a_level;
		recursion = &fd17a_recursion;
		when = "AFTER ";
	}

	if (!TransactionIdIsCurrentTransactionId(*xid))
	{
		*xid = GetCurrentTransactionId();
		*level = 0;
		*recursion = true;
	}

	if (*level == 17)
	{
		*recursion = false;
		return PointerGetDatum(tuple);
	}

	if (!(*recursion))
		return PointerGetDatum(tuple);

	(*level)++;

	SPI_connect();

	fieldval = SPI_getvalue(tuple, tupdesc, 1);
	fieldtype = SPI_gettype(tupdesc, 1);

	query = (char *) palloc(100 + NAMEDATALEN * 3 +
							strlen(fieldval) + strlen(fieldtype));

	sprintf(query, "insert into %s select * from %s where %s = '%s'::%s",
			SPI_getrelname(rel), SPI_getrelname(rel),
			SPI_fname(tupdesc, 1),
			fieldval, fieldtype);

	if ((ret = SPI_exec(query, 0)) < 0)
		elog(ERROR, "funny_dup17 (fired %s) on level %3d: SPI_exec (insert ...) returned %d",
			 when, *level, ret);

	inserted = SPI_processed;

	sprintf(query, "select count (*) from %s where %s = '%s'::%s",
			SPI_getrelname(rel),
			SPI_fname(tupdesc, 1),
			fieldval, fieldtype);

	if ((ret = SPI_exec(query, 0)) < 0)
		elog(ERROR, "funny_dup17 (fired %s) on level %3d: SPI_exec (select ...) returned %d",
			 when, *level, ret);

	if (SPI_processed > 0)
	{
		selected = DatumGetInt32(DirectFunctionCall1(int4in,
								 CStringGetDatum(SPI_getvalue(
									   SPI_tuptable->vals[0],
									   SPI_tuptable->tupdesc,
									   1
									   ))));
	}

	elog(NOTICE, "funny_dup17 (fired %s) on level %3d: %d/%d tuples inserted/selected",
		 when, *level, inserted, selected);

	SPI_finish();

	(*level)--;

	if (*level == 0)
		*xid = InvalidTransactionId;

	return PointerGetDatum(tuple);
}

extern Datum ttdummy(PG_FUNCTION_ARGS);
extern Datum set_ttdummy(PG_FUNCTION_ARGS);

#define TTDUMMY_INFINITY	999999

static void *splan = NULL;
static bool ttoff = false;

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
	if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		elog(ERROR, "ttdummy: can't process STATEMENT events");
	if (TRIGGER_FIRED_AFTER(trigdata->tg_event))
		elog(ERROR, "ttdummy: must be fired before event");
	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		elog(ERROR, "ttdummy: can't process INSERT event");
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
		if (attnum[i] < 0)
			elog(ERROR, "ttdummy (%s): there is no attribute %s", relname, args[i]);
		if (SPI_gettypeid(tupdesc, attnum[i]) != INT4OID)
			elog(ERROR, "ttdummy (%s): attributes %s and %s must be of abstime type",
				 relname, args[0], args[1]);
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
			elog(ERROR, "ttdummy (%s): you can't change %s and/or %s columns (use set_ttdummy)",
				 relname, args[0], args[1]);

		if (newoff != TTDUMMY_INFINITY)
		{
			pfree(relname);		/* allocated in upper executor context */
			return PointerGetDatum(NULL);
		}
	}
	else if (oldoff != TTDUMMY_INFINITY)		/* DELETE */
	{
		pfree(relname);
		return PointerGetDatum(NULL);
	}

	{
		text   *seqname = textin("ttdummy_seq");

		newoff = DirectFunctionCall1(nextval,
									 PointerGetDatum(seqname));
		pfree(seqname);
	}

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
		cvals[attnum[1] - 1] = TTDUMMY_INFINITY;		/* stop_date eq INFINITY */
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
		void	   *pplan;
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
			elog(ERROR, "ttdummy (%s): SPI_prepare returned %d", relname, SPI_result);

		pplan = SPI_saveplan(pplan);
		if (pplan == NULL)
			elog(ERROR, "ttdummy (%s): SPI_saveplan returned %d", relname, SPI_result);

		splan = pplan;
	}

	ret = SPI_execp(splan, cvals, cnulls, 0);

	if (ret < 0)
		elog(ERROR, "ttdummy (%s): SPI_execp returned %d", relname, ret);

	/* Tuple to return to upper Executor ... */
	if (newtuple)				/* UPDATE */
	{
		HeapTuple	tmptuple;

		tmptuple = SPI_copytuple(trigtuple);
		rettuple = SPI_modifytuple(rel, tmptuple, 1, &(attnum[1]), &newoff, NULL);
		SPI_freetuple(tmptuple);
	}
	else
/* DELETE */
		rettuple = trigtuple;

	SPI_finish();				/* don't forget say Bye to SPI mgr */

	pfree(relname);

	return PointerGetDatum(rettuple);
}

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
