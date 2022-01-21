/*-------------------------------------------------------------------------
 *
 * joininfo.h
 *	  prototypes for joininfo.c.
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/joininfo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef JOININFO_H
#define JOININFO_H

#include "nodes/pathnodes.h"


extern bool have_relevant_joinclause(PlannerInfo *root,
									 RelOptInfo *rel1, RelOptInfo *rel2);

extern void add_join_clause_to_rels(PlannerInfo *root,
									RestrictInfo *restrictinfo,
									Relids join_relids);
extern void remove_join_clause_from_rels(PlannerInfo *root,
										 RestrictInfo *restrictinfo,
										 Relids join_relids);

#endif							/* JOININFO_H */
