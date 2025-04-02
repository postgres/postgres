/*-------------------------------------------------------------------------
 *
 * funccache.c
 *	  Function cache management.
 *
 * funccache.c manages a cache of function execution data.  The cache
 * is used by SQL-language and PL/pgSQL functions, and could be used by
 * other function languages.  Each cache entry is specific to the execution
 * of a particular function (identified by OID) with specific input data
 * types; so a polymorphic function could have many associated cache entries.
 * Trigger functions similarly have a cache entry per trigger.  These rules
 * allow the cached data to be specific to the particular data types the
 * function call will be dealing with.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/funccache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_proc.h"
#include "commands/event_trigger.h"
#include "commands/trigger.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "utils/funccache.h"
#include "utils/hsearch.h"
#include "utils/syscache.h"


/*
 * Hash table for cached functions
 */
static HTAB *cfunc_hashtable = NULL;

typedef struct CachedFunctionHashEntry
{
	CachedFunctionHashKey key;	/* hash key, must be first */
	CachedFunction *function;	/* points to data of language-specific size */
} CachedFunctionHashEntry;

#define FUNCS_PER_USER		128 /* initial table size */

static uint32 cfunc_hash(const void *key, Size keysize);
static int	cfunc_match(const void *key1, const void *key2, Size keysize);


/*
 * Initialize the hash table on first use.
 *
 * The hash table will be in TopMemoryContext regardless of caller's context.
 */
static void
cfunc_hashtable_init(void)
{
	HASHCTL		ctl;

	/* don't allow double-initialization */
	Assert(cfunc_hashtable == NULL);

	ctl.keysize = sizeof(CachedFunctionHashKey);
	ctl.entrysize = sizeof(CachedFunctionHashEntry);
	ctl.hash = cfunc_hash;
	ctl.match = cfunc_match;
	cfunc_hashtable = hash_create("Cached function hash",
								  FUNCS_PER_USER,
								  &ctl,
								  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);
}

/*
 * cfunc_hash: hash function for cfunc hash table
 *
 * We need special hash and match functions to deal with the optional
 * presence of a TupleDesc in the hash keys.  As long as we have to do
 * that, we might as well also be smart about not comparing unused
 * elements of the argtypes arrays.
 */
static uint32
cfunc_hash(const void *key, Size keysize)
{
	const CachedFunctionHashKey *k = (const CachedFunctionHashKey *) key;
	uint32		h;

	Assert(keysize == sizeof(CachedFunctionHashKey));
	/* Hash all the fixed fields except callResultType */
	h = DatumGetUInt32(hash_any((const unsigned char *) k,
								offsetof(CachedFunctionHashKey, callResultType)));
	/* Incorporate input argument types */
	if (k->nargs > 0)
		h = hash_combine(h,
						 DatumGetUInt32(hash_any((const unsigned char *) k->argtypes,
												 k->nargs * sizeof(Oid))));
	/* Incorporate callResultType if present */
	if (k->callResultType)
		h = hash_combine(h, hashRowType(k->callResultType));
	return h;
}

/*
 * cfunc_match: match function to use with cfunc_hash
 */
static int
cfunc_match(const void *key1, const void *key2, Size keysize)
{
	const CachedFunctionHashKey *k1 = (const CachedFunctionHashKey *) key1;
	const CachedFunctionHashKey *k2 = (const CachedFunctionHashKey *) key2;

	Assert(keysize == sizeof(CachedFunctionHashKey));
	/* Compare all the fixed fields except callResultType */
	if (memcmp(k1, k2, offsetof(CachedFunctionHashKey, callResultType)) != 0)
		return 1;				/* not equal */
	/* Compare input argument types (we just verified that nargs matches) */
	if (k1->nargs > 0 &&
		memcmp(k1->argtypes, k2->argtypes, k1->nargs * sizeof(Oid)) != 0)
		return 1;				/* not equal */
	/* Compare callResultType */
	if (k1->callResultType)
	{
		if (k2->callResultType)
		{
			if (!equalRowTypes(k1->callResultType, k2->callResultType))
				return 1;		/* not equal */
		}
		else
			return 1;			/* not equal */
	}
	else
	{
		if (k2->callResultType)
			return 1;			/* not equal */
	}
	return 0;					/* equal */
}

/*
 * Look up the CachedFunction for the given hash key.
 * Returns NULL if not present.
 */
static CachedFunction *
cfunc_hashtable_lookup(CachedFunctionHashKey *func_key)
{
	CachedFunctionHashEntry *hentry;

	if (cfunc_hashtable == NULL)
		return NULL;

	hentry = (CachedFunctionHashEntry *) hash_search(cfunc_hashtable,
													 func_key,
													 HASH_FIND,
													 NULL);
	if (hentry)
		return hentry->function;
	else
		return NULL;
}

/*
 * Insert a hash table entry.
 */
static void
cfunc_hashtable_insert(CachedFunction *function,
					   CachedFunctionHashKey *func_key)
{
	CachedFunctionHashEntry *hentry;
	bool		found;

	if (cfunc_hashtable == NULL)
		cfunc_hashtable_init();

	hentry = (CachedFunctionHashEntry *) hash_search(cfunc_hashtable,
													 func_key,
													 HASH_ENTER,
													 &found);
	if (found)
		elog(WARNING, "trying to insert a function that already exists");

	/*
	 * If there's a callResultType, copy it into TopMemoryContext.  If we're
	 * unlucky enough for that to fail, leave the entry with null
	 * callResultType, which will probably never match anything.
	 */
	if (func_key->callResultType)
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);

		hentry->key.callResultType = NULL;
		hentry->key.callResultType = CreateTupleDescCopy(func_key->callResultType);
		MemoryContextSwitchTo(oldcontext);
	}

	hentry->function = function;

	/* Set back-link from function to hashtable key */
	function->fn_hashkey = &hentry->key;
}

/*
 * Delete a hash table entry.
 */
static void
cfunc_hashtable_delete(CachedFunction *function)
{
	CachedFunctionHashEntry *hentry;
	TupleDesc	tupdesc;

	/* do nothing if not in table */
	if (function->fn_hashkey == NULL)
		return;

	/*
	 * We need to free the callResultType if present, which is slightly tricky
	 * because it has to be valid during the hashtable search.  Fortunately,
	 * because we have the hashkey back-link, we can grab that pointer before
	 * deleting the hashtable entry.
	 */
	tupdesc = function->fn_hashkey->callResultType;

	hentry = (CachedFunctionHashEntry *) hash_search(cfunc_hashtable,
													 function->fn_hashkey,
													 HASH_REMOVE,
													 NULL);
	if (hentry == NULL)
		elog(WARNING, "trying to delete function that does not exist");

	/* Remove back link, which no longer points to allocated storage */
	function->fn_hashkey = NULL;

	/* Release the callResultType if present */
	if (tupdesc)
		FreeTupleDesc(tupdesc);
}

/*
 * Compute the hashkey for a given function invocation
 *
 * The hashkey is returned into the caller-provided storage at *hashkey.
 * Note however that if a callResultType is incorporated, we've not done
 * anything about copying that.
 */
static void
compute_function_hashkey(FunctionCallInfo fcinfo,
						 Form_pg_proc procStruct,
						 CachedFunctionHashKey *hashkey,
						 Size cacheEntrySize,
						 bool includeResultType,
						 bool forValidator)
{
	/* Make sure pad bytes within fixed part of the struct are zero */
	memset(hashkey, 0, offsetof(CachedFunctionHashKey, argtypes));

	/* get function OID */
	hashkey->funcOid = fcinfo->flinfo->fn_oid;

	/* get call context */
	hashkey->isTrigger = CALLED_AS_TRIGGER(fcinfo);
	hashkey->isEventTrigger = CALLED_AS_EVENT_TRIGGER(fcinfo);

	/* record cacheEntrySize so multiple languages can share hash table */
	hashkey->cacheEntrySize = cacheEntrySize;

	/*
	 * If DML trigger, include trigger's OID in the hash, so that each trigger
	 * usage gets a different hash entry, allowing for e.g. different relation
	 * rowtypes or transition table names.  In validation mode we do not know
	 * what relation or transition table names are intended to be used, so we
	 * leave trigOid zero; the hash entry built in this case will never be
	 * used for any actual calls.
	 *
	 * We don't currently need to distinguish different event trigger usages
	 * in the same way, since the special parameter variables don't vary in
	 * type in that case.
	 */
	if (hashkey->isTrigger && !forValidator)
	{
		TriggerData *trigdata = (TriggerData *) fcinfo->context;

		hashkey->trigOid = trigdata->tg_trigger->tgoid;
	}

	/* get input collation, if known */
	hashkey->inputCollation = fcinfo->fncollation;

	/*
	 * We include only input arguments in the hash key, since output argument
	 * types can be deduced from those, and it would require extra cycles to
	 * include the output arguments.  But we have to resolve any polymorphic
	 * argument types to the real types for the call.
	 */
	if (procStruct->pronargs > 0)
	{
		hashkey->nargs = procStruct->pronargs;
		memcpy(hashkey->argtypes, procStruct->proargtypes.values,
			   procStruct->pronargs * sizeof(Oid));
		cfunc_resolve_polymorphic_argtypes(procStruct->pronargs,
										   hashkey->argtypes,
										   NULL,	/* all args are inputs */
										   fcinfo->flinfo->fn_expr,
										   forValidator,
										   NameStr(procStruct->proname));
	}

	/*
	 * While regular OUT arguments are sufficiently represented by the
	 * resolved input arguments, a function returning composite has additional
	 * variability: ALTER TABLE/ALTER TYPE could affect what it returns. Also,
	 * a function returning RECORD may depend on a column definition list to
	 * determine its output rowtype.  If the caller needs the exact result
	 * type to be part of the hash lookup key, we must run
	 * get_call_result_type() to find that out.
	 */
	if (includeResultType)
	{
		Oid			resultTypeId;
		TupleDesc	tupdesc;

		switch (get_call_result_type(fcinfo, &resultTypeId, &tupdesc))
		{
			case TYPEFUNC_COMPOSITE:
			case TYPEFUNC_COMPOSITE_DOMAIN:
				hashkey->callResultType = tupdesc;
				break;
			default:
				/* scalar result, or indeterminate rowtype */
				break;
		}
	}
}

/*
 * This is the same as the standard resolve_polymorphic_argtypes() function,
 * except that:
 * 1. We go ahead and report the error if we can't resolve the types.
 * 2. We treat RECORD-type input arguments (not output arguments) as if
 *    they were polymorphic, replacing their types with the actual input
 *    types if we can determine those.  This allows us to create a separate
 *    function cache entry for each named composite type passed to such an
 *    argument.
 * 3. In validation mode, we have no inputs to look at, so assume that
 *    polymorphic arguments are integer, integer-array or integer-range.
 */
void
cfunc_resolve_polymorphic_argtypes(int numargs,
								   Oid *argtypes, char *argmodes,
								   Node *call_expr, bool forValidator,
								   const char *proname)
{
	int			i;

	if (!forValidator)
	{
		int			inargno;

		/* normal case, pass to standard routine */
		if (!resolve_polymorphic_argtypes(numargs, argtypes, argmodes,
										  call_expr))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not determine actual argument "
							"type for polymorphic function \"%s\"",
							proname)));
		/* also, treat RECORD inputs (but not outputs) as polymorphic */
		inargno = 0;
		for (i = 0; i < numargs; i++)
		{
			char		argmode = argmodes ? argmodes[i] : PROARGMODE_IN;

			if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				continue;
			if (argtypes[i] == RECORDOID || argtypes[i] == RECORDARRAYOID)
			{
				Oid			resolvedtype = get_call_expr_argtype(call_expr,
																 inargno);

				if (OidIsValid(resolvedtype))
					argtypes[i] = resolvedtype;
			}
			inargno++;
		}
	}
	else
	{
		/* special validation case (no need to do anything for RECORD) */
		for (i = 0; i < numargs; i++)
		{
			switch (argtypes[i])
			{
				case ANYELEMENTOID:
				case ANYNONARRAYOID:
				case ANYENUMOID:	/* XXX dubious */
				case ANYCOMPATIBLEOID:
				case ANYCOMPATIBLENONARRAYOID:
					argtypes[i] = INT4OID;
					break;
				case ANYARRAYOID:
				case ANYCOMPATIBLEARRAYOID:
					argtypes[i] = INT4ARRAYOID;
					break;
				case ANYRANGEOID:
				case ANYCOMPATIBLERANGEOID:
					argtypes[i] = INT4RANGEOID;
					break;
				case ANYMULTIRANGEOID:
					argtypes[i] = INT4MULTIRANGEOID;
					break;
				default:
					break;
			}
		}
	}
}

/*
 * delete_function - clean up as much as possible of a stale function cache
 *
 * We can't release the CachedFunction struct itself, because of the
 * possibility that there are fn_extra pointers to it.  We can release
 * the subsidiary storage, but only if there are no active evaluations
 * in progress.  Otherwise we'll just leak that storage.  Since the
 * case would only occur if a pg_proc update is detected during a nested
 * recursive call on the function, a leak seems acceptable.
 *
 * Note that this can be called more than once if there are multiple fn_extra
 * pointers to the same function cache.  Hence be careful not to do things
 * twice.
 */
static void
delete_function(CachedFunction *func)
{
	/* remove function from hash table (might be done already) */
	cfunc_hashtable_delete(func);

	/* release the function's storage if safe and not done already */
	if (func->use_count == 0 &&
		func->dcallback != NULL)
	{
		func->dcallback(func);
		func->dcallback = NULL;
	}
}

/*
 * Compile a cached function, if no existing cache entry is suitable.
 *
 * fcinfo is the current call information.
 *
 * function should be NULL or the result of a previous call of
 * cached_function_compile() for the same fcinfo.  The caller will
 * typically save the result in fcinfo->flinfo->fn_extra, or in a
 * field of a struct pointed to by fn_extra, to re-use in later
 * calls within the same query.
 *
 * ccallback and dcallback are function-language-specific callbacks to
 * compile and delete a cached function entry.  dcallback can be NULL
 * if there's nothing for it to do.
 *
 * cacheEntrySize is the function-language-specific size of the cache entry
 * (which embeds a CachedFunction struct and typically has many more fields
 * after that).
 *
 * If includeResultType is true and the function returns composite,
 * include the actual result descriptor in the cache lookup key.
 *
 * If forValidator is true, we're only compiling for validation purposes,
 * and so some checks are skipped.
 *
 * Note: it's important for this to fall through quickly if the function
 * has already been compiled.
 *
 * Note: this function leaves the "use_count" field as zero.  The caller
 * is expected to increment the use_count and decrement it when done with
 * the cache entry.
 */
CachedFunction *
cached_function_compile(FunctionCallInfo fcinfo,
						CachedFunction *function,
						CachedFunctionCompileCallback ccallback,
						CachedFunctionDeleteCallback dcallback,
						Size cacheEntrySize,
						bool includeResultType,
						bool forValidator)
{
	Oid			funcOid = fcinfo->flinfo->fn_oid;
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	CachedFunctionHashKey hashkey;
	bool		function_valid = false;
	bool		hashkey_valid = false;

	/*
	 * Lookup the pg_proc tuple by Oid; we'll need it in any case
	 */
	procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcOid));
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", funcOid);
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/*
	 * Do we already have a cache entry for the current FmgrInfo?  If not, try
	 * to find one in the hash table.
	 */
recheck:
	if (!function)
	{
		/* Compute hashkey using function signature and actual arg types */
		compute_function_hashkey(fcinfo, procStruct, &hashkey,
								 cacheEntrySize, includeResultType,
								 forValidator);
		hashkey_valid = true;

		/* And do the lookup */
		function = cfunc_hashtable_lookup(&hashkey);
	}

	if (function)
	{
		/* We have a compiled function, but is it still valid? */
		if (function->fn_xmin == HeapTupleHeaderGetRawXmin(procTup->t_data) &&
			ItemPointerEquals(&function->fn_tid, &procTup->t_self))
			function_valid = true;
		else
		{
			/*
			 * Nope, so remove it from hashtable and try to drop associated
			 * storage (if not done already).
			 */
			delete_function(function);

			/*
			 * If the function isn't in active use then we can overwrite the
			 * func struct with new data, allowing any other existing fn_extra
			 * pointers to make use of the new definition on their next use.
			 * If it is in use then just leave it alone and make a new one.
			 * (The active invocations will run to completion using the
			 * previous definition, and then the cache entry will just be
			 * leaked; doesn't seem worth adding code to clean it up, given
			 * what a corner case this is.)
			 *
			 * If we found the function struct via fn_extra then it's possible
			 * a replacement has already been made, so go back and recheck the
			 * hashtable.
			 */
			if (function->use_count != 0)
			{
				function = NULL;
				if (!hashkey_valid)
					goto recheck;
			}
		}
	}

	/*
	 * If the function wasn't found or was out-of-date, we have to compile it.
	 */
	if (!function_valid)
	{
		/*
		 * Calculate hashkey if we didn't already; we'll need it to store the
		 * completed function.
		 */
		if (!hashkey_valid)
			compute_function_hashkey(fcinfo, procStruct, &hashkey,
									 cacheEntrySize, includeResultType,
									 forValidator);

		/*
		 * Create the new function struct, if not done already.  The function
		 * structs are never thrown away, so keep them in TopMemoryContext.
		 */
		Assert(cacheEntrySize >= sizeof(CachedFunction));
		if (function == NULL)
		{
			function = (CachedFunction *)
				MemoryContextAllocZero(TopMemoryContext, cacheEntrySize);
		}
		else
		{
			/* re-using a previously existing struct, so clear it out */
			memset(function, 0, cacheEntrySize);
		}

		/*
		 * Fill in the CachedFunction part.  fn_hashkey and use_count remain
		 * zeroes for now.
		 */
		function->fn_xmin = HeapTupleHeaderGetRawXmin(procTup->t_data);
		function->fn_tid = procTup->t_self;
		function->dcallback = dcallback;

		/*
		 * Do the hard, language-specific part.
		 */
		ccallback(fcinfo, procTup, &hashkey, function, forValidator);

		/*
		 * Add the completed struct to the hash table.
		 */
		cfunc_hashtable_insert(function, &hashkey);
	}

	ReleaseSysCache(procTup);

	/*
	 * Finally return the compiled function
	 */
	return function;
}
