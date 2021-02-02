/*-------------------------------------------------------------------------
 *
 * pg_attrdef.h
 *	  definition of the "attribute defaults" system catalog (pg_attrdef)
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_attrdef.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_ATTRDEF_H
#define PG_ATTRDEF_H

#include "catalog/genbki.h"
#include "catalog/pg_attrdef_d.h"

/* ----------------
 *		pg_attrdef definition.  cpp turns this into
 *		typedef struct FormData_pg_attrdef
 * ----------------
 */
CATALOG(pg_attrdef,2604,AttrDefaultRelationId)
{
	Oid			oid;			/* oid */

	Oid			adrelid BKI_LOOKUP(pg_class);	/* OID of table containing
												 * attribute */
	int16		adnum;			/* attnum of attribute */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	pg_node_tree adbin BKI_FORCE_NOT_NULL;	/* nodeToString representation of
											 * default */
#endif
} FormData_pg_attrdef;

/* ----------------
 *		Form_pg_attrdef corresponds to a pointer to a tuple with
 *		the format of pg_attrdef relation.
 * ----------------
 */
typedef FormData_pg_attrdef *Form_pg_attrdef;

DECLARE_TOAST(pg_attrdef, 2830, 2831);

DECLARE_UNIQUE_INDEX(pg_attrdef_adrelid_adnum_index, 2656, on pg_attrdef using btree(adrelid oid_ops, adnum int2_ops));
#define AttrDefaultIndexId	2656
DECLARE_UNIQUE_INDEX_PKEY(pg_attrdef_oid_index, 2657, on pg_attrdef using btree(oid oid_ops));
#define AttrDefaultOidIndexId  2657

DECLARE_FOREIGN_KEY((adrelid, adnum), pg_attribute, (attrelid, attnum));

#endif							/* PG_ATTRDEF_H */
