/*-------------------------------------------------------------------------
 *
 * parsetree.h
 *	  Routines to access various components and subcomponents of
 *	  parse trees.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parsetree.h,v 1.13 2001/01/24 19:43:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSETREE_H
#define PARSETREE_H

#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"		/* for nth(), etc */


/* ----------------
 *		range table macros
 * ----------------
 */

/*
 *		rt_fetch
 *		rt_store
 *
 *		Access and (destructively) replace rangetable entries.
 */
#define rt_fetch(rangetable_index, rangetable) \
	((RangeTblEntry *) nth((rangetable_index)-1, rangetable))

#define rt_store(rangetable_index, rangetable, rt) \
	set_nth(rangetable, (rangetable_index)-1, rt)

/*
 *		getrelid
 *
 *		Given the range index of a relation, return the corresponding
 *		relation OID.  Note that InvalidOid will be returned if the
 *		RTE is for a sub-select rather than a relation.
 */
#define getrelid(rangeindex,rangetable) \
	(rt_fetch(rangeindex, rangetable)->relid)

/*
 * Given an RTE and an attribute number, return the appropriate
 * variable name or alias for that attribute of that RTE.
 */
extern char *get_rte_attribute_name(RangeTblEntry *rte, AttrNumber attnum);

#endif	 /* PARSETREE_H */
