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
 * $Id: parsetree.h,v 1.17 2002/03/12 00:52:04 tgl Exp $
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
 *		RTE is for a non-relation-type RTE.
 */
#define getrelid(rangeindex,rangetable) \
	(rt_fetch(rangeindex, rangetable)->relid)

/*
 * Given an RTE and an attribute number, return the appropriate
 * variable name or alias for that attribute of that RTE.
 */
extern char *get_rte_attribute_name(RangeTblEntry *rte, AttrNumber attnum);

/*
 * Given an RTE and an attribute number, return the appropriate
 * type and typemod info for that attribute of that RTE.
 */
extern void get_rte_attribute_type(RangeTblEntry *rte, AttrNumber attnum,
								   Oid *vartype, int32 *vartypmod);

#endif   /* PARSETREE_H */
