/*-------------------------------------------------------------------------
 *
 * functions.c
 *	  Routines to handle functions called from the executor
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/functions.c,v 1.45 2001/03/22 06:16:12 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/execdefs.h"
#include "executor/executor.h"
#include "executor/functions.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/syscache.h"


/*
 * We have an execution_state record for each query in the function.
 */
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


/*
 * An SQLFunctionCache record is built during the first call,
 * and linked to from the fn_extra field of the FmgrInfo struct.
 */

typedef struct
{
	int			typlen;			/* length of the return type */
	bool		typbyval;		/* true if return type is pass by value */
	bool		returnsTuple;	/* true if return type is a tuple */

	TupleTableSlot *funcSlot;	/* if one result we need to copy it before
								 * we end execution of the function and
								 * free stuff */

	/* head of linked list of execution_state records */
	execution_state *func_state;
} SQLFunctionCache;

typedef SQLFunctionCache *SQLFunctionCachePtr;


/* non-export function prototypes */
static execution_state *init_execution_state(char *src,
					 Oid *argOidVect, int nargs);
static void init_sql_fcache(FmgrInfo *finfo);
static void postquel_start(execution_state *es);
static TupleTableSlot *postquel_getnext(execution_state *es);
static void postquel_end(execution_state *es);
static void postquel_sub_params(execution_state *es, FunctionCallInfo fcinfo);
static Datum postquel_execute(execution_state *es,
				 FunctionCallInfo fcinfo,
				 SQLFunctionCachePtr fcache);


static execution_state *
init_execution_state(char *src, Oid *argOidVect, int nargs)
{
	execution_state *newes;
	execution_state *nextes;
	execution_state *preves;
	List	   *queryTree_list,
			   *qtl_item;

	newes = (execution_state *) palloc(sizeof(execution_state));
	nextes = newes;
	preves = (execution_state *) NULL;

	queryTree_list = pg_parse_and_rewrite(src, argOidVect, nargs);

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


static void
init_sql_fcache(FmgrInfo *finfo)
{
	Oid			foid = finfo->fn_oid;
	HeapTuple	procedureTuple;
	HeapTuple	typeTuple;
	Form_pg_proc procedureStruct;
	Form_pg_type typeStruct;
	SQLFunctionCachePtr fcache;
	Oid		   *argOidVect;
	char	   *src;
	int			nargs;
	Datum		tmp;
	bool		isNull;

	/*
	 * get the procedure tuple corresponding to the given function Oid
	 */
	procedureTuple = SearchSysCache(PROCOID,
									ObjectIdGetDatum(foid),
									0, 0, 0);
	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR, "init_sql_fcache: Cache lookup failed for procedure %u",
			 foid);

	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	/*
	 * get the return type from the procedure tuple
	 */
	typeTuple = SearchSysCache(TYPEOID,
						   ObjectIdGetDatum(procedureStruct->prorettype),
							   0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "init_sql_fcache: Cache lookup failed for type %u",
			 procedureStruct->prorettype);

	typeStruct = (Form_pg_type) GETSTRUCT(typeTuple);

	fcache = (SQLFunctionCachePtr) palloc(sizeof(SQLFunctionCache));
	MemSet(fcache, 0, sizeof(SQLFunctionCache));

	/*
	 * get the type length and by-value flag from the type tuple
	 */
	fcache->typlen = typeStruct->typlen;
	if (typeStruct->typrelid == InvalidOid)
	{
		/* The return type is not a relation, so just use byval */
		fcache->typbyval = typeStruct->typbyval;
		fcache->returnsTuple = false;
	}
	else
	{

		/*
		 * This is a hack.	We assume here that any function returning a
		 * tuple returns it by reference.  This needs to be fixed, since
		 * actually the mechanism isn't quite like return-by-reference.
		 */
		fcache->typbyval = false;
		fcache->returnsTuple = true;
	}

	/*
	 * If we are returning exactly one result then we have to copy tuples
	 * and by reference results because we have to end the execution
	 * before we return the results.  When you do this everything
	 * allocated by the executor (i.e. slots and tuples) is freed.
	 */
	if (!finfo->fn_retset && !fcache->typbyval)
		fcache->funcSlot = MakeTupleTableSlot();
	else
		fcache->funcSlot = NULL;

	nargs = procedureStruct->pronargs;

	if (nargs > 0)
	{
		argOidVect = (Oid *) palloc(nargs * sizeof(Oid));
		memcpy(argOidVect,
			   procedureStruct->proargtypes,
			   nargs * sizeof(Oid));
	}
	else
		argOidVect = (Oid *) NULL;

	tmp = SysCacheGetAttr(PROCOID,
						  procedureTuple,
						  Anum_pg_proc_prosrc,
						  &isNull);
	if (isNull)
		elog(ERROR, "init_sql_fcache: null prosrc for procedure %u",
			 foid);
	src = DatumGetCString(DirectFunctionCall1(textout, tmp));

	fcache->func_state = init_execution_state(src, argOidVect, nargs);

	pfree(src);

	ReleaseSysCache(typeTuple);
	ReleaseSysCache(procedureTuple);

	finfo->fn_extra = (void *) fcache;
}


static void
postquel_start(execution_state *es)
{

	/*
	 * Do nothing for utility commands. (create, destroy...)  DZ -
	 * 30-8-1996
	 */
	if (es->qd->operation == CMD_UTILITY)
		return;
	ExecutorStart(es->qd, es->estate);
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

	return ExecutorRun(es->qd, es->estate, feature, 0L);
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
copy_function_result(SQLFunctionCachePtr fcache,
					 TupleTableSlot *resultSlot)
{
	TupleTableSlot *funcSlot;
	TupleDesc	resultTd;
	HeapTuple	resultTuple;
	HeapTuple	newTuple;

	Assert(!TupIsNull(resultSlot));
	resultTuple = resultSlot->val;

	funcSlot = fcache->funcSlot;

	if (funcSlot == NULL)
		return resultSlot;		/* no need to copy result */

	/*
	 * If first time through, we have to initialize the funcSlot's tuple
	 * descriptor.
	 */
	if (funcSlot->ttc_tupleDescriptor == NULL)
	{
		resultTd = CreateTupleDescCopy(resultSlot->ttc_tupleDescriptor);
		ExecSetSlotDescriptor(funcSlot, resultTd, true);
		ExecSetSlotDescriptorIsNew(funcSlot, true);
	}

	newTuple = heap_copytuple(resultTuple);

	return ExecStoreTuple(newTuple, funcSlot, InvalidBuffer, true);
}

static Datum
postquel_execute(execution_state *es,
				 FunctionCallInfo fcinfo,
				 SQLFunctionCachePtr fcache)
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

		/*
		 * If we are supposed to return a tuple, we return the tuple slot
		 * pointer converted to Datum.	If we are supposed to return a
		 * simple value, then project out the first attribute of the
		 * result tuple (ie, take the first result column of the final
		 * SELECT).
		 */
		if (fcache->returnsTuple)
		{

			/*
			 * XXX do we need to remove junk attrs from the result tuple?
			 * Probably OK to leave them, as long as they are at the end.
			 */
			value = PointerGetDatum(resSlot);
			fcinfo->isnull = false;
		}
		else
		{
			value = heap_getattr(resSlot->val,
								 1,
								 resSlot->ttc_tupleDescriptor,
								 &(fcinfo->isnull));

			/*
			 * Note: if result type is pass-by-reference then we are
			 * returning a pointer into the tuple copied by
			 * copy_function_result.  This is OK.
			 */
		}

		/*
		 * If this is a single valued function we have to end the function
		 * execution now.
		 */
		if (!fcinfo->flinfo->fn_retset)
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
fmgr_sql(PG_FUNCTION_ARGS)
{
	MemoryContext oldcontext;
	SQLFunctionCachePtr fcache;
	execution_state *es;
	Datum		result = 0;
	CommandId	savedId;

	/*
	 * Switch to context in which the fcache lives.  This ensures that
	 * parsetrees, plans, etc, will have sufficient lifetime.  The
	 * sub-executor is responsible for deleting per-tuple information.
	 */
	oldcontext = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);

	/*
	 * Before we start do anything we must save CurrentScanCommandId to
	 * restore it before return to upper Executor. Also, we have to set
	 * CurrentScanCommandId equal to CurrentCommandId. - vadim 08/29/97
	 */
	savedId = GetScanCommandId();
	SetScanCommandId(GetCurrentCommandId());

	/*
	 * Initialize fcache and execution state if first time through.
	 */
	fcache = (SQLFunctionCachePtr) fcinfo->flinfo->fn_extra;
	if (fcache == NULL)
	{
		init_sql_fcache(fcinfo->flinfo);
		fcache = (SQLFunctionCachePtr) fcinfo->flinfo->fn_extra;
	}
	es = fcache->func_state;
	Assert(es);

	/*
	 * Find first unfinished query in function.
	 */
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
		result = postquel_execute(es, fcinfo, fcache);
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
		es = fcache->func_state;
		while (es)
		{
			es->status = F_EXEC_START;
			es = es->next;
		}

		/*
		 * Let caller know we're finished.
		 */
		if (fcinfo->flinfo->fn_retset)
		{
			ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

			if (rsi && IsA(rsi, ReturnSetInfo))
				rsi->isDone = ExprEndResult;
			else
				elog(ERROR, "Set-valued function called in context that cannot accept a set");
			fcinfo->isnull = true;
			result = (Datum) 0;
		}

		MemoryContextSwitchTo(oldcontext);

		return result;
	}

	/*
	 * If we got a result from a command within the function it has to be
	 * the final command.  All others shouldn't be returning anything.
	 */
	Assert(LAST_POSTQUEL_COMMAND(es));

	/*
	 * Let caller know we're not finished.
	 */
	if (fcinfo->flinfo->fn_retset)
	{
		ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

		if (rsi && IsA(rsi, ReturnSetInfo))
			rsi->isDone = ExprMultipleResult;
		else
			elog(ERROR, "Set-valued function called in context that cannot accept a set");
	}

	MemoryContextSwitchTo(oldcontext);

	return result;
}
