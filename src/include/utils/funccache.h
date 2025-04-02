/*-------------------------------------------------------------------------
 *
 * funccache.h
 *	  Function cache definitions.
 *
 * See funccache.c for comments.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/funccache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FUNCCACHE_H
#define FUNCCACHE_H

#include "access/htup_details.h"
#include "fmgr.h"
#include "storage/itemptr.h"

struct CachedFunctionHashKey;	/* forward references */
struct CachedFunction;

/*
 * Callback that cached_function_compile() invokes when it's necessary to
 * compile a cached function.  The callback must fill in *function (except
 * for the fields of struct CachedFunction), or throw an error if trouble.
 *	fcinfo: current call information
 *	procTup: function's pg_proc row from catcache
 *	hashkey: hash key that will be used for the function
 *	function: pre-zeroed workspace, of size passed to cached_function_compile()
 *	forValidator: passed through from cached_function_compile()
 */
typedef void (*CachedFunctionCompileCallback) (FunctionCallInfo fcinfo,
											   HeapTuple procTup,
											   const struct CachedFunctionHashKey *hashkey,
											   struct CachedFunction *function,
											   bool forValidator);

/*
 * Callback called when discarding a cache entry.  Free any free-able
 * subsidiary data of cfunc, but not the struct CachedFunction itself.
 */
typedef void (*CachedFunctionDeleteCallback) (struct CachedFunction *cfunc);

/*
 * Hash lookup key for functions.  This must account for all aspects
 * of a specific call that might lead to different data types or
 * collations being used within the function.
 */
typedef struct CachedFunctionHashKey
{
	Oid			funcOid;

	bool		isTrigger;		/* true if called as a DML trigger */
	bool		isEventTrigger; /* true if called as an event trigger */

	/* be careful that pad bytes in this struct get zeroed! */

	/*
	 * We include the language-specific size of the function's cache entry in
	 * the cache key.  This covers the case where CREATE OR REPLACE FUNCTION
	 * is used to change the implementation language, and the new language
	 * also uses funccache.c but needs a different-sized cache entry.
	 */
	Size		cacheEntrySize;

	/*
	 * For a trigger function, the OID of the trigger is part of the hash key
	 * --- we want to compile the trigger function separately for each trigger
	 * it is used with, in case the rowtype or transition table names are
	 * different.  Zero if not called as a DML trigger.
	 */
	Oid			trigOid;

	/*
	 * We must include the input collation as part of the hash key too,
	 * because we have to generate different plans (with different Param
	 * collations) for different collation settings.
	 */
	Oid			inputCollation;

	/* Number of arguments (counting input arguments only, ie pronargs) */
	int			nargs;

	/* If you change anything below here, fix hashing code in funccache.c! */

	/*
	 * If relevant, the result descriptor for a function returning composite.
	 */
	TupleDesc	callResultType;

	/*
	 * Input argument types, with any polymorphic types resolved to actual
	 * types.  Only the first nargs entries are valid.
	 */
	Oid			argtypes[FUNC_MAX_ARGS];
} CachedFunctionHashKey;

/*
 * Representation of a compiled function.  This struct contains just the
 * fields that funccache.c needs to deal with.  It will typically be
 * embedded in a larger struct containing function-language-specific data.
 */
typedef struct CachedFunction
{
	/* back-link to hashtable entry, or NULL if not in hash table */
	CachedFunctionHashKey *fn_hashkey;
	/* xmin and ctid of function's pg_proc row; used to detect invalidation */
	TransactionId fn_xmin;
	ItemPointerData fn_tid;
	/* deletion callback */
	CachedFunctionDeleteCallback dcallback;

	/* this field changes when the function is used: */
	uint64		use_count;
} CachedFunction;

extern CachedFunction *cached_function_compile(FunctionCallInfo fcinfo,
											   CachedFunction *function,
											   CachedFunctionCompileCallback ccallback,
											   CachedFunctionDeleteCallback dcallback,
											   Size cacheEntrySize,
											   bool includeResultType,
											   bool forValidator);
extern void cfunc_resolve_polymorphic_argtypes(int numargs,
											   Oid *argtypes,
											   char *argmodes,
											   Node *call_expr,
											   bool forValidator,
											   const char *proname);

#endif							/* FUNCCACHE_H */
