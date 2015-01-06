/*-------------------------------------------------------------------------
 *
 * pg_foreign_table.h
 *	  definition of the system "foreign table" relation (pg_foreign_table)
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_foreign_table.h
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_FOREIGN_TABLE_H
#define PG_FOREIGN_TABLE_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_foreign_table definition.  cpp turns this into
 *		typedef struct FormData_pg_foreign_table
 * ----------------
 */
#define ForeignTableRelationId 3118

CATALOG(pg_foreign_table,3118) BKI_WITHOUT_OIDS
{
	Oid			ftrelid;		/* OID of foreign table */
	Oid			ftserver;		/* OID of foreign server */

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

/* ----------------
 *		compiler constants for pg_foreign_table
 * ----------------
 */

#define Natts_pg_foreign_table					3
#define Anum_pg_foreign_table_ftrelid			1
#define Anum_pg_foreign_table_ftserver			2
#define Anum_pg_foreign_table_ftoptions			3

#endif   /* PG_FOREIGN_TABLE_H */
