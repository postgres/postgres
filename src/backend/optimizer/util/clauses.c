/*-------------------------------------------------------------------------
 *
 * clauses.c
 *	  routines to manipulate qualification clauses
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/clauses.c,v 1.84 2001/03/27 17:12:34 tgl Exp $
 *
 * HISTORY
 *	  AUTHOR			DATE			MAJOR EVENT
 *	  Andrew Yu			Nov 3, 1994		clause.c and clauses.c combined
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* note that pg_type.h hardwires size of bool as 1 ... duplicate it */
#define MAKEBOOLCONST(val,isnull) \
	((Node *) makeConst(BOOLOID, 1, (Datum) (val), \
						(isnull), true, false, false))

static bool contain_agg_clause_walker(Node *node, void *context);
static bool pull_agg_clause_walker(Node *node, List **listptr);
static bool contain_subplans_walker(Node *node, void *context);
static bool pull_subplans_walker(Node *node, List **listptr);
static bool check_subplans_for_ungrouped_vars_walker(Node *node,
										 Query *context);
static bool contain_noncachable_functions_walker(Node *node, void *context);
static int	is_single_func(Node *node);
static Node *eval_const_expressions_mutator(Node *node, void *context);
static Expr *simplify_op_or_func(Expr *expr, List *args);


Expr *
make_clause(int type, Node *oper, List *args)
{
	Expr	   *expr = makeNode(Expr);

	switch (type)
	{
		case AND_EXPR:
		case OR_EXPR:
		case NOT_EXPR:
			expr->typeOid = BOOLOID;
			break;
		case OP_EXPR:
			expr->typeOid = ((Oper *) oper)->opresulttype;
			break;
		case FUNC_EXPR:
			expr->typeOid = ((Func *) oper)->functype;
			break;
		default:
			elog(ERROR, "make_clause: unsupported type %d", type);
			break;
	}
	expr->opType = type;
	expr->oper = oper;			/* ignored for AND, OR, NOT */
	expr->args = args;
	return expr;
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
			IsA(clause, Expr) &&
			((Expr *) clause)->opType == OP_EXPR);
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

	expr->typeOid = op->opresulttype;
	expr->opType = OP_EXPR;
	expr->oper = (Node *) op;
	if (rightop)
		expr->args = makeList2(leftop, rightop);
	else
		expr->args = makeList1(leftop);
	return expr;
}

/*
 * get_leftop
 *
 * Returns the left operand of a clause of the form (op expr expr)
 *		or (op expr)
 *
 * NB: for historical reasons, the result is declared Var *, even
 * though many callers can cope with results that are not Vars.
 * The result really ought to be declared Expr * or Node *.
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
			IsA(clause, Expr) &&
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

	expr->typeOid = func->functype;
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
	return (clause != NULL &&
			IsA(clause, Expr) &&
			((Expr *) clause)->opType == OR_EXPR);
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

	expr->typeOid = BOOLOID;
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
			IsA(clause, Expr) &&
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

	expr->typeOid = BOOLOID;
	expr->opType = NOT_EXPR;
	expr->oper = NULL;
	expr->args = makeList1(notclause);
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
			IsA(clause, Expr) &&
			((Expr *) clause)->opType == AND_EXPR);
}

/*
 * make_andclause
 *
 * Create an 'and' clause given its arguments in a list.
 */
Expr *
make_andclause(List *andclauses)
{
	Expr	   *expr = makeNode(Expr);

	expr->typeOid = BOOLOID;
	expr->opType = AND_EXPR;
	expr->oper = NULL;
	expr->args = andclauses;
	return expr;
}

/*
 * make_and_qual
 *
 * Variant of make_andclause for ANDing two qual conditions together.
 * Qual conditions have the property that a NULL nodetree is interpreted
 * as 'true'.
 */
Node *
make_and_qual(Node *qual1, Node *qual2)
{
	if (qual1 == NULL)
		return qual2;
	if (qual2 == NULL)
		return qual1;
	return (Node *) make_andclause(makeList2(qual1, qual2));
}

/*
 * Sometimes (such as in the result of canonicalize_qual or the input of
 * ExecQual), we use lists of expression nodes with implicit AND semantics.
 *
 * These functions convert between an AND-semantics expression list and the
 * ordinary representation of a boolean expression.
 *
 * Note that an empty list is considered equivalent to TRUE.
 */
Expr *
make_ands_explicit(List *andclauses)
{
	if (andclauses == NIL)
		return (Expr *) MAKEBOOLCONST(true, false);
	else if (lnext(andclauses) == NIL)
		return (Expr *) lfirst(andclauses);
	else
		return make_andclause(andclauses);
}

List *
make_ands_implicit(Expr *clause)
{

	/*
	 * NB: because the parser sets the qual field to NULL in a query that
	 * has no WHERE clause, we must consider a NULL input clause as TRUE,
	 * even though one might more reasonably think it FALSE.  Grumble. If
	 * this causes trouble, consider changing the parser's behavior.
	 */
	if (clause == NULL)
		return NIL;				/* NULL -> NIL list == TRUE */
	else if (and_clause((Node *) clause))
		return clause->args;
	else if (IsA(clause, Const) &&
			 !((Const *) clause)->constisnull &&
			 DatumGetBool(((Const *) clause)->constvalue))
		return NIL;				/* constant TRUE input -> NIL list */
	else
		return makeList1(clause);
}


/*****************************************************************************
 *		Aggregate-function clause manipulation
 *****************************************************************************/

/*
 * contain_agg_clause
 *	  Recursively search for Aggref nodes within a clause.
 *
 *	  Returns true if any aggregate found.
 */
bool
contain_agg_clause(Node *clause)
{
	return contain_agg_clause_walker(clause, NULL);
}

static bool
contain_agg_clause_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
		return true;			/* abort the tree traversal and return
								 * true */
	return expression_tree_walker(node, contain_agg_clause_walker, context);
}

/*
 * pull_agg_clause
 *	  Recursively pulls all Aggref nodes from an expression tree.
 *
 *	  Returns list of Aggref nodes found.  Note the nodes themselves are not
 *	  copied, only referenced.
 *
 *	  Note: this also checks for nested aggregates, which are an error.
 */
List *
pull_agg_clause(Node *clause)
{
	List	   *result = NIL;

	pull_agg_clause_walker(clause, &result);
	return result;
}

static bool
pull_agg_clause_walker(Node *node, List **listptr)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		*listptr = lappend(*listptr, node);

		/*
		 * Complain if the aggregate's argument contains any aggregates;
		 * nested agg functions are semantically nonsensical.
		 */
		if (contain_agg_clause(((Aggref *) node)->target))
			elog(ERROR, "Aggregate function calls may not be nested");

		/*
		 * Having checked that, we need not recurse into the argument.
		 */
		return false;
	}
	return expression_tree_walker(node, pull_agg_clause_walker,
								  (void *) listptr);
}


/*****************************************************************************
 *		Subplan clause manipulation
 *****************************************************************************/

/*
 * contain_subplans
 *	  Recursively search for subplan nodes within a clause.
 *
 * If we see a SubLink node, we will return TRUE.  This is only possible if
 * the expression tree hasn't yet been transformed by subselect.c.  We do not
 * know whether the node will produce a true subplan or just an initplan,
 * but we make the conservative assumption that it will be a subplan.
 *
 * Returns true if any subplan found.
 */
bool
contain_subplans(Node *clause)
{
	return contain_subplans_walker(clause, NULL);
}

static bool
contain_subplans_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (is_subplan(node) || IsA(node, SubLink))
		return true;			/* abort the tree traversal and return
								 * true */
	return expression_tree_walker(node, contain_subplans_walker, context);
}

/*
 * pull_subplans
 *	  Recursively pulls all subplans from an expression tree.
 *
 *	  Returns list of subplan nodes found.	Note the nodes themselves are not
 *	  copied, only referenced.
 */
List *
pull_subplans(Node *clause)
{
	List	   *result = NIL;

	pull_subplans_walker(clause, &result);
	return result;
}

static bool
pull_subplans_walker(Node *node, List **listptr)
{
	if (node == NULL)
		return false;
	if (is_subplan(node))
	{
		*listptr = lappend(*listptr, ((Expr *) node)->oper);
		/* fall through to check args to subplan */
	}
	return expression_tree_walker(node, pull_subplans_walker,
								  (void *) listptr);
}

/*
 * check_subplans_for_ungrouped_vars
 *		Check for subplans that are being passed ungrouped variables as
 *		parameters; generate an error message if any are found.
 *
 * In most contexts, ungrouped variables will be detected by the parser (see
 * parse_agg.c, check_ungrouped_columns()). But that routine currently does
 * not check subplans, because the necessary info is not computed until the
 * planner runs.  So we do it here, after we have processed sublinks into
 * subplans.  This ought to be cleaned up someday.
 *
 * 'clause' is the expression tree to be searched for subplans.
 * 'query' provides the GROUP BY list, the target list that the group clauses
 * refer to, and the range table.
 */
void
check_subplans_for_ungrouped_vars(Node *clause,
								  Query *query)
{

	/*
	 * No special setup needed; context for walker is just the Query
	 * pointer
	 */
	check_subplans_for_ungrouped_vars_walker(clause, query);
}

static bool
check_subplans_for_ungrouped_vars_walker(Node *node,
										 Query *context)
{
	if (node == NULL)
		return false;

	/*
	 * If we find an aggregate function, do not recurse into its
	 * arguments.  Subplans invoked within aggregate calls are allowed to
	 * receive ungrouped variables.
	 */
	if (IsA(node, Aggref))
		return false;

	/*
	 * We can ignore Vars other than in subplan args lists, since the
	 * parser already checked 'em.
	 */
	if (is_subplan(node))
	{

		/*
		 * The args list of the subplan node represents attributes from
		 * outside passed into the sublink.
		 */
		List	   *t;

		foreach(t, ((Expr *) node)->args)
		{
			Node	   *thisarg = lfirst(t);
			Var		   *var;
			bool		contained_in_group_clause;
			List	   *gl;

			/*
			 * We do not care about args that are not local variables;
			 * params or outer-level vars are not our responsibility to
			 * check.  (The outer-level query passing them to us needs to
			 * worry, instead.)
			 */
			if (!IsA(thisarg, Var))
				continue;
			var = (Var *) thisarg;
			if (var->varlevelsup > 0)
				continue;

			/*
			 * Else, see if it is a grouping column.
			 */
			contained_in_group_clause = false;
			foreach(gl, context->groupClause)
			{
				GroupClause *gcl = lfirst(gl);
				Node	   *groupexpr;

				groupexpr = get_sortgroupclause_expr(gcl,
													 context->targetList);
				if (equal(thisarg, groupexpr))
				{
					contained_in_group_clause = true;
					break;
				}
			}

			if (!contained_in_group_clause)
			{
				/* Found an ungrouped argument.  Complain. */
				RangeTblEntry *rte;
				char	   *attname;

				Assert(var->varno > 0 &&
					   (int) var->varno <= length(context->rtable));
				rte = rt_fetch(var->varno, context->rtable);
				attname = get_rte_attribute_name(rte, var->varattno);
				elog(ERROR, "Sub-SELECT uses un-GROUPed attribute %s.%s from outer query",
					 rte->eref->relname, attname);
			}
		}
	}
	return expression_tree_walker(node,
								check_subplans_for_ungrouped_vars_walker,
								  (void *) context);
}


/*****************************************************************************
 *		Check clauses for noncachable functions
 *****************************************************************************/

/*
 * contain_noncachable_functions
 *	  Recursively search for noncachable functions within a clause.
 *
 * Returns true if any noncachable function (or operator implemented by a
 * noncachable function) is found.	This test is needed so that we don't
 * mistakenly think that something like "WHERE random() < 0.5" can be treated
 * as a constant qualification.
 *
 * XXX we do not examine sublinks/subplans to see if they contain uses of
 * noncachable functions.  It's not real clear if that is correct or not...
 */
bool
contain_noncachable_functions(Node *clause)
{
	return contain_noncachable_functions_walker(clause, NULL);
}

static bool
contain_noncachable_functions_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Expr))
	{
		Expr	   *expr = (Expr *) node;

		switch (expr->opType)
		{
			case OP_EXPR:
				if (!op_iscachable(((Oper *) expr->oper)->opno))
					return true;
				break;
			case FUNC_EXPR:
				if (!func_iscachable(((Func *) expr->oper)->funcid))
					return true;
				break;
			default:
				break;
		}
	}
	return expression_tree_walker(node, contain_noncachable_functions_walker,
								  context);
}


/*****************************************************************************
 *		Check for "pseudo-constant" clauses
 *****************************************************************************/

/*
 * is_pseudo_constant_clause
 *	  Detect whether a clause is "constant", ie, it contains no variables
 *	  of the current query level and no uses of noncachable functions.
 *	  Such a clause is not necessarily a true constant: it can still contain
 *	  Params and outer-level Vars.	However, its value will be constant over
 *	  any one scan of the current query, so it can be used as an indexscan
 *	  key or (if a top-level qual) can be pushed up to become a gating qual.
 */
bool
is_pseudo_constant_clause(Node *clause)
{

	/*
	 * We could implement this check in one recursive scan.  But since the
	 * check for noncachable functions is both moderately expensive and
	 * unlikely to fail, it seems better to look for Vars first and only
	 * check for noncachable functions if we find no Vars.
	 */
	if (!contain_var_clause(clause) &&
		!contain_noncachable_functions(clause))
		return true;
	return false;
}

/*
 * pull_constant_clauses
 *		Scan through a list of qualifications and separate "constant" quals
 *		from those that are not.
 *
 * Returns a list of the pseudo-constant clauses in constantQual and the
 * remaining quals as the return value.
 */
List *
pull_constant_clauses(List *quals, List **constantQual)
{
	List	   *constqual = NIL;
	List	   *restqual = NIL;
	List	   *q;

	foreach(q, quals)
	{
		Node	   *qual = (Node *) lfirst(q);

		if (is_pseudo_constant_clause(qual))
			constqual = lappend(constqual, qual);
		else
			restqual = lappend(restqual, qual);
	}
	*constantQual = constqual;
	return restqual;
}


/*****************************************************************************
 *																			 *
 *		General clause-manipulating routines								 *
 *																			 *
 *****************************************************************************/

/*
 * clause_relids_vars
 *	  Retrieves distinct relids and vars appearing within a clause.
 *
 * '*relids' is set to an integer list of all distinct "varno"s appearing
 *		in Vars within the clause.
 * '*vars' is set to a list of all distinct Vars appearing within the clause.
 *		Var nodes are considered distinct if they have different varno
 *		or varattno values.  If there are several occurrences of the same
 *		varno/varattno, you get a randomly chosen one...
 *
 * Note that upper-level vars are ignored, since they normally will
 * become Params with respect to this query level.
 */
void
clause_get_relids_vars(Node *clause, Relids *relids, List **vars)
{
	List	   *clvars = pull_var_clause(clause, false);
	List	   *varno_list = NIL;
	List	   *var_list = NIL;
	List	   *i;

	foreach(i, clvars)
	{
		Var		   *var = (Var *) lfirst(i);
		List	   *vi;

		if (!intMember(var->varno, varno_list))
			varno_list = lconsi(var->varno, varno_list);
		foreach(vi, var_list)
		{
			Var		   *in_list = (Var *) lfirst(vi);

			if (in_list->varno == var->varno &&
				in_list->varattno == var->varattno)
				break;
		}
		if (vi == NIL)
			var_list = lcons(var, var_list);
	}
	freeList(clvars);

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
	List	   *varno_list = pull_varnos(clause);
	int			result = length(varno_list);

	freeList(varno_list);
	return result;
}

/*
 * get_relattval
 *		Extract information from a restriction or join clause for
 *		selectivity estimation.  The inputs are an expression
 *		and a relation number (which can be 0 if we don't care which
 *		relation is used; that'd normally be the case for restriction
 *		clauses, where the caller already knows that only one relation
 *		is referenced in the clause).  The routine checks that the
 *		expression is of the form (var op something) or (something op var)
 *		where the var is an attribute of the specified relation, or
 *		a function of a var of the specified relation.	If so, it
 *		returns the following info:
 *			the found relation number (same as targetrelid unless that is 0)
 *			the found var number (or InvalidAttrNumber if a function)
 *			if the "something" is a constant, the value of the constant
 *			flags indicating whether a constant was found, and on which side.
 *		Default values are returned if the expression is too complicated,
 *		specifically 0 for the relid and attno, 0 for the constant value.
 *
 *		Note that negative attno values are *not* invalid, but represent
 *		system attributes such as OID.	It's sufficient to check for relid=0
 *		to determine whether the routine succeeded.
 */
void
get_relattval(Node *clause,
			  int targetrelid,
			  int *relid,
			  AttrNumber *attno,
			  Datum *constval,
			  int *flag)
{
	Var		   *left,
			   *right,
			   *other;
	int			funcvarno;

	/* Careful; the passed clause might not be a binary operator at all */

	if (!is_opclause(clause))
		goto default_results;

	left = get_leftop((Expr *) clause);
	right = get_rightop((Expr *) clause);

	if (!right)
		goto default_results;

	/* Ignore any binary-compatible relabeling */

	if (IsA(left, RelabelType))
		left = (Var *) ((RelabelType *) left)->arg;
	if (IsA(right, RelabelType))
		right = (Var *) ((RelabelType *) right)->arg;

	/* First look for the var or func */

	if (IsA(left, Var) &&
		(targetrelid == 0 || targetrelid == left->varno))
	{
		*relid = left->varno;
		*attno = left->varattno;
		*flag = SEL_RIGHT;
	}
	else if (IsA(right, Var) &&
			 (targetrelid == 0 || targetrelid == right->varno))
	{
		*relid = right->varno;
		*attno = right->varattno;
		*flag = 0;
	}
	else if ((funcvarno = is_single_func((Node *) left)) != 0 &&
			 (targetrelid == 0 || targetrelid == funcvarno))
	{
		*relid = funcvarno;
		*attno = InvalidAttrNumber;
		*flag = SEL_RIGHT;
	}
	else if ((funcvarno = is_single_func((Node *) right)) != 0 &&
			 (targetrelid == 0 || targetrelid == funcvarno))
	{
		*relid = funcvarno;
		*attno = InvalidAttrNumber;
		*flag = 0;
	}
	else
	{
		/* Duh, it's too complicated for me... */
default_results:
		*relid = 0;
		*attno = 0;
		*constval = 0;
		*flag = 0;
		return;
	}

	/* OK, we identified the var or func; now look at the other side */

	other = (*flag == 0) ? left : right;

	if (IsA(other, Const) &&
		!((Const *) other)->constisnull)
	{
		*constval = ((Const *) other)->constvalue;
		*flag |= SEL_CONSTANT;
	}
	else
		*constval = 0;
}

/*
 * is_single_func
 *	 If the given expression is a function of a single relation,
 *	 return the relation number; else return 0
 */
static int
is_single_func(Node *node)
{
	if (is_funcclause(node))
	{
		List	   *varnos = pull_varnos(node);

		if (length(varnos) == 1)
		{
			int			funcvarno = lfirsti(varnos);

			freeList(varnos);
			return funcvarno;
		}
		freeList(varnos);
	}
	return 0;
}

/*
 * get_rels_atts
 *
 * Returns the info
 *				( relid1 attno1 relid2 attno2 )
 *		for a joinclause.
 *
 * If the clause is not of the form (var op var) or if any of the vars
 * refer to nested attributes, then zeroes are returned.
 */
void
get_rels_atts(Node *clause,
			  int *relid1,
			  AttrNumber *attno1,
			  int *relid2,
			  AttrNumber *attno2)
{
	/* set default values */
	*relid1 = 0;
	*attno1 = 0;
	*relid2 = 0;
	*attno2 = 0;

	if (is_opclause(clause))
	{
		Var		   *left = get_leftop((Expr *) clause);
		Var		   *right = get_rightop((Expr *) clause);

		if (left && right)
		{
			int			funcvarno;

			/* Ignore any binary-compatible relabeling */
			if (IsA(left, RelabelType))
				left = (Var *) ((RelabelType *) left)->arg;
			if (IsA(right, RelabelType))
				right = (Var *) ((RelabelType *) right)->arg;

			if (IsA(left, Var))
			{
				*relid1 = left->varno;
				*attno1 = left->varattno;
			}
			else if ((funcvarno = is_single_func((Node *) left)) != 0)
			{
				*relid1 = funcvarno;
				*attno1 = InvalidAttrNumber;
			}

			if (IsA(right, Var))
			{
				*relid2 = right->varno;
				*attno2 = right->varattno;
			}
			else if ((funcvarno = is_single_func((Node *) right)) != 0)
			{
				*relid2 = funcvarno;
				*attno2 = InvalidAttrNumber;
			}
		}
	}
}

/*--------------------
 * CommuteClause: commute a binary operator clause
 *
 * XXX the clause is destructively modified!
 *--------------------
 */
void
CommuteClause(Expr *clause)
{
	Oid			opoid;
	HeapTuple	optup;
	Form_pg_operator commuTup;
	Oper	   *commu;
	Node	   *temp;

	if (!is_opclause((Node *) clause) ||
		length(clause->args) != 2)
		elog(ERROR, "CommuteClause: applied to non-binary-operator clause");

	opoid = ((Oper *) clause->oper)->opno;

	optup = SearchSysCache(OPEROID,
						   ObjectIdGetDatum(get_commutator(opoid)),
						   0, 0, 0);
	if (!HeapTupleIsValid(optup))
		elog(ERROR, "CommuteClause: no commutator for operator %u", opoid);

	commuTup = (Form_pg_operator) GETSTRUCT(optup);

	commu = makeOper(optup->t_data->t_oid,
					 commuTup->oprcode,
					 commuTup->oprresult);

	ReleaseSysCache(optup);

	/*
	 * re-form the clause in-place!
	 */
	clause->oper = (Node *) commu;
	temp = lfirst(clause->args);
	lfirst(clause->args) = lsecond(clause->args);
	lsecond(clause->args) = temp;
}


/*--------------------
 * eval_const_expressions
 *
 * Reduce any recognizably constant subexpressions of the given
 * expression tree, for example "2 + 2" => "4".  More interestingly,
 * we can reduce certain boolean expressions even when they contain
 * non-constant subexpressions: "x OR true" => "true" no matter what
 * the subexpression x is.	(XXX We assume that no such subexpression
 * will have important side-effects, which is not necessarily a good
 * assumption in the presence of user-defined functions; do we need a
 * pg_proc flag that prevents discarding the execution of a function?)
 *
 * We do understand that certain functions may deliver non-constant
 * results even with constant inputs, "nextval()" being the classic
 * example.  Functions that are not marked "proiscachable" in pg_proc
 * will not be pre-evaluated here, although we will reduce their
 * arguments as far as possible.  Functions that are the arguments
 * of Iter nodes are also not evaluated.
 *
 * We assume that the tree has already been type-checked and contains
 * only operators and functions that are reasonable to try to execute.
 *
 * This routine should be invoked before converting sublinks to subplans
 * (subselect.c's SS_process_sublinks()).  The converted form contains
 * bogus "Const" nodes that are actually placeholders where the executor
 * will insert values from the inner plan, and obviously we mustn't try
 * to reduce the expression as though these were really constants.
 * As a safeguard, if we happen to find an already-converted SubPlan node,
 * we will return it unchanged rather than recursing into it.
 *--------------------
 */
Node *
eval_const_expressions(Node *node)
{
	/* no context or special setup needed, so away we go... */
	return eval_const_expressions_mutator(node, NULL);
}

static Node *
eval_const_expressions_mutator(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Expr))
	{
		Expr	   *expr = (Expr *) node;
		List	   *args;
		Const	   *const_input;
		Expr	   *newexpr;

		/*
		 * Reduce constants in the Expr's arguments.  We know args is
		 * either NIL or a List node, so we can call
		 * expression_tree_mutator directly rather than recursing to self.
		 */
		args = (List *) expression_tree_mutator((Node *) expr->args,
										  eval_const_expressions_mutator,
												(void *) context);

		switch (expr->opType)
		{
			case OP_EXPR:
			case FUNC_EXPR:

				/*
				 * Code for op/func case is pretty bulky, so split it out
				 * as a separate function.
				 */
				newexpr = simplify_op_or_func(expr, args);
				if (newexpr)	/* successfully simplified it */
					return (Node *) newexpr;

				/*
				 * else fall out to build new Expr node with simplified
				 * args
				 */
				break;
			case OR_EXPR:
				{

					/*----------
					 * OR arguments are handled as follows:
					 *	non constant: keep
					 *	FALSE: drop (does not affect result)
					 *	TRUE: force result to TRUE
					 *	NULL: keep only one
					 * We keep one NULL input because ExecEvalOr returns NULL
					 * when no input is TRUE and at least one is NULL.
					 *----------
					 */
					List	   *newargs = NIL;
					List	   *arg;
					bool		haveNull = false;
					bool		forceTrue = false;

					foreach(arg, args)
					{
						if (!IsA(lfirst(arg), Const))
						{
							newargs = lappend(newargs, lfirst(arg));
							continue;
						}
						const_input = (Const *) lfirst(arg);
						if (const_input->constisnull)
							haveNull = true;
						else if (DatumGetBool(const_input->constvalue))
							forceTrue = true;
						/* otherwise, we can drop the constant-false input */
					}

					/*
					 * We could return TRUE before falling out of the
					 * loop, but this coding method will be easier to
					 * adapt if we ever add a notion of non-removable
					 * functions. We'd need to check all the inputs for
					 * non-removability.
					 */
					if (forceTrue)
						return MAKEBOOLCONST(true, false);
					if (haveNull)
						newargs = lappend(newargs, MAKEBOOLCONST(false, true));
					/* If all the inputs are FALSE, result is FALSE */
					if (newargs == NIL)
						return MAKEBOOLCONST(false, false);
					/* If only one nonconst-or-NULL input, it's the result */
					if (lnext(newargs) == NIL)
						return (Node *) lfirst(newargs);
					/* Else we still need an OR node */
					return (Node *) make_orclause(newargs);
				}
			case AND_EXPR:
				{

					/*----------
					 * AND arguments are handled as follows:
					 *	non constant: keep
					 *	TRUE: drop (does not affect result)
					 *	FALSE: force result to FALSE
					 *	NULL: keep only one
					 * We keep one NULL input because ExecEvalAnd returns NULL
					 * when no input is FALSE and at least one is NULL.
					 *----------
					 */
					List	   *newargs = NIL;
					List	   *arg;
					bool		haveNull = false;
					bool		forceFalse = false;

					foreach(arg, args)
					{
						if (!IsA(lfirst(arg), Const))
						{
							newargs = lappend(newargs, lfirst(arg));
							continue;
						}
						const_input = (Const *) lfirst(arg);
						if (const_input->constisnull)
							haveNull = true;
						else if (!DatumGetBool(const_input->constvalue))
							forceFalse = true;
						/* otherwise, we can drop the constant-true input */
					}

					/*
					 * We could return FALSE before falling out of the
					 * loop, but this coding method will be easier to
					 * adapt if we ever add a notion of non-removable
					 * functions. We'd need to check all the inputs for
					 * non-removability.
					 */
					if (forceFalse)
						return MAKEBOOLCONST(false, false);
					if (haveNull)
						newargs = lappend(newargs, MAKEBOOLCONST(false, true));
					/* If all the inputs are TRUE, result is TRUE */
					if (newargs == NIL)
						return MAKEBOOLCONST(true, false);
					/* If only one nonconst-or-NULL input, it's the result */
					if (lnext(newargs) == NIL)
						return (Node *) lfirst(newargs);
					/* Else we still need an AND node */
					return (Node *) make_andclause(newargs);
				}
			case NOT_EXPR:
				Assert(length(args) == 1);
				if (!IsA(lfirst(args), Const))
					break;
				const_input = (Const *) lfirst(args);
				/* NOT NULL => NULL */
				if (const_input->constisnull)
					return MAKEBOOLCONST(false, true);
				/* otherwise pretty easy */
				return MAKEBOOLCONST(!DatumGetBool(const_input->constvalue),
									 false);
			case SUBPLAN_EXPR:

				/*
				 * Safety measure per notes at head of this routine:
				 * return a SubPlan unchanged.	Too late to do anything
				 * with it.  The arglist simplification above was wasted
				 * work (the list probably only contains Var nodes
				 * anyway).
				 */
				return (Node *) expr;
			default:
				elog(ERROR, "eval_const_expressions: unexpected opType %d",
					 (int) expr->opType);
				break;
		}

		/*
		 * If we break out of the above switch on opType, then the
		 * expression cannot be simplified any further, so build and
		 * return a replacement Expr node using the possibly-simplified
		 * arguments and the original oper node. Can't use make_clause()
		 * here because we want to be sure the typeOid field is
		 * preserved...
		 */
		newexpr = makeNode(Expr);
		newexpr->typeOid = expr->typeOid;
		newexpr->opType = expr->opType;
		newexpr->oper = expr->oper;
		newexpr->args = args;
		return (Node *) newexpr;
	}
	if (IsA(node, RelabelType))
	{

		/*
		 * If we can simplify the input to a constant, then we don't need
		 * the RelabelType node anymore: just change the type field of the
		 * Const node.	Otherwise, must copy the RelabelType node.
		 */
		RelabelType *relabel = (RelabelType *) node;
		Node	   *arg;

		arg = eval_const_expressions_mutator(relabel->arg, context);

		/*
		 * If we find stacked RelabelTypes (eg, from foo :: int :: oid) we
		 * can discard all but the top one.
		 */
		while (arg && IsA(arg, RelabelType))
			arg = ((RelabelType *) arg)->arg;

		if (arg && IsA(arg, Const))
		{
			Const	   *con = (Const *) arg;

			con->consttype = relabel->resulttype;

			/*
			 * relabel's resulttypmod is discarded, which is OK for now;
			 * if the type actually needs a runtime length coercion then
			 * there should be a function call to do it just above this
			 * node.
			 */
			return (Node *) con;
		}
		else
		{
			RelabelType *newrelabel = makeNode(RelabelType);

			newrelabel->arg = arg;
			newrelabel->resulttype = relabel->resulttype;
			newrelabel->resulttypmod = relabel->resulttypmod;
			return (Node *) newrelabel;
		}
	}
	if (IsA(node, CaseExpr))
	{

		/*----------
		 * CASE expressions can be simplified if there are constant
		 * condition clauses:
		 *		FALSE (or NULL): drop the alternative
		 *		TRUE: drop all remaining alternatives
		 * If the first non-FALSE alternative is a constant TRUE, we can
		 * simplify the entire CASE to that alternative's expression.
		 * If there are no non-FALSE alternatives, we simplify the entire
		 * CASE to the default result (ELSE result).
		 *----------
		 */
		CaseExpr   *caseexpr = (CaseExpr *) node;
		CaseExpr   *newcase;
		List	   *newargs = NIL;
		Node	   *defresult;
		Const	   *const_input;
		List	   *arg;

		foreach(arg, caseexpr->args)
		{
			/* Simplify this alternative's condition and result */
			CaseWhen   *casewhen = (CaseWhen *)
			expression_tree_mutator((Node *) lfirst(arg),
									eval_const_expressions_mutator,
									(void *) context);

			Assert(IsA(casewhen, CaseWhen));
			if (casewhen->expr == NULL ||
				!IsA(casewhen->expr, Const))
			{
				newargs = lappend(newargs, casewhen);
				continue;
			}
			const_input = (Const *) casewhen->expr;
			if (const_input->constisnull ||
				!DatumGetBool(const_input->constvalue))
				continue;		/* drop alternative with FALSE condition */

			/*
			 * Found a TRUE condition.	If it's the first (un-dropped)
			 * alternative, the CASE reduces to just this alternative.
			 */
			if (newargs == NIL)
				return casewhen->result;

			/*
			 * Otherwise, add it to the list, and drop all the rest.
			 */
			newargs = lappend(newargs, casewhen);
			break;
		}

		/* Simplify the default result */
		defresult = eval_const_expressions_mutator(caseexpr->defresult,
												   context);

		/*
		 * If no non-FALSE alternatives, CASE reduces to the default
		 * result
		 */
		if (newargs == NIL)
			return defresult;
		/* Otherwise we need a new CASE node */
		newcase = makeNode(CaseExpr);
		newcase->casetype = caseexpr->casetype;
		newcase->arg = NULL;
		newcase->args = newargs;
		newcase->defresult = defresult;
		return (Node *) newcase;
	}
	if (IsA(node, Iter))
	{

		/*
		 * The argument of an Iter is normally a function call. We must
		 * not try to eliminate the function, but we can try to simplify
		 * its arguments.  If, by chance, the arg is NOT a function then
		 * we go ahead and try to simplify it (by falling into
		 * expression_tree_mutator). Is that the right thing?
		 */
		Iter	   *iter = (Iter *) node;

		if (is_funcclause(iter->iterexpr))
		{
			Expr	   *func = (Expr *) iter->iterexpr;
			Expr	   *newfunc;
			Iter	   *newiter;

			newfunc = makeNode(Expr);
			newfunc->typeOid = func->typeOid;
			newfunc->opType = func->opType;
			newfunc->oper = func->oper;
			newfunc->args = (List *)
				expression_tree_mutator((Node *) func->args,
										eval_const_expressions_mutator,
										(void *) context);
			newiter = makeNode(Iter);
			newiter->iterexpr = (Node *) newfunc;
			newiter->itertype = iter->itertype;
			return (Node *) newiter;
		}
	}

	/*
	 * For any node type not handled above, we recurse using
	 * expression_tree_mutator, which will copy the node unchanged but try
	 * to simplify its arguments (if any) using this routine. For example:
	 * we cannot eliminate an ArrayRef node, but we might be able to
	 * simplify constant expressions in its subscripts.
	 */
	return expression_tree_mutator(node, eval_const_expressions_mutator,
								   (void *) context);
}

/*
 * Subroutine for eval_const_expressions: try to evaluate an op or func
 *
 * Inputs are the op or func Expr node, and the pre-simplified argument list.
 * Returns a simplified expression if successful, or NULL if cannot
 * simplify the op/func.
 *
 * XXX Possible future improvement: if the func is SQL-language, and its
 * definition is simply "SELECT expression", we could parse and substitute
 * the expression here.  This would avoid much runtime overhead, and perhaps
 * expose opportunities for constant-folding within the expression even if
 * not all the func's input args are constants.  It'd be appropriate to do
 * that here, not in the parser, since we wouldn't want it to happen until
 * after rule substitution/rewriting.
 */
static Expr *
simplify_op_or_func(Expr *expr, List *args)
{
	List	   *arg;
	Oid			funcid;
	Oid			result_typeid;
	HeapTuple	func_tuple;
	Form_pg_proc funcform;
	bool		proiscachable;
	bool		proisstrict;
	bool		proretset;
	int16		resultTypLen;
	bool		resultTypByVal;
	Expr	   *newexpr;
	ExprContext *econtext;
	Datum		const_val;
	bool		has_nonconst_input = false;
	bool		has_null_input = false;
	bool		const_is_null;

	/*
	 * Check for constant inputs and especially constant-NULL inputs.
	 */
	foreach(arg, args)
	{
		if (IsA(lfirst(arg), Const))
			has_null_input |= ((Const *) lfirst(arg))->constisnull;
		else
			has_nonconst_input = true;
	}

	/*
	 * If the function is strict and has a constant-NULL input, it will
	 * never be called at all, so we can replace the call by a NULL
	 * constant even if there are other inputs that aren't constant.
	 * Otherwise, we can only simplify if all inputs are constants. We can
	 * skip the function lookup if neither case applies.
	 */
	if (has_nonconst_input && !has_null_input)
		return NULL;

	/*
	 * Get the function procedure's OID and look to see whether it is
	 * marked proiscachable.
	 *
	 * XXX would it be better to take the result type from the pg_proc tuple,
	 * rather than the Oper or Func node?
	 */
	if (expr->opType == OP_EXPR)
	{
		Oper	   *oper = (Oper *) expr->oper;

		replace_opid(oper);		/* OK to scribble on input to this extent */
		funcid = oper->opid;
		result_typeid = oper->opresulttype;
	}
	else
	{
		Func	   *func = (Func *) expr->oper;

		funcid = func->funcid;
		result_typeid = func->functype;
	}

	/*
	 * we could use func_iscachable() here, but we need several fields out
	 * of the func tuple, so might as well just look it up once.
	 */
	func_tuple = SearchSysCache(PROCOID,
								ObjectIdGetDatum(funcid),
								0, 0, 0);
	if (!HeapTupleIsValid(func_tuple))
		elog(ERROR, "Function OID %u does not exist", funcid);
	funcform = (Form_pg_proc) GETSTRUCT(func_tuple);
	proiscachable = funcform->proiscachable;
	proisstrict = funcform->proisstrict;
	proretset = funcform->proretset;
	ReleaseSysCache(func_tuple);

	if (!proiscachable)
		return NULL;

	/*
	 * Also check to make sure it doesn't return a set.
	 */
	if (proretset)
		return NULL;

	/*
	 * Now that we know if the function is strict, we can finish the
	 * checks for simplifiable inputs that we started above.
	 */
	if (proisstrict && has_null_input)
	{

		/*
		 * It's strict and has NULL input, so must produce NULL output.
		 * Return a NULL constant of the right type.
		 */
		return (Expr *) makeNullConst(result_typeid);
	}

	/*
	 * Otherwise, can simplify only if all inputs are constants. (For a
	 * non-strict function, constant NULL inputs are treated the same as
	 * constant non-NULL inputs.)
	 */
	if (has_nonconst_input)
		return NULL;

	/*
	 * OK, looks like we can simplify this operator/function.
	 *
	 * We use the executor's routine ExecEvalExpr() to avoid duplication of
	 * code and ensure we get the same result as the executor would get.
	 *
	 * Build a new Expr node containing the already-simplified arguments. The
	 * only other setup needed here is the replace_opid() that we already
	 * did for the OP_EXPR case.
	 */
	newexpr = makeNode(Expr);
	newexpr->typeOid = expr->typeOid;
	newexpr->opType = expr->opType;
	newexpr->oper = expr->oper;
	newexpr->args = args;

	/* Get info needed about result datatype */
	get_typlenbyval(result_typeid, &resultTypLen, &resultTypByVal);

	/*
	 * It is OK to pass a dummy econtext because none of the
	 * ExecEvalExpr() code used in this situation will use econtext.  That
	 * might seem fortuitous, but it's not so unreasonable --- a constant
	 * expression does not depend on context, by definition, n'est ce pas?
	 */
	econtext = MakeExprContext(NULL, CurrentMemoryContext);

	const_val = ExecEvalExprSwitchContext((Node *) newexpr, econtext,
										  &const_is_null, NULL);

	/* Must copy result out of sub-context used by expression eval */
	const_val = datumCopy(const_val, resultTypByVal, resultTypLen);

	FreeExprContext(econtext);
	pfree(newexpr);

	/*
	 * Make the constant result node.
	 */
	return (Expr *) makeConst(result_typeid, resultTypLen,
							  const_val, const_is_null,
							  resultTypByVal, false, false);
}


/*
 * Standard expression-tree walking support
 *
 * We used to have near-duplicate code in many different routines that
 * understood how to recurse through an expression node tree.  That was
 * a pain to maintain, and we frequently had bugs due to some particular
 * routine neglecting to support a particular node type.  In most cases,
 * these routines only actually care about certain node types, and don't
 * care about other types except insofar as they have to recurse through
 * non-primitive node types.  Therefore, we now provide generic tree-walking
 * logic to consolidate the redundant "boilerplate" code.  There are
 * two versions: expression_tree_walker() and expression_tree_mutator().
 */

/*--------------------
 * expression_tree_walker() is designed to support routines that traverse
 * a tree in a read-only fashion (although it will also work for routines
 * that modify nodes in-place but never add/delete/replace nodes).
 * A walker routine should look like this:
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
 * information the walker routine needs --- it can be used to return data
 * gathered by the walker, too.  This argument is not touched by
 * expression_tree_walker, but it is passed down to recursive sub-invocations
 * of my_walker.  The tree walk is started from a setup routine that
 * fills in the appropriate context struct, calls my_walker with the top-level
 * node of the tree, and then examines the results.
 *
 * The walker routine should return "false" to continue the tree walk, or
 * "true" to abort the walk and immediately return "true" to the top-level
 * caller.	This can be used to short-circuit the traversal if the walker
 * has found what it came for.	"false" is returned to the top-level caller
 * iff no invocation of the walker returned "true".
 *
 * The node types handled by expression_tree_walker include all those
 * normally found in target lists and qualifier clauses during the planning
 * stage.  In particular, it handles List nodes since a cnf-ified qual clause
 * will have List structure at the top level, and it handles TargetEntry nodes
 * so that a scan of a target list can be handled without additional code.
 * (But only the "expr" part of a TargetEntry is examined, unless the walker
 * chooses to process TargetEntry nodes specially.)  Also, RangeTblRef,
 * FromExpr, JoinExpr, and SetOperationStmt nodes are handled, so that query
 * jointrees and setOperation trees can be processed without additional code.
 *
 * expression_tree_walker will handle SubLink and SubPlan nodes by recursing
 * normally into the "lefthand" arguments (which belong to the outer plan).
 * It will also call the walker on the sub-Query node; however, when
 * expression_tree_walker itself is called on a Query node, it does nothing
 * and returns "false".  The net effect is that unless the walker does
 * something special at a Query node, sub-selects will not be visited
 * during an expression tree walk.	This is exactly the behavior wanted
 * in many cases --- and for those walkers that do want to recurse into
 * sub-selects, special behavior is typically needed anyway at the entry
 * to a sub-select (such as incrementing a depth counter).	A walker that
 * wants to examine sub-selects should include code along the lines of:
 *
 *		if (IsA(node, Query))
 *		{
 *			adjust context for subquery;
 *			result = query_tree_walker((Query *) node, my_walker, context,
 *									   true); // to visit subquery RTEs too
 *			restore context if needed;
 *			return result;
 *		}
 *
 * query_tree_walker is a convenience routine (see below) that calls the
 * walker on all the expression subtrees of the given Query node.
 *
 * NOTE: currently, because make_subplan() clears the subselect link in
 * a SubLink node, it is not actually possible to recurse into subselects
 * of an already-planned expression tree.  This is OK for current uses,
 * but ought to be cleaned up when we redesign querytree processing.
 *--------------------
 */

bool
expression_tree_walker(Node *node,
					   bool (*walker) (),
					   void *context)
{
	List	   *temp;

	/*
	 * The walker has already visited the current node, and so we need
	 * only recurse into any sub-nodes it has.
	 *
	 * We assume that the walker is not interested in List nodes per se, so
	 * when we expect a List we just recurse directly to self without
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
		case T_RangeTblRef:
			/* primitive node types with no subnodes */
			break;
		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;

				if (expr->opType == SUBPLAN_EXPR)
				{
					/* recurse to the SubLink node (skipping SubPlan!) */
					if (walker((Node *) ((SubPlan *) expr->oper)->sublink,
							   context))
						return true;
				}
				/* for all Expr node types, examine args list */
				if (expression_tree_walker((Node *) expr->args,
										   walker, context))
					return true;
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
		case T_FieldSelect:
			return walker(((FieldSelect *) node)->arg, context);
		case T_RelabelType:
			return walker(((RelabelType *) node)->arg, context);
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
			{
				SubLink    *sublink = (SubLink *) node;

				/*
				 * If the SubLink has already been processed by
				 * subselect.c, it will have lefthand=NIL, and we need to
				 * scan the oper list.	Otherwise we only need to look at
				 * the lefthand list (the incomplete Oper nodes in the
				 * oper list are deemed uninteresting, perhaps even
				 * confusing).
				 */
				if (sublink->lefthand)
				{
					if (walker((Node *) sublink->lefthand, context))
						return true;
				}
				else
				{
					if (walker((Node *) sublink->oper, context))
						return true;
				}

				/*
				 * Also invoke the walker on the sublink's Query node, so
				 * it can recurse into the sub-query if it wants to.
				 */
				return walker(sublink->subselect, context);
			}
			break;
		case T_Query:
			/* Do nothing with a sub-Query, per discussion above */
			break;
		case T_List:
			foreach(temp, (List *) node)
			{
				if (walker((Node *) lfirst(temp), context))
					return true;
			}
			break;
		case T_TargetEntry:
			return walker(((TargetEntry *) node)->expr, context);
		case T_FromExpr:
			{
				FromExpr   *from = (FromExpr *) node;

				if (walker(from->fromlist, context))
					return true;
				if (walker(from->quals, context))
					return true;
			}
			break;
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) node;

				if (walker(join->larg, context))
					return true;
				if (walker(join->rarg, context))
					return true;
				if (walker(join->quals, context))
					return true;
				if (walker((Node *) join->colvars, context))
					return true;

				/*
				 * alias clause, using list, colnames list are deemed
				 * uninteresting.
				 */
			}
			break;
		case T_SetOperationStmt:
			{
				SetOperationStmt *setop = (SetOperationStmt *) node;

				if (walker(setop->larg, context))
					return true;
				if (walker(setop->rarg, context))
					return true;
			}
			break;
		default:
			elog(ERROR, "expression_tree_walker: Unexpected node type %d",
				 nodeTag(node));
			break;
	}
	return false;
}

/*
 * query_tree_walker --- initiate a walk of a Query's expressions
 *
 * This routine exists just to reduce the number of places that need to know
 * where all the expression subtrees of a Query are.  Note it can be used
 * for starting a walk at top level of a Query regardless of whether the
 * walker intends to descend into subqueries.  It is also useful for
 * descending into subqueries within a walker.
 *
 * If visitQueryRTEs is true, the walker will also be called on sub-Query
 * nodes present in subquery rangetable entries of the given Query.  This
 * is optional since some callers handle those sub-queries separately,
 * or don't really want to see subqueries anyway.
 */
bool
query_tree_walker(Query *query,
				  bool (*walker) (),
				  void *context,
				  bool visitQueryRTEs)
{
	Assert(query != NULL && IsA(query, Query));

	if (walker((Node *) query->targetList, context))
		return true;
	if (walker((Node *) query->jointree, context))
		return true;
	if (walker(query->setOperations, context))
		return true;
	if (walker(query->havingQual, context))
		return true;
	if (visitQueryRTEs)
	{
		List	   *rt;

		foreach(rt, query->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

			if (rte->subquery)
				if (walker(rte->subquery, context))
					return true;
		}
	}
	return false;
}


/*--------------------
 * expression_tree_mutator() is designed to support routines that make a
 * modified copy of an expression tree, with some nodes being added,
 * removed, or replaced by new subtrees.  The original tree is (normally)
 * not changed.  Each recursion level is responsible for returning a copy of
 * (or appropriately modified substitute for) the subtree it is handed.
 * A mutator routine should look like this:
 *
 * Node * my_mutator (Node *node, my_struct *context)
 * {
 *		if (node == NULL)
 *			return NULL;
 *		// check for nodes that special work is required for, eg:
 *		if (IsA(node, Var))
 *		{
 *			... create and return modified copy of Var node
 *		}
 *		else if (IsA(node, ...))
 *		{
 *			... do special transformations of other node types
 *		}
 *		// for any node type not specially processed, do:
 *		return expression_tree_mutator(node, my_mutator, (void *) context);
 * }
 *
 * The "context" argument points to a struct that holds whatever context
 * information the mutator routine needs --- it can be used to return extra
 * data gathered by the mutator, too.  This argument is not touched by
 * expression_tree_mutator, but it is passed down to recursive sub-invocations
 * of my_mutator.  The tree walk is started from a setup routine that
 * fills in the appropriate context struct, calls my_mutator with the
 * top-level node of the tree, and does any required post-processing.
 *
 * Each level of recursion must return an appropriately modified Node.
 * If expression_tree_mutator() is called, it will make an exact copy
 * of the given Node, but invoke my_mutator() to copy the sub-node(s)
 * of that Node.  In this way, my_mutator() has full control over the
 * copying process but need not directly deal with expression trees
 * that it has no interest in.
 *
 * Just as for expression_tree_walker, the node types handled by
 * expression_tree_mutator include all those normally found in target lists
 * and qualifier clauses during the planning stage.
 *
 * expression_tree_mutator will handle a SUBPLAN_EXPR node by recursing into
 * the args and slink->oper lists (which belong to the outer plan), but it
 * will simply copy the link to the inner plan, since that's typically what
 * expression tree mutators want.  A mutator that wants to modify the subplan
 * can force appropriate behavior by recognizing subplan expression nodes
 * and doing the right thing.
 *
 * Bare SubLink nodes (without a SUBPLAN_EXPR) are handled by recursing into
 * the "lefthand" argument list only.  (A bare SubLink should be seen only if
 * the tree has not yet been processed by subselect.c.)  Again, this can be
 * overridden by the mutator, but it seems to be the most useful default
 * behavior.
 *--------------------
 */

Node *
expression_tree_mutator(Node *node,
						Node *(*mutator) (),
						void *context)
{

	/*
	 * The mutator has already decided not to modify the current node, but
	 * we must call the mutator for any sub-nodes.
	 */

#define FLATCOPY(newnode, node, nodetype)  \
	( (newnode) = makeNode(nodetype), \
	  memcpy((newnode), (node), sizeof(nodetype)) )

#define CHECKFLATCOPY(newnode, node, nodetype)	\
	( AssertMacro(IsA((node), nodetype)), \
	  (newnode) = makeNode(nodetype), \
	  memcpy((newnode), (node), sizeof(nodetype)) )

#define MUTATE(newfield, oldfield, fieldtype)  \
		( (newfield) = (fieldtype) mutator((Node *) (oldfield), context) )

	if (node == NULL)
		return NULL;
	switch (nodeTag(node))
	{
		case T_Ident:
		case T_Const:
		case T_Var:
		case T_Param:
		case T_RangeTblRef:
			/* primitive node types with no subnodes */
			return (Node *) copyObject(node);
		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;
				Expr	   *newnode;

				FLATCOPY(newnode, expr, Expr);

				if (expr->opType == SUBPLAN_EXPR)
				{
					SubLink    *oldsublink = ((SubPlan *) expr->oper)->sublink;
					SubPlan    *newsubplan;

					/* flat-copy the oper node, which is a SubPlan */
					CHECKFLATCOPY(newsubplan, expr->oper, SubPlan);
					newnode->oper = (Node *) newsubplan;
					/* likewise its SubLink node */
					CHECKFLATCOPY(newsubplan->sublink, oldsublink, SubLink);

					/*
					 * transform args list (params to be passed to
					 * subplan)
					 */
					MUTATE(newnode->args, expr->args, List *);
					/* transform sublink's oper list as well */
					MUTATE(newsubplan->sublink->oper, oldsublink->oper, List *);

					/*
					 * but not the subplan itself, which is referenced
					 * as-is
					 */
				}
				else
				{

					/*
					 * for other Expr node types, just transform args
					 * list, linking to original oper node (OK?)
					 */
					MUTATE(newnode->args, expr->args, List *);
				}
				return (Node *) newnode;
			}
			break;
		case T_Aggref:
			{
				Aggref	   *aggref = (Aggref *) node;
				Aggref	   *newnode;

				FLATCOPY(newnode, aggref, Aggref);
				MUTATE(newnode->target, aggref->target, Node *);
				return (Node *) newnode;
			}
			break;
		case T_Iter:
			{
				Iter	   *iter = (Iter *) node;
				Iter	   *newnode;

				FLATCOPY(newnode, iter, Iter);
				MUTATE(newnode->iterexpr, iter->iterexpr, Node *);
				return (Node *) newnode;
			}
			break;
		case T_ArrayRef:
			{
				ArrayRef   *arrayref = (ArrayRef *) node;
				ArrayRef   *newnode;

				FLATCOPY(newnode, arrayref, ArrayRef);
				MUTATE(newnode->refupperindexpr, arrayref->refupperindexpr,
					   List *);
				MUTATE(newnode->reflowerindexpr, arrayref->reflowerindexpr,
					   List *);
				MUTATE(newnode->refexpr, arrayref->refexpr,
					   Node *);
				MUTATE(newnode->refassgnexpr, arrayref->refassgnexpr,
					   Node *);
				return (Node *) newnode;
			}
			break;
		case T_FieldSelect:
			{
				FieldSelect *fselect = (FieldSelect *) node;
				FieldSelect *newnode;

				FLATCOPY(newnode, fselect, FieldSelect);
				MUTATE(newnode->arg, fselect->arg, Node *);
				return (Node *) newnode;
			}
			break;
		case T_RelabelType:
			{
				RelabelType *relabel = (RelabelType *) node;
				RelabelType *newnode;

				FLATCOPY(newnode, relabel, RelabelType);
				MUTATE(newnode->arg, relabel->arg, Node *);
				return (Node *) newnode;
			}
			break;
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;
				CaseExpr   *newnode;

				FLATCOPY(newnode, caseexpr, CaseExpr);
				MUTATE(newnode->args, caseexpr->args, List *);
				/* caseexpr->arg should be null, but we'll check it anyway */
				MUTATE(newnode->arg, caseexpr->arg, Node *);
				MUTATE(newnode->defresult, caseexpr->defresult, Node *);
				return (Node *) newnode;
			}
			break;
		case T_CaseWhen:
			{
				CaseWhen   *casewhen = (CaseWhen *) node;
				CaseWhen   *newnode;

				FLATCOPY(newnode, casewhen, CaseWhen);
				MUTATE(newnode->expr, casewhen->expr, Node *);
				MUTATE(newnode->result, casewhen->result, Node *);
				return (Node *) newnode;
			}
			break;
		case T_SubLink:
			{

				/*
				 * A "bare" SubLink (note we will not come here if we
				 * found a SUBPLAN_EXPR node above it).  Transform the
				 * lefthand side, but not the oper list nor the subquery.
				 */
				SubLink    *sublink = (SubLink *) node;
				SubLink    *newnode;

				FLATCOPY(newnode, sublink, SubLink);
				MUTATE(newnode->lefthand, sublink->lefthand, List *);
				return (Node *) newnode;
			}
			break;
		case T_List:
			{

				/*
				 * We assume the mutator isn't interested in the list
				 * nodes per se, so just invoke it on each list element.
				 * NOTE: this would fail badly on a list with integer
				 * elements!
				 */
				List	   *resultlist = NIL;
				List	   *temp;

				foreach(temp, (List *) node)
				{
					resultlist = lappend(resultlist,
										 mutator((Node *) lfirst(temp),
												 context));
				}
				return (Node *) resultlist;
			}
			break;
		case T_TargetEntry:
			{

				/*
				 * We mutate the expression, but not the resdom, by
				 * default.
				 */
				TargetEntry *targetentry = (TargetEntry *) node;
				TargetEntry *newnode;

				FLATCOPY(newnode, targetentry, TargetEntry);
				MUTATE(newnode->expr, targetentry->expr, Node *);
				return (Node *) newnode;
			}
			break;
		case T_FromExpr:
			{
				FromExpr   *from = (FromExpr *) node;
				FromExpr   *newnode;

				FLATCOPY(newnode, from, FromExpr);
				MUTATE(newnode->fromlist, from->fromlist, List *);
				MUTATE(newnode->quals, from->quals, Node *);
				return (Node *) newnode;
			}
			break;
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) node;
				JoinExpr   *newnode;

				FLATCOPY(newnode, join, JoinExpr);
				MUTATE(newnode->larg, join->larg, Node *);
				MUTATE(newnode->rarg, join->rarg, Node *);
				MUTATE(newnode->quals, join->quals, Node *);
				MUTATE(newnode->colvars, join->colvars, List *);
				/* We do not mutate alias, using, or colnames by default */
				return (Node *) newnode;
			}
			break;
		case T_SetOperationStmt:
			{
				SetOperationStmt *setop = (SetOperationStmt *) node;
				SetOperationStmt *newnode;

				FLATCOPY(newnode, setop, SetOperationStmt);
				MUTATE(newnode->larg, setop->larg, Node *);
				MUTATE(newnode->rarg, setop->rarg, Node *);
				return (Node *) newnode;
			}
			break;
		default:
			elog(ERROR, "expression_tree_mutator: Unexpected node type %d",
				 nodeTag(node));
			break;
	}
	/* can't get here, but keep compiler happy */
	return NULL;
}


/*
 * query_tree_mutator --- initiate modification of a Query's expressions
 *
 * This routine exists just to reduce the number of places that need to know
 * where all the expression subtrees of a Query are.  Note it can be used
 * for starting a walk at top level of a Query regardless of whether the
 * mutator intends to descend into subqueries.	It is also useful for
 * descending into subqueries within a mutator.
 *
 * The specified Query node is modified-in-place; do a FLATCOPY() beforehand
 * if you don't want to change the original.  All substructure is safely
 * copied, however.
 *
 * If visitQueryRTEs is true, the mutator will also be called on sub-Query
 * nodes present in subquery rangetable entries of the given Query.  This
 * is optional since some callers handle those sub-queries separately,
 * or don't really want to see subqueries anyway.
 */
void
query_tree_mutator(Query *query,
				   Node *(*mutator) (),
				   void *context,
				   bool visitQueryRTEs)
{
	Assert(query != NULL && IsA(query, Query));

	MUTATE(query->targetList, query->targetList, List *);
	MUTATE(query->jointree, query->jointree, FromExpr *);
	MUTATE(query->setOperations, query->setOperations, Node *);
	MUTATE(query->havingQual, query->havingQual, Node *);
	if (visitQueryRTEs)
	{
		List	   *newrt = NIL;
		List	   *rt;

		foreach(rt, query->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

			if (rte->subquery)
			{
				RangeTblEntry *newrte;

				FLATCOPY(newrte, rte, RangeTblEntry);
				CHECKFLATCOPY(newrte->subquery, rte->subquery, Query);
				MUTATE(newrte->subquery, newrte->subquery, Query *);
				rte = newrte;
			}
			newrt = lappend(newrt, rte);
		}
		query->rtable = newrt;
	}
}
