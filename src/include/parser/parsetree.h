/*-------------------------------------------------------------------------
 *
 * parsetree.h--
 *    Routines to access various components and subcomponents of
 *    parse trees.  
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parsetree.h,v 1.1 1996/08/28 07:23:57 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSETREE_H
#define PARSETREE_H		/* include once only */

/* ----------------
 *	need pg_list.h for definitions of CAR(), etc. macros
 * ----------------
 */
#include "nodes/pg_list.h"
#include "nodes/parsenodes.h"

/* ----------------
 *	range table macros
 *
 *  parse tree:
 *	(root targetlist qual)
 *	 ^^^^
 *  parse root:
 *	(numlevels cmdtype resrel rangetable priority ruleinfo nestdotinfo)
 *			          ^^^^^^^^^^
 *  range table:
 *	(rtentry ...)
 *
 *  rtentry:
 *	note: this might be wrong, I don't understand how
 *	rt_time / rt_archive_time work together.  anyways it
 *      looks something like:
 *
 *	   (relname ?       relid timestuff flags rulelocks)
 *	or (new/cur relname relid timestuff flags rulelocks)
 *
 *	someone who knows more should correct this -cim 6/9/91
 * ----------------
 */

#define rt_relname(rt_entry) \
      ((!strcmp(((rt_entry)->refname),"*CURRENT*") ||\
        !strcmp(((rt_entry)->refname),"*NEW*")) ? ((rt_entry)->refname) : \
        ((char *)(rt_entry)->relname))

/*
 *	rt_fetch
 *	rt_store
 *
 *	Access and (destructively) replace rangetable entries.
 *
 */
#define rt_fetch(rangetable_index, rangetable) \
    ((RangeTblEntry*)nth((rangetable_index)-1, rangetable))

#define rt_store(rangetable_index, rangetable, rt) \
    set_nth(rangetable, (rangetable_index)-1, rt)

/*
 *	getrelid
 *	getrelname
 *
 *	Given the range index of a relation, return the corresponding
 *	relation id or relation name.
 */
#define getrelid(rangeindex,rangetable) \
    ((RangeTblEntry*)nth((rangeindex)-1, rangetable))->relid

#define getrelname(rangeindex, rangetable) \
    rt_relname((RangeTblEntry*)nth((rangeindex)-1, rangetable))

#endif /* PARSETREE_H */
	     
