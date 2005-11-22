/*-------------------------------------------------------------------------
 *
 * prepunion.c
 *	  Routines to plan set-operation queries.  The filename is a leftover
 *	  from a time when only UNIONs were implemented.
 *
 * There is also some code here to support planning of queries that use
 * inheritance (SELECT FROM foo*).	This no longer has much connection
 * to the processing of UNION queries, but it's still here.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/prep/prepunion.c,v 1.127.2.1 2005/11/22 18:23:11 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include "access/heapam.h"
#include "catalog/namespace.h"
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
#include "parser/parse_expr.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"


typedef struct
{
	Index		old_rt_index;
	Index		new_rt_index;
	Oid			old_rel_type;
	Oid			new_rel_type;
	TupleDesc	old_tupdesc;
	TupleDesc	new_tupdesc;
	char	   *old_rel_name;
	char	   *new_rel_name;
} adjust_inherited_attrs_context;

static Plan *recurse_set_operations(Node *setOp, PlannerInfo *root,
					   double tuple_fraction,
					   List *colTypes, bool junkOK,
					   int flag, List *refnames_tlist,
					   List **sortClauses);
static Plan *generate_union_plan(SetOperationStmt *op, PlannerInfo *root,
					double tuple_fraction,
					List *refnames_tlist, List **sortClauses);
static Plan *generate_nonunion_plan(SetOperationStmt *op, PlannerInfo *root,
					   List *refnames_tlist, List **sortClauses);
static List *recurse_union_children(Node *setOp, PlannerInfo *root,
					   double tuple_fraction,
					   SetOperationStmt *top_union,
					   List *refnames_tlist);
static List *generate_setop_tlist(List *colTypes, int flag,
					 Index varno,
					 bool hack_constants,
					 List *input_tlist,
					 List *refnames_tlist);
static List *generate_append_tlist(List *colTypes, bool flag,
					  List *input_plans,
					  List *refnames_tlist);
static bool tlist_same_datatypes(List *tlist, List *colTypes, bool junkOK);
static Node *adjust_inherited_attrs_mutator(Node *node,
							   adjust_inherited_attrs_context *context);
static Relids adjust_relid_set(Relids relids, Index oldrelid, Index newrelid);
static List *adjust_inherited_tlist(List *tlist,
					   adjust_inherited_attrs_context *context);


/*
 * plan_set_operations
 *
 *	  Plans the queries for a tree of set operations (UNION/INTERSECT/EXCEPT)
 *
 * This routine only deals with the setOperations tree of the given query.
 * Any top-level ORDER BY requested in root->parse->sortClause will be added
 * when we return to grouping_planner.
 *
 * tuple_fraction is the fraction of tuples we expect will be retrieved.
 * tuple_fraction is interpreted as for grouping_planner(); in particular,
 * zero means "all the tuples will be fetched".  Any LIMIT present at the
 * top level has already been factored into tuple_fraction.
 *
 * *sortClauses is an output argument: it is set to a list of SortClauses
 * representing the result ordering of the topmost set operation.
 */
Plan *
plan_set_operations(PlannerInfo *root, double tuple_fraction,
					List **sortClauses)
{
	Query	   *parse = root->parse;
	SetOperationStmt *topop = (SetOperationStmt *) parse->setOperations;
	Node	   *node;
	Query	   *leftmostQuery;

	Assert(topop && IsA(topop, SetOperationStmt));

	/* check for unsupported stuff */
	Assert(parse->utilityStmt == NULL);
	Assert(parse->jointree->fromlist == NIL);
	Assert(parse->jointree->quals == NULL);
	Assert(parse->groupClause == NIL);
	Assert(parse->havingQual == NULL);
	Assert(parse->distinctClause == NIL);

	/*
	 * Find the leftmost component Query.  We need to use its column names for
	 * all generated tlists (else SELECT INTO won't work right).
	 */
	node = topop->larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostQuery = rt_fetch(((RangeTblRef *) node)->rtindex,
							 parse->rtable)->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * Recurse on setOperations tree to generate plans for set ops. The final
	 * output plan should have just the column types shown as the output from
	 * the top-level node, plus possibly resjunk working columns (we can rely
	 * on upper-level nodes to deal with that).
	 */
	return recurse_set_operations((Node *) topop, root, tuple_fraction,
								  topop->colTypes, true, -1,
								  leftmostQuery->targetList,
								  sortClauses);
}

/*
 * recurse_set_operations
 *	  Recursively handle one step in a tree of set operations
 *
 * tuple_fraction: fraction of tuples we expect to retrieve from node
 * colTypes: list of type OIDs of expected output columns
 * junkOK: if true, child resjunk columns may be left in the result
 * flag: if >= 0, add a resjunk output column indicating value of flag
 * refnames_tlist: targetlist to take column names from
 * *sortClauses: receives list of SortClauses for result plan, if any
 */
static Plan *
recurse_set_operations(Node *setOp, PlannerInfo *root,
					   double tuple_fraction,
					   List *colTypes, bool junkOK,
					   int flag, List *refnames_tlist,
					   List **sortClauses)
{
	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, root->parse->rtable);
		Query	   *subquery = rte->subquery;
		Plan	   *subplan,
				   *plan;

		Assert(subquery != NULL);

		/*
		 * Generate plan for primitive subquery
		 */
		subplan = subquery_planner(subquery, tuple_fraction, NULL);

		/*
		 * Add a SubqueryScan with the caller-requested targetlist
		 */
		plan = (Plan *)
			make_subqueryscan(generate_setop_tlist(colTypes, flag,
												   rtr->rtindex,
												   true,
												   subplan->targetlist,
												   refnames_tlist),
							  NIL,
							  rtr->rtindex,
							  subplan);

		/*
		 * We don't bother to determine the subquery's output ordering since
		 * it won't be reflected in the set-op result anyhow.
		 */
		*sortClauses = NIL;

		return plan;
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;
		Plan	   *plan;

		/* UNIONs are much different from INTERSECT/EXCEPT */
		if (op->op == SETOP_UNION)
			plan = generate_union_plan(op, root, tuple_fraction,
									   refnames_tlist,
									   sortClauses);
		else
			plan = generate_nonunion_plan(op, root,
										  refnames_tlist,
										  sortClauses);

		/*
		 * If necessary, add a Result node to project the caller-requested
		 * output columns.
		 *
		 * XXX you don't really want to know about this: setrefs.c will apply
		 * replace_vars_with_subplan_refs() to the Result node's tlist. This
		 * would fail if the Vars generated by generate_setop_tlist() were not
		 * exactly equal() to the corresponding tlist entries of the subplan.
		 * However, since the subplan was generated by generate_union_plan()
		 * or generate_nonunion_plan(), and hence its tlist was generated by
		 * generate_append_tlist(), this will work.  We just tell
		 * generate_setop_tlist() to use varno 0.
		 */
		if (flag >= 0 ||
			!tlist_same_datatypes(plan->targetlist, colTypes, junkOK))
		{
			plan = (Plan *)
				make_result(generate_setop_tlist(colTypes, flag,
												 0,
												 false,
												 plan->targetlist,
												 refnames_tlist),
							NULL,
							plan);
		}
		return plan;
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
		return NULL;			/* keep compiler quiet */
	}
}

/*
 * Generate plan for a UNION or UNION ALL node
 */
static Plan *
generate_union_plan(SetOperationStmt *op, PlannerInfo *root,
					double tuple_fraction,
					List *refnames_tlist,
					List **sortClauses)
{
	List	   *planlist;
	List	   *tlist;
	Plan	   *plan;

	/*
	 * If plain UNION, tell children to fetch all tuples.
	 *
	 * Note: in UNION ALL, we pass the top-level tuple_fraction unmodified to
	 * each arm of the UNION ALL.  One could make a case for reducing the
	 * tuple fraction for later arms (discounting by the expected size of the
	 * earlier arms' results) but it seems not worth the trouble. The normal
	 * case where tuple_fraction isn't already zero is a LIMIT at top level,
	 * and passing it down as-is is usually enough to get the desired result
	 * of preferring fast-start plans.
	 */
	if (!op->all)
		tuple_fraction = 0.0;

	/*
	 * If any of my children are identical UNION nodes (same op, all-flag, and
	 * colTypes) then they can be merged into this node so that we generate
	 * only one Append and Sort for the lot.  Recurse to find such nodes and
	 * compute their children's plans.
	 */
	planlist = list_concat(recurse_union_children(op->larg, root,
												  tuple_fraction,
												  op, refnames_tlist),
						   recurse_union_children(op->rarg, root,
												  tuple_fraction,
												  op, refnames_tlist));

	/*
	 * Generate tlist for Append plan node.
	 *
	 * The tlist for an Append plan isn't important as far as the Append is
	 * concerned, but we must make it look real anyway for the benefit of the
	 * next plan level up.
	 */
	tlist = generate_append_tlist(op->colTypes, false,
								  planlist, refnames_tlist);

	/*
	 * Append the child results together.
	 */
	plan = (Plan *) make_append(planlist, false, tlist);

	/*
	 * For UNION ALL, we just need the Append plan.  For UNION, need to add
	 * Sort and Unique nodes to produce unique output.
	 */
	if (!op->all)
	{
		List	   *sortList;

		sortList = addAllTargetsToSortList(NULL, NIL, tlist, false);
		if (sortList)
		{
			plan = (Plan *) make_sort_from_sortclauses(root, sortList, plan);
			plan = (Plan *) make_unique(plan, sortList);
		}
		*sortClauses = sortList;
	}
	else
		*sortClauses = NIL;

	return plan;
}

/*
 * Generate plan for an INTERSECT, INTERSECT ALL, EXCEPT, or EXCEPT ALL node
 */
static Plan *
generate_nonunion_plan(SetOperationStmt *op, PlannerInfo *root,
					   List *refnames_tlist,
					   List **sortClauses)
{
	Plan	   *lplan,
			   *rplan,
			   *plan;
	List	   *tlist,
			   *sortList,
			   *planlist,
			   *child_sortclauses;
	SetOpCmd	cmd;

	/* Recurse on children, ensuring their outputs are marked */
	lplan = recurse_set_operations(op->larg, root,
								   0.0 /* all tuples needed */ ,
								   op->colTypes, false, 0,
								   refnames_tlist,
								   &child_sortclauses);
	rplan = recurse_set_operations(op->rarg, root,
								   0.0 /* all tuples needed */ ,
								   op->colTypes, false, 1,
								   refnames_tlist,
								   &child_sortclauses);
	planlist = list_make2(lplan, rplan);

	/*
	 * Generate tlist for Append plan node.
	 *
	 * The tlist for an Append plan isn't important as far as the Append is
	 * concerned, but we must make it look real anyway for the benefit of the
	 * next plan level up.	In fact, it has to be real enough that the flag
	 * column is shown as a variable not a constant, else setrefs.c will get
	 * confused.
	 */
	tlist = generate_append_tlist(op->colTypes, true,
								  planlist, refnames_tlist);

	/*
	 * Append the child results together.
	 */
	plan = (Plan *) make_append(planlist, false, tlist);

	/*
	 * Sort the child results, then add a SetOp plan node to generate the
	 * correct output.
	 */
	sortList = addAllTargetsToSortList(NULL, NIL, tlist, false);

	if (sortList == NIL)		/* nothing to sort on? */
	{
		*sortClauses = NIL;
		return plan;
	}

	plan = (Plan *) make_sort_from_sortclauses(root, sortList, plan);
	switch (op->op)
	{
		case SETOP_INTERSECT:
			cmd = op->all ? SETOPCMD_INTERSECT_ALL : SETOPCMD_INTERSECT;
			break;
		case SETOP_EXCEPT:
			cmd = op->all ? SETOPCMD_EXCEPT_ALL : SETOPCMD_EXCEPT;
			break;
		default:
			elog(ERROR, "unrecognized set op: %d",
				 (int) op->op);
			cmd = SETOPCMD_INTERSECT;	/* keep compiler quiet */
			break;
	}
	plan = (Plan *) make_setop(cmd, plan, sortList, list_length(op->colTypes) + 1);

	*sortClauses = sortList;

	return plan;
}

/*
 * Pull up children of a UNION node that are identically-propertied UNIONs.
 *
 * NOTE: we can also pull a UNION ALL up into a UNION, since the distinct
 * output rows will be lost anyway.
 */
static List *
recurse_union_children(Node *setOp, PlannerInfo *root,
					   double tuple_fraction,
					   SetOperationStmt *top_union,
					   List *refnames_tlist)
{
	List	   *child_sortclauses;

	if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		if (op->op == top_union->op &&
			(op->all == top_union->all || op->all) &&
			equal(op->colTypes, top_union->colTypes))
		{
			/* Same UNION, so fold children into parent's subplan list */
			return list_concat(recurse_union_children(op->larg, root,
													  tuple_fraction,
													  top_union,
													  refnames_tlist),
							   recurse_union_children(op->rarg, root,
													  tuple_fraction,
													  top_union,
													  refnames_tlist));
		}
	}

	/*
	 * Not same, so plan this child separately.
	 *
	 * Note we disallow any resjunk columns in child results.  This is
	 * necessary since the Append node that implements the union won't do any
	 * projection, and upper levels will get confused if some of our output
	 * tuples have junk and some don't.  This case only arises when we have an
	 * EXCEPT or INTERSECT as child, else there won't be resjunk anyway.
	 */
	return list_make1(recurse_set_operations(setOp, root,
											 tuple_fraction,
											 top_union->colTypes, false,
											 -1, refnames_tlist,
											 &child_sortclauses));
}

/*
 * Generate targetlist for a set-operation plan node
 *
 * colTypes: column datatypes for non-junk columns
 * flag: -1 if no flag column needed, 0 or 1 to create a const flag column
 * varno: varno to use in generated Vars
 * hack_constants: true to copy up constants (see comments in code)
 * input_tlist: targetlist of this node's input node
 * refnames_tlist: targetlist to take column names from
 */
static List *
generate_setop_tlist(List *colTypes, int flag,
					 Index varno,
					 bool hack_constants,
					 List *input_tlist,
					 List *refnames_tlist)
{
	List	   *tlist = NIL;
	int			resno = 1;
	ListCell   *i,
			   *j,
			   *k;
	TargetEntry *tle;
	Node	   *expr;

	j = list_head(input_tlist);
	k = list_head(refnames_tlist);
	foreach(i, colTypes)
	{
		Oid			colType = lfirst_oid(i);
		TargetEntry *inputtle = (TargetEntry *) lfirst(j);
		TargetEntry *reftle = (TargetEntry *) lfirst(k);

		Assert(inputtle->resno == resno);
		Assert(reftle->resno == resno);
		Assert(!inputtle->resjunk);
		Assert(!reftle->resjunk);

		/*
		 * Generate columns referencing input columns and having appropriate
		 * data types and column names.  Insert datatype coercions where
		 * necessary.
		 *
		 * HACK: constants in the input's targetlist are copied up as-is
		 * rather than being referenced as subquery outputs.  This is mainly
		 * to ensure that when we try to coerce them to the output column's
		 * datatype, the right things happen for UNKNOWN constants.  But do
		 * this only at the first level of subquery-scan plans; we don't want
		 * phony constants appearing in the output tlists of upper-level
		 * nodes!
		 */
		if (hack_constants && inputtle->expr && IsA(inputtle->expr, Const))
			expr = (Node *) inputtle->expr;
		else
			expr = (Node *) makeVar(varno,
									inputtle->resno,
									exprType((Node *) inputtle->expr),
									exprTypmod((Node *) inputtle->expr),
									0);
		if (exprType(expr) != colType)
		{
			expr = coerce_to_common_type(NULL,	/* no UNKNOWNs here */
										 expr,
										 colType,
										 "UNION/INTERSECT/EXCEPT");
		}
		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup(reftle->resname),
							  false);
		tlist = lappend(tlist, tle);

		j = lnext(j);
		k = lnext(k);
	}

	if (flag >= 0)
	{
		/* Add a resjunk flag column */
		/* flag value is the given constant */
		expr = (Node *) makeConst(INT4OID,
								  sizeof(int4),
								  Int32GetDatum(flag),
								  false,
								  true);
		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup("flag"),
							  true);
		tlist = lappend(tlist, tle);
	}

	return tlist;
}

/*
 * Generate targetlist for a set-operation Append node
 *
 * colTypes: column datatypes for non-junk columns
 * flag: true to create a flag column copied up from subplans
 * input_plans: list of sub-plans of the Append
 * refnames_tlist: targetlist to take column names from
 *
 * The entries in the Append's targetlist should always be simple Vars;
 * we just have to make sure they have the right datatypes and typmods.
 * The Vars are always generated with varno 0.
 */
static List *
generate_append_tlist(List *colTypes, bool flag,
					  List *input_plans,
					  List *refnames_tlist)
{
	List	   *tlist = NIL;
	int			resno = 1;
	ListCell   *curColType;
	ListCell   *ref_tl_item;
	int			colindex;
	TargetEntry *tle;
	Node	   *expr;
	ListCell   *planl;
	int32	   *colTypmods;

	/*
	 * First extract typmods to use.
	 *
	 * If the inputs all agree on type and typmod of a particular column, use
	 * that typmod; else use -1.
	 */
	colTypmods = (int32 *) palloc(list_length(colTypes) * sizeof(int32));

	foreach(planl, input_plans)
	{
		Plan	   *subplan = (Plan *) lfirst(planl);
		ListCell   *subtlist;

		curColType = list_head(colTypes);
		colindex = 0;
		foreach(subtlist, subplan->targetlist)
		{
			TargetEntry *subtle = (TargetEntry *) lfirst(subtlist);

			if (subtle->resjunk)
				continue;
			Assert(curColType != NULL);
			if (exprType((Node *) subtle->expr) == lfirst_oid(curColType))
			{
				/* If first subplan, copy the typmod; else compare */
				int32		subtypmod = exprTypmod((Node *) subtle->expr);

				if (planl == list_head(input_plans))
					colTypmods[colindex] = subtypmod;
				else if (subtypmod != colTypmods[colindex])
					colTypmods[colindex] = -1;
			}
			else
			{
				/* types disagree, so force typmod to -1 */
				colTypmods[colindex] = -1;
			}
			curColType = lnext(curColType);
			colindex++;
		}
		Assert(curColType == NULL);
	}

	/*
	 * Now we can build the tlist for the Append.
	 */
	colindex = 0;
	forboth(curColType, colTypes, ref_tl_item, refnames_tlist)
	{
		Oid			colType = lfirst_oid(curColType);
		int32		colTypmod = colTypmods[colindex++];
		TargetEntry *reftle = (TargetEntry *) lfirst(ref_tl_item);

		Assert(reftle->resno == resno);
		Assert(!reftle->resjunk);
		expr = (Node *) makeVar(0,
								resno,
								colType,
								colTypmod,
								0);
		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup(reftle->resname),
							  false);
		tlist = lappend(tlist, tle);
	}

	if (flag)
	{
		/* Add a resjunk flag column */
		/* flag value is shown as copied up from subplan */
		expr = (Node *) makeVar(0,
								resno,
								INT4OID,
								-1,
								0);
		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup("flag"),
							  true);
		tlist = lappend(tlist, tle);
	}

	pfree(colTypmods);

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
	ListCell   *l;
	ListCell   *curColType = list_head(colTypes);

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk)
		{
			if (!junkOK)
				return false;
		}
		else
		{
			if (curColType == NULL)
				return false;
			if (exprType((Node *) tle->expr) != lfirst_oid(curColType))
				return false;
			curColType = lnext(curColType);
		}
	}
	if (curColType != NULL)
		return false;
	return true;
}


/*
 * find_all_inheritors -
 *		Returns a list of relation OIDs including the given rel plus
 *		all relations that inherit from it, directly or indirectly.
 */
List *
find_all_inheritors(Oid parentrel)
{
	List	   *rels_list;
	ListCell   *l;

	/*
	 * We build a list starting with the given rel and adding all direct and
	 * indirect children.  We can use a single list as both the record of
	 * already-found rels and the agenda of rels yet to be scanned for more
	 * children.  This is a bit tricky but works because the foreach() macro
	 * doesn't fetch the next list element until the bottom of the loop.
	 */
	rels_list = list_make1_oid(parentrel);

	foreach(l, rels_list)
	{
		Oid			currentrel = lfirst_oid(l);
		List	   *currentchildren;

		/* Get the direct children of this rel */
		currentchildren = find_inheritance_children(currentrel);

		/*
		 * Add to the queue only those children not already seen. This avoids
		 * making duplicate entries in case of multiple inheritance paths from
		 * the same parent.  (It'll also keep us from getting into an infinite
		 * loop, though theoretically there can't be any cycles in the
		 * inheritance graph anyway.)
		 */
		rels_list = list_concat_unique_oid(rels_list, currentchildren);
	}

	return rels_list;
}

/*
 * expand_inherited_rtentry
 *		Check whether a rangetable entry represents an inheritance set.
 *		If so, add entries for all the child tables to the query's
 *		rangetable, and return an integer list of RT indexes for the
 *		whole inheritance set (parent and children).
 *		If not, return NIL.
 *
 * Note that the original RTE is considered to represent the whole
 * inheritance set.  The first member of the returned list is an RTE
 * for the same table, but with inh = false, to represent the parent table
 * in its role as a simple member of the set.  The original RT index is
 * never a member of the returned list.
 *
 * A childless table is never considered to be an inheritance set; therefore
 * the result will never be a one-element list.  It'll be either empty
 * or have two or more elements.
 *
 * Note: there are cases in which this routine will be invoked multiple
 * times on the same RTE.  We will generate a separate set of child RTEs
 * for each invocation.  This is somewhat wasteful but seems not worth
 * trying to avoid.
 */
List *
expand_inherited_rtentry(PlannerInfo *root, Index rti)
{
	Query	   *parse = root->parse;
	RangeTblEntry *rte = rt_fetch(rti, parse->rtable);
	Oid			parentOID;
	List	   *inhOIDs;
	List	   *inhRTIs;
	ListCell   *l;

	/* Does RT entry allow inheritance? */
	if (!rte->inh)
		return NIL;
	Assert(rte->rtekind == RTE_RELATION);
	/* Fast path for common case of childless table */
	parentOID = rte->relid;
	if (!has_subclass(parentOID))
	{
		/* Clear flag to save repeated tests if called again */
		rte->inh = false;
		return NIL;
	}
	/* Scan for all members of inheritance set */
	inhOIDs = find_all_inheritors(parentOID);

	/*
	 * Check that there's at least one descendant, else treat as no-child
	 * case.  This could happen despite above has_subclass() check, if table
	 * once had a child but no longer does.
	 */
	if (list_length(inhOIDs) < 2)
	{
		/* Clear flag to save repeated tests if called again */
		rte->inh = false;
		return NIL;
	}

	/* OK, it's an inheritance set; expand it */
	inhRTIs = NIL;
	foreach(l, inhOIDs)
	{
		Oid			childOID = lfirst_oid(l);
		RangeTblEntry *childrte;
		Index		childRTindex;

		/*
		 * It is possible that the parent table has children that are temp
		 * tables of other backends.  We cannot safely access such tables
		 * (because of buffering issues), and the best thing to do seems to be
		 * to silently ignore them.
		 */
		if (childOID != parentOID &&
			isOtherTempNamespace(get_rel_namespace(childOID)))
			continue;

		/*
		 * Build an RTE for the child, and attach to query's rangetable list.
		 * We copy most fields of the parent's RTE, but replace relation OID,
		 * and set inh = false.
		 */
		childrte = copyObject(rte);
		childrte->relid = childOID;
		childrte->inh = false;
		parse->rtable = lappend(parse->rtable, childrte);
		childRTindex = list_length(parse->rtable);
		inhRTIs = lappend_int(inhRTIs, childRTindex);
	}

	/*
	 * If all the children were temp tables, pretend it's a non-inheritance
	 * situation.  The duplicate RTE we added for the parent table is
	 * harmless.
	 */
	if (list_length(inhRTIs) < 2)
	{
		/* Clear flag to save repeated tests if called again */
		rte->inh = false;
		return NIL;
	}

	/*
	 * The executor will check the parent table's access permissions when it
	 * examines the parent's inheritlist entry.  There's no need to check
	 * twice, so turn off access check bits in the original RTE. (If we are
	 * invoked more than once, extra copies of the child RTEs will also not
	 * cause duplicate permission checks.)
	 */
	rte->requiredPerms = 0;

	return inhRTIs;
}

/*
 * adjust_inherited_attrs
 *	  Copy the specified query or expression and translate Vars referring
 *	  to old_rt_index to refer to new_rt_index.
 *
 * We also adjust varattno to match the new table by column name, rather
 * than column number.	This hack makes it possible for child tables to have
 * different column positions for the "same" attribute as a parent, which
 * is necessary for ALTER TABLE ADD COLUMN.
 */
Node *
adjust_inherited_attrs(Node *node,
					   Index old_rt_index, Oid old_relid,
					   Index new_rt_index, Oid new_relid)
{
	Node	   *result;
	adjust_inherited_attrs_context context;
	Relation	oldrelation;
	Relation	newrelation;

	/* Handle simple case simply... */
	if (old_rt_index == new_rt_index)
	{
		Assert(old_relid == new_relid);
		return copyObject(node);
	}

	/*
	 * We assume that by now the planner has acquired at least AccessShareLock
	 * on both rels, and so we need no additional lock now.
	 */
	oldrelation = heap_open(old_relid, NoLock);
	newrelation = heap_open(new_relid, NoLock);

	context.old_rt_index = old_rt_index;
	context.new_rt_index = new_rt_index;
	context.old_rel_type = oldrelation->rd_rel->reltype;
	context.new_rel_type = newrelation->rd_rel->reltype;
	context.old_tupdesc = RelationGetDescr(oldrelation);
	context.new_tupdesc = RelationGetDescr(newrelation);
	context.old_rel_name = RelationGetRelationName(oldrelation);
	context.new_rel_name = RelationGetRelationName(newrelation);

	/*
	 * Must be prepared to start with a Query or a bare expression tree.
	 */
	if (node && IsA(node, Query))
	{
		Query	   *newnode;

		newnode = query_tree_mutator((Query *) node,
									 adjust_inherited_attrs_mutator,
									 (void *) &context,
									 QTW_IGNORE_RT_SUBQUERIES);
		if (newnode->resultRelation == old_rt_index)
		{
			newnode->resultRelation = new_rt_index;
			/* Fix tlist resnos too, if it's inherited UPDATE */
			if (newnode->commandType == CMD_UPDATE)
				newnode->targetList =
					adjust_inherited_tlist(newnode->targetList,
										   &context);
		}
		result = (Node *) newnode;
	}
	else
		result = adjust_inherited_attrs_mutator(node, &context);

	heap_close(oldrelation, NoLock);
	heap_close(newrelation, NoLock);

	return result;
}

/*
 * Translate parent's attribute number into child's.
 *
 * For paranoia's sake, we match type as well as attribute name.
 */
static AttrNumber
translate_inherited_attnum(AttrNumber old_attno,
						   adjust_inherited_attrs_context *context)
{
	Form_pg_attribute att;
	char	   *attname;
	Oid			atttypid;
	int32		atttypmod;
	int			newnatts;
	int			i;

	if (old_attno <= 0 || old_attno > context->old_tupdesc->natts)
		elog(ERROR, "attribute %d of relation \"%s\" does not exist",
			 (int) old_attno, context->old_rel_name);
	att = context->old_tupdesc->attrs[old_attno - 1];
	if (att->attisdropped)
		elog(ERROR, "attribute %d of relation \"%s\" does not exist",
			 (int) old_attno, context->old_rel_name);
	attname = NameStr(att->attname);
	atttypid = att->atttypid;
	atttypmod = att->atttypmod;

	newnatts = context->new_tupdesc->natts;
	for (i = 0; i < newnatts; i++)
	{
		att = context->new_tupdesc->attrs[i];
		if (att->attisdropped)
			continue;
		if (strcmp(attname, NameStr(att->attname)) == 0)
		{
			/* Found it, check type */
			if (atttypid != att->atttypid || atttypmod != att->atttypmod)
				elog(ERROR, "attribute \"%s\" of relation \"%s\" does not match parent's type",
					 attname, context->new_rel_name);
			return (AttrNumber) (i + 1);
		}
	}

	elog(ERROR, "attribute \"%s\" of relation \"%s\" does not exist",
		 attname, context->new_rel_name);
	return 0;					/* keep compiler quiet */
}

static Node *
adjust_inherited_attrs_mutator(Node *node,
							   adjust_inherited_attrs_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) copyObject(node);

		if (var->varlevelsup == 0 &&
			var->varno == context->old_rt_index)
		{
			var->varno = context->new_rt_index;
			var->varnoold = context->new_rt_index;
			if (var->varattno > 0)
			{
				var->varattno = translate_inherited_attnum(var->varattno,
														   context);
				var->varoattno = var->varattno;
			}
			else if (var->varattno == 0)
			{
				/*
				 * Whole-row Var: we need to insert a coercion step to convert
				 * the tuple layout to the parent's rowtype.
				 */
				if (context->old_rel_type != context->new_rel_type)
				{
					ConvertRowtypeExpr *r = makeNode(ConvertRowtypeExpr);

					r->arg = (Expr *) var;
					r->resulttype = context->old_rel_type;
					r->convertformat = COERCE_IMPLICIT_CAST;
					/* Make sure the Var node has the right type ID, too */
					Assert(var->vartype == context->old_rel_type);
					var->vartype = context->new_rel_type;
					return (Node *) r;
				}
			}
			/* system attributes don't need any translation */
		}
		return (Node *) var;
	}
	if (IsA(node, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) copyObject(node);

		if (rtr->rtindex == context->old_rt_index)
			rtr->rtindex = context->new_rt_index;
		return (Node *) rtr;
	}
	if (IsA(node, JoinExpr))
	{
		/* Copy the JoinExpr node with correct mutation of subnodes */
		JoinExpr   *j;

		j = (JoinExpr *) expression_tree_mutator(node,
											  adjust_inherited_attrs_mutator,
												 (void *) context);
		/* now fix JoinExpr's rtindex */
		if (j->rtindex == context->old_rt_index)
			j->rtindex = context->new_rt_index;
		return (Node *) j;
	}
	if (IsA(node, InClauseInfo))
	{
		/* Copy the InClauseInfo node with correct mutation of subnodes */
		InClauseInfo *ininfo;

		ininfo = (InClauseInfo *) expression_tree_mutator(node,
											  adjust_inherited_attrs_mutator,
														  (void *) context);
		/* now fix InClauseInfo's relid sets */
		ininfo->lefthand = adjust_relid_set(ininfo->lefthand,
											context->old_rt_index,
											context->new_rt_index);
		ininfo->righthand = adjust_relid_set(ininfo->righthand,
											 context->old_rt_index,
											 context->new_rt_index);
		return (Node *) ininfo;
	}

	/*
	 * We have to process RestrictInfo nodes specially.
	 */
	if (IsA(node, RestrictInfo))
	{
		RestrictInfo *oldinfo = (RestrictInfo *) node;
		RestrictInfo *newinfo = makeNode(RestrictInfo);

		/* Copy all flat-copiable fields */
		memcpy(newinfo, oldinfo, sizeof(RestrictInfo));

		/* Recursively fix the clause itself */
		newinfo->clause = (Expr *)
			adjust_inherited_attrs_mutator((Node *) oldinfo->clause, context);

		/* and the modified version, if an OR clause */
		newinfo->orclause = (Expr *)
			adjust_inherited_attrs_mutator((Node *) oldinfo->orclause, context);

		/* adjust relid sets too */
		newinfo->clause_relids = adjust_relid_set(oldinfo->clause_relids,
												  context->old_rt_index,
												  context->new_rt_index);
		newinfo->required_relids = adjust_relid_set(oldinfo->required_relids,
													context->old_rt_index,
													context->new_rt_index);
		newinfo->left_relids = adjust_relid_set(oldinfo->left_relids,
												context->old_rt_index,
												context->new_rt_index);
		newinfo->right_relids = adjust_relid_set(oldinfo->right_relids,
												 context->old_rt_index,
												 context->new_rt_index);

		/*
		 * Reset cached derivative fields, since these might need to have
		 * different values when considering the child relation.
		 */
		newinfo->eval_cost.startup = -1;
		newinfo->this_selec = -1;
		newinfo->left_pathkey = NIL;
		newinfo->right_pathkey = NIL;
		newinfo->left_mergescansel = -1;
		newinfo->right_mergescansel = -1;
		newinfo->left_bucketsize = -1;
		newinfo->right_bucketsize = -1;

		return (Node *) newinfo;
	}

	/*
	 * NOTE: we do not need to recurse into sublinks, because they should
	 * already have been converted to subplans before we see them.
	 */
	Assert(!IsA(node, SubLink));
	Assert(!IsA(node, Query));

	/*
	 * BUT: although we don't need to recurse into subplans, we do need to
	 * make sure that they are copied, not just referenced as
	 * expression_tree_mutator will do by default.	Otherwise we'll have the
	 * same subplan node referenced from each arm of the inheritance APPEND
	 * plan, which will cause trouble in the executor.	This is a kluge that
	 * should go away when we redesign querytrees.
	 */
	if (is_subplan(node))
	{
		SubPlan    *subplan;

		/* Copy the node and process subplan args */
		node = expression_tree_mutator(node, adjust_inherited_attrs_mutator,
									   (void *) context);
		/* Make sure we have separate copies of subplan and its rtable */
		subplan = (SubPlan *) node;
		subplan->plan = copyObject(subplan->plan);
		subplan->rtable = copyObject(subplan->rtable);
		return node;
	}

	return expression_tree_mutator(node, adjust_inherited_attrs_mutator,
								   (void *) context);
}

/*
 * Substitute newrelid for oldrelid in a Relid set
 */
static Relids
adjust_relid_set(Relids relids, Index oldrelid, Index newrelid)
{
	if (bms_is_member(oldrelid, relids))
	{
		/* Ensure we have a modifiable copy */
		relids = bms_copy(relids);
		/* Remove old, add new */
		relids = bms_del_member(relids, oldrelid);
		relids = bms_add_member(relids, newrelid);
	}
	return relids;
}

/*
 * Adjust the targetlist entries of an inherited UPDATE operation
 *
 * The expressions have already been fixed, but we have to make sure that
 * the target resnos match the child table (they may not, in the case of
 * a column that was added after-the-fact by ALTER TABLE).	In some cases
 * this can force us to re-order the tlist to preserve resno ordering.
 * (We do all this work in special cases so that preptlist.c is fast for
 * the typical case.)
 *
 * The given tlist has already been through expression_tree_mutator;
 * therefore the TargetEntry nodes are fresh copies that it's okay to
 * scribble on.
 *
 * Note that this is not needed for INSERT because INSERT isn't inheritable.
 */
static List *
adjust_inherited_tlist(List *tlist,
					   adjust_inherited_attrs_context *context)
{
	bool		changed_it = false;
	ListCell   *tl;
	List	   *new_tlist;
	bool		more;
	int			attrno;

	/* Scan tlist and update resnos to match attnums of new_relid */
	foreach(tl, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);

		if (tle->resjunk)
			continue;			/* ignore junk items */

		attrno = translate_inherited_attnum(tle->resno, context);

		if (tle->resno != attrno)
		{
			tle->resno = attrno;
			changed_it = true;
		}
	}

	/*
	 * If we changed anything, re-sort the tlist by resno, and make sure
	 * resjunk entries have resnos above the last real resno.  The sort
	 * algorithm is a bit stupid, but for such a seldom-taken path, small is
	 * probably better than fast.
	 */
	if (!changed_it)
		return tlist;

	new_tlist = NIL;
	more = true;
	for (attrno = 1; more; attrno++)
	{
		more = false;
		foreach(tl, tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tl);

			if (tle->resjunk)
				continue;		/* ignore junk items */

			if (tle->resno == attrno)
				new_tlist = lappend(new_tlist, tle);
			else if (tle->resno > attrno)
				more = true;
		}
	}

	foreach(tl, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);

		if (!tle->resjunk)
			continue;			/* here, ignore non-junk items */

		tle->resno = attrno;
		new_tlist = lappend(new_tlist, tle);
		attrno++;
	}

	return new_tlist;
}
