/*-------------------------------------------------------------------------
 *
 * pl_handler.c		- Handler for the PL/pgSQL
 *			  procedural language
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/pl/plpgsql/src/pl_handler.c
 *
 *-------------------------------------------------------------------------
 */

#include "plpgsql.h"

#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static bool plpgsql_extra_checks_check_hook(char **newvalue, void **extra, GucSource source);
static void plpgsql_extra_warnings_assign_hook(const char *newvalue, void *extra);
static void plpgsql_extra_errors_assign_hook(const char *newvalue, void *extra);

PG_MODULE_MAGIC;

/* Custom GUC variable */
static const struct config_enum_entry variable_conflict_options[] = {
	{"error", PLPGSQL_RESOLVE_ERROR, false},
	{"use_variable", PLPGSQL_RESOLVE_VARIABLE, false},
	{"use_column", PLPGSQL_RESOLVE_COLUMN, false},
	{NULL, 0, false}
};

int			plpgsql_variable_conflict = PLPGSQL_RESOLVE_ERROR;

bool		plpgsql_print_strict_params = false;

bool		plpgsql_check_asserts = true;

char	   *plpgsql_extra_warnings_string = NULL;
char	   *plpgsql_extra_errors_string = NULL;
int			plpgsql_extra_warnings;
int			plpgsql_extra_errors;

/* Hook for plugins */
PLpgSQL_plugin **plugin_ptr = NULL;


static bool
plpgsql_extra_checks_check_hook(char **newvalue, void **extra, GucSource source)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	int			extrachecks = 0;
	int		   *myextra;

	if (pg_strcasecmp(*newvalue, "all") == 0)
		extrachecks = PLPGSQL_XCHECK_ALL;
	else if (pg_strcasecmp(*newvalue, "none") == 0)
		extrachecks = PLPGSQL_XCHECK_NONE;
	else
	{
		/* Need a modifiable copy of string */
		rawstring = pstrdup(*newvalue);

		/* Parse string into list of identifiers */
		if (!SplitIdentifierString(rawstring, ',', &elemlist))
		{
			/* syntax error in list */
			GUC_check_errdetail("List syntax is invalid.");
			pfree(rawstring);
			list_free(elemlist);
			return false;
		}

		foreach(l, elemlist)
		{
			char	   *tok = (char *) lfirst(l);

			if (pg_strcasecmp(tok, "shadowed_variables") == 0)
				extrachecks |= PLPGSQL_XCHECK_SHADOWVAR;
			else if (pg_strcasecmp(tok, "all") == 0 || pg_strcasecmp(tok, "none") == 0)
			{
				GUC_check_errdetail("Key word \"%s\" cannot be combined with other key words.", tok);
				pfree(rawstring);
				list_free(elemlist);
				return false;
			}
			else
			{
				GUC_check_errdetail("Unrecognized key word: \"%s\".", tok);
				pfree(rawstring);
				list_free(elemlist);
				return false;
			}
		}

		pfree(rawstring);
		list_free(elemlist);
	}

	myextra = (int *) malloc(sizeof(int));
	*myextra = extrachecks;
	*extra = (void *) myextra;

	return true;
}

static void
plpgsql_extra_warnings_assign_hook(const char *newvalue, void *extra)
{
	plpgsql_extra_warnings = *((int *) extra);
}

static void
plpgsql_extra_errors_assign_hook(const char *newvalue, void *extra)
{
	plpgsql_extra_errors = *((int *) extra);
}


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

	pg_bindtextdomain(TEXTDOMAIN);

	DefineCustomEnumVariable("plpgsql.variable_conflict",
							 gettext_noop("Sets handling of conflicts between PL/pgSQL variable names and table column names."),
							 NULL,
							 &plpgsql_variable_conflict,
							 PLPGSQL_RESOLVE_ERROR,
							 variable_conflict_options,
							 PGC_SUSET, 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("plpgsql.print_strict_params",
							 gettext_noop("Print information about parameters in the DETAIL part of the error messages generated on INTO ... STRICT failures."),
							 NULL,
							 &plpgsql_print_strict_params,
							 false,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("plpgsql.check_asserts",
				  gettext_noop("Perform checks given in ASSERT statements."),
							 NULL,
							 &plpgsql_check_asserts,
							 true,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("plpgsql.extra_warnings",
							   gettext_noop("List of programming constructs that should produce a warning."),
							   NULL,
							   &plpgsql_extra_warnings_string,
							   "none",
							   PGC_USERSET, GUC_LIST_INPUT,
							   plpgsql_extra_checks_check_hook,
							   plpgsql_extra_warnings_assign_hook,
							   NULL);

	DefineCustomStringVariable("plpgsql.extra_errors",
							   gettext_noop("List of programming constructs that should produce an error."),
							   NULL,
							   &plpgsql_extra_errors_string,
							   "none",
							   PGC_USERSET, GUC_LIST_INPUT,
							   plpgsql_extra_checks_check_hook,
							   plpgsql_extra_errors_assign_hook,
							   NULL);

	EmitWarningsOnPlaceholders("plpgsql");

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
	PLpgSQL_execstate *save_cur_estate;
	Datum		retval;
	int			rc;

	/*
	 * Connect to SPI manager
	 */
	if ((rc = SPI_connect()) != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed: %s", SPI_result_code_string(rc));

	/* Find or compile the function */
	func = plpgsql_compile(fcinfo, false);

	/* Must save and restore prior value of cur_estate */
	save_cur_estate = func->cur_estate;

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
		else if (CALLED_AS_EVENT_TRIGGER(fcinfo))
		{
			plpgsql_exec_event_trigger(func,
									   (EventTriggerData *) fcinfo->context);
			retval = (Datum) 0;
		}
		else
			retval = plpgsql_exec_function(func, fcinfo, NULL);
	}
	PG_CATCH();
	{
		/* Decrement use-count, restore cur_estate, and propagate error */
		func->use_count--;
		func->cur_estate = save_cur_estate;
		PG_RE_THROW();
	}
	PG_END_TRY();

	func->use_count--;

	func->cur_estate = save_cur_estate;

	/*
	 * Disconnect from SPI manager
	 */
	if ((rc = SPI_finish()) != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed: %s", SPI_result_code_string(rc));

	return retval;
}

/* ----------
 * plpgsql_inline_handler
 *
 * Called by PostgreSQL to execute an anonymous code block
 * ----------
 */
PG_FUNCTION_INFO_V1(plpgsql_inline_handler);

Datum
plpgsql_inline_handler(PG_FUNCTION_ARGS)
{
	InlineCodeBlock *codeblock = (InlineCodeBlock *) DatumGetPointer(PG_GETARG_DATUM(0));
	PLpgSQL_function *func;
	FunctionCallInfoData fake_fcinfo;
	FmgrInfo	flinfo;
	EState	   *simple_eval_estate;
	Datum		retval;
	int			rc;

	Assert(IsA(codeblock, InlineCodeBlock));

	/*
	 * Connect to SPI manager
	 */
	if ((rc = SPI_connect()) != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed: %s", SPI_result_code_string(rc));

	/* Compile the anonymous code block */
	func = plpgsql_compile_inline(codeblock->source_text);

	/* Mark the function as busy, just pro forma */
	func->use_count++;

	/*
	 * Set up a fake fcinfo with just enough info to satisfy
	 * plpgsql_exec_function().  In particular note that this sets things up
	 * with no arguments passed.
	 */
	MemSet(&fake_fcinfo, 0, sizeof(fake_fcinfo));
	MemSet(&flinfo, 0, sizeof(flinfo));
	fake_fcinfo.flinfo = &flinfo;
	flinfo.fn_oid = InvalidOid;
	flinfo.fn_mcxt = CurrentMemoryContext;

	/* Create a private EState for simple-expression execution */
	simple_eval_estate = CreateExecutorState();

	/* And run the function */
	PG_TRY();
	{
		retval = plpgsql_exec_function(func, &fake_fcinfo, simple_eval_estate);
	}
	PG_CATCH();
	{
		/*
		 * We need to clean up what would otherwise be long-lived resources
		 * accumulated by the failed DO block, principally cached plans for
		 * statements (which can be flushed with plpgsql_free_function_memory)
		 * and execution trees for simple expressions, which are in the
		 * private EState.
		 *
		 * Before releasing the private EState, we must clean up any
		 * simple_econtext_stack entries pointing into it, which we can do by
		 * invoking the subxact callback.  (It will be called again later if
		 * some outer control level does a subtransaction abort, but no harm
		 * is done.)  We cheat a bit knowing that plpgsql_subxact_cb does not
		 * pay attention to its parentSubid argument.
		 */
		plpgsql_subxact_cb(SUBXACT_EVENT_ABORT_SUB,
						   GetCurrentSubTransactionId(),
						   0, NULL);

		/* Clean up the private EState */
		FreeExecutorState(simple_eval_estate);

		/* Function should now have no remaining use-counts ... */
		func->use_count--;
		Assert(func->use_count == 0);

		/* ... so we can free subsidiary storage */
		plpgsql_free_function_memory(func);

		/* And propagate the error */
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Clean up the private EState */
	FreeExecutorState(simple_eval_estate);

	/* Function should now have no remaining use-counts ... */
	func->use_count--;
	Assert(func->use_count == 0);

	/* ... so we can free subsidiary storage */
	plpgsql_free_function_memory(func);

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
	bool		is_dml_trigger = false;
	bool		is_event_trigger = false;
	int			i;

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	/* Get the new function's pg_proc entry */
	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	functyptype = get_typtype(proc->prorettype);

	/* Disallow pseudotype result */
	/* except for TRIGGER, RECORD, VOID, or polymorphic */
	if (functyptype == TYPTYPE_PSEUDO)
	{
		/* we assume OPAQUE with no arguments means a trigger */
		if (proc->prorettype == TRIGGEROID ||
			(proc->prorettype == OPAQUEOID && proc->pronargs == 0))
			is_dml_trigger = true;
		else if (proc->prorettype == EVTTRIGGEROID)
			is_event_trigger = true;
		else if (proc->prorettype != RECORDOID &&
				 proc->prorettype != VOIDOID &&
				 !IsPolymorphicType(proc->prorettype))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("PL/pgSQL functions cannot return type %s",
							format_type_be(proc->prorettype))));
	}

	/* Disallow pseudotypes in arguments (either IN or OUT) */
	/* except for polymorphic */
	numargs = get_func_arg_info(tuple,
								&argtypes, &argnames, &argmodes);
	for (i = 0; i < numargs; i++)
	{
		if (get_typtype(argtypes[i]) == TYPTYPE_PSEUDO)
		{
			if (!IsPolymorphicType(argtypes[i]))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("PL/pgSQL functions cannot accept type %s",
								format_type_be(argtypes[i]))));
		}
	}

	/* Postpone body checks if !check_function_bodies */
	if (check_function_bodies)
	{
		FunctionCallInfoData fake_fcinfo;
		FmgrInfo	flinfo;
		int			rc;
		TriggerData trigdata;
		EventTriggerData etrigdata;

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
		if (is_dml_trigger)
		{
			MemSet(&trigdata, 0, sizeof(trigdata));
			trigdata.type = T_TriggerData;
			fake_fcinfo.context = (Node *) &trigdata;
		}
		else if (is_event_trigger)
		{
			MemSet(&etrigdata, 0, sizeof(etrigdata));
			etrigdata.type = T_EventTriggerData;
			fake_fcinfo.context = (Node *) &etrigdata;
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
