/*-------------------------------------------------------------------------
 *
 * pgpa_scan.h
 *	  analysis of scans in Plan trees
 *
 * For purposes of this module, a "scan" includes (1) single plan nodes that
 * scan multiple RTIs, such as a degenerate Result node that replaces what
 * would otherwise have been a join, and (2) Append and MergeAppend nodes
 * implementing a partitionwise scan or a partitionwise join. Said
 * differently, scans are the leaves of the join tree for a single join
 * problem.
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_scan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGPA_SCAN_H
#define PGPA_SCAN_H

#include "nodes/plannodes.h"

typedef struct pgpa_plan_walker_context pgpa_plan_walker_context;

/*
 * Scan strategies.
 *
 * PGPA_SCAN_ORDINARY is any scan strategy that isn't interesting to us
 * because there is no meaningful planner decision involved. For example,
 * the only way to scan a subquery is a SubqueryScan, and the only way to
 * scan a VALUES construct is a ValuesScan. We need not care exactly which
 * type of planner node was used in such cases, because the same thing will
 * happen when replanning.
 *
 * PGPA_SCAN_ORDINARY also includes Result nodes that correspond to scans
 * or even joins that are proved empty. We don't know whether or not the scan
 * or join will still be provably empty at replanning time, but if it is,
 * then no scan-type advice is needed, and if it's not, we can't recommend
 * a scan type based on the current plan.
 *
 * PGPA_SCAN_PARTITIONWISE also lumps together scans and joins: this can
 * be either a partitionwise scan of a partitioned table or a partitionwise
 * join between several partitioned tables. Note that all decisions about
 * whether or not to use partitionwise join are meaningful: no matter what
 * we decided this time, we could do more or fewer things partitionwise the
 * next time.
 *
 * PGPA_SCAN_FOREIGN is only used when there's more than one relation involved;
 * a single-table foreign scan is classified as ordinary, since there is no
 * decision to make in that case.
 *
 * Other scan strategies map one-to-one to plan nodes.
 */
typedef enum
{
	PGPA_SCAN_ORDINARY = 0,
	PGPA_SCAN_SEQ,
	PGPA_SCAN_BITMAP_HEAP,
	PGPA_SCAN_FOREIGN,
	PGPA_SCAN_INDEX,
	PGPA_SCAN_INDEX_ONLY,
	PGPA_SCAN_PARTITIONWISE,
	PGPA_SCAN_TID
	/* update NUM_PGPA_SCAN_STRATEGY if you add anything here */
} pgpa_scan_strategy;

#define NUM_PGPA_SCAN_STRATEGY	((int) PGPA_SCAN_TID + 1)

/*
 * All of the details we need regarding a scan.
 */
typedef struct pgpa_scan
{
	Plan	   *plan;
	pgpa_scan_strategy strategy;
	Bitmapset  *relids;
} pgpa_scan;

extern pgpa_scan *pgpa_build_scan(pgpa_plan_walker_context *walker, Plan *plan,
								  ElidedNode *elided_node,
								  bool beneath_any_gather,
								  bool within_join_problem);

#endif
