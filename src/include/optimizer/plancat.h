/*-------------------------------------------------------------------------
 *
 * plancat.h
 *	  prototypes for plancat.c.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/plancat.h,v 1.42 2006/10/04 00:30:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANCAT_H
#define PLANCAT_H

#include "nodes/relation.h"


extern void get_relation_info(PlannerInfo *root, Oid relationObjectId,
				  bool inhparent, RelOptInfo *rel);

extern bool relation_excluded_by_constraints(RelOptInfo *rel,
								 RangeTblEntry *rte);

extern List *build_physical_tlist(PlannerInfo *root, RelOptInfo *rel);

extern List *find_inheritance_children(Oid inhparent);

extern bool has_subclass(Oid relationId);

extern bool has_unique_index(RelOptInfo *rel, AttrNumber attno);

extern Selectivity restriction_selectivity(PlannerInfo *root,
						Oid operator,
						List *args,
						int varRelid);

extern Selectivity join_selectivity(PlannerInfo *root,
				 Oid operator,
				 List *args,
				 JoinType jointype);

#endif   /* PLANCAT_H */
