/*-------------------------------------------------------------------------
 *
 * execUtils.c
 *	  miscellaneous executor utility routines
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execUtils.c,v 1.69 2000/11/16 22:30:20 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecAssignExprContext	Common code for plan node init routines.
 *
 *		ExecGetTypeInfo			  |  old execCStructs interface
 *		ExecMakeTypeInfo		  |  code from the version 1
 *		ExecOrderTypeInfo		  |  lisp system.  These should
 *		ExecSetTypeInfo			  |  go away or be updated soon.
 *		ExecFreeTypeInfo		  |  -cim 11/1/89
 *		ExecTupleAttributes		/
 *

 *		QueryDescGetTypeInfo - moved here from main.c
 *								am not sure what uses it -cim 10/12/89
 *
 *		ExecOpenIndices			\
 *		ExecCloseIndices		 | referenced by InitPlan, EndPlan,
 *		ExecInsertIndexTuples	/  ExecAppend, ExecReplace
 *
 *	 NOTES
 *		This file has traditionally been the place to stick misc.
 *		executor support stuff that doesn't really go anyplace else.
 *
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/index.h"
#include "catalog/catalog.h"
#include "catalog/pg_index.h"
#include "executor/execdebug.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/syscache.h"


/* ----------------------------------------------------------------
 *		global counters for number of tuples processed, retrieved,
 *		appended, replaced, deleted.
 * ----------------------------------------------------------------
 */
int			NTupleProcessed;
int			NTupleRetrieved;
int			NTupleReplaced;
int			NTupleAppended;
int			NTupleDeleted;
int			NIndexTupleInserted;
extern int	NIndexTupleProcessed;		/* have to be defined in the
										 * access method level so that the
										 * cinterface.a will link ok. */

/* ----------------------------------------------------------------
 *						statistic functions
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ResetTupleCount
 * ----------------------------------------------------------------
 */
#ifdef NOT_USED
void
ResetTupleCount(void)
{
	NTupleProcessed = 0;
	NTupleRetrieved = 0;
	NTupleAppended = 0;
	NTupleDeleted = 0;
	NTupleReplaced = 0;
	NIndexTupleProcessed = 0;
}

#endif

/* ----------------------------------------------------------------
 *		PrintTupleCount
 * ----------------------------------------------------------------
 */
#ifdef NOT_USED
void
DisplayTupleCount(FILE *statfp)
{
	if (NTupleProcessed > 0)
		fprintf(statfp, "!\t%d tuple%s processed, ", NTupleProcessed,
				(NTupleProcessed == 1) ? "" : "s");
	else
	{
		fprintf(statfp, "!\tno tuples processed.\n");
		return;
	}
	if (NIndexTupleProcessed > 0)
		fprintf(statfp, "%d indextuple%s processed, ", NIndexTupleProcessed,
				(NIndexTupleProcessed == 1) ? "" : "s");
	if (NIndexTupleInserted > 0)
		fprintf(statfp, "%d indextuple%s inserted, ", NIndexTupleInserted,
				(NIndexTupleInserted == 1) ? "" : "s");
	if (NTupleRetrieved > 0)
		fprintf(statfp, "%d tuple%s retrieved. ", NTupleRetrieved,
				(NTupleRetrieved == 1) ? "" : "s");
	if (NTupleAppended > 0)
		fprintf(statfp, "%d tuple%s appended. ", NTupleAppended,
				(NTupleAppended == 1) ? "" : "s");
	if (NTupleDeleted > 0)
		fprintf(statfp, "%d tuple%s deleted. ", NTupleDeleted,
				(NTupleDeleted == 1) ? "" : "s");
	if (NTupleReplaced > 0)
		fprintf(statfp, "%d tuple%s replaced. ", NTupleReplaced,
				(NTupleReplaced == 1) ? "" : "s");
	fprintf(statfp, "\n");
}

#endif

/* ----------------------------------------------------------------
 *				 miscellaneous node-init support functions
 *
 *		ExecAssignExprContext	- assigns the node's expression context
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExecAssignExprContext
 *
 *		This initializes the ExprContext field.  It is only necessary
 *		to do this for nodes which use ExecQual or ExecProject
 *		because those routines depend on econtext.	Other nodes that
 *		don't have to evaluate expressions don't need to do this.
 *
 * Note: we assume CurrentMemoryContext is the correct per-query context.
 * This should be true during plan node initialization.
 * ----------------
 */
void
ExecAssignExprContext(EState *estate, CommonState *commonstate)
{
	ExprContext *econtext = makeNode(ExprContext);

	econtext->ecxt_scantuple = NULL;
	econtext->ecxt_innertuple = NULL;
	econtext->ecxt_outertuple = NULL;
	econtext->ecxt_per_query_memory = CurrentMemoryContext;
	/*
	 * Create working memory for expression evaluation in this context.
	 */
	econtext->ecxt_per_tuple_memory =
		AllocSetContextCreate(CurrentMemoryContext,
							  "PlanExprContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);
	econtext->ecxt_param_exec_vals = estate->es_param_exec_vals;
	econtext->ecxt_param_list_info = estate->es_param_list_info;
	econtext->ecxt_aggvalues = NULL;
	econtext->ecxt_aggnulls = NULL;

	commonstate->cs_ExprContext = econtext;
}

/* ----------------
 *		MakeExprContext
 *
 *		Build an expression context for use outside normal plan-node cases.
 *		A fake scan-tuple slot can be supplied (pass NULL if not needed).
 *		A memory context sufficiently long-lived to use as fcache context
 *		must be supplied as well.
 * ----------------
 */
ExprContext *
MakeExprContext(TupleTableSlot *slot,
				MemoryContext queryContext)
{
	ExprContext *econtext = makeNode(ExprContext);

	econtext->ecxt_scantuple = slot;
	econtext->ecxt_innertuple = NULL;
	econtext->ecxt_outertuple = NULL;
	econtext->ecxt_per_query_memory = queryContext;
	/*
	 * We make the temporary context a child of current working context,
	 * not of the specified queryContext.  This seems reasonable but I'm
	 * not totally sure about it...
	 *
	 * Expression contexts made via this routine typically don't live long
	 * enough to get reset, so specify a minsize of 0.  That avoids alloc'ing
	 * any memory in the common case where expr eval doesn't use any.
	 */
	econtext->ecxt_per_tuple_memory =
		AllocSetContextCreate(CurrentMemoryContext,
							  "TempExprContext",
							  0,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);
	econtext->ecxt_param_exec_vals = NULL;
	econtext->ecxt_param_list_info = NULL;
	econtext->ecxt_aggvalues = NULL;
	econtext->ecxt_aggnulls = NULL;

	return econtext;
}

/*
 * Free an ExprContext made by MakeExprContext, including the temporary
 * context used for expression evaluation.  Note this will cause any
 * pass-by-reference expression result to go away!
 */
void
FreeExprContext(ExprContext *econtext)
{
	MemoryContextDelete(econtext->ecxt_per_tuple_memory);
	pfree(econtext);
}

/* ----------------------------------------------------------------
 *		Result slot tuple type and ProjectionInfo support
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExecAssignResultType
 * ----------------
 */
void
ExecAssignResultType(CommonState *commonstate,
					 TupleDesc tupDesc)
{
	TupleTableSlot *slot;

	slot = commonstate->cs_ResultTupleSlot;
	slot->ttc_tupleDescriptor = tupDesc;
}

/* ----------------
 *		ExecAssignResultTypeFromOuterPlan
 * ----------------
 */
void
ExecAssignResultTypeFromOuterPlan(Plan *node, CommonState *commonstate)
{
	Plan	   *outerPlan;
	TupleDesc	tupDesc;

	outerPlan = outerPlan(node);
	tupDesc = ExecGetTupType(outerPlan);

	ExecAssignResultType(commonstate, tupDesc);
}

/* ----------------
 *		ExecAssignResultTypeFromTL
 * ----------------
 */
void
ExecAssignResultTypeFromTL(Plan *node, CommonState *commonstate)
{
	List	   *targetList;
	TupleDesc	tupDesc;
	int			len;

	targetList = node->targetlist;
	tupDesc = ExecTypeFromTL(targetList);
	len = ExecTargetListLength(targetList);

	if (len > 0)
		ExecAssignResultType(commonstate, tupDesc);
	else
		ExecAssignResultType(commonstate, (TupleDesc) NULL);
}

/* ----------------
 *		ExecGetResultType
 * ----------------
 */
TupleDesc
ExecGetResultType(CommonState *commonstate)
{
	TupleTableSlot *slot = commonstate->cs_ResultTupleSlot;

	return slot->ttc_tupleDescriptor;
}

/* ----------------
 *		ExecFreeResultType
 * ----------------
 */
#ifdef NOT_USED
void
ExecFreeResultType(CommonState *commonstate)
{
	TupleTableSlot *slot;
	TupleDesc	tupType;

	slot = commonstate->cs_ResultTupleSlot;
	tupType = slot->ttc_tupleDescriptor;

	ExecFreeTypeInfo(tupType);
}

#endif

/* ----------------
 *		ExecAssignProjectionInfo
		  forms the projection information from the node's targetlist
 * ----------------
 */
void
ExecAssignProjectionInfo(Plan *node, CommonState *commonstate)
{
	ProjectionInfo *projInfo;
	List	   *targetList;
	int			len;

	targetList = node->targetlist;
	len = ExecTargetListLength(targetList);

	projInfo = makeNode(ProjectionInfo);
	projInfo->pi_targetlist = targetList;
	projInfo->pi_len = len;
	projInfo->pi_tupValue = (len <= 0) ? NULL : (Datum *) palloc(sizeof(Datum) * len);
	projInfo->pi_exprContext = commonstate->cs_ExprContext;
	projInfo->pi_slot = commonstate->cs_ResultTupleSlot;

	commonstate->cs_ProjInfo = projInfo;
}


/* ----------------
 *		ExecFreeProjectionInfo
 * ----------------
 */
void
ExecFreeProjectionInfo(CommonState *commonstate)
{
	ProjectionInfo *projInfo;

	/* ----------------
	 *	get projection info.  if NULL then this node has
	 *	none so we just return.
	 * ----------------
	 */
	projInfo = commonstate->cs_ProjInfo;
	if (projInfo == NULL)
		return;

	/* ----------------
	 *	clean up memory used.
	 * ----------------
	 */
	if (projInfo->pi_tupValue != NULL)
		pfree(projInfo->pi_tupValue);

	pfree(projInfo);
	commonstate->cs_ProjInfo = NULL;
}

/* ----------------
 *		ExecFreeExprContext
 * ----------------
 */
void
ExecFreeExprContext(CommonState *commonstate)
{
	ExprContext *econtext;

	/* ----------------
	 *	get expression context.  if NULL then this node has
	 *	none so we just return.
	 * ----------------
	 */
	econtext = commonstate->cs_ExprContext;
	if (econtext == NULL)
		return;

	/* ----------------
	 *	clean up memory used.
	 * ----------------
	 */
	MemoryContextDelete(econtext->ecxt_per_tuple_memory);
	pfree(econtext);
	commonstate->cs_ExprContext = NULL;
}

/* ----------------
 *		ExecFreeTypeInfo
 * ----------------
 */
#ifdef NOT_USED
void
ExecFreeTypeInfo(CommonState *commonstate)
{
	TupleDesc	tupDesc;

	tupDesc = commonstate->cs_ResultTupleSlot->ttc_tupleDescriptor;
	if (tupDesc == NULL)
		return;

	/* ----------------
	 *	clean up memory used.
	 * ----------------
	 */
	FreeTupleDesc(tupDesc);
	commonstate->cs_ResultTupleSlot->ttc_tupleDescriptor = NULL;
}
#endif

/* ----------------------------------------------------------------
 *		the following scan type support functions are for
 *		those nodes which are stubborn and return tuples in
 *		their Scan tuple slot instead of their Result tuple
 *		slot..	luck fur us, these nodes do not do projections
 *		so we don't have to worry about getting the ProjectionInfo
 *		right for them...  -cim 6/3/91
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExecGetScanType
 * ----------------
 */
TupleDesc
ExecGetScanType(CommonScanState *csstate)
{
	TupleTableSlot *slot = csstate->css_ScanTupleSlot;

	return slot->ttc_tupleDescriptor;
}

/* ----------------
 *		ExecFreeScanType
 * ----------------
 */
#ifdef NOT_USED
void
ExecFreeScanType(CommonScanState *csstate)
{
	TupleTableSlot *slot;
	TupleDesc	tupType;

	slot = csstate->css_ScanTupleSlot;
	tupType = slot->ttc_tupleDescriptor;

	ExecFreeTypeInfo(tupType);
}

#endif

/* ----------------
 *		ExecAssignScanType
 * ----------------
 */
void
ExecAssignScanType(CommonScanState *csstate,
				   TupleDesc tupDesc)
{
	TupleTableSlot *slot;

	slot = (TupleTableSlot *) csstate->css_ScanTupleSlot;
	slot->ttc_tupleDescriptor = tupDesc;
}

/* ----------------
 *		ExecAssignScanTypeFromOuterPlan
 * ----------------
 */
void
ExecAssignScanTypeFromOuterPlan(Plan *node, CommonScanState *csstate)
{
	Plan	   *outerPlan;
	TupleDesc	tupDesc;

	outerPlan = outerPlan(node);
	tupDesc = ExecGetTupType(outerPlan);

	ExecAssignScanType(csstate, tupDesc);
}


/* ----------------------------------------------------------------
 *		ExecTypeFromTL support routines.
 *
 *		these routines are used mainly from ExecTypeFromTL.
 *		-cim 6/12/90
 *
 * old comments
 *		Routines dealing with the structure 'attribute' which conatains
 *		the type information about attributes in a tuple:
 *
 *		ExecMakeTypeInfo(noType)
 *				returns pointer to array of 'noType' structure 'attribute'.
 *		ExecSetTypeInfo(index, typeInfo, attNum, attLen)
 *				sets the element indexed by 'index' in typeInfo with
 *				the values: attNum, attLen.
 *		ExecFreeTypeInfo(typeInfo)
 *				frees the structure 'typeInfo'.
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExecSetTypeInfo
 *
 *		This initializes fields of a single attribute in a
 *		tuple descriptor from the specified parameters.
 *
 *		XXX this duplicates much of the functionality of TupleDescInitEntry.
 *			the routines should be moved to the same place and be rewritten
 *			to share common code.
 * ----------------
 */
#ifdef NOT_USED
void
ExecSetTypeInfo(int index,
				TupleDesc typeInfo,
				Oid typeID,
				int attNum,
				int attLen,
				char *attName,
				bool attbyVal,
				char attalign)
{
	Form_pg_attribute att;

	/* ----------------
	 *	get attribute pointer and preform a sanity check..
	 * ----------------
	 */
	att = typeInfo[index];
	if (att == NULL)
		elog(ERROR, "ExecSetTypeInfo: trying to assign through NULL");

	/* ----------------
	 *	assign values to the tuple descriptor, being careful not
	 *	to copy a null attName..
	 *
	 *	XXX it is unknown exactly what information is needed to
	 *		initialize the attribute struct correctly so for now
	 *		we use 0.  this should be fixed -- otherwise we run the
	 *		risk of using garbage data. -cim 5/5/91
	 * ----------------
	 */
	att->attrelid = 0;			/* dummy value */

	if (attName != (char *) NULL)
		StrNCpy(NameStr(att->attname), attName, NAMEDATALEN);
	else
		MemSet(NameStr(att->attname), 0, NAMEDATALEN);

	att->atttypid = typeID;
	att->attdefrel = 0;			/* dummy value */
	att->attdispersion = 0;		/* dummy value */
	att->atttyparg = 0;			/* dummy value */
	att->attlen = attLen;
	att->attnum = attNum;
	att->attbound = 0;			/* dummy value */
	att->attbyval = attbyVal;
	att->attcanindex = 0;		/* dummy value */
	att->attproc = 0;			/* dummy value */
	att->attnelems = 0;			/* dummy value */
	att->attcacheoff = -1;
	att->atttypmod = -1;
	att->attisset = false;
	att->attstorage = 'p';
	att->attalign = attalign;
}

/* ----------------
 *		ExecFreeTypeInfo frees the array of attrbutes
 *		created by ExecMakeTypeInfo and returned by ExecTypeFromTL...
 * ----------------
 */
void
ExecFreeTypeInfo(TupleDesc typeInfo)
{
	/* ----------------
	 *	do nothing if asked to free a null pointer
	 * ----------------
	 */
	if (typeInfo == NULL)
		return;

	/* ----------------
	 *	the entire array of typeinfo pointers created by
	 *	ExecMakeTypeInfo was allocated with a single palloc()
	 *	so we can deallocate the whole array with a single pfree().
	 *	(we should not try and free all the elements in the array)
	 *	-cim 6/12/90
	 * ----------------
	 */
	pfree(typeInfo);
}


/* ----------------------------------------------------------------
 *		QueryDescGetTypeInfo
 *
 *|		I don't know how this is used, all I know is that it
 *|		appeared one day in main.c so I moved it here. -cim 11/1/89
 * ----------------------------------------------------------------
 */
TupleDesc
QueryDescGetTypeInfo(QueryDesc *queryDesc)
{
	Plan	   *plan;
	TupleDesc	tupleType;
	List	   *targetList;
	AttrInfo   *attinfo = (AttrInfo *) palloc(sizeof(AttrInfo));

	plan = queryDesc->plantree;
	tupleType = (TupleDesc) ExecGetTupType(plan);
/*
	targetList =  plan->targetlist;

	attinfo->numAttr = ExecTargetListLength(targetList);
	attinfo->attrs = tupleType;
*/
	attinfo->numAttr = tupleType->natts;
	attinfo->attrs = tupleType->attrs;
	return attinfo;
}

#endif

/* ----------------------------------------------------------------
 *				  ExecInsertIndexTuples support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecOpenIndices
 *
 *		Find the indices associated with a result relation, open them,
 *		and save information about them in the result ResultRelInfo.
 *
 *		At entry, caller has already opened and locked
 *		resultRelInfo->ri_RelationDesc.
 *
 *		This used to be horribly ugly code, and slow too because it
 *		did a sequential scan of pg_index.  Now we rely on the relcache
 *		to cache a list of the OIDs of the indices associated with any
 *		specific relation, and we use the pg_index syscache to get the
 *		entries we need from pg_index.
 * ----------------------------------------------------------------
 */
void
ExecOpenIndices(ResultRelInfo *resultRelInfo)
{
	Relation	resultRelation = resultRelInfo->ri_RelationDesc;
	List	   *indexoidlist,
			   *indexoidscan;
	int			len,
				i;
	RelationPtr relationDescs;
	IndexInfo **indexInfoArray;

	resultRelInfo->ri_NumIndices = 0;

	/* checks for disabled indexes */
	if (! RelationGetForm(resultRelation)->relhasindex)
		return;
	if (IsIgnoringSystemIndexes() &&
		IsSystemRelationName(RelationGetRelationName(resultRelation)))
		return;

	/* ----------------
	 *	 Get cached list of index OIDs
	 * ----------------
	 */
	indexoidlist = RelationGetIndexList(resultRelation);
	len = length(indexoidlist);
	if (len == 0)
		return;

	/* ----------------
	 *	 allocate space for result arrays
	 * ----------------
	 */
	relationDescs = (RelationPtr) palloc(len * sizeof(Relation));
	indexInfoArray = (IndexInfo **) palloc(len * sizeof(IndexInfo *));

	resultRelInfo->ri_NumIndices = len;
	resultRelInfo->ri_IndexRelationDescs = relationDescs;
	resultRelInfo->ri_IndexRelationInfo = indexInfoArray;

	/* ----------------
	 *	 For each index, open the index relation and save pg_index info.
	 * ----------------
	 */
	i = 0;
	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexOid = lfirsti(indexoidscan);
		Relation	indexDesc;
		HeapTuple	indexTuple;
		IndexInfo  *ii;

		/* ----------------
		 * Open (and lock, if necessary) the index relation
		 *
		 * Hack for not btree and hash indices: they use relation level
		 * exclusive locking on update (i.e. - they are not ready for MVCC)
		 * and so we have to exclusively lock indices here to prevent
		 * deadlocks if we will scan them - index_beginscan places
		 * AccessShareLock, indices update methods don't use locks at all.
		 * We release this lock in ExecCloseIndices. Note, that hashes use
		 * page level locking - i.e. are not deadlock-free - let's them be
		 * on their way -:)) vadim 03-12-1998
		 *
		 * If there are multiple not-btree-or-hash indices, all backends must
		 * lock the indices in the same order or we will get deadlocks here
		 * during concurrent updates.  This is now guaranteed by
		 * RelationGetIndexList(), which promises to return the index list
		 * in OID order.  tgl 06-19-2000
		 * ----------------
		 */
		indexDesc = index_open(indexOid);

		if (indexDesc->rd_rel->relam != BTREE_AM_OID &&
			indexDesc->rd_rel->relam != HASH_AM_OID)
			LockRelation(indexDesc, AccessExclusiveLock);

		/* ----------------
		 *	Get the pg_index tuple for the index
		 * ----------------
		 */
		indexTuple = SearchSysCache(INDEXRELID,
									ObjectIdGetDatum(indexOid),
									0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "ExecOpenIndices: index %u not found", indexOid);

		/* ----------------
		 *	extract the index key information from the tuple
		 * ----------------
		 */
		ii = BuildIndexInfo(indexTuple);

		ReleaseSysCache(indexTuple);

		relationDescs[i] = indexDesc;
		indexInfoArray[i] = ii;
		i++;
	}

	freeList(indexoidlist);
}

/* ----------------------------------------------------------------
 *		ExecCloseIndices
 *
 *		Close the index relations stored in resultRelInfo
 * ----------------------------------------------------------------
 */
void
ExecCloseIndices(ResultRelInfo *resultRelInfo)
{
	int			i;
	int			numIndices;
	RelationPtr relationDescs;

	numIndices = resultRelInfo->ri_NumIndices;
	relationDescs = resultRelInfo->ri_IndexRelationDescs;

	for (i = 0; i < numIndices; i++)
	{
		if (relationDescs[i] == NULL)
			continue;

		/*
		 * See notes in ExecOpenIndices.
		 */
		if (relationDescs[i]->rd_rel->relam != BTREE_AM_OID &&
			relationDescs[i]->rd_rel->relam != HASH_AM_OID)
			UnlockRelation(relationDescs[i], AccessExclusiveLock);

		index_close(relationDescs[i]);
	}

	/*
	 * XXX should free indexInfo array here too.
	 */
}

/* ----------------------------------------------------------------
 *		ExecInsertIndexTuples
 *
 *		This routine takes care of inserting index tuples
 *		into all the relations indexing the result relation
 *		when a heap tuple is inserted into the result relation.
 *		Much of this code should be moved into the genam
 *		stuff as it only exists here because the genam stuff
 *		doesn't provide the functionality needed by the
 *		executor.. -cim 9/27/89
 * ----------------------------------------------------------------
 */
void
ExecInsertIndexTuples(TupleTableSlot *slot,
					  ItemPointer tupleid,
					  EState *estate,
					  bool is_update)
{
	HeapTuple	heapTuple;
	ResultRelInfo *resultRelInfo;
	int			i;
	int			numIndices;
	RelationPtr relationDescs;
	Relation	heapRelation;
	TupleDesc	heapDescriptor;
	IndexInfo **indexInfoArray;
	ExprContext *econtext;
	Datum		datum[INDEX_MAX_KEYS];
	char		nullv[INDEX_MAX_KEYS];

	heapTuple = slot->val;

	/*
	 * Get information from the result relation info structure.
	 */
	resultRelInfo = estate->es_result_relation_info;
	numIndices = resultRelInfo->ri_NumIndices;
	relationDescs = resultRelInfo->ri_IndexRelationDescs;
	indexInfoArray = resultRelInfo->ri_IndexRelationInfo;
	heapRelation = resultRelInfo->ri_RelationDesc;
	heapDescriptor = RelationGetDescr(heapRelation);

	/*
	 * We will use the EState's per-tuple context for evaluating predicates
	 * and functional-index functions.  Create it if it's not already there;
	 * if it is, reset it to free previously-used storage.
	 */
	econtext = estate->es_per_tuple_exprcontext;
	if (econtext == NULL)
	{
		MemoryContext	oldContext;

		oldContext = MemoryContextSwitchTo(estate->es_query_cxt);
		estate->es_per_tuple_exprcontext = econtext =
			MakeExprContext(NULL, estate->es_query_cxt);
		MemoryContextSwitchTo(oldContext);
	}
	else
		ResetExprContext(econtext);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* ----------------
	 *	for each index, form and insert the index tuple
	 * ----------------
	 */
	for (i = 0; i < numIndices; i++)
	{
		IndexInfo  *indexInfo;
		Node	   *predicate;
		InsertIndexResult result;

		if (relationDescs[i] == NULL)
			continue;

		indexInfo = indexInfoArray[i];
		predicate = indexInfo->ii_Predicate;
		if (predicate != NULL)
		{
			/* Skip this index-update if the predicate isn't satisfied */
			if (!ExecQual((List *) predicate, econtext, false))
				continue;
		}

		/* ----------------
		 *	FormIndexDatum fills in its datum and null parameters
		 *	with attribute information taken from the given heap tuple.
		 * ----------------
		 */
		FormIndexDatum(indexInfo,
					   heapTuple,
					   heapDescriptor,
					   econtext->ecxt_per_tuple_memory,
					   datum,
					   nullv);

		result = index_insert(relationDescs[i], /* index relation */
							  datum,	/* array of heaptuple Datums */
							  nullv,	/* info on nulls */
							  &(heapTuple->t_self),		/* tid of heap tuple */
							  heapRelation);

		/* ----------------
		 *		keep track of index inserts for debugging
		 * ----------------
		 */
		IncrIndexInserted();

		if (result)
			pfree(result);
	}
}

void
SetChangedParamList(Plan *node, List *newchg)
{
	List	   *nl;

	foreach(nl, newchg)
	{
		int			paramId = lfirsti(nl);

		/* if this node doesn't depend on a param ... */
		if (!intMember(paramId, node->extParam) &&
			!intMember(paramId, node->locParam))
			continue;
		/* if this param is already in list of changed ones ... */
		if (intMember(paramId, node->chgParam))
			continue;
		/* else - add this param to the list */
		node->chgParam = lappendi(node->chgParam, paramId);
	}
}
