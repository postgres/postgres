/*-------------------------------------------------------------------------
 *
 * prepunion.c
 *	  Routines to plan set-operation and inheritance queries.  The filename
 *	  is a leftover from a time when only UNIONs were handled.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/prep/prepunion.c,v 1.55 2000/11/09 02:46:17 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/types.h>

#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

typedef struct
{
	Index		rt_index;
	int			sublevels_up;
	Oid			old_relid;
	Oid			new_relid;
} fix_parsetree_attnums_context;

static Plan *recurse_set_operations(Node *setOp, Query *parse,
									List *colTypes, bool junkOK,
									int flag, List *refnames_tlist);
static Plan *generate_union_plan(SetOperationStmt *op, Query *parse,
								 List *refnames_tlist);
static Plan *generate_nonunion_plan(SetOperationStmt *op, Query *parse,
									List *refnames_tlist);
static List *recurse_union_children(Node *setOp, Query *parse,
									SetOperationStmt *top_union,
									List *refnames_tlist);
static List *generate_setop_tlist(List *colTypes, int flag,
								  bool hack_constants,
								  List *input_tlist,
								  List *refnames_tlist);
static bool tlist_same_datatypes(List *tlist, List *colTypes, bool junkOK);
static void fix_parsetree_attnums(Index rt_index, Oid old_relid,
					  Oid new_relid, Query *parsetree);
static bool fix_parsetree_attnums_walker(Node *node,
							 fix_parsetree_attnums_context *context);
static RangeTblEntry *new_rangetable_entry(Oid new_relid,
					 RangeTblEntry *old_entry);
static Append *make_append(List *appendplans, Index rt_index,
						   List *inheritrtable, List *tlist);


/*
 * plan_set_operations
 *
 *	  Plans the queries for a tree of set operations (UNION/INTERSECT/EXCEPT)
 *
 * This routine only deals with the setOperations tree of the given query.
 * Any top-level ORDER BY requested in parse->sortClause will be added on
 * back in union_planner.
 */
Plan *
plan_set_operations(Query *parse)
{
	SetOperationStmt *topop = (SetOperationStmt *) parse->setOperations;
	Node	   *node;
	Query	   *leftmostQuery;

	Assert(topop && IsA(topop, SetOperationStmt));

	/*
	 * Find the leftmost component Query.  We need to use its column names
	 * for all generated tlists (else SELECT INTO won't work right).
	 */
	node = topop->larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostQuery = rt_fetch(((RangeTblRef *) node)->rtindex,
							 parse->rtable)->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * Recurse on setOperations tree to generate plans for set ops.
	 * The final output plan should have just the column types shown
	 * as the output from the top-level node, plus possibly a resjunk
	 * working column (we can rely on upper-level nodes to deal with that).
	 */
	return recurse_set_operations((Node *) topop, parse,
								  topop->colTypes, true, -1,
								  leftmostQuery->targetList);
}

/*
 * recurse_set_operations
 *	  Recursively handle one step in a tree of set operations
 *
 * colTypes: integer list of type OIDs of expected output columns
 * junkOK: if true, child resjunk columns may be left in the result
 * flag: if >= 0, add a resjunk output column indicating value of flag
 * refnames_tlist: targetlist to take column names from
 */
static Plan *
recurse_set_operations(Node *setOp, Query *parse,
					   List *colTypes, bool junkOK,
					   int flag, List *refnames_tlist)
{
	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, parse->rtable);
		Query  *subquery = rte->subquery;
		Plan   *subplan,
			   *plan;

		Assert(subquery != NULL);
		/*
		 * Generate plan for primitive subquery
		 */
		subplan = subquery_planner(subquery,
								   -1.0 /* default case */ );
		/*
		 * Add a SubqueryScan with the caller-requested targetlist
		 */
		plan = (Plan *)
			make_subqueryscan(generate_setop_tlist(colTypes, flag, true,
												   subplan->targetlist,
												   refnames_tlist),
							  NIL,
							  rtr->rtindex,
							  subplan);
		copy_plan_costsize(plan, subplan);
		return plan;
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;
		Plan   *plan;

		/* UNIONs are much different from INTERSECT/EXCEPT */
		if (op->op == SETOP_UNION)
			plan = generate_union_plan(op, parse, refnames_tlist);
		else
			plan = generate_nonunion_plan(op, parse, refnames_tlist);
		/*
		 * If necessary, add a Result node to project the caller-requested
		 * output columns.
		 *
		 * XXX you don't really want to know about this: setrefs.c will apply
		 * replace_vars_with_subplan_refs() to the Result node's tlist.
		 * This would fail if the input plan's non-resjunk tlist entries were
		 * not all simple Vars equal() to the referencing Vars generated by
		 * generate_setop_tlist().  However, since the input plan was
		 * generated by generate_union_plan() or generate_nonunion_plan(),
		 * the referencing Vars will equal the tlist entries they reference.
		 * Ugly but I don't feel like making that code more general right now.
		 */
		if (flag >= 0 ||
			! tlist_same_datatypes(plan->targetlist, colTypes, junkOK))
		{
			plan = (Plan *)
				make_result(generate_setop_tlist(colTypes, flag, false,
												 plan->targetlist,
												 refnames_tlist),
							NULL,
							plan);
		}
		return plan;
	}
	else
	{
		elog(ERROR, "recurse_set_operations: unexpected node %d",
			 (int) nodeTag(setOp));
		return NULL;			/* keep compiler quiet */
	}
}

/*
 * Generate plan for a UNION or UNION ALL node
 */
static Plan *
generate_union_plan(SetOperationStmt *op, Query *parse,
					List *refnames_tlist)
{
	List   *planlist;
	Plan   *plan;

	/*
	 * If any of my children are identical UNION nodes (same op, all-flag,
	 * and colTypes) then they can be merged into this node so that we
	 * generate only one Append and Sort for the lot.  Recurse to find
	 * such nodes and compute their children's plans.
	 */
	planlist = nconc(recurse_union_children(op->larg, parse,
											op, refnames_tlist),
					 recurse_union_children(op->rarg, parse,
											op, refnames_tlist));
	/*
	 * Append the child results together.
	 *
	 * The tlist for an Append plan isn't important as far as the Append
	 * is concerned, but we must make it look real anyway for the benefit
	 * of the next plan level up.
	 */
	plan = (Plan *)
		make_append(planlist,
					0,
					NIL,
					generate_setop_tlist(op->colTypes, -1, false,
									((Plan *) lfirst(planlist))->targetlist,
									refnames_tlist));
	/*
	 * For UNION ALL, we just need the Append plan.  For UNION,
	 * need to add Sort and Unique nodes to produce unique output.
	 */
	if (! op->all)
	{
		List   *tlist,
			   *sortList;

		tlist = new_unsorted_tlist(plan->targetlist);
		sortList = addAllTargetsToSortList(NIL, tlist);
		plan = make_sortplan(tlist, plan, sortList);
		plan = (Plan *) make_unique(tlist, plan, copyObject(sortList));
	}
	return plan;
}

/*
 * Generate plan for an INTERSECT, INTERSECT ALL, EXCEPT, or EXCEPT ALL node
 */
static Plan *
generate_nonunion_plan(SetOperationStmt *op, Query *parse,
					   List *refnames_tlist)
{
	Plan   *lplan,
		   *rplan,
		   *plan;
	List   *tlist,
		   *sortList;
	SetOpCmd cmd;

	/* Recurse on children, ensuring their outputs are marked */
	lplan = recurse_set_operations(op->larg, parse,
								   op->colTypes, false, 0,
								   refnames_tlist);
	rplan = recurse_set_operations(op->rarg, parse,
								   op->colTypes, false, 1,
								   refnames_tlist);
	/*
	 * Append the child results together.
	 *
	 * The tlist for an Append plan isn't important as far as the Append
	 * is concerned, but we must make it look real anyway for the benefit
	 * of the next plan level up.
	 */
	plan = (Plan *)
		make_append(makeList2(lplan, rplan),
					0,
					NIL,
					generate_setop_tlist(op->colTypes, 0, false,
										 lplan->targetlist,
										 refnames_tlist));
	/*
	 * Sort the child results, then add a SetOp plan node to
	 * generate the correct output.
	 */
	tlist = new_unsorted_tlist(plan->targetlist);
	sortList = addAllTargetsToSortList(NIL, tlist);
	plan = make_sortplan(tlist, plan, sortList);
	switch (op->op)
	{
		case SETOP_INTERSECT:
			cmd = op->all ? SETOPCMD_INTERSECT_ALL : SETOPCMD_INTERSECT;
			break;
		case SETOP_EXCEPT:
			cmd = op->all ? SETOPCMD_EXCEPT_ALL : SETOPCMD_EXCEPT;
			break;
		default:
			elog(ERROR, "generate_nonunion_plan: bogus operation code");
			cmd = SETOPCMD_INTERSECT; /* keep compiler quiet */
			break;
	}
	plan = (Plan *) make_setop(cmd, tlist, plan, sortList,
							   length(op->colTypes)+1);
	return plan;
}

/*
 * Pull up children of a UNION node that are identically-propertied UNIONs.
 *
 * NOTE: we can also pull a UNION ALL up into a UNION, since the distinct
 * output rows will be lost anyway.
 */
static List *
recurse_union_children(Node *setOp, Query *parse,
					   SetOperationStmt *top_union,
					   List *refnames_tlist)
{
	if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		if (op->op == top_union->op &&
			(op->all == top_union->all || op->all) &&
			equali(op->colTypes, top_union->colTypes))
		{
			/* Same UNION, so fold children into parent's subplan list */
			return nconc(recurse_union_children(op->larg, parse,
												top_union, refnames_tlist),
						 recurse_union_children(op->rarg, parse,
												top_union, refnames_tlist));
		}
	}
	/*
	 * Not same, so plan this child separately.
	 *
	 * Note we disallow any resjunk columns in child results.  This
	 * is necessary since the Append node that implements the union
	 * won't do any projection, and upper levels will get confused if
	 * some of our output tuples have junk and some don't.  This case
	 * only arises when we have an EXCEPT or INTERSECT as child, else
	 * there won't be resjunk anyway.
	 */
	return makeList1(recurse_set_operations(setOp, parse,
											top_union->colTypes, false,
											-1, refnames_tlist));
}

/*
 * Generate targetlist for a set-operation plan node
 */
static List *
generate_setop_tlist(List *colTypes, int flag,
					 bool hack_constants,
					 List *input_tlist,
					 List *refnames_tlist)
{
	List	   *tlist = NIL;
	int			resno = 1;
	List	   *i;
	Resdom	   *resdom;
	Node	   *expr;

	foreach(i, colTypes)
	{
		Oid		colType = (Oid) lfirsti(i);
		TargetEntry *inputtle = (TargetEntry *) lfirst(input_tlist);
		TargetEntry *reftle = (TargetEntry *) lfirst(refnames_tlist);

		Assert(inputtle->resdom->resno == resno);
		Assert(reftle->resdom->resno == resno);
		Assert(!inputtle->resdom->resjunk);
		Assert(!reftle->resdom->resjunk);
		/*
		 * Generate columns referencing input columns and having
		 * appropriate data types and column names.  Insert datatype
		 * coercions where necessary.
		 *
		 * HACK: constants in the input's targetlist are copied up as-is
		 * rather than being referenced as subquery outputs.  This is mainly
		 * to ensure that when we try to coerce them to the output column's
		 * datatype, the right things happen for UNKNOWN constants.  But do
		 * this only at the first level of subquery-scan plans; we don't
		 * want phony constants appearing in the output tlists of upper-level
		 * nodes!
		 */
		resdom = makeResdom((AttrNumber) resno++,
							colType,
							-1,
							pstrdup(reftle->resdom->resname),
							false);
		if (hack_constants && inputtle->expr && IsA(inputtle->expr, Const))
			expr = inputtle->expr;
		else
			expr = (Node *) makeVar(0,
									inputtle->resdom->resno,
									inputtle->resdom->restype,
									inputtle->resdom->restypmod,
									0);
		expr = coerce_to_common_type(NULL,
									 expr,
									 colType,
									 "UNION/INTERSECT/EXCEPT");
		tlist = lappend(tlist, makeTargetEntry(resdom, expr));
		input_tlist = lnext(input_tlist);
		refnames_tlist = lnext(refnames_tlist);
	}

	if (flag >= 0)
	{
		/* Add a resjunk column yielding specified flag value */
		resdom = makeResdom((AttrNumber) resno++,
							INT4OID,
							-1,
							pstrdup("flag"),
							true);
		expr = (Node *) makeConst(INT4OID,
								  sizeof(int4),
								  Int32GetDatum(flag),
								  false,
								  true,
								  false,
								  false);
		tlist = lappend(tlist, makeTargetEntry(resdom, expr));
	}

	return tlist;
}

/*
 * Does tlist have same datatypes as requested colTypes?
 *
 * Resjunk columns are ignored if junkOK is true; otherwise presence of
 * a resjunk column will always cause a 'false' result.
 */
static bool
tlist_same_datatypes(List *tlist, List *colTypes, bool junkOK)
{
	List	   *i;

	foreach(i, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(i);

		if (tle->resdom->resjunk)
		{
			if (! junkOK)
				return false;
		}
		else
		{
			if (colTypes == NIL)
				return false;
			if (tle->resdom->restype != (Oid) lfirsti(colTypes))
				return false;
			colTypes = lnext(colTypes);
		}
	}
	if (colTypes != NIL)
		return false;
	return true;
}


/*
 * plan_inherit_queries
 *	  Plans the queries for an inheritance tree rooted at a parent relation.
 *
 * Inputs:
 *	root = parent parse tree
 *	tlist = target list for inheritance subqueries (not same as parent's!)
 *	rt_index = rangetable index for current inheritance item
 *	inheritors = list of OIDs of the target rel plus all its descendants
 *
 * Returns an APPEND node that forms the result of performing the given
 * query for each member relation of the inheritance group.
 *
 * If grouping, aggregation, or sorting is specified in the parent plan,
 * the subplans should not do any of those steps --- we must do those
 * operations just once above the APPEND node.	The given tlist has been
 * modified appropriately to remove group/aggregate expressions, but the
 * Query node still has the relevant fields set.  We remove them in the
 * copies used for subplans.
 *
 * NOTE: this can be invoked recursively if more than one inheritance wildcard
 * is present.	At each level of recursion, the first wildcard remaining in
 * the rangetable is expanded.
 *
 * NOTE: don't bother optimizing this routine for the case that the target
 * rel has no children.  We won't get here unless find_inheritable_rt_entry
 * found at least two members in the inheritance group, so an APPEND is
 * certainly necessary.
 */
Plan *
plan_inherit_queries(Query *root, List *tlist,
					 Index rt_index, List *inheritors)
{
	RangeTblEntry *rt_entry = rt_fetch(rt_index, root->rtable);
	List	   *union_plans = NIL;
	List	   *union_rtentries = NIL;
	List	   *save_tlist = root->targetList;
	double		tuple_fraction;
	List	   *i;

	/*
	 * Avoid making copies of the root's tlist, which we aren't going to
	 * use anyway (we are going to make copies of the passed tlist,
	 * instead).  This is purely a space-saving hack.  Note we restore
	 * the root's tlist before exiting.
	 */
	root->targetList = NIL;

	/*
	 * If we are going to need sorting or grouping at the top level, force
	 * lower-level planners to assume that all tuples will be retrieved.
	 */
	if (root->distinctClause || root->sortClause ||
		root->groupClause || root->hasAggs)
		tuple_fraction = 0.0;	/* will need all tuples from each subplan */
	else
		tuple_fraction = -1.0;	/* default behavior is OK (I think) */

	foreach(i, inheritors)
	{
		Oid			relid = lfirsti(i);

		/*
		 * Make a modifiable copy of the original query, and replace the
		 * target rangetable entry in it with a new one identifying this
		 * child table.  The new rtentry is marked inh = false --- this
		 * is essential to prevent infinite recursion when the subquery
		 * is rescanned by find_inheritable_rt_entry!
		 */
		Query	   *new_root = copyObject(root);
		RangeTblEntry *new_rt_entry = new_rangetable_entry(relid,
														   rt_entry);

		new_rt_entry->inh = false;
		rt_store(rt_index, new_root->rtable, new_rt_entry);

		/*
		 * Insert (a modifiable copy of) the desired simplified tlist into
		 * the subquery
		 */
		new_root->targetList = copyObject(tlist);

		/*
		 * Clear the sorting and grouping qualifications in the subquery,
		 * so that sorting will only be done once after append
		 */
		new_root->distinctClause = NIL;
		new_root->sortClause = NIL;
		new_root->groupClause = NIL;
		new_root->havingQual = NULL;
		new_root->limitOffset = NULL;	/* LIMIT's probably unsafe too */
		new_root->limitCount = NULL;
		new_root->hasAggs = false;		/* shouldn't be any left ... */

		/*
		 * Update attribute numbers in case child has different ordering
		 * of columns than parent (as can happen after ALTER TABLE).
		 *
		 * XXX This is a crock, and it doesn't really work.  It'd be better
		 * to fix ALTER TABLE to preserve consistency of attribute
		 * numbering.
		 */
		fix_parsetree_attnums(rt_index,
							  rt_entry->relid,
							  relid,
							  new_root);

		/*
		 * Plan the subquery by recursively calling union_planner().
		 * Add plan and child rtentry to lists for APPEND.
		 */
		union_plans = lappend(union_plans,
							  union_planner(new_root, tuple_fraction));
		union_rtentries = lappend(union_rtentries, new_rt_entry);
	}

	/* Restore root's tlist */
	root->targetList = save_tlist;

	/* Construct the finished Append plan. */
	return (Plan *) make_append(union_plans,
								rt_index,
								union_rtentries,
								((Plan *) lfirst(union_plans))->targetlist);
}

/*
 * find_all_inheritors -
 *		Returns an integer list of relids including the given rel plus
 *		all relations that inherit from it, directly or indirectly.
 */
List *
find_all_inheritors(Oid parentrel)
{
	List	   *examined_relids = NIL;
	List	   *unexamined_relids = lconsi(parentrel, NIL);

	/*
	 * While the queue of unexamined relids is nonempty, remove the first
	 * element, mark it examined, and find its direct descendants. NB:
	 * cannot use foreach(), since we modify the queue inside loop.
	 */
	while (unexamined_relids != NIL)
	{
		Oid			currentrel = lfirsti(unexamined_relids);
		List	   *currentchildren;

		unexamined_relids = lnext(unexamined_relids);
		examined_relids = lappendi(examined_relids, currentrel);
		currentchildren = find_inheritance_children(currentrel);

		/*
		 * Add to the queue only those children not already seen.
		 * This avoids making duplicate entries in case of multiple
		 * inheritance paths from the same parent.  (It'll also keep
		 * us from getting into an infinite loop, though theoretically
		 * there can't be any cycles in the inheritance graph anyway.)
		 */
		currentchildren = set_differencei(currentchildren, examined_relids);
		unexamined_relids = set_unioni(unexamined_relids, currentchildren);
	}

	return examined_relids;
}

/*
 * find_inheritable_rt_entry -
 *		Given a rangetable, find the first rangetable entry that represents
 *		an inheritance set.
 *
 *		If successful, set *rt_index to the index (1..n) of the entry,
 *		set *inheritors to a list of the relation OIDs of the set,
 *		and return TRUE.
 *
 *		If there is no entry that requires inheritance processing,
 *		return FALSE.
 *
 * NOTE: We return the inheritors list so that plan_inherit_queries doesn't
 * have to compute it again.
 *
 * NOTE: We clear the inh flag in any entries that have it set but turn
 * out not to have any actual inheritance children.  This is an efficiency
 * hack to avoid having to repeat the inheritance checks if the list is
 * scanned again (as will happen during expansion of any subsequent entry
 * that does have inheritance children).  Although modifying the input
 * rangetable in-place may seem uncool, there's no reason not to do it,
 * since any re-examination of the entry would just come to the same
 * conclusion that the table has no children.
 */
bool
find_inheritable_rt_entry(List *rangetable,
						  Index *rt_index,
						  List **inheritors)
{
	Index		count = 0;
	List	   *temp;

	foreach(temp, rangetable)
	{
		RangeTblEntry  *rt_entry = (RangeTblEntry *) lfirst(temp);
		List		   *inhs;

		count++;
		/* Ignore non-inheritable RT entries */
		if (! rt_entry->inh)
			continue;
		/* Fast path for common case of childless table */
		if (! has_subclass(rt_entry->relid))
		{
			rt_entry->inh = false;
			continue;
		}
		/* Scan for all members of inheritance set */
		inhs = find_all_inheritors(rt_entry->relid);
		/*
		 * Check that there's at least one descendant, else treat as
		 * no-child case.  This could happen despite above has_subclass()
		 * check, if table once had a child but no longer does.
		 */
		if (lnext(inhs) == NIL)
		{
			rt_entry->inh = false;
			continue;
		}
		/* OK, found our boy */
		*rt_index = count;
		*inheritors = inhs;
		return true;
	}

	return false;
}

/*
 * new_rangetable_entry -
 *		Replaces the name and relid of 'old_entry' with the values for
 *		'new_relid'.
 *
 *		Returns a copy of 'old_entry' with the parameters substituted.
 */
static RangeTblEntry *
new_rangetable_entry(Oid new_relid, RangeTblEntry *old_entry)
{
	RangeTblEntry *new_entry = copyObject(old_entry);

	/* Replace relation real name and OID, but not the reference name */
	new_entry->relname = get_rel_name(new_relid);
	new_entry->relid = new_relid;
	return new_entry;
}

/*
 * fix_parsetree_attnums
 *	  Replaces attribute numbers from the relation represented by
 *	  'old_relid' in 'parsetree' with the attribute numbers from
 *	  'new_relid'.
 *
 * The parsetree is MODIFIED IN PLACE.	This is OK only because
 * plan_inherit_queries made a copy of the tree for us to hack upon.
 */
static void
fix_parsetree_attnums(Index rt_index,
					  Oid old_relid,
					  Oid new_relid,
					  Query *parsetree)
{
	fix_parsetree_attnums_context context;

	if (old_relid == new_relid)
		return;					/* no work needed for parent rel itself */

	context.rt_index = rt_index;
	context.old_relid = old_relid;
	context.new_relid = new_relid;
	context.sublevels_up = 0;

	query_tree_walker(parsetree,
					  fix_parsetree_attnums_walker,
					  (void *) &context,
					  true);
}

/*
 * Adjust varnos for child tables.	This routine makes it possible for
 * child tables to have different column positions for the "same" attribute
 * as a parent, which helps ALTER TABLE ADD COLUMN.  Unfortunately this isn't
 * nearly enough to make it work transparently; there are other places where
 * things fall down if children and parents don't have the same column numbers
 * for inherited attributes.  It'd be better to rip this code out and fix
 * ALTER TABLE...
 */
static bool
fix_parsetree_attnums_walker(Node *node,
							 fix_parsetree_attnums_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up &&
			var->varno == context->rt_index &&
			var->varattno > 0)
		{
			var->varattno = get_attnum(context->new_relid,
									   get_attname(context->old_relid,
												   var->varattno));
		}
		return false;
	}
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   fix_parsetree_attnums_walker,
								   (void *) context,
								   true);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, fix_parsetree_attnums_walker,
								  (void *) context);
}

static Append *
make_append(List *appendplans,
			Index rt_index,
			List *inheritrtable,
			List *tlist)
{
	Append	   *node = makeNode(Append);
	List	   *subnode;

	node->appendplans = appendplans;
	node->inheritrelid = rt_index;
	node->inheritrtable = inheritrtable;
	node->plan.startup_cost = 0;
	node->plan.total_cost = 0;
	node->plan.plan_rows = 0;
	node->plan.plan_width = 0;
	foreach(subnode, appendplans)
	{
		Plan	   *subplan = (Plan *) lfirst(subnode);

		if (subnode == appendplans)		/* first node? */
			node->plan.startup_cost = subplan->startup_cost;
		node->plan.total_cost += subplan->total_cost;
		node->plan.plan_rows += subplan->plan_rows;
		if (node->plan.plan_width < subplan->plan_width)
			node->plan.plan_width = subplan->plan_width;
	}
	node->plan.state = (EState *) NULL;
	node->plan.targetlist = tlist;
	node->plan.qual = NIL;
	node->plan.lefttree = (Plan *) NULL;
	node->plan.righttree = (Plan *) NULL;

	return node;
}
