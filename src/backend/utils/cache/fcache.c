/*-------------------------------------------------------------------------
 *
 * fcache.c
 *	  Code for the 'function cache' used in Oper and Func nodes.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/Attic/fcache.c,v 1.40 2001/09/21 00:11:31 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/fcache.h"


/*
 * Build a 'FunctionCache' struct given the PG_PROC oid.
 */
FunctionCachePtr
init_fcache(Oid foid, int nargs, MemoryContext fcacheCxt)
{
	MemoryContext oldcontext;
	FunctionCachePtr retval;

	/* Safety check (should never fail, as parser should check sooner) */
	if (nargs > FUNC_MAX_ARGS)
		elog(ERROR, "init_fcache: too many arguments");

	/* Switch to a context long-lived enough for the fcache entry */
	oldcontext = MemoryContextSwitchTo(fcacheCxt);

	retval = (FunctionCachePtr) palloc(sizeof(FunctionCache));
	MemSet(retval, 0, sizeof(FunctionCache));

	/* Set up the primary fmgr lookup information */
	fmgr_info(foid, &(retval->func));

	/* Initialize additional info */
	retval->setArgsValid = false;

	MemoryContextSwitchTo(oldcontext);

	return retval;
}
