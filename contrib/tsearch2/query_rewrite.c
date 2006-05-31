#include "postgres.h"
#include "executor/spi.h"

#include "query_util.h"

MemoryContext AggregateContext = NULL;

static int
addone(int *counters, int last, int total)
{
	counters[last]++;
	if (counters[last] >= total)
	{
		if (last == 0)
			return 0;
		if (addone(counters, last - 1, total - 1) == 0)
			return 0;
		counters[last] = counters[last - 1] + 1;
	}
	return 1;
}

static QTNode *
findeq(QTNode * node, QTNode * ex, MemoryType memtype, QTNode * subs, bool *isfind)
{

	if ((node->sign & ex->sign) != ex->sign || node->valnode->type != ex->valnode->type || node->valnode->val != ex->valnode->val)
		return node;

	if (node->flags & QTN_NOCHANGE)
		return node;

	if (node->valnode->type == OPR)
	{
		if (node->nchild == ex->nchild)
		{
			if (QTNEq(node, ex))
			{
				QTNFree(node);
				if (subs)
				{
					node = QTNCopy(subs, memtype);
					node->flags |= QTN_NOCHANGE;
				}
				else
					node = NULL;
				*isfind = true;
			}
		}
		else if (node->nchild > ex->nchild)
		{
			int		   *counters = (int *) palloc(sizeof(int) * node->nchild);
			int			i;
			QTNode	   *tnode = (QTNode *) MEMALLOC(memtype, sizeof(QTNode));

			memset(tnode, 0, sizeof(QTNode));
			tnode->child = (QTNode **) MEMALLOC(memtype, sizeof(QTNode *) * ex->nchild);
			tnode->nchild = ex->nchild;
			tnode->valnode = (ITEM *) MEMALLOC(memtype, sizeof(ITEM));
			*(tnode->valnode) = *(ex->valnode);

			for (i = 0; i < ex->nchild; i++)
				counters[i] = i;

			do
			{
				tnode->sign = 0;
				for (i = 0; i < ex->nchild; i++)
				{
					tnode->child[i] = node->child[counters[i]];
					tnode->sign |= tnode->child[i]->sign;
				}

				if (QTNEq(tnode, ex))
				{
					int			j = 0;

					MEMFREE(memtype, tnode->valnode);
					MEMFREE(memtype, tnode->child);
					MEMFREE(memtype, tnode);
					if (subs)
					{
						tnode = QTNCopy(subs, memtype);
						tnode->flags = QTN_NOCHANGE | QTN_NEEDFREE;
					}
					else
						tnode = NULL;

					node->child[counters[0]] = tnode;

					for (i = 1; i < ex->nchild; i++)
						node->child[counters[i]] = NULL;
					for (i = 0; i < node->nchild; i++)
					{
						if (node->child[i])
						{
							node->child[j] = node->child[i];
							j++;
						}
					}

					node->nchild = j;

					*isfind = true;

					break;
				}
			} while (addone(counters, ex->nchild - 1, node->nchild));
			if (tnode && (tnode->flags & QTN_NOCHANGE) == 0)
			{
				MEMFREE(memtype, tnode->valnode);
				MEMFREE(memtype, tnode->child);
				MEMFREE(memtype, tnode);
			}
			else
				QTNSort(node);
			pfree(counters);
		}
	}
	else if (QTNEq(node, ex))
	{
		QTNFree(node);
		if (subs)
		{
			node = QTNCopy(subs, memtype);
			node->flags |= QTN_NOCHANGE;
		}
		else
		{
			node = NULL;
		}
		*isfind = true;
	}

	return node;
}

static QTNode *
dofindsubquery(QTNode * root, QTNode * ex, MemoryType memtype, QTNode * subs, bool *isfind)
{
	root = findeq(root, ex, memtype, subs, isfind);

	if (root && (root->flags & QTN_NOCHANGE) == 0 && root->valnode->type == OPR)
	{
		int			i;

		for (i = 0; i < root->nchild; i++)
			root->child[i] = dofindsubquery(root->child[i], ex, memtype, subs, isfind);
	}

	return root;
}

static QTNode *
dropvoidsubtree(QTNode * root)
{

	if (!root)
		return NULL;

	if (root->valnode->type == OPR)
	{
		int			i,
					j = 0;

		for (i = 0; i < root->nchild; i++)
		{
			if (root->child[i])
			{
				root->child[j] = root->child[i];
				j++;
			}
		}

		root->nchild = j;

		if (root->valnode->val == (int4) '!' && root->nchild == 0)
		{
			QTNFree(root);
			root = NULL;
		}
		else if (root->nchild == 1)
		{
			QTNode	   *nroot = root->child[0];

			pfree(root);
			root = nroot;
		}
	}

	return root;
}

static QTNode *
findsubquery(QTNode * root, QTNode * ex, MemoryType memtype, QTNode * subs, bool *isfind)
{
	bool		DidFind = false;

	root = dofindsubquery(root, ex, memtype, subs, &DidFind);

	if (!subs && DidFind)
		root = dropvoidsubtree(root);

	if (isfind)
		*isfind = DidFind;

	return root;
}

static Oid	tsqOid = InvalidOid;
static void
get_tsq_Oid(void)
{
	int			ret;
	bool		isnull;

	if ((ret = SPI_exec("select oid from pg_type where typname='tsquery'", 1)) < 0)
		/* internal error */
		elog(ERROR, "SPI_exec to get tsquery oid returns %d", ret);

	if (SPI_processed < 1)
		/* internal error */
		elog(ERROR, "there is no tsvector type");
	tsqOid = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
	if (tsqOid == InvalidOid)
		/* internal error */
		elog(ERROR, "tsquery type has InvalidOid");
}


PG_FUNCTION_INFO_V1(tsquery_rewrite);
PG_FUNCTION_INFO_V1(rewrite_accum);
Datum		rewrite_accum(PG_FUNCTION_ARGS);

Datum
rewrite_accum(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *acc = (QUERYTYPE *) PG_GETARG_POINTER(0);
	ArrayType  *qa = (ArrayType *) DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(1)));
	QUERYTYPE  *q;
	QTNode	   *qex,
			   *subs = NULL,
			   *acctree;
	bool		isfind = false;
	Datum	   *elemsp;
	int			nelemsp;

	AggregateContext = ((AggState *) fcinfo->context)->aggcontext;

	if (acc == NULL || PG_ARGISNULL(0))
	{
		acc = (QUERYTYPE *) MEMALLOC(AggMemory, sizeof(QUERYTYPE));
		acc->len = HDRSIZEQT;
		acc->size = 0;
	}

	if (qa == NULL || PG_ARGISNULL(1))
	{
		PG_FREE_IF_COPY(qa, 1);
		PG_RETURN_POINTER(acc);
	}

	if (ARR_NDIM(qa) != 1)
		elog(ERROR, "array must be one-dimensional, not %d dimension", ARR_NDIM(qa));

	if (ArrayGetNItems(ARR_NDIM(qa), ARR_DIMS(qa)) != 3)
		elog(ERROR, "array should have only three elements");

	if (tsqOid == InvalidOid)
	{
		SPI_connect();
		get_tsq_Oid();
		SPI_finish();
	}

	if (ARR_ELEMTYPE(qa) != tsqOid)
		elog(ERROR, "array should contain tsquery type");

	deconstruct_array(qa, tsqOid, -1, false, 'i', &elemsp, NULL, &nelemsp);

	q = (QUERYTYPE *) DatumGetPointer(elemsp[0]);
	if (q->size == 0)
	{
		pfree(elemsp);
		PG_RETURN_POINTER(acc);
	}

	if (!acc->size)
	{
		if (acc->len > HDRSIZEQT)
		{
			pfree(elemsp);
			PG_RETURN_POINTER(acc);
		}
		else
			acctree = QT2QTN(GETQUERY(q), GETOPERAND(q));
	}
	else
		acctree = QT2QTN(GETQUERY(acc), GETOPERAND(acc));

	QTNTernary(acctree);
	QTNSort(acctree);

	q = (QUERYTYPE *) DatumGetPointer(elemsp[1]);
	if (q->size == 0)
	{
		pfree(elemsp);
		PG_RETURN_POINTER(acc);
	}
	qex = QT2QTN(GETQUERY(q), GETOPERAND(q));
	QTNTernary(qex);
	QTNSort(qex);

	q = (QUERYTYPE *) DatumGetPointer(elemsp[2]);
	if (q->size)
		subs = QT2QTN(GETQUERY(q), GETOPERAND(q));

	acctree = findsubquery(acctree, qex, PlainMemory, subs, &isfind);

	if (isfind || !acc->size)
	{
		/* pfree( acc ); do not pfree(p), because nodeAgg.c will */
		if (acctree)
		{
			QTNBinary(acctree);
			acc = QTN2QT(acctree, AggMemory);
		}
		else
		{
			acc = (QUERYTYPE *) MEMALLOC(AggMemory, HDRSIZEQT * 2);
			acc->len = HDRSIZEQT * 2;
			acc->size = 0;
		}
	}

	pfree(elemsp);
	QTNFree(qex);
	QTNFree(subs);
	QTNFree(acctree);

	PG_RETURN_POINTER(acc);
}

PG_FUNCTION_INFO_V1(rewrite_finish);
Datum		rewrite_finish(PG_FUNCTION_ARGS);

Datum
rewrite_finish(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *acc = (QUERYTYPE *) PG_GETARG_POINTER(0);
	QUERYTYPE  *rewrited;

	if (acc == NULL || PG_ARGISNULL(0) || acc->size == 0)
	{
		acc = (QUERYTYPE *) palloc(sizeof(QUERYTYPE));
		acc->len = HDRSIZEQT;
		acc->size = 0;
	}

	rewrited = (QUERYTYPE *) palloc(acc->len);
	memcpy(rewrited, acc, acc->len);
	pfree(acc);

	PG_RETURN_POINTER(rewrited);
}

Datum		tsquery_rewrite(PG_FUNCTION_ARGS);

Datum
tsquery_rewrite(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *query = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0)));
	text	   *in = PG_GETARG_TEXT_P(1);
	QUERYTYPE  *rewrited = query;
	QTNode	   *tree;
	char	   *buf;
	void	   *plan;
	Portal		portal;
	bool		isnull;
	int			i;

	if (query->size == 0)
	{
		PG_FREE_IF_COPY(in, 1);
		PG_RETURN_POINTER(rewrited);
	}

	tree = QT2QTN(GETQUERY(query), GETOPERAND(query));
	QTNTernary(tree);
	QTNSort(tree);

	buf = (char *) palloc(VARSIZE(in));
	memcpy(buf, VARDATA(in), VARSIZE(in) - VARHDRSZ);
	buf[VARSIZE(in) - VARHDRSZ] = '\0';

	SPI_connect();

	if (tsqOid == InvalidOid)
		get_tsq_Oid();

	if ((plan = SPI_prepare(buf, 0, NULL)) == NULL)
		elog(ERROR, "SPI_prepare('%s') returns NULL", buf);

	if ((portal = SPI_cursor_open(NULL, plan, NULL, NULL, false)) == NULL)
		elog(ERROR, "SPI_cursor_open('%s') returns NULL", buf);

	SPI_cursor_fetch(portal, true, 100);

	if (SPI_tuptable->tupdesc->natts != 2)
		elog(ERROR, "number of fields doesn't equal to 2");

	if (SPI_gettypeid(SPI_tuptable->tupdesc, 1) != tsqOid)
		elog(ERROR, "column #1 isn't of tsquery type");

	if (SPI_gettypeid(SPI_tuptable->tupdesc, 2) != tsqOid)
		elog(ERROR, "column #2 isn't of tsquery type");

	while (SPI_processed > 0 && tree)
	{
		for (i = 0; i < SPI_processed && tree; i++)
		{
			Datum		qdata = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);
			Datum		sdata;

			if (isnull)
				continue;

			sdata = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2, &isnull);

			if (!isnull)
			{
				QUERYTYPE  *qtex = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM(qdata));
				QUERYTYPE  *qtsubs = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM(sdata));
				QTNode	   *qex,
						   *qsubs = NULL;

				if (qtex->size == 0)
				{
					if (qtex != (QUERYTYPE *) DatumGetPointer(qdata))
						pfree(qtex);
					if (qtsubs != (QUERYTYPE *) DatumGetPointer(sdata))
						pfree(qtsubs);
					continue;
				}

				qex = QT2QTN(GETQUERY(qtex), GETOPERAND(qtex));

				QTNTernary(qex);
				QTNSort(qex);

				if (qtsubs->size)
					qsubs = QT2QTN(GETQUERY(qtsubs), GETOPERAND(qtsubs));

				tree = findsubquery(tree, qex, SPIMemory, qsubs, NULL);

				QTNFree(qex);
				if (qtex != (QUERYTYPE *) DatumGetPointer(qdata))
					pfree(qtex);
				QTNFree(qsubs);
				if (qtsubs != (QUERYTYPE *) DatumGetPointer(sdata))
					pfree(qtsubs);
			}
		}

		SPI_freetuptable(SPI_tuptable);
		SPI_cursor_fetch(portal, true, 100);
	}

	SPI_freetuptable(SPI_tuptable);
	SPI_cursor_close(portal);
	SPI_freeplan(plan);
	SPI_finish();


	if (tree)
	{
		QTNBinary(tree);
		rewrited = QTN2QT(tree, PlainMemory);
		QTNFree(tree);
		PG_FREE_IF_COPY(query, 0);
	}
	else
	{
		rewrited->len = HDRSIZEQT;
		rewrited->size = 0;
	}

	pfree(buf);
	PG_FREE_IF_COPY(in, 1);
	PG_RETURN_POINTER(rewrited);
}


PG_FUNCTION_INFO_V1(tsquery_rewrite_query);
Datum		tsquery_rewrite_query(PG_FUNCTION_ARGS);

Datum
tsquery_rewrite_query(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *query = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0)));
	QUERYTYPE  *ex = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(1)));
	QUERYTYPE  *subst = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(2)));
	QUERYTYPE  *rewrited = query;
	QTNode	   *tree,
			   *qex,
			   *subs = NULL;

	if (query->size == 0 || ex->size == 0)
	{
		PG_FREE_IF_COPY(ex, 1);
		PG_FREE_IF_COPY(subst, 2);
		PG_RETURN_POINTER(rewrited);
	}

	tree = QT2QTN(GETQUERY(query), GETOPERAND(query));
	QTNTernary(tree);
	QTNSort(tree);

	qex = QT2QTN(GETQUERY(ex), GETOPERAND(ex));
	QTNTernary(qex);
	QTNSort(qex);

	if (subst->size)
		subs = QT2QTN(GETQUERY(subst), GETOPERAND(subst));

	tree = findsubquery(tree, qex, PlainMemory, subs, NULL);
	QTNFree(qex);
	QTNFree(subs);

	if (!tree)
	{
		rewrited->len = HDRSIZEQT;
		rewrited->size = 0;
		PG_FREE_IF_COPY(ex, 1);
		PG_FREE_IF_COPY(subst, 2);
		PG_RETURN_POINTER(rewrited);
	}
	else
	{
		QTNBinary(tree);
		rewrited = QTN2QT(tree, PlainMemory);
		QTNFree(tree);
	}

	PG_FREE_IF_COPY(query, 0);
	PG_FREE_IF_COPY(ex, 1);
	PG_FREE_IF_COPY(subst, 2);
	PG_RETURN_POINTER(rewrited);
}
