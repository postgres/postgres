/*-------------------------------------------------------------------------
 *
 * executor.h--
 *	  support for the POSTGRES executor module
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: executor.h,v 1.19 1998/02/10 04:02:19 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <catalog/pg_index.h>
#include <access/itup.h>
#include <stdio.h>
#include <executor/execdesc.h>

/*
 * prototypes from functions in execAmi.c
 */
extern void ExecOpenScanR(Oid relOid, int nkeys, ScanKey skeys, bool isindex,
			  ScanDirection dir, Relation *returnRelation,
			  Pointer *returnScanDesc);
extern void ExecCloseR(Plan *node);
extern void ExecReScan(Plan *node, ExprContext *exprCtxt, Plan *parent);
extern HeapScanDesc ExecReScanR(Relation relDesc, HeapScanDesc scanDesc,
			ScanDirection direction, int nkeys, ScanKey skeys);
extern void ExecMarkPos(Plan *node);
extern void ExecRestrPos(Plan *node);
extern Relation ExecCreatR(TupleDesc tupType, Oid relationOid);

/*
 * prototypes from functions in execJunk.c
 */
extern JunkFilter *ExecInitJunkFilter(List *targetList);
extern bool ExecGetJunkAttribute(JunkFilter *junkfilter, TupleTableSlot *slot,
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

extern Datum ExecExtractResult(TupleTableSlot *slot, AttrNumber attnum,
				  bool *isNull);
extern Datum ExecEvalParam(Param *expression, ExprContext *econtext,
			  bool *isNull);

/* stop here */
extern char *GetAttributeByNum(TupleTableSlot *slot, AttrNumber attrno,
			 bool *isNull);
extern char *GetAttributeByName(TupleTableSlot *slot, char *attname, bool *isNull);
extern Datum ExecEvalExpr(Node *expression, ExprContext *econtext, bool *isNull,
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
extern TupleTableSlot *ExecStoreTuple(HeapTuple tuple,
			   TupleTableSlot *slot,
			   Buffer buffer,
			   bool shouldFree);
extern TupleTableSlot *ExecClearTuple(TupleTableSlot *slot);
extern bool ExecSetSlotPolicy(TupleTableSlot *slot, bool shouldFree);
extern TupleDesc ExecSetSlotDescriptor(TupleTableSlot *slot,
					  TupleDesc tupdesc);
extern void ExecSetSlotDescriptorIsNew(TupleTableSlot *slot, bool isNew);
extern void ExecIncrSlotBufferRefcnt(TupleTableSlot *slot);
extern bool TupIsNull(TupleTableSlot *slot);
extern void ExecInitResultTupleSlot(EState *estate, CommonState *commonstate);
extern void ExecInitScanTupleSlot(EState *estate,
					  CommonScanState *commonscanstate);
extern void ExecInitMarkedTupleSlot(EState *estate, MergeJoinState *mergestate);
extern void ExecInitOuterTupleSlot(EState *estate, HashJoinState *hashstate);

extern TupleDesc ExecGetTupType(Plan *node);
extern TupleDesc ExecTypeFromTL(List *targetList);

/*
 * prototypes from functions in execTuples.c
 */
extern void ResetTupleCount(void);
extern void ExecAssignNodeBaseInfo(EState *estate, CommonState *basenode,
					   Plan *parent);
extern void ExecAssignExprContext(EState *estate, CommonState *commonstate);
extern void ExecAssignResultType(CommonState *commonstate,
					 TupleDesc tupDesc);
extern void ExecAssignResultTypeFromOuterPlan(Plan *node,
								  CommonState *commonstate);
extern void ExecAssignResultTypeFromTL(Plan *node, CommonState *commonstate);
extern TupleDesc ExecGetResultType(CommonState *commonstate);
extern void ExecAssignProjectionInfo(Plan *node, CommonState *commonstate);
extern void ExecFreeProjectionInfo(CommonState *commonstate);
extern TupleDesc ExecGetScanType(CommonScanState *csstate);
extern void ExecAssignScanType(CommonScanState *csstate,
				   TupleDesc tupDesc);
extern void ExecAssignScanTypeFromOuterPlan(Plan *node,
								CommonScanState *csstate);
extern AttributeTupleForm ExecGetTypeInfo(Relation relDesc);

extern void ExecOpenIndices(Oid resultRelationOid,
				RelationInfo *resultRelationInfo);
extern void ExecCloseIndices(RelationInfo *resultRelationInfo);
extern void ExecInsertIndexTuples(TupleTableSlot *slot, ItemPointer tupleid,
					  EState *estate, bool is_update);

#endif							/* EXECUTOR_H  */
