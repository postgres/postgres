/*-------------------------------------------------------------------------
 *
 * sets.c
 *	  Functions for sets, which are defined by queries.
 *	  Example:	 a set is defined as being the result of the query
 *			retrieve (X.all)
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/sets.c,v 1.33 2000/08/24 03:29:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_proc.h"
#include "executor/executor.h"
#include "utils/fcache.h"
#include "utils/sets.h"
#include "utils/syscache.h"

extern CommandDest whereToSendOutput;	/* defined in tcop/postgres.c */


/*
 *	  SetDefine		   - converts query string defining set to an oid
 *
 *	  We create an SQL function having the given querystring as its body.
 *	  The name of the function is then changed to use the OID of its tuple
 *	  in pg_proc.
 */
Oid
SetDefine(char *querystr, char *typename)
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
							 true,		/* returnsSet */
							 typename,	/* returnTypeName */
							 "sql",		/* languageName */
							 querystr,	/* sourceCode */
							 fileName,	/* fileName */
							 true,		/* trusted */
							 false,		/* canCache (assume unsafe) */
							 false,		/* isStrict (irrelevant, no args) */
							 100,		/* byte_pct */
							 0,			/* perbyte_cpu */
							 0,			/* percall_cpu */
							 100,		/* outin_ratio */
							 NIL,		/* argList */
							 whereToSendOutput);

	/*
	 * Since we're still inside this command of the transaction, we can't
	 * see the results of the procedure definition unless we pretend we've
	 * started the next command.  (Postgres's solution to the Halloween
	 * problem is to not allow you to see the results of your command
	 * until you start the next command.)
	 */
	CommandCounterIncrement();

	tup = SearchSysCacheTuple(PROCOID,
							  ObjectIdGetDatum(setoid),
							  0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "SetDefine: unable to define set %s", querystr);

	/*
	 * We can tell whether the set was already defined by checking the
	 * name.   If it's GENERICSETNAME, the set is new.  If it's "set<some
	 * oid>" it's already defined.
	 */
	proc = (Form_pg_proc) GETSTRUCT(tup);
	if (strcmp(procname, NameStr(proc->proname)) == 0)
	{
		/* make the real proc name */
		sprintf(realprocname, "set%u", setoid);

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
		procrel = heap_openr(ProcedureRelationName, RowExclusiveLock);

		tup = SearchSysCacheTuple(PROCOID,
								  ObjectIdGetDatum(setoid),
								  0, 0, 0);
		if (HeapTupleIsValid(tup))
		{
			newtup = heap_modifytuple(tup,
									  procrel,
									  replValue,
									  replNull,
									  repl);

			heap_update(procrel, &tup->t_self, newtup, NULL);

			setoid = newtup->t_data->t_oid;
		}
		else
			elog(ERROR, "SetDefine: could not find new set oid tuple");

		if (RelationGetForm(procrel)->relhasindex)
		{
			Relation	idescs[Num_pg_proc_indices];

			CatalogOpenIndices(Num_pg_proc_indices, Name_pg_proc_indices, idescs);
			CatalogIndexInsert(idescs, Num_pg_proc_indices, procrel, newtup);
			CatalogCloseIndices(Num_pg_proc_indices, idescs);
		}
		heap_close(procrel, RowExclusiveLock);
	}

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
	FunctionCachePtr fcache;
	Datum		result;
	bool		isNull;
	ExprDoneCond isDone;

	/*
	 * If this is the first call, we need to set up the fcache for the
	 * target set's function.
	 */
	fcache = (FunctionCachePtr) fcinfo->flinfo->fn_extra;
	if (fcache == NULL)
	{
		fcache = init_fcache(funcoid, 0, fcinfo->flinfo->fn_mcxt);
		fcinfo->flinfo->fn_extra = (void *) fcache;
	}

	/*
	 * Evaluate the function.  NOTE: we need no econtext because there
	 * are no arguments to evaluate.
	 */

	/* ExecMakeFunctionResult assumes these are initialized at call: */
	isNull = false;
	isDone = ExprSingleResult;

	result = ExecMakeFunctionResult(fcache,
									NIL,
									NULL, /* no econtext, see above */
									&isNull,
									&isDone);

	/*
	 * If we're done with the results of this set function, get rid of
	 * its func cache so that we will start from the top next time.
	 * (Can you say "memory leak"?  This feature is a crock anyway...)
	 */
	if (isDone != ExprMultipleResult)
	{
		pfree(fcache);
		fcinfo->flinfo->fn_extra = NULL;
	}

	/*
	 * Return isNull/isDone status.
	 */
	fcinfo->isnull = isNull;

	if (isDone != ExprSingleResult)
	{
		ReturnSetInfo  *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

		if (rsi && IsA(rsi, ReturnSetInfo))
			rsi->isDone = isDone;
		else
			elog(ERROR, "Set-valued function called in context that cannot accept a set");
	}

	PG_RETURN_DATUM(result);
}
