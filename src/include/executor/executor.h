/*-------------------------------------------------------------------------
 *
 * executor.h--
 *	  support for the POSTGRES executor module
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: executor.h,v 1.22 1998/04/24 14:43:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <catalog/pg_index.h>
#include <storage/bufmgr.h>
#include <access/itup.h>
#include <stdio.h>
#include <executor/execdesc.h>

/* ----------------
 *		TupIsNull
 *
 *		This is used mainly to detect when there are no more
 *		tuples to process.
 * ----------------
 */
/* return: true if tuple in slot is NULL, slot is slot to test */
#define TupIsNull(slot) \
( \
	((slot) == NULL) ? \
		true \
	: \
	( \
		((slot)->val == NULL) ? \
			true \
		: \
			false \
	) \
)

/* --------------------------------
 *		ExecIncrSlotBufferRefcnt
 *
 *		When we pass around buffers in the tuple table, we have to
 *		be careful to increment reference counts appropriately.
 *		This is used mainly in the mergejoin code.
 * --------------------------------
 */
#define ExecIncrSlotBufferRefcnt(slot) \
( \
	BufferIsValid((slot)->ttc_buffer) ? \
		IncrBufferRefCount((slot)->ttc_buffer) \
	: (void)NULL \
)


/*
 * prototypes from functions in execAmi.c
 */
extern void
ExecOpenScanR(Oid relOid, int nkeys, ScanKey skeys, bool isindex,
			  ScanDirection dir, Relation *returnRelation,
			  Pointer *returnScanDesc);
extern void ExecCloseR(Plan *node);
extern void ExecReScan(Plan *node, ExprContext *exprCtxt, Plan *parent);
extern HeapScanDesc
ExecReScanR(Relation relDesc, HeapScanDesc scanDesc,
			ScanDirection direction, int nkeys, ScanKey skeys);
extern void ExecMarkPos(Plan *node);
extern void ExecRestrPos(Plan *node);
extern Relation ExecCreatR(TupleDesc tupType, Oid relationOid);

/*
 * prototypes from functions in execJunk.c
 */
extern JunkFilter *ExecInitJunkFilter(List *targetList);
extern bool
ExecGetJunkAttribute(JunkFilter *junkfilter, TupleTableSlot *slot,
					 char *attrName, Datum *value, bool *isNull);
extern HeapTuple ExecRemoveJunk(JunkFilter *junkfilter, TupleTableSlot *slot);


/*
 * prototypes from functions in execMain.c
 */
extern TupleDesc ExecutorStart(QueryDesc *queryDesc, EState *estate);
extern TupleTableSlot *ExecutorRun(QueryDesc *queryDesc, EState *estate, int feature, int count);
extern void ExecutorEnd(QueryDesc *queryDesc, EState *estate);
extern HeapTuple ExecConstraints(char *caller, Relation rel, HeapTuple tuple);

/*
 * prototypes from functions in execProcnode.c
 */
extern bool ExecInitNode(Plan *node, EState *estate, Plan *parent);
extern TupleTableSlot *ExecProcNode(Plan *node, Plan *parent);
extern int	ExecCountSlotsNode(Plan *node);
extern void ExecEndNode(Plan *node, Plan *parent);

/*
 * prototypes from functions in execQual.c
 */
extern bool execConstByVal;
extern int	execConstLen;

extern Datum
ExecExtractResult(TupleTableSlot *slot, AttrNumber attnum,
				  bool *isNull);
extern Datum
ExecEvalParam(Param *expression, ExprContext *econtext,
			  bool *isNull);

/* stop here */
extern char *
GetAttributeByNum(TupleTableSlot *slot, AttrNumber attrno,
				  bool *isNull);
extern char *GetAttributeByName(TupleTableSlot *slot, char *attname, bool *isNull);
extern Datum
ExecEvalExpr(Node *expression, ExprContext *econtext, bool *isNull,
			 bool *isDone);
extern bool ExecQual(List *qual, ExprContext *econtext);
extern int	ExecTargetListLength(List *targetlist);
extern TupleTableSlot *ExecProject(ProjectionInfo *projInfo, bool *isDone);

/*
 * prototypes from functions in execScan.c
 */
extern TupleTableSlot *ExecScan(Scan *node, TupleTableSlot *(*accessMtd) ());

/*
 * prototypes from functions in execTuples.c
 */
extern TupleTable ExecCreateTupleTable(int initialSize);
extern void ExecDestroyTupleTable(TupleTable table, bool shouldFree);
extern TupleTableSlot *ExecAllocTableSlot(TupleTable table);
extern TupleTableSlot *
ExecStoreTuple(HeapTuple tuple,
			   TupleTableSlot *slot,
			   Buffer buffer,
			   bool shouldFree);
extern TupleTableSlot *ExecClearTuple(TupleTableSlot *slot);
extern bool ExecSetSlotPolicy(TupleTableSlot *slot, bool shouldFree);
extern TupleDesc
ExecSetSlotDescriptor(TupleTableSlot *slot,
					  TupleDesc tupdesc);
extern void ExecSetSlotDescriptorIsNew(TupleTableSlot *slot, bool isNew);
extern void ExecInitResultTupleSlot(EState *estate, CommonState *commonstate);
extern void
ExecInitScanTupleSlot(EState *estate,
					  CommonScanState *commonscanstate);
extern void ExecInitMarkedTupleSlot(EState *estate, MergeJoinState *mergestate);
extern void ExecInitOuterTupleSlot(EState *estate, HashJoinState *hashstate);

extern TupleDesc ExecGetTupType(Plan *node);
extern TupleDesc ExecTypeFromTL(List *targetList);
extern void SetChangedParamList(Plan *node, List *newchg);

/*
 * prototypes from functions in execTuples.c
 */
extern void ResetTupleCount(void);
extern void
ExecAssignNodeBaseInfo(EState *estate, CommonState *basenode,
					   Plan *parent);
extern void ExecAssignExprContext(EState *estate, CommonState *commonstate);
extern void
ExecAssignResultType(CommonState *commonstate,
					 TupleDesc tupDesc);
extern void
ExecAssignResultTypeFromOuterPlan(Plan *node,
								  CommonState *commonstate);
extern void ExecAssignResultTypeFromTL(Plan *node, CommonState *commonstate);
extern TupleDesc ExecGetResultType(CommonState *commonstate);
extern void ExecAssignProjectionInfo(Plan *node, CommonState *commonstate);
extern void ExecFreeProjectionInfo(CommonState *commonstate);
extern TupleDesc ExecGetScanType(CommonScanState *csstate);
extern void
ExecAssignScanType(CommonScanState *csstate,
				   TupleDesc tupDesc);
extern void
ExecAssignScanTypeFromOuterPlan(Plan *node,
								CommonScanState *csstate);
extern AttributeTupleForm ExecGetTypeInfo(Relation relDesc);

extern void
ExecOpenIndices(Oid resultRelationOid,
				RelationInfo *resultRelationInfo);
extern void ExecCloseIndices(RelationInfo *resultRelationInfo);
extern void
ExecInsertIndexTuples(TupleTableSlot *slot, ItemPointer tupleid,
					  EState *estate, bool is_update);

#endif							/* EXECUTOR_H  */
