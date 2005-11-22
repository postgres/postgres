#include "postgres.h"
#include "fmgr.h"

#include "query_util.h"

PG_FUNCTION_INFO_V1(tsquery_numnode);
Datum		tsquery_numnode(PG_FUNCTION_ARGS);

Datum
tsquery_numnode(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *query = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0)));
	int			nnode = query->size;

	PG_FREE_IF_COPY(query, 0);
	PG_RETURN_INT32(nnode);
}

static QTNode *
join_tsqueries(QUERYTYPE * a, QUERYTYPE * b)
{
	QTNode	   *res = (QTNode *) palloc0(sizeof(QTNode));

	res->flags |= QTN_NEEDFREE;

	res->valnode = (ITEM *) palloc0(sizeof(ITEM));
	res->valnode->type = OPR;

	res->child = (QTNode **) palloc0(sizeof(QTNode *) * 2);
	res->child[0] = QT2QTN(GETQUERY(b), GETOPERAND(b));
	res->child[1] = QT2QTN(GETQUERY(a), GETOPERAND(a));
	res->nchild = 2;

	return res;
}

PG_FUNCTION_INFO_V1(tsquery_and);
Datum		tsquery_and(PG_FUNCTION_ARGS);

Datum
tsquery_and(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *a = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0)));
	QUERYTYPE  *b = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(1)));
	QTNode	   *res;
	QUERYTYPE  *query;

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

	res = join_tsqueries(a, b);

	res->valnode->val = '&';

	query = QTN2QT(res, PlainMemory);

	QTNFree(res);
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);

	PG_RETURN_POINTER(query);
}

PG_FUNCTION_INFO_V1(tsquery_or);
Datum		tsquery_or(PG_FUNCTION_ARGS);

Datum
tsquery_or(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *a = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0)));
	QUERYTYPE  *b = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(1)));
	QTNode	   *res;
	QUERYTYPE  *query;

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

	res = join_tsqueries(a, b);

	res->valnode->val = '|';

	query = QTN2QT(res, PlainMemory);

	QTNFree(res);
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);

	PG_RETURN_POINTER(query);
}

PG_FUNCTION_INFO_V1(tsquery_not);
Datum		tsquery_not(PG_FUNCTION_ARGS);

Datum
tsquery_not(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *a = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0)));
	QTNode	   *res;
	QUERYTYPE  *query;

	if (a->size == 0)
		PG_RETURN_POINTER(a);

	res = (QTNode *) palloc0(sizeof(QTNode));

	res->flags |= QTN_NEEDFREE;

	res->valnode = (ITEM *) palloc0(sizeof(ITEM));
	res->valnode->type = OPR;
	res->valnode->val = '!';

	res->child = (QTNode **) palloc0(sizeof(QTNode *));
	res->child[0] = QT2QTN(GETQUERY(a), GETOPERAND(a));
	res->nchild = 1;

	query = QTN2QT(res, PlainMemory);

	QTNFree(res);
	PG_FREE_IF_COPY(a, 0);

	PG_RETURN_POINTER(query);
}

static int
CompareTSQ(QUERYTYPE * a, QUERYTYPE * b)
{
	if (a->size != b->size)
	{
		return (a->size < b->size) ? -1 : 1;
	}
	else if (a->len != b->len)
	{
		return (a->len < b->len) ? -1 : 1;
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

PG_FUNCTION_INFO_V1(tsquery_cmp);
\
Datum		tsquery_cmp(PG_FUNCTION_ARGS);

Datum
tsquery_cmp(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *a = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0)));
	QUERYTYPE  *b = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(1)));
	int			res = CompareTSQ(a, b);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);

	PG_RETURN_INT32(res);
}

#define CMPFUNC( NAME, ACTION )										\
PG_FUNCTION_INFO_V1(NAME);										\
Datum	NAME(PG_FUNCTION_ARGS);										\
													\
Datum													\
NAME(PG_FUNCTION_ARGS) {										\
	QUERYTYPE  *a = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0)));	\
	QUERYTYPE  *b = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(1)));	\
	int res = CompareTSQ(a,b);									\
													\
	PG_FREE_IF_COPY(a,0);										\
	PG_FREE_IF_COPY(b,1);										\
													\
	PG_RETURN_BOOL( ACTION );									\
}

CMPFUNC(tsquery_lt, res < 0);
CMPFUNC(tsquery_le, res <= 0);
CMPFUNC(tsquery_eq, res == 0);
CMPFUNC(tsquery_ge, res >= 0);
CMPFUNC(tsquery_gt, res > 0);
CMPFUNC(tsquery_ne, res != 0);
