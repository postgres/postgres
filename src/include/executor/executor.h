/*-------------------------------------------------------------------------
 *
 * executor.h
 *	  support for the POSTGRES executor module
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: executor.h,v 1.56 2001/01/29 00:39:20 tgl Exp $
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
extern void ExecFreeJunkFilter(JunkFilter *junkfilter);
extern bool ExecGetJunkAttribute(JunkFilter *junkfilter, TupleTableSlot *slot,
					 char *attrName, Datum *value, bool *isNull);
extern HeapTuple ExecRemoveJunk(JunkFilter *junkfilter, TupleTableSlot *slot);


/*
 * prototypes from functions in execMain.c
 */
extern TupleDesc ExecutorStart(QueryDesc *queryDesc, EState *estate);
extern TupleTableSlot *ExecutorRun(QueryDesc *queryDesc, EState *estate,
								   int feature, long count);
extern void ExecutorEnd(QueryDesc *queryDesc, EState *estate);
extern void ExecConstraints(char *caller, ResultRelInfo *resultRelInfo,
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
extern TupleDesc ExecGetTupType(Plan *node);

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
extern TupleTableSlot *MakeTupleTableSlot(void);
extern TupleTableSlot *ExecStoreTuple(HeapTuple tuple,
			   TupleTableSlot *slot,
			   Buffer buffer,
			   bool shouldFree);
extern TupleTableSlot *ExecClearTuple(TupleTableSlot *slot);
extern void ExecSetSlotDescriptor(TupleTableSlot *slot,
								  TupleDesc tupdesc, bool shouldFree);
extern void ExecSetSlotDescriptorIsNew(TupleTableSlot *slot, bool isNew);
extern void ExecInitResultTupleSlot(EState *estate, CommonState *commonstate);
extern void ExecInitScanTupleSlot(EState *estate,
					  CommonScanState *commonscanstate);
extern TupleTableSlot *ExecInitExtraTupleSlot(EState *estate);
extern TupleTableSlot *ExecInitNullTupleSlot(EState *estate,
											 TupleDesc tupType);
extern TupleDesc ExecTypeFromTL(List *targetList);
extern void SetChangedParamList(Plan *node, List *newchg);

/*
 * prototypes from functions in execUtils.c
 */
extern void ResetTupleCount(void);
extern void ExecAssignExprContext(EState *estate, CommonState *commonstate);
extern void ExecAssignResultType(CommonState *commonstate,
								 TupleDesc tupDesc, bool shouldFree);
extern void ExecAssignResultTypeFromOuterPlan(Plan *node,
								  CommonState *commonstate);
extern void ExecAssignResultTypeFromTL(Plan *node, CommonState *commonstate);
extern TupleDesc ExecGetResultType(CommonState *commonstate);
extern void ExecAssignProjectionInfo(Plan *node, CommonState *commonstate);
extern void ExecFreeProjectionInfo(CommonState *commonstate);
extern void ExecFreeExprContext(CommonState *commonstate);
extern TupleDesc ExecGetScanType(CommonScanState *csstate);
extern void ExecAssignScanType(CommonScanState *csstate,
							   TupleDesc tupDesc, bool shouldFree);
extern void ExecAssignScanTypeFromOuterPlan(Plan *node,
								CommonScanState *csstate);
extern Form_pg_attribute ExecGetTypeInfo(Relation relDesc);

extern ExprContext *MakeExprContext(TupleTableSlot *slot,
									MemoryContext queryContext);
extern void FreeExprContext(ExprContext *econtext);

#define ResetExprContext(econtext) \
	MemoryContextReset((econtext)->ecxt_per_tuple_memory)

extern ExprContext *MakePerTupleExprContext(EState *estate);

/* Get an EState's per-output-tuple exprcontext, making it if first use */
#define GetPerTupleExprContext(estate) \
	((estate)->es_per_tuple_exprcontext ? \
	 (estate)->es_per_tuple_exprcontext : \
	 MakePerTupleExprContext(estate))

#define GetPerTupleMemoryContext(estate) \
	(GetPerTupleExprContext(estate)->ecxt_per_tuple_memory)

/* Reset an EState's per-output-tuple exprcontext, if one's been created */
#define ResetPerTupleExprContext(estate) \
	do { \
		if ((estate)->es_per_tuple_exprcontext) \
			ResetExprContext((estate)->es_per_tuple_exprcontext); \
	} while (0)

extern void ExecOpenIndices(ResultRelInfo *resultRelInfo);
extern void ExecCloseIndices(ResultRelInfo *resultRelInfo);
extern void ExecInsertIndexTuples(TupleTableSlot *slot, ItemPointer tupleid,
					  EState *estate, bool is_update);

#endif	 /* EXECUTOR_H	*/
