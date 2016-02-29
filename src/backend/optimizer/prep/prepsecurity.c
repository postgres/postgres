/*-------------------------------------------------------------------------
 *
 * prepsecurity.c
 *	  Routines for preprocessing security barrier quals.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/prepsecurity.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/sysattr.h"
#include "catalog/heap.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/prep.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/rel.h"


typedef struct
{
	int			rt_index;		/* Index of security barrier RTE */
	int			sublevels_up;	/* Current nesting depth */
	Relation	rel;			/* RTE relation at rt_index */
	List	   *targetlist;		/* Targetlist for new subquery RTE */
	List	   *colnames;		/* Column names in subquery RTE */
	List	   *vars_processed; /* List of Vars already processed */
} security_barrier_replace_vars_context;

static void expand_security_qual(PlannerInfo *root, List *tlist, int rt_index,
					 RangeTblEntry *rte, Node *qual, bool targetRelation);

static void security_barrier_replace_vars(Node *node,
							  security_barrier_replace_vars_context *context);

static bool security_barrier_replace_vars_walker(Node *node,
							 security_barrier_replace_vars_context *context);


/*
 * expand_security_quals -
 *	  expands any security barrier quals on RTEs in the query rtable, turning
 *	  them into security barrier subqueries.
 *
 * Any given RTE may have multiple security barrier quals in a list, from which
 * we create a set of nested subqueries to isolate each security barrier from
 * the others, providing protection against malicious user-defined security
 * barriers.  The first security barrier qual in the list will be used in the
 * innermost subquery.
 *
 * In practice, the only RTEs that will have security barrier quals are those
 * that refer to tables with row-level security, or which are the target
 * relation of an update to an auto-updatable security barrier view.  RTEs
 * that read from a security barrier view will have already been expanded by
 * the rewriter.
 */
void
expand_security_quals(PlannerInfo *root, List *tlist)
{
	Query	   *parse = root->parse;
	int			rt_index;
	ListCell   *cell;

	/*
	 * Process each RTE in the rtable list.
	 *
	 * We only ever modify entries in place and append to the rtable, so it is
	 * safe to use a foreach loop here.
	 */
	rt_index = 0;
	foreach(cell, parse->rtable)
	{
		bool		targetRelation = false;
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(cell);

		rt_index++;

		if (rte->securityQuals == NIL)
			continue;

		/*
		 * Ignore any RTEs that aren't used in the query (such RTEs may be
		 * present for permissions checks).
		 */
		if (rt_index != parse->resultRelation &&
			!rangeTableEntry_used((Node *) parse, rt_index, 0))
			continue;

		/*
		 * If this RTE is the target then we need to make a copy of it before
		 * expanding it.  The unexpanded copy will become the new target, and
		 * the original RTE will be expanded to become the source of rows to
		 * update/delete.
		 */
		if (rt_index == parse->resultRelation)
		{
			RangeTblEntry *newrte = copyObject(rte);

			/*
			 * We need to let expand_security_qual know if this is the target
			 * relation, as it has additional work to do in that case.
			 *
			 * Capture that information here as we're about to replace
			 * parse->resultRelation.
			 */
			targetRelation = true;

			parse->rtable = lappend(parse->rtable, newrte);
			parse->resultRelation = list_length(parse->rtable);

			/*
			 * Wipe out any copied security barrier quals on the new target to
			 * prevent infinite recursion.
			 */
			newrte->securityQuals = NIL;

			/*
			 * There's no need to do permissions checks twice, so wipe out the
			 * permissions info for the original RTE (we prefer to keep the
			 * bits set on the result RTE).
			 */
			rte->requiredPerms = 0;
			rte->checkAsUser = InvalidOid;
			rte->selectedCols = NULL;
			rte->insertedCols = NULL;
			rte->updatedCols = NULL;

			/*
			 * For the most part, Vars referencing the original relation
			 * should remain as they are, meaning that they pull OLD values
			 * from the expanded RTE.  But in the RETURNING list and in any
			 * WITH CHECK OPTION quals, we want such Vars to represent NEW
			 * values, so change them to reference the new RTE.
			 */
			ChangeVarNodes((Node *) parse->returningList, rt_index,
						   parse->resultRelation, 0);

			ChangeVarNodes((Node *) parse->withCheckOptions, rt_index,
						   parse->resultRelation, 0);
		}

		/*
		 * Process each security barrier qual in turn, starting with the
		 * innermost one (the first in the list) and working outwards.
		 *
		 * We remove each qual from the list before processing it, so that its
		 * variables aren't modified by expand_security_qual.  Also we don't
		 * necessarily want the attributes referred to by the qual to be
		 * exposed by the newly built subquery.
		 */
		while (rte->securityQuals != NIL)
		{
			Node	   *qual = (Node *) linitial(rte->securityQuals);

			rte->securityQuals = list_delete_first(rte->securityQuals);

			ChangeVarNodes(qual, rt_index, 1, 0);
			expand_security_qual(root, tlist, rt_index, rte, qual,
								 targetRelation);
		}
	}
}


/*
 * expand_security_qual -
 *	  expand the specified security barrier qual on a query RTE, turning the
 *	  RTE into a security barrier subquery.
 */
static void
expand_security_qual(PlannerInfo *root, List *tlist, int rt_index,
					 RangeTblEntry *rte, Node *qual, bool targetRelation)
{
	Query	   *parse = root->parse;
	Oid			relid = rte->relid;
	Query	   *subquery;
	RangeTblEntry *subrte;
	RangeTblRef *subrtr;
	PlanRowMark *rc;
	security_barrier_replace_vars_context context;
	ListCell   *cell;

	/*
	 * There should only be 2 possible cases:
	 *
	 * 1. A relation RTE, which we turn into a subquery RTE containing all
	 * referenced columns.
	 *
	 * 2. A subquery RTE (either from a prior call to this function or from an
	 * expanded view).  In this case we build a new subquery on top of it to
	 * isolate this security barrier qual from any other quals.
	 */
	switch (rte->rtekind)
	{
		case RTE_RELATION:

			/*
			 * Turn the relation RTE into a security barrier subquery RTE,
			 * moving all permissions checks down into the subquery.
			 */
			subquery = makeNode(Query);
			subquery->commandType = CMD_SELECT;
			subquery->querySource = QSRC_INSTEAD_RULE;

			subrte = copyObject(rte);
			subrte->inFromCl = true;
			subrte->securityQuals = NIL;
			subquery->rtable = list_make1(subrte);

			subrtr = makeNode(RangeTblRef);
			subrtr->rtindex = 1;
			subquery->jointree = makeFromExpr(list_make1(subrtr), qual);
			subquery->hasSubLinks = checkExprHasSubLink(qual);

			rte->rtekind = RTE_SUBQUERY;
			rte->relid = InvalidOid;
			rte->subquery = subquery;
			rte->security_barrier = true;
			rte->inh = false;	/* must not be set for a subquery */

			/* the permissions checks have now been moved down */
			rte->requiredPerms = 0;
			rte->checkAsUser = InvalidOid;
			rte->selectedCols = NULL;
			rte->insertedCols = NULL;
			rte->updatedCols = NULL;

			/*
			 * Now deal with any PlanRowMark on this RTE by requesting a lock
			 * of the same strength on the RTE copied down to the subquery.
			 *
			 * Note that we can only push down user-defined quals if they are
			 * only using leakproof (and therefore trusted) functions and
			 * operators.  As a result, we may end up locking more rows than
			 * strictly necessary (and, in the worst case, we could end up
			 * locking all rows which pass the securityQuals).  This is
			 * currently documented behavior, but it'd be nice to come up with
			 * a better solution some day.
			 */
			rc = get_plan_rowmark(root->rowMarks, rt_index);
			if (rc != NULL)
			{
				if (rc->strength != LCS_NONE)
					applyLockingClause(subquery, 1, rc->strength,
									   rc->waitPolicy, false);
				root->rowMarks = list_delete_ptr(root->rowMarks, rc);
			}

			/*
			 * When we are replacing the target relation with a subquery, we
			 * need to make sure to add a locking clause explicitly to the
			 * generated subquery since there won't be any row marks against
			 * the target relation itself.
			 */
			if (targetRelation)
				applyLockingClause(subquery, 1, LCS_FORUPDATE,
								   LockWaitBlock, false);

			/*
			 * Replace any variables in the outer query that refer to the
			 * original relation RTE with references to columns that we will
			 * expose in the new subquery, building the subquery's targetlist
			 * as we go.  Also replace any references in the translated_vars
			 * lists of any appendrels.
			 */
			context.rt_index = rt_index;
			context.sublevels_up = 0;
			context.rel = heap_open(relid, NoLock);
			context.targetlist = NIL;
			context.colnames = NIL;
			context.vars_processed = NIL;

			security_barrier_replace_vars((Node *) parse, &context);
			security_barrier_replace_vars((Node *) tlist, &context);
			security_barrier_replace_vars((Node *) root->append_rel_list,
										  &context);

			heap_close(context.rel, NoLock);

			/* Now we know what columns the subquery needs to expose */
			rte->subquery->targetList = context.targetlist;
			rte->eref = makeAlias(rte->eref->aliasname, context.colnames);

			break;

		case RTE_SUBQUERY:

			/*
			 * Build a new subquery that includes all the same columns as the
			 * original subquery.
			 */
			subquery = makeNode(Query);
			subquery->commandType = CMD_SELECT;
			subquery->querySource = QSRC_INSTEAD_RULE;
			subquery->targetList = NIL;

			foreach(cell, rte->subquery->targetList)
			{
				TargetEntry *tle;
				Var		   *var;

				tle = (TargetEntry *) lfirst(cell);
				var = makeVarFromTargetEntry(1, tle);

				tle = makeTargetEntry((Expr *) var,
									  list_length(subquery->targetList) + 1,
									  pstrdup(tle->resname),
									  tle->resjunk);
				subquery->targetList = lappend(subquery->targetList, tle);
			}

			subrte = makeNode(RangeTblEntry);
			subrte->rtekind = RTE_SUBQUERY;
			subrte->subquery = rte->subquery;
			subrte->security_barrier = rte->security_barrier;
			subrte->eref = copyObject(rte->eref);
			subrte->inFromCl = true;
			subquery->rtable = list_make1(subrte);

			subrtr = makeNode(RangeTblRef);
			subrtr->rtindex = 1;
			subquery->jointree = makeFromExpr(list_make1(subrtr), qual);
			subquery->hasSubLinks = checkExprHasSubLink(qual);

			rte->subquery = subquery;
			rte->security_barrier = true;

			break;

		default:
			elog(ERROR, "invalid range table entry for security barrier qual");
	}
}


/*
 * security_barrier_replace_vars -
 *	  Apply security barrier variable replacement to an expression tree.
 *
 * This also builds/updates a targetlist with entries for each replacement
 * variable that needs to be exposed by the security barrier subquery RTE.
 *
 * NOTE: although this has the form of a walker, we cheat and modify the
 * nodes in-place.  The given expression tree should have been copied
 * earlier to ensure that no unwanted side-effects occur!
 */
static void
security_barrier_replace_vars(Node *node,
							  security_barrier_replace_vars_context *context)
{
	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, go straight to query_tree_walker to make sure that
	 * sublevels_up doesn't get incremented prematurely.
	 */
	if (node && IsA(node, Query))
		query_tree_walker((Query *) node,
						  security_barrier_replace_vars_walker,
						  (void *) context, 0);
	else
		security_barrier_replace_vars_walker(node, context);
}

static bool
security_barrier_replace_vars_walker(Node *node,
							  security_barrier_replace_vars_context *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		/*
		 * Note that the same Var may be present in different lists, so we
		 * need to take care not to process it multiple times.
		 */
		if (var->varno == context->rt_index &&
			var->varlevelsup == context->sublevels_up &&
			!list_member_ptr(context->vars_processed, var))
		{
			/*
			 * Found a matching variable. Make sure that it is in the subquery
			 * targetlist and map its attno accordingly.
			 */
			AttrNumber	attno;
			ListCell   *l;
			TargetEntry *tle;
			char	   *attname;
			Var		   *newvar;

			/* Search for the base attribute in the subquery targetlist */
			attno = InvalidAttrNumber;
			foreach(l, context->targetlist)
			{
				tle = (TargetEntry *) lfirst(l);
				attno++;

				Assert(IsA(tle->expr, Var));
				if (((Var *) tle->expr)->varattno == var->varattno &&
					((Var *) tle->expr)->varcollid == var->varcollid)
				{
					/* Map the variable onto this subquery targetlist entry */
					var->varattno = var->varoattno = attno;
					/* Mark this var as having been processed */
					context->vars_processed = lappend(context->vars_processed, var);
					return false;
				}
			}

			/* Not in the subquery targetlist, so add it. Get its name. */
			if (var->varattno < 0)
			{
				Form_pg_attribute att_tup;

				att_tup = SystemAttributeDefinition(var->varattno,
										   context->rel->rd_rel->relhasoids);
				attname = NameStr(att_tup->attname);
			}
			else if (var->varattno == InvalidAttrNumber)
			{
				attname = "wholerow";
			}
			else if (var->varattno <= context->rel->rd_att->natts)
			{
				Form_pg_attribute att_tup;

				att_tup = context->rel->rd_att->attrs[var->varattno - 1];
				attname = NameStr(att_tup->attname);
			}
			else
			{
				elog(ERROR, "invalid attribute number %d in security_barrier_replace_vars", var->varattno);
			}

			/* New variable for subquery targetlist */
			newvar = copyObject(var);
			newvar->varno = newvar->varnoold = 1;
			newvar->varlevelsup = 0;

			attno = list_length(context->targetlist) + 1;
			tle = makeTargetEntry((Expr *) newvar,
								  attno,
								  pstrdup(attname),
								  false);

			context->targetlist = lappend(context->targetlist, tle);

			context->colnames = lappend(context->colnames,
										makeString(pstrdup(attname)));

			/* Update the outer query's variable */
			var->varattno = var->varoattno = attno;

			/* Remember this Var so that we don't process it again */
			context->vars_processed = lappend(context->vars_processed, var);
		}
		return false;
	}

	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   security_barrier_replace_vars_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}

	return expression_tree_walker(node, security_barrier_replace_vars_walker,
								  (void *) context);
}
