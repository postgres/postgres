/*-------------------------------------------------------------------------
 *
 * pg_tablespace.h
 *	  definition of the "tablespace" system catalog (pg_tablespace)
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_tablespace.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TABLESPACE_H
#define PG_TABLESPACE_H

#include "catalog/genbki.h"
#include "catalog/pg_tablespace_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_tablespace definition.  cpp turns this into
 *		typedef struct FormData_pg_tablespace
 * ----------------
 */
CATALOG(pg_tablespace,1213,TableSpaceRelationId) BKI_SHARED_RELATION
{
	Oid			oid;			/* oid */
	NameData	spcname;		/* tablespace name */

	/* owner of tablespace */
	Oid			spcowner BKI_DEFAULT(POSTGRES) BKI_LOOKUP(pg_authid);

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	aclitem		spcacl[1];		/* access permissions */
	text		spcoptions[1];	/* per-tablespace options */
#endif
} FormData_pg_tablespace;

/* ----------------
 *		Form_pg_tablespace corresponds to a pointer to a tuple with
 *		the format of pg_tablespace relation.
 * ----------------
 */
typedef FormData_pg_tablespace *Form_pg_tablespace;

DECLARE_TOAST_WITH_MACRO(pg_tablespace, 4185, 4186, PgTablespaceToastTable, PgTablespaceToastIndex);

DECLARE_UNIQUE_INDEX_PKEY(pg_tablespace_oid_index, 2697, TablespaceOidIndexId, pg_tablespace, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_tablespace_spcname_index, 2698, TablespaceNameIndexId, pg_tablespace, btree(spcname name_ops));

MAKE_SYSCACHE(TABLESPACEOID, pg_tablespace_oid_index, 4);

#endif							/* PG_TABLESPACE_H */
