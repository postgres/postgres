/*
 * Python procedure manipulation for plpython
 *
 * src/pl/plpython/plpy_procedure.c
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "plpy_elog.h"
#include "plpy_main.h"
#include "plpy_procedure.h"
#include "plpython.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

static HTAB *PLy_procedure_cache = NULL;

static PLyProcedure *PLy_procedure_create(HeapTuple procTup, Oid fn_oid, bool is_trigger);
static bool PLy_procedure_valid(PLyProcedure *proc, HeapTuple procTup);
static char *PLy_procedure_munge_source(const char *name, const char *src);


void
init_procedure_caches(void)
{
	HASHCTL		hash_ctl;

	hash_ctl.keysize = sizeof(PLyProcedureKey);
	hash_ctl.entrysize = sizeof(PLyProcedureEntry);
	PLy_procedure_cache = hash_create("PL/Python procedures", 32, &hash_ctl,
									  HASH_ELEM | HASH_BLOBS);
}

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
 * PLy_procedure_get: returns a cached PLyProcedure, or creates, stores and
 * returns a new PLyProcedure.
 *
 * fn_oid is the OID of the function requested
 * fn_rel is InvalidOid or the relation this function triggers on
 * is_trigger denotes whether the function is a trigger function
 *
 * The reason that both fn_rel and is_trigger need to be passed is that when
 * trigger functions get validated we don't know which relation(s) they'll
 * be used with, so no sensible fn_rel can be passed.
 */
PLyProcedure *
PLy_procedure_get(Oid fn_oid, Oid fn_rel, bool is_trigger)
{
	bool		use_cache = !(is_trigger && fn_rel == InvalidOid);
	HeapTuple	procTup;
	PLyProcedureKey key;
	PLyProcedureEntry *volatile entry = NULL;
	PLyProcedure *volatile proc = NULL;
	bool		found = false;

	procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fn_oid));
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);

	/*
	 * Look for the function in the cache, unless we don't have the necessary
	 * information (e.g. during validation). In that case we just don't cache
	 * anything.
	 */
	if (use_cache)
	{
		key.fn_oid = fn_oid;
		key.fn_rel = fn_rel;
		entry = hash_search(PLy_procedure_cache, &key, HASH_ENTER, &found);
		proc = entry->proc;
	}

	PG_TRY();
	{
		if (!found)
		{
			/* Haven't found it, create a new procedure */
			proc = PLy_procedure_create(procTup, fn_oid, is_trigger);
			if (use_cache)
				entry->proc = proc;
		}
		else if (!PLy_procedure_valid(proc, procTup))
		{
			/* Found it, but it's invalid, free and reuse the cache entry */
			entry->proc = NULL;
			if (proc)
				PLy_procedure_delete(proc);
			proc = PLy_procedure_create(procTup, fn_oid, is_trigger);
			entry->proc = proc;
		}
		/* Found it and it's valid, it's fine to use it */
	}
	PG_CATCH();
	{
		/* Do not leave an uninitialized entry in the cache */
		if (use_cache)
			hash_search(PLy_procedure_cache, &key, HASH_REMOVE, NULL);
		PG_RE_THROW();
	}
	PG_END_TRY();

	ReleaseSysCache(procTup);

	return proc;
}

/*
 * Create a new PLyProcedure structure
 */
static PLyProcedure *
PLy_procedure_create(HeapTuple procTup, Oid fn_oid, bool is_trigger)
{
	char		procName[NAMEDATALEN + 256];
	Form_pg_proc procStruct;
	PLyProcedure *volatile proc;
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

	proc = (PLyProcedure *) palloc0(sizeof(PLyProcedure));
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
		proc->fn_xmin = HeapTupleHeaderGetRawXmin(procTup->t_data);
		proc->fn_tid = procTup->t_self;
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
		if (!is_trigger)
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
			proc->argnames = (char **) palloc0(sizeof(char *) * proc->nargs);
			proc->args = (PLyDatumToOb *) palloc0(sizeof(PLyDatumToOb) * proc->nargs);

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
	return proc;
}

/*
 * Insert the procedure into the Python interpreter
 */
void
PLy_procedure_compile(PLyProcedure *proc, const char *src)
{
	PyObject   *crv = NULL;
	char	   *msrc;

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
	crv = PyRun_String(msrc, Py_file_input, proc->globals, NULL);
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

/*
 * Decide whether a cached PLyProcedure struct is still valid
 */
static bool
PLy_procedure_valid(PLyProcedure *proc, HeapTuple procTup)
{
	if (proc == NULL)
		return false;

	/* If the pg_proc tuple has changed, it's not valid */
	if (!(proc->fn_xmin == HeapTupleHeaderGetRawXmin(procTup->t_data) &&
		  ItemPointerEquals(&proc->fn_tid, &procTup->t_self)))
		return false;

	return true;
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
