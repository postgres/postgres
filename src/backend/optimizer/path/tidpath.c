/*-------------------------------------------------------------------------
 *
 * tidpath.c
 *	  Routines to determine which TID conditions are usable for scanning
 *	  a given relation, and create TidPaths accordingly.
 *
 * What we are looking for here is WHERE conditions of the form
 * "CTID = pseudoconstant", which can be implemented by just fetching
 * the tuple directly via heap_fetch().  We can also handle OR conditions
 * if each OR arm contains such a condition; in particular this allows
 *		WHERE ctid IN (tid1, tid2, ...)
 *
 * There is currently no special support for joins involving CTID; in
 * particular nothing corresponding to best_inner_indexscan().	Since it's
 * not very useful to store TIDs of one table in another table, there
 * doesn't seem to be enough use-case to justify adding a lot of code
 * for that.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/path/tidpath.c,v 1.25 2005/10/15 02:49:20 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "optimizer/clauses.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "parser/parse_expr.h"


static Node *IsTidEqualClause(int varno, OpExpr *node);
static List *TidQualFromExpr(int varno, Node *expr);
static List *TidQualFromRestrictinfo(int varno, List *restrictinfo);


/*
 * Check to see if an opclause is of the form
 *		CTID = pseudoconstant
 * or
 *		pseudoconstant = CTID
 *
 * If it is, return the pseudoconstant subnode; if not, return NULL.
 *
 * We check that the CTID Var belongs to relation "varno".	That is probably
 * redundant considering this is only applied to restriction clauses, but
 * let's be safe.
 */
static Node *
IsTidEqualClause(int varno, OpExpr *node)
{
	Node	   *arg1,
			   *arg2,
			   *other;
	Var		   *var;

	/* Operator must be tideq */
	if (node->opno != TIDEqualOperator)
		return NULL;
	if (list_length(node->args) != 2)
		return NULL;
	arg1 = linitial(node->args);
	arg2 = lsecond(node->args);

	/* Look for CTID as either argument */
	other = NULL;
	if (arg1 && IsA(arg1, Var))
	{
		var = (Var *) arg1;
		if (var->varattno == SelfItemPointerAttributeNumber &&
			var->vartype == TIDOID &&
			var->varno == varno &&
			var->varlevelsup == 0)
			other = arg2;
	}
	if (!other && arg2 && IsA(arg2, Var))
	{
		var = (Var *) arg2;
		if (var->varattno == SelfItemPointerAttributeNumber &&
			var->vartype == TIDOID &&
			var->varno == varno &&
			var->varlevelsup == 0)
			other = arg1;
	}
	if (!other)
		return NULL;
	if (exprType(other) != TIDOID)
		return NULL;			/* probably can't happen */

	/* The other argument must be a pseudoconstant */
	if (!is_pseudo_constant_clause(other))
		return NULL;

	return other;				/* success */
}

/*
 *	Extract a set of CTID conditions from the given qual expression
 *
 *	If the expression is an AND clause, we can use a CTID condition
 *	from any sub-clause.  If it is an OR clause, we must be able to
 *	extract a CTID condition from every sub-clause, or we can't use it.
 *
 *	In theory, in the AND case we could get CTID conditions from different
 *	sub-clauses, in which case we could try to pick the most efficient one.
 *	In practice, such usage seems very unlikely, so we don't bother; we
 *	just exit as soon as we find the first candidate.
 *
 *	Returns a List of pseudoconstant TID expressions, or NIL if no match.
 *	(Has to be a list for the OR case.)
 */
static List *
TidQualFromExpr(int varno, Node *expr)
{
	List	   *rlst = NIL,
			   *frtn;
	ListCell   *l;
	Node	   *rnode;

	if (is_opclause(expr))
	{
		/* base case: check for tideq opclause */
		rnode = IsTidEqualClause(varno, (OpExpr *) expr);
		if (rnode)
			rlst = list_make1(rnode);
	}
	else if (and_clause(expr))
	{
		foreach(l, ((BoolExpr *) expr)->args)
		{
			rlst = TidQualFromExpr(varno, (Node *) lfirst(l));
			if (rlst)
				break;
		}
	}
	else if (or_clause(expr))
	{
		foreach(l, ((BoolExpr *) expr)->args)
		{
			frtn = TidQualFromExpr(varno, (Node *) lfirst(l));
			if (frtn)
				rlst = list_concat(rlst, frtn);
			else
			{
				if (rlst)
					list_free(rlst);
				rlst = NIL;
				break;
			}
		}
	}
	return rlst;
}

/*
 *	Extract a set of CTID conditions from the given restrictinfo list
 *
 *	This is essentially identical to the AND case of TidQualFromExpr,
 *	except for the format of the input.
 */
static List *
TidQualFromRestrictinfo(int varno, List *restrictinfo)
{
	List	   *rlst = NIL;
	ListCell   *l;

	foreach(l, restrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		if (!IsA(rinfo, RestrictInfo))
			continue;			/* probably should never happen */
		rlst = TidQualFromExpr(varno, (Node *) rinfo->clause);
		if (rlst)
			break;
	}
	return rlst;
}

/*
 * create_tidscan_paths
 *	  Create paths corresponding to direct TID scans of the given rel.
 *
 *	  Candidate paths are added to the rel's pathlist (using add_path).
 */
void
create_tidscan_paths(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *tideval;

	tideval = TidQualFromRestrictinfo(rel->relid, rel->baserestrictinfo);

	if (tideval)
		add_path(rel, (Path *) create_tidscan_path(root, rel, tideval));
}
