/*-------------------------------------------------------------------------
 *
 * execMain.c--
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
 *	ExecutorRun accepts 'feature' and 'count' arguments that specify whether
 *	the plan is to be executed forwards, backwards, and for how many tuples.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execMain.c,v 1.42 1998/02/13 03:26:38 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"
#include "miscadmin.h"

#include "executor/executor.h"
#include "executor/execdefs.h"
#include "executor/execdebug.h"
#include "executor/nodeIndexscan.h"
#include "utils/builtins.h"
#include "utils/palloc.h"
#include "utils/acl.h"
#include "utils/syscache.h"
#include "utils/tqual.h"
#include "parser/parsetree.h"	/* rt_fetch() */
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "commands/async.h"
/* #include "access/localam.h" */
#include "optimizer/var.h"
#include "access/heapam.h"
#include "catalog/heap.h"
#include "commands/trigger.h"



/* decls for local routines only used within this module */
static void ExecCheckPerms(CmdType operation, int resultRelation, List *rangeTable,
			   Query *parseTree);
static TupleDesc InitPlan(CmdType operation, Query *parseTree,
		 Plan *plan, EState *estate);
static void EndPlan(Plan *plan, EState *estate);
static TupleTableSlot * ExecutePlan(EState *estate, Plan *plan,
			Query *parseTree, CmdType operation,
			int numberTuples, ScanDirection direction,
			void (*printfunc) ());
static void ExecRetrieve(TupleTableSlot *slot, void (*printfunc) (),
									 EState *estate);
static void ExecAppend(TupleTableSlot *slot, ItemPointer tupleid,
		   EState *estate);
static void ExecDelete(TupleTableSlot *slot, ItemPointer tupleid,
		   EState *estate);
static void ExecReplace(TupleTableSlot *slot, ItemPointer tupleid,
			EState *estate, Query *parseTree);

/* end of local decls */

#ifdef QUERY_LIMIT
static int	queryLimit = ALL_TUPLES;

#undef ALL_TUPLES
#define ALL_TUPLES queryLimit

int ExecutorLimit(int limit);

int
ExecutorLimit(int limit)
{
	return queryLimit = limit;
}
#endif

/* ----------------------------------------------------------------
 *		ExecutorStart
 *
 *		This routine must be called at the beginning of any execution of any
 *		query plan
 *
 *		returns (AttrInfo*) which describes the attributes of the tuples to
 *		be returned by the query.
 *
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
		estate->es_param_exec_vals = (ParamExecData*) 
			palloc (queryDesc->plantree->nParamExec * sizeof (ParamExecData));
		memset (estate->es_param_exec_vals, 0 , queryDesc->plantree->nParamExec * sizeof (ParamExecData));
	}
	
	result = InitPlan(queryDesc->operation,
					  queryDesc->parsetree,
					  queryDesc->plantree,
					  estate);

	/*
	 * reset buffer refcount.  the current refcounts are saved and will be
	 * restored when ExecutorEnd is called
	 *
	 * this makes sure that when ExecutorRun's are called recursively as for
	 * postquel functions, the buffers pinned by one ExecutorRun will not
	 * be unpinned by another ExecutorRun.
	 */
	BufferRefCountReset(estate->es_refcount);

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
 *		the different features supported are:
 *			 EXEC_RUN:	retrieve all tuples in the forward direction
 *			 EXEC_FOR:	retrieve 'count' number of tuples in the forward dir
 *			 EXEC_BACK: retrieve 'count' number of tuples in the backward dir
 *			 EXEC_RETONE: return one tuple but don't 'retrieve' it
 *						   used in postquel function processing
 *
 *
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecutorRun(QueryDesc *queryDesc, EState *estate, int feature, int count)
{
	CmdType		operation;
	Query	   *parseTree;
	Plan	   *plan;
	TupleTableSlot *result;
	CommandDest dest;
	void		(*destination) ();

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(queryDesc != NULL);

	/* ----------------
	 *	extract information from the query descriptor
	 *	and the query feature.
	 * ----------------
	 */
	operation = queryDesc->operation;
	parseTree = queryDesc->parsetree;
	plan = queryDesc->plantree;
	dest = queryDesc->dest;
	destination = (void (*) ()) DestToFunction(dest);
	estate->es_processed = 0;
	estate->es_lastoid = InvalidOid;

	switch (feature)
	{

		case EXEC_RUN:
			result = ExecutePlan(estate,
								 plan,
								 parseTree,
								 operation,
								 ALL_TUPLES,
								 ForwardScanDirection,
								 destination);
			break;
		case EXEC_FOR:
			result = ExecutePlan(estate,
								 plan,
								 parseTree,
								 operation,
								 count,
								 ForwardScanDirection,
								 destination);
			break;

			/* ----------------
			 *		retrieve next n "backward" tuples
			 * ----------------
			 */
		case EXEC_BACK:
			result = ExecutePlan(estate,
								 plan,
								 parseTree,
								 operation,
								 count,
								 BackwardScanDirection,
								 destination);
			break;

			/* ----------------
			 *		return one tuple but don't "retrieve" it.
			 *		(this is used by the rule manager..) -cim 9/14/89
			 * ----------------
			 */
		case EXEC_RETONE:
			result = ExecutePlan(estate,
								 plan,
								 parseTree,
								 operation,
								 ONE_TUPLE,
								 ForwardScanDirection,
								 destination);
			break;
		default:
			result = NULL;
			elog(DEBUG, "ExecutorRun: Unknown feature %d", feature);
			break;
	}

	return result;
}

/* ----------------------------------------------------------------
 *		ExecutorEnd
 *
 *		This routine must be called at the end of any execution of any
 *		query plan
 *
 *		returns (AttrInfo*) which describes the attributes of the tuples to
 *		be returned by the query.
 *
 * ----------------------------------------------------------------
 */
void
ExecutorEnd(QueryDesc *queryDesc, EState *estate)
{
	/* sanity checks */
	Assert(queryDesc != NULL);

	EndPlan(queryDesc->plantree, estate);

	/* restore saved refcounts. */
	BufferRefCountRestore(estate->es_refcount);
}

/* ===============================================================
 * ===============================================================
						 static routines follow
 * ===============================================================
 * ===============================================================
 */

static void
ExecCheckPerms(CmdType operation,
			   int resultRelation,
			   List *rangeTable,
			   Query *parseTree)
{
	int			i = 1;
	Oid			relid;
	HeapTuple	htp;
	List	   *lp;
	List	   *qvars,
			   *tvars;
	int32		ok = 1,
				aclcheck_result = -1;
	char	   *opstr;
	NameData	rname;
	char	   *userName;

#define CHECK(MODE)		pg_aclcheck(rname.data, userName, MODE)

	userName = GetPgUserName();

	foreach(lp, rangeTable)
	{
		RangeTblEntry *rte = lfirst(lp);

		relid = rte->relid;
		htp = SearchSysCacheTuple(RELOID,
								  ObjectIdGetDatum(relid),
								  0, 0, 0);
		if (!HeapTupleIsValid(htp))
			elog(ERROR, "ExecCheckPerms: bogus RT relid: %d",
				 relid);
		StrNCpy(rname.data,
				((Form_pg_class) GETSTRUCT(htp))->relname.data,
				NAMEDATALEN);
		if (i == resultRelation)
		{						/* this is the result relation */
			qvars = pull_varnos(parseTree->qual);
			tvars = pull_varnos((Node *) parseTree->targetList);
			if (intMember(resultRelation, qvars) ||
				intMember(resultRelation, tvars))
			{
				/* result relation is scanned */
				ok = ((aclcheck_result = CHECK(ACL_RD)) == ACLCHECK_OK);
				opstr = "read";
				if (!ok)
					break;
			}
			switch (operation)
			{
				case CMD_INSERT:
					ok = ((aclcheck_result = CHECK(ACL_AP)) == ACLCHECK_OK) ||
						((aclcheck_result = CHECK(ACL_WR)) == ACLCHECK_OK);
					opstr = "append";
					break;
				case CMD_NOTIFY:		/* what does this mean?? -- jw,
										 * 1/6/94 */
				case CMD_DELETE:
				case CMD_UPDATE:
					ok = ((aclcheck_result = CHECK(ACL_WR)) == ACLCHECK_OK);
					opstr = "write";
					break;
				default:
					elog(ERROR, "ExecCheckPerms: bogus operation %d",
						 operation);
			}
		}
		else
		{
			/* XXX NOTIFY?? */
			ok = ((aclcheck_result = CHECK(ACL_RD)) == ACLCHECK_OK);
			opstr = "read";
		}
		if (!ok)
			break;
		++i;
	}
	if (!ok)
	{
		elog(ERROR, "%s: %s", rname.data, aclcheck_error_strings[aclcheck_result]);
	}
}


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
	int			resultRelation;
	Relation	intoRelationDesc;

	TupleDesc	tupType;
	List	   *targetList;
	int			len;

	/* ----------------
	 *	get information from query descriptor
	 * ----------------
	 */
	rangeTable = parseTree->rtable;
	resultRelation = parseTree->resultRelation;

	/* ----------------
	 *	initialize the node's execution state
	 * ----------------
	 */
	estate->es_range_table = rangeTable;

	/* ----------------
	 *	initialize the BaseId counter so node base_id's
	 *	are assigned correctly.  Someday baseid's will have to
	 *	be stored someplace other than estate because they
	 *	should be unique per query planned.
	 * ----------------
	 */
	estate->es_BaseId = 1;

	/* ----------------
	 *	initialize result relation stuff
	 * ----------------
	 */

	if (resultRelation != 0 && operation != CMD_SELECT)
	{
		/* ----------------
		 *	  if we have a result relation, open it and

		 *	  initialize the result relation info stuff.
		 * ----------------
		 */
		RelationInfo *resultRelationInfo;
		Index		resultRelationIndex;
		RangeTblEntry *rtentry;
		Oid			resultRelationOid;
		Relation	resultRelationDesc;

		resultRelationIndex = resultRelation;
		rtentry = rt_fetch(resultRelationIndex, rangeTable);
		resultRelationOid = rtentry->relid;
		resultRelationDesc = heap_open(resultRelationOid);

		if (resultRelationDesc->rd_rel->relkind == RELKIND_SEQUENCE)
			elog(ERROR, "You can't change sequence relation %s",
				 resultRelationDesc->rd_rel->relname.data);

		/*
		 * Write-lock the result relation right away: if the relation is
		 * used in a subsequent scan, we won't have to elevate the
		 * read-lock set by heap_beginscan to a write-lock (needed by
		 * heap_insert, heap_delete and heap_replace). This will hopefully
		 * prevent some deadlocks.	- 01/24/94
		 */
		RelationSetLockForWrite(resultRelationDesc);

		resultRelationInfo = makeNode(RelationInfo);
		resultRelationInfo->ri_RangeTableIndex = resultRelationIndex;
		resultRelationInfo->ri_RelationDesc = resultRelationDesc;
		resultRelationInfo->ri_NumIndices = 0;
		resultRelationInfo->ri_IndexRelationDescs = NULL;
		resultRelationInfo->ri_IndexRelationInfo = NULL;

		/* ----------------
		 *	open indices on result relation and save descriptors
		 *	in the result relation information..
		 * ----------------
		 */
		ExecOpenIndices(resultRelationOid, resultRelationInfo);

		estate->es_result_relation_info = resultRelationInfo;
	}
	else
	{
		/* ----------------
		 *		if no result relation, then set state appropriately
		 * ----------------
		 */
		estate->es_result_relation_info = NULL;
	}

#ifndef NO_SECURITY
	ExecCheckPerms(operation, resultRelation, rangeTable, parseTree);
#endif

	/* ----------------
	 *	  initialize the executor "tuple" table.
	 * ----------------
	 */
	{
		int			nSlots = ExecCountSlotsNode(plan);
		TupleTable	tupleTable = ExecCreateTupleTable(nSlots + 10);		/* why add ten? - jolly */

		estate->es_tupleTable = tupleTable;
	}

	/* ----------------
	 *	   initialize the private state information for
	 *	   all the nodes in the query tree.  This opens
	 *	   files, allocates storage and leaves us ready
	 *	   to start processing tuples..
	 * ----------------
	 */
	ExecInitNode(plan, estate, NULL);

	/* ----------------
	 *	   get the tuple descriptor describing the type
	 *	   of tuples to return.. (this is especially important
	 *	   if we are creating a relation with "retrieve into")
	 * ----------------
	 */
	tupType = ExecGetTupType(plan);		/* tuple descriptor */
	targetList = plan->targetlist;
	len = ExecTargetListLength(targetList);		/* number of attributes */

	/* ----------------
	 *	  now that we have the target list, initialize the junk filter
	 *	  if this is a REPLACE or a DELETE query.
	 *	  We also init the junk filter if this is an append query
	 *	  (there might be some rule lock info there...)
	 *	  NOTE: in the future we might want to initialize the junk
	 *	  filter for all queries.
	 * ----------------
	 */
	if (operation == CMD_UPDATE || operation == CMD_DELETE ||
		operation == CMD_INSERT)
	{

		JunkFilter *j = (JunkFilter *) ExecInitJunkFilter(targetList);

		estate->es_junkFilter = j;
	}
	else
		estate->es_junkFilter = NULL;

	/* ----------------
	 *	initialize the "into" relation
	 * ----------------
	 */
	intoRelationDesc = (Relation) NULL;

	if (operation == CMD_SELECT)
	{
		char	   *intoName;
		Oid			intoRelationId;
		TupleDesc	tupdesc;

		if (!parseTree->isPortal)
		{

			/*
			 * a select into table
			 */
			if (parseTree->into != NULL)
			{
				/* ----------------
				 *	create the "into" relation
				 * ----------------
				 */
				intoName = parseTree->into;

				/*
				 * have to copy tupType to get rid of constraints
				 */
				tupdesc = CreateTupleDescCopy(tupType);

				intoRelationId = heap_create_with_catalog(intoName, tupdesc);

				FreeTupleDesc(tupdesc);

				/* ----------------
				 *	XXX rather than having to call setheapoverride(true)
				 *		and then back to false, we should change the
				 *		arguments to heap_open() instead..
				 * ----------------
				 */
				setheapoverride(true);

				intoRelationDesc = heap_open(intoRelationId);

				setheapoverride(false);
			}
		}
	}

	estate->es_into_relation_descriptor = intoRelationDesc;

	/* ----------------
	 *	return the type information..
	 * ----------------
	 */
/*
	attinfo = (AttrInfo *)palloc(sizeof(AttrInfo));
	attinfo->numAttr = len;
	attinfo->attrs = tupType->attrs;
*/

	return tupType;
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
	RelationInfo *resultRelationInfo;
	Relation	intoRelationDesc;

	/* ----------------
	 *	get information from state
	 * ----------------
	 */
	resultRelationInfo = estate->es_result_relation_info;
	intoRelationDesc = estate->es_into_relation_descriptor;

	/* ----------------
	 *	 shut down the query
	 * ----------------
	 */
	ExecEndNode(plan, plan);

	/* ----------------
	 *	  destroy the executor "tuple" table.
	 * ----------------
	 */
	{
		TupleTable	tupleTable = (TupleTable) estate->es_tupleTable;

		ExecDestroyTupleTable(tupleTable, true);		/* was missing last arg */
		estate->es_tupleTable = NULL;
	}

	/* ----------------
	 *	 close the result relations if necessary
	 * ----------------
	 */
	if (resultRelationInfo != NULL)
	{
		Relation	resultRelationDesc;

		resultRelationDesc = resultRelationInfo->ri_RelationDesc;
		heap_close(resultRelationDesc);

		/* ----------------
		 *	close indices on the result relation
		 * ----------------
		 */
		ExecCloseIndices(resultRelationInfo);
	}

	/* ----------------
	 *	 close the "into" relation if necessary
	 * ----------------
	 */
	if (intoRelationDesc != NULL)
	{
		heap_close(intoRelationDesc);
	}
}

/* ----------------------------------------------------------------
 *		ExecutePlan
 *
 *		processes the query plan to retrieve 'tupleCount' tuples in the
 *		direction specified.
 *		Retrieves all tuples if tupleCount is 0
 *
 *		result is either a slot containing a tuple in the case
 *		of a RETRIEVE or NULL otherwise.
 *
 * ----------------------------------------------------------------
 */

/* the ctid attribute is a 'junk' attribute that is removed before the
   user can see it*/

static TupleTableSlot *
ExecutePlan(EState *estate,
			Plan *plan,
			Query *parseTree,
			CmdType operation,
			int numberTuples,
			ScanDirection direction,
			void (*printfunc) ())
{
	JunkFilter *junkfilter;

	TupleTableSlot *slot;
	ItemPointer tupleid = NULL;
	ItemPointerData tuple_ctid;
	int			current_tuple_count;
	TupleTableSlot *result;

	/* ----------------
	 *	initialize local variables
	 * ----------------
	 */
	slot = NULL;
	current_tuple_count = 0;
	result = NULL;

	/* ----------------
	 *	Set the direction.
	 * ----------------
	 */
	estate->es_direction = direction;

	/* ----------------
	 *	Loop until we've processed the proper number
	 *	of tuples from the plan..
	 * ----------------
	 */

	for (;;)
	{
		if (operation != CMD_NOTIFY)
		{
			/* ----------------
			 *	Execute the plan and obtain a tuple
			 * ----------------
			 */
			/* at the top level, the parent of a plan (2nd arg) is itself */
			slot = ExecProcNode(plan, plan);

			/* ----------------
			 *	if the tuple is null, then we assume
			 *	there is nothing more to process so
			 *	we just return null...
			 * ----------------
			 */
			if (TupIsNull(slot))
			{
				result = NULL;
				break;
			}
		}

		/* ----------------
		 *		if we have a junk filter, then project a new
		 *		tuple with the junk removed.
		 *
		 *		Store this new "clean" tuple in the place of the
		 *		original tuple.
		 *
		 *		Also, extract all the junk ifnormation we need.
		 * ----------------
		 */
		if ((junkfilter = estate->es_junkFilter) != (JunkFilter *) NULL)
		{
			Datum		datum;

/*			NameData	attrName; */
			HeapTuple	newTuple;
			bool		isNull;

			/* ---------------
			 * extract the 'ctid' junk attribute.
			 * ---------------
			 */
			if (operation == CMD_UPDATE || operation == CMD_DELETE)
			{
				if (!ExecGetJunkAttribute(junkfilter,
										  slot,
										  "ctid",
										  &datum,
										  &isNull))
					elog(ERROR, "ExecutePlan: NO (junk) `ctid' was found!");

				if (isNull)
					elog(ERROR, "ExecutePlan: (junk) `ctid' is NULL!");

				tupleid = (ItemPointer) DatumGetPointer(datum);
				tuple_ctid = *tupleid;	/* make sure we don't free the
										 * ctid!! */
				tupleid = &tuple_ctid;
			}

			/* ---------------
			 * Finally create a new "clean" tuple with all junk attributes
			 * removed
			 * ---------------
			 */
			newTuple = ExecRemoveJunk(junkfilter, slot);

			slot = ExecStoreTuple(newTuple,		/* tuple to store */
								  slot, /* destination slot */
								  InvalidBuffer,		/* this tuple has no
														 * buffer */
								  true);		/* tuple should be pfreed */
		}						/* if (junkfilter... */

		/* ----------------
		 *		now that we have a tuple, do the appropriate thing
		 *		with it.. either return it to the user, add
		 *		it to a relation someplace, delete it from a
		 *		relation, or modify some of it's attributes.
		 * ----------------
		 */

		switch (operation)
		{
			case CMD_SELECT:
				ExecRetrieve(slot,		/* slot containing tuple */
							 printfunc, /* print function */
							 estate);	/* */
				result = slot;
				break;

			case CMD_INSERT:
				ExecAppend(slot, tupleid, estate);
				result = NULL;
				break;

			case CMD_DELETE:
				ExecDelete(slot, tupleid, estate);
				result = NULL;
				break;

			case CMD_UPDATE:
				ExecReplace(slot, tupleid, estate, parseTree);
				result = NULL;
				break;

				/*
				 * Total hack. I'm ignoring any accessor functions for
				 * Relation, RelationTupleForm, NameData. Assuming that
				 * NameData.data has offset 0.
				 */
			case CMD_NOTIFY:
				{
					RelationInfo *rInfo = estate->es_result_relation_info;
					Relation	rDesc = rInfo->ri_RelationDesc;

					Async_Notify(rDesc->rd_rel->relname.data);
					result = NULL;
					current_tuple_count = 0;
					numberTuples = 1;
					elog(DEBUG, "ExecNotify %s", &rDesc->rd_rel->relname);
				}
				break;

			default:
				elog(DEBUG, "ExecutePlan: unknown operation in queryDesc");
				result = NULL;
				break;
		}
		/* ----------------
		 *		check our tuple count.. if we've returned the
		 *		proper number then return, else loop again and
		 *		process more tuples..
		 * ----------------
		 */
		current_tuple_count += 1;
		if (numberTuples == current_tuple_count)
			break;
	}

	/* ----------------
	 *	here, result is either a slot containing a tuple in the case
	 *	of a RETRIEVE or NULL otherwise.
	 * ----------------
	 */
	return result;
}

/* ----------------------------------------------------------------
 *		ExecRetrieve
 *
 *		RETRIEVEs are easy.. we just pass the tuple to the appropriate
 *		print function.  The only complexity is when we do a
 *		"retrieve into", in which case we insert the tuple into
 *		the appropriate relation (note: this is a newly created relation
 *		so we don't need to worry about indices or locks.)
 * ----------------------------------------------------------------
 */
static void
ExecRetrieve(TupleTableSlot *slot,
			 void (*printfunc) (),
			 EState *estate)
{
	HeapTuple	tuple;
	TupleDesc	attrtype;

	/* ----------------
	 *	get the heap tuple out of the tuple table slot
	 * ----------------
	 */
	tuple = slot->val;
	attrtype = slot->ttc_tupleDescriptor;

	/* ----------------
	 *	insert the tuple into the "into relation"
	 * ----------------
	 */
	if (estate->es_into_relation_descriptor != NULL)
	{
		heap_insert(estate->es_into_relation_descriptor, tuple);
		IncrAppended();
	}

	/* ----------------
	 *	send the tuple to the front end (or the screen)
	 * ----------------
	 */
	(*printfunc) (tuple, attrtype);
	IncrRetrieved();
	(estate->es_processed)++;
}

/* ----------------------------------------------------------------
 *		ExecAppend
 *
 *		APPENDs are trickier.. we have to insert the tuple into
 *		the base relation and insert appropriate tuples into the
 *		index relations.
 * ----------------------------------------------------------------
 */

static void
ExecAppend(TupleTableSlot *slot,
		   ItemPointer tupleid,
		   EState *estate)
{
	HeapTuple	tuple;
	RelationInfo *resultRelationInfo;
	Relation	resultRelationDesc;
	int			numIndices;
	Oid			newId;

	/* ----------------
	 *	get the heap tuple out of the tuple table slot
	 * ----------------
	 */
	tuple = slot->val;

	/* ----------------
	 *	get information on the result relation
	 * ----------------
	 */
	resultRelationInfo = estate->es_result_relation_info;
	resultRelationDesc = resultRelationInfo->ri_RelationDesc;

	/* ----------------
	 *	have to add code to preform unique checking here.
	 *	cim -12/1/89
	 * ----------------
	 */

	/* BEFORE ROW INSERT Triggers */
	if (resultRelationDesc->trigdesc &&
	resultRelationDesc->trigdesc->n_before_row[TRIGGER_EVENT_INSERT] > 0)
	{
		HeapTuple	newtuple;

		newtuple = ExecBRInsertTriggers(resultRelationDesc, tuple);

		if (newtuple == NULL)	/* "do nothing" */
			return;

		if (newtuple != tuple)	/* modified by Trigger(s) */
		{
			Assert(slot->ttc_shouldFree);
			pfree(tuple);
			slot->val = tuple = newtuple;
		}
	}

	/* ----------------
	 * Check the constraints of a tuple
	 * ----------------
	 */

	if (resultRelationDesc->rd_att->constr)
	{
		HeapTuple	newtuple;

		newtuple = ExecConstraints("ExecAppend", resultRelationDesc, tuple);

		if (newtuple != tuple)	/* modified by DEFAULT */
		{
			Assert(slot->ttc_shouldFree);
			pfree(tuple);
			slot->val = tuple = newtuple;
		}
	}

	/* ----------------
	 *	insert the tuple
	 * ----------------
	 */
	newId = heap_insert(resultRelationDesc,		/* relation desc */
						tuple); /* heap tuple */
	IncrAppended();

	/* ----------------
	 *	process indices
	 *
	 *	Note: heap_insert adds a new tuple to a relation.  As a side
	 *	effect, the tupleid of the new tuple is placed in the new
	 *	tuple's t_ctid field.
	 * ----------------
	 */
	numIndices = resultRelationInfo->ri_NumIndices;
	if (numIndices > 0)
	{
		ExecInsertIndexTuples(slot, &(tuple->t_ctid), estate, false);
	}
	(estate->es_processed)++;
	estate->es_lastoid = newId;

	/* AFTER ROW INSERT Triggers */
	if (resultRelationDesc->trigdesc &&
	 resultRelationDesc->trigdesc->n_after_row[TRIGGER_EVENT_INSERT] > 0)
		ExecARInsertTriggers(resultRelationDesc, tuple);
}

/* ----------------------------------------------------------------
 *		ExecDelete
 *
 *		DELETE is like append, we delete the tuple and its
 *		index tuples.
 * ----------------------------------------------------------------
 */
static void
ExecDelete(TupleTableSlot *slot,
		   ItemPointer tupleid,
		   EState *estate)
{
	RelationInfo *resultRelationInfo;
	Relation	resultRelationDesc;

	/* ----------------
	 *	get the result relation information
	 * ----------------
	 */
	resultRelationInfo = estate->es_result_relation_info;
	resultRelationDesc = resultRelationInfo->ri_RelationDesc;

	/* BEFORE ROW DELETE Triggers */
	if (resultRelationDesc->trigdesc &&
	resultRelationDesc->trigdesc->n_before_row[TRIGGER_EVENT_DELETE] > 0)
	{
		bool		dodelete;

		dodelete = ExecBRDeleteTriggers(resultRelationDesc, tupleid);

		if (!dodelete)			/* "do nothing" */
			return;
	}

	/* ----------------
	 *	delete the tuple
	 * ----------------
	 */
	if (heap_delete(resultRelationDesc, /* relation desc */
					tupleid))	/* item pointer to tuple */
		return;

	IncrDeleted();
	(estate->es_processed)++;

	/* ----------------
	 *	Note: Normally one would think that we have to
	 *		  delete index tuples associated with the
	 *		  heap tuple now..
	 *
	 *		  ... but in POSTGRES, we have no need to do this
	 *		  because the vacuum daemon automatically
	 *		  opens an index scan and deletes index tuples
	 *		  when it finds deleted heap tuples. -cim 9/27/89
	 * ----------------
	 */

	/* AFTER ROW DELETE Triggers */
	if (resultRelationDesc->trigdesc &&
	 resultRelationDesc->trigdesc->n_after_row[TRIGGER_EVENT_DELETE] > 0)
		ExecARDeleteTriggers(resultRelationDesc, tupleid);

}

/* ----------------------------------------------------------------
 *		ExecReplace
 *
 *		note: we can't run replace queries with transactions
 *		off because replaces are actually appends and our
 *		scan will mistakenly loop forever, replacing the tuple
 *		it just appended..	This should be fixed but until it
 *		is, we don't want to get stuck in an infinite loop
 *		which corrupts your database..
 * ----------------------------------------------------------------
 */
static void
ExecReplace(TupleTableSlot *slot,
			ItemPointer tupleid,
			EState *estate,
			Query *parseTree)
{
	HeapTuple	tuple;
	RelationInfo *resultRelationInfo;
	Relation	resultRelationDesc;
	int			numIndices;

	/* ----------------
	 *	abort the operation if not running transactions
	 * ----------------
	 */
	if (IsBootstrapProcessingMode())
	{
		elog(DEBUG, "ExecReplace: replace can't run without transactions");
		return;
	}

	/* ----------------
	 *	get the heap tuple out of the tuple table slot
	 * ----------------
	 */
	tuple = slot->val;

	/* ----------------
	 *	get the result relation information
	 * ----------------
	 */
	resultRelationInfo = estate->es_result_relation_info;
	resultRelationDesc = resultRelationInfo->ri_RelationDesc;

	/* ----------------
	 *	have to add code to preform unique checking here.
	 *	in the event of unique tuples, this becomes a deletion
	 *	of the original tuple affected by the replace.
	 *	cim -12/1/89
	 * ----------------
	 */

	/* BEFORE ROW UPDATE Triggers */
	if (resultRelationDesc->trigdesc &&
	resultRelationDesc->trigdesc->n_before_row[TRIGGER_EVENT_UPDATE] > 0)
	{
		HeapTuple	newtuple;

		newtuple = ExecBRUpdateTriggers(resultRelationDesc, tupleid, tuple);

		if (newtuple == NULL)	/* "do nothing" */
			return;

		if (newtuple != tuple)	/* modified by Trigger(s) */
		{
			Assert(slot->ttc_shouldFree);
			pfree(tuple);
			slot->val = tuple = newtuple;
		}
	}

	/* ----------------
	 * Check the constraints of a tuple
	 * ----------------
	 */

	if (resultRelationDesc->rd_att->constr)
	{
		HeapTuple	newtuple;

		newtuple = ExecConstraints("ExecReplace", resultRelationDesc, tuple);

		if (newtuple != tuple)	/* modified by DEFAULT */
		{
			Assert(slot->ttc_shouldFree);
			pfree(tuple);
			slot->val = tuple = newtuple;
		}
	}

	/* ----------------
	 *	replace the heap tuple
	 *
	 * Don't want to continue if our heap_replace didn't actually
	 * do a replace. This would be the case if heap_replace
	 * detected a non-functional update. -kw 12/30/93
	 * ----------------
	 */
	if (heap_replace(resultRelationDesc,		/* relation desc */
					 tupleid,	/* item ptr of tuple to replace */
					 tuple))
	{							/* replacement heap tuple */
		return;
	}

	IncrReplaced();
	(estate->es_processed)++;

	/* ----------------
	 *	Note: instead of having to update the old index tuples
	 *		  associated with the heap tuple, all we do is form
	 *		  and insert new index tuples..  This is because
	 *		  replaces are actually deletes and inserts and
	 *		  index tuple deletion is done automagically by
	 *		  the vaccuum deamon.. All we do is insert new
	 *		  index tuples.  -cim 9/27/89
	 * ----------------
	 */

	/* ----------------
	 *	process indices
	 *
	 *	heap_replace updates a tuple in the base relation by invalidating
	 *	it and then appending a new tuple to the relation.	As a side
	 *	effect, the tupleid of the new tuple is placed in the new
	 *	tuple's t_ctid field.  So we now insert index tuples using
	 *	the new tupleid stored there.
	 * ----------------
	 */

	numIndices = resultRelationInfo->ri_NumIndices;
	if (numIndices > 0)
	{
		ExecInsertIndexTuples(slot, &(tuple->t_ctid), estate, true);
	}

	/* AFTER ROW UPDATE Triggers */
	if (resultRelationDesc->trigdesc &&
	 resultRelationDesc->trigdesc->n_after_row[TRIGGER_EVENT_UPDATE] > 0)
		ExecARUpdateTriggers(resultRelationDesc, tupleid, tuple);
}

#if 0
static HeapTuple
ExecAttrDefault(Relation rel, HeapTuple tuple)
{
	int			ndef = rel->rd_att->constr->num_defval;
	AttrDefault *attrdef = rel->rd_att->constr->defval;
	ExprContext *econtext = makeNode(ExprContext);
	HeapTuple	newtuple;
	Node	   *expr;
	bool		isnull;
	bool		isdone;
	Datum		val;
	Datum	   *replValue = NULL;
	char	   *replNull = NULL;
	char	   *repl = NULL;
	int			i;

	econtext->ecxt_scantuple = NULL;	/* scan tuple slot */
	econtext->ecxt_innertuple = NULL;	/* inner tuple slot */
	econtext->ecxt_outertuple = NULL;	/* outer tuple slot */
	econtext->ecxt_relation = NULL;		/* relation */
	econtext->ecxt_relid = 0;	/* relid */
	econtext->ecxt_param_list_info = NULL;	/* param list info */
	econtext->ecxt_param_exec_vals = NULL;	/* exec param values */
	econtext->ecxt_range_table = NULL;	/* range table */
	for (i = 0; i < ndef; i++)
	{
		if (!heap_attisnull(tuple, attrdef[i].adnum))
			continue;
		expr = (Node *) stringToNode(attrdef[i].adbin);

		val = ExecEvalExpr(expr, econtext, &isnull, &isdone);

		pfree(expr);

		if (isnull)
			continue;

		if (repl == NULL)
		{
			repl = (char *) palloc(rel->rd_att->natts * sizeof(char));
			replNull = (char *) palloc(rel->rd_att->natts * sizeof(char));
			replValue = (Datum *) palloc(rel->rd_att->natts * sizeof(Datum));
			MemSet(repl, ' ', rel->rd_att->natts * sizeof(char));
		}

		repl[attrdef[i].adnum - 1] = 'r';
		replNull[attrdef[i].adnum - 1] = ' ';
		replValue[attrdef[i].adnum - 1] = val;

	}

	pfree(econtext);

	if (repl == NULL)
		return (tuple);

	newtuple = heap_modifytuple(tuple, InvalidBuffer, rel, replValue, replNull, repl);

	pfree(repl);
	pfree(replNull);
	pfree(replValue);

	return (newtuple);

}
#endif

static char *
ExecRelCheck(Relation rel, HeapTuple tuple)
{
	int			ncheck = rel->rd_att->constr->num_check;
	ConstrCheck *check = rel->rd_att->constr->check;
	ExprContext *econtext = makeNode(ExprContext);
	TupleTableSlot *slot = makeNode(TupleTableSlot);
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	List	   *rtlist;
	List	   *qual;
	bool		res;
	int			i;

	slot->val = tuple;
	slot->ttc_shouldFree = false;
	slot->ttc_descIsNew = true;
	slot->ttc_tupleDescriptor = rel->rd_att;
	slot->ttc_buffer = InvalidBuffer;
	slot->ttc_whichplan = -1;
	rte->relname = nameout(&(rel->rd_rel->relname));
	rte->refname = rte->relname;
	rte->relid = rel->rd_id;
	rte->inh = false;
	rte->inFromCl = true;
	rtlist = lcons(rte, NIL);
	econtext->ecxt_scantuple = slot;	/* scan tuple slot */
	econtext->ecxt_innertuple = NULL;	/* inner tuple slot */
	econtext->ecxt_outertuple = NULL;	/* outer tuple slot */
	econtext->ecxt_relation = rel;		/* relation */
	econtext->ecxt_relid = 0;	/* relid */
	econtext->ecxt_param_list_info = NULL;		/* param list info */
	econtext->ecxt_param_exec_vals = NULL;		/* exec param values */
	econtext->ecxt_range_table = rtlist;		/* range table */

	for (i = 0; i < ncheck; i++)
	{
		qual = (List *) stringToNode(check[i].ccbin);

		res = ExecQual(qual, econtext);

		pfree(qual);

		if (!res)
			return (check[i].ccname);
	}

	pfree(slot);
	pfree(rte->relname);
	pfree(rte);
	pfree(rtlist);
	pfree(econtext);

	return ((char *) NULL);

}

HeapTuple
ExecConstraints(char *caller, Relation rel, HeapTuple tuple)
{
	HeapTuple	newtuple = tuple;

	Assert(rel->rd_att->constr);

#if 0
	if (rel->rd_att->constr->num_defval > 0)
		newtuple = tuple = ExecAttrDefault(rel, tuple);
#endif

	if (rel->rd_att->constr->has_not_null)
	{
		int			attrChk;

		for (attrChk = 1; attrChk <= rel->rd_att->natts; attrChk++)
		{
			if (rel->rd_att->attrs[attrChk - 1]->attnotnull && heap_attisnull(tuple, attrChk))
				elog(ERROR, "%s: Fail to add null value in not null attribute %s",
				  caller, rel->rd_att->attrs[attrChk - 1]->attname.data);
		}
	}

	if (rel->rd_att->constr->num_check > 0)
	{
		char	   *failed;

		if ((failed = ExecRelCheck(rel, tuple)) != NULL)
			elog(ERROR, "%s: rejected due to CHECK constraint %s", caller, failed);
	}

	return (newtuple);
}
