/*-------------------------------------------------------------------------
 *
 * pg_publication_namespace.h
 *	  definition of the system catalog for mappings between schemas and
 *	  publications (pg_publication_namespace)
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_publication_namespace.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PUBLICATION_NAMESPACE_H
#define PG_PUBLICATION_NAMESPACE_H

#include "catalog/genbki.h"
#include "catalog/pg_publication_namespace_d.h"


/* ----------------
 *		pg_publication_namespace definition.  cpp turns this into
 *		typedef struct FormData_pg_publication_namespace
 * ----------------
 */
CATALOG(pg_publication_namespace,6237,PublicationNamespaceRelationId)
{
	Oid			oid;			/* oid */
	Oid			pnpubid BKI_LOOKUP(pg_publication); /* Oid of the publication */
	Oid			pnnspid BKI_LOOKUP(pg_namespace);	/* Oid of the schema */
} FormData_pg_publication_namespace;

/* ----------------
 *		Form_pg_publication_namespace corresponds to a pointer to a tuple with
 *		the format of pg_publication_namespace relation.
 * ----------------
 */
typedef FormData_pg_publication_namespace *Form_pg_publication_namespace;

DECLARE_UNIQUE_INDEX_PKEY(pg_publication_namespace_oid_index, 6238, PublicationNamespaceObjectIndexId, on pg_publication_namespace using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_publication_namespace_pnnspid_pnpubid_index, 6239, PublicationNamespacePnnspidPnpubidIndexId, on pg_publication_namespace using btree(pnnspid oid_ops, pnpubid oid_ops));

#endif							/* PG_PUBLICATION_NAMESPACE_H */
