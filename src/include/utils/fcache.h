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
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fcache.h,v 1.12 2000/07/12 02:37:35 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FCACHE_H
#define FCACHE_H

#include "fmgr.h"


typedef struct
{
	FmgrInfo	func;			/* info for fmgr call mechanism */
	Oid			foid;			/* oid of the function in pg_proc */
	Oid			language;		/* oid of the language in pg_language */
	int			typlen;			/* length of the return type */
	bool		typbyval;		/* true if return type is pass by value */

	bool		oneResult;		/* true we only want 1 result from the
								 * function */
	bool		hasSetArg;		/* true if func is part of a nested dot
								 * expr whose argument is func returning a
								 * set ugh! */

	/* If additional info is added to an existing fcache, be sure to
	 * allocate it in the fcacheCxt.
	 */
	MemoryContext fcacheCxt;	/* context the fcache lives in */

	int			nargs;			/* actual number of arguments */
	Oid		   *argOidVect;		/* oids of all the argument types */

	char	   *src;			/* source code of the function */
	char	   *bin;			/* binary object code ?? */
	char	   *func_state;		/* function_state struct for execution */

	Pointer		funcSlot;		/* if one result we need to copy it before
								 * we end execution of the function and
								 * free stuff */

	Datum		setArg;			/* current argument for nested dot
								 * execution Nested dot expressions mean
								 * we have funcs whose argument is a set
								 * of tuples */
} FunctionCache;

typedef FunctionCache *FunctionCachePtr;

#endif	 /* FCACHE_H */
