/*-------------------------------------------------------------------------
 *
 * pgpa_scan.c
 *	  analysis of scans in Plan trees
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_scan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pgpa_scan.h"
#include "pgpa_walker.h"

#include "nodes/parsenodes.h"
#include "parser/parsetree.h"

static pgpa_scan *pgpa_make_scan(pgpa_plan_walker_context *walker, Plan *plan,
								 pgpa_scan_strategy strategy,
								 Bitmapset *relids);


static RTEKind unique_nonjoin_rtekind(Bitmapset *relids, List *rtable);

/*
 * Build a pgpa_scan object for a Plan node and update the plan walker
 * context as appropriate.  If this is an Append or MergeAppend scan, also
 * build pgpa_scan for any scans that were consolidated into this one by
 * Append/MergeAppend pull-up.
 *
 * If there is at least one ElidedNode for this plan node, pass the uppermost
 * one as elided_node, else pass NULL.
 *
 * Set the 'beneath_any_gather' node if we are underneath a Gather or
 * Gather Merge node (except for a single-copy Gather node, for which
 * GATHER or GATHER_MERGE advice should not be emitted).
 *
 * Set the 'within_join_problem' flag if we're inside of a join problem and
 * not otherwise.
 */
pgpa_scan *
pgpa_build_scan(pgpa_plan_walker_context *walker, Plan *plan,
				ElidedNode *elided_node,
				bool beneath_any_gather, bool within_join_problem)
{
	pgpa_scan_strategy strategy = PGPA_SCAN_ORDINARY;
	Bitmapset  *relids = NULL;
	int			rti = -1;
	List	   *child_append_relid_sets = NIL;
	NodeTag		nodetype = nodeTag(plan);

	if (elided_node != NULL)
	{
		nodetype = elided_node->elided_type;
		relids = elided_node->relids;

		/*
		 * If setrefs processing elided an Append or MergeAppend node that had
		 * only one surviving child, it could be either a partitionwise
		 * operation or a setop over subqueries, depending on the rtekind.
		 *
		 * A setop over subqueries, or a trivial SubqueryScan that was elided,
		 * is an "ordinary" scan i.e. one for which we do not need to generate
		 * advice because the planner has not made any meaningful choice.
		 *
		 * Note that the PGPA_SCAN_PARTITIONWISE case also includes
		 * partitionwise joins; this module considers those to be a form of
		 * scan, since they lack internal structure that we can decompose.
		 */
		if ((nodetype == T_Append || nodetype == T_MergeAppend) &&
			unique_nonjoin_rtekind(relids,
								   walker->pstmt->rtable) == RTE_RELATION)
			strategy = PGPA_SCAN_PARTITIONWISE;
		else
			strategy = PGPA_SCAN_ORDINARY;

		/* Join RTIs can be present, but advice never refers to them. */
		relids = pgpa_filter_out_join_relids(relids, walker->pstmt->rtable);
	}
	else if ((rti = pgpa_scanrelid(plan)) != 0)
	{
		relids = bms_make_singleton(rti);

		switch (nodeTag(plan))
		{
			case T_SeqScan:
				strategy = PGPA_SCAN_SEQ;
				break;
			case T_BitmapHeapScan:
				strategy = PGPA_SCAN_BITMAP_HEAP;
				break;
			case T_IndexScan:
				strategy = PGPA_SCAN_INDEX;
				break;
			case T_IndexOnlyScan:
				strategy = PGPA_SCAN_INDEX_ONLY;
				break;
			case T_TidScan:
			case T_TidRangeScan:
				strategy = PGPA_SCAN_TID;
				break;
			default:

				/*
				 * This case includes a ForeignScan targeting a single
				 * relation; no other strategy is possible in that case, but
				 * see below, where things are different in multi-relation
				 * cases.
				 */
				strategy = PGPA_SCAN_ORDINARY;
				break;
		}
	}
	else if ((relids = pgpa_relids(plan)) != NULL)
	{
		switch (nodeTag(plan))
		{
			case T_ForeignScan:

				/*
				 * If multiple relations are being targeted by a single
				 * foreign scan, then the foreign join has been pushed to the
				 * remote side, and we want that to be reflected in the
				 * generated advice.
				 */
				strategy = PGPA_SCAN_FOREIGN;
				break;
			case T_Append:

				/*
				 * Append nodes can represent partitionwise scans of a
				 * relation, but when they implement a set operation, they are
				 * just ordinary scans.
				 */
				if (unique_nonjoin_rtekind(relids, walker->pstmt->rtable)
					== RTE_RELATION)
					strategy = PGPA_SCAN_PARTITIONWISE;
				else
					strategy = PGPA_SCAN_ORDINARY;

				/* Be sure to account for pulled-up scans. */
				child_append_relid_sets =
					((Append *) plan)->child_append_relid_sets;
				break;
			case T_MergeAppend:
				/* Same logic here as for Append, above. */
				if (unique_nonjoin_rtekind(relids, walker->pstmt->rtable)
					== RTE_RELATION)
					strategy = PGPA_SCAN_PARTITIONWISE;
				else
					strategy = PGPA_SCAN_ORDINARY;

				/* Be sure to account for pulled-up scans. */
				child_append_relid_sets =
					((MergeAppend *) plan)->child_append_relid_sets;
				break;
			default:
				strategy = PGPA_SCAN_ORDINARY;
				break;
		}


		/* Join RTIs can be present, but advice never refers to them. */
		relids = pgpa_filter_out_join_relids(relids, walker->pstmt->rtable);
	}

	/*
	 * If this is an Append or MergeAppend node into which subordinate Append
	 * or MergeAppend paths were merged, each of those merged paths is
	 * effectively another scan for which we need to account.
	 */
	foreach_node(Bitmapset, child_relids, child_append_relid_sets)
	{
		Bitmapset  *child_nonjoin_relids;

		child_nonjoin_relids =
			pgpa_filter_out_join_relids(child_relids,
										walker->pstmt->rtable);
		(void) pgpa_make_scan(walker, plan, strategy,
							  child_nonjoin_relids);
	}

	/*
	 * If this plan node has no associated RTIs, it's not a scan. When the
	 * 'within_join_problem' flag is set, that's unexpected, so throw an
	 * error, else return quietly.
	 */
	if (relids == NULL)
	{
		if (within_join_problem)
			elog(ERROR, "plan node has no RTIs: %d", (int) nodeTag(plan));
		return NULL;
	}

	/*
	 * Add the appropriate set of RTIs to walker->no_gather_scans.
	 *
	 * Add nothing if we're beneath a Gather or Gather Merge node, since
	 * NO_GATHER advice is clearly inappropriate in that situation.
	 *
	 * Add nothing if this is an Append or MergeAppend node, whether or not
	 * elided. We'll emit NO_GATHER() for the underlying scan, which is good
	 * enough.
	 */
	if (!beneath_any_gather && nodetype != T_Append &&
		nodetype != T_MergeAppend)
		walker->no_gather_scans =
			bms_add_members(walker->no_gather_scans, relids);

	/* Caller tells us whether NO_GATHER() advice for this scan is needed. */
	return pgpa_make_scan(walker, plan, strategy, relids);
}

/*
 * Create a single pgpa_scan object and update the pgpa_plan_walker_context.
 */
static pgpa_scan *
pgpa_make_scan(pgpa_plan_walker_context *walker, Plan *plan,
			   pgpa_scan_strategy strategy, Bitmapset *relids)
{
	pgpa_scan  *scan;

	/* Create the scan object. */
	scan = palloc(sizeof(pgpa_scan));
	scan->plan = plan;
	scan->strategy = strategy;
	scan->relids = relids;

	/* Add it to the appropriate list. */
	walker->scans[scan->strategy] = lappend(walker->scans[scan->strategy],
											scan);

	return scan;
}

/*
 * Determine the unique rtekind of a set of relids.
 */
static RTEKind
unique_nonjoin_rtekind(Bitmapset *relids, List *rtable)
{
	int			rti = -1;
	bool		first = true;
	RTEKind		rtekind;

	Assert(relids != NULL);

	while ((rti = bms_next_member(relids, rti)) >= 0)
	{
		RangeTblEntry *rte = rt_fetch(rti, rtable);

		if (rte->rtekind == RTE_JOIN)
			continue;

		if (first)
		{
			rtekind = rte->rtekind;
			first = false;
		}
		else if (rtekind != rte->rtekind)
			elog(ERROR, "rtekind mismatch: %d vs. %d",
				 rtekind, rte->rtekind);
	}

	if (first)
		elog(ERROR, "no non-RTE_JOIN RTEs found");

	return rtekind;
}
