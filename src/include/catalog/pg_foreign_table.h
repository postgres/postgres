/*-------------------------------------------------------------------------
 *
 * pg_foreign_table.h
 *	  definition of the "foreign table" system catalog (pg_foreign_table)
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_foreign_table.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_FOREIGN_TABLE_H
#define PG_FOREIGN_TABLE_H

#include "catalog/genbki.h"
#include "catalog/pg_foreign_table_d.h"

/* ----------------
 *		pg_foreign_table definition.  cpp turns this into
 *		typedef struct FormData_pg_foreign_table
 * ----------------
 */
CATALOG(pg_foreign_table,3118,ForeignTableRelationId)
{
	Oid			ftrelid BKI_LOOKUP(pg_class);	/* OID of foreign table */
	Oid			ftserver BKI_LOOKUP(pg_foreign_server); /* OID of foreign server */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		ftoptions[1];	/* FDW-specific options */
#endif
} FormData_pg_foreign_table;

/* ----------------
 *		Form_pg_foreign_table corresponds to a pointer to a tuple with
 *		the format of pg_foreign_table relation.
 * ----------------
 */
typedef FormData_pg_foreign_table *Form_pg_foreign_table;

DECLARE_TOAST(pg_foreign_table, 4153, 4154);

DECLARE_UNIQUE_INDEX_PKEY(pg_foreign_table_relid_index, 3119, on pg_foreign_table using btree(ftrelid oid_ops));
#define ForeignTableRelidIndexId 3119

#endif							/* PG_FOREIGN_TABLE_H */
