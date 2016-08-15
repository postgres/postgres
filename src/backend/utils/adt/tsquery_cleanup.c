/*-------------------------------------------------------------------------
 *
 * tsquery_cleanup.c
 *	 Cleanup query from NOT values and/or stopword
 *	 Utility functions to correct work.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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
 * To simplify walking on query tree and pushing down of phrase operator
 * we define some fake priority here: phrase operator has highest priority
 * of any other operators (and we believe here that OP_PHRASE is a highest
 * code of operations) and value node has ever highest priority.
 * Priority values of other operations don't matter until they are less than
 * phrase operator and value node.
 */
#define VALUE_PRIORITY			(OP_COUNT + 1)
#define NODE_PRIORITY(x) \
	( ((x)->valnode->qoperator.type == QI_OPR) ? \
		(x)->valnode->qoperator.oper : VALUE_PRIORITY )

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
 * Remove QI_VALSTOP (stopword nodes) from query tree.
 */
static NODE *
clean_fakeval_intree(NODE *node, char *result, int *adddistance)
{
	char		lresult = V_UNKNOWN,
				rresult = V_UNKNOWN;

	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	if (adddistance)
		*adddistance = 0;

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
		node->right = clean_fakeval_intree(node->right, &rresult, NULL);
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
		int			ndistance,
					ldistance = 0,
					rdistance = 0;

		ndistance = (node->valnode->qoperator.oper == OP_PHRASE) ?
			node->valnode->qoperator.distance :
			0;

		node->left = clean_fakeval_intree(node->left,
										  &lresult,
										  ndistance ? &ldistance : NULL);

		node->right = clean_fakeval_intree(node->right,
										   &rresult,
										   ndistance ? &rdistance : NULL);

		/*
		 * ndistance, ldistance and rdistance are greater than zero if their
		 * corresponding nodes are OP_PHRASE
		 */

		if (lresult == V_STOP && rresult == V_STOP)
		{
			if (adddistance && ndistance)
				*adddistance = ldistance + ndistance + rdistance;
			freetree(node);
			*result = V_STOP;
			return NULL;
		}
		else if (lresult == V_STOP)
		{
			res = node->right;

			/*
			 * propagate distance from current node to the right upper
			 * subtree.
			 */
			if (adddistance && ndistance)
				*adddistance = rdistance;
			pfree(node);
		}
		else if (rresult == V_STOP)
		{
			res = node->left;

			/*
			 * propagate distance from current node to the upper tree.
			 */
			if (adddistance && ndistance)
				*adddistance = ndistance + ldistance;
			pfree(node);
		}
		else if (ndistance)
		{
			node->valnode->qoperator.distance += ldistance;
			if (adddistance)
				*adddistance = 0;
		}
		else if (adddistance)
		{
			*adddistance = 0;
		}

		return res;
	}
	return node;
}

static NODE *
copyNODE(NODE *node)
{
	NODE	   *cnode = palloc(sizeof(NODE));

	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	cnode->valnode = palloc(sizeof(QueryItem));
	*(cnode->valnode) = *(node->valnode);

	if (node->valnode->type == QI_OPR)
	{
		cnode->right = copyNODE(node->right);
		if (node->valnode->qoperator.oper != OP_NOT)
			cnode->left = copyNODE(node->left);
	}

	return cnode;
}

static NODE *
makeNODE(int8 op, NODE *left, NODE *right)
{
	NODE	   *node = palloc(sizeof(NODE));

	/* zeroing allocation to prevent difference in unused bytes */
	node->valnode = palloc0(sizeof(QueryItem));

	node->valnode->qoperator.type = QI_OPR;
	node->valnode->qoperator.oper = op;

	node->left = left;
	node->right = right;

	return node;
}

/*
 * Move operation with high priority to the leaves. This guarantees
 * that the phrase operator will be near the bottom of the tree.
 * An idea behind is do not store position of lexemes during execution
 * of ordinary operations (AND, OR, NOT) because it could be expensive.
 * Actual transformation will be performed only on subtrees under the
 * <-> (<n>) operation since it's needed solely for the phrase operator.
 *
 * Rules:
 *	  a  <->  (b | c)	=>	(a <-> b)  |   (a <-> c)
 *	 (a | b)  <->	 c	   =>	(a <-> c)  |   (b <-> c)
 *	  a  <->	!b	   =>		a	  &  !(a <-> b)
 *	 !a  <->	 b	   =>		b	  &  !(a <-> b)
 *
 * Warnings for readers:
 *		  a <-> b	   !=	   b <-> a
 *
 *	  a <n> (b <n> c)	!=	 (a <n> b) <n> c since the phrase lengths are:
 *			 n					2n-1
 */
static NODE *
normalize_phrase_tree(NODE *node)
{
	/* there should be no stop words at this point */
	Assert(node->valnode->type != QI_VALSTOP);

	if (node->valnode->type == QI_VAL)
		return node;

	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	Assert(node->valnode->type == QI_OPR);

	if (node->valnode->qoperator.oper == OP_NOT)
	{
		NODE	   *orignode = node;

		/* eliminate NOT sequence */
		while (node->valnode->type == QI_OPR &&
		node->valnode->qoperator.oper == node->right->valnode->qoperator.oper)
		{
			node = node->right->right;
		}

		if (orignode != node)
			/* current node isn't checked yet */
			node = normalize_phrase_tree(node);
		else
			node->right = normalize_phrase_tree(node->right);
	}
	else if (node->valnode->qoperator.oper == OP_PHRASE)
	{
		int16		distance;
		NODE	   *X;

		node->left = normalize_phrase_tree(node->left);
		node->right = normalize_phrase_tree(node->right);

		/*
		 * if subtree contains only nodes with higher "priority" then we are
		 * done. See comment near NODE_PRIORITY()
		 */
		if (NODE_PRIORITY(node) <= NODE_PRIORITY(node->right) &&
			NODE_PRIORITY(node) <= NODE_PRIORITY(node->left))
			return node;

		/*
		 * We can't swap left-right and works only with left child because of
		 * a <-> b	!=	b <-> a
		 */

		distance = node->valnode->qoperator.distance;

		if (node->right->valnode->type == QI_OPR)
		{
			switch (node->right->valnode->qoperator.oper)
			{
				case OP_AND:
					/* a <-> (b & c)  =>  (a <-> b) & (a <-> c) */
					node = makeNODE(OP_AND,
									makeNODE(OP_PHRASE,
											 node->left,
											 node->right->left),
									makeNODE(OP_PHRASE,
											 copyNODE(node->left),
											 node->right->right));
					node->left->valnode->qoperator.distance =
						node->right->valnode->qoperator.distance = distance;
					break;
				case OP_OR:
					/* a <-> (b | c)  =>  (a <-> b) | (a <-> c) */
					node = makeNODE(OP_OR,
									makeNODE(OP_PHRASE,
											 node->left,
											 node->right->left),
									makeNODE(OP_PHRASE,
											 copyNODE(node->left),
											 node->right->right));
					node->left->valnode->qoperator.distance =
						node->right->valnode->qoperator.distance = distance;
					break;
				case OP_NOT:
					/* a <-> !b  =>  a & !(a <-> b) */
					X = node->right;
					node->right = node->right->right;
					X->right = node;
					node = makeNODE(OP_AND,
									copyNODE(node->left),
									X);
					break;
				case OP_PHRASE:
					/* no-op */
					break;
				default:
					elog(ERROR, "Wrong type of tsquery node: %d",
						 node->right->valnode->qoperator.oper);
			}
		}

		if (node->left->valnode->type == QI_OPR &&
			node->valnode->qoperator.oper == OP_PHRASE)
		{
			/*
			 * if the node is still OP_PHRASE, check the left subtree,
			 * otherwise the whole node will be transformed later.
			 */
			switch (node->left->valnode->qoperator.oper)
			{
				case OP_AND:
					/* (a & b) <-> c  =>  (a <-> c) & (b <-> c) */
					node = makeNODE(OP_AND,
									makeNODE(OP_PHRASE,
											 node->left->left,
											 node->right),
									makeNODE(OP_PHRASE,
											 node->left->right,
											 copyNODE(node->right)));
					node->left->valnode->qoperator.distance =
						node->right->valnode->qoperator.distance = distance;
					break;
				case OP_OR:
					/* (a | b) <-> c  =>  (a <-> c) | (b <-> c) */
					node = makeNODE(OP_OR,
									makeNODE(OP_PHRASE,
											 node->left->left,
											 node->right),
									makeNODE(OP_PHRASE,
											 node->left->right,
											 copyNODE(node->right)));
					node->left->valnode->qoperator.distance =
						node->right->valnode->qoperator.distance = distance;
					break;
				case OP_NOT:
					/* !a <-> b  =>  b & !(a <-> b) */
					X = node->left;
					node->left = node->left->right;
					X->right = node;
					node = makeNODE(OP_AND,
									X,
									copyNODE(node->right));
					break;
				case OP_PHRASE:
					/* no-op */
					break;
				default:
					elog(ERROR, "Wrong type of tsquery node: %d",
						 node->left->valnode->qoperator.oper);
			}
		}

		/* continue transformation */
		node = normalize_phrase_tree(node);
	}
	else	/* AND or OR */
	{
		node->left = normalize_phrase_tree(node->left);
		node->right = normalize_phrase_tree(node->right);
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

TSQuery
cleanup_fakeval_and_phrase(TSQuery in)
{
	int32		len,
				lenstr,
				commonlen,
				i;
	NODE	   *root;
	char		result = V_UNKNOWN;
	TSQuery		out;
	QueryItem  *items;
	char	   *operands;

	if (in->size == 0)
		return in;

	/* eliminate stop words */
	root = clean_fakeval_intree(maketree(GETQUERY(in)), &result, NULL);
	if (result != V_UNKNOWN)
	{
		ereport(NOTICE,
				(errmsg("text-search query contains only stop words or doesn't contain lexemes, ignored")));
		out = palloc(HDRSIZETQ);
		out->size = 0;
		SET_VARSIZE(out, HDRSIZETQ);
		return out;
	}

	/* push OP_PHRASE nodes down */
	root = normalize_phrase_tree(root);

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
