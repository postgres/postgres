/*-------------------------------------------------------------------------
 *
 * execUtils.c--
 *	  miscellanious executor utility routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execUtils.c,v 1.32 1998/06/15 19:28:20 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecAssignNodeBaseInfo	\
 *		ExecAssignDebugHooks	 >	preforms misc work done in all the
 *		ExecAssignExprContext	/	init node routines.
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
 *		ExecGetIndexKeyInfo		\
 *		ExecOpenIndices			 | referenced by InitPlan, EndPlan,
 *		ExecCloseIndices		 | ExecAppend, ExecReplace
 *		ExecFormIndexTuple		 |
 *		ExecInsertIndexTuple	/
 *
 *	 NOTES
 *		This file has traditionally been the place to stick misc.
 *		executor support stuff that doesn't really go anyplace else.
 *
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/itup.h"
#include "catalog/catname.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "commands/command.h"
#include "executor/execdebug.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "optimizer/clauses.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/mcxt.h"

static void
ExecGetIndexKeyInfo(IndexTupleForm indexTuple, int *numAttsOutP,
					AttrNumber **attsOutP, FuncIndexInfoPtr fInfoP);

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
 *				 miscellanious init node support functions
 *
 *		ExecAssignNodeBaseInfo	- assigns the baseid field of the node
 *		ExecAssignDebugHooks	- assigns the node's debugging hooks
 *		ExecAssignExprContext	- assigns the node's expression context
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExecAssignNodeBaseInfo
 *
 *		as it says, this assigns the baseid field of the node and
 *		increments the counter in the estate.  In addition, it initializes
 *		the base_parent field of the basenode.
 * ----------------
 */
void
ExecAssignNodeBaseInfo(EState *estate, CommonState *cstate, Plan *parent)
{
	int			baseId;

	baseId = estate->es_BaseId;
	cstate->cs_base_id = baseId;
	estate->es_BaseId = baseId + 1;
}

/* ----------------
 *		ExecAssignExprContext
 *
 *		This initializes the ExprContext field.  It is only necessary
 *		to do this for nodes which use ExecQual or ExecTargetList
 *		because those routines depend on econtext.	Other nodes which
 *		dont have to evaluate expressions don't need to do this.
 * ----------------
 */
void
ExecAssignExprContext(EState *estate, CommonState *commonstate)
{
	ExprContext *econtext;

	econtext = makeNode(ExprContext);
	econtext->ecxt_scantuple = NULL;	/* scan tuple slot */
	econtext->ecxt_innertuple = NULL;	/* inner tuple slot */
	econtext->ecxt_outertuple = NULL;	/* outer tuple slot */
	econtext->ecxt_relation = NULL;		/* relation */
	econtext->ecxt_relid = 0;	/* relid */
	econtext->ecxt_param_list_info = estate->es_param_list_info;
	econtext->ecxt_param_exec_vals = estate->es_param_exec_vals;
	econtext->ecxt_range_table = estate->es_range_table;		/* range table */

	commonstate->cs_ExprContext = econtext;
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
	int			i;
	int			len;
	List	   *tl;
	TargetEntry *tle;
	List	   *fjtl;
	TupleDesc	origTupDesc;

	targetList = node->targetlist;
	origTupDesc = ExecTypeFromTL(targetList);
	len = ExecTargetListLength(targetList);

	fjtl = NIL;
	tl = targetList;
	i = 0;
	while (tl != NIL || fjtl != NIL)
	{
		if (fjtl != NIL)
		{
			tle = lfirst(fjtl);
			fjtl = lnext(fjtl);
		}
		else
		{
			tle = lfirst(tl);
			tl = lnext(tl);
		}
#ifdef SETS_FIXED
		if (!tl_is_resdom(tle))
		{
			Fjoin	   *fj = (Fjoin *) lfirst(tle);

			/* it is a FJoin */
			fjtl = lnext(tle);
			tle = fj->fj_innerNode;
		}
#endif
		i++;
	}
	if (len > 0)
	{
		ExecAssignResultType(commonstate,
							 origTupDesc);
	}
	else
		ExecAssignResultType(commonstate,
							 (TupleDesc) NULL);
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

/*	  ExecFreeTypeInfo(tupType); */
	pfree(tupType);
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
	projInfo->pi_tupValue =
		(len <= 0) ? NULL : (Datum *) palloc(sizeof(Datum) * len);
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

/*	  ExecFreeTypeInfo(tupType); */
	pfree(tupType);
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
 *		ExecMakeTypeInfo(noType) --
 *				returns pointer to array of 'noType' structure 'attribute'.
 *		ExecSetTypeInfo(index, typeInfo, attNum, attLen) --
 *				sets the element indexed by 'index' in typeInfo with
 *				the values: attNum, attLen.
 *		ExecFreeTypeInfo(typeInfo) --
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
	AttributeTupleForm att;

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
		StrNCpy(att->attname.data, attName, NAMEDATALEN);
	else
		MemSet(att->attname.data, 0, NAMEDATALEN);

	att->atttypid = typeID;
	att->attdefrel = 0;			/* dummy value */
	att->attdisbursion = 0;		/* dummy value */
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
 *		ExecGetIndexKeyInfo
 *
 *		Extracts the index key attribute numbers from
 *		an index tuple form (i.e. a tuple from the pg_index relation)
 *		into an array of attribute numbers.  The array and the
 *		size of the array are returned to the caller via return
 *		parameters.
 * ----------------------------------------------------------------
 */
static void
ExecGetIndexKeyInfo(IndexTupleForm indexTuple,
					int *numAttsOutP,
					AttrNumber **attsOutP,
					FuncIndexInfoPtr fInfoP)
{
	int			i;
	int			numKeys;
	AttrNumber *attKeys;

	/* ----------------
	 *	check parameters
	 * ----------------
	 */
	if (numAttsOutP == NULL && attsOutP == NULL)
	{
		elog(DEBUG, "ExecGetIndexKeyInfo: %s",
		"invalid parameters: numAttsOutP and attsOutP must be non-NULL");
	}

	/* ----------------
	 * set the procid for a possible functional index.
	 * ----------------
	 */
	FIsetProcOid(fInfoP, indexTuple->indproc);

	/* ----------------
	 *	count the number of keys..
	 * ----------------
	 */
	numKeys = 0;
	for (i = 0; i < 8 && indexTuple->indkey[i] != 0; i++)
		numKeys++;

	/* ----------------
	 *	place number keys in callers return area
	 *	or the number of arguments for a functional index.
	 *
	 *	If we have a functional index then the number of
	 *	attributes defined in the index must 1 (the function's
	 *	single return value).
	 * ----------------
	 */
	if (FIgetProcOid(fInfoP) != InvalidOid)
	{
		FIsetnArgs(fInfoP, numKeys);
		(*numAttsOutP) = 1;
	}
	else
		(*numAttsOutP) = numKeys;

	if (numKeys < 1)
	{
		elog(DEBUG, "ExecGetIndexKeyInfo: %s",
			 "all index key attribute numbers are zero!");
		(*attsOutP) = NULL;
		return;
	}

	/* ----------------
	 *	allocate and fill in array of key attribute numbers
	 * ----------------
	 */
	CXT1_printf("ExecGetIndexKeyInfo: context is %d\n", CurrentMemoryContext);

	attKeys = (AttrNumber *)
		palloc(numKeys * sizeof(AttrNumber));

	for (i = 0; i < numKeys; i++)
		attKeys[i] = indexTuple->indkey[i];

	/* ----------------
	 *	return array to caller.
	 * ----------------
	 */
	(*attsOutP) = attKeys;
}

/* ----------------------------------------------------------------
 *		ExecOpenIndices
 *
 *		Here we scan the pg_index relation to find indices
 *		associated with a given heap relation oid.	Since we
 *		don't know in advance how many indices we have, we
 *		form lists containing the information we need from
 *		pg_index and then process these lists.
 *
 *		Note: much of this code duplicates effort done by
 *		the IndexCatalogInformation function in plancat.c
 *		because IndexCatalogInformation is poorly written.
 *
 *		It would be much better the functionality provided
 *		by this function and IndexCatalogInformation was
 *		in the form of a small set of orthogonal routines..
 *		If you are trying to understand this, I suggest you
 *		look at the code to IndexCatalogInformation and
 *		FormIndexTuple.. -cim 9/27/89
 * ----------------------------------------------------------------
 */
void
ExecOpenIndices(Oid resultRelationOid,
				RelationInfo *resultRelationInfo)
{
	Relation	indexRd;
	HeapScanDesc indexSd;
	ScanKeyData key;
	HeapTuple	tuple;
	IndexTupleForm indexStruct;
	Oid			indexOid;
	List	   *oidList;
	List	   *nkeyList;
	List	   *keyList;
	List	   *fiList;
	char	   *predString;
	List	   *predList;
	List	   *indexoid;
	List	   *numkeys;
	List	   *indexkeys;
	List	   *indexfuncs;
	List	   *indexpreds;
	int			len;

	RelationPtr relationDescs;
	IndexInfo **indexInfoArray;
	FuncIndexInfoPtr fInfoP;
	int			numKeyAtts;
	AttrNumber *indexKeyAtts;
	PredInfo   *predicate;
	int			i;

	/* ----------------
	 *	open pg_index
	 * ----------------
	 */
	indexRd = heap_openr(IndexRelationName);

	/* ----------------
	 *	form a scan key
	 * ----------------
	 */
	ScanKeyEntryInitialize(&key, 0, Anum_pg_index_indrelid,
						   F_OIDEQ,
						   ObjectIdGetDatum(resultRelationOid));

	/* ----------------
	 *	scan the index relation, looking for indices for our
	 *	result relation..
	 * ----------------
	 */
	indexSd = heap_beginscan(indexRd,	/* scan desc */
							 false,		/* scan backward flag */
							 false,		/* see self */
							 1, /* number scan keys */
							 &key);		/* scan keys */

	oidList = NIL;
	nkeyList = NIL;
	keyList = NIL;
	fiList = NIL;
	predList = NIL;

	while (tuple = heap_getnext(indexSd,		/* scan desc */
								false,	/* scan backward flag */
								NULL),	/* return: buffer */
		   HeapTupleIsValid(tuple))
	{

		/* ----------------
		 *	For each index relation we find, extract the information
		 *	we need and store it in a list..
		 *
		 *	first get the oid of the index relation from the tuple
		 * ----------------
		 */
		indexStruct = (IndexTupleForm) GETSTRUCT(tuple);
		indexOid = indexStruct->indexrelid;

		/* ----------------
		 * allocate space for functional index information.
		 * ----------------
		 */
		fInfoP = (FuncIndexInfoPtr) palloc(sizeof(*fInfoP));

		/* ----------------
		 *	next get the index key information from the tuple
		 * ----------------
		 */
		ExecGetIndexKeyInfo(indexStruct,
							&numKeyAtts,
							&indexKeyAtts,
							fInfoP);

		/* ----------------
		 *	next get the index predicate from the tuple
		 * ----------------
		 */
		if (VARSIZE(&indexStruct->indpred) != 0)
		{
			predString = fmgr(F_TEXTOUT, &indexStruct->indpred);
			predicate = (PredInfo *) stringToNode(predString);
			pfree(predString);
		}
		else
			predicate = NULL;

		/* ----------------
		 *	save the index information into lists
		 * ----------------
		 */
		oidList = lconsi(indexOid, oidList);
		nkeyList = lconsi(numKeyAtts, nkeyList);
		keyList = lcons(indexKeyAtts, keyList);
		fiList = lcons(fInfoP, fiList);
		predList = lcons(predicate, predList);
	}

	/* ----------------
	 *	we have the info we need so close the pg_index relation..
	 * ----------------
	 */
	heap_endscan(indexSd);
	heap_close(indexRd);

	/* ----------------
	 *	Now that we've collected the index information into three
	 *	lists, we open the index relations and store the descriptors
	 *	and the key information into arrays.
	 * ----------------
	 */
	len = length(oidList);
	if (len > 0)
	{
		/* ----------------
		 *	 allocate space for relation descs
		 * ----------------
		 */
		CXT1_printf("ExecOpenIndices: context is %d\n", CurrentMemoryContext);
		relationDescs = (RelationPtr)
			palloc(len * sizeof(Relation));

		/* ----------------
		 *	 initialize index info array
		 * ----------------
		 */
		CXT1_printf("ExecOpenIndices: context is %d\n", CurrentMemoryContext);
		indexInfoArray = (IndexInfo **)
			palloc(len * sizeof(IndexInfo *));

		for (i = 0; i < len; i++)
		{
			IndexInfo  *ii = makeNode(IndexInfo);

			ii->ii_NumKeyAttributes = 0;
			ii->ii_KeyAttributeNumbers = (AttrNumber *) NULL;
			ii->ii_FuncIndexInfo = (FuncIndexInfoPtr) NULL;
			ii->ii_Predicate = NULL;
			indexInfoArray[i] = ii;
		}

		/* ----------------
		 *	 attempt to open each of the indices.  If we succeed,
		 *	 then store the index relation descriptor into the
		 *	 relation descriptor array.
		 * ----------------
		 */
		i = 0;
		foreach(indexoid, oidList)
		{
			Relation	indexDesc;

			indexOid = lfirsti(indexoid);
			indexDesc = index_open(indexOid);
			if (indexDesc != NULL)
				relationDescs[i++] = indexDesc;
		}

		/* ----------------
		 *	 store the relation descriptor array and number of
		 *	 descs into the result relation info.
		 * ----------------
		 */
		resultRelationInfo->ri_NumIndices = i;
		resultRelationInfo->ri_IndexRelationDescs = relationDescs;

		/* ----------------
		 *	 store the index key information collected in our
		 *	 lists into the index info array
		 * ----------------
		 */
		i = 0;
		foreach(numkeys, nkeyList)
		{
			numKeyAtts = lfirsti(numkeys);
			indexInfoArray[i++]->ii_NumKeyAttributes = numKeyAtts;
		}

		i = 0;
		foreach(indexkeys, keyList)
		{
			indexKeyAtts = (AttrNumber *) lfirst(indexkeys);
			indexInfoArray[i++]->ii_KeyAttributeNumbers = indexKeyAtts;
		}

		i = 0;
		foreach(indexfuncs, fiList)
		{
			FuncIndexInfoPtr fiP = (FuncIndexInfoPtr) lfirst(indexfuncs);

			indexInfoArray[i++]->ii_FuncIndexInfo = fiP;
		}

		i = 0;
		foreach(indexpreds, predList)
			indexInfoArray[i++]->ii_Predicate = lfirst(indexpreds);
		/* ----------------
		 *	 store the index info array into relation info
		 * ----------------
		 */
		resultRelationInfo->ri_IndexRelationInfo = indexInfoArray;
	}

	/* ----------------
	 *	All done,  resultRelationInfo now contains complete information
	 *	on the indices associated with the result relation.
	 * ----------------
	 */

	/* should free oidList, nkeyList and keyList here */
	/* OK - let's do it   -jolly */
	freeList(oidList);
	freeList(nkeyList);
	freeList(keyList);
	freeList(fiList);
	freeList(predList);
}

/* ----------------------------------------------------------------
 *		ExecCloseIndices
 *
 *		Close the index relations stored in resultRelationInfo
 * ----------------------------------------------------------------
 */
void
ExecCloseIndices(RelationInfo *resultRelationInfo)
{
	int			i;
	int			numIndices;
	RelationPtr relationDescs;

	numIndices = resultRelationInfo->ri_NumIndices;
	relationDescs = resultRelationInfo->ri_IndexRelationDescs;

	for (i = 0; i < numIndices; i++)
		if (relationDescs[i] != NULL)
			index_close(relationDescs[i]);

	/*
	 * XXX should free indexInfo array here too.
	 */
}

/* ----------------------------------------------------------------
 *		ExecFormIndexTuple
 *
 *		Most of this code is cannabilized from DefaultBuild().
 *		As said in the comments for ExecOpenIndices, most of
 *		this functionality should be rearranged into a proper
 *		set of routines..
 * ----------------------------------------------------------------
 */
#ifdef NOT_USED
IndexTuple
ExecFormIndexTuple(HeapTuple heapTuple,
				   Relation heapRelation,
				   Relation indexRelation,
				   IndexInfo *indexInfo)
{
	IndexTuple	indexTuple;
	TupleDesc	heapDescriptor;
	TupleDesc	indexDescriptor;
	Datum	   *datum;
	char	   *nulls;

	int			numberOfAttributes;
	AttrNumber *keyAttributeNumbers;
	FuncIndexInfoPtr fInfoP;

	/* ----------------
	 *	get information from index info structure
	 * ----------------
	 */
	numberOfAttributes = indexInfo->ii_NumKeyAttributes;
	keyAttributeNumbers = indexInfo->ii_KeyAttributeNumbers;
	fInfoP = indexInfo->ii_FuncIndexInfo;

	/* ----------------
	 *	datum and null are arrays in which we collect the index attributes
	 *	when forming a new index tuple.
	 * ----------------
	 */
	CXT1_printf("ExecFormIndexTuple: context is %d\n", CurrentMemoryContext);
	datum = (Datum *) palloc(numberOfAttributes * sizeof *datum);
	nulls = (char *) palloc(numberOfAttributes * sizeof *nulls);

	/* ----------------
	 *	get the tuple descriptors from the relations so we know
	 *	how to form the index tuples..
	 * ----------------
	 */
	heapDescriptor = RelationGetTupleDescriptor(heapRelation);
	indexDescriptor = RelationGetTupleDescriptor(indexRelation);

	/* ----------------
	 *	FormIndexDatum fills in its datum and null parameters
	 *	with attribute information taken from the given heap tuple.
	 * ----------------
	 */
	FormIndexDatum(numberOfAttributes,	/* num attributes */
				   keyAttributeNumbers, /* array of att nums to extract */
				   heapTuple,	/* tuple from base relation */
				   heapDescriptor,		/* heap tuple's descriptor */
				   InvalidBuffer,		/* buffer associated with heap
										 * tuple */
				   datum,		/* return: array of attributes */
				   nulls,		/* return: array of char's */
				   fInfoP);		/* functional index information */

	indexTuple = index_formtuple(indexDescriptor,
								 datum,
								 nulls);

	/* ----------------
	 *	free temporary arrays
	 *
	 *	XXX should store these in the IndexInfo instead of allocating
	 *	   and freeing on every insertion, but efficency here is not
	 *	   that important and FormIndexTuple is wasteful anyways..
	 *	   -cim 9/27/89
	 * ----------------
	 */
	pfree(nulls);
	pfree(datum);

	return indexTuple;
}

#endif

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
	RelationInfo *resultRelationInfo;
	int			i;
	int			numIndices;
	RelationPtr relationDescs;
	Relation	heapRelation;
	IndexInfo **indexInfoArray;
	IndexInfo  *indexInfo;
	Node	   *predicate;
	bool		satisfied;
	ExprContext *econtext;
	InsertIndexResult result;
	int			numberOfAttributes;
	AttrNumber *keyAttributeNumbers;
	FuncIndexInfoPtr fInfoP;
	TupleDesc	heapDescriptor;
	Datum	   *datum;
	char	   *nulls;

	heapTuple = slot->val;

	/* ----------------
	 *	get information from the result relation info structure.
	 * ----------------
	 */
	resultRelationInfo = estate->es_result_relation_info;
	numIndices = resultRelationInfo->ri_NumIndices;
	relationDescs = resultRelationInfo->ri_IndexRelationDescs;
	indexInfoArray = resultRelationInfo->ri_IndexRelationInfo;
	heapRelation = resultRelationInfo->ri_RelationDesc;

	/* ----------------
	 *	for each index, form and insert the index tuple
	 * ----------------
	 */
	econtext = NULL;
	for (i = 0; i < numIndices; i++)
	{
		if (relationDescs[i] == NULL)
			continue;

		indexInfo = indexInfoArray[i];
		predicate = indexInfo->ii_Predicate;
		if (predicate != NULL)
		{
			if (econtext == NULL)
				econtext = makeNode(ExprContext);
			econtext->ecxt_scantuple = slot;

			/* Skip this index-update if the predicate isn't satisfied */
			satisfied = ExecQual((List *) predicate, econtext);
			if (satisfied == false)
				continue;
		}

		/* ----------------
		 *		get information from index info structure
		 * ----------------
		 */
		numberOfAttributes = indexInfo->ii_NumKeyAttributes;
		keyAttributeNumbers = indexInfo->ii_KeyAttributeNumbers;
		fInfoP = indexInfo->ii_FuncIndexInfo;
		datum = (Datum *) palloc(numberOfAttributes * sizeof *datum);
		nulls = (char *) palloc(numberOfAttributes * sizeof *nulls);
		heapDescriptor = (TupleDesc) RelationGetTupleDescriptor(heapRelation);

		FormIndexDatum(numberOfAttributes,		/* num attributes */
					   keyAttributeNumbers,		/* array of att nums to
												 * extract */
					   heapTuple,		/* tuple from base relation */
					   heapDescriptor,	/* heap tuple's descriptor */
					   InvalidBuffer,	/* buffer associated with heap
										 * tuple */
					   datum,	/* return: array of attributes */
					   nulls,	/* return: array of char's */
					   fInfoP); /* functional index information */


		result = index_insert(relationDescs[i], /* index relation */
							  datum,	/* array of heaptuple Datums */
							  nulls,	/* info on nulls */
							  &(heapTuple->t_ctid),		/* oid of heap tuple */
							  heapRelation);

		/* ----------------
		 *		keep track of index inserts for debugging
		 * ----------------
		 */
		IncrIndexInserted();

		/* ----------------
		 *		free index tuple after insertion
		 * ----------------
		 */
		if (result)
			pfree(result);
	}
	if (econtext != NULL)
		pfree(econtext);
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
