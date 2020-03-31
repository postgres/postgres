/*
 * op function for ltree and lquery
 * Teodor Sigaev <teodor@stack.net>
 * contrib/ltree/lquery_op.c
 */
#include "postgres.h"

#include <ctype.h>

#include "catalog/pg_collation.h"
#include "ltree.h"
#include "miscadmin.h"
#include "utils/formatting.h"

PG_FUNCTION_INFO_V1(ltq_regex);
PG_FUNCTION_INFO_V1(ltq_rregex);

PG_FUNCTION_INFO_V1(lt_q_regex);
PG_FUNCTION_INFO_V1(lt_q_rregex);

#define NEXTVAL(x) ( (lquery*)( (char*)(x) + INTALIGN( VARSIZE(x) ) ) )

static char *
getlexeme(char *start, char *end, int *len)
{
	char	   *ptr;
	int			charlen;

	while (start < end && (charlen = pg_mblen(start)) == 1 && t_iseq(start, '_'))
		start += charlen;

	ptr = start;
	if (ptr >= end)
		return NULL;

	while (ptr < end && !((charlen = pg_mblen(ptr)) == 1 && t_iseq(ptr, '_')))
		ptr += charlen;

	*len = ptr - start;
	return start;
}

bool
compare_subnode(ltree_level *t, char *qn, int len, int (*cmpptr) (const char *, const char *, size_t), bool anyend)
{
	char	   *endt = t->name + t->len;
	char	   *endq = qn + len;
	char	   *tn;
	int			lent,
				lenq;
	bool		isok;

	while ((qn = getlexeme(qn, endq, &lenq)) != NULL)
	{
		tn = t->name;
		isok = false;
		while ((tn = getlexeme(tn, endt, &lent)) != NULL)
		{
			if ((lent == lenq || (lent > lenq && anyend)) &&
				(*cmpptr) (qn, tn, lenq) == 0)
			{

				isok = true;
				break;
			}
			tn += lent;
		}

		if (!isok)
			return false;
		qn += lenq;
	}

	return true;
}

int
ltree_strncasecmp(const char *a, const char *b, size_t s)
{
	char	   *al = str_tolower(a, s, DEFAULT_COLLATION_OID);
	char	   *bl = str_tolower(b, s, DEFAULT_COLLATION_OID);
	int			res;

	res = strncmp(al, bl, s);

	pfree(al);
	pfree(bl);

	return res;
}

/*
 * See if an lquery_level matches an ltree_level
 *
 * This accounts for all flags including LQL_NOT, but does not
 * consider repetition counts.
 */
static bool
checkLevel(lquery_level *curq, ltree_level *curt)
{
	lquery_variant *curvar = LQL_FIRST(curq);
	bool		success;

	success = (curq->flag & LQL_NOT) ? false : true;

	/* numvar == 0 means '*' which matches anything */
	if (curq->numvar == 0)
		return success;

	for (int i = 0; i < curq->numvar; i++)
	{
		int			(*cmpptr) (const char *, const char *, size_t);

		cmpptr = (curvar->flag & LVAR_INCASE) ? ltree_strncasecmp : strncmp;

		if (curvar->flag & LVAR_SUBLEXEME)
		{
			if (compare_subnode(curt, curvar->name, curvar->len, cmpptr,
								(curvar->flag & LVAR_ANYEND)))
				return success;
		}
		else if ((curvar->len == curt->len ||
				  (curt->len > curvar->len && (curvar->flag & LVAR_ANYEND))) &&
				 (*cmpptr) (curvar->name, curt->name, curvar->len) == 0)
			return success;

		curvar = LVAR_NEXT(curvar);
	}
	return !success;
}

/*
 * Try to match an lquery (of qlen items) to an ltree (of tlen items)
 */
static bool
checkCond(lquery_level *curq, int qlen,
		  ltree_level *curt, int tlen)
{
	/* Since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	/* Pathological patterns could take awhile, too */
	CHECK_FOR_INTERRUPTS();

	/* Loop while we have query items to consider */
	while (qlen > 0)
	{
		int			low,
					high;
		lquery_level *nextq;

		/*
		 * Get min and max repetition counts for this query item, dealing with
		 * the backwards-compatibility hack that the low/high fields aren't
		 * meaningful for non-'*' items unless LQL_COUNT is set.
		 */
		if ((curq->flag & LQL_COUNT) || curq->numvar == 0)
			low = curq->low, high = curq->high;
		else
			low = high = 1;

		/*
		 * We may limit "high" to the remaining text length; this avoids
		 * separate tests below.
		 */
		if (high > tlen)
			high = tlen;

		/* Fail if a match of required number of items is impossible */
		if (high < low)
			return false;

		/*
		 * Recursively check the rest of the pattern against each possible
		 * start point following some of this item's match(es).
		 */
		nextq = LQL_NEXT(curq);
		qlen--;

		for (int matchcnt = 0; matchcnt < high; matchcnt++)
		{
			/*
			 * If we've consumed an acceptable number of matches of this item,
			 * and the rest of the pattern matches beginning here, we're good.
			 */
			if (matchcnt >= low && checkCond(nextq, qlen, curt, tlen))
				return true;

			/*
			 * Otherwise, try to match one more text item to this query item.
			 */
			if (!checkLevel(curq, curt))
				return false;

			curt = LEVEL_NEXT(curt);
			tlen--;
		}

		/*
		 * Once we've consumed "high" matches, we can succeed only if the rest
		 * of the pattern matches beginning here.  Loop around (if you prefer,
		 * think of this as tail recursion).
		 */
		curq = nextq;
	}

	/*
	 * Once we're out of query items, we match only if there's no remaining
	 * text either.
	 */
	return (tlen == 0);
}

Datum
ltq_regex(PG_FUNCTION_ARGS)
{
	ltree	   *tree = PG_GETARG_LTREE_P(0);
	lquery	   *query = PG_GETARG_LQUERY_P(1);
	bool		res;

	res = checkCond(LQUERY_FIRST(query), query->numlevel,
					LTREE_FIRST(tree), tree->numlevel);

	PG_FREE_IF_COPY(tree, 0);
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(res);
}

Datum
ltq_rregex(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall2(ltq_regex,
										PG_GETARG_DATUM(1),
										PG_GETARG_DATUM(0)
										));
}

Datum
lt_q_regex(PG_FUNCTION_ARGS)
{
	ltree	   *tree = PG_GETARG_LTREE_P(0);
	ArrayType  *_query = PG_GETARG_ARRAYTYPE_P(1);
	lquery	   *query = (lquery *) ARR_DATA_PTR(_query);
	bool		res = false;
	int			num = ArrayGetNItems(ARR_NDIM(_query), ARR_DIMS(_query));

	if (ARR_NDIM(_query) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array must be one-dimensional")));
	if (array_contains_nulls(_query))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("array must not contain nulls")));

	while (num > 0)
	{
		if (DatumGetBool(DirectFunctionCall2(ltq_regex,
											 PointerGetDatum(tree), PointerGetDatum(query))))
		{

			res = true;
			break;
		}
		num--;
		query = NEXTVAL(query);
	}

	PG_FREE_IF_COPY(tree, 0);
	PG_FREE_IF_COPY(_query, 1);
	PG_RETURN_BOOL(res);
}

Datum
lt_q_rregex(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall2(lt_q_regex,
										PG_GETARG_DATUM(1),
										PG_GETARG_DATUM(0)
										));
}
