/*-------------------------------------------------------------------------
 *
 * execMain.c
 *	  top level executor interface routines
 *
 * INTERFACE ROUTINES
 *	ExecutorStart()
 *	ExecutorRun()
 *	ExecutorEnd()
 *
 *	The old ExecutorMain() has been replaced by ExecutorStart(),
 *	ExecutorRun() and ExecutorEnd()
 *
 *	These three procedures are the external interfaces to the executor.
 *	In each case, the query descriptor and the execution state is required
 *	 as arguments
 *
 *	ExecutorStart() must be called at the beginning of any execution of any
 *	query plan and ExecutorEnd() should always be called at the end of
 *	execution of a plan.
 *
 *	ExecutorRun accepts direction and count arguments that specify whether
 *	the plan is to be executed forwards, backwards, and for how many tuples.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execMain.c,v 1.180.2.1 2003/01/23 05:10:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "executor/execdebug.h"
#include "executor/execdefs.h"
#include "miscadmin.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"


/* decls for local routines only used within this module */
static TupleDesc InitPlan(CmdType operation,
		 Query *parseTree,
		 Plan *plan,
		 EState *estate);
static void initResultRelInfo(ResultRelInfo *resultRelInfo,
				  Index resultRelationIndex,
				  List *rangeTable,
				  CmdType operation);
static void EndPlan(Plan *plan, EState *estate);
static TupleTableSlot *ExecutePlan(EState *estate, Plan *plan,
			CmdType operation,
			long numberTuples,
			ScanDirection direction,
			DestReceiver *destfunc);
static void ExecSelect(TupleTableSlot *slot,
		   DestReceiver *destfunc,
		   EState *estate);
static void ExecInsert(TupleTableSlot *slot, ItemPointer tupleid,
		   EState *estate);
static void ExecDelete(TupleTableSlot *slot, ItemPointer tupleid,
		   EState *estate);
static void ExecUpdate(TupleTableSlot *slot, ItemPointer tupleid,
		   EState *estate);
static TupleTableSlot *EvalPlanQualNext(EState *estate);
static void EndEvalPlanQual(EState *estate);
static void ExecCheckQueryPerms(CmdType operation, Query *parseTree,
					Plan *plan);
static void ExecCheckPlanPerms(Plan *plan, List *rangeTable,
				   CmdType operation);
static void ExecCheckRTPerms(List *rangeTable, CmdType operation);
static void ExecCheckRTEPerms(RangeTblEntry *rte, CmdType operation);

/* end of local decls */


/* ----------------------------------------------------------------
 *		ExecutorStart
 *
 *		This routine must be called at the beginning of any execution of any
 *		query plan
 *
 *		returns a TupleDesc which describes the attributes of the tuples to
 *		be returned by the query.  (Same value is saved in queryDesc)
 *
 * NB: the CurrentMemoryContext when this is called must be the context
 * to be used as the per-query context for the query plan.	ExecutorRun()
 * and ExecutorEnd() must be called in this same memory context.
 * ----------------------------------------------------------------
 */
TupleDesc
ExecutorStart(QueryDesc *queryDesc, EState *estate)
{
	TupleDesc	result;

	/* sanity checks */
	Assert(queryDesc != NULL);

	if (queryDesc->plantree->nParamExec > 0)
	{
		estate->es_param_exec_vals = (ParamExecData *)
			palloc(queryDesc->plantree->nParamExec * sizeof(ParamExecData));
		MemSet(estate->es_param_exec_vals, 0,
			   queryDesc->plantree->nParamExec * sizeof(ParamExecData));
	}

	/*
	 * Make our own private copy of the current query snapshot data.
	 *
	 * This "freezes" our idea of which tuples are good and which are not for
	 * the life of this query, even if it outlives the current command and
	 * current snapshot.
	 */
	estate->es_snapshot = CopyQuerySnapshot();

	/*
	 * Initialize the plan
	 */
	result = InitPlan(queryDesc->operation,
					  queryDesc->parsetree,
					  queryDesc->plantree,
					  estate);

	queryDesc->tupDesc = result;

	return result;
}

/* ----------------------------------------------------------------
 *		ExecutorRun
 *
 *		This is the main routine of the executor module. It accepts
 *		the query descriptor from the traffic cop and executes the
 *		query plan.
 *
 *		ExecutorStart must have been called already.
 *
 *		If direction is NoMovementScanDirection then nothing is done
 *		except to start up/shut down the destination.  Otherwise,
 *		we retrieve up to 'count' tuples in the specified direction.
 *
 *		Note: count = 0 is interpreted as no portal limit, e.g. run to
 *		completion.
 *
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecutorRun(QueryDesc *queryDesc, EState *estate,
			ScanDirection direction, long count)
{
	CmdType		operation;
	Plan	   *plan;
	CommandDest dest;
	DestReceiver *destfunc;
	TupleTableSlot *result;

	/*
	 * sanity checks
	 */
	Assert(queryDesc != NULL);

	/*
	 * extract information from the query descriptor and the query
	 * feature.
	 */
	operation = queryDesc->operation;
	plan = queryDesc->plantree;
	dest = queryDesc->dest;

	/*
	 * startup tuple receiver
	 */
	estate->es_processed = 0;
	estate->es_lastoid = InvalidOid;

	destfunc = DestToFunction(dest);
	(*destfunc->setup) (destfunc, (int) operation,
						queryDesc->portalName, queryDesc->tupDesc);

	/*
	 * run plan
	 */
	if (direction == NoMovementScanDirection)
		result = NULL;
	else
		result = ExecutePlan(estate,
							 plan,
							 operation,
							 count,
							 direction,
							 destfunc);

	/*
	 * shutdown receiver
	 */
	(*destfunc->cleanup) (destfunc);

	return result;
}

/* ----------------------------------------------------------------
 *		ExecutorEnd
 *
 *		This routine must be called at the end of execution of any
 *		query plan
 * ----------------------------------------------------------------
 */
void
ExecutorEnd(QueryDesc *queryDesc, EState *estate)
{
	/* sanity checks */
	Assert(queryDesc != NULL);

	EndPlan(queryDesc->plantree, estate);

	if (estate->es_snapshot != NULL)
	{
		if (estate->es_snapshot->xcnt > 0)
			pfree(estate->es_snapshot->xip);
		pfree(estate->es_snapshot);
		estate->es_snapshot = NULL;
	}

	if (estate->es_param_exec_vals != NULL)
	{
		pfree(estate->es_param_exec_vals);
		estate->es_param_exec_vals = NULL;
	}
}


/*
 * ExecCheckQueryPerms
 *		Check access permissions for all relations referenced in a query.
 */
static void
ExecCheckQueryPerms(CmdType operation, Query *parseTree, Plan *plan)
{
	/*
	 * Check RTEs in the query's primary rangetable.
	 */
	ExecCheckRTPerms(parseTree->rtable, operation);

	/*
	 * Search for subplans and APPEND nodes to check their rangetables.
	 */
	ExecCheckPlanPerms(plan, parseTree->rtable, operation);
}

/*
 * ExecCheckPlanPerms
 *		Recursively scan the plan tree to check access permissions in
 *		subplans.
 */
static void
ExecCheckPlanPerms(Plan *plan, List *rangeTable, CmdType operation)
{
	List	   *subp;

	if (plan == NULL)
		return;

	/* Check subplans, which we assume are plain SELECT queries */

	foreach(subp, plan->initPlan)
	{
		SubPlan    *subplan = (SubPlan *) lfirst(subp);

		ExecCheckRTPerms(subplan->rtable, CMD_SELECT);
		ExecCheckPlanPerms(subplan->plan, subplan->rtable, CMD_SELECT);
	}
	foreach(subp, plan->subPlan)
	{
		SubPlan    *subplan = (SubPlan *) lfirst(subp);

		ExecCheckRTPerms(subplan->rtable, CMD_SELECT);
		ExecCheckPlanPerms(subplan->plan, subplan->rtable, CMD_SELECT);
	}

	/* Check lower plan nodes */

	ExecCheckPlanPerms(plan->lefttree, rangeTable, operation);
	ExecCheckPlanPerms(plan->righttree, rangeTable, operation);

	/* Do node-type-specific checks */

	switch (nodeTag(plan))
	{
		case T_SubqueryScan:
			{
				SubqueryScan *scan = (SubqueryScan *) plan;
				RangeTblEntry *rte;

				/* Recursively check the subquery */
				rte = rt_fetch(scan->scan.scanrelid, rangeTable);
				Assert(rte->rtekind == RTE_SUBQUERY);
				ExecCheckQueryPerms(operation, rte->subquery, scan->subplan);
				break;
			}
		case T_Append:
			{
				Append	   *app = (Append *) plan;
				List	   *appendplans;

				foreach(appendplans, app->appendplans)
				{
					ExecCheckPlanPerms((Plan *) lfirst(appendplans),
									   rangeTable,
									   operation);
				}
				break;
			}

		default:
			break;
	}
}

/*
 * ExecCheckRTPerms
 *		Check access permissions for all relations listed in a range table.
 */
static void
ExecCheckRTPerms(List *rangeTable, CmdType operation)
{
	List	   *lp;

	foreach(lp, rangeTable)
	{
		RangeTblEntry *rte = lfirst(lp);

		ExecCheckRTEPerms(rte, operation);
	}
}

/*
 * ExecCheckRTEPerms
 *		Check access permissions for a single RTE.
 */
static void
ExecCheckRTEPerms(RangeTblEntry *rte, CmdType operation)
{
	Oid			relOid;
	Oid			userid;
	AclResult	aclcheck_result;

	/*
	 * Only plain-relation RTEs need to be checked here.  Subquery RTEs
	 * will be checked when ExecCheckPlanPerms finds the SubqueryScan
	 * node, and function RTEs are checked by init_fcache when the
	 * function is prepared for execution.	Join and special RTEs need no
	 * checks.
	 */
	if (rte->rtekind != RTE_RELATION)
		return;

	relOid = rte->relid;

	/*
	 * userid to check as: current user unless we have a setuid
	 * indication.
	 *
	 * Note: GetUserId() is presently fast enough that there's no harm in
	 * calling it separately for each RTE.	If that stops being true, we
	 * could call it once in ExecCheckQueryPerms and pass the userid down
	 * from there.	But for now, no need for the extra clutter.
	 */
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

#define CHECK(MODE)		pg_class_aclcheck(relOid, userid, MODE)

	if (rte->checkForRead)
	{
		aclcheck_result = CHECK(ACL_SELECT);
		if (aclcheck_result != ACLCHECK_OK)
			aclcheck_error(aclcheck_result, get_rel_name(relOid));
	}

	if (rte->checkForWrite)
	{
		/*
		 * Note: write access in a SELECT context means SELECT FOR UPDATE.
		 * Right now we don't distinguish that from true update as far as
		 * permissions checks are concerned.
		 */
		switch (operation)
		{
			case CMD_INSERT:
				aclcheck_result = CHECK(ACL_INSERT);
				break;
			case CMD_SELECT:
			case CMD_UPDATE:
				aclcheck_result = CHECK(ACL_UPDATE);
				break;
			case CMD_DELETE:
				aclcheck_result = CHECK(ACL_DELETE);
				break;
			default:
				elog(ERROR, "ExecCheckRTEPerms: bogus operation %d",
					 operation);
				aclcheck_result = ACLCHECK_OK;	/* keep compiler quiet */
				break;
		}
		if (aclcheck_result != ACLCHECK_OK)
			aclcheck_error(aclcheck_result, get_rel_name(relOid));
	}
}


/* ===============================================================
 * ===============================================================
						 static routines follow
 * ===============================================================
 * ===============================================================
 */

typedef struct execRowMark
{
	Relation	relation;
	Index		rti;
	char		resname[32];
} execRowMark;

typedef struct evalPlanQual
{
	Plan	   *plan;
	Index		rti;
	EState		estate;
	struct evalPlanQual *free;
} evalPlanQual;

/* ----------------------------------------------------------------
 *		InitPlan
 *
 *		Initializes the query plan: open files, allocate storage
 *		and start up the rule manager
 * ----------------------------------------------------------------
 */
static TupleDesc
InitPlan(CmdType operation, Query *parseTree, Plan *plan, EState *estate)
{
	List	   *rangeTable;
	Relation	intoRelationDesc;
	bool		do_select_into;
	TupleDesc	tupType;

	/*
	 * Do permissions checks.
	 */
	ExecCheckQueryPerms(operation, parseTree, plan);

	/*
	 * get information from query descriptor
	 */
	rangeTable = parseTree->rtable;

	/*
	 * initialize the node's execution state
	 */
	estate->es_range_table = rangeTable;

	/*
	 * if there is a result relation, initialize result relation stuff
	 */
	if (parseTree->resultRelation != 0 && operation != CMD_SELECT)
	{
		List	   *resultRelations = parseTree->resultRelations;
		int			numResultRelations;
		ResultRelInfo *resultRelInfos;

		if (resultRelations != NIL)
		{
			/*
			 * Multiple result relations (due to inheritance)
			 * parseTree->resultRelations identifies them all
			 */
			ResultRelInfo *resultRelInfo;

			numResultRelations = length(resultRelations);
			resultRelInfos = (ResultRelInfo *)
				palloc(numResultRelations * sizeof(ResultRelInfo));
			resultRelInfo = resultRelInfos;
			while (resultRelations != NIL)
			{
				initResultRelInfo(resultRelInfo,
								  lfirsti(resultRelations),
								  rangeTable,
								  operation);
				resultRelInfo++;
				resultRelations = lnext(resultRelations);
			}
		}
		else
		{
			/*
			 * Single result relation identified by
			 * parseTree->resultRelation
			 */
			numResultRelations = 1;
			resultRelInfos = (ResultRelInfo *) palloc(sizeof(ResultRelInfo));
			initResultRelInfo(resultRelInfos,
							  parseTree->resultRelation,
							  rangeTable,
							  operation);
		}

		estate->es_result_relations = resultRelInfos;
		estate->es_num_result_relations = numResultRelations;
		/* Initialize to first or only result rel */
		estate->es_result_relation_info = resultRelInfos;
	}
	else
	{
		/*
		 * if no result relation, then set state appropriately
		 */
		estate->es_result_relations = NULL;
		estate->es_num_result_relations = 0;
		estate->es_result_relation_info = NULL;
	}

	/*
	 * Detect whether we're doing SELECT INTO.  If so, set the force_oids
	 * flag appropriately so that the plan tree will be initialized with
	 * the correct tuple descriptors.
	 */
	do_select_into = false;

	if (operation == CMD_SELECT &&
		!parseTree->isPortal &&
		parseTree->into != NULL)
	{
		do_select_into = true;
		/*
		 * For now, always create OIDs in SELECT INTO; this is for backwards
		 * compatibility with pre-7.3 behavior.  Eventually we might want
		 * to allow the user to choose.
		 */
		estate->es_force_oids = true;
	}

	/*
	 * Have to lock relations selected for update
	 */
	estate->es_rowMark = NIL;
	if (parseTree->rowMarks != NIL)
	{
		List	   *l;

		foreach(l, parseTree->rowMarks)
		{
			Index		rti = lfirsti(l);
			Oid			relid = getrelid(rti, rangeTable);
			Relation	relation;
			execRowMark *erm;

			relation = heap_open(relid, RowShareLock);
			erm = (execRowMark *) palloc(sizeof(execRowMark));
			erm->relation = relation;
			erm->rti = rti;
			snprintf(erm->resname, 32, "ctid%u", rti);
			estate->es_rowMark = lappend(estate->es_rowMark, erm);
		}
	}

	/*
	 * initialize the executor "tuple" table.  We need slots for all the
	 * plan nodes, plus possibly output slots for the junkfilter(s). At
	 * this point we aren't sure if we need junkfilters, so just add slots
	 * for them unconditionally.
	 */
	{
		int			nSlots = ExecCountSlotsNode(plan);

		if (parseTree->resultRelations != NIL)
			nSlots += length(parseTree->resultRelations);
		else
			nSlots += 1;
		estate->es_tupleTable = ExecCreateTupleTable(nSlots);
	}

	/* mark EvalPlanQual not active */
	estate->es_origPlan = plan;
	estate->es_evalPlanQual = NULL;
	estate->es_evTuple = NULL;
	estate->es_evTupleNull = NULL;
	estate->es_useEvalPlan = false;

	/*
	 * initialize the private state information for all the nodes in the
	 * query tree.	This opens files, allocates storage and leaves us
	 * ready to start processing tuples.
	 */
	ExecInitNode(plan, estate, NULL);

	/*
	 * Get the tuple descriptor describing the type of tuples to return.
	 * (this is especially important if we are creating a relation with
	 * "SELECT INTO")
	 */
	tupType = ExecGetTupType(plan);		/* tuple descriptor */

	/*
	 * Initialize the junk filter if needed. SELECT and INSERT queries
	 * need a filter if there are any junk attrs in the tlist.	UPDATE and
	 * DELETE always need one, since there's always a junk 'ctid'
	 * attribute present --- no need to look first.
	 */
	{
		bool		junk_filter_needed = false;
		List	   *tlist;

		switch (operation)
		{
			case CMD_SELECT:
			case CMD_INSERT:
				foreach(tlist, plan->targetlist)
				{
					TargetEntry *tle = (TargetEntry *) lfirst(tlist);

					if (tle->resdom->resjunk)
					{
						junk_filter_needed = true;
						break;
					}
				}
				break;
			case CMD_UPDATE:
			case CMD_DELETE:
				junk_filter_needed = true;
				break;
			default:
				break;
		}

		if (junk_filter_needed)
		{
			/*
			 * If there are multiple result relations, each one needs its
			 * own junk filter.  Note this is only possible for
			 * UPDATE/DELETE, so we can't be fooled by some needing a
			 * filter and some not.
			 */
			if (parseTree->resultRelations != NIL)
			{
				List	   *subplans;
				ResultRelInfo *resultRelInfo;

				/* Top plan had better be an Append here. */
				Assert(IsA(plan, Append));
				Assert(((Append *) plan)->isTarget);
				subplans = ((Append *) plan)->appendplans;
				Assert(length(subplans) == estate->es_num_result_relations);
				resultRelInfo = estate->es_result_relations;
				while (subplans != NIL)
				{
					Plan	   *subplan = (Plan *) lfirst(subplans);
					JunkFilter *j;

					j = ExecInitJunkFilter(subplan->targetlist,
										   ExecGetTupType(subplan),
							  ExecAllocTableSlot(estate->es_tupleTable));
					resultRelInfo->ri_junkFilter = j;
					resultRelInfo++;
					subplans = lnext(subplans);
				}

				/*
				 * Set active junkfilter too; at this point ExecInitAppend
				 * has already selected an active result relation...
				 */
				estate->es_junkFilter =
					estate->es_result_relation_info->ri_junkFilter;
			}
			else
			{
				/* Normal case with just one JunkFilter */
				JunkFilter *j;

				j = ExecInitJunkFilter(plan->targetlist,
									   tupType,
							  ExecAllocTableSlot(estate->es_tupleTable));
				estate->es_junkFilter = j;
				if (estate->es_result_relation_info)
					estate->es_result_relation_info->ri_junkFilter = j;

				/* For SELECT, want to return the cleaned tuple type */
				if (operation == CMD_SELECT)
					tupType = j->jf_cleanTupType;
			}
		}
		else
			estate->es_junkFilter = NULL;
	}

	/*
	 * initialize the "into" relation
	 */
	intoRelationDesc = (Relation) NULL;

	if (do_select_into)
	{
				char	   *intoName;
				Oid			namespaceId;
				AclResult	aclresult;
				Oid			intoRelationId;
				TupleDesc	tupdesc;

				/*
				 * find namespace to create in, check permissions
				 */
				intoName = parseTree->into->relname;
				namespaceId = RangeVarGetCreationNamespace(parseTree->into);

				aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(),
												  ACL_CREATE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult,
								   get_namespace_name(namespaceId));

				/*
				 * have to copy tupType to get rid of constraints
				 */
				tupdesc = CreateTupleDescCopy(tupType);

				intoRelationId =
					heap_create_with_catalog(intoName,
											 namespaceId,
											 tupdesc,
											 RELKIND_RELATION,
											 false,
											 allowSystemTableMods);

				FreeTupleDesc(tupdesc);

				/*
				 * Advance command counter so that the newly-created
				 * relation's catalog tuples will be visible to heap_open.
				 */
				CommandCounterIncrement();

				/*
				 * If necessary, create a TOAST table for the into
				 * relation. Note that AlterTableCreateToastTable ends
				 * with CommandCounterIncrement(), so that the TOAST table
				 * will be visible for insertion.
				 */
				AlterTableCreateToastTable(intoRelationId, true);

				intoRelationDesc = heap_open(intoRelationId,
											 AccessExclusiveLock);
	}

	estate->es_into_relation_descriptor = intoRelationDesc;

	return tupType;
}

/*
 * Initialize ResultRelInfo data for one result relation
 */
static void
initResultRelInfo(ResultRelInfo *resultRelInfo,
				  Index resultRelationIndex,
				  List *rangeTable,
				  CmdType operation)
{
	Oid			resultRelationOid;
	Relation	resultRelationDesc;

	resultRelationOid = getrelid(resultRelationIndex, rangeTable);
	resultRelationDesc = heap_open(resultRelationOid, RowExclusiveLock);

	switch (resultRelationDesc->rd_rel->relkind)
	{
		case RELKIND_SEQUENCE:
			elog(ERROR, "You can't change sequence relation %s",
				 RelationGetRelationName(resultRelationDesc));
			break;
		case RELKIND_TOASTVALUE:
			elog(ERROR, "You can't change toast relation %s",
				 RelationGetRelationName(resultRelationDesc));
			break;
		case RELKIND_VIEW:
			elog(ERROR, "You can't change view relation %s",
				 RelationGetRelationName(resultRelationDesc));
			break;
	}

	MemSet(resultRelInfo, 0, sizeof(ResultRelInfo));
	resultRelInfo->type = T_ResultRelInfo;
	resultRelInfo->ri_RangeTableIndex = resultRelationIndex;
	resultRelInfo->ri_RelationDesc = resultRelationDesc;
	resultRelInfo->ri_NumIndices = 0;
	resultRelInfo->ri_IndexRelationDescs = NULL;
	resultRelInfo->ri_IndexRelationInfo = NULL;
	/* make a copy so as not to depend on relcache info not changing... */
	resultRelInfo->ri_TrigDesc = CopyTriggerDesc(resultRelationDesc->trigdesc);
	resultRelInfo->ri_TrigFunctions = NULL;
	resultRelInfo->ri_ConstraintExprs = NULL;
	resultRelInfo->ri_junkFilter = NULL;

	/*
	 * If there are indices on the result relation, open them and save
	 * descriptors in the result relation info, so that we can add new
	 * index entries for the tuples we add/update.	We need not do this
	 * for a DELETE, however, since deletion doesn't affect indexes.
	 */
	if (resultRelationDesc->rd_rel->relhasindex &&
		operation != CMD_DELETE)
		ExecOpenIndices(resultRelInfo);
}

/* ----------------------------------------------------------------
 *		EndPlan
 *
 *		Cleans up the query plan -- closes files and free up storages
 * ----------------------------------------------------------------
 */
static void
EndPlan(Plan *plan, EState *estate)
{
	ResultRelInfo *resultRelInfo;
	int			i;
	List	   *l;

	/*
	 * shut down any PlanQual processing we were doing
	 */
	if (estate->es_evalPlanQual != NULL)
		EndEvalPlanQual(estate);

	/*
	 * shut down the node-type-specific query processing
	 */
	ExecEndNode(plan, NULL);

	/*
	 * destroy the executor "tuple" table.
	 */
	ExecDropTupleTable(estate->es_tupleTable, true);
	estate->es_tupleTable = NULL;

	/*
	 * close the result relation(s) if any, but hold locks until xact
	 * commit.	Also clean up junkfilters if present.
	 */
	resultRelInfo = estate->es_result_relations;
	for (i = estate->es_num_result_relations; i > 0; i--)
	{
		/* Close indices and then the relation itself */
		ExecCloseIndices(resultRelInfo);
		heap_close(resultRelInfo->ri_RelationDesc, NoLock);
		/* Delete the junkfilter if any */
		if (resultRelInfo->ri_junkFilter != NULL)
			ExecFreeJunkFilter(resultRelInfo->ri_junkFilter);
		resultRelInfo++;
	}

	/*
	 * close the "into" relation if necessary, again keeping lock
	 */
	if (estate->es_into_relation_descriptor != NULL)
		heap_close(estate->es_into_relation_descriptor, NoLock);

	/*
	 * There might be a junkfilter without a result relation.
	 */
	if (estate->es_num_result_relations == 0 &&
		estate->es_junkFilter != NULL)
	{
		ExecFreeJunkFilter(estate->es_junkFilter);
		estate->es_junkFilter = NULL;
	}

	/*
	 * close any relations selected FOR UPDATE, again keeping locks
	 */
	foreach(l, estate->es_rowMark)
	{
		execRowMark *erm = lfirst(l);

		heap_close(erm->relation, NoLock);
	}
}

/* ----------------------------------------------------------------
 *		ExecutePlan
 *
 *		processes the query plan to retrieve 'numberTuples' tuples in the
 *		direction specified.
 *		Retrieves all tuples if numberTuples is 0
 *
 *		result is either a slot containing the last tuple in the case
 *		of a SELECT or NULL otherwise.
 *
 * Note: the ctid attribute is a 'junk' attribute that is removed before the
 * user can see it
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecutePlan(EState *estate,
			Plan *plan,
			CmdType operation,
			long numberTuples,
			ScanDirection direction,
			DestReceiver *destfunc)
{
	JunkFilter *junkfilter;
	TupleTableSlot *slot;
	ItemPointer tupleid = NULL;
	ItemPointerData tuple_ctid;
	long		current_tuple_count;
	TupleTableSlot *result;

	/*
	 * initialize local variables
	 */
	slot = NULL;
	current_tuple_count = 0;
	result = NULL;

	/*
	 * Set the direction.
	 */
	estate->es_direction = direction;

	/*
	 * Loop until we've processed the proper number of tuples from the
	 * plan.
	 */

	for (;;)
	{
		/* Reset the per-output-tuple exprcontext */
		ResetPerTupleExprContext(estate);

		/*
		 * Execute the plan and obtain a tuple
		 */
lnext:	;
		if (estate->es_useEvalPlan)
		{
			slot = EvalPlanQualNext(estate);
			if (TupIsNull(slot))
				slot = ExecProcNode(plan, NULL);
		}
		else
			slot = ExecProcNode(plan, NULL);

		/*
		 * if the tuple is null, then we assume there is nothing more to
		 * process so we just return null...
		 */
		if (TupIsNull(slot))
		{
			result = NULL;
			break;
		}

		/*
		 * if we have a junk filter, then project a new tuple with the
		 * junk removed.
		 *
		 * Store this new "clean" tuple in the junkfilter's resultSlot.
		 * (Formerly, we stored it back over the "dirty" tuple, which is
		 * WRONG because that tuple slot has the wrong descriptor.)
		 *
		 * Also, extract all the junk information we need.
		 */
		if ((junkfilter = estate->es_junkFilter) != (JunkFilter *) NULL)
		{
			Datum		datum;
			HeapTuple	newTuple;
			bool		isNull;

			/*
			 * extract the 'ctid' junk attribute.
			 */
			if (operation == CMD_UPDATE || operation == CMD_DELETE)
			{
				if (!ExecGetJunkAttribute(junkfilter,
										  slot,
										  "ctid",
										  &datum,
										  &isNull))
					elog(ERROR, "ExecutePlan: NO (junk) `ctid' was found!");

				/* shouldn't ever get a null result... */
				if (isNull)
					elog(ERROR, "ExecutePlan: (junk) `ctid' is NULL!");

				tupleid = (ItemPointer) DatumGetPointer(datum);
				tuple_ctid = *tupleid;	/* make sure we don't free the
										 * ctid!! */
				tupleid = &tuple_ctid;
			}
			else if (estate->es_rowMark != NIL)
			{
				List	   *l;

		lmark:	;
				foreach(l, estate->es_rowMark)
				{
					execRowMark *erm = lfirst(l);
					Buffer		buffer;
					HeapTupleData tuple;
					TupleTableSlot *newSlot;
					int			test;

					if (!ExecGetJunkAttribute(junkfilter,
											  slot,
											  erm->resname,
											  &datum,
											  &isNull))
						elog(ERROR, "ExecutePlan: NO (junk) `%s' was found!",
							 erm->resname);

					/* shouldn't ever get a null result... */
					if (isNull)
						elog(ERROR, "ExecutePlan: (junk) `%s' is NULL!",
							 erm->resname);

					tuple.t_self = *((ItemPointer) DatumGetPointer(datum));
					test = heap_mark4update(erm->relation, &tuple, &buffer,
											estate->es_snapshot->curcid);
					ReleaseBuffer(buffer);
					switch (test)
					{
						case HeapTupleSelfUpdated:
							/* treat it as deleted; do not process */
							goto lnext;

						case HeapTupleMayBeUpdated:
							break;

						case HeapTupleUpdated:
							if (XactIsoLevel == XACT_SERIALIZABLE)
								elog(ERROR, "Can't serialize access due to concurrent update");
							if (!(ItemPointerEquals(&(tuple.t_self),
								  (ItemPointer) DatumGetPointer(datum))))
							{
								newSlot = EvalPlanQual(estate, erm->rti, &(tuple.t_self));
								if (!(TupIsNull(newSlot)))
								{
									slot = newSlot;
									estate->es_useEvalPlan = true;
									goto lmark;
								}
							}

							/*
							 * if tuple was deleted or PlanQual failed for
							 * updated tuple - we must not return this
							 * tuple!
							 */
							goto lnext;

						default:
							elog(ERROR, "Unknown status %u from heap_mark4update", test);
							return (NULL);
					}
				}
			}

			/*
			 * Finally create a new "clean" tuple with all junk attributes
			 * removed
			 */
			newTuple = ExecRemoveJunk(junkfilter, slot);

			slot = ExecStoreTuple(newTuple,		/* tuple to store */
								  junkfilter->jf_resultSlot,	/* dest slot */
								  InvalidBuffer,		/* this tuple has no
														 * buffer */
								  true);		/* tuple should be pfreed */
		}

		/*
		 * now that we have a tuple, do the appropriate thing with it..
		 * either return it to the user, add it to a relation someplace,
		 * delete it from a relation, or modify some of its attributes.
		 */
		switch (operation)
		{
			case CMD_SELECT:
				ExecSelect(slot,	/* slot containing tuple */
						   destfunc,	/* destination's tuple-receiver
										 * obj */
						   estate);
				result = slot;
				break;

			case CMD_INSERT:
				ExecInsert(slot, tupleid, estate);
				result = NULL;
				break;

			case CMD_DELETE:
				ExecDelete(slot, tupleid, estate);
				result = NULL;
				break;

			case CMD_UPDATE:
				ExecUpdate(slot, tupleid, estate);
				result = NULL;
				break;

			default:
				elog(LOG, "ExecutePlan: unknown operation in queryDesc");
				result = NULL;
				break;
		}

		/*
		 * check our tuple count.. if we've processed the proper number
		 * then quit, else loop again and process more tuples..
		 */
		current_tuple_count++;
		if (numberTuples == current_tuple_count)
			break;
	}

	/*
	 * here, result is either a slot containing a tuple in the case of a
	 * SELECT or NULL otherwise.
	 */
	return result;
}

/* ----------------------------------------------------------------
 *		ExecSelect
 *
 *		SELECTs are easy.. we just pass the tuple to the appropriate
 *		print function.  The only complexity is when we do a
 *		"SELECT INTO", in which case we insert the tuple into
 *		the appropriate relation (note: this is a newly created relation
 *		so we don't need to worry about indices or locks.)
 * ----------------------------------------------------------------
 */
static void
ExecSelect(TupleTableSlot *slot,
		   DestReceiver *destfunc,
		   EState *estate)
{
	HeapTuple	tuple;
	TupleDesc	attrtype;

	/*
	 * get the heap tuple out of the tuple table slot
	 */
	tuple = slot->val;
	attrtype = slot->ttc_tupleDescriptor;

	/*
	 * insert the tuple into the "into relation"
	 */
	if (estate->es_into_relation_descriptor != NULL)
	{
		heap_insert(estate->es_into_relation_descriptor, tuple,
					estate->es_snapshot->curcid);
		IncrAppended();
	}

	/*
	 * send the tuple to the front end (or the screen)
	 */
	(*destfunc->receiveTuple) (tuple, attrtype, destfunc);
	IncrRetrieved();
	(estate->es_processed)++;
}

/* ----------------------------------------------------------------
 *		ExecInsert
 *
 *		INSERTs are trickier.. we have to insert the tuple into
 *		the base relation and insert appropriate tuples into the
 *		index relations.
 * ----------------------------------------------------------------
 */
static void
ExecInsert(TupleTableSlot *slot,
		   ItemPointer tupleid,
		   EState *estate)
{
	HeapTuple	tuple;
	ResultRelInfo *resultRelInfo;
	Relation	resultRelationDesc;
	int			numIndices;
	Oid			newId;

	/*
	 * get the heap tuple out of the tuple table slot
	 */
	tuple = slot->val;

	/*
	 * get information on the (current) result relation
	 */
	resultRelInfo = estate->es_result_relation_info;
	resultRelationDesc = resultRelInfo->ri_RelationDesc;

	/* BEFORE ROW INSERT Triggers */
	if (resultRelInfo->ri_TrigDesc &&
	  resultRelInfo->ri_TrigDesc->n_before_row[TRIGGER_EVENT_INSERT] > 0)
	{
		HeapTuple	newtuple;

		newtuple = ExecBRInsertTriggers(estate, resultRelInfo, tuple);

		if (newtuple == NULL)	/* "do nothing" */
			return;

		if (newtuple != tuple)	/* modified by Trigger(s) */
		{
			/*
			 * Insert modified tuple into tuple table slot, replacing the
			 * original.  We assume that it was allocated in per-tuple
			 * memory context, and therefore will go away by itself. The
			 * tuple table slot should not try to clear it.
			 */
			ExecStoreTuple(newtuple, slot, InvalidBuffer, false);
			tuple = newtuple;
		}
	}

	/*
	 * Check the constraints of the tuple
	 */
	if (resultRelationDesc->rd_att->constr)
		ExecConstraints("ExecInsert", resultRelInfo, slot, estate);

	/*
	 * insert the tuple
	 */
	newId = heap_insert(resultRelationDesc, tuple,
						estate->es_snapshot->curcid);

	IncrAppended();
	(estate->es_processed)++;
	estate->es_lastoid = newId;
	setLastTid(&(tuple->t_self));

	/*
	 * process indices
	 *
	 * Note: heap_insert adds a new tuple to a relation.  As a side effect,
	 * the tupleid of the new tuple is placed in the new tuple's t_ctid
	 * field.
	 */
	numIndices = resultRelInfo->ri_NumIndices;
	if (numIndices > 0)
		ExecInsertIndexTuples(slot, &(tuple->t_self), estate, false);

	/* AFTER ROW INSERT Triggers */
	if (resultRelInfo->ri_TrigDesc)
		ExecARInsertTriggers(estate, resultRelInfo, tuple);
}

/* ----------------------------------------------------------------
 *		ExecDelete
 *
 *		DELETE is like UPDATE, we delete the tuple and its
 *		index tuples.
 * ----------------------------------------------------------------
 */
static void
ExecDelete(TupleTableSlot *slot,
		   ItemPointer tupleid,
		   EState *estate)
{
	ResultRelInfo *resultRelInfo;
	Relation	resultRelationDesc;
	ItemPointerData ctid;
	int			result;

	/*
	 * get information on the (current) result relation
	 */
	resultRelInfo = estate->es_result_relation_info;
	resultRelationDesc = resultRelInfo->ri_RelationDesc;

	/* BEFORE ROW DELETE Triggers */
	if (resultRelInfo->ri_TrigDesc &&
	  resultRelInfo->ri_TrigDesc->n_before_row[TRIGGER_EVENT_DELETE] > 0)
	{
		bool		dodelete;

		dodelete = ExecBRDeleteTriggers(estate, resultRelInfo, tupleid);

		if (!dodelete)			/* "do nothing" */
			return;
	}

	/*
	 * delete the tuple
	 */
ldelete:;
	result = heap_delete(resultRelationDesc, tupleid,
						 &ctid,
						 estate->es_snapshot->curcid);
	switch (result)
	{
		case HeapTupleSelfUpdated:
			/* already deleted by self; nothing to do */
			return;

		case HeapTupleMayBeUpdated:
			break;

		case HeapTupleUpdated:
			if (XactIsoLevel == XACT_SERIALIZABLE)
				elog(ERROR, "Can't serialize access due to concurrent update");
			else if (!(ItemPointerEquals(tupleid, &ctid)))
			{
				TupleTableSlot *epqslot = EvalPlanQual(estate,
							   resultRelInfo->ri_RangeTableIndex, &ctid);

				if (!TupIsNull(epqslot))
				{
					*tupleid = ctid;
					goto ldelete;
				}
			}
			/* tuple already deleted; nothing to do */
			return;

		default:
			elog(ERROR, "Unknown status %u from heap_delete", result);
			return;
	}

	IncrDeleted();
	(estate->es_processed)++;

	/*
	 * Note: Normally one would think that we have to delete index tuples
	 * associated with the heap tuple now..
	 *
	 * ... but in POSTGRES, we have no need to do this because the vacuum
	 * daemon automatically opens an index scan and deletes index tuples
	 * when it finds deleted heap tuples. -cim 9/27/89
	 */

	/* AFTER ROW DELETE Triggers */
	if (resultRelInfo->ri_TrigDesc)
		ExecARDeleteTriggers(estate, resultRelInfo, tupleid);
}

/* ----------------------------------------------------------------
 *		ExecUpdate
 *
 *		note: we can't run UPDATE queries with transactions
 *		off because UPDATEs are actually INSERTs and our
 *		scan will mistakenly loop forever, updating the tuple
 *		it just inserted..	This should be fixed but until it
 *		is, we don't want to get stuck in an infinite loop
 *		which corrupts your database..
 * ----------------------------------------------------------------
 */
static void
ExecUpdate(TupleTableSlot *slot,
		   ItemPointer tupleid,
		   EState *estate)
{
	HeapTuple	tuple;
	ResultRelInfo *resultRelInfo;
	Relation	resultRelationDesc;
	ItemPointerData ctid;
	int			result;
	int			numIndices;

	/*
	 * abort the operation if not running transactions
	 */
	if (IsBootstrapProcessingMode())
	{
		elog(WARNING, "ExecUpdate: UPDATE can't run without transactions");
		return;
	}

	/*
	 * get the heap tuple out of the tuple table slot
	 */
	tuple = slot->val;

	/*
	 * get information on the (current) result relation
	 */
	resultRelInfo = estate->es_result_relation_info;
	resultRelationDesc = resultRelInfo->ri_RelationDesc;

	/* BEFORE ROW UPDATE Triggers */
	if (resultRelInfo->ri_TrigDesc &&
	  resultRelInfo->ri_TrigDesc->n_before_row[TRIGGER_EVENT_UPDATE] > 0)
	{
		HeapTuple	newtuple;

		newtuple = ExecBRUpdateTriggers(estate, resultRelInfo,
										tupleid, tuple);

		if (newtuple == NULL)	/* "do nothing" */
			return;

		if (newtuple != tuple)	/* modified by Trigger(s) */
		{
			/*
			 * Insert modified tuple into tuple table slot, replacing the
			 * original.  We assume that it was allocated in per-tuple
			 * memory context, and therefore will go away by itself. The
			 * tuple table slot should not try to clear it.
			 */
			ExecStoreTuple(newtuple, slot, InvalidBuffer, false);
			tuple = newtuple;
		}
	}

	/*
	 * Check the constraints of the tuple
	 *
	 * If we generate a new candidate tuple after EvalPlanQual testing, we
	 * must loop back here and recheck constraints.  (We don't need to
	 * redo triggers, however.	If there are any BEFORE triggers then
	 * trigger.c will have done mark4update to lock the correct tuple, so
	 * there's no need to do them again.)
	 */
lreplace:;
	if (resultRelationDesc->rd_att->constr)
		ExecConstraints("ExecUpdate", resultRelInfo, slot, estate);

	/*
	 * replace the heap tuple
	 */
	result = heap_update(resultRelationDesc, tupleid, tuple,
						 &ctid,
						 estate->es_snapshot->curcid);
	switch (result)
	{
		case HeapTupleSelfUpdated:
			/* already deleted by self; nothing to do */
			return;

		case HeapTupleMayBeUpdated:
			break;

		case HeapTupleUpdated:
			if (XactIsoLevel == XACT_SERIALIZABLE)
				elog(ERROR, "Can't serialize access due to concurrent update");
			else if (!(ItemPointerEquals(tupleid, &ctid)))
			{
				TupleTableSlot *epqslot = EvalPlanQual(estate,
							   resultRelInfo->ri_RangeTableIndex, &ctid);

				if (!TupIsNull(epqslot))
				{
					*tupleid = ctid;
					tuple = ExecRemoveJunk(estate->es_junkFilter, epqslot);
					slot = ExecStoreTuple(tuple,
									estate->es_junkFilter->jf_resultSlot,
										  InvalidBuffer, true);
					goto lreplace;
				}
			}
			/* tuple already deleted; nothing to do */
			return;

		default:
			elog(ERROR, "Unknown status %u from heap_update", result);
			return;
	}

	IncrReplaced();
	(estate->es_processed)++;

	/*
	 * Note: instead of having to update the old index tuples associated
	 * with the heap tuple, all we do is form and insert new index tuples.
	 * This is because UPDATEs are actually DELETEs and INSERTs and index
	 * tuple deletion is done automagically by the vacuum daemon. All we
	 * do is insert new index tuples.  -cim 9/27/89
	 */

	/*
	 * process indices
	 *
	 * heap_update updates a tuple in the base relation by invalidating it
	 * and then inserting a new tuple to the relation.	As a side effect,
	 * the tupleid of the new tuple is placed in the new tuple's t_ctid
	 * field.  So we now insert index tuples using the new tupleid stored
	 * there.
	 */

	numIndices = resultRelInfo->ri_NumIndices;
	if (numIndices > 0)
		ExecInsertIndexTuples(slot, &(tuple->t_self), estate, false);

	/* AFTER ROW UPDATE Triggers */
	if (resultRelInfo->ri_TrigDesc)
		ExecARUpdateTriggers(estate, resultRelInfo, tupleid, tuple);
}

static char *
ExecRelCheck(ResultRelInfo *resultRelInfo,
			 TupleTableSlot *slot, EState *estate)
{
	Relation	rel = resultRelInfo->ri_RelationDesc;
	int			ncheck = rel->rd_att->constr->num_check;
	ConstrCheck *check = rel->rd_att->constr->check;
	ExprContext *econtext;
	MemoryContext oldContext;
	List	   *qual;
	int			i;

	/*
	 * If first time through for this result relation, build expression
	 * nodetrees for rel's constraint expressions.  Keep them in the
	 * per-query memory context so they'll survive throughout the query.
	 */
	if (resultRelInfo->ri_ConstraintExprs == NULL)
	{
		oldContext = MemoryContextSwitchTo(estate->es_query_cxt);
		resultRelInfo->ri_ConstraintExprs =
			(List **) palloc(ncheck * sizeof(List *));
		for (i = 0; i < ncheck; i++)
		{
			qual = (List *) stringToNode(check[i].ccbin);
			resultRelInfo->ri_ConstraintExprs[i] = qual;
		}
		MemoryContextSwitchTo(oldContext);
	}

	/*
	 * We will use the EState's per-tuple context for evaluating
	 * constraint expressions (creating it if it's not already there).
	 */
	econtext = GetPerTupleExprContext(estate);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* And evaluate the constraints */
	for (i = 0; i < ncheck; i++)
	{
		qual = resultRelInfo->ri_ConstraintExprs[i];

		/*
		 * NOTE: SQL92 specifies that a NULL result from a constraint
		 * expression is not to be treated as a failure.  Therefore, tell
		 * ExecQual to return TRUE for NULL.
		 */
		if (!ExecQual(qual, econtext, true))
			return check[i].ccname;
	}

	/* NULL result means no error */
	return (char *) NULL;
}

void
ExecConstraints(const char *caller, ResultRelInfo *resultRelInfo,
				TupleTableSlot *slot, EState *estate)
{
	Relation	rel = resultRelInfo->ri_RelationDesc;
	HeapTuple	tuple = slot->val;
	TupleConstr *constr = rel->rd_att->constr;

	Assert(constr);

	if (constr->has_not_null)
	{
		int			natts = rel->rd_att->natts;
		int			attrChk;

		for (attrChk = 1; attrChk <= natts; attrChk++)
		{
			if (rel->rd_att->attrs[attrChk - 1]->attnotnull &&
				heap_attisnull(tuple, attrChk))
				elog(ERROR, "%s: Fail to add null value in not null attribute %s",
					 caller, NameStr(rel->rd_att->attrs[attrChk - 1]->attname));
		}
	}

	if (constr->num_check > 0)
	{
		char	   *failed;

		if ((failed = ExecRelCheck(resultRelInfo, slot, estate)) != NULL)
			elog(ERROR, "%s: rejected due to CHECK constraint \"%s\" on \"%s\"",
				 caller, failed, RelationGetRelationName(rel));
	}
}

/*
 * Check a modified tuple to see if we want to process its updated version
 * under READ COMMITTED rules.
 *
 * See backend/executor/README for some info about how this works.
 */
TupleTableSlot *
EvalPlanQual(EState *estate, Index rti, ItemPointer tid)
{
	evalPlanQual *epq;
	EState	   *epqstate;
	Relation	relation;
	HeapTupleData tuple;
	HeapTuple	copyTuple = NULL;
	int			rtsize;
	bool		endNode;

	Assert(rti != 0);

	/*
	 * find relation containing target tuple
	 */
	if (estate->es_result_relation_info != NULL &&
		estate->es_result_relation_info->ri_RangeTableIndex == rti)
		relation = estate->es_result_relation_info->ri_RelationDesc;
	else
	{
		List	   *l;

		relation = NULL;
		foreach(l, estate->es_rowMark)
		{
			if (((execRowMark *) lfirst(l))->rti == rti)
			{
				relation = ((execRowMark *) lfirst(l))->relation;
				break;
			}
		}
		if (relation == NULL)
			elog(ERROR, "EvalPlanQual: can't find RTE %d", (int) rti);
	}

	/*
	 * fetch tid tuple
	 *
	 * Loop here to deal with updated or busy tuples
	 */
	tuple.t_self = *tid;
	for (;;)
	{
		Buffer		buffer;

		if (heap_fetch(relation, SnapshotDirty, &tuple, &buffer, false, NULL))
		{
			TransactionId xwait = SnapshotDirty->xmax;

			if (TransactionIdIsValid(SnapshotDirty->xmin))
				elog(ERROR, "EvalPlanQual: t_xmin is uncommitted ?!");

			/*
			 * If tuple is being updated by other transaction then we have
			 * to wait for its commit/abort.
			 */
			if (TransactionIdIsValid(xwait))
			{
				ReleaseBuffer(buffer);
				XactLockTableWait(xwait);
				continue;
			}

			/*
			 * We got tuple - now copy it for use by recheck query.
			 */
			copyTuple = heap_copytuple(&tuple);
			ReleaseBuffer(buffer);
			break;
		}

		/*
		 * Oops! Invalid tuple. Have to check is it updated or deleted.
		 * Note that it's possible to get invalid SnapshotDirty->tid if
		 * tuple updated by this transaction. Have we to check this ?
		 */
		if (ItemPointerIsValid(&(SnapshotDirty->tid)) &&
			!(ItemPointerEquals(&(tuple.t_self), &(SnapshotDirty->tid))))
		{
			/* updated, so look at the updated copy */
			tuple.t_self = SnapshotDirty->tid;
			continue;
		}

		/*
		 * Deleted or updated by this transaction; forget it.
		 */
		return NULL;
	}

	/*
	 * For UPDATE/DELETE we have to return tid of actual row we're
	 * executing PQ for.
	 */
	*tid = tuple.t_self;

	/*
	 * Need to run a recheck subquery.	Find or create a PQ stack entry.
	 */
	epq = (evalPlanQual *) estate->es_evalPlanQual;
	rtsize = length(estate->es_range_table);
	endNode = true;

	if (epq != NULL && epq->rti == 0)
	{
		/* Top PQ stack entry is idle, so re-use it */
		Assert(!(estate->es_useEvalPlan) &&
			   epq->estate.es_evalPlanQual == NULL);
		epq->rti = rti;
		endNode = false;
	}

	/*
	 * If this is request for another RTE - Ra, - then we have to check
	 * wasn't PlanQual requested for Ra already and if so then Ra' row was
	 * updated again and we have to re-start old execution for Ra and
	 * forget all what we done after Ra was suspended. Cool? -:))
	 */
	if (epq != NULL && epq->rti != rti &&
		epq->estate.es_evTuple[rti - 1] != NULL)
	{
		do
		{
			evalPlanQual *oldepq;

			/* pop previous PlanQual from the stack */
			epqstate = &(epq->estate);
			oldepq = (evalPlanQual *) epqstate->es_evalPlanQual;
			Assert(oldepq->rti != 0);
			/* stop execution */
			ExecEndNode(epq->plan, NULL);
			ExecDropTupleTable(epqstate->es_tupleTable, true);
			epqstate->es_tupleTable = NULL;
			heap_freetuple(epqstate->es_evTuple[epq->rti - 1]);
			epqstate->es_evTuple[epq->rti - 1] = NULL;
			/* push current PQ to freePQ stack */
			oldepq->free = epq;
			epq = oldepq;
			estate->es_evalPlanQual = (Pointer) epq;
		} while (epq->rti != rti);
	}

	/*
	 * If we are requested for another RTE then we have to suspend
	 * execution of current PlanQual and start execution for new one.
	 */
	if (epq == NULL || epq->rti != rti)
	{
		/* try to reuse plan used previously */
		evalPlanQual *newepq = (epq != NULL) ? epq->free : NULL;

		if (newepq == NULL)		/* first call or freePQ stack is empty */
		{
			newepq = (evalPlanQual *) palloc(sizeof(evalPlanQual));
			newepq->free = NULL;

			/*
			 * Each stack level has its own copy of the plan tree.	This
			 * is wasteful, but necessary as long as plan nodes point to
			 * exec state nodes rather than vice versa.  Note that
			 * copyfuncs.c doesn't attempt to copy the exec state nodes,
			 * which is a good thing in this situation.
			 */
			newepq->plan = copyObject(estate->es_origPlan);

			/*
			 * Init stack level's EState.  We share top level's copy of
			 * es_result_relations array and other non-changing status. We
			 * need our own tupletable, es_param_exec_vals, and other
			 * changeable state.
			 */
			epqstate = &(newepq->estate);
			memcpy(epqstate, estate, sizeof(EState));
			epqstate->es_direction = ForwardScanDirection;
			if (estate->es_origPlan->nParamExec > 0)
				epqstate->es_param_exec_vals = (ParamExecData *)
					palloc(estate->es_origPlan->nParamExec *
						   sizeof(ParamExecData));
			epqstate->es_tupleTable = NULL;
			epqstate->es_per_tuple_exprcontext = NULL;

			/*
			 * Each epqstate must have its own es_evTupleNull state, but
			 * all the stack entries share es_evTuple state.  This allows
			 * sub-rechecks to inherit the value being examined by an
			 * outer recheck.
			 */
			epqstate->es_evTupleNull = (bool *) palloc(rtsize * sizeof(bool));
			if (epq == NULL)
			{
				/* first PQ stack entry */
				epqstate->es_evTuple = (HeapTuple *)
					palloc(rtsize * sizeof(HeapTuple));
				memset(epqstate->es_evTuple, 0, rtsize * sizeof(HeapTuple));
			}
			else
			{
				/* later stack entries share the same storage */
				epqstate->es_evTuple = epq->estate.es_evTuple;
			}
		}
		else
		{
			/* recycle previously used EState */
			epqstate = &(newepq->estate);
		}
		/* push current PQ to the stack */
		epqstate->es_evalPlanQual = (Pointer) epq;
		epq = newepq;
		estate->es_evalPlanQual = (Pointer) epq;
		epq->rti = rti;
		endNode = false;
	}

	Assert(epq->rti == rti);
	epqstate = &(epq->estate);

	/*
	 * Ok - we're requested for the same RTE.  Unfortunately we still have
	 * to end and restart execution of the plan, because ExecReScan
	 * wouldn't ensure that upper plan nodes would reset themselves.  We
	 * could make that work if insertion of the target tuple were
	 * integrated with the Param mechanism somehow, so that the upper plan
	 * nodes know that their children's outputs have changed.
	 */
	if (endNode)
	{
		/* stop execution */
		ExecEndNode(epq->plan, NULL);
		ExecDropTupleTable(epqstate->es_tupleTable, true);
		epqstate->es_tupleTable = NULL;
	}

	/*
	 * free old RTE' tuple, if any, and store target tuple where
	 * relation's scan node will see it
	 */
	if (epqstate->es_evTuple[rti - 1] != NULL)
		heap_freetuple(epqstate->es_evTuple[rti - 1]);
	epqstate->es_evTuple[rti - 1] = copyTuple;

	/*
	 * Initialize for new recheck query; be careful to copy down state
	 * that might have changed in top EState.
	 */
	epqstate->es_result_relation_info = estate->es_result_relation_info;
	epqstate->es_junkFilter = estate->es_junkFilter;
	if (estate->es_origPlan->nParamExec > 0)
		memset(epqstate->es_param_exec_vals, 0,
			   estate->es_origPlan->nParamExec * sizeof(ParamExecData));
	epqstate->es_force_oids = estate->es_force_oids;
	memset(epqstate->es_evTupleNull, false, rtsize * sizeof(bool));
	epqstate->es_useEvalPlan = false;
	Assert(epqstate->es_tupleTable == NULL);
	epqstate->es_tupleTable =
		ExecCreateTupleTable(estate->es_tupleTable->size);

	ExecInitNode(epq->plan, epqstate, NULL);

	return EvalPlanQualNext(estate);
}

static TupleTableSlot *
EvalPlanQualNext(EState *estate)
{
	evalPlanQual *epq = (evalPlanQual *) estate->es_evalPlanQual;
	EState	   *epqstate = &(epq->estate);
	evalPlanQual *oldepq;
	TupleTableSlot *slot;

	Assert(epq->rti != 0);

lpqnext:;
	slot = ExecProcNode(epq->plan, NULL);

	/*
	 * No more tuples for this PQ. Continue previous one.
	 */
	if (TupIsNull(slot))
	{
		/* stop execution */
		ExecEndNode(epq->plan, NULL);
		ExecDropTupleTable(epqstate->es_tupleTable, true);
		epqstate->es_tupleTable = NULL;
		heap_freetuple(epqstate->es_evTuple[epq->rti - 1]);
		epqstate->es_evTuple[epq->rti - 1] = NULL;
		/* pop old PQ from the stack */
		oldepq = (evalPlanQual *) epqstate->es_evalPlanQual;
		if (oldepq == (evalPlanQual *) NULL)
		{
			epq->rti = 0;		/* this is the first (oldest) */
			estate->es_useEvalPlan = false;		/* PQ - mark as free and	  */
			return (NULL);		/* continue Query execution   */
		}
		Assert(oldepq->rti != 0);
		/* push current PQ to freePQ stack */
		oldepq->free = epq;
		epq = oldepq;
		epqstate = &(epq->estate);
		estate->es_evalPlanQual = (Pointer) epq;
		goto lpqnext;
	}

	return (slot);
}

static void
EndEvalPlanQual(EState *estate)
{
	evalPlanQual *epq = (evalPlanQual *) estate->es_evalPlanQual;
	EState	   *epqstate = &(epq->estate);
	evalPlanQual *oldepq;

	if (epq->rti == 0)			/* plans already shutdowned */
	{
		Assert(epq->estate.es_evalPlanQual == NULL);
		return;
	}

	for (;;)
	{
		/* stop execution */
		ExecEndNode(epq->plan, NULL);
		ExecDropTupleTable(epqstate->es_tupleTable, true);
		epqstate->es_tupleTable = NULL;
		if (epqstate->es_evTuple[epq->rti - 1] != NULL)
		{
			heap_freetuple(epqstate->es_evTuple[epq->rti - 1]);
			epqstate->es_evTuple[epq->rti - 1] = NULL;
		}
		/* pop old PQ from the stack */
		oldepq = (evalPlanQual *) epqstate->es_evalPlanQual;
		if (oldepq == (evalPlanQual *) NULL)
		{
			epq->rti = 0;		/* this is the first (oldest) */
			estate->es_useEvalPlan = false;		/* PQ - mark as free */
			break;
		}
		Assert(oldepq->rti != 0);
		/* push current PQ to freePQ stack */
		oldepq->free = epq;
		epq = oldepq;
		epqstate = &(epq->estate);
		estate->es_evalPlanQual = (Pointer) epq;
	}
}
