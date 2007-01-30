/*-------------------------------------------------------------------------
 *
 * pl_handler.c		- Handler for the PL/pgSQL
 *			  procedural language
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/pl/plpgsql/src/pl_handler.c,v 1.33.2.2 2007/01/30 22:05:20 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "plpgsql.h"
#include "pl.tab.h"

#include "access/heapam.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

PLpgSQL_plugin **plugin_ptr = NULL;


/*
 * _PG_init()			- library load-time initialization
 *
 * DO NOT make this static nor change its name!
 */
void
_PG_init(void)
{
	/* Be sure we do initialization only once (should be redundant now) */
	static bool inited = false;

	if (inited)
		return;

	plpgsql_HashTableInit();
	RegisterXactCallback(plpgsql_xact_cb, NULL);
	RegisterSubXactCallback(plpgsql_subxact_cb, NULL);

	/* Set up a rendezvous point with optional instrumentation plugin */
	plugin_ptr = (PLpgSQL_plugin **) find_rendezvous_variable("PLpgSQL_plugin");

	inited = true;
}

/* ----------
 * plpgsql_call_handler
 *
 * The PostgreSQL function manager and trigger manager
 * call this function for execution of PL/pgSQL procedures.
 * ----------
 */
PG_FUNCTION_INFO_V1(plpgsql_call_handler);

Datum
plpgsql_call_handler(PG_FUNCTION_ARGS)
{
	PLpgSQL_function *func;
	Datum		retval;
	int			rc;

	/*
	 * Connect to SPI manager
	 */
	if ((rc = SPI_connect()) != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed: %s", SPI_result_code_string(rc));

	/* Find or compile the function */
	func = plpgsql_compile(fcinfo, false);

	/* Mark the function as busy, so it can't be deleted from under us */
	func->use_count++;

	PG_TRY();
	{
		/*
		 * Determine if called as function or trigger and call appropriate
		 * subhandler
		 */
		if (CALLED_AS_TRIGGER(fcinfo))
			retval = PointerGetDatum(plpgsql_exec_trigger(func,
										   (TriggerData *) fcinfo->context));
		else
			retval = plpgsql_exec_function(func, fcinfo);
	}
	PG_CATCH();
	{
		/* Decrement use-count and propagate error */
		func->use_count--;
		PG_RE_THROW();
	}
	PG_END_TRY();

	func->use_count--;

	/*
	 * Disconnect from SPI manager
	 */
	if ((rc = SPI_finish()) != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed: %s", SPI_result_code_string(rc));

	return retval;
}

/* ----------
 * plpgsql_validator
 *
 * This function attempts to validate a PL/pgSQL function at
 * CREATE FUNCTION time.
 * ----------
 */
PG_FUNCTION_INFO_V1(plpgsql_validator);

Datum
plpgsql_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_proc proc;
	char		functyptype;
	int			numargs;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	bool		istrigger = false;
	int			i;

	/* Get the new function's pg_proc entry */
	tuple = SearchSysCache(PROCOID,
						   ObjectIdGetDatum(funcoid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	functyptype = get_typtype(proc->prorettype);

	/* Disallow pseudotype result */
	/* except for TRIGGER, RECORD, VOID, ANYARRAY, or ANYELEMENT */
	if (functyptype == 'p')
	{
		/* we assume OPAQUE with no arguments means a trigger */
		if (proc->prorettype == TRIGGEROID ||
			(proc->prorettype == OPAQUEOID && proc->pronargs == 0))
			istrigger = true;
		else if (proc->prorettype != RECORDOID &&
				 proc->prorettype != VOIDOID &&
				 proc->prorettype != ANYARRAYOID &&
				 proc->prorettype != ANYELEMENTOID)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("plpgsql functions cannot return type %s",
							format_type_be(proc->prorettype))));
	}

	/* Disallow pseudotypes in arguments (either IN or OUT) */
	/* except for ANYARRAY or ANYELEMENT */
	numargs = get_func_arg_info(tuple,
								&argtypes, &argnames, &argmodes);
	for (i = 0; i < numargs; i++)
	{
		if (get_typtype(argtypes[i]) == 'p')
		{
			if (argtypes[i] != ANYARRAYOID &&
				argtypes[i] != ANYELEMENTOID)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("plpgsql functions cannot take type %s",
								format_type_be(argtypes[i]))));
		}
	}

	/* Postpone body checks if !check_function_bodies */
	if (check_function_bodies)
	{
		FunctionCallInfoData fake_fcinfo;
		FmgrInfo	flinfo;
		TriggerData trigdata;
		int			rc;

		/*
		 * Connect to SPI manager (is this needed for compilation?)
		 */
		if ((rc = SPI_connect()) != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed: %s", SPI_result_code_string(rc));

		/*
		 * Set up a fake fcinfo with just enough info to satisfy
		 * plpgsql_compile().
		 */
		MemSet(&fake_fcinfo, 0, sizeof(fake_fcinfo));
		MemSet(&flinfo, 0, sizeof(flinfo));
		fake_fcinfo.flinfo = &flinfo;
		flinfo.fn_oid = funcoid;
		flinfo.fn_mcxt = CurrentMemoryContext;
		if (istrigger)
		{
			MemSet(&trigdata, 0, sizeof(trigdata));
			trigdata.type = T_TriggerData;
			fake_fcinfo.context = (Node *) &trigdata;
		}

		/* Test-compile the function */
		plpgsql_compile(&fake_fcinfo, true);

		/*
		 * Disconnect from SPI manager
		 */
		if ((rc = SPI_finish()) != SPI_OK_FINISH)
			elog(ERROR, "SPI_finish failed: %s", SPI_result_code_string(rc));
	}

	ReleaseSysCache(tuple);

	PG_RETURN_VOID();
}
