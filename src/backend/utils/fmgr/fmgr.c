/*-------------------------------------------------------------------------
 *
 * fmgr.c
 *	  The Postgres function manager.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/fmgr/fmgr.c,v 1.45 2000/07/06 05:48:13 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "utils/builtins.h"
#include "utils/fmgrtab.h"
#include "utils/syscache.h"

/*
 * Declaration for old-style function pointer type.  This is now used only
 * in fmgr_oldstyle() and is no longer exported.
 *
 * The m68k SVR4 ABI defines that pointers are returned in %a0 instead of
 * %d0. So if a function pointer is declared to return a pointer, the
 * compiler may look only into %a0, but if the called function was declared
 * to return an integer type, it puts its value only into %d0. So the
 * caller doesn't pink up the correct return value. The solution is to
 * declare the function pointer to return int, so the compiler picks up the
 * return value from %d0. (Functions returning pointers put their value
 * *additionally* into %d0 for compatibility.) The price is that there are
 * some warnings about int->pointer conversions...
 */
#if defined(__mc68000__) && defined(__ELF__)
typedef int32 ((*func_ptr) ());
#else
typedef char *((*func_ptr) ());
#endif


static Datum fmgr_oldstyle(PG_FUNCTION_ARGS);
static Datum fmgr_untrusted(PG_FUNCTION_ARGS);
static Datum fmgr_sql(PG_FUNCTION_ARGS);


/*
 * Lookup routines for builtin-function table.  We can search by either Oid
 * or name, but search by Oid is much faster.
 */

static const FmgrBuiltin *
fmgr_isbuiltin(Oid id)
{
	int		low = 0;
	int		high = fmgr_nbuiltins - 1;

	/* Loop invariant: low is the first index that could contain target
	 * entry, and high is the last index that could contain it.
	 */
	while (low <= high)
	{
		int					i = (high + low) / 2;
		const FmgrBuiltin  *ptr = &fmgr_builtins[i];

		if (id == ptr->foid)
			return ptr;
		else if (id > ptr->foid)
			low = i + 1;
		else
			high = i - 1;
	}
	return (const FmgrBuiltin *) NULL;
}

/*
 * Lookup a builtin by name.  Note there can be more than one entry in
 * the array with the same name, but they should all point to the same
 * routine.
 */
static const FmgrBuiltin *
fmgr_lookupByName(const char *name) 
{
	int i;

	for (i = 0; i < fmgr_nbuiltins; i++)
	{
		if (strcmp(name, fmgr_builtins[i].funcName) == 0)
			return fmgr_builtins + i;
    }
	return (const FmgrBuiltin *) NULL;
}

/*
 * This routine fills a FmgrInfo struct, given the OID
 * of the function to be called.
 */
void
fmgr_info(Oid functionId, FmgrInfo *finfo)
{
	const FmgrBuiltin *fbp;
	HeapTuple	procedureTuple;
	Form_pg_proc procedureStruct;
	HeapTuple	languageTuple;
	Form_pg_language languageStruct;
	Oid			language;
	char	   *prosrc;

	finfo->fn_oid = functionId;
	finfo->fn_extra = NULL;

	if ((fbp = fmgr_isbuiltin(functionId)) != NULL)
	{
		/*
		 * Fast path for builtin functions: don't bother consulting pg_proc
		 */
		finfo->fn_nargs = fbp->nargs;
		finfo->fn_strict = fbp->strict;
		if (fbp->oldstyle)
		{
			finfo->fn_addr = fmgr_oldstyle;
			finfo->fn_extra = (void *) fbp->func;
		}
		else
		{
			finfo->fn_addr = fbp->func;
		}
		return;
	}

	/* Otherwise we need the pg_proc entry */
	procedureTuple = SearchSysCacheTuple(PROCOID,
										 ObjectIdGetDatum(functionId),
										 0, 0, 0);
	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR, "fmgr_info: function %u: cache lookup failed",
			 functionId);
	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	finfo->fn_nargs = procedureStruct->pronargs;
	finfo->fn_strict = procedureStruct->proisstrict;

	if (!procedureStruct->proistrusted)
	{
		finfo->fn_addr = fmgr_untrusted;
		return;
	}

	language = procedureStruct->prolang;
	switch (language)
	{
		case INTERNALlanguageId:
		case NEWINTERNALlanguageId:
			/*
			 * For an ordinary builtin function, we should never get
			 * here because the isbuiltin() search above will have
			 * succeeded. However, if the user has done a CREATE
			 * FUNCTION to create an alias for a builtin function, we
			 * can end up here.  In that case we have to look up the
			 * function by name.  The name of the internal function is
			 * stored in prosrc (it doesn't have to be the same as the
			 * name of the alias!)
			 */
			prosrc = DatumGetCString(DirectFunctionCall1(textout,
								PointerGetDatum(&procedureStruct->prosrc)));
			fbp = fmgr_lookupByName(prosrc);
			if (fbp == NULL)
				elog(ERROR, "fmgr_info: function %s not in internal table",
					 prosrc);
			pfree(prosrc);
			if (fbp->oldstyle)
			{
				finfo->fn_addr = fmgr_oldstyle;
				finfo->fn_extra = (void *) fbp->func;
			}
			else
			{
				finfo->fn_addr = fbp->func;
			}
			break;

		case ClanguageId:
			finfo->fn_addr = fmgr_oldstyle;
			finfo->fn_extra = (void *) fmgr_dynamic(functionId);
			break;

		case NEWClanguageId:
			finfo->fn_addr = fmgr_dynamic(functionId);
			break;

		case SQLlanguageId:
			finfo->fn_addr = fmgr_sql;
			break;

		default:
			/*
			 * Might be a created procedural language; try to look it up.
			 */
			languageTuple = SearchSysCacheTuple(LANGOID,
												ObjectIdGetDatum(language),
												0, 0, 0);
			if (!HeapTupleIsValid(languageTuple))
			{
				elog(ERROR, "fmgr_info: cache lookup for language %u failed",
					 language);
			}
			languageStruct = (Form_pg_language) GETSTRUCT(languageTuple);
			if (languageStruct->lanispl)
			{
				FmgrInfo	plfinfo;

				fmgr_info(languageStruct->lanplcallfoid, &plfinfo);
				finfo->fn_addr = plfinfo.fn_addr;
				/*
				 * If lookup of the PL handler function produced nonnull
				 * fn_extra, complain --- it must be an oldstyle function!
				 * We no longer support oldstyle PL handlers.
				 */
				if (plfinfo.fn_extra != NULL)
					elog(ERROR, "fmgr_info: language %u has old-style handler",
						 language);
			}
			else
			{
				elog(ERROR, "fmgr_info: function %u: unsupported language %u",
					 functionId, language);
			}
			break;
	}
}


/*
 * Specialized lookup routine for pg_proc.c: given the alleged name of
 * an internal function, return the OID of the function's language.
 * If the name is not known, return InvalidOid.
 */
Oid
fmgr_internal_language(const char *proname)
{
	const FmgrBuiltin *fbp = fmgr_lookupByName(proname);

	if (fbp == NULL)
		return InvalidOid;
	return fbp->oldstyle ? INTERNALlanguageId : NEWINTERNALlanguageId;
}


/*
 * Handler for old-style internal and "C" language functions
 *
 * We expect fmgr_info to have placed the old-style function's address
 * in fn_extra of *flinfo.  This is a bit of a hack since fn_extra is really
 * void * which might be a different size than a pointer to function, but
 * it will work on any machine that our old-style call interface works on...
 */
static Datum
fmgr_oldstyle(PG_FUNCTION_ARGS)
{
	char	   *returnValue = NULL;
	int			n_arguments = fcinfo->nargs;
	int			i;
	bool		isnull;
	func_ptr	user_fn;

	if (fcinfo->flinfo == NULL || fcinfo->flinfo->fn_extra == NULL)
		elog(ERROR, "Internal error: fmgr_oldstyle received NULL function pointer");

	/*
	 * Result is NULL if any argument is NULL, but we still call the function
	 * (peculiar, but that's the way it worked before, and after all this is
	 * a backwards-compatibility wrapper).  Note, however, that we'll never
	 * get here with NULL arguments if the function is marked strict.
	 */
	isnull = false;
	for (i = 0; i < n_arguments; i++)
		isnull |= PG_ARGISNULL(i);
	fcinfo->isnull = isnull;

	user_fn = (func_ptr) fcinfo->flinfo->fn_extra;

	switch (n_arguments)
	{
		case 0:
			returnValue = (*user_fn) ();
			break;
		case 1:
			/*
			 * nullvalue() used to use isNull to check if arg is NULL;
			 * perhaps there are other functions still out there that
			 * also rely on this undocumented hack?
			 */
			returnValue = (*user_fn) (fcinfo->arg[0], & fcinfo->isnull);
			break;
		case 2:
			returnValue = (*user_fn) (fcinfo->arg[0], fcinfo->arg[1]);
			break;
		case 3:
			returnValue = (*user_fn) (fcinfo->arg[0], fcinfo->arg[1],
									  fcinfo->arg[2]);
			break;
		case 4:
			returnValue = (*user_fn) (fcinfo->arg[0], fcinfo->arg[1],
									  fcinfo->arg[2], fcinfo->arg[3]);
			break;
		case 5:
			returnValue = (*user_fn) (fcinfo->arg[0], fcinfo->arg[1],
									  fcinfo->arg[2], fcinfo->arg[3],
									  fcinfo->arg[4]);
			break;
		case 6:
			returnValue = (*user_fn) (fcinfo->arg[0], fcinfo->arg[1],
									  fcinfo->arg[2], fcinfo->arg[3],
									  fcinfo->arg[4], fcinfo->arg[5]);
			break;
		case 7:
			returnValue = (*user_fn) (fcinfo->arg[0], fcinfo->arg[1],
									  fcinfo->arg[2], fcinfo->arg[3],
									  fcinfo->arg[4], fcinfo->arg[5],
									  fcinfo->arg[6]);
			break;
		case 8:
			returnValue = (*user_fn) (fcinfo->arg[0], fcinfo->arg[1],
									  fcinfo->arg[2], fcinfo->arg[3],
									  fcinfo->arg[4], fcinfo->arg[5],
									  fcinfo->arg[6], fcinfo->arg[7]);
			break;
		case 9:
			returnValue = (*user_fn) (fcinfo->arg[0], fcinfo->arg[1],
									  fcinfo->arg[2], fcinfo->arg[3],
									  fcinfo->arg[4], fcinfo->arg[5],
									  fcinfo->arg[6], fcinfo->arg[7],
									  fcinfo->arg[8]);
			break;
		case 10:
			returnValue = (*user_fn) (fcinfo->arg[0], fcinfo->arg[1],
									  fcinfo->arg[2], fcinfo->arg[3],
									  fcinfo->arg[4], fcinfo->arg[5],
									  fcinfo->arg[6], fcinfo->arg[7],
									  fcinfo->arg[8], fcinfo->arg[9]);
			break;
		case 11:
			returnValue = (*user_fn) (fcinfo->arg[0], fcinfo->arg[1],
									  fcinfo->arg[2], fcinfo->arg[3],
									  fcinfo->arg[4], fcinfo->arg[5],
									  fcinfo->arg[6], fcinfo->arg[7],
									  fcinfo->arg[8], fcinfo->arg[9],
									  fcinfo->arg[10]);
			break;
		case 12:
			returnValue = (*user_fn) (fcinfo->arg[0], fcinfo->arg[1],
									  fcinfo->arg[2], fcinfo->arg[3],
									  fcinfo->arg[4], fcinfo->arg[5],
									  fcinfo->arg[6], fcinfo->arg[7],
									  fcinfo->arg[8], fcinfo->arg[9],
									  fcinfo->arg[10], fcinfo->arg[11]);
			break;
		case 13:
			returnValue = (*user_fn) (fcinfo->arg[0], fcinfo->arg[1],
									  fcinfo->arg[2], fcinfo->arg[3],
									  fcinfo->arg[4], fcinfo->arg[5],
									  fcinfo->arg[6], fcinfo->arg[7],
									  fcinfo->arg[8], fcinfo->arg[9],
									  fcinfo->arg[10], fcinfo->arg[11],
									  fcinfo->arg[12]);
			break;
		case 14:
			returnValue = (*user_fn) (fcinfo->arg[0], fcinfo->arg[1],
									  fcinfo->arg[2], fcinfo->arg[3],
									  fcinfo->arg[4], fcinfo->arg[5],
									  fcinfo->arg[6], fcinfo->arg[7],
									  fcinfo->arg[8], fcinfo->arg[9],
									  fcinfo->arg[10], fcinfo->arg[11],
									  fcinfo->arg[12], fcinfo->arg[13]);
			break;
		case 15:
			returnValue = (*user_fn) (fcinfo->arg[0], fcinfo->arg[1],
									  fcinfo->arg[2], fcinfo->arg[3],
									  fcinfo->arg[4], fcinfo->arg[5],
									  fcinfo->arg[6], fcinfo->arg[7],
									  fcinfo->arg[8], fcinfo->arg[9],
									  fcinfo->arg[10], fcinfo->arg[11],
									  fcinfo->arg[12], fcinfo->arg[13],
									  fcinfo->arg[14]);
			break;
		case 16:
			returnValue = (*user_fn) (fcinfo->arg[0], fcinfo->arg[1],
									  fcinfo->arg[2], fcinfo->arg[3],
									  fcinfo->arg[4], fcinfo->arg[5],
									  fcinfo->arg[6], fcinfo->arg[7],
									  fcinfo->arg[8], fcinfo->arg[9],
									  fcinfo->arg[10], fcinfo->arg[11],
									  fcinfo->arg[12], fcinfo->arg[13],
									  fcinfo->arg[14], fcinfo->arg[15]);
			break;
		default:
			/*
			 * Increasing FUNC_MAX_ARGS doesn't automatically add cases
			 * to the above code, so mention the actual value in this error
			 * not FUNC_MAX_ARGS.  You could add cases to the above if you
			 * needed to support old-style functions with many arguments,
			 * but making 'em be new-style is probably a better idea.
			 */
			elog(ERROR, "fmgr_oldstyle: function %u: too many arguments (%d > %d)",
				 fcinfo->flinfo->fn_oid, n_arguments, 16);
			break;
	}

	return (Datum) returnValue;
}


/*
 * Handler for all functions marked "untrusted"
 */
static Datum
fmgr_untrusted(PG_FUNCTION_ARGS)
{
	/*
	 * Currently these are unsupported.  Someday we might do something
	 * like forking a subprocess to execute 'em.
	 */
	elog(ERROR, "Untrusted functions not supported");
	return 0;					/* keep compiler happy */
}

/*
 * Handler for SQL-language functions
 */
static Datum
fmgr_sql(PG_FUNCTION_ARGS)
{
	/*
	 * XXX It'd be really nice to support SQL functions anywhere that
	 * builtins are supported.	What would we have to do?  What pitfalls
	 * are there?
	 */
	elog(ERROR, "SQL-language function not supported in this context");
	return 0;					/* keep compiler happy */
}


/*-------------------------------------------------------------------------
 *		Support routines for callers of fmgr-compatible functions
 *-------------------------------------------------------------------------
 */

/* These are for invocation of a specifically named function with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.  Also, the function cannot be one that needs to
 * look at FmgrInfo, since there won't be any.
 */
Datum
DirectFunctionCall1(PGFunction func, Datum arg1)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.nargs = 1;
	fcinfo.arg[0] = arg1;

	result = (* func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "DirectFunctionCall1: function %p returned NULL",
			 (void *) func);

	return result;
}

Datum
DirectFunctionCall2(PGFunction func, Datum arg1, Datum arg2)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.nargs = 2;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;

	result = (* func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "DirectFunctionCall2: function %p returned NULL",
			 (void *) func);

	return result;
}

Datum
DirectFunctionCall3(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.nargs = 3;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;

	result = (* func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "DirectFunctionCall3: function %p returned NULL",
			 (void *) func);

	return result;
}

Datum
DirectFunctionCall4(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.nargs = 4;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;

	result = (* func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "DirectFunctionCall4: function %p returned NULL",
			 (void *) func);

	return result;
}

Datum
DirectFunctionCall5(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4, Datum arg5)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.nargs = 5;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;

	result = (* func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "DirectFunctionCall5: function %p returned NULL",
			 (void *) func);

	return result;
}

Datum
DirectFunctionCall6(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4, Datum arg5,
					Datum arg6)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.nargs = 6;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;

	result = (* func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "DirectFunctionCall6: function %p returned NULL",
			 (void *) func);

	return result;
}

Datum
DirectFunctionCall7(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4, Datum arg5,
					Datum arg6, Datum arg7)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.nargs = 7;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;

	result = (* func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "DirectFunctionCall7: function %p returned NULL",
			 (void *) func);

	return result;
}

Datum
DirectFunctionCall8(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4, Datum arg5,
					Datum arg6, Datum arg7, Datum arg8)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.nargs = 8;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;
	fcinfo.arg[7] = arg8;

	result = (* func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "DirectFunctionCall8: function %p returned NULL",
			 (void *) func);

	return result;
}

Datum
DirectFunctionCall9(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4, Datum arg5,
					Datum arg6, Datum arg7, Datum arg8,
					Datum arg9)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.nargs = 9;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;
	fcinfo.arg[7] = arg8;
	fcinfo.arg[8] = arg9;

	result = (* func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "DirectFunctionCall9: function %p returned NULL",
			 (void *) func);

	return result;
}


/* These are for invocation of a previously-looked-up function with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.
 */
Datum
FunctionCall1(FmgrInfo *flinfo, Datum arg1)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = flinfo;
	fcinfo.nargs = 1;
	fcinfo.arg[0] = arg1;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "FunctionCall1: function %u returned NULL",
			 fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall2(FmgrInfo *flinfo, Datum arg1, Datum arg2)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = flinfo;
	fcinfo.nargs = 2;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "FunctionCall2: function %u returned NULL",
			 fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall3(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = flinfo;
	fcinfo.nargs = 3;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "FunctionCall3: function %u returned NULL",
			 fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall4(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = flinfo;
	fcinfo.nargs = 4;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "FunctionCall4: function %u returned NULL",
			 fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall5(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4, Datum arg5)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = flinfo;
	fcinfo.nargs = 5;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "FunctionCall5: function %u returned NULL",
			 fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall6(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4, Datum arg5,
			  Datum arg6)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = flinfo;
	fcinfo.nargs = 6;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "FunctionCall6: function %u returned NULL",
			 fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall7(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4, Datum arg5,
			  Datum arg6, Datum arg7)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = flinfo;
	fcinfo.nargs = 7;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "FunctionCall7: function %u returned NULL",
			 fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall8(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4, Datum arg5,
			  Datum arg6, Datum arg7, Datum arg8)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = flinfo;
	fcinfo.nargs = 8;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;
	fcinfo.arg[7] = arg8;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "FunctionCall8: function %u returned NULL",
			 fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall9(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4, Datum arg5,
			  Datum arg6, Datum arg7, Datum arg8,
			  Datum arg9)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = flinfo;
	fcinfo.nargs = 9;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;
	fcinfo.arg[7] = arg8;
	fcinfo.arg[8] = arg9;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "FunctionCall9: function %u returned NULL",
			 fcinfo.flinfo->fn_oid);

	return result;
}


/* These are for invocation of a function identified by OID with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.  These are essentially fmgr_info() followed
 * by FunctionCallN().  If the same function is to be invoked repeatedly,
 * do the fmgr_info() once and then use FunctionCallN().
 */
Datum
OidFunctionCall1(Oid functionId, Datum arg1)
{
	FmgrInfo				flinfo;
	FunctionCallInfoData	fcinfo;
	Datum					result;

	fmgr_info(functionId, &flinfo);

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = &flinfo;
	fcinfo.nargs = 1;
	fcinfo.arg[0] = arg1;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "OidFunctionCall1: function %u returned NULL",
			 flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall2(Oid functionId, Datum arg1, Datum arg2)
{
	FmgrInfo				flinfo;
	FunctionCallInfoData	fcinfo;
	Datum					result;

	fmgr_info(functionId, &flinfo);

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = &flinfo;
	fcinfo.nargs = 2;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "OidFunctionCall2: function %u returned NULL",
			 flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall3(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3)
{
	FmgrInfo				flinfo;
	FunctionCallInfoData	fcinfo;
	Datum					result;

	fmgr_info(functionId, &flinfo);

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = &flinfo;
	fcinfo.nargs = 3;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "OidFunctionCall3: function %u returned NULL",
			 flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall4(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4)
{
	FmgrInfo				flinfo;
	FunctionCallInfoData	fcinfo;
	Datum					result;

	fmgr_info(functionId, &flinfo);

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = &flinfo;
	fcinfo.nargs = 4;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "OidFunctionCall4: function %u returned NULL",
			 flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall5(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4, Datum arg5)
{
	FmgrInfo				flinfo;
	FunctionCallInfoData	fcinfo;
	Datum					result;

	fmgr_info(functionId, &flinfo);

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = &flinfo;
	fcinfo.nargs = 5;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "OidFunctionCall5: function %u returned NULL",
			 flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall6(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4, Datum arg5,
				 Datum arg6)
{
	FmgrInfo				flinfo;
	FunctionCallInfoData	fcinfo;
	Datum					result;

	fmgr_info(functionId, &flinfo);

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = &flinfo;
	fcinfo.nargs = 6;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "OidFunctionCall6: function %u returned NULL",
			 flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall7(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4, Datum arg5,
				 Datum arg6, Datum arg7)
{
	FmgrInfo				flinfo;
	FunctionCallInfoData	fcinfo;
	Datum					result;

	fmgr_info(functionId, &flinfo);

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = &flinfo;
	fcinfo.nargs = 7;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "OidFunctionCall7: function %u returned NULL",
			 flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall8(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4, Datum arg5,
				 Datum arg6, Datum arg7, Datum arg8)
{
	FmgrInfo				flinfo;
	FunctionCallInfoData	fcinfo;
	Datum					result;

	fmgr_info(functionId, &flinfo);

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = &flinfo;
	fcinfo.nargs = 8;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;
	fcinfo.arg[7] = arg8;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "OidFunctionCall8: function %u returned NULL",
			 flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall9(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4, Datum arg5,
				 Datum arg6, Datum arg7, Datum arg8,
				 Datum arg9)
{
	FmgrInfo				flinfo;
	FunctionCallInfoData	fcinfo;
	Datum					result;

	fmgr_info(functionId, &flinfo);

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = &flinfo;
	fcinfo.nargs = 9;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;
	fcinfo.arg[7] = arg8;
	fcinfo.arg[8] = arg9;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "OidFunctionCall9: function %u returned NULL",
			 flinfo.fn_oid);

	return result;
}


/*
 * !!! OLD INTERFACE !!!
 *
 * fmgr() is the only remaining vestige of the old-style caller support
 * functions.  It's no longer used anywhere in the Postgres distribution,
 * but we should leave it around for a release or two to ease the transition
 * for user-supplied C functions.  OidFunctionCallN() replaces it for new
 * code.
 *
 * DEPRECATED, DO NOT USE IN NEW CODE
 */
char *
fmgr(Oid procedureId,...)
{
	FmgrInfo				flinfo;
	FunctionCallInfoData	fcinfo;
	int						n_arguments;
	Datum					result;

	fmgr_info(procedureId, &flinfo);

	MemSet(&fcinfo, 0, sizeof(fcinfo));
    fcinfo.flinfo = &flinfo;
	fcinfo.nargs = flinfo.fn_nargs;
	n_arguments = fcinfo.nargs;

	if (n_arguments > 0)
	{
		va_list		pvar;
		int			i;

		if (n_arguments > FUNC_MAX_ARGS)
			elog(ERROR, "fmgr: function %u: too many arguments (%d > %d)",
				 flinfo.fn_oid, n_arguments, FUNC_MAX_ARGS);
		va_start(pvar, procedureId);
		for (i = 0; i < n_arguments; i++)
			fcinfo.arg[i] = (Datum) va_arg(pvar, char *);
		va_end(pvar);
	}

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "fmgr: function %u returned NULL",
			 flinfo.fn_oid);

	return (char *) result;
}


/*-------------------------------------------------------------------------
 *		Support routines for standard pass-by-reference datatypes
 *
 * Note: at some point, at least on some platforms, these might become
 * pass-by-value types.  Obviously Datum must be >= 8 bytes to allow
 * int64 or float8 to be pass-by-value.  I think that Float4GetDatum
 * and Float8GetDatum will need to be out-of-line routines anyway,
 * since just casting from float to Datum will not do the right thing;
 * some kind of trick with pointer-casting or a union will be needed.
 *-------------------------------------------------------------------------
 */

Datum
Int64GetDatum(int64 X)
{
	int64	   *retval = (int64 *) palloc(sizeof(int64));

	*retval = X;
	return PointerGetDatum(retval);
}

Datum
Float4GetDatum(float4 X)
{
	float4	   *retval = (float4 *) palloc(sizeof(float4));

	*retval = X;
	return PointerGetDatum(retval);
}

Datum
Float8GetDatum(float8 X)
{
	float8	   *retval = (float8 *) palloc(sizeof(float8));

	*retval = X;
	return PointerGetDatum(retval);
}

/*-------------------------------------------------------------------------
 *		Support routines for toastable datatypes
 *-------------------------------------------------------------------------
 */

struct varlena *
pg_detoast_datum(struct varlena * datum)
{
	if (VARATT_IS_EXTENDED(datum))
		return (struct varlena *) heap_tuple_untoast_attr((varattrib *) datum);
	else
		return datum;
}

struct varlena *
pg_detoast_datum_copy(struct varlena * datum)
{
	if (VARATT_IS_EXTENDED(datum))
		return (struct varlena *) heap_tuple_untoast_attr((varattrib *) datum);
	else
	{
		/* Make a modifiable copy of the varlena object */
		Size			len = VARSIZE(datum);
		struct varlena *result = (struct varlena *) palloc(len);

		memcpy(result, datum, len);
		return result;
	}
}
