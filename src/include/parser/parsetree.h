/*-------------------------------------------------------------------------
 *
 * parsetree.h
 *	  Routines to access various components and subcomponents of
 *	  parse trees.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parsetree.h,v 1.11 2000/09/12 21:07:12 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSETREE_H
#define PARSETREE_H

#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"

/* ----------------
 *		need pg_list.h for definitions of nth(), etc.
 * ----------------
 */

/* ----------------
 *		range table macros
 * ----------------
 */

/*
 *		rt_fetch
 *		rt_store
 *
 *		Access and (destructively) replace rangetable entries.
 *
 */
#define rt_fetch(rangetable_index, rangetable) \
	((RangeTblEntry*) nth((rangetable_index)-1, rangetable))

#define rt_store(rangetable_index, rangetable, rt) \
	set_nth(rangetable, (rangetable_index)-1, rt)

/*
 *		getrelid
 *
 *		Given the range index of a relation, return the corresponding
 *		relation OID.
 */
#define getrelid(rangeindex,rangetable) \
	(rt_fetch(rangeindex, rangetable)->relid)

#endif	 /* PARSETREE_H */
