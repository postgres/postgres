/*-------------------------------------------------------------------------
 *
 * pg_largeobject.h
 *	  definition of the "large object" system catalog (pg_largeobject)
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_largeobject.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LARGEOBJECT_H
#define PG_LARGEOBJECT_H

#include "catalog/genbki.h"
#include "catalog/pg_largeobject_d.h"

/* ----------------
 *		pg_largeobject definition.  cpp turns this into
 *		typedef struct FormData_pg_largeobject
 * ----------------
 */
CATALOG(pg_largeobject,2613,LargeObjectRelationId)
{
	Oid			loid BKI_LOOKUP(pg_largeobject_metadata);	/* Identifier of large
															 * object */
	int32		pageno;			/* Page number (starting from 0) */

	/* data has variable length, but we allow direct access; see inv_api.c */
	bytea		data BKI_FORCE_NOT_NULL;	/* Data for page (may be
											 * zero-length) */
} FormData_pg_largeobject;

/* ----------------
 *		Form_pg_largeobject corresponds to a pointer to a tuple with
 *		the format of pg_largeobject relation.
 * ----------------
 */
typedef FormData_pg_largeobject *Form_pg_largeobject;

DECLARE_UNIQUE_INDEX_PKEY(pg_largeobject_loid_pn_index, 2683, LargeObjectLOidPNIndexId, pg_largeobject, btree(loid oid_ops, pageno int4_ops));

extern Oid	LargeObjectCreate(Oid loid);
extern void LargeObjectDrop(Oid loid);
extern bool LargeObjectExists(Oid loid);

#endif							/* PG_LARGEOBJECT_H */
