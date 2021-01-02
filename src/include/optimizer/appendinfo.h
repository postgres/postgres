/*-------------------------------------------------------------------------
 *
 * appendinfo.h
 *	  Routines for mapping expressions between append rel parent(s) and
 *	  children
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/appendinfo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef APPENDINFO_H
#define APPENDINFO_H

#include "nodes/pathnodes.h"
#include "utils/relcache.h"

extern AppendRelInfo *make_append_rel_info(Relation parentrel,
										   Relation childrel,
										   Index parentRTindex, Index childRTindex);
extern Node *adjust_appendrel_attrs(PlannerInfo *root, Node *node,
									int nappinfos, AppendRelInfo **appinfos);
extern Node *adjust_appendrel_attrs_multilevel(PlannerInfo *root, Node *node,
											   Relids child_relids,
											   Relids top_parent_relids);
extern Relids adjust_child_relids(Relids relids, int nappinfos,
								  AppendRelInfo **appinfos);
extern Relids adjust_child_relids_multilevel(PlannerInfo *root, Relids relids,
											 Relids child_relids, Relids top_parent_relids);
extern AppendRelInfo **find_appinfos_by_relids(PlannerInfo *root,
											   Relids relids, int *nappinfos);

#endif							/* APPENDINFO_H */
