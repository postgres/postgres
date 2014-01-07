/*-------------------------------------------------------------------------
 *
 * tsquery_rewrite.c
 *	  Utilities for reconstructing tsquery
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsquery_rewrite.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"


static int
addone(int *counters, int last, int total)
{
	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

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

/*
 * If node is equal to ex, replace it with subs. Replacement is actually done
 * by returning either node or a copy of subs.
 */
static QTNode *
findeq(QTNode *node, QTNode *ex, QTNode *subs, bool *isfind)
{
	if ((node->sign & ex->sign) != ex->sign ||
		node->valnode->type != ex->valnode->type)
		return node;

	if (node->flags & QTN_NOCHANGE)
		return node;

	if (node->valnode->type == QI_OPR)
	{
		if (node->valnode->qoperator.oper != ex->valnode->qoperator.oper)
			return node;

		if (node->nchild == ex->nchild)
		{
			if (QTNEq(node, ex))
			{
				QTNFree(node);
				if (subs)
				{
					node = QTNCopy(subs);
					node->flags |= QTN_NOCHANGE;
				}
				else
					node = NULL;
				*isfind = true;
			}
		}
		else if (node->nchild > ex->nchild)
		{
			/*
			 * AND and NOT are commutative, so we check if a subset of the
			 * children match. For example, if tnode is A | B | C, and ex is B
			 * | C, we have a match after we convert tnode to A | (B | C).
			 */
			int		   *counters = (int *) palloc(sizeof(int) * node->nchild);
			int			i;
			QTNode	   *tnode = (QTNode *) palloc(sizeof(QTNode));

			memset(tnode, 0, sizeof(QTNode));
			tnode->child = (QTNode **) palloc(sizeof(QTNode *) * ex->nchild);
			tnode->nchild = ex->nchild;
			tnode->valnode = (QueryItem *) palloc(sizeof(QueryItem));
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

					pfree(tnode->valnode);
					pfree(tnode->child);
					pfree(tnode);
					if (subs)
					{
						tnode = QTNCopy(subs);
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
				pfree(tnode->valnode);
				pfree(tnode->child);
				pfree(tnode);
			}
			else
				QTNSort(node);
			pfree(counters);
		}
	}
	else
	{
		Assert(node->valnode->type == QI_VAL);

		if (node->valnode->qoperand.valcrc != ex->valnode->qoperand.valcrc)
			return node;
		else if (QTNEq(node, ex))
		{
			QTNFree(node);
			if (subs)
			{
				node = QTNCopy(subs);
				node->flags |= QTN_NOCHANGE;
			}
			else
			{
				node = NULL;
			}
			*isfind = true;
		}
	}

	return node;
}

static QTNode *
dofindsubquery(QTNode *root, QTNode *ex, QTNode *subs, bool *isfind)
{
	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	root = findeq(root, ex, subs, isfind);

	if (root && (root->flags & QTN_NOCHANGE) == 0 && root->valnode->type == QI_OPR)
	{
		int			i;

		for (i = 0; i < root->nchild; i++)
			root->child[i] = dofindsubquery(root->child[i], ex, subs, isfind);
	}

	return root;
}

static QTNode *
dropvoidsubtree(QTNode *root)
{
	if (!root)
		return NULL;

	if (root->valnode->type == QI_OPR)
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

		if (root->nchild == 0)
		{
			QTNFree(root);
			root = NULL;
		}
		else if (root->nchild == 1 && root->valnode->qoperator.oper != OP_NOT)
		{
			QTNode	   *nroot = root->child[0];

			pfree(root);
			root = nroot;
		}
	}

	return root;
}

QTNode *
findsubquery(QTNode *root, QTNode *ex, QTNode *subs, bool *isfind)
{
	bool		DidFind = false;

	root = dofindsubquery(root, ex, subs, &DidFind);

	if (!subs && DidFind)
		root = dropvoidsubtree(root);

	if (isfind)
		*isfind = DidFind;

	return root;
}

Datum
tsquery_rewrite_query(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY_COPY(0);
	text	   *in = PG_GETARG_TEXT_P(1);
	TSQuery		rewrited = query;
	MemoryContext outercontext = CurrentMemoryContext;
	MemoryContext oldcontext;
	QTNode	   *tree;
	char	   *buf;
	SPIPlanPtr	plan;
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

	buf = text_to_cstring(in);

	SPI_connect();

	if ((plan = SPI_prepare(buf, 0, NULL)) == NULL)
		elog(ERROR, "SPI_prepare(\"%s\") failed", buf);

	if ((portal = SPI_cursor_open(NULL, plan, NULL, NULL, true)) == NULL)
		elog(ERROR, "SPI_cursor_open(\"%s\") failed", buf);

	SPI_cursor_fetch(portal, true, 100);

	if (SPI_tuptable == NULL ||
		SPI_tuptable->tupdesc->natts != 2 ||
		SPI_gettypeid(SPI_tuptable->tupdesc, 1) != TSQUERYOID ||
		SPI_gettypeid(SPI_tuptable->tupdesc, 2) != TSQUERYOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ts_rewrite query must return two tsquery columns")));

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
				TSQuery		qtex = DatumGetTSQuery(qdata);
				TSQuery		qtsubs = DatumGetTSQuery(sdata);
				QTNode	   *qex,
						   *qsubs = NULL;

				if (qtex->size == 0)
				{
					if (qtex != (TSQuery) DatumGetPointer(qdata))
						pfree(qtex);
					if (qtsubs != (TSQuery) DatumGetPointer(sdata))
						pfree(qtsubs);
					continue;
				}

				qex = QT2QTN(GETQUERY(qtex), GETOPERAND(qtex));

				QTNTernary(qex);
				QTNSort(qex);

				if (qtsubs->size)
					qsubs = QT2QTN(GETQUERY(qtsubs), GETOPERAND(qtsubs));

				oldcontext = MemoryContextSwitchTo(outercontext);
				tree = findsubquery(tree, qex, qsubs, NULL);
				MemoryContextSwitchTo(oldcontext);

				QTNFree(qex);
				if (qtex != (TSQuery) DatumGetPointer(qdata))
					pfree(qtex);
				QTNFree(qsubs);
				if (qtsubs != (TSQuery) DatumGetPointer(sdata))
					pfree(qtsubs);

				if (tree)
				{
					/* ready the tree for another pass */
					QTNClearFlags(tree, QTN_NOCHANGE);
					QTNSort(tree);
				}
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
		rewrited = QTN2QT(tree);
		QTNFree(tree);
		PG_FREE_IF_COPY(query, 0);
	}
	else
	{
		SET_VARSIZE(rewrited, HDRSIZETQ);
		rewrited->size = 0;
	}

	pfree(buf);
	PG_FREE_IF_COPY(in, 1);
	PG_RETURN_POINTER(rewrited);
}

Datum
tsquery_rewrite(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY_COPY(0);
	TSQuery		ex = PG_GETARG_TSQUERY(1);
	TSQuery		subst = PG_GETARG_TSQUERY(2);
	TSQuery		rewrited = query;
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

	tree = findsubquery(tree, qex, subs, NULL);

	QTNFree(qex);
	QTNFree(subs);

	if (!tree)
	{
		SET_VARSIZE(rewrited, HDRSIZETQ);
		rewrited->size = 0;
		PG_FREE_IF_COPY(ex, 1);
		PG_FREE_IF_COPY(subst, 2);
		PG_RETURN_POINTER(rewrited);
	}
	else
	{
		QTNBinary(tree);
		rewrited = QTN2QT(tree);
		QTNFree(tree);
	}

	PG_FREE_IF_COPY(query, 0);
	PG_FREE_IF_COPY(ex, 1);
	PG_FREE_IF_COPY(subst, 2);
	PG_RETURN_POINTER(rewrited);
}
