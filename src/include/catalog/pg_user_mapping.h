/*-------------------------------------------------------------------------
 *
 * pg_user_mapping.h
 *	  definition of the "user mapping" system catalog (pg_user_mapping)
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include "catalog/pg_user_mapping_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_user_mapping definition.  cpp turns this into
 *		typedef struct FormData_pg_user_mapping
 * ----------------
 */
CATALOG(pg_user_mapping,1418,UserMappingRelationId)
{
	Oid			oid;			/* oid */

	Oid			umuser BKI_LOOKUP_OPT(pg_authid);	/* Id of the user,
													 * InvalidOid if PUBLIC is
													 * wanted */
	Oid			umserver BKI_LOOKUP(pg_foreign_server); /* server of this
														 * mapping */

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

DECLARE_TOAST(pg_user_mapping, 4173, 4174);

DECLARE_UNIQUE_INDEX_PKEY(pg_user_mapping_oid_index, 174, UserMappingOidIndexId, pg_user_mapping, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_user_mapping_user_server_index, 175, UserMappingUserServerIndexId, pg_user_mapping, btree(umuser oid_ops, umserver oid_ops));

MAKE_SYSCACHE(USERMAPPINGOID, pg_user_mapping_oid_index, 2);
MAKE_SYSCACHE(USERMAPPINGUSERSERVER, pg_user_mapping_user_server_index, 2);

#endif							/* PG_USER_MAPPING_H */
