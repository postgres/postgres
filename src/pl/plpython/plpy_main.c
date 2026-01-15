/*
 * PL/Python main entry points
 *
 * src/pl/plpython/plpy_main.c
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/event_trigger.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "plpy_elog.h"
#include "plpy_exec.h"
#include "plpy_main.h"
#include "plpy_plpymodule.h"
#include "plpy_procedure.h"
#include "plpy_subxactobject.h"
#include "plpy_util.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * exported functions
 */

PG_MODULE_MAGIC_EXT(
					.name = "plpython",
					.version = PG_VERSION
);

PG_FUNCTION_INFO_V1(plpython3_validator);
PG_FUNCTION_INFO_V1(plpython3_call_handler);
PG_FUNCTION_INFO_V1(plpython3_inline_handler);


static PLyTrigType PLy_procedure_is_trigger(Form_pg_proc procStruct);
static void plpython_error_callback(void *arg);
static void plpython_inline_error_callback(void *arg);

static PLyExecutionContext *PLy_push_execution_context(bool atomic_context);
static void PLy_pop_execution_context(void);

/* initialize global variables */
PyObject   *PLy_interp_globals = NULL;

/* this doesn't need to be global; use PLy_current_execution_context() */
static PLyExecutionContext *PLy_execution_contexts = NULL;


void
_PG_init(void)
{
	PyObject   *main_mod;
	PyObject   *main_dict;
	PyObject   *GD;
	PyObject   *plpy_mod;

	pg_bindtextdomain(TEXTDOMAIN);

	/* Add plpy to table of built-in modules. */
	PyImport_AppendInittab("plpy", PyInit_plpy);

	/* Initialize Python interpreter. */
	Py_Initialize();

	main_mod = PyImport_AddModule("__main__");
	if (main_mod == NULL || PyErr_Occurred())
		PLy_elog(ERROR, "could not import \"%s\" module", "__main__");
	Py_INCREF(main_mod);

	main_dict = PyModule_GetDict(main_mod);
	if (main_dict == NULL)
		PLy_elog(ERROR, NULL);

	/*
	 * Set up GD.
	 */
	GD = PyDict_New();
	if (GD == NULL)
		PLy_elog(ERROR, NULL);
	PyDict_SetItemString(main_dict, "GD", GD);

	/*
	 * Import plpy.
	 */
	plpy_mod = PyImport_ImportModule("plpy");
	if (plpy_mod == NULL)
		PLy_elog(ERROR, "could not import \"%s\" module", "plpy");
	if (PyDict_SetItemString(main_dict, "plpy", plpy_mod) == -1)
		PLy_elog(ERROR, NULL);

	if (PyErr_Occurred())
		PLy_elog(FATAL, "untrapped error in initialization");

	Py_INCREF(main_dict);
	PLy_interp_globals = main_dict;

	Py_DECREF(main_mod);

	init_procedure_caches();

	explicit_subtransactions = NIL;

	PLy_execution_contexts = NULL;
}

Datum
plpython3_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_proc procStruct;
	PLyTrigType is_trigger;

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	if (!check_function_bodies)
		PG_RETURN_VOID();

	/* Get the new function's pg_proc entry */
	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	procStruct = (Form_pg_proc) GETSTRUCT(tuple);

	is_trigger = PLy_procedure_is_trigger(procStruct);

	ReleaseSysCache(tuple);

	/* We can't validate triggers against any particular table ... */
	(void) PLy_procedure_get(funcoid, InvalidOid, is_trigger);

	PG_RETURN_VOID();
}

Datum
plpython3_call_handler(PG_FUNCTION_ARGS)
{
	bool		nonatomic;
	Datum		retval;
	PLyExecutionContext *exec_ctx;
	ErrorContextCallback plerrcontext;

	nonatomic = fcinfo->context &&
		IsA(fcinfo->context, CallContext) &&
		!castNode(CallContext, fcinfo->context)->atomic;

	/* Note: SPI_finish() happens in plpy_exec.c, which is dubious design */
	SPI_connect_ext(nonatomic ? SPI_OPT_NONATOMIC : 0);

	/*
	 * Push execution context onto stack.  It is important that this get
	 * popped again, so avoid putting anything that could throw error between
	 * here and the PG_TRY.
	 */
	exec_ctx = PLy_push_execution_context(!nonatomic);

	PG_TRY();
	{
		Oid			funcoid = fcinfo->flinfo->fn_oid;
		PLyProcedure *proc;

		/*
		 * Setup error traceback support for ereport().  Note that the PG_TRY
		 * structure pops this for us again at exit, so we needn't do that
		 * explicitly, nor do we risk the callback getting called after we've
		 * destroyed the exec_ctx.
		 */
		plerrcontext.callback = plpython_error_callback;
		plerrcontext.arg = exec_ctx;
		plerrcontext.previous = error_context_stack;
		error_context_stack = &plerrcontext;

		if (CALLED_AS_TRIGGER(fcinfo))
		{
			Relation	tgrel = ((TriggerData *) fcinfo->context)->tg_relation;
			HeapTuple	trv;

			proc = PLy_procedure_get(funcoid, RelationGetRelid(tgrel), PLPY_TRIGGER);
			exec_ctx->curr_proc = proc;
			trv = PLy_exec_trigger(fcinfo, proc);
			retval = PointerGetDatum(trv);
		}
		else if (CALLED_AS_EVENT_TRIGGER(fcinfo))
		{
			proc = PLy_procedure_get(funcoid, InvalidOid, PLPY_EVENT_TRIGGER);
			exec_ctx->curr_proc = proc;
			PLy_exec_event_trigger(fcinfo, proc);
			retval = (Datum) 0;
		}
		else
		{
			proc = PLy_procedure_get(funcoid, InvalidOid, PLPY_NOT_TRIGGER);
			exec_ctx->curr_proc = proc;
			retval = PLy_exec_function(fcinfo, proc);
		}
	}
	PG_CATCH();
	{
		PLy_pop_execution_context();
		PyErr_Clear();
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Destroy the execution context */
	PLy_pop_execution_context();

	return retval;
}

Datum
plpython3_inline_handler(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(fake_fcinfo, 0);
	InlineCodeBlock *codeblock = (InlineCodeBlock *) DatumGetPointer(PG_GETARG_DATUM(0));
	FmgrInfo	flinfo;
	PLyProcedure proc;
	PLyExecutionContext *exec_ctx;
	ErrorContextCallback plerrcontext;

	/* Note: SPI_finish() happens in plpy_exec.c, which is dubious design */
	SPI_connect_ext(codeblock->atomic ? 0 : SPI_OPT_NONATOMIC);

	MemSet(fcinfo, 0, SizeForFunctionCallInfo(0));
	MemSet(&flinfo, 0, sizeof(flinfo));
	fake_fcinfo->flinfo = &flinfo;
	flinfo.fn_oid = InvalidOid;
	flinfo.fn_mcxt = CurrentMemoryContext;

	MemSet(&proc, 0, sizeof(PLyProcedure));
	proc.mcxt = AllocSetContextCreate(TopMemoryContext,
									  "__plpython_inline_block",
									  ALLOCSET_DEFAULT_SIZES);
	proc.pyname = MemoryContextStrdup(proc.mcxt, "__plpython_inline_block");
	proc.langid = codeblock->langOid;

	/*
	 * This is currently sufficient to get PLy_exec_function to work, but
	 * someday we might need to be honest and use PLy_output_setup_func.
	 */
	proc.result.typoid = VOIDOID;

	/*
	 * Push execution context onto stack.  It is important that this get
	 * popped again, so avoid putting anything that could throw error between
	 * here and the PG_TRY.
	 */
	exec_ctx = PLy_push_execution_context(codeblock->atomic);

	PG_TRY();
	{
		/*
		 * Setup error traceback support for ereport().
		 * plpython_inline_error_callback doesn't currently need exec_ctx, but
		 * for consistency with plpython3_call_handler we do it the same way.
		 */
		plerrcontext.callback = plpython_inline_error_callback;
		plerrcontext.arg = exec_ctx;
		plerrcontext.previous = error_context_stack;
		error_context_stack = &plerrcontext;

		PLy_procedure_compile(&proc, codeblock->source_text);
		exec_ctx->curr_proc = &proc;
		PLy_exec_function(fake_fcinfo, &proc);
	}
	PG_CATCH();
	{
		PLy_pop_execution_context();
		PLy_procedure_delete(&proc);
		PyErr_Clear();
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Destroy the execution context */
	PLy_pop_execution_context();

	/* Now clean up the transient procedure we made */
	PLy_procedure_delete(&proc);

	PG_RETURN_VOID();
}

static PLyTrigType
PLy_procedure_is_trigger(Form_pg_proc procStruct)
{
	PLyTrigType ret;

	switch (procStruct->prorettype)
	{
		case TRIGGEROID:
			ret = PLPY_TRIGGER;
			break;
		case EVENT_TRIGGEROID:
			ret = PLPY_EVENT_TRIGGER;
			break;
		default:
			ret = PLPY_NOT_TRIGGER;
			break;
	}

	return ret;
}

static void
plpython_error_callback(void *arg)
{
	PLyExecutionContext *exec_ctx = (PLyExecutionContext *) arg;

	if (exec_ctx->curr_proc)
	{
		if (exec_ctx->curr_proc->is_procedure)
			errcontext("PL/Python procedure \"%s\"",
					   PLy_procedure_name(exec_ctx->curr_proc));
		else
			errcontext("PL/Python function \"%s\"",
					   PLy_procedure_name(exec_ctx->curr_proc));
	}
}

static void
plpython_inline_error_callback(void *arg)
{
	errcontext("PL/Python anonymous code block");
}

PLyExecutionContext *
PLy_current_execution_context(void)
{
	if (PLy_execution_contexts == NULL)
		elog(ERROR, "no Python function is currently executing");

	return PLy_execution_contexts;
}

MemoryContext
PLy_get_scratch_context(PLyExecutionContext *context)
{
	/*
	 * A scratch context might never be needed in a given plpython procedure,
	 * so allocate it on first request.
	 */
	if (context->scratch_ctx == NULL)
		context->scratch_ctx =
			AllocSetContextCreate(TopTransactionContext,
								  "PL/Python scratch context",
								  ALLOCSET_DEFAULT_SIZES);
	return context->scratch_ctx;
}

static PLyExecutionContext *
PLy_push_execution_context(bool atomic_context)
{
	PLyExecutionContext *context;

	/* Pick a memory context similar to what SPI uses. */
	context = (PLyExecutionContext *)
		MemoryContextAlloc(atomic_context ? TopTransactionContext : PortalContext,
						   sizeof(PLyExecutionContext));
	context->curr_proc = NULL;
	context->scratch_ctx = NULL;
	context->next = PLy_execution_contexts;
	PLy_execution_contexts = context;
	return context;
}

static void
PLy_pop_execution_context(void)
{
	PLyExecutionContext *context = PLy_execution_contexts;

	if (context == NULL)
		elog(ERROR, "no Python function is currently executing");

	PLy_execution_contexts = context->next;

	if (context->scratch_ctx)
		MemoryContextDelete(context->scratch_ctx);
	pfree(context);
}
