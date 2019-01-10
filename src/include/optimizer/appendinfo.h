/*-------------------------------------------------------------------------
 *
 * appendinfo.h
 *	  Routines for mapping expressions between append rel parent(s) and
 *	  children
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/appendinfo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef APPENDINFO_H
#define APPENDINFO_H

#include "nodes/plannodes.h"
#include "nodes/relation.h"
#include "utils/relcache.h"

extern AppendRelInfo *make_append_rel_info(Relation parentrel,
					 Relation childrel,
					 Index parentRTindex, Index childRTindex);
extern Bitmapset *translate_col_privs(const Bitmapset *parent_privs,
					List *translated_vars);
extern Node *adjust_appendrel_attrs(PlannerInfo *root, Node *node,
					   int nappinfos, AppendRelInfo **appinfos);

extern Node *adjust_appendrel_attrs_multilevel(PlannerInfo *root, Node *node,
								  Relids child_relids,
								  Relids top_parent_relids);

extern AppendRelInfo **find_appinfos_by_relids(PlannerInfo *root,
						Relids relids, int *nappinfos);

extern SpecialJoinInfo *build_child_join_sjinfo(PlannerInfo *root,
						SpecialJoinInfo *parent_sjinfo,
						Relids left_relids, Relids right_relids);
extern Relids adjust_child_relids_multilevel(PlannerInfo *root, Relids relids,
							   Relids child_relids, Relids top_parent_relids);

#endif							/* APPENDINFO_H */
