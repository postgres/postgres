/*
 * op function for ltree and lquery
 * Teodor Sigaev <teodor@stack.net>
 * $PostgreSQL: pgsql/contrib/ltree/lquery_op.c,v 1.14 2009/06/11 14:48:51 momjian Exp $
 */
#include "postgres.h"

#include <ctype.h>

#include "utils/array.h"
#include "utils/formatting.h"
#include "ltree.h"

PG_FUNCTION_INFO_V1(ltq_regex);
PG_FUNCTION_INFO_V1(ltq_rregex);

PG_FUNCTION_INFO_V1(lt_q_regex);
PG_FUNCTION_INFO_V1(lt_q_rregex);

#define NEXTVAL(x) ( (lquery*)( (char*)(x) + INTALIGN( VARSIZE(x) ) ) )

typedef struct
{
	lquery_level *q;
	int			nq;
	ltree_level *t;
	int			nt;
	int			posq;
	int			post;
} FieldNot;

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
			if (
				(
				 lent == lenq ||
				 (lent > lenq && anyend)
				 ) &&
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
	char	   *al = str_tolower(a, s);
	char	   *bl = str_tolower(b, s);
	int			res;

	res = strncmp(al, bl, s);

	pfree(al);
	pfree(bl);

	return res;
}

static bool
checkLevel(lquery_level *curq, ltree_level *curt)
{
	int			(*cmpptr) (const char *, const char *, size_t);
	lquery_variant *curvar = LQL_FIRST(curq);
	int			i;

	for (i = 0; i < curq->numvar; i++)
	{
		cmpptr = (curvar->flag & LVAR_INCASE) ? ltree_strncasecmp : strncmp;

		if (curvar->flag & LVAR_SUBLEXEME)
		{
			if (compare_subnode(curt, curvar->name, curvar->len, cmpptr, (curvar->flag & LVAR_ANYEND)))
				return true;
		}
		else if (
				 (
				  curvar->len == curt->len ||
				  (curt->len > curvar->len && (curvar->flag & LVAR_ANYEND))
				  ) &&
				 (*cmpptr) (curvar->name, curt->name, curvar->len) == 0)
		{

			return true;
		}
		curvar = LVAR_NEXT(curvar);
	}
	return false;
}

/*
void
printFieldNot(FieldNot *fn ) {
	while(fn->q) {
		elog(NOTICE,"posQ:%d lenQ:%d posT:%d lenT:%d", fn->posq,fn->nq,fn->post,fn->nt);
		fn++;
	}
}
*/

static struct
{
	bool		muse;
	uint32		high_pos;
}	SomeStack =

{
	false, 0,
};

static bool
checkCond(lquery_level *curq, int query_numlevel, ltree_level *curt, int tree_numlevel, FieldNot *ptr)
{
	uint32		low_pos = 0,
				high_pos = 0,
				cur_tpos = 0;
	int			tlen = tree_numlevel,
				qlen = query_numlevel;
	int			isok;
	lquery_level *prevq = NULL;
	ltree_level *prevt = NULL;

	if (SomeStack.muse)
	{
		high_pos = SomeStack.high_pos;
		qlen--;
		prevq = curq;
		curq = LQL_NEXT(curq);
		SomeStack.muse = false;
	}

	while (tlen > 0 && qlen > 0)
	{
		if (curq->numvar)
		{
			prevt = curt;
			while (cur_tpos < low_pos)
			{
				curt = LEVEL_NEXT(curt);
				tlen--;
				cur_tpos++;
				if (tlen == 0)
					return false;
				if (ptr && ptr->q)
					ptr->nt++;
			}

			if (ptr && curq->flag & LQL_NOT)
			{
				if (!(prevq && prevq->numvar == 0))
					prevq = curq;
				if (ptr->q == NULL)
				{
					ptr->t = prevt;
					ptr->q = prevq;
					ptr->nt = 1;
					ptr->nq = 1 + ((prevq == curq) ? 0 : 1);
					ptr->posq = query_numlevel - qlen - ((prevq == curq) ? 0 : 1);
					ptr->post = cur_tpos;
				}
				else
				{
					ptr->nt++;
					ptr->nq++;
				}

				if (qlen == 1 && ptr->q->numvar == 0)
					ptr->nt = tree_numlevel - ptr->post;
				curt = LEVEL_NEXT(curt);
				tlen--;
				cur_tpos++;
				if (high_pos < cur_tpos)
					high_pos++;
			}
			else
			{
				isok = false;
				while (cur_tpos <= high_pos && tlen > 0 && !isok)
				{
					isok = checkLevel(curq, curt);
					curt = LEVEL_NEXT(curt);
					tlen--;
					cur_tpos++;
					if (isok && prevq && prevq->numvar == 0 && tlen > 0 && cur_tpos <= high_pos)
					{
						FieldNot	tmpptr;

						if (ptr)
							memcpy(&tmpptr, ptr, sizeof(FieldNot));
						SomeStack.high_pos = high_pos - cur_tpos;
						SomeStack.muse = true;
						if (checkCond(prevq, qlen + 1, curt, tlen, (ptr) ? &tmpptr : NULL))
							return true;
					}
					if (!isok && ptr)
						ptr->nt++;
				}
				if (!isok)
					return false;

				if (ptr && ptr->q)
				{
					if (checkCond(ptr->q, ptr->nq, ptr->t, ptr->nt, NULL))
						return false;
					ptr->q = NULL;
				}
				low_pos = cur_tpos;
				high_pos = cur_tpos;
			}
		}
		else
		{
			low_pos = cur_tpos + curq->low;
			high_pos = cur_tpos + curq->high;
			if (ptr && ptr->q)
			{
				ptr->nq++;
				if (qlen == 1)
					ptr->nt = tree_numlevel - ptr->post;
			}
		}

		prevq = curq;
		curq = LQL_NEXT(curq);
		qlen--;
	}

	if (low_pos > tree_numlevel || tree_numlevel > high_pos)
		return false;

	while (qlen > 0)
	{
		if (curq->numvar)
		{
			if (!(curq->flag & LQL_NOT))
				return false;
		}
		else
		{
			low_pos = cur_tpos + curq->low;
			high_pos = cur_tpos + curq->high;
		}

		curq = LQL_NEXT(curq);
		qlen--;
	}

	if (low_pos > tree_numlevel || tree_numlevel > high_pos)
		return false;

	if (ptr && ptr->q && checkCond(ptr->q, ptr->nq, ptr->t, ptr->nt, NULL))
		return false;

	return true;
}

Datum
ltq_regex(PG_FUNCTION_ARGS)
{
	ltree	   *tree = PG_GETARG_LTREE(0);
	lquery	   *query = PG_GETARG_LQUERY(1);
	bool		res = false;

	if (query->flag & LQUERY_HASNOT)
	{
		FieldNot	fn;

		fn.q = NULL;

		res = checkCond(LQUERY_FIRST(query), query->numlevel,
						LTREE_FIRST(tree), tree->numlevel, &fn);
	}
	else
	{
		res = checkCond(LQUERY_FIRST(query), query->numlevel,
						LTREE_FIRST(tree), tree->numlevel, NULL);
	}

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
	ltree	   *tree = PG_GETARG_LTREE(0);
	ArrayType  *_query = PG_GETARG_ARRAYTYPE_P(1);
	lquery	   *query = (lquery *) ARR_DATA_PTR(_query);
	bool		res = false;
	int			num = ArrayGetNItems(ARR_NDIM(_query), ARR_DIMS(_query));

	if (ARR_NDIM(_query) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array must be one-dimensional")));
	if (ARR_HASNULL(_query))
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
