/*-------------------------------------------------------------------------
 *
 * cost.h
 *	  prototypes for costsize.c and clausesel.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: cost.h,v 1.26 2000/01/22 23:50:26 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COST_H
#define COST_H

#include "nodes/relation.h"

/* defaults for function attributes used for expensive function calculations */
#define BYTE_PCT 100
#define PERBYTE_CPU 0
#define PERCALL_CPU 0
#define OUTIN_RATIO 100
/* defaults for costsize.c's Cost parameters */
/* NB: cost-estimation code should use the variables, not the constants! */
#define CPU_PAGE_WEIGHT  0.033
#define CPU_INDEX_PAGE_WEIGHT  0.017


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
extern void set_rel_rows_width(Query *root, RelOptInfo *rel);
extern void set_joinrel_rows_width(Query *root, RelOptInfo *rel,
								   JoinPath *joinpath);

/*
 * prototypes for clausesel.c
 *	  routines to compute clause selectivities
 */
extern Selectivity restrictlist_selec(Query *root, List *restrictinfo_list);
extern Selectivity clauselist_selec(Query *root, List *clauses);
extern Selectivity compute_clause_selec(Query *root, Node *clause);

#endif	 /* COST_H */
