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
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/Attic/fcache.c,v 1.42 2002/02/18 23:11:25 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/fcache.h"


/*
 * Build a 'FunctionCache' struct given the PG_PROC oid.
 */
FunctionCachePtr
init_fcache(Oid foid, int nargs, MemoryContext fcacheCxt)
{
	FunctionCachePtr retval;

	/* Safety check (should never fail, as parser should check sooner) */
	if (nargs > FUNC_MAX_ARGS)
		elog(ERROR, "init_fcache: too many arguments");

	/* Create fcache entry in the desired context */
	retval = (FunctionCachePtr) MemoryContextAlloc(fcacheCxt,
												   sizeof(FunctionCache));
	MemSet(retval, 0, sizeof(FunctionCache));

	/* Set up the primary fmgr lookup information */
	fmgr_info_cxt(foid, &(retval->func), fcacheCxt);

	/* Initialize additional info */
	retval->setArgsValid = false;

	retval->permission_ok = pg_proc_aclcheck(foid, GetUserId()) == ACLCHECK_OK;

	return retval;
}
