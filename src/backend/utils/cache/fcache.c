/*-------------------------------------------------------------------------
 *
 * fcache.c
 *	  Code for the 'function cache' used in Oper and Func nodes.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/Attic/fcache.c,v 1.45 2002/06/20 20:29:39 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/fcache.h"
#include "utils/lsyscache.h"


/*
 * Build a 'FunctionCache' struct given the PG_PROC oid.
 */
FunctionCachePtr
init_fcache(Oid foid, int nargs, MemoryContext fcacheCxt)
{
	FunctionCachePtr retval;
	AclResult	aclresult;

	/* Check permission to call function */
	aclresult = pg_proc_aclcheck(foid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_func_name(foid));

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

	return retval;
}
