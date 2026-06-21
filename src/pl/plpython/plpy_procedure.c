/*
 * Python procedure manipulation for plpython
 *
 * src/pl/plpython/plpy_procedure.c
 */

#include "postgres.h"

#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/event_trigger.h"
#include "commands/trigger.h"
#include "funcapi.h"
#include "plpy_elog.h"
#include "plpy_exec.h"
#include "plpy_main.h"
#include "plpy_procedure.h"
#include "plpy_util.h"
#include "utils/builtins.h"
#include "utils/funccache.h"
#include "utils/syscache.h"

static void PLy_procedure_create(PLyProcedure *proc,
								 HeapTuple procTup,
								 Oid fn_oid,
								 PLyTrigType is_trigger);
static char *PLy_procedure_munge_source(const char *name, const char *src);
static void PLy_compile_callback(FunctionCallInfo fcinfo,
								 HeapTuple procTup,
								 const CachedFunctionHashKey *hashkey,
								 CachedFunction *cfunc,
								 bool forValidator);
static void PLy_delete_callback(CachedFunction *cfunc);
static void RemovePLyProcedureCache(void *arg);


/*
 * PLy_procedure_name: get the name of the specified procedure.
 *
 * NB: this returns the SQL name, not the internal Python procedure name
 */
char *
PLy_procedure_name(PLyProcedure *proc)
{
	if (proc == NULL)
		return "<unknown procedure>";
	return proc->proname;
}

/*
 * PLy_procedure_get: returns a PLyProcedureCache struct for the function,
 * making it valid if necessary.
 *
 * The PLyProcedureCache contains a pointer to the long-lived PLyProcedure
 * (managed by funccache.c) and execution-specific state like SRF state.
 *
 * For SRFs, if we are resuming execution (srfstate->iter != NULL), we skip
 * revalidation and continue using the same PLyProcedure to ensure consistent
 * behavior throughout the SRF execution.
 */
PLyProcedureCache *
PLy_procedure_get(FunctionCallInfo fcinfo, bool forValidator)
{
	FmgrInfo   *finfo = fcinfo->flinfo;
	PLyProcedureCache *pcache;
	PLyProcedure *proc;

	/*
	 * If this is the first execution for this FmgrInfo, set up a cache struct
	 * (initially containing null pointers).  The cache must live as long as
	 * the FmgrInfo, so it goes in fn_mcxt.  Also set up a memory context
	 * callback that will be invoked when fn_mcxt is reset/deleted.
	 */
	pcache = finfo->fn_extra;
	if (pcache == NULL)
	{
		pcache = (PLyProcedureCache *)
			MemoryContextAllocZero(finfo->fn_mcxt, sizeof(PLyProcedureCache));

		pcache->fcontext = finfo->fn_mcxt;
		pcache->mcb.func = RemovePLyProcedureCache;
		pcache->mcb.arg = pcache;

		MemoryContextRegisterResetCallback(finfo->fn_mcxt, &pcache->mcb);

		finfo->fn_extra = pcache;
	}

	/*
	 * If we are resuming execution of a set-returning function, just keep
	 * using the same cache.  We do not ask funccache.c to re-validate the
	 * PLyProcedure: we want to run to completion using the function's initial
	 * definition.
	 *
	 * A live iterator (srfstate->iter != NULL) reliably means a genuine
	 * resume: when an iteration ends for any reason, srfstate->iter is reset
	 * to NULL (see comments for PLy_function_cleanup_srfstate).
	 */
	if (pcache->srfstate != NULL && pcache->srfstate->iter != NULL)
	{
		Assert(pcache->proc != NULL);
		return pcache;
	}

	/*
	 * Look up, or re-validate, the long-lived hash entry.  Like SQL-language
	 * functions, make the hash key depend on the result of
	 * get_call_result_type() when that's composite, so that we can safely
	 * assume that we'll build a new hash entry if the composite rowtype
	 * changes.
	 */
	proc = (PLyProcedure *)
		cached_function_compile(fcinfo,
								(CachedFunction *) pcache->proc,
								PLy_compile_callback,
								PLy_delete_callback,
								sizeof(PLyProcedure),
								true,
								forValidator);

	/*
	 * Install the hash pointer in the PLyProcedureCache, and increment its
	 * use count to reflect that.  If cached_function_compile gave us back a
	 * different hash entry than we were using before, we must decrement that
	 * one's use count.
	 */
	if (proc != pcache->proc)
	{
		if (pcache->proc != NULL)
		{
			Assert(pcache->proc->cfunc.use_count > 0);
			pcache->proc->cfunc.use_count--;
		}
		pcache->proc = proc;
		proc->cfunc.use_count++;
	}

	return pcache;
}

/*
 * Create (well, fill in) a new PLyProcedure structure
 */
static void
PLy_procedure_create(PLyProcedure *proc,
					 HeapTuple procTup,
					 Oid fn_oid,
					 PLyTrigType is_trigger)
{
	char		procName[NAMEDATALEN + 256];
	Form_pg_proc procStruct;
	MemoryContext cxt;
	MemoryContext oldcxt;
	int			rv;
	char	   *ptr;

	procStruct = (Form_pg_proc) GETSTRUCT(procTup);
	rv = snprintf(procName, sizeof(procName),
				  "__plpython_procedure_%s_%u",
				  NameStr(procStruct->proname),
				  fn_oid);
	if (rv >= sizeof(procName) || rv < 0)
		elog(ERROR, "procedure name would overrun buffer");

	/* Replace any not-legal-in-Python-names characters with '_' */
	for (ptr = procName; *ptr; ptr++)
	{
		if (!((*ptr >= 'A' && *ptr <= 'Z') ||
			  (*ptr >= 'a' && *ptr <= 'z') ||
			  (*ptr >= '0' && *ptr <= '9')))
			*ptr = '_';
	}

	/* Create long-lived context that all procedure info will live in */
	cxt = AllocSetContextCreate(TopMemoryContext,
								"PL/Python function",
								ALLOCSET_DEFAULT_SIZES);

	oldcxt = MemoryContextSwitchTo(cxt);

	proc->mcxt = cxt;

	PG_TRY();
	{
		Datum		protrftypes_datum;
		Datum		prosrcdatum;
		bool		isnull;
		char	   *procSource;
		int			i;

		proc->proname = pstrdup(NameStr(procStruct->proname));
		MemoryContextSetIdentifier(cxt, proc->proname);
		proc->pyname = pstrdup(procName);
		proc->fn_readonly = (procStruct->provolatile != PROVOLATILE_VOLATILE);
		proc->is_setof = procStruct->proretset;
		proc->is_procedure = (procStruct->prokind == PROKIND_PROCEDURE);
		proc->is_trigger = is_trigger;
		proc->src = NULL;
		proc->argnames = NULL;
		proc->args = NULL;
		proc->nargs = 0;
		proc->langid = procStruct->prolang;
		protrftypes_datum = SysCacheGetAttr(PROCOID, procTup,
											Anum_pg_proc_protrftypes,
											&isnull);
		proc->trftypes = isnull ? NIL : oid_array_to_list(protrftypes_datum);
		proc->code = NULL;
		proc->statics = NULL;
		proc->globals = NULL;
		proc->calldepth = 0;
		proc->argstack = NULL;

		/*
		 * get information required for output conversion of the return value,
		 * but only if this isn't a trigger.
		 */
		if (is_trigger == PLPY_NOT_TRIGGER)
		{
			Oid			rettype = procStruct->prorettype;
			HeapTuple	rvTypeTup;
			Form_pg_type rvTypeStruct;

			rvTypeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(rettype));
			if (!HeapTupleIsValid(rvTypeTup))
				elog(ERROR, "cache lookup failed for type %u", rettype);
			rvTypeStruct = (Form_pg_type) GETSTRUCT(rvTypeTup);

			/* Disallow pseudotype result, except for void or record */
			if (rvTypeStruct->typtype == TYPTYPE_PSEUDO)
			{
				if (rettype == VOIDOID ||
					rettype == RECORDOID)
					 /* okay */ ;
				else if (rettype == TRIGGEROID || rettype == EVENT_TRIGGEROID)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("trigger functions can only be called as triggers")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("PL/Python functions cannot return type %s",
									format_type_be(rettype))));
			}

			/* set up output function for procedure result */
			PLy_output_setup_func(&proc->result, proc->mcxt,
								  rettype, -1, proc);

			ReleaseSysCache(rvTypeTup);
		}
		else
		{
			/*
			 * In a trigger function, we use proc->result and proc->result_in
			 * for converting tuples, but we don't yet have enough info to set
			 * them up.  PLy_exec_trigger will deal with it.
			 */
			proc->result.typoid = InvalidOid;
			proc->result_in.typoid = InvalidOid;
		}

		/*
		 * Now get information required for input conversion of the
		 * procedure's arguments.  Note that we ignore output arguments here.
		 * If the function returns record, those I/O functions will be set up
		 * when the function is first called.
		 */
		if (procStruct->pronargs)
		{
			Oid		   *types;
			char	  **names,
					   *modes;
			int			pos,
						total;

			/* extract argument type info from the pg_proc tuple */
			total = get_func_arg_info(procTup, &types, &names, &modes);

			/* count number of in+inout args into proc->nargs */
			if (modes == NULL)
				proc->nargs = total;
			else
			{
				/* proc->nargs was initialized to 0 above */
				for (i = 0; i < total; i++)
				{
					if (modes[i] != PROARGMODE_OUT &&
						modes[i] != PROARGMODE_TABLE)
						(proc->nargs)++;
				}
			}

			/* Allocate arrays for per-input-argument data */
			proc->argnames = (char **) palloc0_array(char *, proc->nargs);
			proc->args = (PLyDatumToOb *) palloc0_array(PLyDatumToOb, proc->nargs);

			for (i = pos = 0; i < total; i++)
			{
				HeapTuple	argTypeTup;
				Form_pg_type argTypeStruct;

				if (modes &&
					(modes[i] == PROARGMODE_OUT ||
					 modes[i] == PROARGMODE_TABLE))
					continue;	/* skip OUT arguments */

				Assert(types[i] == procStruct->proargtypes.values[pos]);

				argTypeTup = SearchSysCache1(TYPEOID,
											 ObjectIdGetDatum(types[i]));
				if (!HeapTupleIsValid(argTypeTup))
					elog(ERROR, "cache lookup failed for type %u", types[i]);
				argTypeStruct = (Form_pg_type) GETSTRUCT(argTypeTup);

				/* disallow pseudotype arguments */
				if (argTypeStruct->typtype == TYPTYPE_PSEUDO)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("PL/Python functions cannot accept type %s",
									format_type_be(types[i]))));

				/* set up I/O function info */
				PLy_input_setup_func(&proc->args[pos], proc->mcxt,
									 types[i], -1,	/* typmod not known */
									 proc);

				/* get argument name */
				proc->argnames[pos] = names ? pstrdup(names[i]) : NULL;

				ReleaseSysCache(argTypeTup);

				pos++;
			}
		}

		/*
		 * get the text of the function.
		 */
		prosrcdatum = SysCacheGetAttrNotNull(PROCOID, procTup,
											 Anum_pg_proc_prosrc);
		procSource = TextDatumGetCString(prosrcdatum);

		PLy_procedure_compile(proc, procSource);

		pfree(procSource);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcxt);
		PLy_procedure_delete(proc);
		PG_RE_THROW();
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Insert the procedure into the Python interpreter
 */
void
PLy_procedure_compile(PLyProcedure *proc, const char *src)
{
	PyObject   *crv = NULL;
	char	   *msrc;
	PyObject   *code0;

	proc->globals = PyDict_Copy(PLy_interp_globals);

	/*
	 * SD is private preserved data between calls. GD is global data shared by
	 * all functions
	 */
	proc->statics = PyDict_New();
	if (!proc->statics)
		PLy_elog(ERROR, NULL);
	PyDict_SetItemString(proc->globals, "SD", proc->statics);

	/*
	 * insert the function code into the interpreter
	 */
	msrc = PLy_procedure_munge_source(proc->pyname, src);
	/* Save the mangled source for later inclusion in tracebacks */
	proc->src = MemoryContextStrdup(proc->mcxt, msrc);
	code0 = Py_CompileString(msrc, "<string>", Py_file_input);
	if (code0)
		crv = PyEval_EvalCode(code0, proc->globals, NULL);
	pfree(msrc);

	if (crv != NULL)
	{
		int			clen;
		char		call[NAMEDATALEN + 256];

		Py_DECREF(crv);

		/*
		 * compile a call to the function
		 */
		clen = snprintf(call, sizeof(call), "%s()", proc->pyname);
		if (clen < 0 || clen >= sizeof(call))
			elog(ERROR, "string would overflow buffer");
		proc->code = Py_CompileString(call, "<string>", Py_eval_input);
		if (proc->code != NULL)
			return;
	}

	if (proc->proname)
		PLy_elog(ERROR, "could not compile PL/Python function \"%s\"",
				 proc->proname);
	else
		PLy_elog(ERROR, "could not compile anonymous PL/Python code block");
}

void
PLy_procedure_delete(PLyProcedure *proc)
{
	Py_XDECREF(proc->code);
	Py_XDECREF(proc->statics);
	Py_XDECREF(proc->globals);
	MemoryContextDelete(proc->mcxt);
}

static char *
PLy_procedure_munge_source(const char *name, const char *src)
{
	char	   *mrc,
			   *mp;
	const char *sp;
	size_t		mlen;
	int			plen;

	/*
	 * room for function source and the def statement
	 */
	mlen = (strlen(src) * 2) + strlen(name) + 16;

	mrc = palloc(mlen);
	plen = snprintf(mrc, mlen, "def %s():\n\t", name);
	Assert(plen >= 0 && plen < mlen);

	sp = src;
	mp = mrc + plen;

	while (*sp != '\0')
	{
		if (*sp == '\r' && *(sp + 1) == '\n')
			sp++;

		if (*sp == '\n' || *sp == '\r')
		{
			*mp++ = '\n';
			*mp++ = '\t';
			sp++;
		}
		else
			*mp++ = *sp++;
	}
	*mp++ = '\n';
	*mp++ = '\n';
	*mp = '\0';

	if (mp > (mrc + mlen))
		elog(FATAL, "buffer overrun in PLy_procedure_munge_source");

	return mrc;
}

/*
 * Compile callback for funccache.c.
 *
 * cached_function_compile() calls this when it needs to (re)compile the
 * long-lived PLyProcedure for a function.  The CachedFunction handed to us is
 * pre-zeroed workspace of size sizeof(PLyProcedure); we just have to fill in
 * the PL/Python-specific fields.
 */
static void
PLy_compile_callback(FunctionCallInfo fcinfo,
					 HeapTuple procTup,
					 const CachedFunctionHashKey *hashkey,
					 CachedFunction *cfunc,
					 bool forValidator)
{
	PLyProcedure *proc = (PLyProcedure *) cfunc;
	Oid			fn_oid = fcinfo->flinfo->fn_oid;
	PLyTrigType is_trigger;

	/*
	 * Derive the trigger type from the call context, matching what
	 * plpython3_call_handler dispatches on.
	 */
	if (CALLED_AS_TRIGGER(fcinfo))
		is_trigger = PLPY_TRIGGER;
	else if (CALLED_AS_EVENT_TRIGGER(fcinfo))
		is_trigger = PLPY_EVENT_TRIGGER;
	else
		is_trigger = PLPY_NOT_TRIGGER;

	PLy_procedure_create(proc, procTup, fn_oid, is_trigger);
}

/*
 * Deletion callback for funccache.c.
 *
 * cached_function_compile() calls this when it discards a cache entry, which
 * only happens once the entry's use count has dropped to zero.  We must free
 * the subsidiary data but not the CachedFunction struct itself.
 */
static void
PLy_delete_callback(CachedFunction *cfunc)
{
	PLyProcedure *proc = (PLyProcedure *) cfunc;

	Assert(proc->cfunc.use_count == 0);
	Assert(proc->calldepth == 0);

	PLy_procedure_delete(proc);
}

/*
 * MemoryContext callback function
 *
 * We register this in the memory context that contains a PLyProcedureCache
 * struct (that is, the FmgrInfo's fn_mcxt).  When the memory context is reset
 * or deleted, we release the reference count (if any) that the cache holds on
 * the long-lived hash entry.  Note that this will happen even during error
 * aborts.
 *
 * This is also our opportunity to release the Python references held by an
 * interrupted set-returning function.  ShutdownPLyFunction() handles that for
 * routine in-query cancellation cases, but it does not run during an error
 * abort; this callback does, so it is the backstop that prevents leaking the
 * SRF's iterator and saved arguments when a query errors out mid-iteration.
 */
static void
RemovePLyProcedureCache(void *arg)
{
	PLyProcedureCache *pcache = (PLyProcedureCache *) arg;

	/* Release any Python state left behind by an interrupted SRF */
	PLy_function_cleanup_srfstate(pcache);

	/* Release reference count on PLyProcedure */
	if (pcache->proc != NULL)
	{
		Assert(pcache->proc->cfunc.use_count > 0);
		pcache->proc->cfunc.use_count--;
		pcache->proc = NULL;
	}

	/* We needn't free the pcache object itself, context cleanup does that */
}
