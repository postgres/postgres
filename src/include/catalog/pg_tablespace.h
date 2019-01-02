/*-------------------------------------------------------------------------
 *
 * pg_tablespace.h
 *	  definition of the "tablespace" system catalog (pg_tablespace)
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
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
#include "catalog/pg_tablespace_d.h"

/* ----------------
 *		pg_tablespace definition.  cpp turns this into
 *		typedef struct FormData_pg_tablespace
 * ----------------
 */
CATALOG(pg_tablespace,1213,TableSpaceRelationId) BKI_SHARED_RELATION
{
	Oid			oid;			/* oid */
	NameData	spcname;		/* tablespace name */
	Oid			spcowner;		/* owner of tablespace */

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

#endif							/* PG_TABLESPACE_H */
