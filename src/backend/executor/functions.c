/*-------------------------------------------------------------------------
 *
 * functions.c--
 *	  Routines to handle functions called from the executor
 *	  Putting this stuff in fmgr makes the postmaster a mess....
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/functions.c,v 1.19 1998/09/01 03:22:22 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"

#include "nodes/primnodes.h"
#include "nodes/relation.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

#include "catalog/pg_proc.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "nodes/params.h"
#include "fmgr.h"
#include "utils/fcache.h"
#include "utils/datum.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/syscache.h"
#include "catalog/pg_language.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "executor/execdefs.h"
#include "executor/functions.h"

#undef new

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

#define LAST_POSTQUEL_COMMAND(es) ((es)->next == (execution_state *)NULL)

/* non-export function prototypes */
static TupleDesc postquel_start(execution_state *es);
static execution_state *
init_execution_state(FunctionCachePtr fcache,
					 char *args[]);
static TupleTableSlot *postquel_getnext(execution_state *es);
static void postquel_end(execution_state *es);
static void
postquel_sub_params(execution_state *es, int nargs,
					char *args[], bool *nullV);
static Datum
postquel_execute(execution_state *es, FunctionCachePtr fcache,
				 List *fTlist, char **args, bool *isNull);


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
init_execution_state(FunctionCachePtr fcache,
					 char *args[])
{
	execution_state *newes;
	execution_state *nextes;
	execution_state *preves;
	QueryTreeList *queryTree_list;
	int			i;
	List	   *planTree_list;
	int			nargs;

	nargs = fcache->nargs;

	newes = (execution_state *) palloc(sizeof(execution_state));
	nextes = newes;
	preves = (execution_state *) NULL;


	planTree_list = (List *)
		pg_parse_and_plan(fcache->src, fcache->argOidVect, nargs, &queryTree_list, None, FALSE);

	for (i = 0; i < queryTree_list->len; i++)
	{
		EState	   *estate;
		Query	   *queryTree = (Query *) (queryTree_list->qtrees[i]);
		Plan	   *planTree = lfirst(planTree_list);

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

		if (nargs > 0)
		{
			int			i;
			ParamListInfo paramLI;

			paramLI =
				(ParamListInfo) palloc((nargs + 1) * sizeof(ParamListInfoData));

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

		planTree_list = lnext(planTree_list);
	}

	return newes;
}

static TupleDesc
postquel_start(execution_state *es)
{
#ifdef FUNC_UTIL_PATCH

	/*
	 * Do nothing for utility commands. (create, destroy...)  DZ -
	 * 30-8-1996
	 */
	if (es->qd->operation == CMD_UTILITY)
		return (TupleDesc) NULL;
#endif
	return ExecutorStart(es->qd, es->estate);
}

static TupleTableSlot *
postquel_getnext(execution_state *es)
{
	int			feature;

#ifdef FUNC_UTIL_PATCH
	if (es->qd->operation == CMD_UTILITY)
	{

		/*
		 * Process an utility command. (create, destroy...)  DZ -
		 * 30-8-1996
		 */
		ProcessUtility(es->qd->parsetree->utilityStmt, es->qd->dest);
		if (!LAST_POSTQUEL_COMMAND(es))
			CommandCounterIncrement();
		return (TupleTableSlot *) NULL;
	}
#endif

	feature = (LAST_POSTQUEL_COMMAND(es)) ? EXEC_RETONE : EXEC_RUN;

	return ExecutorRun(es->qd, es->estate, feature, 0);
}

static void
postquel_end(execution_state *es)
{
#ifdef FUNC_UTIL_PATCH

	/*
	 * Do nothing for utility commands. (create, destroy...)  DZ -
	 * 30-8-1996
	 */
	if (es->qd->operation == CMD_UTILITY)
		return;
#endif
	ExecutorEnd(es->qd, es->estate);
}

static void
postquel_sub_params(execution_state *es,
					int nargs,
					char *args[],
					bool *nullV)
{
	ParamListInfo paramLI;
	EState	   *estate;

	estate = es->estate;
	paramLI = estate->es_param_list_info;

	while (paramLI->kind != PARAM_INVALID)
	{
		if (paramLI->kind == PARAM_NUM)
		{
			Assert(paramLI->id <= nargs);
			paramLI->value = (Datum) args[(paramLI->id - 1)];
			paramLI->isnull = nullV[(paramLI->id - 1)];
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

		while (i < oldTuple->t_natts)
		{
			funcTd->attrs[i] =
				(Form_pg_attribute) palloc(ATTRIBUTE_TUPLE_SIZE);
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
				 FunctionCachePtr fcache,
				 List *fTlist,
				 char **args,
				 bool *isNull)
{
	TupleTableSlot *slot;
	Datum		value;

	/*
	 * It's more right place to do it (before
	 * postquel_start->ExecutorStart). Now
	 * ExecutorStart->ExecInitIndexScan->ExecEvalParam works ok. (But
	 * note: I HOPE we can do it here). - vadim 01/22/97
	 */
	if (fcache->nargs > 0)
		postquel_sub_params(es, fcache->nargs, args, fcache->nullVect);

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
		*isNull = true;

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
		if (fTlist != NIL)
		{
			HeapTuple	tup;
			TargetEntry *tle = lfirst(fTlist);

			tup = resSlot->val;
			value = ProjectAttribute(resSlot->ttc_tupleDescriptor,
									 tle,
									 tup,
									 isNull);
		}
		else
		{
			value = (Datum) resSlot;
			*isNull = false;
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
postquel_function(Func *funcNode, char **args, bool *isNull, bool *isDone)
{
	execution_state *es;
	Datum		result = 0;
	FunctionCachePtr fcache = funcNode->func_fcache;
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
		es = init_execution_state(fcache, args);
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
								  fcache,
								  funcNode->func_tlist,
								  args,
								  isNull);
		if (es->status != F_EXEC_DONE)
			break;
		es = es->next;
	}

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
		SetScanCommandId(savedId);
		return (fcache->oneResult) ? result : (Datum) NULL;
	}

	/*
	 * If we got a result from a command within the function it has to be
	 * the final command.  All others shouldn't be returing anything.
	 */
	Assert(LAST_POSTQUEL_COMMAND(es));
	*isDone = false;

	SetScanCommandId(savedId);
	return result;
}
