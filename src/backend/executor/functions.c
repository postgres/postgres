/*-------------------------------------------------------------------------
 *
 * functions.c
 *	  Routines to handle functions called from the executor
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/functions.c,v 1.35 2000/06/28 03:31:33 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "executor/execdefs.h"
#include "executor/executor.h"
#include "executor/functions.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/datum.h"


typedef enum
{
	F_EXEC_START, F_EXEC_RUN, F_EXEC_DONE
} ExecStatus;

typedef struct local_es
{
	QueryDesc  *qd;
	EState	   *estate;
	struct local_es *next;
	ExecStatus	status;
} execution_state;

#define LAST_POSTQUEL_COMMAND(es) ((es)->next == (execution_state *) NULL)

/* non-export function prototypes */
static TupleDesc postquel_start(execution_state *es);
static execution_state *init_execution_state(FunctionCachePtr fcache);
static TupleTableSlot *postquel_getnext(execution_state *es);
static void postquel_end(execution_state *es);
static void postquel_sub_params(execution_state *es, FunctionCallInfo fcinfo);
static Datum postquel_execute(execution_state *es,
							  FunctionCallInfo fcinfo,
							  FunctionCachePtr fcache,
							  List *func_tlist);


Datum
ProjectAttribute(TupleDesc TD,
				 TargetEntry *tlist,
				 HeapTuple tup,
				 bool *isnullP)
{
	Datum		val,
				valueP;
	Var		   *attrVar = (Var *) tlist->expr;
	AttrNumber	attrno = attrVar->varattno;

	val = heap_getattr(tup, attrno, TD, isnullP);
	if (*isnullP)
		return (Datum) NULL;

	valueP = datumCopy(val,
					   TD->attrs[attrno - 1]->atttypid,
					   TD->attrs[attrno - 1]->attbyval,
					   (Size) TD->attrs[attrno - 1]->attlen);
	return valueP;
}

static execution_state *
init_execution_state(FunctionCachePtr fcache)
{
	execution_state *newes;
	execution_state *nextes;
	execution_state *preves;
	List	   *queryTree_list,
			   *qtl_item;
	int			nargs = fcache->nargs;

	newes = (execution_state *) palloc(sizeof(execution_state));
	nextes = newes;
	preves = (execution_state *) NULL;

	queryTree_list = pg_parse_and_rewrite(fcache->src,
										  fcache->argOidVect, nargs);

	foreach(qtl_item, queryTree_list)
	{
		Query	   *queryTree = lfirst(qtl_item);
		Plan	   *planTree;
		EState	   *estate;

		planTree = pg_plan_query(queryTree);

		if (!nextes)
			nextes = (execution_state *) palloc(sizeof(execution_state));
		if (preves)
			preves->next = nextes;

		nextes->next = NULL;
		nextes->status = F_EXEC_START;
		nextes->qd = CreateQueryDesc(queryTree,
									 planTree,
									 None);
		estate = CreateExecutorState();

		if (queryTree->limitOffset != NULL || queryTree->limitCount != NULL)
			elog(ERROR, "LIMIT clause from SQL functions not yet implemented");

		if (nargs > 0)
		{
			int			i;
			ParamListInfo paramLI;

			paramLI = (ParamListInfo) palloc((nargs + 1) * sizeof(ParamListInfoData));

			MemSet(paramLI, 0, nargs * sizeof(ParamListInfoData));

			estate->es_param_list_info = paramLI;

			for (i = 0; i < nargs; paramLI++, i++)
			{
				paramLI->kind = PARAM_NUM;
				paramLI->id = i + 1;
				paramLI->isnull = false;
				paramLI->value = (Datum) NULL;
			}
			paramLI->kind = PARAM_INVALID;
		}
		else
			estate->es_param_list_info = (ParamListInfo) NULL;
		nextes->estate = estate;
		preves = nextes;
		nextes = (execution_state *) NULL;
	}

	return newes;
}

static TupleDesc
postquel_start(execution_state *es)
{

	/*
	 * Do nothing for utility commands. (create, destroy...)  DZ -
	 * 30-8-1996
	 */
	if (es->qd->operation == CMD_UTILITY)
		return (TupleDesc) NULL;
	return ExecutorStart(es->qd, es->estate);
}

static TupleTableSlot *
postquel_getnext(execution_state *es)
{
	int			feature;

	if (es->qd->operation == CMD_UTILITY)
	{

		/*
		 * Process a utility command. (create, destroy...)	DZ - 30-8-1996
		 */
		ProcessUtility(es->qd->parsetree->utilityStmt, es->qd->dest);
		if (!LAST_POSTQUEL_COMMAND(es))
			CommandCounterIncrement();
		return (TupleTableSlot *) NULL;
	}

	feature = (LAST_POSTQUEL_COMMAND(es)) ? EXEC_RETONE : EXEC_RUN;

	return ExecutorRun(es->qd, es->estate, feature, (Node *) NULL, (Node *) NULL);
}

static void
postquel_end(execution_state *es)
{

	/*
	 * Do nothing for utility commands. (create, destroy...)  DZ -
	 * 30-8-1996
	 */
	if (es->qd->operation == CMD_UTILITY)
		return;
	ExecutorEnd(es->qd, es->estate);
}

static void
postquel_sub_params(execution_state *es, FunctionCallInfo fcinfo)
{
	EState	   *estate;
	ParamListInfo paramLI;

	estate = es->estate;
	paramLI = estate->es_param_list_info;

	while (paramLI->kind != PARAM_INVALID)
	{
		if (paramLI->kind == PARAM_NUM)
		{
			Assert(paramLI->id <= fcinfo->nargs);
			paramLI->value = fcinfo->arg[paramLI->id - 1];
			paramLI->isnull = fcinfo->argnull[paramLI->id - 1];
		}
		paramLI++;
	}
}

static TupleTableSlot *
copy_function_result(FunctionCachePtr fcache,
					 TupleTableSlot *resultSlot)
{
	TupleTableSlot *funcSlot;
	TupleDesc	resultTd;
	HeapTuple	newTuple;
	HeapTuple	oldTuple;

	Assert(!TupIsNull(resultSlot));
	oldTuple = resultSlot->val;

	funcSlot = (TupleTableSlot *) fcache->funcSlot;

	if (funcSlot == (TupleTableSlot *) NULL)
		return resultSlot;

	resultTd = resultSlot->ttc_tupleDescriptor;

	/*
	 * When the funcSlot is NULL we have to initialize the funcSlot's
	 * tuple descriptor.
	 */
	if (TupIsNull(funcSlot))
	{
		int			i = 0;
		TupleDesc	funcTd = funcSlot->ttc_tupleDescriptor;

		while (i < oldTuple->t_data->t_natts)
		{
			funcTd->attrs[i] = (Form_pg_attribute) palloc(ATTRIBUTE_TUPLE_SIZE);
			memmove(funcTd->attrs[i],
					resultTd->attrs[i],
					ATTRIBUTE_TUPLE_SIZE);
			i++;
		}
	}

	newTuple = heap_copytuple(oldTuple);

	return ExecStoreTuple(newTuple, funcSlot, InvalidBuffer, true);
}

static Datum
postquel_execute(execution_state *es,
				 FunctionCallInfo fcinfo,
				 FunctionCachePtr fcache,
				 List *func_tlist)
{
	TupleTableSlot *slot;
	Datum		value;

	/*
	 * It's more right place to do it (before
	 * postquel_start->ExecutorStart). Now
	 * ExecutorStart->ExecInitIndexScan->ExecEvalParam works ok. (But
	 * note: I HOPE we can do it here). - vadim 01/22/97
	 */
	if (fcinfo->nargs > 0)
		postquel_sub_params(es, fcinfo);

	if (es->status == F_EXEC_START)
	{
		postquel_start(es);
		es->status = F_EXEC_RUN;
	}

	slot = postquel_getnext(es);

	if (TupIsNull(slot))
	{
		postquel_end(es);
		es->status = F_EXEC_DONE;
		fcinfo->isnull = true;

		/*
		 * If this isn't the last command for the function we have to
		 * increment the command counter so that subsequent commands can
		 * see changes made by previous ones.
		 */
		if (!LAST_POSTQUEL_COMMAND(es))
			CommandCounterIncrement();
		return (Datum) NULL;
	}

	if (LAST_POSTQUEL_COMMAND(es))
	{
		TupleTableSlot *resSlot;

		/*
		 * Copy the result.  copy_function_result is smart enough to do
		 * nothing when no action is called for.  This helps reduce the
		 * logic and code redundancy here.
		 */
		resSlot = copy_function_result(fcache, slot);
		if (func_tlist != NIL)
		{
			TargetEntry *tle = lfirst(func_tlist);

			value = ProjectAttribute(resSlot->ttc_tupleDescriptor,
									 tle,
									 resSlot->val,
									 &fcinfo->isnull);
		}
		else
		{
			/* XXX is this right?  Return whole tuple slot?? */
			value = PointerGetDatum(resSlot);
			fcinfo->isnull = false;
		}

		/*
		 * If this is a single valued function we have to end the function
		 * execution now.
		 */
		if (fcache->oneResult)
		{
			postquel_end(es);
			es->status = F_EXEC_DONE;
		}

		return value;
	}

	/*
	 * If this isn't the last command for the function, we don't return
	 * any results, but we have to increment the command counter so that
	 * subsequent commands can see changes made by previous ones.
	 */
	CommandCounterIncrement();
	return (Datum) NULL;
}

Datum
postquel_function(FunctionCallInfo fcinfo,
				  FunctionCachePtr fcache,
				  List *func_tlist,
				  bool *isDone)
{
	execution_state *es;
	Datum		result = 0;
	CommandId	savedId;

	/*
	 * Before we start do anything we must save CurrentScanCommandId to
	 * restore it before return to upper Executor. Also, we have to set
	 * CurrentScanCommandId equal to CurrentCommandId. - vadim 08/29/97
	 */
	savedId = GetScanCommandId();
	SetScanCommandId(GetCurrentCommandId());

	es = (execution_state *) fcache->func_state;
	if (es == NULL)
	{
		es = init_execution_state(fcache);
		fcache->func_state = (char *) es;
	}

	while (es && es->status == F_EXEC_DONE)
		es = es->next;

	Assert(es);

	/*
	 * Execute each command in the function one after another until we're
	 * executing the final command and get a result or we run out of
	 * commands.
	 */
	while (es != (execution_state *) NULL)
	{
		result = postquel_execute(es,
								  fcinfo,
								  fcache,
								  func_tlist);
		if (es->status != F_EXEC_DONE)
			break;
		es = es->next;
	}

	/*
	 * Restore outer command ID.
	 */
	SetScanCommandId(savedId);

	/*
	 * If we've gone through every command in this function, we are done.
	 */
	if (es == (execution_state *) NULL)
	{

		/*
		 * Reset the execution states to start over again
		 */
		es = (execution_state *) fcache->func_state;
		while (es)
		{
			es->status = F_EXEC_START;
			es = es->next;
		}

		/*
		 * Let caller know we're finished.
		 */
		*isDone = true;
		return (fcache->oneResult) ? result : (Datum) NULL;
	}

	/*
	 * If we got a result from a command within the function it has to be
	 * the final command.  All others shouldn't be returning anything.
	 */
	Assert(LAST_POSTQUEL_COMMAND(es));

	*isDone = false;
	return result;
}
