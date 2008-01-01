/*-------------------------------------------------------------------------
 *
 * tsquery_op.c
 *	  Various operations with tsquery
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/tsquery_op.c,v 1.3 2008/01/01 19:45:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "tsearch/ts_type.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_utils.h"
#include "utils/pg_crc.h"

Datum
tsquery_numnode(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY(0);
	int			nnode = query->size;

	PG_FREE_IF_COPY(query, 0);
	PG_RETURN_INT32(nnode);
}

static QTNode *
join_tsqueries(TSQuery a, TSQuery b, int8 operator)
{
	QTNode	   *res = (QTNode *) palloc0(sizeof(QTNode));

	res->flags |= QTN_NEEDFREE;

	res->valnode = (QueryItem *) palloc0(sizeof(QueryItem));
	res->valnode->type = QI_OPR;
	res->valnode->operator.oper = operator;

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

	res = join_tsqueries(a, b, OP_AND);

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

	res = join_tsqueries(a, b, OP_OR);

	query = QTN2QT(res);

	QTNFree(res);
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);

	PG_RETURN_POINTER(query);
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
	res->valnode->operator.oper = OP_NOT;

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
	else
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
}

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
			sign |= ((TSQuerySign) 1) << (ptr->operand.valcrc % TSQS_SIGLEN);
		ptr++;
	}

	return sign;
}

Datum
tsq_mcontains(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY(0);
	TSQuery		ex = PG_GETARG_TSQUERY(1);
	TSQuerySign sq,
				se;
	int			i,
				j;
	QueryItem  *iq,
			   *ie;

	if (query->size < ex->size)
	{
		PG_FREE_IF_COPY(query, 0);
		PG_FREE_IF_COPY(ex, 1);

		PG_RETURN_BOOL(false);
	}

	sq = makeTSQuerySign(query);
	se = makeTSQuerySign(ex);

	if ((sq & se) != se)
	{
		PG_FREE_IF_COPY(query, 0);
		PG_FREE_IF_COPY(ex, 1);

		PG_RETURN_BOOL(false);
	}

	ie = GETQUERY(ex);

	for (i = 0; i < ex->size; i++)
	{
		iq = GETQUERY(query);
		if (ie[i].type != QI_VAL)
			continue;
		for (j = 0; j < query->size; j++)
			if (iq[j].type == QI_VAL && ie[i].operand.valcrc == iq[j].operand.valcrc)
			{
				j = query->size + 1;
				break;
			}
		if (j == query->size)
		{
			PG_FREE_IF_COPY(query, 0);
			PG_FREE_IF_COPY(ex, 1);

			PG_RETURN_BOOL(false);
		}
	}

	PG_FREE_IF_COPY(query, 0);
	PG_FREE_IF_COPY(ex, 1);

	PG_RETURN_BOOL(true);
}

Datum
tsq_mcontained(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(
					DirectFunctionCall2(
										tsq_mcontains,
										PG_GETARG_DATUM(1),
										PG_GETARG_DATUM(0)
										)
		);
}
