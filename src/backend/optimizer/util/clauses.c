/*-------------------------------------------------------------------------
 *
 * clauses.c--
 *	  routines to manipulate qualification clauses
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/clauses.c,v 1.11 1997/09/08 21:45:47 momjian Exp $
 *
 * HISTORY
 *	  AUTHOR			DATE			MAJOR EVENT
 *	  Andrew Yu			Nov 3, 1994		clause.c and clauses.c combined
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <catalog/pg_operator.h>
#include "nodes/primnodes.h"
#include "nodes/relation.h"
#include "nodes/parsenodes.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"

#include "catalog/pg_aggregate.h"

#include "utils/syscache.h"
#include "utils/lsyscache.h"

#include "optimizer/clauses.h"
#include "optimizer/internal.h"
#include "optimizer/var.h"

static bool agg_clause(Node *clause);


Expr	   *
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
		/* will this ever happen? translated from lispy C code - ay 10/94 */
		return ((Expr *) args);
	}
}


/*****************************************************************************
 *		OPERATOR clause functions
 *****************************************************************************/


/*
 * is_opclause--
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
	return
	(clause != NULL &&
	 nodeTag(clause) == T_Expr && ((Expr *) clause)->opType == OP_EXPR);
}

/*
 * make_opclause--
 *	  Creates a clause given its operator left operand and right
 *	  operand (if it is non-null).
 *
 */
Expr	   *
make_opclause(Oper *op, Var *leftop, Var *rightop)
{
	Expr	   *expr = makeNode(Expr);

	expr->typeOid = InvalidOid; /* assume type checking done */
	expr->opType = OP_EXPR;
	expr->oper = (Node *) op;
	expr->args = makeList(leftop, rightop, -1);
	return expr;
}

/*
 * get_leftop--
 *
 * Returns the left operand of a clause of the form (op expr expr)
 *		or (op expr)
 * NB: it is assumed (for now) that all expr must be Var nodes
 */
Var		   *
get_leftop(Expr *clause)
{
	if (clause->args != NULL)
		return (lfirst(clause->args));
	else
		return NULL;
}

/*
 * get_rightop
 *
 * Returns the right operand in a clause of the form (op expr expr).
 *
 */
Var		   *
get_rightop(Expr *clause)
{
	if (clause->args != NULL && lnext(clause->args) != NULL)
		return (lfirst(lnext(clause->args)));
	else
		return NULL;
}

/*****************************************************************************
 *		AGG clause functions
 *****************************************************************************/

static bool
agg_clause(Node *clause)
{
	return
	(clause != NULL && nodeTag(clause) == T_Aggreg);
}

/*****************************************************************************
 *		FUNC clause functions
 *****************************************************************************/

/*
 * is_funcclause--
 *
 * Returns t iff the clause is a function clause: (func { expr }).
 *
 */
bool
is_funcclause(Node *clause)
{
	return
	(clause != NULL &&
	 nodeTag(clause) == T_Expr && ((Expr *) clause)->opType == FUNC_EXPR);
}

/*
 * make_funcclause--
 *
 * Creates a function clause given the FUNC node and the functional
 * arguments.
 *
 */
Expr	   *
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
 * or_clause--
 *
 * Returns t iff the clause is an 'or' clause: (OR { expr }).
 *
 */
bool
or_clause(Node *clause)
{
	return
	(clause != NULL &&
	 nodeTag(clause) == T_Expr && ((Expr *) clause)->opType == OR_EXPR);
}

/*
 * make_orclause--
 *
 * Creates an 'or' clause given a list of its subclauses.
 *
 */
Expr	   *
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
 * not_clause--
 *
 * Returns t iff this is a 'not' clause: (NOT expr).
 *
 */
bool
not_clause(Node *clause)
{
	return
	(clause != NULL &&
	 nodeTag(clause) == T_Expr && ((Expr *) clause)->opType == NOT_EXPR);
}

/*
 * make_notclause--
 *
 * Create a 'not' clause given the expression to be negated.
 *
 */
Expr	   *
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
 * get_notclausearg--
 *
 * Retrieve the clause within a 'not' clause
 *
 */
Expr	   *
get_notclausearg(Expr *notclause)
{
	return (lfirst(notclause->args));
}

/*****************************************************************************
 *		AND clause functions
 *****************************************************************************/


/*
 * and_clause--
 *
 * Returns t iff its argument is an 'and' clause: (AND { expr }).
 *
 */
bool
and_clause(Node *clause)
{
	return
	(clause != NULL &&
	 nodeTag(clause) == T_Expr && ((Expr *) clause)->opType == AND_EXPR);
}

/*
 * make_andclause--
 *
 * Create an 'and' clause given its arguments in a list.
 *
 */
Expr	   *
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
 *																			 *
 *																			 *
 *																			 *
 *****************************************************************************/


/*
 * pull-constant-clauses--
 *	  Scans through a list of qualifications and find those that
 *	  contain no variables.
 *
 * Returns a list of the constant clauses in constantQual and the remaining
 * quals as the return value.
 *
 */
List	   *
pull_constant_clauses(List *quals, List **constantQual)
{
	List	   *q;
	List	   *constqual = NIL;
	List	   *restqual = NIL;

	foreach(q, quals)
	{
		if (!contain_var_clause(lfirst(q)))
		{
			constqual = lcons(lfirst(q), constqual);
		}
		else
		{
			restqual = lcons(lfirst(q), restqual);
		}
	}
	freeList(quals);
	*constantQual = constqual;
	return restqual;
}

/*
 * clause-relids-vars--
 *	  Retrieves relids and vars appearing within a clause.
 *	  Returns ((relid1 relid2 ... relidn) (var1 var2 ... varm)) where
 *	  vars appear in the clause  this is done by recursively searching
 *	  through the left and right operands of a clause.
 *
 * Returns the list of relids and vars.
 *
 * XXX take the nreverse's out later
 *
 */
void
clause_relids_vars(Node *clause, List **relids, List **vars)
{
	List	   *clvars = pull_var_clause(clause);
	List	   *var_list = NIL;
	List	   *varno_list = NIL;
	List	   *i = NIL;

	foreach(i, clvars)
	{
		Var		   *var = (Var *) lfirst(i);
		List	   *vi;

		if (!intMember(var->varno, varno_list))
		{
			varno_list = lappendi(varno_list, var->varno);
		}
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
	return;
}

/*
 * NumRelids--
 *		(formerly clause-relids)
 *
 * Returns the number of different relations referenced in 'clause'.
 */
int
NumRelids(Node *clause)
{
	List	   *vars = pull_var_clause(clause);
	List	   *i = NIL;
	List	   *var_list = NIL;

	foreach(i, vars)
	{
		Var		   *var = (Var *) lfirst(i);

		if (!intMember(var->varno, var_list))
		{
			var_list = lconsi(var->varno, var_list);
		}
	}

	return (length(var_list));
}

/*
 * contains-not--
 *
 * Returns t iff the clause is a 'not' clause or if any of the
 * subclauses within an 'or' clause contain 'not's.
 *
 */
bool
contains_not(Node *clause)
{
	if (single_node(clause))
		return (false);

	if (not_clause(clause))
		return (true);

	if (or_clause(clause))
	{
		List	   *a;

		foreach(a, ((Expr *) clause)->args)
		{
			if (contains_not(lfirst(a)))
				return (true);
		}
	}

	return (false);
}

/*
 * join-clause-p--
 *
 * Returns t iff 'clause' is a valid join clause.
 *
 */
bool
join_clause_p(Node *clause)
{
	Node	   *leftop,
			   *rightop;

	if (!is_opclause(clause))
		return false;

	leftop = (Node *) get_leftop((Expr *) clause);
	rightop = (Node *) get_rightop((Expr *) clause);

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
		return (true);

	return (false);
}

/*
 * qual-clause-p--
 *
 * Returns t iff 'clause' is a valid qualification clause.
 *
 */
bool
qual_clause_p(Node *clause)
{
	if (!is_opclause(clause))
		return false;

	if (IsA(get_leftop((Expr *) clause), Var) &&
		IsA(get_rightop((Expr *) clause), Const))
	{
		return (true);
	}
	else if (IsA(get_rightop((Expr *) clause), Var) &&
			 IsA(get_leftop((Expr *) clause), Const))
	{
		return (true);
	}
	return (false);
}

/*
 * fix-opid--
 *	  Calculate the opfid from the opno...
 *
 * Returns nothing.
 *
 */
void
fix_opid(Node *clause)
{
	if (clause == NULL || single_node(clause))
	{
		;
	}
	else if (or_clause(clause))
	{
		fix_opids(((Expr *) clause)->args);
	}
	else if (is_funcclause(clause))
	{
		fix_opids(((Expr *) clause)->args);
	}
	else if (IsA(clause, ArrayRef))
	{
		ArrayRef   *aref = (ArrayRef *) clause;

		fix_opids(aref->refupperindexpr);
		fix_opids(aref->reflowerindexpr);
		fix_opid(aref->refexpr);
		fix_opid(aref->refassgnexpr);
	}
	else if (not_clause(clause))
	{
		fix_opid((Node *) get_notclausearg((Expr *) clause));
	}
	else if (is_opclause(clause))
	{
		replace_opid((Oper *) ((Expr *) clause)->oper);
		fix_opid((Node *) get_leftop((Expr *) clause));
		fix_opid((Node *) get_rightop((Expr *) clause));
	}
	else if (agg_clause(clause))
	{
		fix_opid(((Aggreg *) clause)->target);
	}

}

/*
 * fix-opids--
 *	  Calculate the opfid from the opno for all the clauses...
 *
 * Returns its argument.
 *
 */
List	   *
fix_opids(List *clauses)
{
	List	   *clause;

	foreach(clause, clauses)
		fix_opid(lfirst(clause));

	return (clauses);
}

/*
 * get_relattval--
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
 * when defining rules one of the attibutes of the operator can
 * be a Param node (which is supposed to be treated as a constant).
 * However as there is no value specified for a parameter until run time
 * this routine used to return "" as value, which made 'compute_selec'
 * to bomb (because it was expecting a lisp integer and got back a lisp
 * string). Now the code returns a plain old good "lispInteger(0)".
 *
 */
void
get_relattval(Node *clause,
			  int *relid,
			  AttrNumber *attno,
			  Datum *constval,
			  int *flag)
{
	Var		   *left = get_leftop((Expr *) clause);
	Var		   *right = get_rightop((Expr *) clause);

	if (is_opclause(clause) && IsA(left, Var) &&
		IsA(right, Const))
	{

		if (right != NULL)
		{

			*relid = left->varno;
			*attno = left->varattno;
			*constval = ((Const *) right)->constvalue;
			*flag = (_SELEC_CONSTANT_RIGHT_ | _SELEC_IS_CONSTANT_);

		}
		else
		{

			*relid = left->varno;
			*attno = left->varattno;
			*constval = 0;
			*flag = (_SELEC_CONSTANT_RIGHT_ | _SELEC_NOT_CONSTANT_);

		}
#ifdef INDEXSCAN_PATCH
	}
	else if (is_opclause(clause) && IsA(left, Var) &&IsA(right, Param))
	{
		/* Function parameter used as index scan arg.  DZ - 27-8-1996 */
		*relid = left->varno;
		*attno = left->varattno;
		*constval = 0;
		*flag = (_SELEC_NOT_CONSTANT_);
#endif
	}
	else if (is_opclause(clause) &&
			 is_funcclause((Node *) left) &&
			 IsA(right, Const))
	{
		List	   *args = ((Expr *) left)->args;


		*relid = ((Var *) lfirst(args))->varno;
		*attno = InvalidAttrNumber;
		*constval = ((Const *) right)->constvalue;
		*flag = (_SELEC_CONSTANT_RIGHT_ | _SELEC_IS_CONSTANT_);

		/*
		 * XXX both of these func clause handling if's seem wrong to me.
		 * they assume that the first argument is the Var.	It could not
		 * handle (for example) f(1, emp.name).  I think I may have been
		 * assuming no constants in functional index scans when I
		 * implemented this originally (still currently true). -mer 10 Aug
		 * 1992
		 */
	}
	else if (is_opclause(clause) &&
			 is_funcclause((Node *) right) &&
			 IsA(left, Const))
	{
		List	   *args = ((Expr *) right)->args;

		*relid = ((Var *) lfirst(args))->varno;
		*attno = InvalidAttrNumber;
		*constval = ((Const *) left)->constvalue;
		*flag = (_SELEC_IS_CONSTANT_);

	}
	else if (is_opclause(clause) && IsA(right, Var) &&
			 IsA(left, Const))
	{
		if (left != NULL)
		{

			*relid = right->varno;
			*attno = right->varattno;
			*constval = ((Const *) left)->constvalue;
			*flag = (_SELEC_IS_CONSTANT_);
		}
		else
		{

			*relid = right->varno;
			*attno = right->varattno;
			*constval = 0;
			*flag = (_SELEC_NOT_CONSTANT_);
		}
#ifdef INDEXSCAN_PATCH
	}
	else if (is_opclause(clause) && IsA(right, Var) &&IsA(left, Param))
	{
		/* ...And here... - vadim 01/22/97 */
		*relid = right->varno;
		*attno = right->varattno;
		*constval = 0;
		*flag = (_SELEC_NOT_CONSTANT_);
#endif
	}
	else
	{

		/*
		 * One or more of the operands are expressions (e.g., oper
		 * clauses)
		 */
		*relid = _SELEC_VALUE_UNKNOWN_;
		*attno = _SELEC_VALUE_UNKNOWN_;
		*constval = 0;
		*flag = (_SELEC_NOT_CONSTANT_);
	}
}

/*
 * get_relsatts--
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
	Var		   *left = get_leftop((Expr *) clause);
	Var		   *right = get_rightop((Expr *) clause);
	bool		var_left = (IsA(left, Var));
	bool		var_right = (IsA(right, Var));
	bool		varexpr_left = (bool) ((IsA(left, Func) ||IsA(left, Oper)) &&
									   contain_var_clause((Node *) left));
	bool		varexpr_right = (bool) ((IsA(right, Func) ||IsA(right, Oper)) &&
									 contain_var_clause((Node *) right));

	if (is_opclause(clause))
	{
		if (var_left && var_right)
		{

			*relid1 = left->varno;
			*attno1 = left->varoattno;
			*relid2 = right->varno;
			*attno2 = right->varoattno;
			return;
		}
		else if (var_left && varexpr_right)
		{

			*relid1 = left->varno;
			*attno1 = left->varoattno;
			*relid2 = _SELEC_VALUE_UNKNOWN_;
			*attno2 = _SELEC_VALUE_UNKNOWN_;
			return;
		}
		else if (varexpr_left && var_right)
		{

			*relid1 = _SELEC_VALUE_UNKNOWN_;
			*attno1 = _SELEC_VALUE_UNKNOWN_;
			*relid2 = right->varno;
			*attno2 = right->varoattno;
			return;
		}
	}

	*relid1 = _SELEC_VALUE_UNKNOWN_;
	*attno1 = _SELEC_VALUE_UNKNOWN_;
	*relid2 = _SELEC_VALUE_UNKNOWN_;
	*attno2 = _SELEC_VALUE_UNKNOWN_;
	return;
}

void
CommuteClause(Node *clause)
{
	Node	   *temp;
	Oper	   *commu;
	OperatorTupleForm commuTup;
	HeapTuple	heapTup;

	if (!is_opclause(clause))
		return;

	heapTup = (HeapTuple)
		get_operator_tuple(get_commutator(((Oper *) ((Expr *) clause)->oper)->opno));

	if (heapTup == (HeapTuple) NULL)
		return;

	commuTup = (OperatorTupleForm) GETSTRUCT(heapTup);

	commu = makeOper(heapTup->t_oid,
					 InvalidOid,
					 commuTup->oprresult,
					 ((Oper *) ((Expr *) clause)->oper)->opsize,
					 NULL);

	/*
	 * reform the clause -> (operator func/var constant)
	 */
	((Expr *) clause)->oper = (Node *) commu;
	temp = lfirst(((Expr *) clause)->args);
	lfirst(((Expr *) clause)->args) = lsecond(((Expr *) clause)->args);
	lsecond(((Expr *) clause)->args) = temp;
}
