/*-------------------------------------------------------------------------
 *
 * cost.h
 *	  prototypes for costsize.c and clausesel.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: cost.h,v 1.25 2000/01/09 00:26:46 tgl Exp $
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

/*
 * prototypes for costsize.c
 *	  routines to compute costs and sizes
 */
extern bool _enable_seqscan_;
extern bool _enable_indexscan_;
extern bool _enable_sort_;
extern bool _enable_nestloop_;
extern bool _enable_mergejoin_;
extern bool _enable_hashjoin_;
extern bool _enable_tidscan_;

extern Cost cost_seqscan(RelOptInfo *baserel);
extern Cost cost_index(RelOptInfo *baserel, IndexOptInfo *index,
					   long expected_indexpages, Selectivity selec,
					   bool is_injoin);
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
