/*-------------------------------------------------------------------------
 *
 * clauses.c
 *	  routines to manipulate qualification clauses
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/clauses.c,v 1.39 1999/07/15 23:03:17 momjian Exp $
 *
 * HISTORY
 *	  AUTHOR			DATE			MAJOR EVENT
 *	  Andrew Yu			Nov 3, 1994		clause.c and clauses.c combined
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_operator.h"
#include "nodes/plannodes.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"


#include "utils/lsyscache.h"

#include "optimizer/clauses.h"
#include "optimizer/internal.h"
#include "optimizer/var.h"


static bool fix_opid_walker(Node *node, void *context);


Expr *
make_clause(int type, Node *oper, List *args)
{
	if (type == AND_EXPR || type == OR_EXPR || type == NOT_EXPR ||
		type == OP_EXPR || type == FUNC_EXPR)
	{
		Expr	   *expr = makeNode(Expr);

		/*
		 * assume type checking already done and we don't need the type of
		 * the expr any more.
		 */
		expr->typeOid = InvalidOid;
		expr->opType = type;
		expr->oper = oper;		/* ignored for AND, OR, NOT */
		expr->args = args;
		return expr;
	}
	else
	{
		elog(ERROR, "make_clause: unsupported type %d", type);
		/* will this ever happen? translated from lispy C code - ay 10/94 */
		return (Expr *) args;
	}
}


/*****************************************************************************
 *		OPERATOR clause functions
 *****************************************************************************/


/*
 * is_opclause
 *
 * Returns t iff the clause is an operator clause:
 *				(op expr expr) or (op expr).
 *
 * [historical note: is_clause has the exact functionality and is used
 *		throughout the code. They're renamed to is_opclause for clarity.
 *												- ay 10/94.]
 */
bool
is_opclause(Node *clause)
{
	return (clause != NULL &&
	  nodeTag(clause) == T_Expr && ((Expr *) clause)->opType == OP_EXPR);
}

/*
 * make_opclause
 *	  Creates a clause given its operator left operand and right
 *	  operand (if it is non-null).
 *
 */
Expr *
make_opclause(Oper *op, Var *leftop, Var *rightop)
{
	Expr	   *expr = makeNode(Expr);

	expr->typeOid = InvalidOid; /* assume type checking done */
	expr->opType = OP_EXPR;
	expr->oper = (Node *) op;
	if (rightop)
		expr->args = lcons(leftop, lcons(rightop, NIL));
	else
		expr->args = lcons(leftop, NIL);
	return expr;
}

/*
 * get_leftop
 *
 * Returns the left operand of a clause of the form (op expr expr)
 *		or (op expr)
 * NB: it is assumed (for now) that all expr must be Var nodes
 */
Var *
get_leftop(Expr *clause)
{
	if (clause->args != NULL)
		return lfirst(clause->args);
	else
		return NULL;
}

/*
 * get_rightop
 *
 * Returns the right operand in a clause of the form (op expr expr).
 * NB: result will be NULL if applied to a unary op clause.
 */
Var *
get_rightop(Expr *clause)
{
	if (clause->args != NULL && lnext(clause->args) != NULL)
		return lfirst(lnext(clause->args));
	else
		return NULL;
}

/*****************************************************************************
 *		FUNC clause functions
 *****************************************************************************/

/*
 * is_funcclause
 *
 * Returns t iff the clause is a function clause: (func { expr }).
 *
 */
bool
is_funcclause(Node *clause)
{
	return (clause != NULL &&
			nodeTag(clause) == T_Expr &&
			((Expr *) clause)->opType == FUNC_EXPR);
}

/*
 * make_funcclause
 *
 * Creates a function clause given the FUNC node and the functional
 * arguments.
 *
 */
Expr *
make_funcclause(Func *func, List *funcargs)
{
	Expr	   *expr = makeNode(Expr);

	expr->typeOid = InvalidOid; /* assume type checking done */
	expr->opType = FUNC_EXPR;
	expr->oper = (Node *) func;
	expr->args = funcargs;
	return expr;
}

/*****************************************************************************
 *		OR clause functions
 *****************************************************************************/

/*
 * or_clause
 *
 * Returns t iff the clause is an 'or' clause: (OR { expr }).
 *
 */
bool
or_clause(Node *clause)
{
	return clause != NULL &&
	nodeTag(clause) == T_Expr &&
	((Expr *) clause)->opType == OR_EXPR;
}

/*
 * make_orclause
 *
 * Creates an 'or' clause given a list of its subclauses.
 *
 */
Expr *
make_orclause(List *orclauses)
{
	Expr	   *expr = makeNode(Expr);

	expr->typeOid = InvalidOid; /* assume type checking done */
	expr->opType = OR_EXPR;
	expr->oper = NULL;
	expr->args = orclauses;
	return expr;
}

/*****************************************************************************
 *		NOT clause functions
 *****************************************************************************/

/*
 * not_clause
 *
 * Returns t iff this is a 'not' clause: (NOT expr).
 *
 */
bool
not_clause(Node *clause)
{
	return (clause != NULL &&
			nodeTag(clause) == T_Expr &&
			((Expr *) clause)->opType == NOT_EXPR);
}

/*
 * make_notclause
 *
 * Create a 'not' clause given the expression to be negated.
 *
 */
Expr *
make_notclause(Expr *notclause)
{
	Expr	   *expr = makeNode(Expr);

	expr->typeOid = InvalidOid; /* assume type checking done */
	expr->opType = NOT_EXPR;
	expr->oper = NULL;
	expr->args = lcons(notclause, NIL);
	return expr;
}

/*
 * get_notclausearg
 *
 * Retrieve the clause within a 'not' clause
 *
 */
Expr *
get_notclausearg(Expr *notclause)
{
	return lfirst(notclause->args);
}

/*****************************************************************************
 *		AND clause functions
 *****************************************************************************/


/*
 * and_clause
 *
 * Returns t iff its argument is an 'and' clause: (AND { expr }).
 *
 */
bool
and_clause(Node *clause)
{
	return (clause != NULL &&
			nodeTag(clause) == T_Expr &&
			((Expr *) clause)->opType == AND_EXPR);
}

/*
 * make_andclause
 *
 * Create an 'and' clause given its arguments in a list.
 *
 */
Expr *
make_andclause(List *andclauses)
{
	Expr	   *expr = makeNode(Expr);

	expr->typeOid = InvalidOid; /* assume type checking done */
	expr->opType = AND_EXPR;
	expr->oper = NULL;
	expr->args = andclauses;
	return expr;
}


/*****************************************************************************
 *		CASE clause functions
 *****************************************************************************/


/*
 * case_clause
 *
 * Returns t iff its argument is a 'case' clause: (CASE { expr }).
 *
 */
bool
case_clause(Node *clause)
{
	return (clause != NULL &&
			nodeTag(clause) == T_CaseExpr);
}

/*****************************************************************************
 *																			 *
 *																			 *
 *																			 *
 *****************************************************************************/


/*
 * pull_constant_clauses
 *	  Scans through a list of qualifications and find those that
 *	  contain no variables.
 *
 * Returns a list of the constant clauses in constantQual and the remaining
 * quals as the return value.
 *
 */
List *
pull_constant_clauses(List *quals, List **constantQual)
{
	List	   *q;
	List	   *constqual = NIL;
	List	   *restqual = NIL;

	foreach(q, quals)
	{
		if (!contain_var_clause(lfirst(q)))
			constqual = lcons(lfirst(q), constqual);
		else
			restqual = lcons(lfirst(q), restqual);
	}
	freeList(quals);
	*constantQual = constqual;
	return restqual;
}

/*
 * clause_relids_vars
 *	  Retrieves relids and vars appearing within a clause.
 *	  Returns ((relid1 relid2 ... relidn) (var1 var2 ... varm)) where
 *	  vars appear in the clause  this is done by recursively searching
 *	  through the left and right operands of a clause.
 *
 * Returns the list of relids and vars.
 *
 */
void
clause_get_relids_vars(Node *clause, Relids *relids, List **vars)
{
	List	   *clvars = pull_var_clause(clause);
	List	   *var_list = NIL;
	List	   *varno_list = NIL;
	List	   *i;

	foreach(i, clvars)
	{
		Var		   *var = (Var *) lfirst(i);
		List	   *vi;

		Assert(var->varlevelsup == 0);
		if (!intMember(var->varno, varno_list))
			varno_list = lappendi(varno_list, var->varno);
		foreach(vi, var_list)
		{
			Var		   *in_list = (Var *) lfirst(vi);

			if (in_list->varno == var->varno &&
				in_list->varattno == var->varattno)
				break;
		}
		if (vi == NIL)
			var_list = lappend(var_list, var);
	}

	*relids = varno_list;
	*vars = var_list;
}

/*
 * NumRelids
 *		(formerly clause_relids)
 *
 * Returns the number of different relations referenced in 'clause'.
 */
int
NumRelids(Node *clause)
{
	List	   *vars = pull_var_clause(clause);
	List	   *var_list = NIL;
	List	   *i;

	foreach(i, vars)
	{
		Var		   *var = (Var *) lfirst(i);

		if (!intMember(var->varno, var_list))
			var_list = lconsi(var->varno, var_list);
	}

	return length(var_list);
}

/*
 * contains_not
 *
 * Returns t iff the clause is a 'not' clause or if any of the
 * subclauses within an 'or' clause contain 'not's.
 *
 * NOTE that only the top-level AND/OR structure is searched for NOTs;
 * we are not interested in buried substructure.
 */
bool
contains_not(Node *clause)
{
	if (single_node(clause))
		return false;

	if (not_clause(clause))
		return true;

	if (or_clause(clause) || and_clause(clause))
	{
		List	   *a;

		foreach(a, ((Expr *) clause)->args)
		{
			if (contains_not(lfirst(a)))
				return true;
		}
	}

	return false;
}

/*
 * is_joinable
 *
 * Returns t iff 'clause' is a valid join clause.
 *
 */
bool
is_joinable(Node *clause)
{
	Node	   *leftop,
			   *rightop;

	if (!is_opclause(clause))
		return false;

	leftop = (Node *) get_leftop((Expr *) clause);
	rightop = (Node *) get_rightop((Expr *) clause);

	if (!rightop)
		return false;			/* unary opclauses need not apply */

	/*
	 * One side of the clause (i.e. left or right operands) must either be
	 * a var node ...
	 */
	if (IsA(leftop, Var) ||IsA(rightop, Var))
		return true;

	/*
	 * ... or a func node.
	 */
	if (is_funcclause(leftop) || is_funcclause(rightop))
		return true;

	return false;
}

/*
 * qual_clause_p
 *
 * Returns t iff 'clause' is a valid qualification clause.
 *
 * For now we accept only "var op const" or "const op var".
 */
bool
qual_clause_p(Node *clause)
{
	Node	   *leftop,
			   *rightop;

	if (!is_opclause(clause))
		return false;

	leftop = (Node *) get_leftop((Expr *) clause);
	rightop = (Node *) get_rightop((Expr *) clause);

	if (!rightop)
		return false;			/* unary opclauses need not apply */

	/* How about Param-s ?	- vadim 02/03/98 */
	if (IsA(leftop, Var) &&IsA(rightop, Const))
		return true;
	if (IsA(rightop, Var) &&IsA(leftop, Const))
		return true;
	return false;
}

/*
 * fix_opid
 *	  Calculate opid field from opno for each Oper node in given tree.
 *
 * Returns nothing.
 */
void
fix_opid(Node *clause)
{
	/* This tree walk requires no special setup, so away we go... */
	fix_opid_walker(clause, NULL);
}

static bool
fix_opid_walker (Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (is_opclause(node))
		replace_opid((Oper *) ((Expr *) node)->oper);
	return expression_tree_walker(node, fix_opid_walker, context);
}

/*
 * fix_opids
 *	  Calculate the opid from the opno for all the clauses...
 *
 * Returns its argument.
 *
 * XXX This could and should be merged with fix_opid.
 *
 */
List *
fix_opids(List *clauses)
{
	fix_opid((Node *) clauses);
	return clauses;
}

/*
 * get_relattval
 *		For a non-join clause, returns a list consisting of the
 *				relid,
 *				attno,
 *				value of the CONST node (if any), and a
 *				flag indicating whether the value appears on the left or right
 *						of the operator and whether the value varied.
 *
 * OLD OBSOLETE COMMENT FOLLOWS:
 *		If 'clause' is not of the format (op var node) or (op node var),
 *		or if the var refers to a nested attribute, then -1's are returned for
 *		everything but the value  a blank string "" (pointer to \0) is
 *		returned for the value if it is unknown or null.
 * END OF OLD OBSOLETE COMMENT.
 * NEW COMMENT:
 * when defining rules one of the attributes of the operator can
 * be a Param node (which is supposed to be treated as a constant).
 * However as there is no value specified for a parameter until run time
 * this routine used to return "" as value, which caused 'compute_selec'
 * to bomb (because it was expecting a lisp integer and got back a lisp
 * string). Now the code returns a plain old good "lispInteger(0)".
 */
void
get_relattval(Node *clause,
			  int *relid,
			  AttrNumber *attno,
			  Datum *constval,
			  int *flag)
{
	Var		   *left,
			   *right;

	/* Careful; the passed clause might not be a binary operator at all */

	if (!is_opclause(clause))
		goto default_results;

	left = get_leftop((Expr *) clause);
	right = get_rightop((Expr *) clause);

	if (!right)
		goto default_results;

	if (IsA(left, Var) &&IsA(right, Const))
	{
		*relid = left->varno;
		*attno = left->varattno;
		*constval = ((Const *) right)->constvalue;
		*flag = (_SELEC_CONSTANT_RIGHT_ | _SELEC_IS_CONSTANT_);
	}
	else if (IsA(left, Var) &&IsA(right, Param))
	{
		*relid = left->varno;
		*attno = left->varattno;
		*constval = 0;
		*flag = (_SELEC_NOT_CONSTANT_);
	}
	else if (is_funcclause((Node *) left) && IsA(right, Const))
	{
		List	   *vars = pull_var_clause((Node *) left);

		*relid = ((Var *) lfirst(vars))->varno;
		*attno = InvalidAttrNumber;
		*constval = ((Const *) right)->constvalue;
		*flag = (_SELEC_CONSTANT_RIGHT_ | _SELEC_IS_CONSTANT_);
	}
	else if (IsA(right, Var) &&IsA(left, Const))
	{
		*relid = right->varno;
		*attno = right->varattno;
		*constval = ((Const *) left)->constvalue;
		*flag = (_SELEC_IS_CONSTANT_);
	}
	else if (IsA(right, Var) &&IsA(left, Param))
	{
		*relid = right->varno;
		*attno = right->varattno;
		*constval = 0;
		*flag = (_SELEC_NOT_CONSTANT_);
	}
	else if (is_funcclause((Node *) right) && IsA(left, Const))
	{
		List	   *vars = pull_var_clause((Node *) right);

		*relid = ((Var *) lfirst(vars))->varno;
		*attno = InvalidAttrNumber;
		*constval = ((Const *) left)->constvalue;
		*flag = (_SELEC_IS_CONSTANT_);
	}
	else
	{
		/* Duh, it's too complicated for me... */
default_results:
		*relid = _SELEC_VALUE_UNKNOWN_;
		*attno = _SELEC_VALUE_UNKNOWN_;
		*constval = 0;
		*flag = (_SELEC_NOT_CONSTANT_);
	}
}

/*
 * get_relsatts
 *
 * Returns a list
 *				( relid1 attno1 relid2 attno2 )
 *		for a joinclause.
 *
 * If the clause is not of the form (op var var) or if any of the vars
 * refer to nested attributes, then -1's are returned.
 *
 */
void
get_rels_atts(Node *clause,
			  int *relid1,
			  AttrNumber *attno1,
			  int *relid2,
			  AttrNumber *attno2)
{
	if (is_opclause(clause))
	{
		Var		   *left = get_leftop((Expr *) clause);
		Var		   *right = get_rightop((Expr *) clause);

		if (left && right)
		{
			bool		var_left = IsA(left, Var);
			bool		var_right = IsA(right, Var);
			bool		varexpr_left = (bool) ((IsA(left, Func) ||IsA(left, Oper)) &&
									  contain_var_clause((Node *) left));
			bool		varexpr_right = (bool) ((IsA(right, Func) ||IsA(right, Oper)) &&
									 contain_var_clause((Node *) right));

			if (var_left && var_right)
			{

				*relid1 = left->varno;
				*attno1 = left->varoattno;
				*relid2 = right->varno;
				*attno2 = right->varoattno;
				return;
			}
			if (var_left && varexpr_right)
			{

				*relid1 = left->varno;
				*attno1 = left->varoattno;
				*relid2 = _SELEC_VALUE_UNKNOWN_;
				*attno2 = _SELEC_VALUE_UNKNOWN_;
				return;
			}
			if (varexpr_left && var_right)
			{

				*relid1 = _SELEC_VALUE_UNKNOWN_;
				*attno1 = _SELEC_VALUE_UNKNOWN_;
				*relid2 = right->varno;
				*attno2 = right->varoattno;
				return;
			}
		}
	}

	*relid1 = _SELEC_VALUE_UNKNOWN_;
	*attno1 = _SELEC_VALUE_UNKNOWN_;
	*relid2 = _SELEC_VALUE_UNKNOWN_;
	*attno2 = _SELEC_VALUE_UNKNOWN_;
}

/*--------------------
 * CommuteClause: commute a binary operator clause
 *--------------------
 */
void
CommuteClause(Node *clause)
{
	Node	   *temp;
	Oper	   *commu;
	Form_pg_operator commuTup;
	HeapTuple	heapTup;

	if (!is_opclause(clause))
		elog(ERROR, "CommuteClause: applied to non-operator clause");

	heapTup = (HeapTuple)
		get_operator_tuple(get_commutator(((Oper *) ((Expr *) clause)->oper)->opno));

	if (heapTup == (HeapTuple) NULL)
		elog(ERROR, "CommuteClause: no commutator for operator %u",
			 ((Oper *) ((Expr *) clause)->oper)->opno);

	commuTup = (Form_pg_operator) GETSTRUCT(heapTup);

	commu = makeOper(heapTup->t_data->t_oid,
					 commuTup->oprcode,
					 commuTup->oprresult,
					 ((Oper *) ((Expr *) clause)->oper)->opsize,
					 NULL);

	/*
	 * re-form the clause in-place!
	 */
	((Expr *) clause)->oper = (Node *) commu;
	temp = lfirst(((Expr *) clause)->args);
	lfirst(((Expr *) clause)->args) = lsecond(((Expr *) clause)->args);
	lsecond(((Expr *) clause)->args) = temp;
}


/*--------------------
 * Standard expression-tree walking support
 *
 * We used to have near-duplicate code in many different routines that
 * understood how to recurse through an expression node tree.  That was
 * a pain to maintain, and we frequently had bugs due to some particular
 * routine neglecting to support a particular node type.  In most cases,
 * these routines only actually care about certain node types, and don't
 * care about other types except insofar as they have to recurse through
 * non-primitive node types.  Therefore, we now provide generic tree-walking
 * logic to consolidate the redundant "boilerplate" code.
 *
 * expression_tree_walker() is designed to support routines that traverse
 * a tree in a read-only fashion (although it will also work for routines
 * that modify nodes in-place but never add or delete nodes).  A walker
 * routine should look like this:
 *
 * bool my_walker (Node *node, my_struct *context)
 * {
 *		if (node == NULL)
 *			return false;
 *		// check for nodes that special work is required for, eg:
 *		if (IsA(node, Var))
 *		{
 *			... do special actions for Var nodes
 *		}
 *		else if (IsA(node, ...))
 *		{
 *			... do special actions for other node types
 *		}
 *		// for any node type not specially processed, do:
 *		return expression_tree_walker(node, my_walker, (void *) context);
 * }
 *
 * The "context" argument points to a struct that holds whatever context
 * information the walker routine needs (it can be used to return data
 * gathered by the walker, too).  This argument is not touched by
 * expression_tree_walker, but it is passed down to recursive sub-invocations
 * of my_walker.  The tree walk is started from a setup routine that
 * fills in the appropriate context struct, calls my_walker with the top-level
 * node of the tree, and then examines the results.
 *
 * The walker routine should return "false" to continue the tree walk, or
 * "true" to abort the walk and immediately return "true" to the top-level
 * caller.  This can be used to short-circuit the traversal if the walker
 * has found what it came for.  "false" is returned to the top-level caller
 * iff no invocation of the walker returned "true".
 *
 * The node types handled by expression_tree_walker include all those
 * normally found in target lists and qualifier clauses during the planning
 * stage.  In particular, it handles List nodes since a cnf-ified qual clause
 * will have List structure at the top level, and it handles TargetEntry nodes
 * so that a scan of a target list can be handled without additional code.
 * (But only the "expr" part of a TargetEntry is examined, unless the walker
 * chooses to process TargetEntry nodes specially.)
 *
 * expression_tree_walker will handle a SUBPLAN_EXPR node by recursing into
 * the args and slink->oper lists (which belong to the outer plan), but it
 * will *not* visit the inner plan, since that's typically what expression
 * tree walkers want.  A walker that wants to visit the subplan can force
 * appropriate behavior by recognizing subplan nodes and doing the right
 * thing.
 *
 * Bare SubLink nodes (without a SUBPLAN_EXPR) are handled by recursing into
 * the "lefthand" argument list only.  (A bare SubLink should be seen only if
 * the tree has not yet been processed by subselect.c.)  Again, this can be
 * overridden by the walker, but it seems to be the most useful default
 * behavior.
 *--------------------
 */

bool
expression_tree_walker(Node *node, bool (*walker) (), void *context)
{
	List	   *temp;

	/*
	 * The walker has already visited the current node,
	 * and so we need only recurse into any sub-nodes it has.
	 *
	 * We assume that the walker is not interested in List nodes per se,
	 * so when we expect a List we just recurse directly to self without
	 * bothering to call the walker.
	 */
	if (node == NULL)
		return false;
	switch (nodeTag(node))
	{
		case T_Ident:
		case T_Const:
		case T_Var:
		case T_Param:
			/* primitive node types with no subnodes */
			break;
		case T_Expr:
			{
				Expr   *expr = (Expr *) node;
				if (expr->opType == SUBPLAN_EXPR)
				{
					/* examine args list (params to be passed to subplan) */
					if (expression_tree_walker((Node *) expr->args,
											   walker, context))
						return true;
					/* examine oper list as well */
					if (expression_tree_walker(
						(Node *) ((SubPlan *) expr->oper)->sublink->oper,
						walker, context))
						return true;
					/* but not the subplan itself */
				}
				else
				{
					/* for other Expr node types, just examine args list */
					if (expression_tree_walker((Node *) expr->args,
											   walker, context))
						return true;
				}
			}
			break;
		case T_Aggref:
			return walker(((Aggref *) node)->target, context);
		case T_Iter:
			return walker(((Iter *) node)->iterexpr, context);
		case T_ArrayRef:
			{
				ArrayRef   *aref = (ArrayRef *) node;
				/* recurse directly for upper/lower array index lists */
				if (expression_tree_walker((Node *) aref->refupperindexpr,
										   walker, context))
					return true;
				if (expression_tree_walker((Node *) aref->reflowerindexpr,
										   walker, context))
					return true;
				/* walker must see the refexpr and refassgnexpr, however */
				if (walker(aref->refexpr, context))
					return true;
				if (walker(aref->refassgnexpr, context))
					return true;
			}
			break;
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;
				/* we assume walker doesn't care about CaseWhens, either */
				foreach(temp, caseexpr->args)
				{
					CaseWhen   *when = (CaseWhen *) lfirst(temp);
					Assert(IsA(when, CaseWhen));
					if (walker(when->expr, context))
						return true;
					if (walker(when->result, context))
						return true;
				}
				/* caseexpr->arg should be null, but we'll check it anyway */
				if (walker(caseexpr->arg, context))
					return true;
				if (walker(caseexpr->defresult, context))
					return true;
			}
			break;
		case T_SubLink:
			/* A "bare" SubLink (note we will not come here if we found
			 * a SUBPLAN_EXPR node above).  Examine the lefthand side,
			 * but not the oper list nor the subquery.
			 */
			return walker(((SubLink *) node)->lefthand, context);
		case T_List:
			foreach(temp, (List *) node)
			{
				if (walker((Node *) lfirst(temp), context))
					return true;
			}
			break;
		case T_TargetEntry:
			return walker(((TargetEntry *) node)->expr, context);
		default:
			elog(ERROR, "expression_tree_walker: Unexpected node type %d",
				 nodeTag(node));
			break;
	}
	return false;
}
