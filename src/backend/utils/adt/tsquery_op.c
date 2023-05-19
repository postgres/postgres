/*-------------------------------------------------------------------------
 *
 * tsquery_op.c
 *	  Various operations with tsquery
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsquery_op.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/qunique.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "varatt.h"

Datum
tsquery_numnode(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY(0);
	int			nnode = query->size;

	PG_FREE_IF_COPY(query, 0);
	PG_RETURN_INT32(nnode);
}

static QTNode *
join_tsqueries(TSQuery a, TSQuery b, int8 operator, uint16 distance)
{
	QTNode	   *res = (QTNode *) palloc0(sizeof(QTNode));

	res->flags |= QTN_NEEDFREE;

	res->valnode = (QueryItem *) palloc0(sizeof(QueryItem));
	res->valnode->type = QI_OPR;
	res->valnode->qoperator.oper = operator;
	if (operator == OP_PHRASE)
		res->valnode->qoperator.distance = distance;

	res->child = (QTNode **) palloc0(sizeof(QTNode *) * 2);
	res->child[0] = QT2QTN(GETQUERY(b), GETOPERAND(b));
	res->child[1] = QT2QTN(GETQUERY(a), GETOPERAND(a));
	res->nchild = 2;

	return res;
}

Datum
tsquery_and(PG_FUNCTION_ARGS)
{
	TSQuery		a = PG_GETARG_TSQUERY_COPY(0);
	TSQuery		b = PG_GETARG_TSQUERY_COPY(1);
	QTNode	   *res;
	TSQuery		query;

	if (a->size == 0)
	{
		PG_FREE_IF_COPY(a, 1);
		PG_RETURN_POINTER(b);
	}
	else if (b->size == 0)
	{
		PG_FREE_IF_COPY(b, 1);
		PG_RETURN_POINTER(a);
	}

	res = join_tsqueries(a, b, OP_AND, 0);

	query = QTN2QT(res);

	QTNFree(res);
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);

	PG_RETURN_TSQUERY(query);
}

Datum
tsquery_or(PG_FUNCTION_ARGS)
{
	TSQuery		a = PG_GETARG_TSQUERY_COPY(0);
	TSQuery		b = PG_GETARG_TSQUERY_COPY(1);
	QTNode	   *res;
	TSQuery		query;

	if (a->size == 0)
	{
		PG_FREE_IF_COPY(a, 1);
		PG_RETURN_POINTER(b);
	}
	else if (b->size == 0)
	{
		PG_FREE_IF_COPY(b, 1);
		PG_RETURN_POINTER(a);
	}

	res = join_tsqueries(a, b, OP_OR, 0);

	query = QTN2QT(res);

	QTNFree(res);
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);

	PG_RETURN_TSQUERY(query);
}

Datum
tsquery_phrase_distance(PG_FUNCTION_ARGS)
{
	TSQuery		a = PG_GETARG_TSQUERY_COPY(0);
	TSQuery		b = PG_GETARG_TSQUERY_COPY(1);
	QTNode	   *res;
	TSQuery		query;
	int32		distance = PG_GETARG_INT32(2);

	if (distance < 0 || distance > MAXENTRYPOS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("distance in phrase operator must be an integer value between zero and %d inclusive",
						MAXENTRYPOS)));
	if (a->size == 0)
	{
		PG_FREE_IF_COPY(a, 1);
		PG_RETURN_POINTER(b);
	}
	else if (b->size == 0)
	{
		PG_FREE_IF_COPY(b, 1);
		PG_RETURN_POINTER(a);
	}

	res = join_tsqueries(a, b, OP_PHRASE, (uint16) distance);

	query = QTN2QT(res);

	QTNFree(res);
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);

	PG_RETURN_TSQUERY(query);
}

Datum
tsquery_phrase(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall3(tsquery_phrase_distance,
										PG_GETARG_DATUM(0),
										PG_GETARG_DATUM(1),
										Int32GetDatum(1)));
}

Datum
tsquery_not(PG_FUNCTION_ARGS)
{
	TSQuery		a = PG_GETARG_TSQUERY_COPY(0);
	QTNode	   *res;
	TSQuery		query;

	if (a->size == 0)
		PG_RETURN_POINTER(a);

	res = (QTNode *) palloc0(sizeof(QTNode));

	res->flags |= QTN_NEEDFREE;

	res->valnode = (QueryItem *) palloc0(sizeof(QueryItem));
	res->valnode->type = QI_OPR;
	res->valnode->qoperator.oper = OP_NOT;

	res->child = (QTNode **) palloc0(sizeof(QTNode *));
	res->child[0] = QT2QTN(GETQUERY(a), GETOPERAND(a));
	res->nchild = 1;

	query = QTN2QT(res);

	QTNFree(res);
	PG_FREE_IF_COPY(a, 0);

	PG_RETURN_POINTER(query);
}

static int
CompareTSQ(TSQuery a, TSQuery b)
{
	if (a->size != b->size)
	{
		return (a->size < b->size) ? -1 : 1;
	}
	else if (VARSIZE(a) != VARSIZE(b))
	{
		return (VARSIZE(a) < VARSIZE(b)) ? -1 : 1;
	}
	else if (a->size != 0)
	{
		QTNode	   *an = QT2QTN(GETQUERY(a), GETOPERAND(a));
		QTNode	   *bn = QT2QTN(GETQUERY(b), GETOPERAND(b));
		int			res = QTNodeCompare(an, bn);

		QTNFree(an);
		QTNFree(bn);

		return res;
	}

	return 0;
}

Datum
tsquery_cmp(PG_FUNCTION_ARGS)
{
	TSQuery		a = PG_GETARG_TSQUERY_COPY(0);
	TSQuery		b = PG_GETARG_TSQUERY_COPY(1);
	int			res = CompareTSQ(a, b);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);

	PG_RETURN_INT32(res);
}

#define CMPFUNC( NAME, CONDITION )				\
Datum											\
NAME(PG_FUNCTION_ARGS) {						\
	TSQuery  a = PG_GETARG_TSQUERY_COPY(0);		\
	TSQuery  b = PG_GETARG_TSQUERY_COPY(1);		\
	int res = CompareTSQ(a,b);					\
												\
	PG_FREE_IF_COPY(a,0);						\
	PG_FREE_IF_COPY(b,1);						\
												\
	PG_RETURN_BOOL( CONDITION );				\
}	\
/* keep compiler quiet - no extra ; */			\
extern int no_such_variable

CMPFUNC(tsquery_lt, res < 0);
CMPFUNC(tsquery_le, res <= 0);
CMPFUNC(tsquery_eq, res == 0);
CMPFUNC(tsquery_ge, res >= 0);
CMPFUNC(tsquery_gt, res > 0);
CMPFUNC(tsquery_ne, res != 0);

TSQuerySign
makeTSQuerySign(TSQuery a)
{
	int			i;
	QueryItem  *ptr = GETQUERY(a);
	TSQuerySign sign = 0;

	for (i = 0; i < a->size; i++)
	{
		if (ptr->type == QI_VAL)
			sign |= ((TSQuerySign) 1) << (((unsigned int) ptr->qoperand.valcrc) % TSQS_SIGLEN);
		ptr++;
	}

	return sign;
}

static char **
collectTSQueryValues(TSQuery a, int *nvalues_p)
{
	QueryItem  *ptr = GETQUERY(a);
	char	   *operand = GETOPERAND(a);
	char	  **values;
	int			nvalues = 0;
	int			i;

	values = (char **) palloc(sizeof(char *) * a->size);

	for (i = 0; i < a->size; i++)
	{
		if (ptr->type == QI_VAL)
		{
			int			len = ptr->qoperand.length;
			char	   *val;

			val = palloc(len + 1);
			memcpy(val, operand + ptr->qoperand.distance, len);
			val[len] = '\0';

			values[nvalues++] = val;
		}
		ptr++;
	}

	*nvalues_p = nvalues;
	return values;
}

static int
cmp_string(const void *a, const void *b)
{
	const char *sa = *((char *const *) a);
	const char *sb = *((char *const *) b);

	return strcmp(sa, sb);
}

Datum
tsq_mcontains(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY(0);
	TSQuery		ex = PG_GETARG_TSQUERY(1);
	char	  **query_values;
	int			query_nvalues;
	char	  **ex_values;
	int			ex_nvalues;
	bool		result = true;

	/* Extract the query terms into arrays */
	query_values = collectTSQueryValues(query, &query_nvalues);
	ex_values = collectTSQueryValues(ex, &ex_nvalues);

	/* Sort and remove duplicates from both arrays */
	qsort(query_values, query_nvalues, sizeof(char *), cmp_string);
	query_nvalues = qunique(query_values, query_nvalues, sizeof(char *),
							cmp_string);
	qsort(ex_values, ex_nvalues, sizeof(char *), cmp_string);
	ex_nvalues = qunique(ex_values, ex_nvalues, sizeof(char *), cmp_string);

	if (ex_nvalues > query_nvalues)
		result = false;
	else
	{
		int			i;
		int			j = 0;

		for (i = 0; i < ex_nvalues; i++)
		{
			for (; j < query_nvalues; j++)
			{
				if (strcmp(ex_values[i], query_values[j]) == 0)
					break;
			}
			if (j == query_nvalues)
			{
				result = false;
				break;
			}
		}
	}

	PG_RETURN_BOOL(result);
}

Datum
tsq_mcontained(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall2(tsq_mcontains,
										PG_GETARG_DATUM(1),
										PG_GETARG_DATUM(0)));
}
