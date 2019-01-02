/*-------------------------------------------------------------------------
 *
 * pg_extension.h
 *	  definition of the "extension" system catalog (pg_extension)
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
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
	Oid			extowner;		/* extension owner */
	Oid			extnamespace;	/* namespace of contained objects */
	bool		extrelocatable; /* if true, allow ALTER EXTENSION SET SCHEMA */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	/* extversion may never be null, but the others can be. */
	text		extversion BKI_FORCE_NOT_NULL;	/* extension version name */
	Oid			extconfig[1];	/* dumpable configuration tables */
	text		extcondition[1];	/* WHERE clauses for config tables */
#endif
} FormData_pg_extension;

/* ----------------
 *		Form_pg_extension corresponds to a pointer to a tuple with
 *		the format of pg_extension relation.
 * ----------------
 */
typedef FormData_pg_extension *Form_pg_extension;

#endif							/* PG_EXTENSION_H */
