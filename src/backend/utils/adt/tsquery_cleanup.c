/*-------------------------------------------------------------------------
 *
 * tsquery_cleanup.c
 *	 Cleanup query from NOT values and/or stopword
 *	 Utility functions to correct work.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsquery_cleanup.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "tsearch/ts_utils.h"

typedef struct NODE
{
	struct NODE *left;
	struct NODE *right;
	QueryItem  *valnode;
} NODE;

/*
 * make query tree from plain view of query
 */
static NODE *
maketree(QueryItem *in)
{
	NODE	   *node = (NODE *) palloc(sizeof(NODE));

	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	node->valnode = in;
	node->right = node->left = NULL;
	if (in->type == QI_OPR)
	{
		node->right = maketree(in + 1);
		if (in->qoperator.oper != OP_NOT)
			node->left = maketree(in + in->qoperator.left);
	}
	return node;
}

/*
 * Internal state for plaintree and plainnode
 */
typedef struct
{
	QueryItem  *ptr;
	int			len;			/* allocated size of ptr */
	int			cur;			/* number of elements in ptr */
} PLAINTREE;

static void
plainnode(PLAINTREE *state, NODE *node)
{
	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	if (state->cur == state->len)
	{
		state->len *= 2;
		state->ptr = (QueryItem *) repalloc((void *) state->ptr, state->len * sizeof(QueryItem));
	}
	memcpy((void *) &(state->ptr[state->cur]), (void *) node->valnode, sizeof(QueryItem));
	if (node->valnode->type == QI_VAL)
		state->cur++;
	else if (node->valnode->qoperator.oper == OP_NOT)
	{
		state->ptr[state->cur].qoperator.left = 1;
		state->cur++;
		plainnode(state, node->right);
	}
	else
	{
		int			cur = state->cur;

		state->cur++;
		plainnode(state, node->right);
		state->ptr[cur].qoperator.left = state->cur - cur;
		plainnode(state, node->left);
	}
	pfree(node);
}

/*
 * make plain view of tree from a NODE-tree representation
 */
static QueryItem *
plaintree(NODE *root, int *len)
{
	PLAINTREE	pl;

	pl.cur = 0;
	pl.len = 16;
	if (root && (root->valnode->type == QI_VAL || root->valnode->type == QI_OPR))
	{
		pl.ptr = (QueryItem *) palloc(pl.len * sizeof(QueryItem));
		plainnode(&pl, root);
	}
	else
		pl.ptr = NULL;
	*len = pl.cur;
	return pl.ptr;
}

static void
freetree(NODE *node)
{
	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	if (!node)
		return;
	if (node->left)
		freetree(node->left);
	if (node->right)
		freetree(node->right);
	pfree(node);
}

/*
 * clean tree for ! operator.
 * It's useful for debug, but in
 * other case, such view is used with search in index.
 * Operator ! always return TRUE
 */
static NODE *
clean_NOT_intree(NODE *node)
{
	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	if (node->valnode->type == QI_VAL)
		return node;

	if (node->valnode->qoperator.oper == OP_NOT)
	{
		freetree(node);
		return NULL;
	}

	/* operator & or | */
	if (node->valnode->qoperator.oper == OP_OR)
	{
		if ((node->left = clean_NOT_intree(node->left)) == NULL ||
			(node->right = clean_NOT_intree(node->right)) == NULL)
		{
			freetree(node);
			return NULL;
		}
	}
	else
	{
		NODE	   *res = node;

		Assert(node->valnode->qoperator.oper == OP_AND ||
			   node->valnode->qoperator.oper == OP_PHRASE);

		node->left = clean_NOT_intree(node->left);
		node->right = clean_NOT_intree(node->right);
		if (node->left == NULL && node->right == NULL)
		{
			pfree(node);
			res = NULL;
		}
		else if (node->left == NULL)
		{
			res = node->right;
			pfree(node);
		}
		else if (node->right == NULL)
		{
			res = node->left;
			pfree(node);
		}
		return res;
	}
	return node;
}

QueryItem *
clean_NOT(QueryItem *ptr, int *len)
{
	NODE	   *root = maketree(ptr);

	return plaintree(clean_NOT_intree(root), len);
}


/*
 * Remove QI_VALSTOP (stopword) nodes from query tree.
 *
 * Returns NULL if the query degenerates to nothing.  Input must not be NULL.
 *
 * When we remove a phrase operator due to removing one or both of its
 * arguments, we might need to adjust the distance of a parent phrase
 * operator.  For example, 'a' is a stopword, so:
 *		(b <-> a) <-> c  should become	b <2> c
 *		b <-> (a <-> c)  should become	b <2> c
 *		(b <-> (a <-> a)) <-> c  should become	b <3> c
 *		b <-> ((a <-> a) <-> c)  should become	b <3> c
 * To handle that, we define two output parameters:
 *		ladd: amount to add to a phrase distance to the left of this node
 *		radd: amount to add to a phrase distance to the right of this node
 * We need two outputs because we could need to bubble up adjustments to two
 * different parent phrase operators.  Consider
 *		w <-> (((a <-> x) <2> (y <3> a)) <-> z)
 * After we've removed the two a's and are considering the <2> node (which is
 * now just x <2> y), we have an ladd distance of 1 that needs to propagate
 * up to the topmost (leftmost) <->, and an radd distance of 3 that needs to
 * propagate to the rightmost <->, so that we'll end up with
 *		w <2> ((x <2> y) <4> z)
 * Near the bottom of the tree, we may have subtrees consisting only of
 * stopwords.  The distances of any phrase operators within such a subtree are
 * summed and propagated to both ladd and radd, since we don't know which side
 * of the lowest surviving phrase operator we are in.  The rule is that any
 * subtree that degenerates to NULL must return equal values of ladd and radd,
 * and the parent node dealing with it should incorporate only one of those.
 *
 * Currently, we only implement this adjustment for adjacent phrase operators.
 * Thus for example 'x <-> ((a <-> y) | z)' will become 'x <-> (y | z)', which
 * isn't ideal, but there is no way to represent the really desired semantics
 * without some redesign of the tsquery structure.  Certainly it would not be
 * any better to convert that to 'x <2> (y | z)'.  Since this is such a weird
 * corner case, let it go for now.  But we can fix it in cases where the
 * intervening non-phrase operator also gets removed, for example
 * '((x <-> a) | a) <-> y' will become 'x <2> y'.
 */
static NODE *
clean_stopword_intree(NODE *node, int *ladd, int *radd)
{
	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	/* default output parameters indicate no change in parent distance */
	*ladd = *radd = 0;

	if (node->valnode->type == QI_VAL)
		return node;
	else if (node->valnode->type == QI_VALSTOP)
	{
		pfree(node);
		return NULL;
	}

	Assert(node->valnode->type == QI_OPR);

	if (node->valnode->qoperator.oper == OP_NOT)
	{
		/* NOT doesn't change pattern width, so just report child distances */
		node->right = clean_stopword_intree(node->right, ladd, radd);
		if (!node->right)
		{
			freetree(node);
			return NULL;
		}
	}
	else
	{
		NODE	   *res = node;
		bool		isphrase;
		int			ndistance,
					lladd,
					lradd,
					rladd,
					rradd;

		/* First, recurse */
		node->left = clean_stopword_intree(node->left, &lladd, &lradd);
		node->right = clean_stopword_intree(node->right, &rladd, &rradd);

		/* Check if current node is OP_PHRASE, get its distance */
		isphrase = (node->valnode->qoperator.oper == OP_PHRASE);
		ndistance = isphrase ? node->valnode->qoperator.distance : 0;

		if (node->left == NULL && node->right == NULL)
		{
			/*
			 * When we collapse out a phrase node entirely, propagate its own
			 * distance into both *ladd and *radd; it is the responsibility of
			 * the parent node to count it only once.  Also, for a phrase
			 * node, distances coming from children are summed and propagated
			 * up to parent (we assume lladd == lradd and rladd == rradd, else
			 * rule was broken at a lower level).  But if this isn't a phrase
			 * node, take the larger of the two child distances; that
			 * corresponds to what TS_execute will do in non-stopword cases.
			 */
			if (isphrase)
				*ladd = *radd = lladd + ndistance + rladd;
			else
				*ladd = *radd = Max(lladd, rladd);
			freetree(node);
			return NULL;
		}
		else if (node->left == NULL)
		{
			/* Removing this operator and left subnode */
			/* lladd and lradd are equal/redundant, don't count both */
			if (isphrase)
			{
				/* operator's own distance must propagate to left */
				*ladd = lladd + ndistance + rladd;
				*radd = rradd;
			}
			else
			{
				/* at non-phrase op, just forget the left subnode entirely */
				*ladd = rladd;
				*radd = rradd;
			}
			res = node->right;
			pfree(node);
		}
		else if (node->right == NULL)
		{
			/* Removing this operator and right subnode */
			/* rladd and rradd are equal/redundant, don't count both */
			if (isphrase)
			{
				/* operator's own distance must propagate to right */
				*ladd = lladd;
				*radd = lradd + ndistance + rradd;
			}
			else
			{
				/* at non-phrase op, just forget the right subnode entirely */
				*ladd = lladd;
				*radd = lradd;
			}
			res = node->left;
			pfree(node);
		}
		else if (isphrase)
		{
			/* Absorb appropriate corrections at this level */
			node->valnode->qoperator.distance += lradd + rladd;
			/* Propagate up any unaccounted-for corrections */
			*ladd = lladd;
			*radd = rradd;
		}
		else
		{
			/* We're keeping a non-phrase operator, so ladd/radd remain 0 */
		}

		return res;
	}
	return node;
}

/*
 * Number of elements in query tree
 */
static int32
calcstrlen(NODE *node)
{
	int32		size = 0;

	if (node->valnode->type == QI_VAL)
	{
		size = node->valnode->qoperand.length + 1;
	}
	else
	{
		Assert(node->valnode->type == QI_OPR);

		size = calcstrlen(node->right);
		if (node->valnode->qoperator.oper != OP_NOT)
			size += calcstrlen(node->left);
	}

	return size;
}

/*
 * Remove QI_VALSTOP (stopword) nodes from TSQuery.
 */
TSQuery
cleanup_tsquery_stopwords(TSQuery in)
{
	int32		len,
				lenstr,
				commonlen,
				i;
	NODE	   *root;
	int			ladd,
				radd;
	TSQuery		out;
	QueryItem  *items;
	char	   *operands;

	if (in->size == 0)
		return in;

	/* eliminate stop words */
	root = clean_stopword_intree(maketree(GETQUERY(in)), &ladd, &radd);
	if (root == NULL)
	{
		ereport(NOTICE,
				(errmsg("text-search query contains only stop words or doesn't contain lexemes, ignored")));
		out = palloc(HDRSIZETQ);
		out->size = 0;
		SET_VARSIZE(out, HDRSIZETQ);
		return out;
	}

	/*
	 * Build TSQuery from plain view
	 */

	lenstr = calcstrlen(root);
	items = plaintree(root, &len);
	commonlen = COMPUTESIZE(len, lenstr);

	out = palloc(commonlen);
	SET_VARSIZE(out, commonlen);
	out->size = len;

	memcpy(GETQUERY(out), items, len * sizeof(QueryItem));

	items = GETQUERY(out);
	operands = GETOPERAND(out);
	for (i = 0; i < out->size; i++)
	{
		QueryOperand *op = (QueryOperand *) &items[i];

		if (op->type != QI_VAL)
			continue;

		memcpy(operands, GETOPERAND(in) + op->distance, op->length);
		operands[op->length] = '\0';
		op->distance = operands - GETOPERAND(out);
		operands += op->length + 1;
	}

	return out;
}
