/*-------------------------------------------------------------------------
 *
 * executor.h
 *	  support for the POSTGRES executor module
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: executor.h,v 1.50 2000/08/24 23:34:09 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "executor/execdesc.h"

/* ----------------
 *		TupIsNull
 *
 *		This is used mainly to detect when there are no more
 *		tuples to process.
 * ----------------
 */
/* return: true if tuple in slot is NULL, slot is slot to test */
#define TupIsNull(slot) \
	((slot) == NULL || (slot)->val == NULL)

/*
 * prototypes from functions in execAmi.c
 */
extern void ExecOpenScanR(Oid relOid, int nkeys, ScanKey skeys, bool isindex,
			  ScanDirection dir, Snapshot snapshot,
			  Relation *returnRelation, Pointer *returnScanDesc);
extern void ExecCloseR(Plan *node);
extern void ExecReScan(Plan *node, ExprContext *exprCtxt, Plan *parent);
extern HeapScanDesc ExecReScanR(Relation relDesc, HeapScanDesc scanDesc,
			ScanDirection direction, int nkeys, ScanKey skeys);
extern void ExecMarkPos(Plan *node);
extern void ExecRestrPos(Plan *node);

/*
 * prototypes from functions in execJunk.c
 */
extern JunkFilter *ExecInitJunkFilter(List *targetList, TupleDesc tupType);
extern bool ExecGetJunkAttribute(JunkFilter *junkfilter, TupleTableSlot *slot,
					 char *attrName, Datum *value, bool *isNull);
extern HeapTuple ExecRemoveJunk(JunkFilter *junkfilter, TupleTableSlot *slot);


/*
 * prototypes from functions in execMain.c
 */
extern TupleDesc ExecutorStart(QueryDesc *queryDesc, EState *estate);
extern TupleTableSlot *ExecutorRun(QueryDesc *queryDesc, EState *estate,
			int feature, Node *limoffset, Node *limcount);
extern void ExecutorEnd(QueryDesc *queryDesc, EState *estate);
extern void ExecConstraints(char *caller, Relation rel,
							TupleTableSlot *slot, EState *estate);
extern TupleTableSlot *EvalPlanQual(EState *estate, Index rti,
									ItemPointer tid);

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
extern Datum ExecEvalParam(Param *expression, ExprContext *econtext,
			  bool *isNull);
extern Datum GetAttributeByNum(TupleTableSlot *slot, AttrNumber attrno,
							   bool *isNull);
extern Datum GetAttributeByName(TupleTableSlot *slot, char *attname,
								bool *isNull);
extern Datum ExecMakeFunctionResult(FunctionCachePtr fcache,
									List *arguments,
									ExprContext *econtext,
									bool *isNull,
									ExprDoneCond *isDone);
extern Datum ExecEvalExpr(Node *expression, ExprContext *econtext,
						  bool *isNull, ExprDoneCond *isDone);
extern Datum ExecEvalExprSwitchContext(Node *expression, ExprContext *econtext,
									   bool *isNull, ExprDoneCond *isDone);
extern bool ExecQual(List *qual, ExprContext *econtext, bool resultForNull);
extern int	ExecTargetListLength(List *targetlist);
extern int	ExecCleanTargetListLength(List *targetlist);
extern TupleTableSlot *ExecProject(ProjectionInfo *projInfo,
								   ExprDoneCond *isDone);

/*
 * prototypes from functions in execScan.c
 */
typedef TupleTableSlot *(*ExecScanAccessMtd) (Scan *node);

extern TupleTableSlot *ExecScan(Scan *node, ExecScanAccessMtd accessMtd);

/*
 * prototypes from functions in execTuples.c
 */
extern TupleTable ExecCreateTupleTable(int initialSize);
extern void ExecDropTupleTable(TupleTable table, bool shouldFree);
extern TupleTableSlot *ExecAllocTableSlot(TupleTable table);
extern TupleTableSlot *ExecStoreTuple(HeapTuple tuple,
			   TupleTableSlot *slot,
			   Buffer buffer,
			   bool shouldFree);
extern TupleTableSlot *ExecClearTuple(TupleTableSlot *slot);
extern TupleDesc ExecSetSlotDescriptor(TupleTableSlot *slot,
					  TupleDesc tupdesc);
extern void ExecSetSlotDescriptorIsNew(TupleTableSlot *slot, bool isNew);
extern void ExecInitResultTupleSlot(EState *estate, CommonState *commonstate);
extern void ExecInitScanTupleSlot(EState *estate,
					  CommonScanState *commonscanstate);
extern void ExecInitOuterTupleSlot(EState *estate, HashJoinState *hashstate);

extern TupleDesc ExecGetTupType(Plan *node);
extern TupleDesc ExecTypeFromTL(List *targetList);
extern void SetChangedParamList(Plan *node, List *newchg);

/*
 * prototypes from functions in execUtils.c
 */
extern void ResetTupleCount(void);
extern void ExecAssignExprContext(EState *estate, CommonState *commonstate);
extern void ExecAssignResultType(CommonState *commonstate,
					 TupleDesc tupDesc);
extern void ExecAssignResultTypeFromOuterPlan(Plan *node,
								  CommonState *commonstate);
extern void ExecAssignResultTypeFromTL(Plan *node, CommonState *commonstate);
extern TupleDesc ExecGetResultType(CommonState *commonstate);
extern void ExecAssignProjectionInfo(Plan *node, CommonState *commonstate);
extern void ExecFreeProjectionInfo(CommonState *commonstate);
extern void ExecFreeExprContext(CommonState *commonstate);
extern TupleDesc ExecGetScanType(CommonScanState *csstate);
extern void ExecAssignScanType(CommonScanState *csstate,
				   TupleDesc tupDesc);
extern void ExecAssignScanTypeFromOuterPlan(Plan *node,
								CommonScanState *csstate);
extern Form_pg_attribute ExecGetTypeInfo(Relation relDesc);

extern ExprContext *MakeExprContext(TupleTableSlot *slot,
									MemoryContext queryContext);
extern void FreeExprContext(ExprContext *econtext);

#define ResetExprContext(econtext) \
	MemoryContextReset((econtext)->ecxt_per_tuple_memory)

extern void ExecOpenIndices(RelationInfo *resultRelationInfo);
extern void ExecCloseIndices(RelationInfo *resultRelationInfo);
extern void ExecInsertIndexTuples(TupleTableSlot *slot, ItemPointer tupleid,
					  EState *estate, bool is_update);

#endif	 /* EXECUTOR_H	*/
