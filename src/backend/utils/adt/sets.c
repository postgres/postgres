/*-------------------------------------------------------------------------
 *
 * sets.c
 *	  Functions for sets, which are defined by queries.
 *	  Example:	 a set is defined as being the result of the query
 *			retrieve (X.all)
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/sets.c,v 1.60 2003/09/15 20:03:37 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "executor/executor.h"
#include "utils/fmgroids.h"
#include "utils/sets.h"
#include "utils/syscache.h"


/*
 *	  SetDefine		   - converts query string defining set to an oid
 *
 *	  We create an SQL function having the given querystring as its body.
 *	  The name of the function is then changed to use the OID of its tuple
 *	  in pg_proc.
 */
Oid
SetDefine(char *querystr, Oid elemType)
{
	Oid			setoid;
	char	   *procname = GENERICSETNAME;
	char	   *fileName = "-";
	char		realprocname[NAMEDATALEN];
	HeapTuple	tup,
				newtup = NULL;
	Form_pg_proc proc;
	Relation	procrel;
	int			i;
	Datum		replValue[Natts_pg_proc];
	char		replNull[Natts_pg_proc];
	char		repl[Natts_pg_proc];

	setoid = ProcedureCreate(procname,	/* changed below, after oid known */
							 PG_CATALOG_NAMESPACE,		/* XXX wrong */
							 false,		/* don't replace */
							 true,		/* returnsSet */
							 elemType,	/* returnType */
							 SQLlanguageId,		/* language */
							 F_FMGR_SQL_VALIDATOR,
							 querystr,	/* prosrc */
							 fileName,	/* probin */
							 false,		/* not aggregate */
							 false,		/* security invoker */
							 false,		/* isStrict (irrelevant, no args) */
							 PROVOLATILE_VOLATILE,		/* assume unsafe */
							 0, /* parameterCount */
							 NULL);		/* parameterTypes */

	/*
	 * Since we're still inside this command of the transaction, we can't
	 * see the results of the procedure definition unless we pretend we've
	 * started the next command.  (Postgres's solution to the Halloween
	 * problem is to not allow you to see the results of your command
	 * until you start the next command.)
	 */
	CommandCounterIncrement();

	procrel = heap_openr(ProcedureRelationName, RowExclusiveLock);

	tup = SearchSysCache(PROCOID,
						 ObjectIdGetDatum(setoid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", setoid);

	/*
	 * We can tell whether the set was already defined by checking the
	 * name.   If it's GENERICSETNAME, the set is new.  If it's "set<some
	 * oid>" it's already defined.
	 */
	proc = (Form_pg_proc) GETSTRUCT(tup);
	if (strcmp(procname, NameStr(proc->proname)) == 0)
	{
		/* make the real proc name */
		snprintf(realprocname, sizeof(realprocname), "set%u", setoid);

		/* set up the attributes to be modified or kept the same */
		repl[0] = 'r';
		for (i = 1; i < Natts_pg_proc; i++)
			repl[i] = ' ';
		replValue[0] = (Datum) realprocname;
		for (i = 1; i < Natts_pg_proc; i++)
			replValue[i] = (Datum) 0;
		for (i = 0; i < Natts_pg_proc; i++)
			replNull[i] = ' ';

		/* change the pg_proc tuple */
		newtup = heap_modifytuple(tup,
								  procrel,
								  replValue,
								  replNull,
								  repl);

		simple_heap_update(procrel, &newtup->t_self, newtup);

		setoid = HeapTupleGetOid(newtup);

		CatalogUpdateIndexes(procrel, newtup);

		heap_freetuple(newtup);
	}

	ReleaseSysCache(tup);

	heap_close(procrel, RowExclusiveLock);

	return setoid;
}

/*
 * This function executes set evaluation.  The parser sets up a set reference
 * as a call to this function with the OID of the set to evaluate as argument.
 *
 * We build a new fcache for execution of the set's function and run the
 * function until it says "no mas".  The fn_extra field of the call's
 * FmgrInfo record is a handy place to hold onto the fcache.  (Since this
 * is a built-in function, there is no competing use of fn_extra.)
 */
Datum
seteval(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	FuncExprState *fcache;
	Datum		result;
	bool		isNull;
	ExprDoneCond isDone;

	/*
	 * If this is the first call, we need to set up the fcache for the
	 * target set's function.
	 */
	fcache = (FuncExprState *) fcinfo->flinfo->fn_extra;
	if (fcache == NULL)
	{
		MemoryContext oldcontext;
		FuncExpr   *func;

		oldcontext = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);

		func = makeNode(FuncExpr);
		func->funcid = funcoid;
		func->funcresulttype = InvalidOid;		/* nothing will look at
												 * this */
		func->funcretset = true;
		func->funcformat = COERCE_EXPLICIT_CALL;
		func->args = NIL;		/* there are no arguments */

		fcache = (FuncExprState *) ExecInitExpr((Expr *) func, NULL);

		MemoryContextSwitchTo(oldcontext);

		init_fcache(funcoid, fcache, fcinfo->flinfo->fn_mcxt);

		fcinfo->flinfo->fn_extra = (void *) fcache;
	}

	/*
	 * Evaluate the function.  NOTE: we need no econtext because there are
	 * no arguments to evaluate.
	 */

	/* ExecMakeFunctionResult assumes these are initialized at call: */
	isNull = false;
	isDone = ExprSingleResult;

	result = ExecMakeFunctionResult(fcache,
									NULL,		/* no econtext, see above */
									&isNull,
									&isDone);

	/*
	 * Return isNull/isDone status.
	 */
	fcinfo->isnull = isNull;

	if (isDone != ExprSingleResult)
	{
		ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

		if (rsi && IsA(rsi, ReturnSetInfo))
			rsi->isDone = isDone;
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-valued function called in context that "
							"cannot accept a set")));
	}

	PG_RETURN_DATUM(result);
}
