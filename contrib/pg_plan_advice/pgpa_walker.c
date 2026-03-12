/*-------------------------------------------------------------------------
 *
 * pgpa_walker.c
 *	  Main entrypoints for analyzing a plan to generate an advice string
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_walker.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pgpa_join.h"
#include "pgpa_scan.h"
#include "pgpa_walker.h"

#include "nodes/plannodes.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

static void pgpa_walk_recursively(pgpa_plan_walker_context *walker, Plan *plan,
								  bool within_join_problem,
								  pgpa_join_unroller *join_unroller,
								  List *active_query_features,
								  bool beneath_any_gather);
static Bitmapset *pgpa_process_unrolled_join(pgpa_plan_walker_context *walker,
											 pgpa_unrolled_join *ujoin);

static pgpa_query_feature *pgpa_add_feature(pgpa_plan_walker_context *walker,
											pgpa_qf_type type,
											Plan *plan);

static void pgpa_qf_add_rti(List *active_query_features, Index rti);
static void pgpa_qf_add_rtis(List *active_query_features, Bitmapset *relids);
static void pgpa_qf_add_plan_rtis(List *active_query_features, Plan *plan,
								  List *rtable);

static bool pgpa_walker_join_order_matches(pgpa_unrolled_join *ujoin,
										   Index rtable_length,
										   pgpa_identifier *rt_identifiers,
										   pgpa_advice_target *target,
										   bool toplevel);
static bool pgpa_walker_join_order_matches_member(pgpa_join_member *member,
												  Index rtable_length,
												  pgpa_identifier *rt_identifiers,
												  pgpa_advice_target *target);
static pgpa_scan *pgpa_walker_find_scan(pgpa_plan_walker_context *walker,
										pgpa_scan_strategy strategy,
										Bitmapset *relids);
static bool pgpa_walker_index_target_matches_plan(pgpa_index_target *itarget,
												  Plan *plan);
static bool pgpa_walker_contains_feature(pgpa_plan_walker_context *walker,
										 pgpa_qf_type type,
										 Bitmapset *relids);
static bool pgpa_walker_contains_join(pgpa_plan_walker_context *walker,
									  pgpa_join_strategy strategy,
									  Bitmapset *relids);
static bool pgpa_walker_contains_no_gather(pgpa_plan_walker_context *walker,
										   Bitmapset *relids);

/*
 * Top-level entrypoint for the plan tree walk.
 *
 * Populates walker based on a traversal of the Plan trees in pstmt.
 *
 * sj_unique_rels is a list of pgpa_sj_unique_rel objects, one for each
 * relation we considered making unique as part of semijoin planning.
 */
void
pgpa_plan_walker(pgpa_plan_walker_context *walker, PlannedStmt *pstmt,
				 List *sj_unique_rels)
{
	ListCell   *lc;
	List	   *sj_unique_rtis = NULL;
	List	   *sj_nonunique_qfs = NULL;

	/* Initialization. */
	memset(walker, 0, sizeof(pgpa_plan_walker_context));
	walker->pstmt = pstmt;

	/* Walk the main plan tree. */
	pgpa_walk_recursively(walker, pstmt->planTree, false, NULL, NIL, false);

	/* Main plan tree walk won't reach subplans, so walk those. */
	foreach(lc, pstmt->subplans)
	{
		Plan	   *plan = lfirst(lc);

		if (plan != NULL)
			pgpa_walk_recursively(walker, plan, false, NULL, NIL, false);
	}

	/* Adjust RTIs from sj_unique_rels for the flattened range table. */
	foreach_ptr(pgpa_sj_unique_rel, ur, sj_unique_rels)
	{
		int			rtindex = -1;
		int			rtoffset = 0;
		bool		dummy = false;
		Bitmapset  *relids = NULL;

		/* If this is a subplan, find the range table offset. */
		if (ur->plan_name != NULL)
		{
			foreach_node(SubPlanRTInfo, rtinfo, pstmt->subrtinfos)
			{
				if (strcmp(ur->plan_name, rtinfo->plan_name) == 0)
				{
					rtoffset = rtinfo->rtoffset;
					dummy = rtinfo->dummy;
					break;
				}
			}

			if (rtoffset == 0)
				elog(ERROR, "no rtoffset for plan %s", ur->plan_name);
		}

		/* If this entry pertains to a dummy subquery, ignore it. */
		if (dummy)
			continue;

		/* Offset each entry from the original set. */
		while ((rtindex = bms_next_member(ur->relids, rtindex)) >= 0)
			relids = bms_add_member(relids, rtindex + rtoffset);

		/* Store the resulting set. */
		sj_unique_rtis = lappend(sj_unique_rtis, relids);
	}

	/*
	 * Remove any non-unique semijoin query features for which making the rel
	 * unique wasn't considered.
	 */
	foreach_ptr(pgpa_query_feature, qf,
				walker->query_features[PGPAQF_SEMIJOIN_NON_UNIQUE])
	{
		if (list_member(sj_unique_rtis, qf->relids))
			sj_nonunique_qfs = lappend(sj_nonunique_qfs, qf);
	}
	walker->query_features[PGPAQF_SEMIJOIN_NON_UNIQUE] = sj_nonunique_qfs;

	/*
	 * If we find any cases where analysis of the Plan tree shows that the
	 * semijoin was made unique but this possibility was never observed to be
	 * considered during planning, then we have a bug somewhere.
	 */
	foreach_ptr(pgpa_query_feature, qf,
				walker->query_features[PGPAQF_SEMIJOIN_UNIQUE])
	{
		if (!list_member(sj_unique_rtis, qf->relids))
		{
			StringInfoData buf;

			initStringInfo(&buf);
			outBitmapset(&buf, qf->relids);
			elog(ERROR,
				 "unique semijoin found for relids %s but not observed during planning",
				 buf.data);
		}
	}

	/*
	 * It's possible for a Gather or Gather Merge query feature to find no
	 * RTIs when partitionwise aggregation is in use. We shouldn't emit
	 * something like GATHER_MERGE(()), so instead emit nothing. This means
	 * that we won't advise either GATHER or GATHER_MERGE or NO_GATHER in such
	 * cases, which might be something we want to improve in the future.
	 *
	 * (Should the Partial Aggregates in such a case be created in an
	 * UPPERREL_GROUP_AGG with a non-empty relid set? Right now that doesn't
	 * happen, but it seems like it would make life easier for us if it did.)
	 */
	for (int t = 0; t < NUM_PGPA_QF_TYPES; ++t)
	{
		List	   *query_features = NIL;

		foreach_ptr(pgpa_query_feature, qf, walker->query_features[t])
		{
			if (qf->relids != NULL)
				query_features = lappend(query_features, qf);
			else
				Assert(t == PGPAQF_GATHER || t == PGPAQF_GATHER_MERGE);
		}

		walker->query_features[t] = query_features;
	}
}

/*
 * Main workhorse for the plan tree walk.
 *
 * If within_join_problem is true, we encountered a join at some higher level
 * of the tree walk and haven't yet descended out of the portion of the plan
 * tree that is part of that same join problem. We're no longer in the same
 * join problem if (1) we cross into a different subquery or (2) we descend
 * through an Append or MergeAppend node, below which any further joins would
 * be partitionwise joins planned separately from the outer join problem.
 *
 * If join_unroller != NULL, the join unroller code expects us to find a join
 * that should be unrolled into that object. This implies that we're within a
 * join problem, but the reverse is not true: when we've traversed all the
 * joins but are still looking for the scan that is the leaf of the join tree,
 * join_unroller will be NULL but within_join_problem will be true.
 *
 * Each element of active_query_features corresponds to some item of advice
 * that needs to enumerate all the relations it affects. We add RTIs we find
 * during tree traversal to each of these query features.
 *
 * If beneath_any_gather == true, some higher level of the tree traversal found
 * a Gather or Gather Merge node.
 */
static void
pgpa_walk_recursively(pgpa_plan_walker_context *walker, Plan *plan,
					  bool within_join_problem,
					  pgpa_join_unroller *join_unroller,
					  List *active_query_features,
					  bool beneath_any_gather)
{
	pgpa_join_unroller *outer_join_unroller = NULL;
	pgpa_join_unroller *inner_join_unroller = NULL;
	bool		join_unroller_toplevel = false;
	ListCell   *lc;
	List	   *extraplans = NIL;
	List	   *elided_nodes = NIL;

	Assert(within_join_problem || join_unroller == NULL);

	/*
	 * Check the future_query_features list to see whether this was previously
	 * identified as a plan node that needs to be treated as a query feature.
	 * We must do this before handling elided nodes, because if there's an
	 * elided node associated with a future query feature, the RTIs associated
	 * with the elided node should be the only ones attributed to the query
	 * feature.
	 */
	foreach_ptr(pgpa_query_feature, qf, walker->future_query_features)
	{
		if (qf->plan == plan)
		{
			active_query_features = list_copy(active_query_features);
			active_query_features = lappend(active_query_features, qf);
			walker->future_query_features =
				list_delete_ptr(walker->future_query_features, qf);
			break;
		}
	}

	/*
	 * Find all elided nodes for this Plan node.
	 */
	foreach_node(ElidedNode, n, walker->pstmt->elidedNodes)
	{
		if (n->plan_node_id == plan->plan_node_id)
			elided_nodes = lappend(elided_nodes, n);
	}

	/* If we found any elided_nodes, handle them. */
	if (elided_nodes != NIL)
	{
		int			num_elided_nodes = list_length(elided_nodes);
		ElidedNode *last_elided_node;

		/*
		 * RTIs for the final -- and thus logically uppermost -- elided node
		 * should be collected for query features passed down by the caller.
		 * However, elided nodes act as barriers to query features, which
		 * means that (1) the remaining elided nodes, if any, should be
		 * ignored for purposes of query features and (2) the list of active
		 * query features should be reset to empty so that we do not add RTIs
		 * from the plan node that is logically beneath the elided node to the
		 * query features passed down from the caller.
		 */
		last_elided_node = list_nth(elided_nodes, num_elided_nodes - 1);
		pgpa_qf_add_rtis(active_query_features,
						 pgpa_filter_out_join_relids(last_elided_node->relids,
													 walker->pstmt->rtable));
		active_query_features = NIL;

		/*
		 * If we're within a join problem, the join_unroller is responsible
		 * for building the scan for the final elided node, so throw it out.
		 */
		if (within_join_problem)
			elided_nodes = list_truncate(elided_nodes, num_elided_nodes - 1);

		/* Build scans for all (or the remaining) elided nodes. */
		foreach_node(ElidedNode, elided_node, elided_nodes)
		{
			(void) pgpa_build_scan(walker, plan, elided_node,
								   beneath_any_gather, within_join_problem);
		}

		/*
		 * If there were any elided nodes, then everything beneath those nodes
		 * is not part of the same join problem.
		 *
		 * In more detail, if an Append or MergeAppend was elided, then a
		 * partitionwise join was chosen and only a single child survived; if
		 * a SubqueryScan was elided, the subquery was planned without
		 * flattening it into the parent.
		 */
		within_join_problem = false;
		join_unroller = NULL;
	}

	/*
	 * If this is a Gather or Gather Merge node, directly add it to the list
	 * of currently-active query features. We must do this after handling
	 * elided nodes, since the Gather or Gather Merge node occurs logically
	 * beneath any associated elided nodes.
	 *
	 * Exception: We disregard any single_copy Gather nodes. These are created
	 * by debug_parallel_query, and having them affect the plan advice is
	 * counterproductive, as the result will be to advise the use of a real
	 * Gather node, rather than a single copy one.
	 */
	if (IsA(plan, Gather) && !((Gather *) plan)->single_copy)
	{
		active_query_features =
			lappend(list_copy(active_query_features),
					pgpa_add_feature(walker, PGPAQF_GATHER, plan));
		beneath_any_gather = true;
	}
	else if (IsA(plan, GatherMerge))
	{
		active_query_features =
			lappend(list_copy(active_query_features),
					pgpa_add_feature(walker, PGPAQF_GATHER_MERGE, plan));
		beneath_any_gather = true;
	}

	/*
	 * If we're within a join problem, the join unroller is responsible for
	 * building any required scan for this node. If not, we do it here.
	 */
	if (!within_join_problem)
		(void) pgpa_build_scan(walker, plan, NULL, beneath_any_gather, false);

	/*
	 * If this join needs to be unrolled but there's no join unroller already
	 * available, create one.
	 */
	if (join_unroller == NULL && pgpa_is_join(plan))
	{
		join_unroller = pgpa_create_join_unroller();
		join_unroller_toplevel = true;
		within_join_problem = true;
	}

	/*
	 * If this join is to be unrolled, pgpa_unroll_join() will return the join
	 * unroller object that should be passed down when we recurse into the
	 * outer and inner sides of the plan.
	 */
	if (join_unroller != NULL)
		pgpa_unroll_join(walker, plan, beneath_any_gather, join_unroller,
						 &outer_join_unroller, &inner_join_unroller);

	/* Add RTIs from the plan node to all active query features. */
	pgpa_qf_add_plan_rtis(active_query_features, plan, walker->pstmt->rtable);

	/*
	 * Recurse into the outer and inner subtrees.
	 *
	 * As an exception, if this is a ForeignScan, don't recurse. postgres_fdw
	 * sometimes stores an EPQ recheck plan in plan->lefttree, but that's
	 * going to mention the same set of relations as the ForeignScan itself,
	 * and we have no way to emit advice targeting the EPQ case vs. the
	 * non-EPQ case. Moreover, it's not entirely clear what other FDWs might
	 * do with the left and right subtrees. Maybe some better handling is
	 * needed here, but for now, we just punt.
	 */
	if (!IsA(plan, ForeignScan))
	{
		if (plan->lefttree != NULL)
			pgpa_walk_recursively(walker, plan->lefttree, within_join_problem,
								  outer_join_unroller, active_query_features,
								  beneath_any_gather);
		if (plan->righttree != NULL)
			pgpa_walk_recursively(walker, plan->righttree, within_join_problem,
								  inner_join_unroller, active_query_features,
								  beneath_any_gather);
	}

	/*
	 * If we created a join unroller up above, then it's also our join to use
	 * it to build the final pgpa_unrolled_join, and to destroy the object.
	 */
	if (join_unroller_toplevel)
	{
		pgpa_unrolled_join *ujoin;

		ujoin = pgpa_build_unrolled_join(walker, join_unroller);
		walker->toplevel_unrolled_joins =
			lappend(walker->toplevel_unrolled_joins, ujoin);
		pgpa_destroy_join_unroller(join_unroller);
		(void) pgpa_process_unrolled_join(walker, ujoin);
	}

	/*
	 * Some plan types can have additional children. Nodes like Append that
	 * can have any number of children store them in a List; a SubqueryScan
	 * just has a field for a single additional Plan.
	 */
	switch (nodeTag(plan))
	{
		case T_Append:
			{
				Append	   *aplan = (Append *) plan;

				extraplans = aplan->appendplans;
			}
			break;
		case T_MergeAppend:
			{
				MergeAppend *maplan = (MergeAppend *) plan;

				extraplans = maplan->mergeplans;
			}
			break;
		case T_BitmapAnd:
			extraplans = ((BitmapAnd *) plan)->bitmapplans;
			break;
		case T_BitmapOr:
			extraplans = ((BitmapOr *) plan)->bitmapplans;
			break;
		case T_SubqueryScan:

			/*
			 * We don't pass down active_query_features across here, because
			 * those are specific to a subquery level.
			 */
			pgpa_walk_recursively(walker, ((SubqueryScan *) plan)->subplan,
								  0, NULL, NIL, beneath_any_gather);
			break;
		case T_CustomScan:
			extraplans = ((CustomScan *) plan)->custom_plans;
			break;
		default:
			break;
	}

	/* If we found a list of extra children, iterate over it. */
	foreach(lc, extraplans)
	{
		Plan	   *subplan = lfirst(lc);

		pgpa_walk_recursively(walker, subplan, false, NULL, NIL,
							  beneath_any_gather);
	}
}

/*
 * Perform final processing of a newly-constructed pgpa_unrolled_join. This
 * only needs to be called for toplevel pgpa_unrolled_join objects, since it
 * recurses to sub-joins as needed.
 *
 * Our goal is to add the set of inner relids to the relevant join_strategies
 * list, and to do the same for any sub-joins. To that end, the return value
 * is the set of relids found beneath the join, but it is expected that
 * the toplevel caller will ignore this.
 */
static Bitmapset *
pgpa_process_unrolled_join(pgpa_plan_walker_context *walker,
						   pgpa_unrolled_join *ujoin)
{
	Bitmapset  *all_relids = bms_copy(ujoin->outer.scan->relids);

	/* If this fails, we didn't unroll properly. */
	Assert(ujoin->outer.unrolled_join == NULL);

	for (int k = 0; k < ujoin->ninner; ++k)
	{
		pgpa_join_member *member = &ujoin->inner[k];
		Bitmapset  *relids;

		if (member->unrolled_join != NULL)
			relids = pgpa_process_unrolled_join(walker,
												member->unrolled_join);
		else
		{
			Assert(member->scan != NULL);
			relids = member->scan->relids;
		}
		walker->join_strategies[ujoin->strategy[k]] =
			lappend(walker->join_strategies[ujoin->strategy[k]], relids);
		all_relids = bms_add_members(all_relids, relids);
	}

	return all_relids;
}

/*
 * Arrange for the given plan node to be treated as a query feature when the
 * tree walk reaches it.
 *
 * Make sure to only use this for nodes that the tree walk can't have reached
 * yet!
 */
void
pgpa_add_future_feature(pgpa_plan_walker_context *walker,
						pgpa_qf_type type, Plan *plan)
{
	pgpa_query_feature *qf = pgpa_add_feature(walker, type, plan);

	walker->future_query_features =
		lappend(walker->future_query_features, qf);
}

/*
 * Return the last of any elided nodes associated with this plan node ID.
 *
 * The last elided node is the one that would have been uppermost in the plan
 * tree had it not been removed during setrefs processing.
 */
ElidedNode *
pgpa_last_elided_node(PlannedStmt *pstmt, Plan *plan)
{
	ElidedNode *elided_node = NULL;

	foreach_node(ElidedNode, n, pstmt->elidedNodes)
	{
		if (n->plan_node_id == plan->plan_node_id)
			elided_node = n;
	}

	return elided_node;
}

/*
 * Certain plan nodes can refer to a set of RTIs. Extract and return the set.
 */
Bitmapset *
pgpa_relids(Plan *plan)
{
	if (IsA(plan, Result))
		return ((Result *) plan)->relids;
	else if (IsA(plan, ForeignScan))
		return ((ForeignScan *) plan)->fs_relids;
	else if (IsA(plan, Append))
		return ((Append *) plan)->apprelids;
	else if (IsA(plan, MergeAppend))
		return ((MergeAppend *) plan)->apprelids;

	return NULL;
}

/*
 * Extract the scanned RTI from a plan node.
 *
 * Returns 0 if there isn't one.
 */
Index
pgpa_scanrelid(Plan *plan)
{
	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_NamedTuplestoreScan:
		case T_WorkTableScan:
		case T_ForeignScan:
		case T_CustomScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
			return ((Scan *) plan)->scanrelid;
		default:
			return 0;
	}
}

/*
 * Construct a new Bitmapset containing non-RTE_JOIN members of 'relids'.
 */
Bitmapset *
pgpa_filter_out_join_relids(Bitmapset *relids, List *rtable)
{
	int			rti = -1;
	Bitmapset  *result = NULL;

	while ((rti = bms_next_member(relids, rti)) >= 0)
	{
		RangeTblEntry *rte = rt_fetch(rti, rtable);

		if (rte->rtekind != RTE_JOIN)
			result = bms_add_member(result, rti);
	}

	return result;
}

/*
 * Create a pgpa_query_feature and add it to the list of all query features
 * for this plan.
 */
static pgpa_query_feature *
pgpa_add_feature(pgpa_plan_walker_context *walker,
				 pgpa_qf_type type, Plan *plan)
{
	pgpa_query_feature *qf = palloc0_object(pgpa_query_feature);

	qf->type = type;
	qf->plan = plan;

	walker->query_features[qf->type] =
		lappend(walker->query_features[qf->type], qf);

	return qf;
}

/*
 * Add a single RTI to each active query feature.
 */
static void
pgpa_qf_add_rti(List *active_query_features, Index rti)
{
	foreach_ptr(pgpa_query_feature, qf, active_query_features)
	{
		qf->relids = bms_add_member(qf->relids, rti);
	}
}

/*
 * Add a set of RTIs to each active query feature.
 */
static void
pgpa_qf_add_rtis(List *active_query_features, Bitmapset *relids)
{
	foreach_ptr(pgpa_query_feature, qf, active_query_features)
	{
		qf->relids = bms_add_members(qf->relids, relids);
	}
}

/*
 * Add RTIs directly contained in a plan node to each active query feature,
 * but filter out any join RTIs, since advice doesn't mention those.
 */
static void
pgpa_qf_add_plan_rtis(List *active_query_features, Plan *plan, List *rtable)
{
	Bitmapset  *relids;
	Index		rti;

	if ((relids = pgpa_relids(plan)) != NULL)
	{
		relids = pgpa_filter_out_join_relids(relids, rtable);
		pgpa_qf_add_rtis(active_query_features, relids);
	}
	else if ((rti = pgpa_scanrelid(plan)) != 0)
		pgpa_qf_add_rti(active_query_features, rti);
}

/*
 * If we generated plan advice using the provided walker object and array
 * of identifiers, would we generate the specified tag/target combination?
 *
 * If yes, the plan conforms to the advice; if no, it does not. Note that
 * we have no way of knowing whether the planner was forced to emit a plan
 * that conformed to the advice or just happened to do so.
 */
bool
pgpa_walker_would_advise(pgpa_plan_walker_context *walker,
						 pgpa_identifier *rt_identifiers,
						 pgpa_advice_tag_type tag,
						 pgpa_advice_target *target)
{
	Index		rtable_length = list_length(walker->pstmt->rtable);
	Bitmapset  *relids = NULL;

	if (tag == PGPA_TAG_JOIN_ORDER)
	{
		foreach_ptr(pgpa_unrolled_join, ujoin, walker->toplevel_unrolled_joins)
		{
			if (pgpa_walker_join_order_matches(ujoin, rtable_length,
											   rt_identifiers, target, true))
				return true;
		}

		return false;
	}

	if (target->ttype == PGPA_TARGET_IDENTIFIER)
	{
		Index		rti;

		rti = pgpa_compute_rti_from_identifier(rtable_length, rt_identifiers,
											   &target->rid);
		if (rti == 0)
			return false;
		relids = bms_make_singleton(rti);
	}
	else
	{
		Assert(target->ttype == PGPA_TARGET_ORDERED_LIST);
		foreach_ptr(pgpa_advice_target, child_target, target->children)
		{
			Index		rti;

			Assert(child_target->ttype == PGPA_TARGET_IDENTIFIER);
			rti = pgpa_compute_rti_from_identifier(rtable_length,
												   rt_identifiers,
												   &child_target->rid);
			if (rti == 0)
				return false;
			relids = bms_add_member(relids, rti);
		}
	}

	switch (tag)
	{
		case PGPA_TAG_JOIN_ORDER:
			/* should have been handled above */
			pg_unreachable();
			break;
		case PGPA_TAG_BITMAP_HEAP_SCAN:
			return pgpa_walker_find_scan(walker,
										 PGPA_SCAN_BITMAP_HEAP,
										 relids) != NULL;
		case PGPA_TAG_FOREIGN_JOIN:
			return pgpa_walker_find_scan(walker,
										 PGPA_SCAN_FOREIGN,
										 relids) != NULL;
		case PGPA_TAG_INDEX_ONLY_SCAN:
			{
				pgpa_scan  *scan;

				scan = pgpa_walker_find_scan(walker, PGPA_SCAN_INDEX_ONLY,
											 relids);
				if (scan == NULL)
					return false;

				return pgpa_walker_index_target_matches_plan(target->itarget, scan->plan);
			}
		case PGPA_TAG_INDEX_SCAN:
			{
				pgpa_scan  *scan;

				scan = pgpa_walker_find_scan(walker, PGPA_SCAN_INDEX,
											 relids);
				if (scan == NULL)
					return false;

				return pgpa_walker_index_target_matches_plan(target->itarget, scan->plan);
			}
		case PGPA_TAG_PARTITIONWISE:
			return pgpa_walker_find_scan(walker,
										 PGPA_SCAN_PARTITIONWISE,
										 relids) != NULL;
		case PGPA_TAG_SEQ_SCAN:
			return pgpa_walker_find_scan(walker,
										 PGPA_SCAN_SEQ,
										 relids) != NULL;
		case PGPA_TAG_TID_SCAN:
			return pgpa_walker_find_scan(walker,
										 PGPA_SCAN_TID,
										 relids) != NULL;
		case PGPA_TAG_GATHER:
			return pgpa_walker_contains_feature(walker,
												PGPAQF_GATHER,
												relids);
		case PGPA_TAG_GATHER_MERGE:
			return pgpa_walker_contains_feature(walker,
												PGPAQF_GATHER_MERGE,
												relids);
		case PGPA_TAG_SEMIJOIN_NON_UNIQUE:
			return pgpa_walker_contains_feature(walker,
												PGPAQF_SEMIJOIN_NON_UNIQUE,
												relids);
		case PGPA_TAG_SEMIJOIN_UNIQUE:
			return pgpa_walker_contains_feature(walker,
												PGPAQF_SEMIJOIN_UNIQUE,
												relids);
		case PGPA_TAG_HASH_JOIN:
			return pgpa_walker_contains_join(walker,
											 JSTRAT_HASH_JOIN,
											 relids);
		case PGPA_TAG_MERGE_JOIN_MATERIALIZE:
			return pgpa_walker_contains_join(walker,
											 JSTRAT_MERGE_JOIN_MATERIALIZE,
											 relids);
		case PGPA_TAG_MERGE_JOIN_PLAIN:
			return pgpa_walker_contains_join(walker,
											 JSTRAT_MERGE_JOIN_PLAIN,
											 relids);
		case PGPA_TAG_NESTED_LOOP_MATERIALIZE:
			return pgpa_walker_contains_join(walker,
											 JSTRAT_NESTED_LOOP_MATERIALIZE,
											 relids);
		case PGPA_TAG_NESTED_LOOP_MEMOIZE:
			return pgpa_walker_contains_join(walker,
											 JSTRAT_NESTED_LOOP_MEMOIZE,
											 relids);
		case PGPA_TAG_NESTED_LOOP_PLAIN:
			return pgpa_walker_contains_join(walker,
											 JSTRAT_NESTED_LOOP_PLAIN,
											 relids);
		case PGPA_TAG_NO_GATHER:
			return pgpa_walker_contains_no_gather(walker, relids);
	}

	/* should not get here */
	return false;
}

/*
 * Does the index target match the Plan?
 *
 * Should only be called when we know that itarget mandates an Index Scan or
 * Index Only Scan and this corresponds to the type of Plan. Here, our job is
 * just to check whether it's the same index.
 */
static bool
pgpa_walker_index_target_matches_plan(pgpa_index_target *itarget, Plan *plan)
{
	Oid			indexoid = InvalidOid;

	/* Retrieve the index OID from the plan. */
	if (IsA(plan, IndexScan))
		indexoid = ((IndexScan *) plan)->indexid;
	else if (IsA(plan, IndexOnlyScan))
		indexoid = ((IndexOnlyScan *) plan)->indexid;
	else
		elog(ERROR, "unrecognized node type: %d", (int) nodeTag(plan));

	/* Check whether schema name matches, if specified in index target. */
	if (itarget->indnamespace != NULL)
	{
		Oid			nspoid = get_rel_namespace(indexoid);
		char	   *relnamespace = get_namespace_name_or_temp(nspoid);

		if (strcmp(itarget->indnamespace, relnamespace) != 0)
			return false;
	}

	/* Check whether relation name matches. */
	return (strcmp(itarget->indname, get_rel_name(indexoid)) == 0);
}

/*
 * Does an unrolled join match the join order specified by an advice target?
 */
static bool
pgpa_walker_join_order_matches(pgpa_unrolled_join *ujoin,
							   Index rtable_length,
							   pgpa_identifier *rt_identifiers,
							   pgpa_advice_target *target,
							   bool toplevel)
{
	int			nchildren = list_length(target->children);

	Assert(target->ttype == PGPA_TARGET_ORDERED_LIST);

	/* At toplevel, we allow a prefix match. */
	if (toplevel)
	{
		if (nchildren > ujoin->ninner + 1)
			return false;
	}
	else
	{
		if (nchildren != ujoin->ninner + 1)
			return false;
	}

	/* Outermost rel must match. */
	if (!pgpa_walker_join_order_matches_member(&ujoin->outer,
											   rtable_length,
											   rt_identifiers,
											   linitial(target->children)))
		return false;

	/* Each inner rel must match. */
	for (int n = 0; n < nchildren - 1; ++n)
	{
		pgpa_advice_target *child_target = list_nth(target->children, n + 1);

		if (!pgpa_walker_join_order_matches_member(&ujoin->inner[n],
												   rtable_length,
												   rt_identifiers,
												   child_target))
			return false;
	}

	return true;
}

/*
 * Does one member of an unrolled join match an advice target?
 */
static bool
pgpa_walker_join_order_matches_member(pgpa_join_member *member,
									  Index rtable_length,
									  pgpa_identifier *rt_identifiers,
									  pgpa_advice_target *target)
{
	Bitmapset  *relids = NULL;

	if (member->unrolled_join != NULL)
	{
		if (target->ttype != PGPA_TARGET_ORDERED_LIST)
			return false;
		return pgpa_walker_join_order_matches(member->unrolled_join,
											  rtable_length,
											  rt_identifiers,
											  target,
											  false);
	}

	Assert(member->scan != NULL);
	switch (target->ttype)
	{
		case PGPA_TARGET_ORDERED_LIST:
			/* Could only match an unrolled join */
			return false;

		case PGPA_TARGET_UNORDERED_LIST:
			{
				foreach_ptr(pgpa_advice_target, child_target, target->children)
				{
					Index		rti;

					rti = pgpa_compute_rti_from_identifier(rtable_length,
														   rt_identifiers,
														   &child_target->rid);
					if (rti == 0)
						return false;
					relids = bms_add_member(relids, rti);
				}
				break;
			}

		case PGPA_TARGET_IDENTIFIER:
			{
				Index		rti;

				rti = pgpa_compute_rti_from_identifier(rtable_length,
													   rt_identifiers,
													   &target->rid);
				if (rti == 0)
					return false;
				relids = bms_make_singleton(rti);
				break;
			}
	}

	return bms_equal(member->scan->relids, relids);
}

/*
 * Find the scan where the walker says that the given scan strategy should be
 * used for the given relid set, if one exists.
 *
 * Returns the pgpa_scan object, or NULL if none was found.
 */
static pgpa_scan *
pgpa_walker_find_scan(pgpa_plan_walker_context *walker,
					  pgpa_scan_strategy strategy,
					  Bitmapset *relids)
{
	List	   *scans = walker->scans[strategy];

	foreach_ptr(pgpa_scan, scan, scans)
	{
		if (bms_equal(scan->relids, relids))
			return scan;
	}

	return NULL;
}

/*
 * Does this walker say that the given query feature applies to the given
 * relid set?
 */
static bool
pgpa_walker_contains_feature(pgpa_plan_walker_context *walker,
							 pgpa_qf_type type,
							 Bitmapset *relids)
{
	List	   *query_features = walker->query_features[type];

	foreach_ptr(pgpa_query_feature, qf, query_features)
	{
		if (bms_equal(qf->relids, relids))
			return true;
	}

	return false;
}

/*
 * Does the walker say that the given join strategy should be used for the
 * given relid set?
 */
static bool
pgpa_walker_contains_join(pgpa_plan_walker_context *walker,
						  pgpa_join_strategy strategy,
						  Bitmapset *relids)
{
	List	   *join_strategies = walker->join_strategies[strategy];

	foreach_ptr(Bitmapset, jsrelids, join_strategies)
	{
		if (bms_equal(jsrelids, relids))
			return true;
	}

	return false;
}

/*
 * Does the walker say that the given relids should be marked as NO_GATHER?
 */
static bool
pgpa_walker_contains_no_gather(pgpa_plan_walker_context *walker,
							   Bitmapset *relids)
{
	return bms_is_subset(relids, walker->no_gather_scans);
}
