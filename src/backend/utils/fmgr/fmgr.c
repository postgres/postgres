/*-------------------------------------------------------------------------
 *
 * fmgr.c
 *	  The Postgres function manager.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/fmgr/fmgr.c,v 1.76 2003/09/25 06:58:05 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/tuptoaster.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "executor/functions.h"
#include "miscadmin.h"
#include "parser/parse_expr.h"
#include "utils/builtins.h"
#include "utils/fmgrtab.h"
#include "utils/lsyscache.h"
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

/*
 * For an oldstyle function, fn_extra points to a record like this:
 */
typedef struct
{
	func_ptr	func;			/* Address of the oldstyle function */
	bool		arg_toastable[FUNC_MAX_ARGS];	/* is n'th arg of a
												 * toastable datatype? */
} Oldstyle_fnextra;


static void fmgr_info_cxt_security(Oid functionId, FmgrInfo *finfo, MemoryContext mcxt,
					   bool ignore_security);
static void fmgr_info_C_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple);
static void fmgr_info_other_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple);
static Datum fmgr_oldstyle(PG_FUNCTION_ARGS);
static Datum fmgr_security_definer(PG_FUNCTION_ARGS);


/*
 * Lookup routines for builtin-function table.	We can search by either Oid
 * or name, but search by Oid is much faster.
 */

static const FmgrBuiltin *
fmgr_isbuiltin(Oid id)
{
	int			low = 0;
	int			high = fmgr_nbuiltins - 1;

	/*
	 * Loop invariant: low is the first index that could contain target
	 * entry, and high is the last index that could contain it.
	 */
	while (low <= high)
	{
		int			i = (high + low) / 2;
		const FmgrBuiltin *ptr = &fmgr_builtins[i];

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
	int			i;

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
 *
 * The caller's CurrentMemoryContext is used as the fn_mcxt of the info
 * struct; this means that any subsidiary data attached to the info struct
 * (either by fmgr_info itself, or later on by a function call handler)
 * will be allocated in that context.  The caller must ensure that this
 * context is at least as long-lived as the info struct itself.  This is
 * not a problem in typical cases where the info struct is on the stack or
 * in freshly-palloc'd space.  However, if one intends to store an info
 * struct in a long-lived table, it's better to use fmgr_info_cxt.
 */
void
fmgr_info(Oid functionId, FmgrInfo *finfo)
{
	fmgr_info_cxt(functionId, finfo, CurrentMemoryContext);
}

/*
 * Fill a FmgrInfo struct, specifying a memory context in which its
 * subsidiary data should go.
 */
void
fmgr_info_cxt(Oid functionId, FmgrInfo *finfo, MemoryContext mcxt)
{
	fmgr_info_cxt_security(functionId, finfo, mcxt, false);
}

/*
 * This one does the actual work.  ignore_security is ordinarily false
 * but is set to true by fmgr_security_definer to avoid infinite
 * recursive lookups.
 */
static void
fmgr_info_cxt_security(Oid functionId, FmgrInfo *finfo, MemoryContext mcxt,
					   bool ignore_security)
{
	const FmgrBuiltin *fbp;
	HeapTuple	procedureTuple;
	Form_pg_proc procedureStruct;
	char	   *prosrc;

	/*
	 * fn_oid *must* be filled in last.  Some code assumes that if fn_oid
	 * is valid, the whole struct is valid.  Some FmgrInfo struct's do
	 * survive elogs.
	 */
	finfo->fn_oid = InvalidOid;
	finfo->fn_extra = NULL;
	finfo->fn_mcxt = mcxt;
	finfo->fn_expr = NULL;		/* caller may set this later */

	if ((fbp = fmgr_isbuiltin(functionId)) != NULL)
	{
		/*
		 * Fast path for builtin functions: don't bother consulting
		 * pg_proc
		 */
		finfo->fn_nargs = fbp->nargs;
		finfo->fn_strict = fbp->strict;
		finfo->fn_retset = fbp->retset;
		finfo->fn_addr = fbp->func;
		finfo->fn_oid = functionId;
		return;
	}

	/* Otherwise we need the pg_proc entry */
	procedureTuple = SearchSysCache(PROCOID,
									ObjectIdGetDatum(functionId),
									0, 0, 0);
	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR, "cache lookup failed for function %u", functionId);
	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	finfo->fn_nargs = procedureStruct->pronargs;
	finfo->fn_strict = procedureStruct->proisstrict;
	finfo->fn_retset = procedureStruct->proretset;

	if (procedureStruct->prosecdef && !ignore_security)
	{
		finfo->fn_addr = fmgr_security_definer;
		finfo->fn_oid = functionId;
		ReleaseSysCache(procedureTuple);
		return;
	}

	switch (procedureStruct->prolang)
	{
		case INTERNALlanguageId:

			/*
			 * For an ordinary builtin function, we should never get here
			 * because the isbuiltin() search above will have succeeded.
			 * However, if the user has done a CREATE FUNCTION to create
			 * an alias for a builtin function, we can end up here.  In
			 * that case we have to look up the function by name.  The
			 * name of the internal function is stored in prosrc (it
			 * doesn't have to be the same as the name of the alias!)
			 */
			prosrc = DatumGetCString(DirectFunctionCall1(textout,
							 PointerGetDatum(&procedureStruct->prosrc)));
			fbp = fmgr_lookupByName(prosrc);
			if (fbp == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
					   errmsg("internal function \"%s\" is not in internal lookup table",
							  prosrc)));
			pfree(prosrc);
			/* Should we check that nargs, strict, retset match the table? */
			finfo->fn_addr = fbp->func;
			break;

		case ClanguageId:
			fmgr_info_C_lang(functionId, finfo, procedureTuple);
			break;

		case SQLlanguageId:
			finfo->fn_addr = fmgr_sql;
			break;

		default:
			fmgr_info_other_lang(functionId, finfo, procedureTuple);
			break;
	}

	finfo->fn_oid = functionId;
	ReleaseSysCache(procedureTuple);
}

/*
 * Special fmgr_info processing for C-language functions.  Note that
 * finfo->fn_oid is not valid yet.
 */
static void
fmgr_info_C_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple)
{
	Form_pg_proc procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);
	Datum		prosrcattr,
				probinattr;
	char	   *prosrcstring,
			   *probinstring;
	void	   *libraryhandle;
	PGFunction	user_fn;
	Pg_finfo_record *inforec;
	Oldstyle_fnextra *fnextra;
	bool		isnull;
	int			i;

	/* Get prosrc and probin strings (link symbol and library filename) */
	prosrcattr = SysCacheGetAttr(PROCOID, procedureTuple,
								 Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc for function %u", functionId);
	prosrcstring = DatumGetCString(DirectFunctionCall1(textout, prosrcattr));

	probinattr = SysCacheGetAttr(PROCOID, procedureTuple,
								 Anum_pg_proc_probin, &isnull);
	if (isnull)
		elog(ERROR, "null probin for function %u", functionId);
	probinstring = DatumGetCString(DirectFunctionCall1(textout, probinattr));

	/* Look up the function itself */
	user_fn = load_external_function(probinstring, prosrcstring, true,
									 &libraryhandle);

	/* Get the function information record (real or default) */
	inforec = fetch_finfo_record(libraryhandle, prosrcstring);

	switch (inforec->api_version)
	{
		case 0:
			/* Old style: need to use a handler */
			finfo->fn_addr = fmgr_oldstyle;
			fnextra = (Oldstyle_fnextra *)
				MemoryContextAlloc(finfo->fn_mcxt, sizeof(Oldstyle_fnextra));
			finfo->fn_extra = (void *) fnextra;
			MemSet(fnextra, 0, sizeof(Oldstyle_fnextra));
			fnextra->func = (func_ptr) user_fn;
			for (i = 0; i < procedureStruct->pronargs; i++)
			{
				fnextra->arg_toastable[i] =
					TypeIsToastable(procedureStruct->proargtypes[i]);
			}
			break;
		case 1:
			/* New style: call directly */
			finfo->fn_addr = user_fn;
			break;
		default:
			/* Shouldn't get here if fetch_finfo_record did its job */
			elog(ERROR, "unrecognized function API version: %d",
				 inforec->api_version);
			break;
	}

	pfree(prosrcstring);
	pfree(probinstring);
}

/*
 * Special fmgr_info processing for other-language functions.  Note
 * that finfo->fn_oid is not valid yet.
 */
static void
fmgr_info_other_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple)
{
	Form_pg_proc procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);
	Oid			language = procedureStruct->prolang;
	HeapTuple	languageTuple;
	Form_pg_language languageStruct;
	FmgrInfo	plfinfo;

	languageTuple = SearchSysCache(LANGOID,
								   ObjectIdGetDatum(language),
								   0, 0, 0);
	if (!HeapTupleIsValid(languageTuple))
		elog(ERROR, "cache lookup failed for language %u", language);
	languageStruct = (Form_pg_language) GETSTRUCT(languageTuple);

	fmgr_info(languageStruct->lanplcallfoid, &plfinfo);
	finfo->fn_addr = plfinfo.fn_addr;

	/*
	 * If lookup of the PL handler function produced nonnull fn_extra,
	 * complain --- it must be an oldstyle function! We no longer support
	 * oldstyle PL handlers.
	 */
	if (plfinfo.fn_extra != NULL)
		elog(ERROR, "language %u has old-style handler", language);

	ReleaseSysCache(languageTuple);
}

/*
 * Fetch and validate the information record for the given external function.
 * The function is specified by a handle for the containing library
 * (obtained from load_external_function) as well as the function name.
 *
 * If no info function exists for the given name, it is not an error.
 * Instead we return a default info record for a version-0 function.
 * We want to raise an error here only if the info function returns
 * something bogus.
 *
 * This function is broken out of fmgr_info_C_lang() so that ProcedureCreate()
 * can validate the information record for a function not yet entered into
 * pg_proc.
 */
Pg_finfo_record *
fetch_finfo_record(void *filehandle, char *funcname)
{
	char	   *infofuncname;
	PGFInfoFunction infofunc;
	Pg_finfo_record *inforec;
	static Pg_finfo_record default_inforec = {0};

	/* Compute name of info func */
	infofuncname = (char *) palloc(strlen(funcname) + 10);
	strcpy(infofuncname, "pg_finfo_");
	strcat(infofuncname, funcname);

	/* Try to look up the info function */
	infofunc = (PGFInfoFunction) lookup_external_function(filehandle,
														  infofuncname);
	if (infofunc == (PGFInfoFunction) NULL)
	{
		/* Not found --- assume version 0 */
		pfree(infofuncname);
		return &default_inforec;
	}

	/* Found, so call it */
	inforec = (*infofunc) ();

	/* Validate result as best we can */
	if (inforec == NULL)
		elog(ERROR, "null result from info function \"%s\"", infofuncname);
	switch (inforec->api_version)
	{
		case 0:
		case 1:
			/* OK, no additional fields to validate */
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized API version %d reported by info function \"%s\"",
							inforec->api_version, infofuncname)));
			break;
	}

	pfree(infofuncname);
	return inforec;
}


/*
 * Copy an FmgrInfo struct
 *
 * This is inherently somewhat bogus since we can't reliably duplicate
 * language-dependent subsidiary info.	We cheat by zeroing fn_extra,
 * instead, meaning that subsidiary info will have to be recomputed.
 */
void
fmgr_info_copy(FmgrInfo *dstinfo, FmgrInfo *srcinfo,
			   MemoryContext destcxt)
{
	memcpy(dstinfo, srcinfo, sizeof(FmgrInfo));
	dstinfo->fn_mcxt = destcxt;
	if (dstinfo->fn_addr == fmgr_oldstyle)
	{
		/* For oldstyle functions we must copy fn_extra */
		Oldstyle_fnextra *fnextra;

		fnextra = (Oldstyle_fnextra *)
			MemoryContextAlloc(destcxt, sizeof(Oldstyle_fnextra));
		memcpy(fnextra, srcinfo->fn_extra, sizeof(Oldstyle_fnextra));
		dstinfo->fn_extra = (void *) fnextra;
	}
	else
		dstinfo->fn_extra = NULL;
}


/*
 * Specialized lookup routine for ProcedureCreate(): given the alleged name
 * of an internal function, return the OID of the function.
 * If the name is not recognized, return InvalidOid.
 */
Oid
fmgr_internal_function(const char *proname)
{
	const FmgrBuiltin *fbp = fmgr_lookupByName(proname);

	if (fbp == NULL)
		return InvalidOid;
	return fbp->foid;
}


/*
 * Handler for old-style "C" language functions
 */
static Datum
fmgr_oldstyle(PG_FUNCTION_ARGS)
{
	Oldstyle_fnextra *fnextra;
	int			n_arguments = fcinfo->nargs;
	int			i;
	bool		isnull;
	func_ptr	user_fn;
	char	   *returnValue;

	if (fcinfo->flinfo == NULL || fcinfo->flinfo->fn_extra == NULL)
		elog(ERROR, "fmgr_oldstyle received NULL pointer");
	fnextra = (Oldstyle_fnextra *) fcinfo->flinfo->fn_extra;

	/*
	 * Result is NULL if any argument is NULL, but we still call the
	 * function (peculiar, but that's the way it worked before, and after
	 * all this is a backwards-compatibility wrapper).	Note, however,
	 * that we'll never get here with NULL arguments if the function is
	 * marked strict.
	 *
	 * We also need to detoast any TOAST-ed inputs, since it's unlikely that
	 * an old-style function knows about TOASTing.
	 */
	isnull = false;
	for (i = 0; i < n_arguments; i++)
	{
		if (PG_ARGISNULL(i))
			isnull = true;
		else if (fnextra->arg_toastable[i])
			fcinfo->arg[i] = PointerGetDatum(PG_DETOAST_DATUM(fcinfo->arg[i]));
	}
	fcinfo->isnull = isnull;

	user_fn = fnextra->func;

	switch (n_arguments)
	{
		case 0:
			returnValue = (*user_fn) ();
			break;
		case 1:

			/*
			 * nullvalue() used to use isNull to check if arg is NULL;
			 * perhaps there are other functions still out there that also
			 * rely on this undocumented hack?
			 */
			returnValue = (*user_fn) (fcinfo->arg[0], &fcinfo->isnull);
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
			 * Increasing FUNC_MAX_ARGS doesn't automatically add cases to
			 * the above code, so mention the actual value in this error
			 * not FUNC_MAX_ARGS.  You could add cases to the above if you
			 * needed to support old-style functions with many arguments,
			 * but making 'em be new-style is probably a better idea.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				   errmsg("function %u has too many arguments (%d, maximum is %d)",
						  fcinfo->flinfo->fn_oid, n_arguments, 16)));
			returnValue = NULL; /* keep compiler quiet */
			break;
	}

	return (Datum) returnValue;
}


/*
 * Support for security definer functions
 */

struct fmgr_security_definer_cache
{
	FmgrInfo	flinfo;
	AclId		userid;
};

/*
 * Function handler for security definer functions.  We extract the
 * OID of the actual function and do a fmgr lookup again.  Then we
 * look up the owner of the function and cache both the fmgr info and
 * the owner ID.  During the call we temporarily replace the flinfo
 * with the cached/looked-up one, while keeping the outer fcinfo
 * (which contains all the actual arguments, etc.) intact.
 */
static Datum
fmgr_security_definer(PG_FUNCTION_ARGS)
{
	Datum		result;
	FmgrInfo   *save_flinfo;
	struct fmgr_security_definer_cache *fcache;
	AclId		save_userid;
	HeapTuple	tuple;

	if (!fcinfo->flinfo->fn_extra)
	{
		fcache = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt, sizeof(*fcache));
		memset(fcache, 0, sizeof(*fcache));

		fmgr_info_cxt_security(fcinfo->flinfo->fn_oid, &fcache->flinfo,
							   fcinfo->flinfo->fn_mcxt, true);

		tuple = SearchSysCache(PROCOID,
							   ObjectIdGetDatum(fcinfo->flinfo->fn_oid),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for function %u",
				 fcinfo->flinfo->fn_oid);
		fcache->userid = ((Form_pg_proc) GETSTRUCT(tuple))->proowner;
		ReleaseSysCache(tuple);

		fcinfo->flinfo->fn_extra = fcache;
	}
	else
		fcache = fcinfo->flinfo->fn_extra;

	save_flinfo = fcinfo->flinfo;
	fcinfo->flinfo = &fcache->flinfo;

	save_userid = GetUserId();
	SetUserId(fcache->userid);
	result = FunctionCallInvoke(fcinfo);
	SetUserId(save_userid);

	fcinfo->flinfo = save_flinfo;

	return result;
}


/*-------------------------------------------------------------------------
 *		Support routines for callers of fmgr-compatible functions
 *
 * NOTE: the simplest way to reliably initialize a FunctionCallInfoData
 * is to MemSet it to zeroes and then fill in the fields that should be
 * nonzero.  However, in a few of the most heavily used paths, we instead
 * just zero the fields that must be zero.	This saves a fair number of
 * cycles so it's worth the extra maintenance effort.  Also see inlined
 * version of FunctionCall2 in utils/sort/tuplesort.c if you need to change
 * these routines!
 *-------------------------------------------------------------------------
 */

/* These are for invocation of a specifically named function with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.	Also, the function cannot be one that needs to
 * look at FmgrInfo, since there won't be any.
 */
Datum
DirectFunctionCall1(PGFunction func, Datum arg1)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	/* MemSet(&fcinfo, 0, sizeof(fcinfo)); */
	fcinfo.flinfo = NULL;
	fcinfo.context = NULL;
	fcinfo.resultinfo = NULL;
	fcinfo.isnull = false;

	fcinfo.nargs = 1;
	fcinfo.arg[0] = arg1;
	fcinfo.argnull[0] = false;

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall2(PGFunction func, Datum arg1, Datum arg2)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	/* MemSet(&fcinfo, 0, sizeof(fcinfo)); */
	fcinfo.flinfo = NULL;
	fcinfo.context = NULL;
	fcinfo.resultinfo = NULL;
	fcinfo.isnull = false;

	fcinfo.nargs = 2;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall3(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.nargs = 3;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall4(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.nargs = 4;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall5(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4, Datum arg5)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.nargs = 5;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall6(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4, Datum arg5,
					Datum arg6)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.nargs = 6;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall7(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4, Datum arg5,
					Datum arg6, Datum arg7)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.nargs = 7;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall8(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4, Datum arg5,
					Datum arg6, Datum arg7, Datum arg8)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

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

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall9(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4, Datum arg5,
					Datum arg6, Datum arg7, Datum arg8,
					Datum arg9)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

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

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}


/* These are for invocation of a previously-looked-up function with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.
 */
Datum
FunctionCall1(FmgrInfo *flinfo, Datum arg1)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	/* MemSet(&fcinfo, 0, sizeof(fcinfo)); */
	fcinfo.context = NULL;
	fcinfo.resultinfo = NULL;
	fcinfo.isnull = false;

	fcinfo.flinfo = flinfo;
	fcinfo.nargs = 1;
	fcinfo.arg[0] = arg1;
	fcinfo.argnull[0] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall2(FmgrInfo *flinfo, Datum arg1, Datum arg2)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	/* MemSet(&fcinfo, 0, sizeof(fcinfo)); */
	fcinfo.context = NULL;
	fcinfo.resultinfo = NULL;
	fcinfo.isnull = false;

	fcinfo.flinfo = flinfo;
	fcinfo.nargs = 2;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall3(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.flinfo = flinfo;
	fcinfo.nargs = 3;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall4(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

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
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall5(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4, Datum arg5)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

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
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall6(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4, Datum arg5,
			  Datum arg6)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

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
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall7(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4, Datum arg5,
			  Datum arg6, Datum arg7)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

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
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall8(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4, Datum arg5,
			  Datum arg6, Datum arg7, Datum arg8)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

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
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall9(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4, Datum arg5,
			  Datum arg6, Datum arg7, Datum arg8,
			  Datum arg9)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

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
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}


/* These are for invocation of a function identified by OID with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.	These are essentially fmgr_info() followed
 * by FunctionCallN().	If the same function is to be invoked repeatedly,
 * do the fmgr_info() once and then use FunctionCallN().
 */
Datum
OidFunctionCall1(Oid functionId, Datum arg1)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

	fmgr_info(functionId, &flinfo);

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.flinfo = &flinfo;
	fcinfo.nargs = 1;
	fcinfo.arg[0] = arg1;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall2(Oid functionId, Datum arg1, Datum arg2)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

	fmgr_info(functionId, &flinfo);

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.flinfo = &flinfo;
	fcinfo.nargs = 2;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall3(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

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
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall4(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

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
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall5(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4, Datum arg5)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

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
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall6(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4, Datum arg5,
				 Datum arg6)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

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
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall7(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4, Datum arg5,
				 Datum arg6, Datum arg7)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

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
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall8(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4, Datum arg5,
				 Datum arg6, Datum arg7, Datum arg8)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

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
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall9(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4, Datum arg5,
				 Datum arg6, Datum arg7, Datum arg8,
				 Datum arg9)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

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
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

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
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	int			n_arguments;
	Datum		result;

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
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				   errmsg("function %u has too many arguments (%d, maximum is %d)",
						  flinfo.fn_oid, n_arguments, FUNC_MAX_ARGS)));
		va_start(pvar, procedureId);
		for (i = 0; i < n_arguments; i++)
			fcinfo.arg[i] = (Datum) va_arg(pvar, char *);
		va_end(pvar);
	}

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

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
#ifndef INT64_IS_BUSTED
	int64	   *retval = (int64 *) palloc(sizeof(int64));

	*retval = X;
	return PointerGetDatum(retval);
#else							/* INT64_IS_BUSTED */

	/*
	 * On a machine with no 64-bit-int C datatype, sizeof(int64) will not
	 * be 8, but we want Int64GetDatum to return an 8-byte object anyway,
	 * with zeroes in the unused bits.	This is needed so that, for
	 * example, hash join of int8 will behave properly.
	 */
	int64	   *retval = (int64 *) palloc0(Max(sizeof(int64), 8));

	*retval = X;
	return PointerGetDatum(retval);
#endif   /* INT64_IS_BUSTED */
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
		Size		len = VARSIZE(datum);
		struct varlena *result = (struct varlena *) palloc(len);

		memcpy(result, datum, len);
		return result;
	}
}

struct varlena *
pg_detoast_datum_slice(struct varlena * datum, int32 first, int32 count)
{
	/* Only get the specified portion from the toast rel */
	return (struct varlena *) heap_tuple_untoast_attr_slice((varattrib *) datum, first, count);
}

/*-------------------------------------------------------------------------
 *		Support routines for extracting info from fn_expr parse tree
 *
 * These are needed by polymorphic functions, which accept multiple possible
 * input types and need help from the parser to know what they've got.
 *-------------------------------------------------------------------------
 */

/*
 * Get the actual type OID of the function return type
 *
 * Returns InvalidOid if information is not available
 */
Oid
get_fn_expr_rettype(FmgrInfo *flinfo)
{
	Node	   *expr;

	/*
	 * can't return anything useful if we have no FmgrInfo or if its
	 * fn_expr node has not been initialized
	 */
	if (!flinfo || !flinfo->fn_expr)
		return InvalidOid;

	expr = flinfo->fn_expr;

	return exprType(expr);
}

/*
 * Get the actual type OID of a specific function argument (counting from 0)
 *
 * Returns InvalidOid if information is not available
 */
Oid
get_fn_expr_argtype(FmgrInfo *flinfo, int argnum)
{
	Node	   *expr;
	List	   *args;
	Oid			argtype;

	/*
	 * can't return anything useful if we have no FmgrInfo or if its
	 * fn_expr node has not been initialized
	 */
	if (!flinfo || !flinfo->fn_expr)
		return InvalidOid;

	expr = flinfo->fn_expr;

	if (IsA(expr, FuncExpr))
		args = ((FuncExpr *) expr)->args;
	else if (IsA(expr, OpExpr))
		args = ((OpExpr *) expr)->args;
	else if (IsA(expr, DistinctExpr))
		args = ((DistinctExpr *) expr)->args;
	else if (IsA(expr, ScalarArrayOpExpr))
		args = ((ScalarArrayOpExpr *) expr)->args;
	else if (IsA(expr, NullIfExpr))
		args = ((NullIfExpr *) expr)->args;
	else
		return InvalidOid;

	if (argnum < 0 || argnum >= length(args))
		return InvalidOid;

	argtype = exprType((Node *) nth(argnum, args));

	/*
	 * special hack for ScalarArrayOpExpr: what the underlying function
	 * will actually get passed is the element type of the array.
	 */
	if (IsA(expr, ScalarArrayOpExpr) &&
		argnum == 1)
		argtype = get_element_type(argtype);

	return argtype;
}
