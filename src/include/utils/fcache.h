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
 * $Id: fcache.h,v 1.20 2001/11/05 17:46:36 momjian Exp $
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
 *
 * We also need to store argument values across calls when evaluating a
 * function-returning-set.	This is pretty ugly (and not re-entrant);
 * current-evaluation info should be somewhere in the econtext, not in
 * the querytree.  As it stands, a function-returning-set can't safely be
 * recursive, at least not if it's in plpgsql which will try to re-use
 * the querytree at multiple execution nesting levels.	FIXME someday.
 */

typedef struct FunctionCache
{
	/*
	 * Function manager's lookup info for the target function.
	 */
	FmgrInfo	func;

	/*
	 * setArgsValid is true when we are evaluating a set-valued function
	 * and we are in the middle of a call series; we want to pass the same
	 * argument values to the function again (and again, until it returns
	 * ExprEndResult).
	 */
	bool		setArgsValid;

	/*
	 * Flag to remember whether we found a set-valued argument to the
	 * function. This causes the function result to be a set as well.
	 * Valid only when setArgsValid is true.
	 */
	bool		setHasSetArg;	/* some argument returns a set */

	/*
	 * Current argument data for a set-valued function; contains valid
	 * data only if setArgsValid is true.
	 */
	FunctionCallInfoData setArgs;
} FunctionCache;


extern FunctionCachePtr init_fcache(Oid foid, int nargs,
			MemoryContext fcacheCxt);

#endif   /* FCACHE_H */
