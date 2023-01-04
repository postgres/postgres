/*-------------------------------------------------------------------------
 *
 * pg_extension.h
 *	  definition of the "extension" system catalog (pg_extension)
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_extension.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_EXTENSION_H
#define PG_EXTENSION_H

#include "catalog/genbki.h"
#include "catalog/pg_extension_d.h"

/* ----------------
 *		pg_extension definition.  cpp turns this into
 *		typedef struct FormData_pg_extension
 * ----------------
 */
CATALOG(pg_extension,3079,ExtensionRelationId)
{
	Oid			oid;			/* oid */
	NameData	extname;		/* extension name */
	Oid			extowner BKI_LOOKUP(pg_authid); /* extension owner */
	Oid			extnamespace BKI_LOOKUP(pg_namespace);	/* namespace of
														 * contained objects */
	bool		extrelocatable; /* if true, allow ALTER EXTENSION SET SCHEMA */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	/* extversion may never be null, but the others can be. */
	text		extversion BKI_FORCE_NOT_NULL;	/* extension version name */
	Oid			extconfig[1] BKI_LOOKUP(pg_class);	/* dumpable configuration
													 * tables */
	text		extcondition[1];	/* WHERE clauses for config tables */
#endif
} FormData_pg_extension;

/* ----------------
 *		Form_pg_extension corresponds to a pointer to a tuple with
 *		the format of pg_extension relation.
 * ----------------
 */
typedef FormData_pg_extension *Form_pg_extension;

DECLARE_TOAST(pg_extension, 4147, 4148);

DECLARE_UNIQUE_INDEX_PKEY(pg_extension_oid_index, 3080, ExtensionOidIndexId, on pg_extension using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_extension_name_index, 3081, ExtensionNameIndexId, on pg_extension using btree(extname name_ops));

#endif							/* PG_EXTENSION_H */
