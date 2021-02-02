/*-------------------------------------------------------------------------
 *
 * pg_publication_rel.h
 *	  definition of the system catalog for mappings between relations and
 *	  publications (pg_publication_rel)
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_publication_rel.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PUBLICATION_REL_H
#define PG_PUBLICATION_REL_H

#include "catalog/genbki.h"
#include "catalog/pg_publication_rel_d.h"

/* ----------------
 *		pg_publication_rel definition.  cpp turns this into
 *		typedef struct FormData_pg_publication_rel
 * ----------------
 */
CATALOG(pg_publication_rel,6106,PublicationRelRelationId)
{
	Oid			oid;			/* oid */
	Oid			prpubid BKI_LOOKUP(pg_publication); /* Oid of the publication */
	Oid			prrelid BKI_LOOKUP(pg_class);	/* Oid of the relation */
} FormData_pg_publication_rel;

/* ----------------
 *		Form_pg_publication_rel corresponds to a pointer to a tuple with
 *		the format of pg_publication_rel relation.
 * ----------------
 */
typedef FormData_pg_publication_rel *Form_pg_publication_rel;

DECLARE_UNIQUE_INDEX_PKEY(pg_publication_rel_oid_index, 6112, on pg_publication_rel using btree(oid oid_ops));
#define PublicationRelObjectIndexId 6112
DECLARE_UNIQUE_INDEX(pg_publication_rel_prrelid_prpubid_index, 6113, on pg_publication_rel using btree(prrelid oid_ops, prpubid oid_ops));
#define PublicationRelPrrelidPrpubidIndexId 6113

#endif							/* PG_PUBLICATION_REL_H */
