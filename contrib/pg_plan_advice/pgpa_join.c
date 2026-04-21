/*-------------------------------------------------------------------------
 *
 * pgpa_join.c
 *	  analysis of joins in Plan trees
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_join.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pgpa_join.h"
#include "pgpa_scan.h"
#include "pgpa_walker.h"

#include "nodes/pathnodes.h"
#include "nodes/print.h"
#include "parser/parsetree.h"

/*
 * Temporary object used when unrolling a join tree.
 */
struct pgpa_join_unroller
{
	unsigned	nallocated;
	unsigned	nused;
	Plan	   *outer_subplan;
	ElidedNode *outer_elided_node;
	bool		outer_beneath_any_gather;
	pgpa_join_strategy *strategy;
	Plan	  **inner_subplans;
	ElidedNode **inner_elided_nodes;
	pgpa_join_unroller **inner_unrollers;
	bool	   *inner_beneath_any_gather;
};

static pgpa_join_strategy pgpa_decompose_join(pgpa_plan_walker_context *walker,
											  Plan *plan,
											  Plan **realouter,
											  Plan **realinner,
											  ElidedNode **elidedrealouter,
											  ElidedNode **elidedrealinner,
											  bool *found_any_outer_gather,
											  bool *found_any_inner_gather);
static ElidedNode *pgpa_descend_node(PlannedStmt *pstmt, Plan **plan);
static ElidedNode *pgpa_descend_any_gather(PlannedStmt *pstmt, Plan **plan,
										   bool *found_any_gather);
static bool pgpa_descend_any_unique(PlannedStmt *pstmt, Plan **plan,
									ElidedNode **elided_node);

static bool is_result_node_with_child(Plan *plan);
static bool is_sorting_plan(Plan *plan);

/*
 * Create an initially-empty object for unrolling joins.
 *
 * This function creates a helper object that can later be used to create a
 * pgpa_unrolled_join, after first calling pgpa_unroll_join one or more times.
 */
pgpa_join_unroller *
pgpa_create_join_unroller(void)
{
	pgpa_join_unroller *join_unroller;

	join_unroller = palloc0_object(pgpa_join_unroller);
	join_unroller->nallocated = 4;
	join_unroller->strategy =
		palloc_array(pgpa_join_strategy, join_unroller->nallocated);
	join_unroller->inner_subplans =
		palloc_array(Plan *, join_unroller->nallocated);
	join_unroller->inner_elided_nodes =
		palloc_array(ElidedNode *, join_unroller->nallocated);
	join_unroller->inner_unrollers =
		palloc_array(pgpa_join_unroller *, join_unroller->nallocated);
	join_unroller->inner_beneath_any_gather =
		palloc_array(bool, join_unroller->nallocated);

	return join_unroller;
}

/*
 * Unroll one level of an unrollable join tree.
 *
 * Our basic goal here is to unroll join trees as they occur in the Plan
 * tree into a simpler and more regular structure that we can more easily
 * use for further processing. Unrolling is outer-deep, so if the plan tree
 * has Join1(Join2(A,B),Join3(C,D)), the same join unroller object should be
 * used for Join1 and Join2, but a different one will be needed for Join3,
 * since that involves a join within the *inner* side of another join.
 *
 * pgpa_plan_walker creates a "top level" join unroller object when it
 * encounters a join in a portion of the plan tree in which no join unroller
 * is already active. From there, this function is responsible for determining
 * to what portion of the plan tree that join unroller applies, and for
 * creating any subordinate join unroller objects that are needed as a result
 * of non-outer-deep join trees. We do this by returning the join unroller
 * objects that should be used for further traversal of the outer and inner
 * subtrees of the current plan node via *outer_join_unroller and
 * *inner_join_unroller, respectively.
 */
void
pgpa_unroll_join(pgpa_plan_walker_context *walker, Plan *plan,
				 bool beneath_any_gather,
				 pgpa_join_unroller *join_unroller,
				 pgpa_join_unroller **outer_join_unroller,
				 pgpa_join_unroller **inner_join_unroller)
{
	pgpa_join_strategy strategy;
	Plan	   *realinner,
			   *realouter;
	ElidedNode *elidedinner,
			   *elidedouter;
	int			n;
	bool		found_any_outer_gather = false;
	bool		found_any_inner_gather = false;

	Assert(join_unroller != NULL);

	/*
	 * We need to pass the join_unroller object down through certain types of
	 * plan nodes -- anything that's considered part of the join strategy, and
	 * any other nodes that can occur in a join tree despite not being scans
	 * or joins.
	 *
	 * This includes:
	 *
	 * (1) Materialize, Memoize, and Hash nodes, which are part of the join
	 * strategy,
	 *
	 * (2) Gather and Gather Merge nodes, which can occur at any point in the
	 * join tree where the planner decided to initiate parallelism,
	 *
	 * (3) Sort and IncrementalSort nodes, which can occur beneath MergeJoin
	 * or GatherMerge,
	 *
	 * (4) Agg and Unique nodes, which can occur when we decide to make the
	 * nullable side of a semijoin unique and then join the result, and
	 *
	 * (5) Result nodes with children, which can be added either to project to
	 * enforce a one-time filter (but Result nodes without children are
	 * degenerate scans or joins).
	 */
	if (IsA(plan, Material) || IsA(plan, Memoize) || IsA(plan, Hash)
		|| IsA(plan, Gather) || IsA(plan, GatherMerge)
		|| is_sorting_plan(plan) || IsA(plan, Agg) || IsA(plan, Unique)
		|| is_result_node_with_child(plan))
	{
		*outer_join_unroller = join_unroller;
		return;
	}

	/*
	 * Since we've already handled nodes that require pass-through treatment,
	 * this should be an unrollable join.
	 */
	strategy = pgpa_decompose_join(walker, plan,
								   &realouter, &realinner,
								   &elidedouter, &elidedinner,
								   &found_any_outer_gather,
								   &found_any_inner_gather);

	/* If our workspace is full, expand it. */
	if (join_unroller->nused >= join_unroller->nallocated)
	{
		join_unroller->nallocated *= 2;
		join_unroller->strategy =
			repalloc_array(join_unroller->strategy,
						   pgpa_join_strategy,
						   join_unroller->nallocated);
		join_unroller->inner_subplans =
			repalloc_array(join_unroller->inner_subplans,
						   Plan *,
						   join_unroller->nallocated);
		join_unroller->inner_elided_nodes =
			repalloc_array(join_unroller->inner_elided_nodes,
						   ElidedNode *,
						   join_unroller->nallocated);
		join_unroller->inner_beneath_any_gather =
			repalloc_array(join_unroller->inner_beneath_any_gather,
						   bool,
						   join_unroller->nallocated);
		join_unroller->inner_unrollers =
			repalloc_array(join_unroller->inner_unrollers,
						   pgpa_join_unroller *,
						   join_unroller->nallocated);
	}

	/*
	 * Since we're flattening outer-deep join trees, it follows that if the
	 * outer side is still an unrollable join, it should be unrolled into this
	 * same object. Otherwise, we've reached the limit of what we can unroll
	 * into this object and must remember the outer side as the final outer
	 * subplan.
	 */
	if (elidedouter == NULL && pgpa_is_join(realouter))
		*outer_join_unroller = join_unroller;
	else
	{
		join_unroller->outer_subplan = realouter;
		join_unroller->outer_elided_node = elidedouter;
		join_unroller->outer_beneath_any_gather =
			beneath_any_gather || found_any_outer_gather;
	}

	/*
	 * Store the inner subplan. If it's an unrollable join, it needs to be
	 * flattened in turn, but into a new unroller object, not this one.
	 */
	n = join_unroller->nused++;
	join_unroller->strategy[n] = strategy;
	join_unroller->inner_subplans[n] = realinner;
	join_unroller->inner_elided_nodes[n] = elidedinner;
	join_unroller->inner_beneath_any_gather[n] =
		beneath_any_gather || found_any_inner_gather;
	if (elidedinner == NULL && pgpa_is_join(realinner))
		*inner_join_unroller = pgpa_create_join_unroller();
	else
		*inner_join_unroller = NULL;
	join_unroller->inner_unrollers[n] = *inner_join_unroller;
}

/*
 * Use the data we've accumulated in a pgpa_join_unroller object to construct
 * a pgpa_unrolled_join.
 */
pgpa_unrolled_join *
pgpa_build_unrolled_join(pgpa_plan_walker_context *walker,
						 pgpa_join_unroller *join_unroller)
{
	pgpa_unrolled_join *ujoin;
	int			i;

	/*
	 * We shouldn't have gone even so far as to create a join unroller unless
	 * we found at least one unrollable join.
	 */
	Assert(join_unroller->nused > 0);

	/* Allocate result structures. */
	ujoin = palloc0_object(pgpa_unrolled_join);
	ujoin->ninner = join_unroller->nused;
	ujoin->strategy = palloc0_array(pgpa_join_strategy, join_unroller->nused);
	ujoin->inner = palloc0_array(pgpa_join_member, join_unroller->nused);

	/* Handle the outermost join. */
	ujoin->outer.plan = join_unroller->outer_subplan;
	ujoin->outer.elided_node = join_unroller->outer_elided_node;
	ujoin->outer.scan =
		pgpa_build_scan(walker, ujoin->outer.plan,
						ujoin->outer.elided_node,
						join_unroller->outer_beneath_any_gather,
						true);

	/*
	 * We want the joins from the deepest part of the plan tree to appear
	 * first in the result object, but the join unroller adds them in exactly
	 * the reverse of that order, so we need to flip the order of the arrays
	 * when constructing the final result.
	 */
	for (i = 0; i < join_unroller->nused; ++i)
	{
		int			k = join_unroller->nused - i - 1;

		/* Copy strategy, Plan, and ElidedNode. */
		ujoin->strategy[i] = join_unroller->strategy[k];
		ujoin->inner[i].plan = join_unroller->inner_subplans[k];
		ujoin->inner[i].elided_node = join_unroller->inner_elided_nodes[k];

		/*
		 * Fill in remaining details, using either the nested join unroller,
		 * or by deriving them from the plan and elided nodes.
		 */
		if (join_unroller->inner_unrollers[k] != NULL)
			ujoin->inner[i].unrolled_join =
				pgpa_build_unrolled_join(walker,
										 join_unroller->inner_unrollers[k]);
		else
			ujoin->inner[i].scan =
				pgpa_build_scan(walker, ujoin->inner[i].plan,
								ujoin->inner[i].elided_node,
								join_unroller->inner_beneath_any_gather[k],
								true);
	}

	return ujoin;
}

/*
 * Free memory allocated for pgpa_join_unroller.
 */
void
pgpa_destroy_join_unroller(pgpa_join_unroller *join_unroller)
{
	pfree(join_unroller->strategy);
	pfree(join_unroller->inner_subplans);
	pfree(join_unroller->inner_elided_nodes);
	pfree(join_unroller->inner_unrollers);
	pfree(join_unroller->inner_beneath_any_gather);
	pfree(join_unroller);
}

/*
 * Identify the join strategy used by a join and the "real" inner and outer
 * plans.
 *
 * For example, a Hash Join always has a Hash node on the inner side, but
 * for all intents and purposes the real inner input is the Hash node's child,
 * not the Hash node itself.
 *
 * Likewise, a Merge Join may have Sort node on the inner or outer side; if
 * it does, the real input to the join is the Sort node's child, not the
 * Sort node itself.
 *
 * In addition, with a Merge Join or a Nested Loop, the join planning code
 * may add additional nodes such as Materialize or Memoize. We regard these
 * as an aspect of the join strategy. As in the previous cases, the true input
 * to the join is the underlying node.
 *
 * However, if any involved child node previously had a now-elided node stacked
 * on top, then we can't "look through" that node -- indeed, what's going to be
 * relevant for our purposes is the ElidedNode on top of that plan node, rather
 * than the plan node itself.
 *
 * If there are multiple elided nodes, we want that one that would have been
 * uppermost in the plan tree prior to setrefs processing; we expect to find
 * that one last in the list of elided nodes.
 *
 * On return *realouter and *realinner will have been set to the real inner
 * and real outer plans that we identified, and *elidedrealouter and
 * *elidedrealinner to the last of any corresponding elided nodes.
 * Additionally, *found_any_outer_gather and *found_any_inner_gather will
 * be set to true if we looked through a Gather or Gather Merge node on
 * that side of the join, and false otherwise.
 */
static pgpa_join_strategy
pgpa_decompose_join(pgpa_plan_walker_context *walker, Plan *plan,
					Plan **realouter, Plan **realinner,
					ElidedNode **elidedrealouter, ElidedNode **elidedrealinner,
					bool *found_any_outer_gather, bool *found_any_inner_gather)
{
	PlannedStmt *pstmt = walker->pstmt;
	JoinType	jointype = ((Join *) plan)->jointype;
	Plan	   *outerplan = plan->lefttree;
	Plan	   *innerplan = plan->righttree;
	ElidedNode *elidedouter;
	ElidedNode *elidedinner;
	pgpa_join_strategy strategy;
	bool		uniqueouter;
	bool		uniqueinner;

	elidedouter = pgpa_last_elided_node(pstmt, outerplan);
	elidedinner = pgpa_last_elided_node(pstmt, innerplan);
	*found_any_outer_gather = false;
	*found_any_inner_gather = false;

	switch (nodeTag(plan))
	{
		case T_MergeJoin:

			/*
			 * The planner may have chosen to place a Material node on the
			 * inner side of the MergeJoin; if this is present, we record it
			 * as part of the join strategy. (However, scan-level Materialize
			 * nodes are an exception.)
			 */
			if (elidedinner == NULL && IsA(innerplan, Material) &&
				!pgpa_is_scan_level_materialize(innerplan))
			{
				elidedinner = pgpa_descend_node(pstmt, &innerplan);
				strategy = JSTRAT_MERGE_JOIN_MATERIALIZE;
			}
			else
				strategy = JSTRAT_MERGE_JOIN_PLAIN;

			/*
			 * For a MergeJoin, either the outer or the inner subplan, or
			 * both, may have needed to be sorted; we must disregard any Sort
			 * or IncrementalSort node to find the real inner or outer
			 * subplan.
			 */
			if (elidedouter == NULL && is_sorting_plan(outerplan))
				elidedouter = pgpa_descend_node(pstmt, &outerplan);
			if (elidedinner == NULL && is_sorting_plan(innerplan))
				elidedinner = pgpa_descend_node(pstmt, &innerplan);
			break;

		case T_NestLoop:

			/*
			 * The planner may have chosen to place a Material or Memoize node
			 * on the inner side of the NestLoop; if this is present, we
			 * record it as part of the join strategy. (However, scan-level
			 * Materialize nodes are an exception.)
			 */
			if (elidedinner == NULL && IsA(innerplan, Material) &&
				!pgpa_is_scan_level_materialize(innerplan))
			{
				elidedinner = pgpa_descend_node(pstmt, &innerplan);
				strategy = JSTRAT_NESTED_LOOP_MATERIALIZE;
			}
			else if (elidedinner == NULL && IsA(innerplan, Memoize))
			{
				elidedinner = pgpa_descend_node(pstmt, &innerplan);
				strategy = JSTRAT_NESTED_LOOP_MEMOIZE;
			}
			else
				strategy = JSTRAT_NESTED_LOOP_PLAIN;
			break;

		case T_HashJoin:

			/*
			 * The inner subplan of a HashJoin is always a Hash node; the real
			 * inner subplan is the Hash node's child.
			 */
			Assert(IsA(innerplan, Hash));
			Assert(elidedinner == NULL);
			elidedinner = pgpa_descend_node(pstmt, &innerplan);
			strategy = JSTRAT_HASH_JOIN;
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(plan));
	}

	/*
	 * The planner may have decided to implement a semijoin by first making
	 * the nullable side of the plan unique, and then performing a normal join
	 * against the result. Therefore, we might need to descend through a
	 * unique node on either side of the plan.
	 */
	uniqueouter = pgpa_descend_any_unique(pstmt, &outerplan, &elidedouter);
	uniqueinner = pgpa_descend_any_unique(pstmt, &innerplan, &elidedinner);

	/*
	 * Can we see a Result node here, to project above a Gather? So far I've
	 * found no example that behaves that way; rather, the Gather or Gather
	 * Merge is made to project. Hence, don't test is_result_node_with_child()
	 * at this point.
	 */

	/*
	 * The planner may have decided to parallelize part of the join tree, so
	 * we could find a Gather or Gather Merge node here. Note that, if
	 * present, this will appear below nodes we considered as part of the join
	 * strategy, but we could find another uniqueness-enforcing node below the
	 * Gather or Gather Merge, if present.
	 */
	if (elidedouter == NULL)
	{
		elidedouter = pgpa_descend_any_gather(pstmt, &outerplan,
											  found_any_outer_gather);
		if (*found_any_outer_gather &&
			pgpa_descend_any_unique(pstmt, &outerplan, &elidedouter))
			uniqueouter = true;
	}
	if (elidedinner == NULL)
	{
		elidedinner = pgpa_descend_any_gather(pstmt, &innerplan,
											  found_any_inner_gather);
		if (*found_any_inner_gather &&
			pgpa_descend_any_unique(pstmt, &innerplan, &elidedinner))
			uniqueinner = true;
	}

	/*
	 * It's possible that a Result node has been inserted either to project a
	 * target list or to implement a one-time filter. If so, we can descend
	 * through it. Note that a Result node without a child would be a
	 * degenerate scan or join, and not something we could descend through.
	 */
	if (elidedouter == NULL && is_result_node_with_child(outerplan))
		elidedouter = pgpa_descend_node(pstmt, &outerplan);
	if (elidedinner == NULL && is_result_node_with_child(innerplan))
		elidedinner = pgpa_descend_node(pstmt, &innerplan);

	/*
	 * If this is a semijoin that was converted to an inner join by making one
	 * side or the other unique, make a note that the inner or outer subplan,
	 * as appropriate, should be treated as a query plan feature when the main
	 * tree traversal reaches it.
	 *
	 * Conversely, if the planner could have made one side of the join unique
	 * and thereby converted it to an inner join, and chose not to do so, that
	 * is also worth noting.
	 *
	 * NB: This code could appear slightly higher up in this function, but
	 * none of the nodes through which we just descended should have
	 * associated RTIs.
	 *
	 * NB: This seems like a somewhat hacky way of passing information up to
	 * the main tree walk, but I don't currently have a better idea.
	 */
	if (uniqueouter)
		pgpa_add_future_feature(walker, PGPAQF_SEMIJOIN_UNIQUE, outerplan);
	else if (jointype == JOIN_RIGHT_SEMI)
		pgpa_add_future_feature(walker, PGPAQF_SEMIJOIN_NON_UNIQUE, outerplan);
	if (uniqueinner)
		pgpa_add_future_feature(walker, PGPAQF_SEMIJOIN_UNIQUE, innerplan);
	else if (jointype == JOIN_SEMI)
		pgpa_add_future_feature(walker, PGPAQF_SEMIJOIN_NON_UNIQUE, innerplan);

	/* Set output parameters. */
	*realouter = outerplan;
	*realinner = innerplan;
	*elidedrealouter = elidedouter;
	*elidedrealinner = elidedinner;
	return strategy;
}

/*
 * Descend through a Plan node in a join tree that the caller has determined
 * to be irrelevant.
 *
 * Updates *plan, and returns the last of any elided nodes pertaining to the
 * new plan node.
 */
static ElidedNode *
pgpa_descend_node(PlannedStmt *pstmt, Plan **plan)
{
	*plan = (*plan)->lefttree;
	return pgpa_last_elided_node(pstmt, *plan);
}

/*
 * Descend through a Gather or Gather Merge node, if present, and any Sort
 * or IncrementalSort node occurring under a Gather Merge.
 *
 * Caller should have verified that there is no ElidedNode pertaining to
 * the initial value of *plan.
 *
 * Updates *plan, and returns the last of any elided nodes pertaining to the
 * new plan node. Sets *found_any_gather = true if either Gather or
 * Gather Merge was found, and otherwise leaves it unchanged.
 */
static ElidedNode *
pgpa_descend_any_gather(PlannedStmt *pstmt, Plan **plan,
						bool *found_any_gather)
{
	if (IsA(*plan, Gather))
	{
		*found_any_gather = true;
		return pgpa_descend_node(pstmt, plan);
	}

	if (IsA(*plan, GatherMerge))
	{
		ElidedNode *elided = pgpa_descend_node(pstmt, plan);

		if (elided == NULL && is_sorting_plan(*plan))
			elided = pgpa_descend_node(pstmt, plan);

		*found_any_gather = true;
		return elided;
	}

	return NULL;
}

/*
 * If *plan is an Agg or Unique node, we want to descend through it, unless
 * it has a corresponding elided node. If its immediate child is a Sort or
 * IncrementalSort, we also want to descend through that, unless it has a
 * corresponding elided node.
 *
 * On entry, *elided_node must be the last of any elided nodes corresponding
 * to *plan; on exit, this will still be true, but *plan may have been updated.
 *
 * The reason we don't want to descend through elided nodes is that a single
 * join tree can't cross through any sort of elided node: subqueries are
 * planned separately, and planning inside an Append or MergeAppend is
 * separate from planning outside of it.
 *
 * The return value is true if we descend through a node that we believe is
 * making one side of a semijoin unique, and otherwise false.
 */
static bool
pgpa_descend_any_unique(PlannedStmt *pstmt, Plan **plan,
						ElidedNode **elided_node)
{
	bool		descend = false;
	bool		sjunique = false;

	if (*elided_node != NULL)
		return sjunique;

	if (IsA(*plan, Unique))
	{
		descend = true;
		sjunique = true;
	}
	else if (IsA(*plan, Agg))
	{
		/*
		 * If this is a simple Agg node, then assume it's here to implement
		 * semijoin uniqueness. Otherwise, assume it's completing an eager
		 * aggregation or partitionwise aggregation operation that began at a
		 * higher level of the plan tree.
		 *
		 * (Note that when we're using an Agg node for uniqueness, there's no
		 * need for any case other than AGGSPLIT_SIMPLE, because there's no
		 * aggregated column being computed. However, the fact that
		 * AGGSPLIT_SIMPLE is in use doesn't prove that this Agg is here for
		 * the semijoin uniqueness. Maybe we should adjust an Agg node to
		 * carry a "purpose" field so that code like this can be more certain
		 * of its analysis.)
		 */
		descend = true;
		sjunique = (((Agg *) *plan)->aggsplit == AGGSPLIT_SIMPLE);
	}

	if (descend)
	{
		*elided_node = pgpa_descend_node(pstmt, plan);

		if (*elided_node == NULL && is_sorting_plan(*plan))
			*elided_node = pgpa_descend_node(pstmt, plan);
	}

	return sjunique;
}

/*
 * Is this a Result node that has a child?
 */
static bool
is_result_node_with_child(Plan *plan)
{
	return IsA(plan, Result) && plan->lefttree != NULL;
}

/*
 * Is this a Plan node whose purpose is to put the data in a certain order?
 */
static bool
is_sorting_plan(Plan *plan)
{
	return IsA(plan, Sort) || IsA(plan, IncrementalSort);
}
