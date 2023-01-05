/*-------------------------------------------------------------------------
 *
 * inherit.h
 *	  prototypes for inherit.c.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/inherit.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INHERIT_H
#define INHERIT_H

#include "nodes/pathnodes.h"


extern void expand_inherited_rtentry(PlannerInfo *root, RelOptInfo *rel,
									 RangeTblEntry *rte, Index rti);

extern Bitmapset *get_rel_all_updated_cols(PlannerInfo *root, RelOptInfo *rel);

extern bool apply_child_basequals(PlannerInfo *root, RelOptInfo *parentrel,
								  RelOptInfo *childrel, RangeTblEntry *childRTE,
								  AppendRelInfo *appinfo);

#endif							/* INHERIT_H */
