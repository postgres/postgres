/*
 * Python procedure manipulation for plpython
 *
 * src/pl/plpython/plpy_procedure.c
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/transam.h"
#include "funcapi.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/syscache.h"

#include "plpython.h"

#include "plpy_procedure.h"

#include "plpy_elog.h"
#include "plpy_main.h"


static HTAB *PLy_procedure_cache = NULL;

static PLyProcedure *PLy_procedure_create(HeapTuple procTup, Oid fn_oid, bool is_trigger);
static bool PLy_procedure_argument_valid(PLyTypeInfo *arg);
static bool PLy_procedure_valid(PLyProcedure *proc, HeapTuple procTup);
static char *PLy_procedure_munge_source(const char *name, const char *src);


void
init_procedure_caches(void)
{
	HASHCTL		hash_ctl;

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(PLyProcedureKey);
	hash_ctl.entrysize = sizeof(PLyProcedureEntry);
	hash_ctl.hash = tag_hash;
	PLy_procedure_cache = hash_create("PL/Python procedures", 32, &hash_ctl,
									  HASH_ELEM | HASH_FUNCTION);
}

/*
 * Get the name of the last procedure called by the backend (the
 * innermost, if a plpython procedure call calls the backend and the
 * backend calls another plpython procedure).
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
			PLy_procedure_delete(proc);
			PLy_free(proc);
			proc = PLy_procedure_create(procTup, fn_oid, is_trigger);
			entry->proc = proc;
		}
		/* Found it and it's valid, it's fine to use it */
	}
	PG_CATCH();
	{
		/* Do not leave an uninitialised entry in the cache */
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
	char	   *volatile procSource = NULL;
	Datum		prosrcdatum;
	bool		isnull;
	int			i,
				rv;

	procStruct = (Form_pg_proc) GETSTRUCT(procTup);
	rv = snprintf(procName, sizeof(procName),
				  "__plpython_procedure_%s_%u",
				  NameStr(procStruct->proname),
				  fn_oid);
	if (rv >= sizeof(procName) || rv < 0)
		elog(ERROR, "procedure name would overrun buffer");

	proc = PLy_malloc(sizeof(PLyProcedure));
	proc->proname = PLy_strdup(NameStr(procStruct->proname));
	proc->pyname = PLy_strdup(procName);
	proc->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
	proc->fn_tid = procTup->t_self;
	/* Remember if function is STABLE/IMMUTABLE */
	proc->fn_readonly =
		(procStruct->provolatile != PROVOLATILE_VOLATILE);
	PLy_typeinfo_init(&proc->result);
	for (i = 0; i < FUNC_MAX_ARGS; i++)
		PLy_typeinfo_init(&proc->args[i]);
	proc->nargs = 0;
	proc->code = proc->statics = NULL;
	proc->globals = NULL;
	proc->is_setof = procStruct->proretset;
	proc->setof = NULL;
	proc->src = NULL;
	proc->argnames = NULL;

	PG_TRY();
	{
		/*
		 * get information required for output conversion of the return value,
		 * but only if this isn't a trigger.
		 */
		if (!is_trigger)
		{
			HeapTuple	rvTypeTup;
			Form_pg_type rvTypeStruct;

			rvTypeTup = SearchSysCache1(TYPEOID,
								   ObjectIdGetDatum(procStruct->prorettype));
			if (!HeapTupleIsValid(rvTypeTup))
				elog(ERROR, "cache lookup failed for type %u",
					 procStruct->prorettype);
			rvTypeStruct = (Form_pg_type) GETSTRUCT(rvTypeTup);

			/* Disallow pseudotype result, except for void or record */
			if (rvTypeStruct->typtype == TYPTYPE_PSEUDO)
			{
				if (procStruct->prorettype == TRIGGEROID)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("trigger functions can only be called as triggers")));
				else if (procStruct->prorettype != VOIDOID &&
						 procStruct->prorettype != RECORDOID)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						  errmsg("PL/Python functions cannot return type %s",
								 format_type_be(procStruct->prorettype))));
			}

			if (rvTypeStruct->typtype == TYPTYPE_COMPOSITE ||
				procStruct->prorettype == RECORDOID)
			{
				/*
				 * Tuple: set up later, during first call to
				 * PLy_function_handler
				 */
				proc->result.out.d.typoid = procStruct->prorettype;
				proc->result.out.d.typmod = -1;
				proc->result.is_rowtype = 2;
			}
			else
			{
				/* do the real work */
				PLy_output_datum_func(&proc->result, rvTypeTup);
			}

			ReleaseSysCache(rvTypeTup);
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
			int			i,
						pos,
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

			proc->argnames = (char **) PLy_malloc0(sizeof(char *) * proc->nargs);
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

				/* check argument type is OK, set up I/O function info */
				switch (argTypeStruct->typtype)
				{
					case TYPTYPE_PSEUDO:
						/* Disallow pseudotype argument */
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						  errmsg("PL/Python functions cannot accept type %s",
								 format_type_be(types[i]))));
						break;
					case TYPTYPE_COMPOSITE:
						/* we'll set IO funcs at first call */
						proc->args[pos].is_rowtype = 2;
						break;
					default:
						PLy_input_datum_func(&(proc->args[pos]),
											 types[i],
											 argTypeTup);
						break;
				}

				/* get argument name */
				proc->argnames[pos] = names ? PLy_strdup(names[i]) : NULL;

				ReleaseSysCache(argTypeTup);

				pos++;
			}
		}

		/*
		 * get the text of the function.
		 */
		prosrcdatum = SysCacheGetAttr(PROCOID, procTup,
									  Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "null prosrc");
		procSource = TextDatumGetCString(prosrcdatum);

		PLy_procedure_compile(proc, procSource);

		pfree(procSource);
		procSource = NULL;
	}
	PG_CATCH();
	{
		PLy_procedure_delete(proc);
		if (procSource)
			pfree(procSource);

		PG_RE_THROW();
	}
	PG_END_TRY();

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
	PyDict_SetItemString(proc->globals, "SD", proc->statics);

	/*
	 * insert the function code into the interpreter
	 */
	msrc = PLy_procedure_munge_source(proc->pyname, src);
	/* Save the mangled source for later inclusion in tracebacks */
	proc->src = PLy_strdup(msrc);
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
	int			i;

	Py_XDECREF(proc->code);
	Py_XDECREF(proc->statics);
	Py_XDECREF(proc->globals);
	if (proc->proname)
		PLy_free(proc->proname);
	if (proc->pyname)
		PLy_free(proc->pyname);
	for (i = 0; i < proc->nargs; i++)
	{
		if (proc->args[i].is_rowtype == 1)
		{
			if (proc->args[i].in.r.atts)
				PLy_free(proc->args[i].in.r.atts);
			if (proc->args[i].out.r.atts)
				PLy_free(proc->args[i].out.r.atts);
		}
		if (proc->argnames && proc->argnames[i])
			PLy_free(proc->argnames[i]);
	}
	if (proc->src)
		PLy_free(proc->src);
	if (proc->argnames)
		PLy_free(proc->argnames);
}

/*
 * Check if our cached information about a datatype is still valid
 */
static bool
PLy_procedure_argument_valid(PLyTypeInfo *arg)
{
	HeapTuple	relTup;
	bool		valid;

	/* Nothing to cache unless type is composite */
	if (arg->is_rowtype != 1)
		return true;

	/*
	 * Zero typ_relid means that we got called on an output argument of a
	 * function returning a unnamed record type; the info for it can't change.
	 */
	if (!OidIsValid(arg->typ_relid))
		return true;

	/* Else we should have some cached data */
	Assert(TransactionIdIsValid(arg->typrel_xmin));
	Assert(ItemPointerIsValid(&arg->typrel_tid));

	/* Get the pg_class tuple for the data type */
	relTup = SearchSysCache1(RELOID, ObjectIdGetDatum(arg->typ_relid));
	if (!HeapTupleIsValid(relTup))
		elog(ERROR, "cache lookup failed for relation %u", arg->typ_relid);

	/* If it has changed, the cached data is not valid */
	valid = (arg->typrel_xmin == HeapTupleHeaderGetXmin(relTup->t_data) &&
			 ItemPointerEquals(&arg->typrel_tid, &relTup->t_self));

	ReleaseSysCache(relTup);

	return valid;
}

/*
 * Decide whether a cached PLyProcedure struct is still valid
 */
static bool
PLy_procedure_valid(PLyProcedure *proc, HeapTuple procTup)
{
	int			i;
	bool		valid;

	Assert(proc != NULL);

	/* If the pg_proc tuple has changed, it's not valid */
	if (!(proc->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
		  ItemPointerEquals(&proc->fn_tid, &procTup->t_self)))
		return false;

	/* Else check the input argument datatypes */
	valid = true;
	for (i = 0; i < proc->nargs; i++)
	{
		valid = PLy_procedure_argument_valid(&proc->args[i]);

		/* Short-circuit on first changed argument */
		if (!valid)
			break;
	}

	/* if the output type is composite, it might have changed */
	if (valid)
		valid = PLy_procedure_argument_valid(&proc->result);

	return valid;
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
		elog(FATAL, "buffer overrun in PLy_munge_source");

	return mrc;
}
