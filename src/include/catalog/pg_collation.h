/*-------------------------------------------------------------------------
 *
 * pg_collation.h
 *	  definition of the "collation" system catalog (pg_collation)
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_collation.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_COLLATION_H
#define PG_COLLATION_H

#include "catalog/genbki.h"
#include "catalog/pg_collation_d.h"

/* ----------------
 *		pg_collation definition.  cpp turns this into
 *		typedef struct FormData_pg_collation
 * ----------------
 */
CATALOG(pg_collation,3456,CollationRelationId)
{
	Oid			oid;			/* oid */
	NameData	collname;		/* collation name */

	/* OID of namespace containing this collation */
	Oid			collnamespace BKI_DEFAULT(pg_catalog) BKI_LOOKUP(pg_namespace);

	/* owner of collation */
	Oid			collowner BKI_DEFAULT(POSTGRES) BKI_LOOKUP(pg_authid);
	char		collprovider;	/* see constants below */
	bool		collisdeterministic BKI_DEFAULT(t);
	int32		collencoding;	/* encoding for this collation; -1 = "all" */
	NameData	collcollate;	/* LC_COLLATE setting */
	NameData	collctype;		/* LC_CTYPE setting */
} FormData_pg_collation;

/* ----------------
 *		Form_pg_collation corresponds to a pointer to a row with
 *		the format of pg_collation relation.
 * ----------------
 */
typedef FormData_pg_collation *Form_pg_collation;

DECLARE_UNIQUE_INDEX(pg_collation_name_enc_nsp_index, 3164, on pg_collation using btree(collname name_ops, collencoding int4_ops, collnamespace oid_ops));
#define CollationNameEncNspIndexId 3164
DECLARE_UNIQUE_INDEX_PKEY(pg_collation_oid_index, 3085, on pg_collation using btree(oid oid_ops));
#define CollationOidIndexId  3085

#ifdef EXPOSE_TO_CLIENT_CODE

#define COLLPROVIDER_DEFAULT	'd'
#define COLLPROVIDER_ICU		'i'
#define COLLPROVIDER_LIBC		'c'

#endif							/* EXPOSE_TO_CLIENT_CODE */


extern Oid	CollationCreate(const char *collname, Oid collnamespace,
							Oid collowner,
							char collprovider,
							bool collisdeterministic,
							int32 collencoding,
							const char *collcollate, const char *collctype,
							bool if_not_exists,
							bool quiet);

#endif							/* PG_COLLATION_H */
