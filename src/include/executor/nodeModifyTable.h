/*-------------------------------------------------------------------------
 *
 * nodeModifyTable.h
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeModifyTable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMODIFYTABLE_H
#define NODEMODIFYTABLE_H

#include "nodes/execnodes.h"

extern void ExecComputeStoredGenerated(ResultRelInfo *resultRelInfo,
									   EState *estate, TupleTableSlot *slot,
									   CmdType cmdtype);

extern ModifyTableState *ExecInitModifyTable(ModifyTable *node, EState *estate, int eflags);
extern void ExecEndModifyTable(ModifyTableState *node);
extern void ExecReScanModifyTable(ModifyTableState *node);

extern void ExecInitMergeTupleSlots(ModifyTableState *mtstate,
									ResultRelInfo *resultRelInfo);

#endif							/* NODEMODIFYTABLE_H */
