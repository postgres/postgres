/*-------------------------------------------------------------------------
 *
 * tsquery_cleanup.c
 *	 Cleanup query from NOT values and/or stopword
 *	 Utility functions to correct work.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsquery_cleanup.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "tsearch/ts_utils.h"
#include "miscadmin.h"

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

		Assert(node->valnode->qoperator.oper == OP_AND);

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


#ifdef V_UNKNOWN				/* exists in Windows headers */
#undef V_UNKNOWN
#endif
#ifdef V_FALSE					/* exists in Solaris headers */
#undef V_FALSE
#endif

/*
 * output values for result output parameter of clean_fakeval_intree
 */
#define V_UNKNOWN	0			/* the expression can't be evaluated
								 * statically */
#define V_TRUE		1			/* the expression is always true (not
								 * implemented) */
#define V_FALSE		2			/* the expression is always false (not
								 * implemented) */
#define V_STOP		3			/* the expression is a stop word */

/*
 * Clean query tree from values which is always in
 * text (stopword)
 */
static NODE *
clean_fakeval_intree(NODE *node, char *result)
{
	char		lresult = V_UNKNOWN,
				rresult = V_UNKNOWN;

	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	if (node->valnode->type == QI_VAL)
		return node;
	else if (node->valnode->type == QI_VALSTOP)
	{
		pfree(node);
		*result = V_STOP;
		return NULL;
	}

	Assert(node->valnode->type == QI_OPR);

	if (node->valnode->qoperator.oper == OP_NOT)
	{
		node->right = clean_fakeval_intree(node->right, &rresult);
		if (!node->right)
		{
			*result = V_STOP;
			freetree(node);
			return NULL;
		}
	}
	else
	{
		NODE	   *res = node;

		node->left = clean_fakeval_intree(node->left, &lresult);
		node->right = clean_fakeval_intree(node->right, &rresult);

		if (lresult == V_STOP && rresult == V_STOP)
		{
			freetree(node);
			*result = V_STOP;
			return NULL;
		}
		else if (lresult == V_STOP)
		{
			res = node->right;
			pfree(node);
		}
		else if (rresult == V_STOP)
		{
			res = node->left;
			pfree(node);
		}
		return res;
	}
	return node;
}

QueryItem *
clean_fakeval(QueryItem *ptr, int *len)
{
	NODE	   *root = maketree(ptr);
	char		result = V_UNKNOWN;
	NODE	   *resroot;

	resroot = clean_fakeval_intree(root, &result);
	if (result != V_UNKNOWN)
	{
		ereport(NOTICE,
				(errmsg("text-search query contains only stop words or doesn't contain lexemes, ignored")));
		*len = 0;
		return NULL;
	}

	return plaintree(resroot, len);
}
