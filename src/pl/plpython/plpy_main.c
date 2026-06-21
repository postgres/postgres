/*
 * PL/Python main entry points
 *
 * src/pl/plpython/plpy_main.c
 */

#include "postgres.h"

#include "catalog/pg_proc.h"
#include "commands/event_trigger.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "plpy_elog.h"
#include "plpy_exec.h"
#include "plpy_main.h"
#include "plpy_plpymodule.h"
#include "plpy_subxactobject.h"
#include "plpy_util.h"
#include "utils/guc.h"
#include "utils/memutils.h"
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

	explicit_subtransactions = NIL;

	PLy_execution_contexts = NULL;
}

Datum
plpython3_validator(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(fake_fcinfo, 0);
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_proc procStruct;
	PLyTrigType is_trigger;
	TriggerData trigdata;
	EventTriggerData etrigdata;
	FmgrInfo	flinfo;
	PLyProcedureCache *pcache;

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

	/*
	 * Set up a fake flinfo/fcinfo with just enough info to satisfy
	 * PLy_procedure_get().  That function derives the call context (plain
	 * function, DML trigger, or event trigger) from the fcinfo, so we have to
	 * construct matching context here.
	 */
	MemSet(fake_fcinfo, 0, SizeForFunctionCallInfo(0));
	MemSet(&flinfo, 0, sizeof(flinfo));
	fake_fcinfo->flinfo = &flinfo;
	flinfo.fn_oid = funcoid;
	flinfo.fn_mcxt = CurrentMemoryContext;

	if (is_trigger == PLPY_TRIGGER)
	{
		MemSet(&trigdata, 0, sizeof(trigdata));
		trigdata.type = T_TriggerData;
		/* We can't validate triggers against any particular table ... */
		fake_fcinfo->context = (Node *) &trigdata;
	}
	else if (is_trigger == PLPY_EVENT_TRIGGER)
	{
		MemSet(&etrigdata, 0, sizeof(etrigdata));
		etrigdata.type = T_EventTriggerData;
		fake_fcinfo->context = (Node *) &etrigdata;
	}

	pcache = PLy_procedure_get(fake_fcinfo, true);

	/*
	 * Release the reference count that PLy_procedure_get acquired; the
	 * PLyProcedure object remains valid for possible future use.  (We could
	 * leave this to be done when the calling memory context is cleaned up,
	 * but it seems neater to do it right away.  Note we mustn't release the
	 * pcache object, since the memory-context reset callback has a reference
	 * to it.)
	 */
	Assert(pcache->proc->cfunc.use_count > 0);
	pcache->proc->cfunc.use_count--;
	pcache->proc = NULL;

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
		PLyProcedureCache *pcache;

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

		/*
		 * Look up (and if necessary compile) the procedure.  This can throw
		 * an error, so it must happen inside the PG_TRY so that the execution
		 * context gets popped on the way out.
		 */
		pcache = PLy_procedure_get(fcinfo, false);
		exec_ctx->curr_proc = pcache->proc;

		if (CALLED_AS_TRIGGER(fcinfo))
		{
			HeapTuple	trv;

			trv = PLy_exec_trigger(fcinfo, pcache->proc);
			retval = PointerGetDatum(trv);
		}
		else if (CALLED_AS_EVENT_TRIGGER(fcinfo))
		{
			PLy_exec_event_trigger(fcinfo, pcache->proc);
			retval = (Datum) 0;
		}
		else
			retval = PLy_exec_function(fcinfo, pcache);
	}
	PG_CATCH();
	{
		/* Destroy the execution context */
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
	PLyProcedureCache pcache;
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

	/* Set up a minimal PLyProcedureCache for the inline block */
	MemSet(&pcache, 0, sizeof(PLyProcedureCache));
	pcache.proc = &proc;
	pcache.fcontext = CurrentMemoryContext;

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
		PLy_exec_function(fake_fcinfo, &pcache);
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

/*
 * Determine whether a function is a (DML or event) trigger from its pg_proc
 * result type.  This is used by the validator, which has no call context to
 * inspect; the call handler instead relies on the fcinfo's call context.
 */
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
