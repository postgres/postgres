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
 *	In each case, the query descriptor is required as an argument.
 *
 *	ExecutorStart() must be called at the beginning of execution of any
 *	query plan and ExecutorEnd() should always be called at the end of
 *	execution of a plan.
 *
 *	ExecutorRun accepts direction and count arguments that specify whether
 *	the plan is to be executed forwards, backwards, and for how many tuples.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execMain.c,v 1.220.2.4 2006/01/12 21:49:32 tgl Exp $
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


typedef struct execRowMark
{
	Relation	relation;
	Index		rti;
	char		resname[32];
} execRowMark;

typedef struct evalPlanQual
{
	Index		rti;
	EState	   *estate;
	PlanState  *planstate;
	struct evalPlanQual *next;	/* stack of active PlanQual plans */
	struct evalPlanQual *free;	/* list of free PlanQual plans */
} evalPlanQual;

/* decls for local routines only used within this module */
static void InitPlan(QueryDesc *queryDesc, bool explainOnly);
static void initResultRelInfo(ResultRelInfo *resultRelInfo,
				  Index resultRelationIndex,
				  List *rangeTable,
				  CmdType operation);
static TupleTableSlot *ExecutePlan(EState *estate, PlanState *planstate,
			CmdType operation,
			long numberTuples,
			ScanDirection direction,
			DestReceiver *dest);
static void ExecSelect(TupleTableSlot *slot,
		   DestReceiver *dest,
		   EState *estate);
static void ExecInsert(TupleTableSlot *slot, ItemPointer tupleid,
		   EState *estate);
static void ExecDelete(TupleTableSlot *slot, ItemPointer tupleid,
		   EState *estate);
static void ExecUpdate(TupleTableSlot *slot, ItemPointer tupleid,
		   EState *estate);
static TupleTableSlot *EvalPlanQualNext(EState *estate);
static void EndEvalPlanQual(EState *estate);
static void ExecCheckRTEPerms(RangeTblEntry *rte, CmdType operation);
static void ExecCheckXactReadOnly(Query *parsetree, CmdType operation);
static void EvalPlanQualStart(evalPlanQual *epq, EState *estate,
				  evalPlanQual *priorepq);
static void EvalPlanQualStop(evalPlanQual *epq);

/* end of local decls */


/* ----------------------------------------------------------------
 *		ExecutorStart
 *
 *		This routine must be called at the beginning of any execution of any
 *		query plan
 *
 * Takes a QueryDesc previously created by CreateQueryDesc (it's not real
 * clear why we bother to separate the two functions, but...).	The tupDesc
 * field of the QueryDesc is filled in to describe the tuples that will be
 * returned, and the internal fields (estate and planstate) are set up.
 *
 * If useCurrentSnapshot is true, run the query with the latest available
 * snapshot, instead of the normal QuerySnapshot.  Also, if it's an update
 * or delete query, check that the rows to be updated or deleted would be
 * visible to the normal QuerySnapshot.  (This is a special-case behavior
 * needed for referential integrity updates in serializable transactions.
 * We must check all currently-committed rows, but we want to throw a
 * can't-serialize error if any rows that would need updates would not be
 * visible under the normal serializable snapshot.)
 *
 * If explainOnly is true, we are not actually intending to run the plan,
 * only to set up for EXPLAIN; so skip unwanted side-effects.
 *
 * NB: the CurrentMemoryContext when this is called will become the parent
 * of the per-query context used for this Executor invocation.
 * ----------------------------------------------------------------
 */
void
ExecutorStart(QueryDesc *queryDesc, bool useCurrentSnapshot, bool explainOnly)
{
	EState	   *estate;
	MemoryContext oldcontext;

	/* sanity checks: queryDesc must not be started already */
	Assert(queryDesc != NULL);
	Assert(queryDesc->estate == NULL);

	/*
	 * If the transaction is read-only, we need to check if any writes are
	 * planned to non-temporary tables.
	 */
	if (!explainOnly)
		ExecCheckXactReadOnly(queryDesc->parsetree, queryDesc->operation);

	/*
	 * Build EState, switch into per-query memory context for startup.
	 */
	estate = CreateExecutorState();
	queryDesc->estate = estate;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/*
	 * Fill in parameters, if any, from queryDesc
	 */
	estate->es_param_list_info = queryDesc->params;

	if (queryDesc->plantree->nParamExec > 0)
		estate->es_param_exec_vals = (ParamExecData *)
			palloc0(queryDesc->plantree->nParamExec * sizeof(ParamExecData));

	estate->es_instrument = queryDesc->doInstrument;

	/*
	 * Make our own private copy of the current query snapshot data.
	 *
	 * This "freezes" our idea of which tuples are good and which are not for
	 * the life of this query, even if it outlives the current command and
	 * current snapshot.
	 */
	if (useCurrentSnapshot)
	{
		/* RI update/delete query --- must use an up-to-date snapshot */
		estate->es_snapshot = CopyCurrentSnapshot();
		/* crosscheck updates/deletes against transaction snapshot */
		estate->es_crosscheck_snapshot = CopyQuerySnapshot();
	}
	else
	{
		/* normal query --- use query snapshot, no crosscheck */
		estate->es_snapshot = CopyQuerySnapshot();
		estate->es_crosscheck_snapshot = SnapshotAny;
	}

	/*
	 * Initialize the plan state tree
	 */
	InitPlan(queryDesc, explainOnly);

	MemoryContextSwitchTo(oldcontext);
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
 *		Note: count = 0 is interpreted as no portal limit, i.e., run to
 *		completion.
 *
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecutorRun(QueryDesc *queryDesc,
			ScanDirection direction, long count)
{
	EState	   *estate;
	CmdType		operation;
	DestReceiver *dest;
	TupleTableSlot *result;
	MemoryContext oldcontext;

	/* sanity checks */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);

	/*
	 * Switch into per-query memory context
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/*
	 * extract information from the query descriptor and the query
	 * feature.
	 */
	operation = queryDesc->operation;
	dest = queryDesc->dest;

	/*
	 * startup tuple receiver
	 */
	estate->es_processed = 0;
	estate->es_lastoid = InvalidOid;

	(*dest->rStartup) (dest, operation, queryDesc->tupDesc);

	/*
	 * run plan
	 */
	if (direction == NoMovementScanDirection)
		result = NULL;
	else
		result = ExecutePlan(estate,
							 queryDesc->planstate,
							 operation,
							 count,
							 direction,
							 dest);

	/*
	 * shutdown receiver
	 */
	(*dest->rShutdown) (dest);

	MemoryContextSwitchTo(oldcontext);

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
ExecutorEnd(QueryDesc *queryDesc)
{
	EState	   *estate;
	MemoryContext oldcontext;

	/* sanity checks */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);

	/*
	 * Switch into per-query memory context to run ExecEndPlan
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	ExecEndPlan(queryDesc->planstate, estate);

	/*
	 * Must switch out of context before destroying it
	 */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * Release EState and per-query memory context.  This should release
	 * everything the executor has allocated.
	 */
	FreeExecutorState(estate);

	/* Reset queryDesc fields that no longer point to anything */
	queryDesc->tupDesc = NULL;
	queryDesc->estate = NULL;
	queryDesc->planstate = NULL;
}

/* ----------------------------------------------------------------
 *		ExecutorRewind
 *
 *		This routine may be called on an open queryDesc to rewind it
 *		to the start.
 * ----------------------------------------------------------------
 */
void
ExecutorRewind(QueryDesc *queryDesc)
{
	EState	   *estate;
	MemoryContext oldcontext;

	/* sanity checks */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);

	/* It's probably not sensible to rescan updating queries */
	Assert(queryDesc->operation == CMD_SELECT);

	/*
	 * Switch into per-query memory context
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/*
	 * rescan plan
	 */
	ExecReScan(queryDesc->planstate, NULL);

	MemoryContextSwitchTo(oldcontext);
}


/*
 * ExecCheckRTPerms
 *		Check access permissions for all relations listed in a range table.
 */
void
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
	AclId		userid;
	AclResult	aclcheck_result;

	/*
	 * If it's a subquery, recursively examine its rangetable.
	 */
	if (rte->rtekind == RTE_SUBQUERY)
	{
		ExecCheckRTPerms(rte->subquery->rtable, operation);
		return;
	}

	/*
	 * Otherwise, only plain-relation RTEs need to be checked here.
	 * Function RTEs are checked by init_fcache when the function is
	 * prepared for execution. Join and special RTEs need no checks.
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
	 * could call it once in ExecCheckRTPerms and pass the userid down
	 * from there.	But for now, no need for the extra clutter.
	 */
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

#define CHECK(MODE)		pg_class_aclcheck(relOid, userid, MODE)

	if (rte->checkForRead)
	{
		aclcheck_result = CHECK(ACL_SELECT);
		if (aclcheck_result != ACLCHECK_OK)
			aclcheck_error(aclcheck_result, ACL_KIND_CLASS,
						   get_rel_name(relOid));
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
				elog(ERROR, "unrecognized operation code: %d",
					 (int) operation);
				aclcheck_result = ACLCHECK_OK;	/* keep compiler quiet */
				break;
		}
		if (aclcheck_result != ACLCHECK_OK)
			aclcheck_error(aclcheck_result, ACL_KIND_CLASS,
						   get_rel_name(relOid));
	}
}

static void
ExecCheckXactReadOnly(Query *parsetree, CmdType operation)
{
	if (!XactReadOnly)
		return;

	/* CREATE TABLE AS or SELECT INTO */
	if (operation == CMD_SELECT && parsetree->into != NULL)
		goto fail;

	if (operation == CMD_DELETE || operation == CMD_INSERT
		|| operation == CMD_UPDATE)
	{
		List	   *lp;

		foreach(lp, parsetree->rtable)
		{
			RangeTblEntry *rte = lfirst(lp);

			if (rte->rtekind != RTE_RELATION)
				continue;

			if (!rte->checkForWrite)
				continue;

			if (isTempNamespace(get_rel_namespace(rte->relid)))
				continue;

			goto fail;
		}
	}

	return;

fail:
	ereport(ERROR,
			(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
			 errmsg("transaction is read-only")));
}


/* ----------------------------------------------------------------
 *		InitPlan
 *
 *		Initializes the query plan: open files, allocate storage
 *		and start up the rule manager
 * ----------------------------------------------------------------
 */
static void
InitPlan(QueryDesc *queryDesc, bool explainOnly)
{
	CmdType		operation = queryDesc->operation;
	Query	   *parseTree = queryDesc->parsetree;
	Plan	   *plan = queryDesc->plantree;
	EState	   *estate = queryDesc->estate;
	PlanState  *planstate;
	List	   *rangeTable;
	Relation	intoRelationDesc;
	bool		do_select_into;
	TupleDesc	tupType;

	/*
	 * Do permissions checks.  It's sufficient to examine the query's top
	 * rangetable here --- subplan RTEs will be checked during
	 * ExecInitSubPlan().
	 */
	ExecCheckRTPerms(parseTree->rtable, operation);

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

	if (operation == CMD_SELECT && parseTree->into != NULL)
	{
		do_select_into = true;
		estate->es_select_into = true;

		/*
		 * For now, always create OIDs in SELECT INTO; this is for
		 * backwards compatibility with pre-7.3 behavior.  Eventually we
		 * might want to allow the user to choose.
		 */
		estate->es_into_oids = true;
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
			snprintf(erm->resname, sizeof(erm->resname), "ctid%u", rti);
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
	estate->es_topPlan = plan;
	estate->es_evalPlanQual = NULL;
	estate->es_evTupleNull = NULL;
	estate->es_evTuple = NULL;
	estate->es_useEvalPlan = false;

	/*
	 * initialize the private state information for all the nodes in the
	 * query tree.	This opens files, allocates storage and leaves us
	 * ready to start processing tuples.
	 */
	planstate = ExecInitNode(plan, estate);

	/*
	 * Get the tuple descriptor describing the type of tuples to return.
	 * (this is especially important if we are creating a relation with
	 * "SELECT INTO")
	 */
	tupType = ExecGetResultType(planstate);

	/*
	 * Initialize the junk filter if needed.  SELECT and INSERT queries
	 * need a filter if there are any junk attrs in the tlist.	INSERT and
	 * SELECT INTO also need a filter if the plan may return raw disk tuples
	 * (else heap_insert will be scribbling on the source relation!).
	 * UPDATE and DELETE always need a filter, since there's always a junk
	 * 'ctid' attribute present --- no need to look first.
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
				if (!junk_filter_needed &&
					(operation == CMD_INSERT || do_select_into) &&
					ExecMayReturnRawTuples(planstate))
					junk_filter_needed = true;
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
				PlanState **appendplans;
				int			as_nplans;
				ResultRelInfo *resultRelInfo;
				int			i;

				/* Top plan had better be an Append here. */
				Assert(IsA(plan, Append));
				Assert(((Append *) plan)->isTarget);
				Assert(IsA(planstate, AppendState));
				appendplans = ((AppendState *) planstate)->appendplans;
				as_nplans = ((AppendState *) planstate)->as_nplans;
				Assert(as_nplans == estate->es_num_result_relations);
				resultRelInfo = estate->es_result_relations;
				for (i = 0; i < as_nplans; i++)
				{
					PlanState  *subplan = appendplans[i];
					JunkFilter *j;

					j = ExecInitJunkFilter(subplan->plan->targetlist,
										   ExecGetResultType(subplan),
							  ExecAllocTableSlot(estate->es_tupleTable));
					resultRelInfo->ri_junkFilter = j;
					resultRelInfo++;
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

				j = ExecInitJunkFilter(planstate->plan->targetlist,
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
	 * If doing SELECT INTO, initialize the "into" relation.  We must wait
	 * till now so we have the "clean" result tuple type to create the new
	 * table from.
	 *
	 * If EXPLAIN, skip creating the "into" relation.
	 */
	intoRelationDesc = (Relation) NULL;

	if (do_select_into && !explainOnly)
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
			aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
						   get_namespace_name(namespaceId));

		/*
		 * have to copy tupType to get rid of constraints
		 */
		tupdesc = CreateTupleDescCopy(tupType);

		intoRelationId = heap_create_with_catalog(intoName,
												  namespaceId,
												  tupdesc,
												  RELKIND_RELATION,
												  false,
												  ONCOMMIT_NOOP,
												  allowSystemTableMods);

		FreeTupleDesc(tupdesc);

		/*
		 * Advance command counter so that the newly-created relation's
		 * catalog tuples will be visible to heap_open.
		 */
		CommandCounterIncrement();

		/*
		 * If necessary, create a TOAST table for the into relation. Note
		 * that AlterTableCreateToastTable ends with
		 * CommandCounterIncrement(), so that the TOAST table will be
		 * visible for insertion.
		 */
		AlterTableCreateToastTable(intoRelationId, true);

		/*
		 * And open the constructed table for writing.
		 */
		intoRelationDesc = heap_open(intoRelationId, AccessExclusiveLock);
	}

	estate->es_into_relation_descriptor = intoRelationDesc;

	queryDesc->tupDesc = tupType;
	queryDesc->planstate = planstate;
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
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change sequence \"%s\"",
						  RelationGetRelationName(resultRelationDesc))));
			break;
		case RELKIND_TOASTVALUE:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change TOAST relation \"%s\"",
						  RelationGetRelationName(resultRelationDesc))));
			break;
		case RELKIND_VIEW:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change view \"%s\"",
						  RelationGetRelationName(resultRelationDesc))));
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

/*
 *		ExecContextForcesOids
 *
 * This is pretty grotty: when doing INSERT, UPDATE, or SELECT INTO,
 * we need to ensure that result tuples have space for an OID iff they are
 * going to be stored into a relation that has OIDs.  In other contexts
 * we are free to choose whether to leave space for OIDs in result tuples
 * (we generally don't want to, but we do if a physical-tlist optimization
 * is possible).  This routine checks the plan context and returns TRUE if the
 * choice is forced, FALSE if the choice is not forced.  In the TRUE case,
 * *hasoids is set to the required value.
 *
 * One reason this is ugly is that all plan nodes in the plan tree will emit
 * tuples with space for an OID, though we really only need the topmost node
 * to do so.  However, node types like Sort don't project new tuples but just
 * return their inputs, and in those cases the requirement propagates down
 * to the input node.  Eventually we might make this code smart enough to
 * recognize how far down the requirement really goes, but for now we just
 * make all plan nodes do the same thing if the top level forces the choice.
 *
 * We assume that estate->es_result_relation_info is already set up to
 * describe the target relation.  Note that in an UPDATE that spans an
 * inheritance tree, some of the target relations may have OIDs and some not.
 * We have to make the decisions on a per-relation basis as we initialize
 * each of the child plans of the topmost Append plan.
 *
 * SELECT INTO is even uglier, because we don't have the INTO relation's
 * descriptor available when this code runs; we have to look aside at a
 * flag set by InitPlan().
 */
bool
ExecContextForcesOids(PlanState *planstate, bool *hasoids)
{
	if (planstate->state->es_select_into)
	{
		*hasoids = planstate->state->es_into_oids;
		return true;
	}
	else
	{
		ResultRelInfo *ri = planstate->state->es_result_relation_info;

		if (ri != NULL)
		{
			Relation	rel = ri->ri_RelationDesc;

			if (rel != NULL)
			{
				*hasoids = rel->rd_rel->relhasoids;
				return true;
			}
		}
	}

	return false;
}

/* ----------------------------------------------------------------
 *		ExecEndPlan
 *
 *		Cleans up the query plan -- closes files and frees up storage
 *
 * NOTE: we are no longer very worried about freeing storage per se
 * in this code; FreeExecutorState should be guaranteed to release all
 * memory that needs to be released.  What we are worried about doing
 * is closing relations and dropping buffer pins.  Thus, for example,
 * tuple tables must be cleared or dropped to ensure pins are released.
 * ----------------------------------------------------------------
 */
void
ExecEndPlan(PlanState *planstate, EState *estate)
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
	ExecEndNode(planstate);

	/*
	 * destroy the executor "tuple" table.
	 */
	ExecDropTupleTable(estate->es_tupleTable, true);
	estate->es_tupleTable = NULL;

	/*
	 * close the result relation(s) if any, but hold locks until xact
	 * commit.
	 */
	resultRelInfo = estate->es_result_relations;
	for (i = estate->es_num_result_relations; i > 0; i--)
	{
		/* Close indices and then the relation itself */
		ExecCloseIndices(resultRelInfo);
		heap_close(resultRelInfo->ri_RelationDesc, NoLock);
		resultRelInfo++;
	}

	/*
	 * close the "into" relation if necessary, again keeping lock
	 */
	if (estate->es_into_relation_descriptor != NULL)
		heap_close(estate->es_into_relation_descriptor, NoLock);

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
 *
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
			PlanState *planstate,
			CmdType operation,
			long numberTuples,
			ScanDirection direction,
			DestReceiver *dest)
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
	 * Process BEFORE EACH STATEMENT triggers
	 */
	switch (operation)
	{
		case CMD_UPDATE:
			ExecBSUpdateTriggers(estate, estate->es_result_relation_info);
			break;
		case CMD_DELETE:
			ExecBSDeleteTriggers(estate, estate->es_result_relation_info);
			break;
		case CMD_INSERT:
			ExecBSInsertTriggers(estate, estate->es_result_relation_info);
			break;
		default:
			/* do nothing */
			break;
	}

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
				slot = ExecProcNode(planstate);
		}
		else
			slot = ExecProcNode(planstate);

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
					elog(ERROR, "could not find junk ctid column");

				/* shouldn't ever get a null result... */
				if (isNull)
					elog(ERROR, "ctid is NULL");

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
					HeapTupleData tuple;
					Buffer		buffer;
					ItemPointerData update_ctid;
					TransactionId update_xmax;
					TupleTableSlot *newSlot;
					int			test;

					if (!ExecGetJunkAttribute(junkfilter,
											  slot,
											  erm->resname,
											  &datum,
											  &isNull))
						elog(ERROR, "could not find junk \"%s\" column",
							 erm->resname);

					/* shouldn't ever get a null result... */
					if (isNull)
						elog(ERROR, "\"%s\" is NULL", erm->resname);

					tuple.t_self = *((ItemPointer) DatumGetPointer(datum));
					test = heap_mark4update(erm->relation, &tuple, &buffer,
											&update_ctid, &update_xmax,
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
								ereport(ERROR,
										(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
										 errmsg("could not serialize access due to concurrent update")));
							if (!ItemPointerEquals(&update_ctid,
												   &tuple.t_self))
							{
								/* updated, so look at updated version */
								newSlot = EvalPlanQual(estate,
													   erm->rti,
													   &update_ctid,
													   update_xmax,
													   estate->es_snapshot->curcid);
								if (!TupIsNull(newSlot))
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
							elog(ERROR, "unrecognized heap_mark4update status: %u",
								 test);
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
						   dest,	/* destination's tuple-receiver obj */
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
				elog(ERROR, "unrecognized operation code: %d",
					 (int) operation);
				result = NULL;
				break;
		}

		/*
		 * check our tuple count.. if we've processed the proper number
		 * then quit, else loop again and process more tuples.	Zero
		 * numberTuples means no limit.
		 */
		current_tuple_count++;
		if (numberTuples && numberTuples == current_tuple_count)
			break;
	}

	/*
	 * Process AFTER EACH STATEMENT triggers
	 */
	switch (operation)
	{
		case CMD_UPDATE:
			ExecASUpdateTriggers(estate, estate->es_result_relation_info);
			break;
		case CMD_DELETE:
			ExecASDeleteTriggers(estate, estate->es_result_relation_info);
			break;
		case CMD_INSERT:
			ExecASInsertTriggers(estate, estate->es_result_relation_info);
			break;
		default:
			/* do nothing */
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
		   DestReceiver *dest,
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
	 *
	 * XXX this probably ought to be replaced by a separate destination
	 */
	if (estate->es_into_relation_descriptor != NULL)
	{
		heap_insert(estate->es_into_relation_descriptor, tuple,
					estate->es_snapshot->curcid);
		IncrAppended();
	}

	/*
	 * send the tuple to the destination
	 */
	(*dest->receiveTuple) (tuple, attrtype, dest);
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
		ExecConstraints(resultRelInfo, slot, estate);

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
	int			result;
	ItemPointerData update_ctid;
	TransactionId update_xmax;

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

		dodelete = ExecBRDeleteTriggers(estate, resultRelInfo, tupleid,
										estate->es_snapshot->curcid);

		if (!dodelete)			/* "do nothing" */
			return;
	}

	/*
	 * delete the tuple
	 */
ldelete:;
	result = heap_delete(resultRelationDesc, tupleid,
						 &update_ctid, &update_xmax,
						 estate->es_snapshot->curcid,
						 estate->es_crosscheck_snapshot,
						 true /* wait for commit */);
	switch (result)
	{
		case HeapTupleSelfUpdated:
			/* already deleted by self; nothing to do */
			return;

		case HeapTupleMayBeUpdated:
			break;

		case HeapTupleUpdated:
			if (XactIsoLevel == XACT_SERIALIZABLE)
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("could not serialize access due to concurrent update")));
			else if (!ItemPointerEquals(tupleid, &update_ctid))
			{
				TupleTableSlot *epqslot;

				epqslot = EvalPlanQual(estate,
									   resultRelInfo->ri_RangeTableIndex,
									   &update_ctid,
									   update_xmax,
									   estate->es_snapshot->curcid);
				if (!TupIsNull(epqslot))
				{
					*tupleid = update_ctid;
					goto ldelete;
				}
			}
			/* tuple already deleted; nothing to do */
			return;

		default:
			elog(ERROR, "unrecognized heap_delete status: %u", result);
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
	int			result;
	ItemPointerData update_ctid;
	TransactionId update_xmax;
	int			numIndices;

	/*
	 * abort the operation if not running transactions
	 */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "cannot UPDATE during bootstrap");

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
										tupleid, tuple,
										estate->es_snapshot->curcid);

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
		ExecConstraints(resultRelInfo, slot, estate);

	/*
	 * replace the heap tuple
	 */
	result = heap_update(resultRelationDesc, tupleid, tuple,
						 &update_ctid, &update_xmax,
						 estate->es_snapshot->curcid,
						 estate->es_crosscheck_snapshot,
						 true /* wait for commit */);
	switch (result)
	{
		case HeapTupleSelfUpdated:
			/* already deleted by self; nothing to do */
			return;

		case HeapTupleMayBeUpdated:
			break;

		case HeapTupleUpdated:
			if (XactIsoLevel == XACT_SERIALIZABLE)
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("could not serialize access due to concurrent update")));
			else if (!(ItemPointerEquals(tupleid, &update_ctid)))
			{
				TupleTableSlot *epqslot;

				epqslot = EvalPlanQual(estate,
									   resultRelInfo->ri_RangeTableIndex,
									   &update_ctid,
									   update_xmax,
									   estate->es_snapshot->curcid);
				if (!TupIsNull(epqslot))
				{
					*tupleid = update_ctid;
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
			elog(ERROR, "unrecognized heap_update status: %u", result);
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
	ExecARUpdateTriggers(estate, resultRelInfo, tupleid, tuple);
}

static const char *
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
			resultRelInfo->ri_ConstraintExprs[i] = (List *)
				ExecPrepareExpr((Expr *) qual, estate);
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
	return NULL;
}

void
ExecConstraints(ResultRelInfo *resultRelInfo,
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
				ereport(ERROR,
						(errcode(ERRCODE_NOT_NULL_VIOLATION),
						 errmsg("null value in column \"%s\" violates not-null constraint",
					NameStr(rel->rd_att->attrs[attrChk - 1]->attname))));
		}
	}

	if (constr->num_check > 0)
	{
		const char *failed;

		if ((failed = ExecRelCheck(resultRelInfo, slot, estate)) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CHECK_VIOLATION),
					 errmsg("new row for relation \"%s\" violates check constraint \"%s\"",
							RelationGetRelationName(rel), failed)));
	}
}

/*
 * Check a modified tuple to see if we want to process its updated version
 * under READ COMMITTED rules.
 *
 * See backend/executor/README for some info about how this works.
 *
 *	estate - executor state data
 *	rti - rangetable index of table containing tuple
 *	*tid - t_ctid from the outdated tuple (ie, next updated version)
 *	priorXmax - t_xmax from the outdated tuple
 *	curCid - command ID of current command of my transaction
 *
 * *tid is also an output parameter: it's modified to hold the TID of the
 * latest version of the tuple (note this may be changed even on failure)
 *
 * Returns a slot containing the new candidate update/delete tuple, or
 * NULL if we determine we shouldn't process the row.
 */
TupleTableSlot *
EvalPlanQual(EState *estate, Index rti,
			 ItemPointer tid, TransactionId priorXmax, CommandId curCid)
{
	evalPlanQual *epq;
	EState	   *epqstate;
	Relation	relation;
	HeapTupleData tuple;
	HeapTuple	copyTuple = NULL;
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
			elog(ERROR, "could not find RowMark for RT index %u", rti);
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

		if (heap_fetch(relation, SnapshotDirty, &tuple, &buffer, true, NULL))
		{
			/*
			 * If xmin isn't what we're expecting, the slot must have been
			 * recycled and reused for an unrelated tuple.  This implies
			 * that the latest version of the row was deleted, so we need
			 * do nothing.  (Should be safe to examine xmin without getting
			 * buffer's content lock, since xmin never changes in an existing
			 * tuple.)
			 */
			if (!TransactionIdEquals(HeapTupleHeaderGetXmin(tuple.t_data),
									 priorXmax))
			{
				ReleaseBuffer(buffer);
				return NULL;
			}

			/* otherwise xmin should not be dirty... */
			if (TransactionIdIsValid(SnapshotDirty->xmin))
				elog(ERROR, "t_xmin is uncommitted in tuple to be updated");

			/*
			 * If tuple is being updated by other transaction then we have
			 * to wait for its commit/abort.
			 */
			if (TransactionIdIsValid(SnapshotDirty->xmax))
			{
				ReleaseBuffer(buffer);
				XactLockTableWait(SnapshotDirty->xmax);
				continue;		/* loop back to repeat heap_fetch */
			}

			/*
			 * If tuple was inserted by our own transaction, we have to check
			 * cmin against curCid: cmin >= curCid means our command cannot
			 * see the tuple, so we should ignore it.  Without this we are
			 * open to the "Halloween problem" of indefinitely re-updating
			 * the same tuple.  (We need not check cmax because
			 * HeapTupleSatisfiesDirty will consider a tuple deleted by
			 * our transaction dead, regardless of cmax.)  We just checked
			 * that priorXmax == xmin, so we can test that variable instead
			 * of doing HeapTupleHeaderGetXmin again.
			 */
			if (TransactionIdIsCurrentTransactionId(priorXmax) &&
				HeapTupleHeaderGetCmin(tuple.t_data) >= curCid)
			{
				ReleaseBuffer(buffer);
				return NULL;
			}

			/*
			 * We got tuple - now copy it for use by recheck query.
			 */
			copyTuple = heap_copytuple(&tuple);
			ReleaseBuffer(buffer);
			break;
		}

		/*
		 * If the referenced slot was actually empty, the latest version
		 * of the row must have been deleted, so we need do nothing.
		 */
		if (tuple.t_data == NULL)
		{
			ReleaseBuffer(buffer);
			return NULL;
		}

		/*
		 * As above, if xmin isn't what we're expecting, do nothing.
		 */
		if (!TransactionIdEquals(HeapTupleHeaderGetXmin(tuple.t_data),
								 priorXmax))
		{
			ReleaseBuffer(buffer);
			return NULL;
		}

		/*
		 * If we get here, the tuple was found but failed SnapshotDirty.
		 * Assuming the xmin is either a committed xact or our own xact
		 * (as it certainly should be if we're trying to modify the tuple),
		 * this must mean that the row was updated or deleted by either
		 * a committed xact or our own xact.  If it was deleted, we can
		 * ignore it; if it was updated then chain up to the next version
		 * and repeat the whole test.
		 *
		 * As above, it should be safe to examine xmax and t_ctid without
		 * the buffer content lock, because they can't be changing.
		 */
		if (ItemPointerEquals(&tuple.t_self, &tuple.t_data->t_ctid))
		{
			/* deleted, so forget about it */
			ReleaseBuffer(buffer);
			return NULL;
		}

		/* updated, so look at the updated row */
		tuple.t_self = tuple.t_data->t_ctid;
		/* updated row should have xmin matching this xmax */
		priorXmax = HeapTupleHeaderGetXmax(tuple.t_data);
		ReleaseBuffer(buffer);
		/* loop back to fetch next in chain */
	}

	/*
	 * For UPDATE/DELETE we have to return tid of actual row we're
	 * executing PQ for.
	 */
	*tid = tuple.t_self;

	/*
	 * Need to run a recheck subquery.	Find or create a PQ stack entry.
	 */
	epq = estate->es_evalPlanQual;
	endNode = true;

	if (epq != NULL && epq->rti == 0)
	{
		/* Top PQ stack entry is idle, so re-use it */
		Assert(!(estate->es_useEvalPlan) && epq->next == NULL);
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
		epq->estate->es_evTuple[rti - 1] != NULL)
	{
		do
		{
			evalPlanQual *oldepq;

			/* stop execution */
			EvalPlanQualStop(epq);
			/* pop previous PlanQual from the stack */
			oldepq = epq->next;
			Assert(oldepq && oldepq->rti != 0);
			/* push current PQ to freePQ stack */
			oldepq->free = epq;
			epq = oldepq;
			estate->es_evalPlanQual = epq;
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
			newepq = (evalPlanQual *) palloc0(sizeof(evalPlanQual));
			newepq->free = NULL;
			newepq->estate = NULL;
			newepq->planstate = NULL;
		}
		else
		{
			/* recycle previously used PlanQual */
			Assert(newepq->estate == NULL);
			epq->free = NULL;
		}
		/* push current PQ to the stack */
		newepq->next = epq;
		epq = newepq;
		estate->es_evalPlanQual = epq;
		epq->rti = rti;
		endNode = false;
	}

	Assert(epq->rti == rti);

	/*
	 * Ok - we're requested for the same RTE.  Unfortunately we still have
	 * to end and restart execution of the plan, because ExecReScan
	 * wouldn't ensure that upper plan nodes would reset themselves.  We
	 * could make that work if insertion of the target tuple were
	 * integrated with the Param mechanism somehow, so that the upper plan
	 * nodes know that their children's outputs have changed.
	 *
	 * Note that the stack of free evalPlanQual nodes is quite useless at the
	 * moment, since it only saves us from pallocing/releasing the
	 * evalPlanQual nodes themselves.  But it will be useful once we
	 * implement ReScan instead of end/restart for re-using PlanQual
	 * nodes.
	 */
	if (endNode)
	{
		/* stop execution */
		EvalPlanQualStop(epq);
	}

	/*
	 * Initialize new recheck query.
	 *
	 * Note: if we were re-using PlanQual plans via ExecReScan, we'd need to
	 * instead copy down changeable state from the top plan (including
	 * es_result_relation_info, es_junkFilter) and reset locally
	 * changeable state in the epq (including es_param_exec_vals,
	 * es_evTupleNull).
	 */
	EvalPlanQualStart(epq, estate, epq->next);

	/*
	 * free old RTE' tuple, if any, and store target tuple where
	 * relation's scan node will see it
	 */
	epqstate = epq->estate;
	if (epqstate->es_evTuple[rti - 1] != NULL)
		heap_freetuple(epqstate->es_evTuple[rti - 1]);
	epqstate->es_evTuple[rti - 1] = copyTuple;

	return EvalPlanQualNext(estate);
}

static TupleTableSlot *
EvalPlanQualNext(EState *estate)
{
	evalPlanQual *epq = estate->es_evalPlanQual;
	MemoryContext oldcontext;
	TupleTableSlot *slot;

	Assert(epq->rti != 0);

lpqnext:;
	oldcontext = MemoryContextSwitchTo(epq->estate->es_query_cxt);
	slot = ExecProcNode(epq->planstate);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * No more tuples for this PQ. Continue previous one.
	 */
	if (TupIsNull(slot))
	{
		evalPlanQual *oldepq;

		/* stop execution */
		EvalPlanQualStop(epq);
		/* pop old PQ from the stack */
		oldepq = epq->next;
		if (oldepq == NULL)
		{
			/* this is the first (oldest) PQ - mark as free */
			epq->rti = 0;
			estate->es_useEvalPlan = false;
			/* and continue Query execution */
			return (NULL);
		}
		Assert(oldepq->rti != 0);
		/* push current PQ to freePQ stack */
		oldepq->free = epq;
		epq = oldepq;
		estate->es_evalPlanQual = epq;
		goto lpqnext;
	}

	return (slot);
}

static void
EndEvalPlanQual(EState *estate)
{
	evalPlanQual *epq = estate->es_evalPlanQual;

	if (epq->rti == 0)			/* plans already shutdowned */
	{
		Assert(epq->next == NULL);
		return;
	}

	for (;;)
	{
		evalPlanQual *oldepq;

		/* stop execution */
		EvalPlanQualStop(epq);
		/* pop old PQ from the stack */
		oldepq = epq->next;
		if (oldepq == NULL)
		{
			/* this is the first (oldest) PQ - mark as free */
			epq->rti = 0;
			estate->es_useEvalPlan = false;
			break;
		}
		Assert(oldepq->rti != 0);
		/* push current PQ to freePQ stack */
		oldepq->free = epq;
		epq = oldepq;
		estate->es_evalPlanQual = epq;
	}
}

/*
 * Start execution of one level of PlanQual.
 *
 * This is a cut-down version of ExecutorStart(): we copy some state from
 * the top-level estate rather than initializing it fresh.
 */
static void
EvalPlanQualStart(evalPlanQual *epq, EState *estate, evalPlanQual *priorepq)
{
	EState	   *epqstate;
	int			rtsize;
	MemoryContext oldcontext;

	rtsize = length(estate->es_range_table);

	epq->estate = epqstate = CreateExecutorState();

	oldcontext = MemoryContextSwitchTo(epqstate->es_query_cxt);

	/*
	 * The epqstates share the top query's copy of unchanging state such
	 * as the snapshot, rangetable, result-rel info, and external Param
	 * info. They need their own copies of local state, including a tuple
	 * table, es_param_exec_vals, etc.
	 */
	epqstate->es_direction = ForwardScanDirection;
	epqstate->es_snapshot = estate->es_snapshot;
	epqstate->es_crosscheck_snapshot = estate->es_crosscheck_snapshot;
	epqstate->es_range_table = estate->es_range_table;
	epqstate->es_result_relations = estate->es_result_relations;
	epqstate->es_num_result_relations = estate->es_num_result_relations;
	epqstate->es_result_relation_info = estate->es_result_relation_info;
	epqstate->es_junkFilter = estate->es_junkFilter;
	epqstate->es_into_relation_descriptor = estate->es_into_relation_descriptor;
	epqstate->es_param_list_info = estate->es_param_list_info;
	if (estate->es_topPlan->nParamExec > 0)
		epqstate->es_param_exec_vals = (ParamExecData *)
			palloc0(estate->es_topPlan->nParamExec * sizeof(ParamExecData));
	epqstate->es_rowMark = estate->es_rowMark;
	epqstate->es_instrument = estate->es_instrument;
	epqstate->es_select_into = estate->es_select_into;
	epqstate->es_into_oids = estate->es_into_oids;
	epqstate->es_topPlan = estate->es_topPlan;

	/*
	 * Each epqstate must have its own es_evTupleNull state, but all the
	 * stack entries share es_evTuple state.  This allows sub-rechecks to
	 * inherit the value being examined by an outer recheck.
	 */
	epqstate->es_evTupleNull = (bool *) palloc0(rtsize * sizeof(bool));
	if (priorepq == NULL)
		/* first PQ stack entry */
		epqstate->es_evTuple = (HeapTuple *)
			palloc0(rtsize * sizeof(HeapTuple));
	else
		/* later stack entries share the same storage */
		epqstate->es_evTuple = priorepq->estate->es_evTuple;

	epqstate->es_tupleTable =
		ExecCreateTupleTable(estate->es_tupleTable->size);

	epq->planstate = ExecInitNode(estate->es_topPlan, epqstate);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * End execution of one level of PlanQual.
 *
 * This is a cut-down version of ExecutorEnd(); basically we want to do most
 * of the normal cleanup, but *not* close result relations (which we are
 * just sharing from the outer query).
 */
static void
EvalPlanQualStop(evalPlanQual *epq)
{
	EState	   *epqstate = epq->estate;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(epqstate->es_query_cxt);

	ExecEndNode(epq->planstate);

	ExecDropTupleTable(epqstate->es_tupleTable, true);
	epqstate->es_tupleTable = NULL;

	if (epqstate->es_evTuple[epq->rti - 1] != NULL)
	{
		heap_freetuple(epqstate->es_evTuple[epq->rti - 1]);
		epqstate->es_evTuple[epq->rti - 1] = NULL;
	}

	MemoryContextSwitchTo(oldcontext);

	FreeExecutorState(epqstate);

	epq->estate = NULL;
	epq->planstate = NULL;
}
