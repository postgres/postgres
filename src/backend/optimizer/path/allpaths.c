/*-------------------------------------------------------------------------
 *
 * allpaths.c
 *	  Routines to find possible search paths for processing a query
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/allpaths.c,v 1.108.2.1 2003/12/17 17:08:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#ifdef OPTIMIZER_DEBUG
#include "nodes/print.h"
#endif
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/geqo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "parser/parse_clause.h"
#include "rewrite/rewriteManip.h"


/* These parameters are set by GUC */
bool		enable_geqo = false;	/* just in case GUC doesn't set it */
int			geqo_threshold;


static void set_base_rel_pathlists(Query *root);
static void set_plain_rel_pathlist(Query *root, RelOptInfo *rel,
					   RangeTblEntry *rte);
static void set_inherited_rel_pathlist(Query *root, RelOptInfo *rel,
						   Index rti, RangeTblEntry *rte,
						   List *inheritlist);
static void set_subquery_pathlist(Query *root, RelOptInfo *rel,
					  Index rti, RangeTblEntry *rte);
static void set_function_pathlist(Query *root, RelOptInfo *rel,
					  RangeTblEntry *rte);
static RelOptInfo *make_one_rel_by_joins(Query *root, int levels_needed,
					  List *initial_rels);
static bool subquery_is_pushdown_safe(Query *subquery, Query *topquery,
						  bool *differentTypes);
static bool recurse_pushdown_safe(Node *setOp, Query *topquery,
					  bool *differentTypes);
static void compare_tlist_datatypes(List *tlist, List *colTypes,
						bool *differentTypes);
static bool qual_is_pushdown_safe(Query *subquery, Index rti, Node *qual,
					  bool *differentTypes);
static void subquery_push_qual(Query *subquery, Index rti, Node *qual);
static void recurse_push_qual(Node *setOp, Query *topquery,
				  Index rti, Node *qual);


/*
 * make_one_rel
 *	  Finds all possible access paths for executing a query, returning a
 *	  single rel that represents the join of all base rels in the query.
 */
RelOptInfo *
make_one_rel(Query *root)
{
	RelOptInfo *rel;

	/*
	 * Generate access paths for the base rels.
	 */
	set_base_rel_pathlists(root);

	/*
	 * Generate access paths for the entire join tree.
	 */
	Assert(root->jointree != NULL && IsA(root->jointree, FromExpr));

	rel = make_fromexpr_rel(root, root->jointree);

	/*
	 * The result should join all the query's base rels.
	 */
	Assert(bms_num_members(rel->relids) == length(root->base_rel_list));

	return rel;
}

/*
 * set_base_rel_pathlists
 *	  Finds all paths available for scanning each base-relation entry.
 *	  Sequential scan and any available indices are considered.
 *	  Each useful path is attached to its relation's 'pathlist' field.
 */
static void
set_base_rel_pathlists(Query *root)
{
	List	   *rellist;

	foreach(rellist, root->base_rel_list)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(rellist);
		Index		rti = rel->relid;
		RangeTblEntry *rte;
		List	   *inheritlist;

		Assert(rti > 0);		/* better be base rel */
		rte = rt_fetch(rti, root->rtable);

		if (rel->rtekind == RTE_SUBQUERY)
		{
			/* Subquery --- generate a separate plan for it */
			set_subquery_pathlist(root, rel, rti, rte);
		}
		else if (rel->rtekind == RTE_FUNCTION)
		{
			/* RangeFunction --- generate a separate plan for it */
			set_function_pathlist(root, rel, rte);
		}
		else if ((inheritlist = expand_inherited_rtentry(root, rti, true))
				 != NIL)
		{
			/* Relation is root of an inheritance tree, process specially */
			set_inherited_rel_pathlist(root, rel, rti, rte, inheritlist);
		}
		else
		{
			/* Plain relation */
			set_plain_rel_pathlist(root, rel, rte);
		}

#ifdef OPTIMIZER_DEBUG
		debug_print_rel(root, rel);
#endif
	}
}

/*
 * set_plain_rel_pathlist
 *	  Build access paths for a plain relation (no subquery, no inheritance)
 */
static void
set_plain_rel_pathlist(Query *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	/* Mark rel with estimated output rows, width, etc */
	set_baserel_size_estimates(root, rel);

	/*
	 * Generate paths and add them to the rel's pathlist.
	 *
	 * Note: add_path() will discard any paths that are dominated by another
	 * available path, keeping only those paths that are superior along at
	 * least one dimension of cost or sortedness.
	 */

	/* Consider sequential scan */
	add_path(rel, create_seqscan_path(root, rel));

	/* Consider TID scans */
	create_tidscan_paths(root, rel);

	/* Consider index paths for both simple and OR index clauses */
	create_index_paths(root, rel);

	/* create_index_paths must be done before create_or_index_paths */
	create_or_index_paths(root, rel);

	/* Now find the cheapest of the paths for this rel */
	set_cheapest(rel);
}

/*
 * set_inherited_rel_pathlist
 *	  Build access paths for a inheritance tree rooted at rel
 *
 * inheritlist is a list of RT indexes of all tables in the inheritance tree,
 * including a duplicate of the parent itself.	Note we will not come here
 * unless there's at least one child in addition to the parent.
 *
 * NOTE: the passed-in rel and RTE will henceforth represent the appended
 * result of the whole inheritance tree.  The members of inheritlist represent
 * the individual tables --- in particular, the inheritlist member that is a
 * duplicate of the parent RTE represents the parent table alone.
 * We will generate plans to scan the individual tables that refer to
 * the inheritlist RTEs, whereas Vars elsewhere in the plan tree that
 * refer to the original RTE are taken to refer to the append output.
 * In particular, this means we have separate RelOptInfos for the parent
 * table and for the append output, which is a good thing because they're
 * not the same size.
 */
static void
set_inherited_rel_pathlist(Query *root, RelOptInfo *rel,
						   Index rti, RangeTblEntry *rte,
						   List *inheritlist)
{
	int			parentRTindex = rti;
	Oid			parentOID = rte->relid;
	List	   *subpaths = NIL;
	List	   *il;

	/*
	 * XXX for now, can't handle inherited expansion of FOR UPDATE; can we
	 * do better?
	 */
	if (intMember(parentRTindex, root->rowMarks))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SELECT FOR UPDATE is not supported for inheritance queries")));

	/*
	 * The executor will check the parent table's access permissions when
	 * it examines the parent's inheritlist entry.  There's no need to
	 * check twice, so turn off access check bits in the original RTE.
	 */
	rte->checkForRead = false;
	rte->checkForWrite = false;

	/*
	 * Initialize to compute size estimates for whole inheritance tree
	 */
	rel->rows = 0;
	rel->width = 0;

	/*
	 * Generate access paths for each table in the tree (parent AND
	 * children), and pick the cheapest path for each table.
	 */
	foreach(il, inheritlist)
	{
		int			childRTindex = lfirsti(il);
		RangeTblEntry *childrte;
		Oid			childOID;
		RelOptInfo *childrel;
		List	   *reltlist;
		List	   *parentvars;
		List	   *childvars;

		childrte = rt_fetch(childRTindex, root->rtable);
		childOID = childrte->relid;

		/*
		 * Make a RelOptInfo for the child so we can do planning.  Do NOT
		 * attach the RelOptInfo to the query's base_rel_list, however,
		 * since the child is not part of the main join tree.  Instead,
		 * the child RelOptInfo is added to other_rel_list.
		 */
		childrel = build_other_rel(root, childRTindex);

		/*
		 * Copy the parent's targetlist and restriction quals to the
		 * child, with attribute-number adjustment as needed.  We don't
		 * bother to copy the join quals, since we can't do any joining of
		 * the individual tables.  Also, we just zap attr_needed rather
		 * than trying to adjust it; it won't be looked at in the child.
		 */
		reltlist = FastListValue(&rel->reltargetlist);
		reltlist = (List *)
			adjust_inherited_attrs((Node *) reltlist,
								   parentRTindex,
								   parentOID,
								   childRTindex,
								   childOID);
		FastListFromList(&childrel->reltargetlist, reltlist);
		childrel->attr_needed = NULL;
		childrel->baserestrictinfo = (List *)
			adjust_inherited_attrs((Node *) rel->baserestrictinfo,
								   parentRTindex,
								   parentOID,
								   childRTindex,
								   childOID);

		/*
		 * Now compute child access paths, and save the cheapest.
		 */
		set_plain_rel_pathlist(root, childrel, childrte);

		subpaths = lappend(subpaths, childrel->cheapest_total_path);

		/*
		 * Propagate size information from the child back to the parent.
		 * For simplicity, we use the largest widths from any child as the
		 * parent estimates.
		 */
		rel->rows += childrel->rows;
		if (childrel->width > rel->width)
			rel->width = childrel->width;

		childvars = FastListValue(&childrel->reltargetlist);
		foreach(parentvars, FastListValue(&rel->reltargetlist))
		{
			Var		   *parentvar = (Var *) lfirst(parentvars);
			Var		   *childvar = (Var *) lfirst(childvars);
			int			parentndx = parentvar->varattno - rel->min_attr;
			int			childndx = childvar->varattno - childrel->min_attr;

			if (childrel->attr_widths[childndx] > rel->attr_widths[parentndx])
				rel->attr_widths[parentndx] = childrel->attr_widths[childndx];
			childvars = lnext(childvars);
		}
	}

	/*
	 * Finally, build Append path and install it as the only access path
	 * for the parent rel.
	 */
	add_path(rel, (Path *) create_append_path(rel, subpaths));

	/* Select cheapest path (pretty easy in this case...) */
	set_cheapest(rel);
}

/*
 * set_subquery_pathlist
 *		Build the (single) access path for a subquery RTE
 */
static void
set_subquery_pathlist(Query *root, RelOptInfo *rel,
					  Index rti, RangeTblEntry *rte)
{
	Query	   *subquery = rte->subquery;
	bool	   *differentTypes;
	List	   *pathkeys;

	/* We need a workspace for keeping track of set-op type coercions */
	differentTypes = (bool *)
		palloc0((length(subquery->targetList) + 1) * sizeof(bool));

	/*
	 * If there are any restriction clauses that have been attached to the
	 * subquery relation, consider pushing them down to become HAVING
	 * quals of the subquery itself.  (Not WHERE clauses, since they may
	 * refer to subquery outputs that are aggregate results.  But
	 * planner.c will transfer them into the subquery's WHERE if they do
	 * not.)  This transformation is useful because it may allow us to
	 * generate a better plan for the subquery than evaluating all the
	 * subquery output rows and then filtering them.
	 *
	 * There are several cases where we cannot push down clauses.
	 * Restrictions involving the subquery are checked by
	 * subquery_is_pushdown_safe().  Restrictions on individual clauses
	 * are checked by qual_is_pushdown_safe().
	 *
	 * Non-pushed-down clauses will get evaluated as qpquals of the
	 * SubqueryScan node.
	 *
	 * XXX Are there any cases where we want to make a policy decision not to
	 * push down a pushable qual, because it'd result in a worse plan?
	 */
	if (rel->baserestrictinfo != NIL &&
		subquery_is_pushdown_safe(subquery, subquery, differentTypes))
	{
		/* OK to consider pushing down individual quals */
		List	   *upperrestrictlist = NIL;
		List	   *lst;

		foreach(lst, rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lst);
			Node	   *clause = (Node *) rinfo->clause;

			if (qual_is_pushdown_safe(subquery, rti, clause, differentTypes))
			{
				/* Push it down */
				subquery_push_qual(subquery, rti, clause);
			}
			else
			{
				/* Keep it in the upper query */
				upperrestrictlist = lappend(upperrestrictlist, rinfo);
			}
		}
		rel->baserestrictinfo = upperrestrictlist;
	}

	pfree(differentTypes);

	/* Generate the plan for the subquery */
	rel->subplan = subquery_planner(subquery, 0.0 /* default case */ );

	/* Copy number of output rows from subplan */
	rel->tuples = rel->subplan->plan_rows;

	/* Mark rel with estimated output rows, width, etc */
	set_baserel_size_estimates(root, rel);

	/* Convert subquery pathkeys to outer representation */
	pathkeys = build_subquery_pathkeys(root, rel, subquery);

	/* Generate appropriate path */
	add_path(rel, create_subqueryscan_path(rel, pathkeys));

	/* Select cheapest path (pretty easy in this case...) */
	set_cheapest(rel);
}

/*
 * set_function_pathlist
 *		Build the (single) access path for a function RTE
 */
static void
set_function_pathlist(Query *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	/* Mark rel with estimated output rows, width, etc */
	set_function_size_estimates(root, rel);

	/* Generate appropriate path */
	add_path(rel, create_functionscan_path(root, rel));

	/* Select cheapest path (pretty easy in this case...) */
	set_cheapest(rel);
}

/*
 * make_fromexpr_rel
 *	  Build access paths for a FromExpr jointree node.
 */
RelOptInfo *
make_fromexpr_rel(Query *root, FromExpr *from)
{
	int			levels_needed;
	List	   *initial_rels = NIL;
	List	   *jt;

	/*
	 * Count the number of child jointree nodes.  This is the depth of the
	 * dynamic-programming algorithm we must employ to consider all ways
	 * of joining the child nodes.
	 */
	levels_needed = length(from->fromlist);

	if (levels_needed <= 0)
		return NULL;			/* nothing to do? */

	/*
	 * Construct a list of rels corresponding to the child jointree nodes.
	 * This may contain both base rels and rels constructed according to
	 * explicit JOIN directives.
	 */
	foreach(jt, from->fromlist)
	{
		Node	   *jtnode = (Node *) lfirst(jt);

		initial_rels = lappend(initial_rels,
							   make_jointree_rel(root, jtnode));
	}

	if (levels_needed == 1)
	{
		/*
		 * Single jointree node, so we're done.
		 */
		return (RelOptInfo *) lfirst(initial_rels);
	}
	else
	{
		/*
		 * Consider the different orders in which we could join the rels,
		 * using either GEQO or regular optimizer.
		 */
		if (enable_geqo && levels_needed >= geqo_threshold)
			return geqo(root, levels_needed, initial_rels);
		else
			return make_one_rel_by_joins(root, levels_needed, initial_rels);
	}
}

/*
 * make_one_rel_by_joins
 *	  Find all possible joinpaths for a query by successively finding ways
 *	  to join component relations into join relations.
 *
 * 'levels_needed' is the number of iterations needed, ie, the number of
 *		independent jointree items in the query.  This is > 1.
 *
 * 'initial_rels' is a list of RelOptInfo nodes for each independent
 *		jointree item.	These are the components to be joined together.
 *
 * Returns the final level of join relations, i.e., the relation that is
 * the result of joining all the original relations together.
 */
static RelOptInfo *
make_one_rel_by_joins(Query *root, int levels_needed, List *initial_rels)
{
	List	  **joinitems;
	int			lev;
	RelOptInfo *rel;

	/*
	 * We employ a simple "dynamic programming" algorithm: we first find
	 * all ways to build joins of two jointree items, then all ways to
	 * build joins of three items (from two-item joins and single items),
	 * then four-item joins, and so on until we have considered all ways
	 * to join all the items into one rel.
	 *
	 * joinitems[j] is a list of all the j-item rels.  Initially we set
	 * joinitems[1] to represent all the single-jointree-item relations.
	 */
	joinitems = (List **) palloc0((levels_needed + 1) * sizeof(List *));

	joinitems[1] = initial_rels;

	for (lev = 2; lev <= levels_needed; lev++)
	{
		List	   *x;

		/*
		 * Determine all possible pairs of relations to be joined at this
		 * level, and build paths for making each one from every available
		 * pair of lower-level relations.
		 */
		joinitems[lev] = make_rels_by_joins(root, lev, joinitems);

		/*
		 * Do cleanup work on each just-processed rel.
		 */
		foreach(x, joinitems[lev])
		{
			rel = (RelOptInfo *) lfirst(x);

#ifdef NOT_USED

			/*
			 * * for each expensive predicate in each path in each
			 * distinct rel, * consider doing pullup  -- JMH
			 */
			if (XfuncMode != XFUNC_NOPULL && XfuncMode != XFUNC_OFF)
				xfunc_trypullup(rel);
#endif

			/* Find and save the cheapest paths for this rel */
			set_cheapest(rel);

#ifdef OPTIMIZER_DEBUG
			debug_print_rel(root, rel);
#endif
		}
	}

	/*
	 * We should have a single rel at the final level.
	 */
	if (joinitems[levels_needed] == NIL)
		elog(ERROR, "failed to build any %d-way joins", levels_needed);
	Assert(length(joinitems[levels_needed]) == 1);

	rel = (RelOptInfo *) lfirst(joinitems[levels_needed]);

	return rel;
}

/*****************************************************************************
 *			PUSHING QUALS DOWN INTO SUBQUERIES
 *****************************************************************************/

/*
 * subquery_is_pushdown_safe - is a subquery safe for pushing down quals?
 *
 * subquery is the particular component query being checked.  topquery
 * is the top component of a set-operations tree (the same Query if no
 * set-op is involved).
 *
 * Conditions checked here:
 *
 * 1. If the subquery has a LIMIT clause, we must not push down any quals,
 * since that could change the set of rows returned.
 *
 * 2. If the subquery contains EXCEPT or EXCEPT ALL set ops we cannot push
 * quals into it, because that would change the results.
 *
 * 3. For subqueries using UNION/UNION ALL/INTERSECT/INTERSECT ALL, we can
 * push quals into each component query, but the quals can only reference
 * subquery columns that suffer no type coercions in the set operation.
 * Otherwise there are possible semantic gotchas.  So, we check the
 * component queries to see if any of them have different output types;
 * differentTypes[k] is set true if column k has different type in any
 * component.
 */
static bool
subquery_is_pushdown_safe(Query *subquery, Query *topquery,
						  bool *differentTypes)
{
	SetOperationStmt *topop;

	/* Check point 1 */
	if (subquery->limitOffset != NULL || subquery->limitCount != NULL)
		return false;

	/* Are we at top level, or looking at a setop component? */
	if (subquery == topquery)
	{
		/* Top level, so check any component queries */
		if (subquery->setOperations != NULL)
			if (!recurse_pushdown_safe(subquery->setOperations, topquery,
									   differentTypes))
				return false;
	}
	else
	{
		/* Setop component must not have more components (too weird) */
		if (subquery->setOperations != NULL)
			return false;
		/* Check whether setop component output types match top level */
		topop = (SetOperationStmt *) topquery->setOperations;
		Assert(topop && IsA(topop, SetOperationStmt));
		compare_tlist_datatypes(subquery->targetList,
								topop->colTypes,
								differentTypes);
	}
	return true;
}

/*
 * Helper routine to recurse through setOperations tree
 */
static bool
recurse_pushdown_safe(Node *setOp, Query *topquery,
					  bool *differentTypes)
{
	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, topquery->rtable);
		Query	   *subquery = rte->subquery;

		Assert(subquery != NULL);
		return subquery_is_pushdown_safe(subquery, topquery, differentTypes);
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		/* EXCEPT is no good */
		if (op->op == SETOP_EXCEPT)
			return false;
		/* Else recurse */
		if (!recurse_pushdown_safe(op->larg, topquery, differentTypes))
			return false;
		if (!recurse_pushdown_safe(op->rarg, topquery, differentTypes))
			return false;
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
	}
	return true;
}

/*
 * Compare tlist's datatypes against the list of set-operation result types.
 * For any items that are different, mark the appropriate element of
 * differentTypes[] to show that this column will have type conversions.
 */
static void
compare_tlist_datatypes(List *tlist, List *colTypes,
						bool *differentTypes)
{
	List	   *i;

	foreach(i, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(i);

		if (tle->resdom->resjunk)
			continue;			/* ignore resjunk columns */
		if (colTypes == NIL)
			elog(ERROR, "wrong number of tlist entries");
		if (tle->resdom->restype != lfirsto(colTypes))
			differentTypes[tle->resdom->resno] = true;
		colTypes = lnext(colTypes);
	}
	if (colTypes != NIL)
		elog(ERROR, "wrong number of tlist entries");
}

/*
 * qual_is_pushdown_safe - is a particular qual safe to push down?
 *
 * qual is a restriction clause applying to the given subquery (whose RTE
 * has index rti in the parent query).
 *
 * Conditions checked here:
 *
 * 1. The qual must not contain any subselects (mainly because I'm not sure
 * it will work correctly: sublinks will already have been transformed into
 * subplans in the qual, but not in the subquery).
 *
 * 2. The qual must not refer to any subquery output columns that were
 * found to have inconsistent types across a set operation tree by
 * subquery_is_pushdown_safe().
 *
 * 3. If the subquery uses DISTINCT ON, we must not push down any quals that
 * refer to non-DISTINCT output columns, because that could change the set
 * of rows returned.  This condition is vacuous for DISTINCT, because then
 * there are no non-DISTINCT output columns, but unfortunately it's fairly
 * expensive to tell the difference between DISTINCT and DISTINCT ON in the
 * parsetree representation.  It's cheaper to just make sure all the Vars
 * in the qual refer to DISTINCT columns.
 *
 * 4. We must not push down any quals that refer to subselect outputs that
 * return sets, else we'd introduce functions-returning-sets into the
 * subquery's WHERE/HAVING quals.
 */
static bool
qual_is_pushdown_safe(Query *subquery, Index rti, Node *qual,
					  bool *differentTypes)
{
	bool		safe = true;
	List	   *vars;
	List	   *vl;
	Bitmapset  *tested = NULL;

	/* Refuse subselects (point 1) */
	if (contain_subplans(qual))
		return false;

	/*
	 * Examine all Vars used in clause; since it's a restriction clause,
	 * all such Vars must refer to subselect output columns.
	 */
	vars = pull_var_clause(qual, false);
	foreach(vl, vars)
	{
		Var		   *var = (Var *) lfirst(vl);
		TargetEntry *tle;

		Assert(var->varno == rti);

		/*
		 * We use a bitmapset to avoid testing the same attno more than
		 * once.  (NB: this only works because subquery outputs can't have
		 * negative attnos.)
		 */
		if (bms_is_member(var->varattno, tested))
			continue;
		tested = bms_add_member(tested, var->varattno);

		/* Check point 2 */
		if (differentTypes[var->varattno])
		{
			safe = false;
			break;
		}

		/* Must find the tlist element referenced by the Var */
		tle = get_tle_by_resno(subquery->targetList, var->varattno);
		Assert(tle != NULL);
		Assert(!tle->resdom->resjunk);

		/* If subquery uses DISTINCT or DISTINCT ON, check point 3 */
		if (subquery->distinctClause != NIL &&
			!targetIsInSortList(tle, subquery->distinctClause))
		{
			/* non-DISTINCT column, so fail */
			safe = false;
			break;
		}

		/* Refuse functions returning sets (point 4) */
		if (expression_returns_set((Node *) tle->expr))
		{
			safe = false;
			break;
		}
	}

	freeList(vars);
	bms_free(tested);

	return safe;
}

/*
 * subquery_push_qual - push down a qual that we have determined is safe
 */
static void
subquery_push_qual(Query *subquery, Index rti, Node *qual)
{
	if (subquery->setOperations != NULL)
	{
		/* Recurse to push it separately to each component query */
		recurse_push_qual(subquery->setOperations, subquery, rti, qual);
	}
	else
	{
		/*
		 * We need to replace Vars in the qual (which must refer to
		 * outputs of the subquery) with copies of the subquery's
		 * targetlist expressions.	Note that at this point, any uplevel
		 * Vars in the qual should have been replaced with Params, so they
		 * need no work.
		 *
		 * This step also ensures that when we are pushing into a setop tree,
		 * each component query gets its own copy of the qual.
		 */
		qual = ResolveNew(qual, rti, 0,
						  subquery->targetList,
						  CMD_SELECT, 0);
		subquery->havingQual = make_and_qual(subquery->havingQual,
											 qual);

		/*
		 * We need not change the subquery's hasAggs or hasSublinks flags,
		 * since we can't be pushing down any aggregates that weren't
		 * there before, and we don't push down subselects at all.
		 */
	}
}

/*
 * Helper routine to recurse through setOperations tree
 */
static void
recurse_push_qual(Node *setOp, Query *topquery,
				  Index rti, Node *qual)
{
	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, topquery->rtable);
		Query	   *subquery = rte->subquery;

		Assert(subquery != NULL);
		subquery_push_qual(subquery, rti, qual);
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		recurse_push_qual(op->larg, topquery, rti, qual);
		recurse_push_qual(op->rarg, topquery, rti, qual);
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
	}
}

/*****************************************************************************
 *			DEBUG SUPPORT
 *****************************************************************************/

#ifdef OPTIMIZER_DEBUG

static void
print_relids(Relids relids)
{
	Relids		tmprelids;
	int			x;
	bool		first = true;

	tmprelids = bms_copy(relids);
	while ((x = bms_first_member(tmprelids)) >= 0)
	{
		if (!first)
			printf(" ");
		printf("%d", x);
		first = false;
	}
	bms_free(tmprelids);
}

static void
print_restrictclauses(Query *root, List *clauses)
{
	List	   *l;

	foreach(l, clauses)
	{
		RestrictInfo *c = lfirst(l);

		print_expr((Node *) c->clause, root->rtable);
		if (lnext(l))
			printf(", ");
	}
}

static void
print_path(Query *root, Path *path, int indent)
{
	const char *ptype;
	bool		join = false;
	Path	   *subpath = NULL;
	int			i;

	switch (nodeTag(path))
	{
		case T_Path:
			ptype = "SeqScan";
			break;
		case T_IndexPath:
			ptype = "IdxScan";
			break;
		case T_TidPath:
			ptype = "TidScan";
			break;
		case T_AppendPath:
			ptype = "Append";
			break;
		case T_ResultPath:
			ptype = "Result";
			subpath = ((ResultPath *) path)->subpath;
			break;
		case T_MaterialPath:
			ptype = "Material";
			subpath = ((MaterialPath *) path)->subpath;
			break;
		case T_UniquePath:
			ptype = "Unique";
			subpath = ((UniquePath *) path)->subpath;
			break;
		case T_NestPath:
			ptype = "NestLoop";
			join = true;
			break;
		case T_MergePath:
			ptype = "MergeJoin";
			join = true;
			break;
		case T_HashPath:
			ptype = "HashJoin";
			join = true;
			break;
		default:
			ptype = "???Path";
			break;
	}

	for (i = 0; i < indent; i++)
		printf("\t");
	printf("%s", ptype);

	if (path->parent)
	{
		printf("(");
		print_relids(path->parent->relids);
		printf(") rows=%.0f", path->parent->rows);
	}
	printf(" cost=%.2f..%.2f\n", path->startup_cost, path->total_cost);

	if (path->pathkeys)
	{
		for (i = 0; i < indent; i++)
			printf("\t");
		printf("  pathkeys: ");
		print_pathkeys(path->pathkeys, root->rtable);
	}

	if (join)
	{
		JoinPath   *jp = (JoinPath *) path;

		for (i = 0; i < indent; i++)
			printf("\t");
		printf("  clauses: ");
		print_restrictclauses(root, jp->joinrestrictinfo);
		printf("\n");

		if (IsA(path, MergePath))
		{
			MergePath  *mp = (MergePath *) path;

			if (mp->outersortkeys || mp->innersortkeys)
			{
				for (i = 0; i < indent; i++)
					printf("\t");
				printf("  sortouter=%d sortinner=%d\n",
					   ((mp->outersortkeys) ? 1 : 0),
					   ((mp->innersortkeys) ? 1 : 0));
			}
		}

		print_path(root, jp->outerjoinpath, indent + 1);
		print_path(root, jp->innerjoinpath, indent + 1);
	}

	if (subpath)
		print_path(root, subpath, indent + 1);
}

void
debug_print_rel(Query *root, RelOptInfo *rel)
{
	List	   *l;

	printf("RELOPTINFO (");
	print_relids(rel->relids);
	printf("): rows=%.0f width=%d\n", rel->rows, rel->width);

	if (rel->baserestrictinfo)
	{
		printf("\tbaserestrictinfo: ");
		print_restrictclauses(root, rel->baserestrictinfo);
		printf("\n");
	}

	foreach(l, rel->joininfo)
	{
		JoinInfo   *j = (JoinInfo *) lfirst(l);

		printf("\tjoininfo (");
		print_relids(j->unjoined_relids);
		printf("): ");
		print_restrictclauses(root, j->jinfo_restrictinfo);
		printf("\n");
	}

	printf("\tpath list:\n");
	foreach(l, rel->pathlist)
		print_path(root, lfirst(l), 1);
	printf("\n\tcheapest startup path:\n");
	print_path(root, rel->cheapest_startup_path, 1);
	printf("\n\tcheapest total path:\n");
	print_path(root, rel->cheapest_total_path, 1);
	printf("\n");
	fflush(stdout);
}

#endif   /* OPTIMIZER_DEBUG */
