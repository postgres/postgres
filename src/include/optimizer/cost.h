/*-------------------------------------------------------------------------
 *
 * cost.h
 *	  prototypes for costsize.c and clausesel.c.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: cost.h,v 1.29 2000/02/07 04:41:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COST_H
#define COST_H

#include "nodes/relation.h"

/* defaults for costsize.c's Cost parameters */
/* NB: cost-estimation code should use the variables, not the constants! */
#define CPU_PAGE_WEIGHT  0.033
#define CPU_INDEX_PAGE_WEIGHT  0.017

/* defaults for function attributes used for expensive function calculations */
#define BYTE_PCT 100
#define PERBYTE_CPU 0
#define PERCALL_CPU 0
#define OUTIN_RATIO 100


/*
 * prototypes for costsize.c
 *	  routines to compute costs and sizes
 */

extern Cost cpu_page_weight;
extern Cost cpu_index_page_weight;
extern Cost disable_cost;
extern bool enable_seqscan;
extern bool enable_indexscan;
extern bool enable_tidscan;
extern bool enable_sort;
extern bool enable_nestloop;
extern bool enable_mergejoin;
extern bool enable_hashjoin;

extern Cost cost_seqscan(RelOptInfo *baserel);
extern Cost cost_index(Query *root, RelOptInfo *baserel, IndexOptInfo *index,
					   List *indexQuals, bool is_injoin);
extern Cost cost_tidscan(RelOptInfo *baserel, List *tideval);
extern Cost cost_sort(List *pathkeys, double tuples, int width);
extern Cost cost_nestloop(Path *outer_path, Path *inner_path,
						  bool is_indexjoin);
extern Cost cost_mergejoin(Path *outer_path, Path *inner_path,
						   List *outersortkeys, List *innersortkeys);
extern Cost cost_hashjoin(Path *outer_path, Path *inner_path,
						  Selectivity innerdisbursion);
extern void set_baserel_size_estimates(Query *root, RelOptInfo *rel);
extern void set_joinrel_size_estimates(Query *root, RelOptInfo *rel,
									   RelOptInfo *outer_rel,
									   RelOptInfo *inner_rel,
									   List *restrictlist);

/*
 * prototypes for clausesel.c
 *	  routines to compute clause selectivities
 */
extern Selectivity restrictlist_selectivity(Query *root,
											List *restrictinfo_list,
											int varRelid);
extern Selectivity clauselist_selectivity(Query *root,
										  List *clauses,
										  int varRelid);
extern Selectivity clause_selectivity(Query *root,
									  Node *clause,
									  int varRelid);

#endif	 /* COST_H */
