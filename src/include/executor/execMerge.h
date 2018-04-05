/*-------------------------------------------------------------------------
 *
 * execMerge.h
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/execMerge.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECMERGE_H
#define EXECMERGE_H

#include "nodes/execnodes.h"

/* flags for mt_merge_subcommands */
#define MERGE_INSERT	0x01
#define MERGE_UPDATE	0x02
#define MERGE_DELETE	0x04

extern void ExecMerge(ModifyTableState *mtstate, EState *estate,
					  TupleTableSlot *slot, JunkFilter *junkfilter,
					  ResultRelInfo *resultRelInfo);

extern void ExecInitMerge(ModifyTableState *mtstate,
						  EState *estate,
						  ResultRelInfo *resultRelInfo);

#endif							/* NODEMERGE_H */
