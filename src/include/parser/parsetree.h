/*-------------------------------------------------------------------------
 *
 * parsetree.h
 *	  Routines to access various components and subcomponents of
 *	  parse trees.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parsetree.h,v 1.22 2003/08/11 20:46:47 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSETREE_H
#define PARSETREE_H

#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"		/* for nth(), etc */


/* ----------------
 *		range table operations
 * ----------------
 */

/*
 *		rt_fetch
 *
 * NB: this will crash and burn if handed an out-of-range RT index
 */
#define rt_fetch(rangetable_index, rangetable) \
	((RangeTblEntry *) nth((rangetable_index)-1, rangetable))

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


/* ----------------
 *		target list operations
 * ----------------
 */

extern TargetEntry *get_tle_by_resno(List *tlist, AttrNumber resno);

#endif   /* PARSETREE_H */
