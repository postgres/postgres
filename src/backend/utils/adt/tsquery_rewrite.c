/*-------------------------------------------------------------------------
 *
 * tsquery_rewrite.c
 *	  Utilities for reconstructing tsquery
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
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


/*
 * If "node" is equal to "ex", return a copy of "subs" instead.
 * If "ex" matches a subset of node's children, return a modified version
 * of "node" in which those children are replaced with a copy of "subs".
 * Otherwise return "node" unmodified.
 *
 * The QTN_NOCHANGE bit is set in successfully modified nodes, so that
 * we won't uselessly recurse into them.
 * Also, set *isfind true if we make a replacement.
 */
static QTNode *
findeq(QTNode *node, QTNode *ex, QTNode *subs, bool *isfind)
{
	/* Can't match unless signature matches and node type matches. */
	if ((node->sign & ex->sign) != ex->sign ||
		node->valnode->type != ex->valnode->type)
		return node;

	/* Ignore nodes marked NOCHANGE, too. */
	if (node->flags & QTN_NOCHANGE)
		return node;

	if (node->valnode->type == QI_OPR)
	{
		/* Must be same operator. */
		if (node->valnode->qoperator.oper != ex->valnode->qoperator.oper)
			return node;

		if (node->nchild == ex->nchild)
		{
			/*
			 * Simple case: when same number of children, match if equal.
			 * (This is reliable when the children were sorted earlier.)
			 */
			if (QTNEq(node, ex))
			{
				/* Match; delete node and return a copy of subs instead. */
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
		else if (node->nchild > ex->nchild && ex->nchild > 0)
		{
			/*
			 * AND and OR are commutative/associative, so we should check if a
			 * subset of the children match.  For example, if node is A|B|C,
			 * and ex is B|C, we have a match after we notionally convert node
			 * to A|(B|C).  This does not work for NOT or PHRASE nodes, but we
			 * can't get here for those node types because they have a fixed
			 * number of children.
			 *
			 * Because we expect that the children are sorted, it suffices to
			 * make one pass through the two lists to find the matches.
			 */
			bool	   *matched;
			int			nmatched;
			int			i,
						j;

			/* Assert that the subset rule is OK */
			Assert(node->valnode->qoperator.oper == OP_AND ||
				   node->valnode->qoperator.oper == OP_OR);

			/* matched[] will record which children of node matched */
			matched = (bool *) palloc0(node->nchild * sizeof(bool));
			nmatched = 0;
			i = j = 0;
			while (i < node->nchild && j < ex->nchild)
			{
				int			cmp = QTNodeCompare(node->child[i], ex->child[j]);

				if (cmp == 0)
				{
					/* match! */
					matched[i] = true;
					nmatched++;
					i++, j++;
				}
				else if (cmp < 0)
				{
					/* node->child[i] has no match, ignore it */
					i++;
				}
				else
				{
					/* ex->child[j] has no match; we can give up immediately */
					break;
				}
			}

			if (nmatched == ex->nchild)
			{
				/* collapse out the matched children of node */
				j = 0;
				for (i = 0; i < node->nchild; i++)
				{
					if (matched[i])
						QTNFree(node->child[i]);
					else
						node->child[j++] = node->child[i];
				}

				/* and instead insert a copy of subs */
				if (subs)
				{
					subs = QTNCopy(subs);
					subs->flags |= QTN_NOCHANGE;
					node->child[j++] = subs;
				}

				node->nchild = j;

				/*
				 * At this point we might have a node with zero or one child,
				 * which should be simplified.  But we leave it to our caller
				 * (dofindsubquery) to take care of that.
				 */

				/*
				 * Re-sort the node to put new child in the right place.  This
				 * is a bit bogus, because it won't matter for findsubquery's
				 * remaining processing, and it's insufficient to prepare the
				 * tree for another search (we would need to re-flatten as
				 * well, and we don't want to do that because we'd lose the
				 * QTN_NOCHANGE marking on the new child).  But it's needed to
				 * keep the results the same as the regression tests expect.
				 */
				QTNSort(node);

				*isfind = true;
			}

			pfree(matched);
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

/*
 * Recursive guts of findsubquery(): attempt to replace "ex" with "subs"
 * at the root node, and if we failed to do so, recursively match against
 * child nodes.
 *
 * Delete any void subtrees resulting from the replacement.
 * In the following example '5' is replaced by empty operand:
 *
 *	  AND		->	  6
 *	 /	 \
 *	5	 OR
 *		/  \
 *	   6	5
 */
static QTNode *
dofindsubquery(QTNode *root, QTNode *ex, QTNode *subs, bool *isfind)
{
	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	/* also, since it's a bit expensive, let's check for query cancel. */
	CHECK_FOR_INTERRUPTS();

	/* match at the node itself */
	root = findeq(root, ex, subs, isfind);

	/* unless we matched here, consider matches at child nodes */
	if (root && (root->flags & QTN_NOCHANGE) == 0 &&
		root->valnode->type == QI_OPR)
	{
		int			i,
					j = 0;

		/*
		 * Any subtrees that are replaced by NULL must be dropped from the
		 * tree.
		 */
		for (i = 0; i < root->nchild; i++)
		{
			root->child[j] = dofindsubquery(root->child[i], ex, subs, isfind);
			if (root->child[j])
				j++;
		}

		root->nchild = j;

		/*
		 * If we have just zero or one remaining child node, simplify out this
		 * operator node.
		 */
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

/*
 * Substitute "subs" for "ex" throughout the QTNode tree at root.
 *
 * If isfind isn't NULL, set *isfind to show whether we made any substitution.
 *
 * Both "root" and "ex" must have been through QTNTernary and QTNSort
 * to ensure reliable matching.
 */
QTNode *
findsubquery(QTNode *root, QTNode *ex, QTNode *subs, bool *isfind)
{
	bool		DidFind = false;

	root = dofindsubquery(root, ex, subs, &DidFind);

	if (isfind)
		*isfind = DidFind;

	return root;
}

Datum
tsquery_rewrite_query(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY_COPY(0);
	text	   *in = PG_GETARG_TEXT_PP(1);
	TSQuery		rewrited = query;
	MemoryContext outercontext = CurrentMemoryContext;
	MemoryContext oldcontext;
	QTNode	   *tree;
	char	   *buf;
	SPIPlanPtr	plan;
	Portal		portal;
	bool		isnull;

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
		uint64		i;

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
					QTNTernary(tree);
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
