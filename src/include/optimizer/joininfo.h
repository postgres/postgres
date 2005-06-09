/*-------------------------------------------------------------------------
 *
 * joininfo.h
 *	  prototypes for joininfo.c.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/joininfo.h,v 1.30 2005/06/09 04:19:00 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef JOININFO_H
#define JOININFO_H

#include "nodes/relation.h"


extern bool have_relevant_joinclause(RelOptInfo *rel1, RelOptInfo *rel2);

extern void add_join_clause_to_rels(PlannerInfo *root,
						RestrictInfo *restrictinfo,
						Relids join_relids);
extern void remove_join_clause_from_rels(PlannerInfo *root,
							 RestrictInfo *restrictinfo,
							 Relids join_relids);

#endif   /* JOININFO_H */
