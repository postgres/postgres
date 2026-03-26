/*-------------------------------------------------------------------------
 *
 * pgpa_walker.h
 *	  Main entrypoints for analyzing a plan to generate an advice string
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_walker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGPA_WALKER_H
#define PGPA_WALKER_H

#include "pgpa_ast.h"
#include "pgpa_join.h"
#include "pgpa_scan.h"

/*
 * We use the term "query feature" to refer to plan nodes that are interesting
 * in the following way: to generate advice, we'll need to know the set of
 * same-subquery, non-join RTIs occurring at or below that plan node, without
 * admixture of parent and child RTIs.
 *
 * For example, Gather nodes, designated by PGPAQF_GATHER, and Gather Merge
 * nodes, designated by PGPAQF_GATHER_MERGE, are query features, because we'll
 * want to admit some kind of advice that describes the portion of the plan
 * tree that appears beneath those nodes.
 *
 * Each semijoin can be implemented either by directly performing a semijoin,
 * or by making one side unique and then performing a normal join. Either way,
 * we use a query feature to notice what decision was made, so that we can
 * describe it by enumerating the RTIs on that side of the join.
 *
 * To elaborate on the "no admixture of parent and child RTIs" rule, in all of
 * these cases, if the entirety of an inheritance hierarchy appears beneath
 * the query feature, we only want to name the parent table. But it's also
 * possible to have cases where we must name child tables. This is particularly
 * likely to happen when partitionwise join is in use, but could happen for
 * Gather or Gather Merge even without that, if one of those appears below
 * an Append or MergeAppend node for a single table.
 */
typedef enum pgpa_qf_type
{
	PGPAQF_GATHER,
	PGPAQF_GATHER_MERGE,
	PGPAQF_SEMIJOIN_NON_UNIQUE,
	PGPAQF_SEMIJOIN_UNIQUE
	/* update NUM_PGPA_QF_TYPES if you add anything here */
} pgpa_qf_type;

#define NUM_PGPA_QF_TYPES ((int) PGPAQF_SEMIJOIN_UNIQUE + 1)

/*
 * For each query feature, we keep track of the feature type and the set of
 * relids that we found underneath the relevant plan node. See the comments
 * on pgpa_qf_type, above, for additional details.
 */
typedef struct pgpa_query_feature
{
	pgpa_qf_type type;
	Plan	   *plan;
	Bitmapset  *relids;
} pgpa_query_feature;

/*
 * Context object for plan tree walk.
 *
 * pstmt is the PlannedStmt we're studying.
 *
 * scans is an array of lists of pgpa_scan objects. The array is indexed by
 * the scan's pgpa_scan_strategy.
 *
 * no_gather_scans is the set of scan RTIs that do not appear beneath any
 * Gather or Gather Merge node.
 *
 * toplevel_unrolled_joins is a list of all pgpa_unrolled_join objects that
 * are not a child of some other pgpa_unrolled_join.
 *
 * join_strategy is an array of lists of Bitmapset objects. Each Bitmapset
 * is the set of relids that appears on the inner side of some join (excluding
 * RTIs from partition children and subqueries). The array is indexed by
 * pgpa_join_strategy.
 *
 * query_features is an array lists of pgpa_query_feature objects, indexed
 * by pgpa_qf_type.
 *
 * future_query_features is only used during the plan tree walk and should
 * be empty when the tree walk concludes. It is a list of pgpa_query_feature
 * objects for Plan nodes that the plan tree walk has not yet encountered;
 * when encountered, they will be moved to the list of active query features
 * that is propagated via the call stack.
 */
typedef struct pgpa_plan_walker_context
{
	PlannedStmt *pstmt;
	List	   *scans[NUM_PGPA_SCAN_STRATEGY];
	Bitmapset  *no_gather_scans;
	List	   *toplevel_unrolled_joins;
	List	   *join_strategies[NUM_PGPA_JOIN_STRATEGY];
	List	   *query_features[NUM_PGPA_QF_TYPES];
	List	   *future_query_features;
	List	   *do_not_scan_identifiers;
} pgpa_plan_walker_context;

extern void pgpa_plan_walker(pgpa_plan_walker_context *walker,
							 PlannedStmt *pstmt,
							 List *proots);

extern void pgpa_add_future_feature(pgpa_plan_walker_context *walker,
									pgpa_qf_type type,
									Plan *plan);

extern ElidedNode *pgpa_last_elided_node(PlannedStmt *pstmt, Plan *plan);
extern Bitmapset *pgpa_relids(Plan *plan);
extern Index pgpa_scanrelid(Plan *plan);
extern Bitmapset *pgpa_filter_out_join_relids(Bitmapset *relids, List *rtable);

extern bool pgpa_walker_would_advise(pgpa_plan_walker_context *walker,
									 pgpa_identifier *rt_identifiers,
									 pgpa_advice_tag_type tag,
									 pgpa_advice_target *target);

#endif
