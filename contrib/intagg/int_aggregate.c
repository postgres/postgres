/*
 * Integer array aggregator / enumerator
 *
 * Mark L. Woodward
 * DMN Digital Music Network.
 * www.dmn.com
 *
 * Copyright (C) Digital Music Network
 * December 20, 2001
 *
 * This file is the property of the Digital Music Network (DMN).
 * It is being made available to users of the PostgreSQL system
 * under the BSD license.
 */
#include "postgres.h"

#include <ctype.h>
#include <sys/types.h>

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "utils/syscache.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"


/* This is actually a postgres version of a one dimensional array */

typedef struct
{
	ArrayType	a;
	int			items;
	int			lower;
	int4		array[1];
}	PGARRAY;

/* This is used to keep track of our position during enumeration */
typedef struct callContext
{
	PGARRAY    *p;
	int			num;
	int			flags;
}	CTX;

#define TOASTED		1
#define START_NUM	8			/* initial size of arrays */
#define PGARRAY_SIZE(n) (sizeof(PGARRAY) + (((n)-1)*sizeof(int4)))

static PGARRAY *GetPGArray(PGARRAY *p, int fAdd);
static PGARRAY *ShrinkPGArray(PGARRAY *p);

Datum		int_agg_state(PG_FUNCTION_ARGS);
Datum		int_agg_final_array(PG_FUNCTION_ARGS);
Datum		int_enum(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(int_agg_state);
PG_FUNCTION_INFO_V1(int_agg_final_array);
PG_FUNCTION_INFO_V1(int_enum);

/*
 * Manage the aggregation state of the array
 *
 * Need to specify a suitably long-lived memory context, or it will vanish!
 * PortalContext isn't really right, but it's close enough.
 */
static PGARRAY *
GetPGArray(PGARRAY *p, int fAdd)
{
	if (!p)
	{
		/* New array */
		int			cb = PGARRAY_SIZE(START_NUM);

		p = (PGARRAY *) MemoryContextAlloc(PortalContext, cb);
		p->a.size = cb;
		p->a.ndim = 0;
		p->a.flags = 0;
		p->a.elemtype = INT4OID;
		p->items = 0;
		p->lower = START_NUM;
	}
	else if (fAdd)
	{							/* Ensure array has space */
		if (p->items >= p->lower)
		{
			PGARRAY    *pn;
			int			n = p->lower + p->lower;
			int			cbNew = PGARRAY_SIZE(n);

			pn = (PGARRAY *) repalloc(p, cbNew);
			pn->a.size = cbNew;
			pn->lower = n;
			return pn;
		}
	}
	return p;
}

/* Shrinks the array to its actual size and moves it into the standard
 * memory allocation context, frees working memory
 */
static PGARRAY *
ShrinkPGArray(PGARRAY * p)
{
	PGARRAY    *pnew = NULL;

	if (p)
	{
		/* get target size */
		int			cb = PGARRAY_SIZE(p->items);

		/* use current transaction context */
		pnew = palloc(cb);

		/*
		 * Fix up the fields in the new structure, so Postgres understands
		 */
		memcpy(pnew, p, cb);
		pnew->a.size = cb;
		pnew->a.ndim = 1;
		pnew->a.flags = 0;
		pnew->a.elemtype = INT4OID;
		pnew->lower = 1;

		pfree(p);
	}
	return pnew;
}

/* Called for each iteration during an aggregate function */
Datum
int_agg_state(PG_FUNCTION_ARGS)
{
	PGARRAY    *state;
	PGARRAY    *p;

	/*
	 * We can keep a pointer in the datum even though nodeAgg thinks it's
	 * an int4.  Note we assume the initial state of int4 zero will look
	 * like a null pointer.
	 */
	state = (PGARRAY *) PG_GETARG_POINTER(0);
	p = GetPGArray(state, 1);

	if (!PG_ARGISNULL(1))
	{
		int4	value = PG_GETARG_INT32(1);

		if (!p)		/* internal error */
			elog(ERROR, "no aggregate storage");
		else if (p->items >= p->lower)		/* internal error */
			elog(ERROR, "aggregate storage too small");
		else
			p->array[p->items++] = value;
	}
	PG_RETURN_POINTER(p);
}

/* This is the final function used for the integer aggregator. It returns all
 * the integers collected as a one dimensional integer array
 */
Datum
int_agg_final_array(PG_FUNCTION_ARGS)
{
	PGARRAY    *state = (PGARRAY *) PG_GETARG_POINTER(0);
	PGARRAY    *pnew = ShrinkPGArray(GetPGArray(state, 0));

	if (pnew)
		PG_RETURN_POINTER(pnew);
	else
		PG_RETURN_NULL();
}

/* This function accepts an array, and returns one item for each entry in the array */
Datum
int_enum(PG_FUNCTION_ARGS)
{
	PGARRAY    *p = (PGARRAY *) PG_GETARG_POINTER(0);
	CTX		   *pc;
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	if (!rsi || !IsA(rsi, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		 errmsg("int_enum called in context that cannot accept a set")));

	if (!p)
	{
		elog(WARNING, "no data sent");
		PG_RETURN_NULL();
	}

	if (!fcinfo->flinfo->fn_extra)
	{
		/* Allocate working state */
		MemoryContext	oldcontext;

		oldcontext = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);

		pc = (CTX *) palloc(sizeof(CTX));

		/* Don't copy attribute if you don't need to */
		if (VARATT_IS_EXTENDED(p))
		{
			/* Toasted!!! */
			pc->p = (PGARRAY *) PG_DETOAST_DATUM_COPY(p);
			pc->flags = TOASTED;
		}
		else
		{
			/* Untoasted */
			pc->p = p;
			pc->flags = 0;
		}
		/* Now that we have a detoasted array, verify dimensions */
		if (pc->p->a.ndim != 1)
			elog(ERROR, "int_enum only accepts 1-D arrays");
		pc->num = 0;
		fcinfo->flinfo->fn_extra = (void *) pc;
		MemoryContextSwitchTo(oldcontext);
	}
	else	/* use existing working state */
		pc = (CTX *) fcinfo->flinfo->fn_extra;

	/* Are we done yet? */
	if (pc->num >= pc->p->items)
	{
		/* We are done */
		if (pc->flags & TOASTED)
			pfree(pc->p);
		pfree(pc);
		fcinfo->flinfo->fn_extra = NULL;
		rsi->isDone = ExprEndResult;
	}
	else
	{
		/* nope, return the next value */
		int			val = pc->p->array[pc->num++];

		rsi->isDone = ExprMultipleResult;
		PG_RETURN_INT32(val);
	}
	PG_RETURN_NULL();
}
