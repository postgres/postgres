/*-------------------------------------------------------------------------
 *
 * cost.h
 *	  prototypes for costsize.c and clausesel.c.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: cost.h,v 1.57 2003/08/08 21:42:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COST_H
#define COST_H

#include "nodes/plannodes.h"
#include "nodes/relation.h"


/* defaults for costsize.c's Cost parameters */
/* NB: cost-estimation code should use the variables, not these constants! */
/* If you change these, update backend/utils/misc/postgresql.sample.conf */
#define DEFAULT_EFFECTIVE_CACHE_SIZE  1000.0	/* measured in pages */
#define DEFAULT_RANDOM_PAGE_COST  4.0
#define DEFAULT_CPU_TUPLE_COST	0.01
#define DEFAULT_CPU_INDEX_TUPLE_COST 0.001
#define DEFAULT_CPU_OPERATOR_COST  0.0025


/*
 * prototypes for costsize.c
 *	  routines to compute costs and sizes
 */

/* parameter variables and flags */
extern double effective_cache_size;
extern double random_page_cost;
extern double cpu_tuple_cost;
extern DLLIMPORT double cpu_index_tuple_cost;
extern double cpu_operator_cost;
extern Cost disable_cost;
extern bool enable_seqscan;
extern bool enable_indexscan;
extern bool enable_tidscan;
extern bool enable_sort;
extern bool enable_hashagg;
extern bool enable_nestloop;
extern bool enable_mergejoin;
extern bool enable_hashjoin;

extern void cost_seqscan(Path *path, Query *root,
			 RelOptInfo *baserel);
extern void cost_index(Path *path, Query *root,
		   RelOptInfo *baserel, IndexOptInfo *index,
		   List *indexQuals, bool is_injoin);
extern void cost_tidscan(Path *path, Query *root,
			 RelOptInfo *baserel, List *tideval);
extern void cost_subqueryscan(Path *path, RelOptInfo *baserel);
extern void cost_functionscan(Path *path, Query *root,
				  RelOptInfo *baserel);
extern void cost_sort(Path *path, Query *root,
		  List *pathkeys, Cost input_cost, double tuples, int width);
extern void cost_material(Path *path,
			  Cost input_cost, double tuples, int width);
extern void cost_agg(Path *path, Query *root,
		 AggStrategy aggstrategy, int numAggs,
		 int numGroupCols, double numGroups,
		 Cost input_startup_cost, Cost input_total_cost,
		 double input_tuples);
extern void cost_group(Path *path, Query *root,
		   int numGroupCols, double numGroups,
		   Cost input_startup_cost, Cost input_total_cost,
		   double input_tuples);
extern void cost_nestloop(NestPath *path, Query *root);
extern void cost_mergejoin(MergePath *path, Query *root);
extern void cost_hashjoin(HashPath *path, Query *root);
extern void cost_qual_eval(QualCost *cost, List *quals);
extern void set_baserel_size_estimates(Query *root, RelOptInfo *rel);
extern void set_joinrel_size_estimates(Query *root, RelOptInfo *rel,
						   RelOptInfo *outer_rel,
						   RelOptInfo *inner_rel,
						   JoinType jointype,
						   List *restrictlist);
extern void set_function_size_estimates(Query *root, RelOptInfo *rel);

/*
 * prototypes for clausesel.c
 *	  routines to compute clause selectivities
 */
extern Selectivity restrictlist_selectivity(Query *root,
						 List *restrictinfo_list,
						 int varRelid,
						 JoinType jointype);
extern Selectivity clauselist_selectivity(Query *root,
					   List *clauses,
					   int varRelid,
					   JoinType jointype);
extern Selectivity clause_selectivity(Query *root,
				   Node *clause,
				   int varRelid,
				   JoinType jointype);

#endif   /* COST_H */
