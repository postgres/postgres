/*-------------------------------------------------------------------------
 *
 * pg_inherits.h
 *	  definition of the "inherits" system catalog (pg_inherits)
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_inherits.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_INHERITS_H
#define PG_INHERITS_H

#include "catalog/genbki.h"
#include "catalog/pg_inherits_d.h"

#include "nodes/pg_list.h"
#include "storage/lock.h"

/* ----------------
 *		pg_inherits definition.  cpp turns this into
 *		typedef struct FormData_pg_inherits
 * ----------------
 */
CATALOG(pg_inherits,2611,InheritsRelationId)
{
	Oid			inhrelid;
	Oid			inhparent;
	int32		inhseqno;
} FormData_pg_inherits;

/* ----------------
 *		Form_pg_inherits corresponds to a pointer to a tuple with
 *		the format of pg_inherits relation.
 * ----------------
 */
typedef FormData_pg_inherits *Form_pg_inherits;

DECLARE_UNIQUE_INDEX(pg_inherits_relid_seqno_index, 2680, on pg_inherits using btree(inhrelid oid_ops, inhseqno int4_ops));
#define InheritsRelidSeqnoIndexId  2680
DECLARE_INDEX(pg_inherits_parent_index, 2187, on pg_inherits using btree(inhparent oid_ops));
#define InheritsParentIndexId  2187


extern List *find_inheritance_children(Oid parentrelId, LOCKMODE lockmode);
extern List *find_all_inheritors(Oid parentrelId, LOCKMODE lockmode,
								 List **parents);
extern bool has_subclass(Oid relationId);
extern bool has_superclass(Oid relationId);
extern bool typeInheritsFrom(Oid subclassTypeId, Oid superclassTypeId);
extern void StoreSingleInheritance(Oid relationId, Oid parentOid,
								   int32 seqNumber);
extern bool DeleteInheritsTuple(Oid inhrelid, Oid inhparent);

#endif							/* PG_INHERITS_H */
