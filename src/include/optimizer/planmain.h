/*-------------------------------------------------------------------------
 *
 * planmain.h
 *	  prototypes for various files in optimizer/plan
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: planmain.h,v 1.34 1999/10/07 04:23:19 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANMAIN_H
#define PLANMAIN_H

#include "nodes/plannodes.h"
#include "nodes/relation.h"

/*
 * prototypes for plan/planmain.c
 */
extern Plan *query_planner(Query *root, List *tlist, List *qual);

/*
 * prototypes for plan/createplan.c
 */
extern Plan *create_plan(Path *best_path);
extern SeqScan *make_seqscan(List *qptlist, List *qpqual, Index scanrelid);
extern Sort *make_sort(List *tlist, Oid nonameid, Plan *lefttree,
		  int keycount);
extern Agg *make_agg(List *tlist, Plan *lefttree);
extern Group *make_group(List *tlist, bool tuplePerGroup, int ngrp,
		   AttrNumber *grpColIdx, Plan *lefttree);
extern Noname *make_noname(List *tlist, List *pathkeys, Plan *subplan);
extern Unique *make_unique(List *tlist, Plan *lefttree, char *uniqueAttr);
extern Result *make_result(List *tlist, Node *resconstantqual, Plan *subplan);

/*
 * prototypes for plan/initsplan.c
 */
extern void make_var_only_tlist(Query *root, List *tlist);
extern void add_restrict_and_join_to_rels(Query *root, List *clauses);
extern void add_missing_rels_to_query(Query *root);
extern void set_joininfo_mergeable_hashable(List *rel_list);

/*
 * prototypes for plan/setrefs.c
 */
extern void set_plan_references(Plan *plan);
extern List *join_references(List *clauses, List *outer_tlist,
							 List *inner_tlist, Index acceptable_rel);
extern void fix_opids(Node *node);

/*
 * prep/prepkeyset.c
 */
extern void transformKeySetQuery(Query *origNode);

#endif	 /* PLANMAIN_H */
