/*-------------------------------------------------------------------------
 *
 * fcache.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fcache.h,v 1.6 1998/01/15 19:46:36 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FCACHE_H
#define FCACHE_H

#include <fmgr.h>


typedef struct
{
	int			typlen;			/* length of the return type */
	int			typbyval;		/* true if return type is pass by value */
	FmgrInfo	func;			/* address of function to call (for c
								 * funcs) */
	Oid			foid;			/* oid of the function in pg_proc */
	Oid			language;		/* oid of the language in pg_language */
	int			nargs;			/* number of arguments */

	/* Might want to make these two arrays of size MAXFUNCARGS */

	Oid		   *argOidVect;		/* oids of all the arguments */
	bool	   *nullVect;		/* keep track of null arguments */

	char	   *src;			/* source code of the function */
	char	   *bin;			/* binary object code ?? */
	char	   *func_state;		/* fuction_state struct for execution */

	bool		oneResult;		/* true we only want 1 result from the
								 * function */
	bool		hasSetArg;		/* true if func is part of a nested dot
								 * expr whose argument is func returning a
								 * set ugh! */

	Pointer		funcSlot;		/* if one result we need to copy it before
								 * we end execution of the function and
								 * free stuff */

	char	   *setArg;			/* current argument for nested dot
								 * execution Nested dot expressions mean
								 * we have funcs whose argument is a set
								 * of tuples */

	bool		istrusted;		/* trusted fn? */
} FunctionCache,
		   *FunctionCachePtr;

#endif							/* FCACHE_H */
