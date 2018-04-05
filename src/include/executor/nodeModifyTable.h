/*-------------------------------------------------------------------------
 *
 * nodeModifyTable.h
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeModifyTable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMODIFYTABLE_H
#define NODEMODIFYTABLE_H

#include "nodes/execnodes.h"

extern ModifyTableState *ExecInitModifyTable(ModifyTable *node, EState *estate, int eflags);
extern void ExecEndModifyTable(ModifyTableState *node);
extern void ExecReScanModifyTable(ModifyTableState *node);
extern TupleTableSlot *ExecPrepareTupleRouting(ModifyTableState *mtstate,
						EState *estate,
						struct PartitionTupleRouting *proute,
						ResultRelInfo *targetRelInfo,
						TupleTableSlot *slot);
extern TupleTableSlot *ExecDelete(ModifyTableState *mtstate,
		   ItemPointer tupleid, HeapTuple oldtuple, TupleTableSlot *planSlot,
		   EPQState *epqstate, EState *estate, bool *tupleDeleted,
		   bool processReturning, HeapUpdateFailureData *hufdp,
		   MergeActionState *actionState, bool canSetTag);
extern TupleTableSlot *ExecUpdate(ModifyTableState *mtstate,
		   ItemPointer tupleid, HeapTuple oldtuple, TupleTableSlot *slot,
		   TupleTableSlot *planSlot, EPQState *epqstate, EState *estate,
		   bool *tuple_updated, HeapUpdateFailureData *hufdp,
		   MergeActionState *actionState, bool canSetTag);
extern TupleTableSlot *ExecInsert(ModifyTableState *mtstate,
		   TupleTableSlot *slot,
		   TupleTableSlot *planSlot,
		   EState *estate,
		   MergeActionState *actionState,
		   bool canSetTag);
extern void ExecCheckPlanOutput(Relation resultRel, List *targetList);

#endif							/* NODEMODIFYTABLE_H */
