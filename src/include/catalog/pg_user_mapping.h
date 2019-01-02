/*-------------------------------------------------------------------------
 *
 * pg_user_mapping.h
 *	  definition of the "user mapping" system catalog (pg_user_mapping)
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_user_mapping.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_USER_MAPPING_H
#define PG_USER_MAPPING_H

#include "catalog/genbki.h"
#include "catalog/pg_user_mapping_d.h"

/* ----------------
 *		pg_user_mapping definition.  cpp turns this into
 *		typedef struct FormData_pg_user_mapping
 * ----------------
 */
CATALOG(pg_user_mapping,1418,UserMappingRelationId)
{
	Oid			oid;			/* oid */

	Oid			umuser;			/* Id of the user, InvalidOid if PUBLIC is
								 * wanted */
	Oid			umserver;		/* server of this mapping */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		umoptions[1];	/* user mapping options */
#endif
} FormData_pg_user_mapping;

/* ----------------
 *		Form_pg_user_mapping corresponds to a pointer to a tuple with
 *		the format of pg_user_mapping relation.
 * ----------------
 */
typedef FormData_pg_user_mapping *Form_pg_user_mapping;

#endif							/* PG_USER_MAPPING_H */
