/*-------------------------------------------------------------------------
 *
 * fcache.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fcache.h,v 1.11 2000/05/28 17:56:20 tgl Exp $
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
