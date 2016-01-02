/*-------------------------------------------------------------------------
 *
 * tsginidx.c
 *	 GIN support functions for tsvector_ops
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsginidx.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gin.h"
#include "access/stratnum.h"
#include "miscadmin.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"


Datum
gin_cmp_tslexeme(PG_FUNCTION_ARGS)
{
	text	   *a = PG_GETARG_TEXT_PP(0);
	text	   *b = PG_GETARG_TEXT_PP(1);
	int			cmp;

	cmp = tsCompareString(VARDATA_ANY(a), VARSIZE_ANY_EXHDR(a),
						  VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b),
						  false);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_INT32(cmp);
}

Datum
gin_cmp_prefix(PG_FUNCTION_ARGS)
{
	text	   *a = PG_GETARG_TEXT_PP(0);
	text	   *b = PG_GETARG_TEXT_PP(1);

#ifdef NOT_USED
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	Pointer		extra_data = PG_GETARG_POINTER(3);
#endif
	int			cmp;

	cmp = tsCompareString(VARDATA_ANY(a), VARSIZE_ANY_EXHDR(a),
						  VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b),
						  true);

	if (cmp < 0)
		cmp = 1;				/* prevent continue scan */

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_INT32(cmp);
}

Datum
gin_extract_tsvector(PG_FUNCTION_ARGS)
{
	TSVector	vector = PG_GETARG_TSVECTOR(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	Datum	   *entries = NULL;

	*nentries = vector->size;
	if (vector->size > 0)
	{
		int			i;
		WordEntry  *we = ARRPTR(vector);

		entries = (Datum *) palloc(sizeof(Datum) * vector->size);

		for (i = 0; i < vector->size; i++)
		{
			text	   *txt;

			txt = cstring_to_text_with_len(STRPTR(vector) + we->pos, we->len);
			entries[i] = PointerGetDatum(txt);

			we++;
		}
	}

	PG_FREE_IF_COPY(vector, 0);
	PG_RETURN_POINTER(entries);
}

Datum
gin_extract_tsquery(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);

	/* StrategyNumber strategy = PG_GETARG_UINT16(2); */
	bool	  **ptr_partialmatch = (bool **) PG_GETARG_POINTER(3);
	Pointer   **extra_data = (Pointer **) PG_GETARG_POINTER(4);

	/* bool   **nullFlags = (bool **) PG_GETARG_POINTER(5); */
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	Datum	   *entries = NULL;

	*nentries = 0;

	if (query->size > 0)
	{
		QueryItem  *item = GETQUERY(query);
		int32		i,
					j;
		bool	   *partialmatch;
		int		   *map_item_operand;

		/*
		 * If the query doesn't have any required positive matches (for
		 * instance, it's something like '! foo'), we have to do a full index
		 * scan.
		 */
		if (tsquery_requires_match(item))
			*searchMode = GIN_SEARCH_MODE_DEFAULT;
		else
			*searchMode = GIN_SEARCH_MODE_ALL;

		/* count number of VAL items */
		j = 0;
		for (i = 0; i < query->size; i++)
		{
			if (item[i].type == QI_VAL)
				j++;
		}
		*nentries = j;

		entries = (Datum *) palloc(sizeof(Datum) * j);
		partialmatch = *ptr_partialmatch = (bool *) palloc(sizeof(bool) * j);

		/*
		 * Make map to convert item's number to corresponding operand's (the
		 * same, entry's) number. Entry's number is used in check array in
		 * consistent method. We use the same map for each entry.
		 */
		*extra_data = (Pointer *) palloc(sizeof(Pointer) * j);
		map_item_operand = (int *) palloc0(sizeof(int) * query->size);

		/* Now rescan the VAL items and fill in the arrays */
		j = 0;
		for (i = 0; i < query->size; i++)
		{
			if (item[i].type == QI_VAL)
			{
				QueryOperand *val = &item[i].qoperand;
				text	   *txt;

				txt = cstring_to_text_with_len(GETOPERAND(query) + val->distance,
											   val->length);
				entries[j] = PointerGetDatum(txt);
				partialmatch[j] = val->prefix;
				(*extra_data)[j] = (Pointer) map_item_operand;
				map_item_operand[i] = j;
				j++;
			}
		}
	}

	PG_FREE_IF_COPY(query, 0);

	PG_RETURN_POINTER(entries);
}

typedef struct
{
	QueryItem  *first_item;
	GinTernaryValue *check;
	int		   *map_item_operand;
	bool	   *need_recheck;
} GinChkVal;

static GinTernaryValue
checkcondition_gin(void *checkval, QueryOperand *val)
{
	GinChkVal  *gcv = (GinChkVal *) checkval;
	int			j;

	/* if any val requiring a weight is used, set recheck flag */
	if (val->weight != 0)
		*(gcv->need_recheck) = true;

	/* convert item's number to corresponding entry's (operand's) number */
	j = gcv->map_item_operand[((QueryItem *) val) - gcv->first_item];

	/* return presence of current entry in indexed value */
	return gcv->check[j];
}

/*
 * Evaluate tsquery boolean expression using ternary logic.
 *
 * chkcond is a callback function used to evaluate each VAL node in the query.
 * checkval can be used to pass information to the callback. TS_execute doesn't
 * do anything with it.
 */
static GinTernaryValue
TS_execute_ternary(QueryItem *curitem, void *checkval,
			  GinTernaryValue (*chkcond) (void *checkval, QueryOperand *val))
{
	GinTernaryValue val1,
				val2,
				result;

	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	if (curitem->type == QI_VAL)
		return chkcond(checkval, (QueryOperand *) curitem);

	switch (curitem->qoperator.oper)
	{
		case OP_NOT:
			result = TS_execute_ternary(curitem + 1, checkval, chkcond);
			if (result == GIN_MAYBE)
				return result;
			return !result;

		case OP_AND:
			val1 = TS_execute_ternary(curitem + curitem->qoperator.left,
									  checkval, chkcond);
			if (val1 == GIN_FALSE)
				return GIN_FALSE;
			val2 = TS_execute_ternary(curitem + 1, checkval, chkcond);
			if (val2 == GIN_FALSE)
				return GIN_FALSE;
			if (val1 == GIN_TRUE && val2 == GIN_TRUE)
				return GIN_TRUE;
			else
				return GIN_MAYBE;

		case OP_OR:
			val1 = TS_execute_ternary(curitem + curitem->qoperator.left,
									  checkval, chkcond);
			if (val1 == GIN_TRUE)
				return GIN_TRUE;
			val2 = TS_execute_ternary(curitem + 1, checkval, chkcond);
			if (val2 == GIN_TRUE)
				return GIN_TRUE;
			if (val1 == GIN_FALSE && val2 == GIN_FALSE)
				return GIN_FALSE;
			else
				return GIN_MAYBE;

		default:
			elog(ERROR, "unrecognized operator: %d", curitem->qoperator.oper);
	}

	/* not reachable, but keep compiler quiet */
	return false;
}

Datum
gin_tsquery_consistent(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);

	/* StrategyNumber strategy = PG_GETARG_UINT16(1); */
	TSQuery		query = PG_GETARG_TSQUERY(2);

	/* int32	nkeys = PG_GETARG_INT32(3); */
	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		res = FALSE;

	/* The query requires recheck only if it involves weights */
	*recheck = false;

	if (query->size > 0)
	{
		QueryItem  *item;
		GinChkVal	gcv;

		/*
		 * check-parameter array has one entry for each value (operand) in the
		 * query.
		 */
		gcv.first_item = item = GETQUERY(query);
		gcv.check = check;
		gcv.map_item_operand = (int *) (extra_data[0]);
		gcv.need_recheck = recheck;

		res = TS_execute(GETQUERY(query),
						 &gcv,
						 true,
						 checkcondition_gin);
	}

	PG_RETURN_BOOL(res);
}

Datum
gin_tsquery_triconsistent(PG_FUNCTION_ARGS)
{
	GinTernaryValue *check = (GinTernaryValue *) PG_GETARG_POINTER(0);

	/* StrategyNumber strategy = PG_GETARG_UINT16(1); */
	TSQuery		query = PG_GETARG_TSQUERY(2);

	/* int32	nkeys = PG_GETARG_INT32(3); */
	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	GinTernaryValue res = GIN_FALSE;
	bool		recheck;

	/* The query requires recheck only if it involves weights */
	recheck = false;

	if (query->size > 0)
	{
		QueryItem  *item;
		GinChkVal	gcv;

		/*
		 * check-parameter array has one entry for each value (operand) in the
		 * query.
		 */
		gcv.first_item = item = GETQUERY(query);
		gcv.check = check;
		gcv.map_item_operand = (int *) (extra_data[0]);
		gcv.need_recheck = &recheck;

		res = TS_execute_ternary(GETQUERY(query),
								 &gcv,
								 checkcondition_gin);

		if (res == GIN_TRUE && recheck)
			res = GIN_MAYBE;
	}

	PG_RETURN_GIN_TERNARY_VALUE(res);
}

/*
 * Formerly, gin_extract_tsvector had only two arguments.  Now it has three,
 * but we still need a pg_proc entry with two args to support reloading
 * pre-9.1 contrib/tsearch2 opclass declarations.  This compatibility
 * function should go away eventually.  (Note: you might say "hey, but the
 * code above is only *using* two args, so let's just declare it that way".
 * If you try that you'll find the opr_sanity regression test complains.)
 */
Datum
gin_extract_tsvector_2args(PG_FUNCTION_ARGS)
{
	if (PG_NARGS() < 3)			/* should not happen */
		elog(ERROR, "gin_extract_tsvector requires three arguments");
	return gin_extract_tsvector(fcinfo);
}

/*
 * Likewise, we need a stub version of gin_extract_tsquery declared with
 * only five arguments.
 */
Datum
gin_extract_tsquery_5args(PG_FUNCTION_ARGS)
{
	if (PG_NARGS() < 7)			/* should not happen */
		elog(ERROR, "gin_extract_tsquery requires seven arguments");
	return gin_extract_tsquery(fcinfo);
}

/*
 * Likewise, we need a stub version of gin_tsquery_consistent declared with
 * only six arguments.
 */
Datum
gin_tsquery_consistent_6args(PG_FUNCTION_ARGS)
{
	if (PG_NARGS() < 8)			/* should not happen */
		elog(ERROR, "gin_tsquery_consistent requires eight arguments");
	return gin_tsquery_consistent(fcinfo);
}
