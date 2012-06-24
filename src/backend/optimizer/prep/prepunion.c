/*-------------------------------------------------------------------------
 *
 * prepunion.c
 *	  Routines to plan set-operation queries.  The filename is a leftover
 *	  from a time when only UNIONs were implemented.
 *
 * There are two code paths in the planner for set-operation queries.
 * If a subquery consists entirely of simple UNION ALL operations, it
 * is converted into an "append relation".	Otherwise, it is handled
 * by the general code in this module (plan_set_operations and its
 * subroutines).  There is some support code here for the append-relation
 * case, but most of the heavy lifting for that is done elsewhere,
 * notably in prepjointree.c and allpaths.c.
 *
 * There is also some code here to support planning of queries that use
 * inheritance (SELECT FROM foo*).	Inheritance trees are converted into
 * append relations, and thenceforth share code with the UNION ALL case.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/prepunion.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include "access/heapam.h"
#include "access/sysattr.h"
#include "catalog/pg_inherits_fn.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"


typedef struct
{
	PlannerInfo *root;
	AppendRelInfo *appinfo;
} adjust_appendrel_attrs_context;

static Plan *recurse_set_operations(Node *setOp, PlannerInfo *root,
					   double tuple_fraction,
					   List *colTypes, List *colCollations,
					   bool junkOK,
					   int flag, List *refnames_tlist,
					   List **sortClauses, double *pNumGroups);
static Plan *generate_recursion_plan(SetOperationStmt *setOp,
						PlannerInfo *root, double tuple_fraction,
						List *refnames_tlist,
						List **sortClauses);
static Plan *generate_union_plan(SetOperationStmt *op, PlannerInfo *root,
					double tuple_fraction,
					List *refnames_tlist,
					List **sortClauses, double *pNumGroups);
static Plan *generate_nonunion_plan(SetOperationStmt *op, PlannerInfo *root,
					   double tuple_fraction,
					   List *refnames_tlist,
					   List **sortClauses, double *pNumGroups);
static List *recurse_union_children(Node *setOp, PlannerInfo *root,
					   double tuple_fraction,
					   SetOperationStmt *top_union,
					   List *refnames_tlist);
static Plan *make_union_unique(SetOperationStmt *op, Plan *plan,
				  PlannerInfo *root, double tuple_fraction,
				  List **sortClauses);
static bool choose_hashed_setop(PlannerInfo *root, List *groupClauses,
					Plan *input_plan,
					double dNumGroups, double dNumOutputRows,
					double tuple_fraction,
					const char *construct);
static List *generate_setop_tlist(List *colTypes, List *colCollations,
					 int flag,
					 Index varno,
					 bool hack_constants,
					 List *input_tlist,
					 List *refnames_tlist);
static List *generate_append_tlist(List *colTypes, List *colCollations,
					  bool flag,
					  List *input_plans,
					  List *refnames_tlist);
static List *generate_setop_grouplist(SetOperationStmt *op, List *targetlist);
static void expand_inherited_rtentry(PlannerInfo *root, RangeTblEntry *rte,
						 Index rti);
static void make_inh_translation_list(Relation oldrelation,
						  Relation newrelation,
						  Index newvarno,
						  List **translated_vars);
static Bitmapset *translate_col_privs(const Bitmapset *parent_privs,
					List *translated_vars);
static Node *adjust_appendrel_attrs_mutator(Node *node,
							   adjust_appendrel_attrs_context *context);
static Relids adjust_relid_set(Relids relids, Index oldrelid, Index newrelid);
static List *adjust_inherited_tlist(List *tlist,
					   AppendRelInfo *context);


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
 * *sortClauses is an output argument: it is set to a list of SortGroupClauses
 * representing the result ordering of the topmost set operation.  (This will
 * be NIL if the output isn't ordered.)
 */
Plan *
plan_set_operations(PlannerInfo *root, double tuple_fraction,
					List **sortClauses)
{
	Query	   *parse = root->parse;
	SetOperationStmt *topop = (SetOperationStmt *) parse->setOperations;
	Node	   *node;
	RangeTblEntry *leftmostRTE;
	Query	   *leftmostQuery;

	Assert(topop && IsA(topop, SetOperationStmt));

	/* check for unsupported stuff */
	Assert(parse->jointree->fromlist == NIL);
	Assert(parse->jointree->quals == NULL);
	Assert(parse->groupClause == NIL);
	Assert(parse->havingQual == NULL);
	Assert(parse->windowClause == NIL);
	Assert(parse->distinctClause == NIL);

	/*
	 * We'll need to build RelOptInfos for each of the leaf subqueries, which
	 * are RTE_SUBQUERY rangetable entries in this Query.  Prepare the index
	 * arrays for that.
	 */
	setup_simple_rel_arrays(root);

	/*
	 * Find the leftmost component Query.  We need to use its column names for
	 * all generated tlists (else SELECT INTO won't work right).
	 */
	node = topop->larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTE = root->simple_rte_array[((RangeTblRef *) node)->rtindex];
	leftmostQuery = leftmostRTE->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * If the topmost node is a recursive union, it needs special processing.
	 */
	if (root->hasRecursion)
		return generate_recursion_plan(topop, root, tuple_fraction,
									   leftmostQuery->targetList,
									   sortClauses);

	/*
	 * Recurse on setOperations tree to generate plans for set ops. The final
	 * output plan should have just the column types shown as the output from
	 * the top-level node, plus possibly resjunk working columns (we can rely
	 * on upper-level nodes to deal with that).
	 */
	return recurse_set_operations((Node *) topop, root, tuple_fraction,
								  topop->colTypes, topop->colCollations,
								  true, -1,
								  leftmostQuery->targetList,
								  sortClauses, NULL);
}

/*
 * recurse_set_operations
 *	  Recursively handle one step in a tree of set operations
 *
 * tuple_fraction: fraction of tuples we expect to retrieve from node
 * colTypes: OID list of set-op's result column datatypes
 * colCollations: OID list of set-op's result column collations
 * junkOK: if true, child resjunk columns may be left in the result
 * flag: if >= 0, add a resjunk output column indicating value of flag
 * refnames_tlist: targetlist to take column names from
 *
 * Returns a plan for the subtree, as well as these output parameters:
 * *sortClauses: receives list of SortGroupClauses for result plan, if any
 * *pNumGroups: if not NULL, we estimate the number of distinct groups
 *		in the result, and store it there
 *
 * We don't have to care about typmods here: the only allowed difference
 * between set-op input and output typmods is input is a specific typmod
 * and output is -1, and that does not require a coercion.
 */
static Plan *
recurse_set_operations(Node *setOp, PlannerInfo *root,
					   double tuple_fraction,
					   List *colTypes, List *colCollations,
					   bool junkOK,
					   int flag, List *refnames_tlist,
					   List **sortClauses, double *pNumGroups)
{
	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = root->simple_rte_array[rtr->rtindex];
		Query	   *subquery = rte->subquery;
		RelOptInfo *rel;
		PlannerInfo *subroot;
		Plan	   *subplan,
				   *plan;

		Assert(subquery != NULL);

		/*
		 * We need to build a RelOptInfo for each leaf subquery.  This isn't
		 * used for anything here, but it carries the subroot data structures
		 * forward to setrefs.c processing.
		 */
		rel = build_simple_rel(root, rtr->rtindex, RELOPT_BASEREL);

		/*
		 * Generate plan for primitive subquery
		 */
		subplan = subquery_planner(root->glob, subquery,
								   root,
								   false, tuple_fraction,
								   &subroot);

		/* Save subroot and subplan in RelOptInfo for setrefs.c */
		rel->subplan = subplan;
		rel->subroot = subroot;

		/*
		 * Estimate number of groups if caller wants it.  If the subquery used
		 * grouping or aggregation, its output is probably mostly unique
		 * anyway; otherwise do statistical estimation.
		 */
		if (pNumGroups)
		{
			if (subquery->groupClause || subquery->distinctClause ||
				subroot->hasHavingQual || subquery->hasAggs)
				*pNumGroups = subplan->plan_rows;
			else
				*pNumGroups = estimate_num_groups(subroot,
								get_tlist_exprs(subquery->targetList, false),
												  subplan->plan_rows);
		}

		/*
		 * Add a SubqueryScan with the caller-requested targetlist
		 */
		plan = (Plan *)
			make_subqueryscan(generate_setop_tlist(colTypes, colCollations,
												   flag,
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
									   sortClauses, pNumGroups);
		else
			plan = generate_nonunion_plan(op, root, tuple_fraction,
										  refnames_tlist,
										  sortClauses, pNumGroups);

		/*
		 * If necessary, add a Result node to project the caller-requested
		 * output columns.
		 *
		 * XXX you don't really want to know about this: setrefs.c will apply
		 * fix_upper_expr() to the Result node's tlist. This would fail if the
		 * Vars generated by generate_setop_tlist() were not exactly equal()
		 * to the corresponding tlist entries of the subplan. However, since
		 * the subplan was generated by generate_union_plan() or
		 * generate_nonunion_plan(), and hence its tlist was generated by
		 * generate_append_tlist(), this will work.  We just tell
		 * generate_setop_tlist() to use varno 0.
		 */
		if (flag >= 0 ||
			!tlist_same_datatypes(plan->targetlist, colTypes, junkOK) ||
			!tlist_same_collations(plan->targetlist, colCollations, junkOK))
		{
			plan = (Plan *)
				make_result(root,
							generate_setop_tlist(colTypes, colCollations,
												 flag,
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
 * Generate plan for a recursive UNION node
 */
static Plan *
generate_recursion_plan(SetOperationStmt *setOp, PlannerInfo *root,
						double tuple_fraction,
						List *refnames_tlist,
						List **sortClauses)
{
	Plan	   *plan;
	Plan	   *lplan;
	Plan	   *rplan;
	List	   *tlist;
	List	   *groupList;
	long		numGroups;

	/* Parser should have rejected other cases */
	if (setOp->op != SETOP_UNION)
		elog(ERROR, "only UNION queries can be recursive");
	/* Worktable ID should be assigned */
	Assert(root->wt_param_id >= 0);

	/*
	 * Unlike a regular UNION node, process the left and right inputs
	 * separately without any intention of combining them into one Append.
	 */
	lplan = recurse_set_operations(setOp->larg, root, tuple_fraction,
								   setOp->colTypes, setOp->colCollations,
								   false, -1,
								   refnames_tlist, sortClauses, NULL);
	/* The right plan will want to look at the left one ... */
	root->non_recursive_plan = lplan;
	rplan = recurse_set_operations(setOp->rarg, root, tuple_fraction,
								   setOp->colTypes, setOp->colCollations,
								   false, -1,
								   refnames_tlist, sortClauses, NULL);
	root->non_recursive_plan = NULL;

	/*
	 * Generate tlist for RecursiveUnion plan node --- same as in Append cases
	 */
	tlist = generate_append_tlist(setOp->colTypes, setOp->colCollations, false,
								  list_make2(lplan, rplan),
								  refnames_tlist);

	/*
	 * If UNION, identify the grouping operators
	 */
	if (setOp->all)
	{
		groupList = NIL;
		numGroups = 0;
	}
	else
	{
		double		dNumGroups;

		/* Identify the grouping semantics */
		groupList = generate_setop_grouplist(setOp, tlist);

		/* We only support hashing here */
		if (!grouping_is_hashable(groupList))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not implement recursive UNION"),
					 errdetail("All column datatypes must be hashable.")));

		/*
		 * For the moment, take the number of distinct groups as equal to the
		 * total input size, ie, the worst case.
		 */
		dNumGroups = lplan->plan_rows + rplan->plan_rows * 10;

		/* Also convert to long int --- but 'ware overflow! */
		numGroups = (long) Min(dNumGroups, (double) LONG_MAX);
	}

	/*
	 * And make the plan node.
	 */
	plan = (Plan *) make_recursive_union(tlist, lplan, rplan,
										 root->wt_param_id,
										 groupList, numGroups);

	*sortClauses = NIL;			/* RecursiveUnion result is always unsorted */

	return plan;
}

/*
 * Generate plan for a UNION or UNION ALL node
 */
static Plan *
generate_union_plan(SetOperationStmt *op, PlannerInfo *root,
					double tuple_fraction,
					List *refnames_tlist,
					List **sortClauses, double *pNumGroups)
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
	 * only one Append and unique-ification for the lot.  Recurse to find such
	 * nodes and compute their children's plans.
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
	tlist = generate_append_tlist(op->colTypes, op->colCollations, false,
								  planlist, refnames_tlist);

	/*
	 * Append the child results together.
	 */
	plan = (Plan *) make_append(planlist, tlist);

	/*
	 * For UNION ALL, we just need the Append plan.  For UNION, need to add
	 * node(s) to remove duplicates.
	 */
	if (op->all)
		*sortClauses = NIL;		/* result of UNION ALL is always unsorted */
	else
		plan = make_union_unique(op, plan, root, tuple_fraction, sortClauses);

	/*
	 * Estimate number of groups if caller wants it.  For now we just assume
	 * the output is unique --- this is certainly true for the UNION case, and
	 * we want worst-case estimates anyway.
	 */
	if (pNumGroups)
		*pNumGroups = plan->plan_rows;

	return plan;
}

/*
 * Generate plan for an INTERSECT, INTERSECT ALL, EXCEPT, or EXCEPT ALL node
 */
static Plan *
generate_nonunion_plan(SetOperationStmt *op, PlannerInfo *root,
					   double tuple_fraction,
					   List *refnames_tlist,
					   List **sortClauses, double *pNumGroups)
{
	Plan	   *lplan,
			   *rplan,
			   *plan;
	List	   *tlist,
			   *groupList,
			   *planlist,
			   *child_sortclauses;
	double		dLeftGroups,
				dRightGroups,
				dNumGroups,
				dNumOutputRows;
	long		numGroups;
	bool		use_hash;
	SetOpCmd	cmd;
	int			firstFlag;

	/* Recurse on children, ensuring their outputs are marked */
	lplan = recurse_set_operations(op->larg, root,
								   0.0 /* all tuples needed */ ,
								   op->colTypes, op->colCollations,
								   false, 0,
								   refnames_tlist,
								   &child_sortclauses, &dLeftGroups);
	rplan = recurse_set_operations(op->rarg, root,
								   0.0 /* all tuples needed */ ,
								   op->colTypes, op->colCollations,
								   false, 1,
								   refnames_tlist,
								   &child_sortclauses, &dRightGroups);

	/*
	 * For EXCEPT, we must put the left input first.  For INTERSECT, either
	 * order should give the same results, and we prefer to put the smaller
	 * input first in order to minimize the size of the hash table in the
	 * hashing case.  "Smaller" means the one with the fewer groups.
	 */
	if (op->op == SETOP_EXCEPT || dLeftGroups <= dRightGroups)
	{
		planlist = list_make2(lplan, rplan);
		firstFlag = 0;
	}
	else
	{
		planlist = list_make2(rplan, lplan);
		firstFlag = 1;
	}

	/*
	 * Generate tlist for Append plan node.
	 *
	 * The tlist for an Append plan isn't important as far as the Append is
	 * concerned, but we must make it look real anyway for the benefit of the
	 * next plan level up.	In fact, it has to be real enough that the flag
	 * column is shown as a variable not a constant, else setrefs.c will get
	 * confused.
	 */
	tlist = generate_append_tlist(op->colTypes, op->colCollations, true,
								  planlist, refnames_tlist);

	/*
	 * Append the child results together.
	 */
	plan = (Plan *) make_append(planlist, tlist);

	/* Identify the grouping semantics */
	groupList = generate_setop_grouplist(op, tlist);

	/* punt if nothing to group on (can this happen?) */
	if (groupList == NIL)
	{
		*sortClauses = NIL;
		return plan;
	}

	/*
	 * Estimate number of distinct groups that we'll need hashtable entries
	 * for; this is the size of the left-hand input for EXCEPT, or the smaller
	 * input for INTERSECT.  Also estimate the number of eventual output rows.
	 * In non-ALL cases, we estimate each group produces one output row; in
	 * ALL cases use the relevant relation size.  These are worst-case
	 * estimates, of course, but we need to be conservative.
	 */
	if (op->op == SETOP_EXCEPT)
	{
		dNumGroups = dLeftGroups;
		dNumOutputRows = op->all ? lplan->plan_rows : dNumGroups;
	}
	else
	{
		dNumGroups = Min(dLeftGroups, dRightGroups);
		dNumOutputRows = op->all ? Min(lplan->plan_rows, rplan->plan_rows) : dNumGroups;
	}

	/* Also convert to long int --- but 'ware overflow! */
	numGroups = (long) Min(dNumGroups, (double) LONG_MAX);

	/*
	 * Decide whether to hash or sort, and add a sort node if needed.
	 */
	use_hash = choose_hashed_setop(root, groupList, plan,
								   dNumGroups, dNumOutputRows, tuple_fraction,
					   (op->op == SETOP_INTERSECT) ? "INTERSECT" : "EXCEPT");

	if (!use_hash)
		plan = (Plan *) make_sort_from_sortclauses(root, groupList, plan);

	/*
	 * Finally, add a SetOp plan node to generate the correct output.
	 */
	switch (op->op)
	{
		case SETOP_INTERSECT:
			cmd = op->all ? SETOPCMD_INTERSECT_ALL : SETOPCMD_INTERSECT;
			break;
		case SETOP_EXCEPT:
			cmd = op->all ? SETOPCMD_EXCEPT_ALL : SETOPCMD_EXCEPT;
			break;
		default:
			elog(ERROR, "unrecognized set op: %d", (int) op->op);
			cmd = SETOPCMD_INTERSECT;	/* keep compiler quiet */
			break;
	}
	plan = (Plan *) make_setop(cmd, use_hash ? SETOP_HASHED : SETOP_SORTED,
							   plan, groupList,
							   list_length(op->colTypes) + 1,
							   use_hash ? firstFlag : -1,
							   numGroups, dNumOutputRows);

	/* Result is sorted only if we're not hashing */
	*sortClauses = use_hash ? NIL : groupList;

	if (pNumGroups)
		*pNumGroups = dNumGroups;

	return plan;
}

/*
 * Pull up children of a UNION node that are identically-propertied UNIONs.
 *
 * NOTE: we can also pull a UNION ALL up into a UNION, since the distinct
 * output rows will be lost anyway.
 *
 * NOTE: currently, we ignore collations while determining if a child has
 * the same properties.  This is semantically sound only so long as all
 * collations have the same notion of equality.  It is valid from an
 * implementation standpoint because we don't care about the ordering of
 * a UNION child's result: UNION ALL results are always unordered, and
 * generate_union_plan will force a fresh sort if the top level is a UNION.
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
											 top_union->colTypes,
											 top_union->colCollations,
											 false, -1,
											 refnames_tlist,
											 &child_sortclauses, NULL));
}

/*
 * Add nodes to the given plan tree to unique-ify the result of a UNION.
 */
static Plan *
make_union_unique(SetOperationStmt *op, Plan *plan,
				  PlannerInfo *root, double tuple_fraction,
				  List **sortClauses)
{
	List	   *groupList;
	double		dNumGroups;
	long		numGroups;

	/* Identify the grouping semantics */
	groupList = generate_setop_grouplist(op, plan->targetlist);

	/* punt if nothing to group on (can this happen?) */
	if (groupList == NIL)
	{
		*sortClauses = NIL;
		return plan;
	}

	/*
	 * XXX for the moment, take the number of distinct groups as equal to the
	 * total input size, ie, the worst case.  This is too conservative, but we
	 * don't want to risk having the hashtable overrun memory; also, it's not
	 * clear how to get a decent estimate of the true size.  One should note
	 * as well the propensity of novices to write UNION rather than UNION ALL
	 * even when they don't expect any duplicates...
	 */
	dNumGroups = plan->plan_rows;

	/* Also convert to long int --- but 'ware overflow! */
	numGroups = (long) Min(dNumGroups, (double) LONG_MAX);

	/* Decide whether to hash or sort */
	if (choose_hashed_setop(root, groupList, plan,
							dNumGroups, dNumGroups, tuple_fraction,
							"UNION"))
	{
		/* Hashed aggregate plan --- no sort needed */
		plan = (Plan *) make_agg(root,
								 plan->targetlist,
								 NIL,
								 AGG_HASHED,
								 NULL,
								 list_length(groupList),
								 extract_grouping_cols(groupList,
													   plan->targetlist),
								 extract_grouping_ops(groupList),
								 numGroups,
								 plan);
		/* Hashed aggregation produces randomly-ordered results */
		*sortClauses = NIL;
	}
	else
	{
		/* Sort and Unique */
		plan = (Plan *) make_sort_from_sortclauses(root, groupList, plan);
		plan = (Plan *) make_unique(plan, groupList);
		plan->plan_rows = dNumGroups;
		/* We know the sort order of the result */
		*sortClauses = groupList;
	}

	return plan;
}

/*
 * choose_hashed_setop - should we use hashing for a set operation?
 */
static bool
choose_hashed_setop(PlannerInfo *root, List *groupClauses,
					Plan *input_plan,
					double dNumGroups, double dNumOutputRows,
					double tuple_fraction,
					const char *construct)
{
	int			numGroupCols = list_length(groupClauses);
	bool		can_sort;
	bool		can_hash;
	Size		hashentrysize;
	Path		hashed_p;
	Path		sorted_p;

	/* Check whether the operators support sorting or hashing */
	can_sort = grouping_is_sortable(groupClauses);
	can_hash = grouping_is_hashable(groupClauses);
	if (can_hash && can_sort)
	{
		/* we have a meaningful choice to make, continue ... */
	}
	else if (can_hash)
		return true;
	else if (can_sort)
		return false;
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/* translator: %s is UNION, INTERSECT, or EXCEPT */
				 errmsg("could not implement %s", construct),
				 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

	/* Prefer sorting when enable_hashagg is off */
	if (!enable_hashagg)
		return false;

	/*
	 * Don't do it if it doesn't look like the hashtable will fit into
	 * work_mem.
	 */
	hashentrysize = MAXALIGN(input_plan->plan_width) + MAXALIGN(sizeof(MinimalTupleData));

	if (hashentrysize * dNumGroups > work_mem * 1024L)
		return false;

	/*
	 * See if the estimated cost is no more than doing it the other way.
	 *
	 * We need to consider input_plan + hashagg versus input_plan + sort +
	 * group.  Note that the actual result plan might involve a SetOp or
	 * Unique node, not Agg or Group, but the cost estimates for Agg and Group
	 * should be close enough for our purposes here.
	 *
	 * These path variables are dummies that just hold cost fields; we don't
	 * make actual Paths for these steps.
	 */
	cost_agg(&hashed_p, root, AGG_HASHED, NULL,
			 numGroupCols, dNumGroups,
			 input_plan->startup_cost, input_plan->total_cost,
			 input_plan->plan_rows);

	/*
	 * Now for the sorted case.  Note that the input is *always* unsorted,
	 * since it was made by appending unrelated sub-relations together.
	 */
	sorted_p.startup_cost = input_plan->startup_cost;
	sorted_p.total_cost = input_plan->total_cost;
	/* XXX cost_sort doesn't actually look at pathkeys, so just pass NIL */
	cost_sort(&sorted_p, root, NIL, sorted_p.total_cost,
			  input_plan->plan_rows, input_plan->plan_width,
			  0.0, work_mem, -1.0);
	cost_group(&sorted_p, root, numGroupCols, dNumGroups,
			   sorted_p.startup_cost, sorted_p.total_cost,
			   input_plan->plan_rows);

	/*
	 * Now make the decision using the top-level tuple fraction.  First we
	 * have to convert an absolute count (LIMIT) into fractional form.
	 */
	if (tuple_fraction >= 1.0)
		tuple_fraction /= dNumOutputRows;

	if (compare_fractional_path_costs(&hashed_p, &sorted_p,
									  tuple_fraction) < 0)
	{
		/* Hashed is cheaper, so use it */
		return true;
	}
	return false;
}

/*
 * Generate targetlist for a set-operation plan node
 *
 * colTypes: OID list of set-op's result column datatypes
 * colCollations: OID list of set-op's result column collations
 * flag: -1 if no flag column needed, 0 or 1 to create a const flag column
 * varno: varno to use in generated Vars
 * hack_constants: true to copy up constants (see comments in code)
 * input_tlist: targetlist of this node's input node
 * refnames_tlist: targetlist to take column names from
 */
static List *
generate_setop_tlist(List *colTypes, List *colCollations,
					 int flag,
					 Index varno,
					 bool hack_constants,
					 List *input_tlist,
					 List *refnames_tlist)
{
	List	   *tlist = NIL;
	int			resno = 1;
	ListCell   *ctlc,
			   *cclc,
			   *itlc,
			   *rtlc;
	TargetEntry *tle;
	Node	   *expr;

	/* there's no forfour() so we must chase one list manually */
	rtlc = list_head(refnames_tlist);
	forthree(ctlc, colTypes, cclc, colCollations, itlc, input_tlist)
	{
		Oid			colType = lfirst_oid(ctlc);
		Oid			colColl = lfirst_oid(cclc);
		TargetEntry *inputtle = (TargetEntry *) lfirst(itlc);
		TargetEntry *reftle = (TargetEntry *) lfirst(rtlc);

		rtlc = lnext(rtlc);

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
									exprCollation((Node *) inputtle->expr),
									0);

		if (exprType(expr) != colType)
		{
			/*
			 * Note: it's not really cool to be applying coerce_to_common_type
			 * here; one notable point is that assign_expr_collations never
			 * gets run on any generated nodes.  For the moment that's not a
			 * problem because we force the correct exposed collation below.
			 * It would likely be best to make the parser generate the correct
			 * output tlist for every set-op to begin with, though.
			 */
			expr = coerce_to_common_type(NULL,	/* no UNKNOWNs here */
										 expr,
										 colType,
										 "UNION/INTERSECT/EXCEPT");
		}

		/*
		 * Ensure the tlist entry's exposed collation matches the set-op. This
		 * is necessary because plan_set_operations() reports the result
		 * ordering as a list of SortGroupClauses, which don't carry collation
		 * themselves but just refer to tlist entries.	If we don't show the
		 * right collation then planner.c might do the wrong thing in
		 * higher-level queries.
		 *
		 * Note we use RelabelType, not CollateExpr, since this expression
		 * will reach the executor without any further processing.
		 */
		if (exprCollation(expr) != colColl)
		{
			expr = (Node *) makeRelabelType((Expr *) expr,
											exprType(expr),
											exprTypmod(expr),
											colColl,
											COERCE_DONTCARE);
		}

		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup(reftle->resname),
							  false);
		tlist = lappend(tlist, tle);
	}

	if (flag >= 0)
	{
		/* Add a resjunk flag column */
		/* flag value is the given constant */
		expr = (Node *) makeConst(INT4OID,
								  -1,
								  InvalidOid,
								  sizeof(int32),
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
 * colTypes: OID list of set-op's result column datatypes
 * colCollations: OID list of set-op's result column collations
 * flag: true to create a flag column copied up from subplans
 * input_plans: list of sub-plans of the Append
 * refnames_tlist: targetlist to take column names from
 *
 * The entries in the Append's targetlist should always be simple Vars;
 * we just have to make sure they have the right datatypes/typmods/collations.
 * The Vars are always generated with varno 0.
 */
static List *
generate_append_tlist(List *colTypes, List *colCollations,
					  bool flag,
					  List *input_plans,
					  List *refnames_tlist)
{
	List	   *tlist = NIL;
	int			resno = 1;
	ListCell   *curColType;
	ListCell   *curColCollation;
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
	forthree(curColType, colTypes, curColCollation, colCollations,
			 ref_tl_item, refnames_tlist)
	{
		Oid			colType = lfirst_oid(curColType);
		int32		colTypmod = colTypmods[colindex++];
		Oid			colColl = lfirst_oid(curColCollation);
		TargetEntry *reftle = (TargetEntry *) lfirst(ref_tl_item);

		Assert(reftle->resno == resno);
		Assert(!reftle->resjunk);
		expr = (Node *) makeVar(0,
								resno,
								colType,
								colTypmod,
								colColl,
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
								InvalidOid,
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
 * generate_setop_grouplist
 *		Build a SortGroupClause list defining the sort/grouping properties
 *		of the setop's output columns.
 *
 * Parse analysis already determined the properties and built a suitable
 * list, except that the entries do not have sortgrouprefs set because
 * the parser output representation doesn't include a tlist for each
 * setop.  So what we need to do here is copy that list and install
 * proper sortgrouprefs into it and into the targetlist.
 */
static List *
generate_setop_grouplist(SetOperationStmt *op, List *targetlist)
{
	List	   *grouplist = (List *) copyObject(op->groupClauses);
	ListCell   *lg;
	ListCell   *lt;
	Index		refno = 1;

	lg = list_head(grouplist);
	foreach(lt, targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lt);
		SortGroupClause *sgc;

		/* tlist shouldn't have any sortgrouprefs yet */
		Assert(tle->ressortgroupref == 0);

		if (tle->resjunk)
			continue;			/* ignore resjunk columns */

		/* non-resjunk columns should have grouping clauses */
		Assert(lg != NULL);
		sgc = (SortGroupClause *) lfirst(lg);
		lg = lnext(lg);
		Assert(sgc->tleSortGroupRef == 0);

		/* we could use assignSortGroupRef here, but seems a bit silly */
		sgc->tleSortGroupRef = tle->ressortgroupref = refno++;
	}
	Assert(lg == NULL);
	return grouplist;
}


/*
 * expand_inherited_tables
 *		Expand each rangetable entry that represents an inheritance set
 *		into an "append relation".	At the conclusion of this process,
 *		the "inh" flag is set in all and only those RTEs that are append
 *		relation parents.
 */
void
expand_inherited_tables(PlannerInfo *root)
{
	Index		nrtes;
	Index		rti;
	ListCell   *rl;

	/*
	 * expand_inherited_rtentry may add RTEs to parse->rtable; there is no
	 * need to scan them since they can't have inh=true.  So just scan as far
	 * as the original end of the rtable list.
	 */
	nrtes = list_length(root->parse->rtable);
	rl = list_head(root->parse->rtable);
	for (rti = 1; rti <= nrtes; rti++)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(rl);

		expand_inherited_rtentry(root, rte, rti);
		rl = lnext(rl);
	}
}

/*
 * expand_inherited_rtentry
 *		Check whether a rangetable entry represents an inheritance set.
 *		If so, add entries for all the child tables to the query's
 *		rangetable, and build AppendRelInfo nodes for all the child tables
 *		and add them to root->append_rel_list.	If not, clear the entry's
 *		"inh" flag to prevent later code from looking for AppendRelInfos.
 *
 * Note that the original RTE is considered to represent the whole
 * inheritance set.  The first of the generated RTEs is an RTE for the same
 * table, but with inh = false, to represent the parent table in its role
 * as a simple member of the inheritance set.
 *
 * A childless table is never considered to be an inheritance set; therefore
 * a parent RTE must always have at least two associated AppendRelInfos.
 */
static void
expand_inherited_rtentry(PlannerInfo *root, RangeTblEntry *rte, Index rti)
{
	Query	   *parse = root->parse;
	Oid			parentOID;
	PlanRowMark *oldrc;
	Relation	oldrelation;
	LOCKMODE	lockmode;
	List	   *inhOIDs;
	List	   *appinfos;
	ListCell   *l;

	/* Does RT entry allow inheritance? */
	if (!rte->inh)
		return;
	/* Ignore any already-expanded UNION ALL nodes */
	if (rte->rtekind != RTE_RELATION)
	{
		Assert(rte->rtekind == RTE_SUBQUERY);
		return;
	}
	/* Fast path for common case of childless table */
	parentOID = rte->relid;
	if (!has_subclass(parentOID))
	{
		/* Clear flag before returning */
		rte->inh = false;
		return;
	}

	/*
	 * The rewriter should already have obtained an appropriate lock on each
	 * relation named in the query.  However, for each child relation we add
	 * to the query, we must obtain an appropriate lock, because this will be
	 * the first use of those relations in the parse/rewrite/plan pipeline.
	 *
	 * If the parent relation is the query's result relation, then we need
	 * RowExclusiveLock.  Otherwise, if it's accessed FOR UPDATE/SHARE, we
	 * need RowShareLock; otherwise AccessShareLock.  We can't just grab
	 * AccessShareLock because then the executor would be trying to upgrade
	 * the lock, leading to possible deadlocks.  (This code should match the
	 * parser and rewriter.)
	 */
	oldrc = get_plan_rowmark(root->rowMarks, rti);
	if (rti == parse->resultRelation)
		lockmode = RowExclusiveLock;
	else if (oldrc && RowMarkRequiresRowShareLock(oldrc->markType))
		lockmode = RowShareLock;
	else
		lockmode = AccessShareLock;

	/* Scan for all members of inheritance set, acquire needed locks */
	inhOIDs = find_all_inheritors(parentOID, lockmode, NULL);

	/*
	 * Check that there's at least one descendant, else treat as no-child
	 * case.  This could happen despite above has_subclass() check, if table
	 * once had a child but no longer does.
	 */
	if (list_length(inhOIDs) < 2)
	{
		/* Clear flag before returning */
		rte->inh = false;
		return;
	}

	/*
	 * If parent relation is selected FOR UPDATE/SHARE, we need to mark its
	 * PlanRowMark as isParent = true, and generate a new PlanRowMark for each
	 * child.
	 */
	if (oldrc)
		oldrc->isParent = true;

	/*
	 * Must open the parent relation to examine its tupdesc.  We need not lock
	 * it; we assume the rewriter already did.
	 */
	oldrelation = heap_open(parentOID, NoLock);

	/* Scan the inheritance set and expand it */
	appinfos = NIL;
	foreach(l, inhOIDs)
	{
		Oid			childOID = lfirst_oid(l);
		Relation	newrelation;
		RangeTblEntry *childrte;
		Index		childRTindex;
		AppendRelInfo *appinfo;

		/* Open rel if needed; we already have required locks */
		if (childOID != parentOID)
			newrelation = heap_open(childOID, NoLock);
		else
			newrelation = oldrelation;

		/*
		 * It is possible that the parent table has children that are temp
		 * tables of other backends.  We cannot safely access such tables
		 * (because of buffering issues), and the best thing to do seems to be
		 * to silently ignore them.
		 */
		if (childOID != parentOID && RELATION_IS_OTHER_TEMP(newrelation))
		{
			heap_close(newrelation, lockmode);
			continue;
		}

		/*
		 * Build an RTE for the child, and attach to query's rangetable list.
		 * We copy most fields of the parent's RTE, but replace relation OID,
		 * and set inh = false.  Also, set requiredPerms to zero since all
		 * required permissions checks are done on the original RTE.
		 */
		childrte = copyObject(rte);
		childrte->relid = childOID;
		childrte->inh = false;
		childrte->requiredPerms = 0;
		parse->rtable = lappend(parse->rtable, childrte);
		childRTindex = list_length(parse->rtable);

		/*
		 * Build an AppendRelInfo for this parent and child.
		 */
		appinfo = makeNode(AppendRelInfo);
		appinfo->parent_relid = rti;
		appinfo->child_relid = childRTindex;
		appinfo->parent_reltype = oldrelation->rd_rel->reltype;
		appinfo->child_reltype = newrelation->rd_rel->reltype;
		make_inh_translation_list(oldrelation, newrelation, childRTindex,
								  &appinfo->translated_vars);
		appinfo->parent_reloid = parentOID;
		appinfos = lappend(appinfos, appinfo);

		/*
		 * Translate the column permissions bitmaps to the child's attnums (we
		 * have to build the translated_vars list before we can do this). But
		 * if this is the parent table, leave copyObject's result alone.
		 *
		 * Note: we need to do this even though the executor won't run any
		 * permissions checks on the child RTE.  The modifiedCols bitmap may
		 * be examined for trigger-firing purposes.
		 */
		if (childOID != parentOID)
		{
			childrte->selectedCols = translate_col_privs(rte->selectedCols,
												   appinfo->translated_vars);
			childrte->modifiedCols = translate_col_privs(rte->modifiedCols,
												   appinfo->translated_vars);
		}

		/*
		 * Build a PlanRowMark if parent is marked FOR UPDATE/SHARE.
		 */
		if (oldrc)
		{
			PlanRowMark *newrc = makeNode(PlanRowMark);

			newrc->rti = childRTindex;
			newrc->prti = rti;
			newrc->rowmarkId = oldrc->rowmarkId;
			newrc->markType = oldrc->markType;
			newrc->noWait = oldrc->noWait;
			newrc->isParent = false;

			root->rowMarks = lappend(root->rowMarks, newrc);
		}

		/* Close child relations, but keep locks */
		if (childOID != parentOID)
			heap_close(newrelation, NoLock);
	}

	heap_close(oldrelation, NoLock);

	/*
	 * If all the children were temp tables, pretend it's a non-inheritance
	 * situation.  The duplicate RTE we added for the parent table is
	 * harmless, so we don't bother to get rid of it.
	 */
	if (list_length(appinfos) < 2)
	{
		/* Clear flag before returning */
		rte->inh = false;
		return;
	}

	/* Otherwise, OK to add to root->append_rel_list */
	root->append_rel_list = list_concat(root->append_rel_list, appinfos);
}

/*
 * make_inh_translation_list
 *	  Build the list of translations from parent Vars to child Vars for
 *	  an inheritance child.
 *
 * For paranoia's sake, we match type/collation as well as attribute name.
 */
static void
make_inh_translation_list(Relation oldrelation, Relation newrelation,
						  Index newvarno,
						  List **translated_vars)
{
	List	   *vars = NIL;
	TupleDesc	old_tupdesc = RelationGetDescr(oldrelation);
	TupleDesc	new_tupdesc = RelationGetDescr(newrelation);
	int			oldnatts = old_tupdesc->natts;
	int			newnatts = new_tupdesc->natts;
	int			old_attno;

	for (old_attno = 0; old_attno < oldnatts; old_attno++)
	{
		Form_pg_attribute att;
		char	   *attname;
		Oid			atttypid;
		int32		atttypmod;
		Oid			attcollation;
		int			new_attno;

		att = old_tupdesc->attrs[old_attno];
		if (att->attisdropped)
		{
			/* Just put NULL into this list entry */
			vars = lappend(vars, NULL);
			continue;
		}
		attname = NameStr(att->attname);
		atttypid = att->atttypid;
		atttypmod = att->atttypmod;
		attcollation = att->attcollation;

		/*
		 * When we are generating the "translation list" for the parent table
		 * of an inheritance set, no need to search for matches.
		 */
		if (oldrelation == newrelation)
		{
			vars = lappend(vars, makeVar(newvarno,
										 (AttrNumber) (old_attno + 1),
										 atttypid,
										 atttypmod,
										 attcollation,
										 0));
			continue;
		}

		/*
		 * Otherwise we have to search for the matching column by name.
		 * There's no guarantee it'll have the same column position, because
		 * of cases like ALTER TABLE ADD COLUMN and multiple inheritance.
		 * However, in simple cases it will be the same column number, so try
		 * that before we go groveling through all the columns.
		 *
		 * Note: the test for (att = ...) != NULL cannot fail, it's just a
		 * notational device to include the assignment into the if-clause.
		 */
		if (old_attno < newnatts &&
			(att = new_tupdesc->attrs[old_attno]) != NULL &&
			!att->attisdropped && att->attinhcount != 0 &&
			strcmp(attname, NameStr(att->attname)) == 0)
			new_attno = old_attno;
		else
		{
			for (new_attno = 0; new_attno < newnatts; new_attno++)
			{
				att = new_tupdesc->attrs[new_attno];
				if (!att->attisdropped && att->attinhcount != 0 &&
					strcmp(attname, NameStr(att->attname)) == 0)
					break;
			}
			if (new_attno >= newnatts)
				elog(ERROR, "could not find inherited attribute \"%s\" of relation \"%s\"",
					 attname, RelationGetRelationName(newrelation));
		}

		/* Found it, check type and collation match */
		if (atttypid != att->atttypid || atttypmod != att->atttypmod)
			elog(ERROR, "attribute \"%s\" of relation \"%s\" does not match parent's type",
				 attname, RelationGetRelationName(newrelation));
		if (attcollation != att->attcollation)
			elog(ERROR, "attribute \"%s\" of relation \"%s\" does not match parent's collation",
				 attname, RelationGetRelationName(newrelation));

		vars = lappend(vars, makeVar(newvarno,
									 (AttrNumber) (new_attno + 1),
									 atttypid,
									 atttypmod,
									 attcollation,
									 0));
	}

	*translated_vars = vars;
}

/*
 * translate_col_privs
 *	  Translate a bitmapset representing per-column privileges from the
 *	  parent rel's attribute numbering to the child's.
 *
 * The only surprise here is that we don't translate a parent whole-row
 * reference into a child whole-row reference.	That would mean requiring
 * permissions on all child columns, which is overly strict, since the
 * query is really only going to reference the inherited columns.  Instead
 * we set the per-column bits for all inherited columns.
 */
static Bitmapset *
translate_col_privs(const Bitmapset *parent_privs,
					List *translated_vars)
{
	Bitmapset  *child_privs = NULL;
	bool		whole_row;
	int			attno;
	ListCell   *lc;

	/* System attributes have the same numbers in all tables */
	for (attno = FirstLowInvalidHeapAttributeNumber + 1; attno < 0; attno++)
	{
		if (bms_is_member(attno - FirstLowInvalidHeapAttributeNumber,
						  parent_privs))
			child_privs = bms_add_member(child_privs,
								 attno - FirstLowInvalidHeapAttributeNumber);
	}

	/* Check if parent has whole-row reference */
	whole_row = bms_is_member(InvalidAttrNumber - FirstLowInvalidHeapAttributeNumber,
							  parent_privs);

	/* And now translate the regular user attributes, using the vars list */
	attno = InvalidAttrNumber;
	foreach(lc, translated_vars)
	{
		Var		   *var = (Var *) lfirst(lc);

		attno++;
		if (var == NULL)		/* ignore dropped columns */
			continue;
		Assert(IsA(var, Var));
		if (whole_row ||
			bms_is_member(attno - FirstLowInvalidHeapAttributeNumber,
						  parent_privs))
			child_privs = bms_add_member(child_privs,
						 var->varattno - FirstLowInvalidHeapAttributeNumber);
	}

	return child_privs;
}

/*
 * adjust_appendrel_attrs
 *	  Copy the specified query or expression and translate Vars referring
 *	  to the parent rel of the specified AppendRelInfo to refer to the
 *	  child rel instead.  We also update rtindexes appearing outside Vars,
 *	  such as resultRelation and jointree relids.
 *
 * Note: this is only applied after conversion of sublinks to subplans,
 * so we don't need to cope with recursion into sub-queries.
 *
 * Note: this is not hugely different from what pullup_replace_vars() does;
 * maybe we should try to fold the two routines together.
 */
Node *
adjust_appendrel_attrs(PlannerInfo *root, Node *node, AppendRelInfo *appinfo)
{
	Node	   *result;
	adjust_appendrel_attrs_context context;

	context.root = root;
	context.appinfo = appinfo;

	/*
	 * Must be prepared to start with a Query or a bare expression tree.
	 */
	if (node && IsA(node, Query))
	{
		Query	   *newnode;

		newnode = query_tree_mutator((Query *) node,
									 adjust_appendrel_attrs_mutator,
									 (void *) &context,
									 QTW_IGNORE_RC_SUBQUERIES);
		if (newnode->resultRelation == appinfo->parent_relid)
		{
			newnode->resultRelation = appinfo->child_relid;
			/* Fix tlist resnos too, if it's inherited UPDATE */
			if (newnode->commandType == CMD_UPDATE)
				newnode->targetList =
					adjust_inherited_tlist(newnode->targetList,
										   appinfo);
		}
		result = (Node *) newnode;
	}
	else
		result = adjust_appendrel_attrs_mutator(node, &context);

	return result;
}

static Node *
adjust_appendrel_attrs_mutator(Node *node,
							   adjust_appendrel_attrs_context *context)
{
	AppendRelInfo *appinfo = context->appinfo;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) copyObject(node);

		if (var->varlevelsup == 0 &&
			var->varno == appinfo->parent_relid)
		{
			var->varno = appinfo->child_relid;
			var->varnoold = appinfo->child_relid;
			if (var->varattno > 0)
			{
				Node	   *newnode;

				if (var->varattno > list_length(appinfo->translated_vars))
					elog(ERROR, "attribute %d of relation \"%s\" does not exist",
						 var->varattno, get_rel_name(appinfo->parent_reloid));
				newnode = copyObject(list_nth(appinfo->translated_vars,
											  var->varattno - 1));
				if (newnode == NULL)
					elog(ERROR, "attribute %d of relation \"%s\" does not exist",
						 var->varattno, get_rel_name(appinfo->parent_reloid));
				return newnode;
			}
			else if (var->varattno == 0)
			{
				/*
				 * Whole-row Var: if we are dealing with named rowtypes, we
				 * can use a whole-row Var for the child table plus a coercion
				 * step to convert the tuple layout to the parent's rowtype.
				 * Otherwise we have to generate a RowExpr.
				 */
				if (OidIsValid(appinfo->child_reltype))
				{
					Assert(var->vartype == appinfo->parent_reltype);
					if (appinfo->parent_reltype != appinfo->child_reltype)
					{
						ConvertRowtypeExpr *r = makeNode(ConvertRowtypeExpr);

						r->arg = (Expr *) var;
						r->resulttype = appinfo->parent_reltype;
						r->convertformat = COERCE_IMPLICIT_CAST;
						r->location = -1;
						/* Make sure the Var node has the right type ID, too */
						var->vartype = appinfo->child_reltype;
						return (Node *) r;
					}
				}
				else
				{
					/*
					 * Build a RowExpr containing the translated variables.
					 *
					 * In practice var->vartype will always be RECORDOID here,
					 * so we need to come up with some suitable column names.
					 * We use the parent RTE's column names.
					 *
					 * Note: we can't get here for inheritance cases, so there
					 * is no need to worry that translated_vars might contain
					 * some dummy NULLs.
					 */
					RowExpr    *rowexpr;
					List	   *fields;
					RangeTblEntry *rte;

					rte = rt_fetch(appinfo->parent_relid,
								   context->root->parse->rtable);
					fields = (List *) copyObject(appinfo->translated_vars);
					rowexpr = makeNode(RowExpr);
					rowexpr->args = fields;
					rowexpr->row_typeid = var->vartype;
					rowexpr->row_format = COERCE_IMPLICIT_CAST;
					rowexpr->colnames = copyObject(rte->eref->colnames);
					rowexpr->location = -1;

					return (Node *) rowexpr;
				}
			}
			/* system attributes don't need any other translation */
		}
		return (Node *) var;
	}
	if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) copyObject(node);

		if (cexpr->cvarno == appinfo->parent_relid)
			cexpr->cvarno = appinfo->child_relid;
		return (Node *) cexpr;
	}
	if (IsA(node, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) copyObject(node);

		if (rtr->rtindex == appinfo->parent_relid)
			rtr->rtindex = appinfo->child_relid;
		return (Node *) rtr;
	}
	if (IsA(node, JoinExpr))
	{
		/* Copy the JoinExpr node with correct mutation of subnodes */
		JoinExpr   *j;

		j = (JoinExpr *) expression_tree_mutator(node,
											  adjust_appendrel_attrs_mutator,
												 (void *) context);
		/* now fix JoinExpr's rtindex (probably never happens) */
		if (j->rtindex == appinfo->parent_relid)
			j->rtindex = appinfo->child_relid;
		return (Node *) j;
	}
	if (IsA(node, PlaceHolderVar))
	{
		/* Copy the PlaceHolderVar node with correct mutation of subnodes */
		PlaceHolderVar *phv;

		phv = (PlaceHolderVar *) expression_tree_mutator(node,
											  adjust_appendrel_attrs_mutator,
														 (void *) context);
		/* now fix PlaceHolderVar's relid sets */
		if (phv->phlevelsup == 0)
			phv->phrels = adjust_relid_set(phv->phrels,
										   appinfo->parent_relid,
										   appinfo->child_relid);
		return (Node *) phv;
	}
	/* Shouldn't need to handle planner auxiliary nodes here */
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, AppendRelInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	/*
	 * We have to process RestrictInfo nodes specially.  (Note: although
	 * set_append_rel_pathlist will hide RestrictInfos in the parent's
	 * baserestrictinfo list from us, it doesn't hide those in joininfo.)
	 */
	if (IsA(node, RestrictInfo))
	{
		RestrictInfo *oldinfo = (RestrictInfo *) node;
		RestrictInfo *newinfo = makeNode(RestrictInfo);

		/* Copy all flat-copiable fields */
		memcpy(newinfo, oldinfo, sizeof(RestrictInfo));

		/* Recursively fix the clause itself */
		newinfo->clause = (Expr *)
			adjust_appendrel_attrs_mutator((Node *) oldinfo->clause, context);

		/* and the modified version, if an OR clause */
		newinfo->orclause = (Expr *)
			adjust_appendrel_attrs_mutator((Node *) oldinfo->orclause, context);

		/* adjust relid sets too */
		newinfo->clause_relids = adjust_relid_set(oldinfo->clause_relids,
												  appinfo->parent_relid,
												  appinfo->child_relid);
		newinfo->required_relids = adjust_relid_set(oldinfo->required_relids,
													appinfo->parent_relid,
													appinfo->child_relid);
		newinfo->outer_relids = adjust_relid_set(oldinfo->outer_relids,
												 appinfo->parent_relid,
												 appinfo->child_relid);
		newinfo->nullable_relids = adjust_relid_set(oldinfo->nullable_relids,
													appinfo->parent_relid,
													appinfo->child_relid);
		newinfo->left_relids = adjust_relid_set(oldinfo->left_relids,
												appinfo->parent_relid,
												appinfo->child_relid);
		newinfo->right_relids = adjust_relid_set(oldinfo->right_relids,
												 appinfo->parent_relid,
												 appinfo->child_relid);

		/*
		 * Reset cached derivative fields, since these might need to have
		 * different values when considering the child relation.  Note we
		 * don't reset left_ec/right_ec: each child variable is implicitly
		 * equivalent to its parent, so still a member of the same EC if any.
		 */
		newinfo->eval_cost.startup = -1;
		newinfo->norm_selec = -1;
		newinfo->outer_selec = -1;
		newinfo->left_em = NULL;
		newinfo->right_em = NULL;
		newinfo->scansel_cache = NIL;
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

	return expression_tree_mutator(node, adjust_appendrel_attrs_mutator,
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
adjust_inherited_tlist(List *tlist, AppendRelInfo *context)
{
	bool		changed_it = false;
	ListCell   *tl;
	List	   *new_tlist;
	bool		more;
	int			attrno;

	/* This should only happen for an inheritance case, not UNION ALL */
	Assert(OidIsValid(context->parent_reloid));

	/* Scan tlist and update resnos to match attnums of child rel */
	foreach(tl, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);
		Var		   *childvar;

		if (tle->resjunk)
			continue;			/* ignore junk items */

		/* Look up the translation of this column: it must be a Var */
		if (tle->resno <= 0 ||
			tle->resno > list_length(context->translated_vars))
			elog(ERROR, "attribute %d of relation \"%s\" does not exist",
				 tle->resno, get_rel_name(context->parent_reloid));
		childvar = (Var *) list_nth(context->translated_vars, tle->resno - 1);
		if (childvar == NULL || !IsA(childvar, Var))
			elog(ERROR, "attribute %d of relation \"%s\" does not exist",
				 tle->resno, get_rel_name(context->parent_reloid));

		if (tle->resno != childvar->varattno)
		{
			tle->resno = childvar->varattno;
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
