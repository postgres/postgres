/*-------------------------------------------------------------------------
 *
 * fcache.h
 *		Declarations for function cache records.
 *
 * The first time any Oper or Func node is evaluated, we compute a cache
 * record for the function being invoked, and save a pointer to the cache
 * record in the Oper or Func node.  This saves repeated lookup of info
 * about the function.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fcache.h,v 1.15 2001/01/24 19:43:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FCACHE_H
#define FCACHE_H

#include "fmgr.h"
#include "nodes/execnodes.h"

/*
 * A FunctionCache record is built for all functions regardless of language.
 *
 * We store the fmgr lookup info to avoid recomputing it on each call.
 * We also store a prebuilt FunctionCallInfo struct.  When evaluating a
 * function-returning-set, fcinfo holds the argument values across calls
 * so that we need not re-evaluate the arguments for each call.  Even for
 * non-set functions, fcinfo saves a few cycles per call by allowing us to
 * avoid redundant setup of its fields.
 */

typedef struct FunctionCache
{
	/*
	 * Function manager's lookup info for the target function.
	 */
	FmgrInfo	func;
	/*
	 * Per-call info for calling the target function.  Unvarying fields
	 * are set up by init_fcache().  Argument values are filled in as needed.
	 */
	FunctionCallInfoData fcinfo;
	/*
	 * "Resultinfo" node --- used only if target function returns a set.
	 */
	ReturnSetInfo rsinfo;
	/*
	 * argsValid is true when we are evaluating a set-valued function and
	 * we are in the middle of a call series; we want to pass the same
	 * argument values to the function again (and again, until it returns
	 * ExprEndResult).
	 */
	bool		argsValid;		/* TRUE if fcinfo contains valid arguments */
	/*
	 * hasSetArg is true if we found a set-valued argument to the function.
	 * This causes the function result to be a set as well.
	 */
	bool		hasSetArg;		/* some argument returns a set */
} FunctionCache;


extern FunctionCachePtr init_fcache(Oid foid, int nargs,
									MemoryContext fcacheCxt);

#endif	 /* FCACHE_H */
