/*-------------------------------------------------------------------------
 *
 * nodeMerge.h
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeMerge.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMERGE_H
#define NODEMERGE_H

#include "nodes/execnodes.h"

extern void
ExecMerge(ModifyTableState *mtstate, EState *estate, TupleTableSlot *slot,
		  JunkFilter *junkfilter, ResultRelInfo *resultRelInfo);

#endif							/* NODEMERGE_H */
