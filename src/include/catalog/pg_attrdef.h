/*-------------------------------------------------------------------------
 *
 * pg_attrdef.h
 *	  definition of the "attribute defaults" system catalog (pg_attrdef)
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
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
#include "catalog/objectaddress.h"
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

DECLARE_UNIQUE_INDEX(pg_attrdef_adrelid_adnum_index, 2656, AttrDefaultIndexId, on pg_attrdef using btree(adrelid oid_ops, adnum int2_ops));
DECLARE_UNIQUE_INDEX_PKEY(pg_attrdef_oid_index, 2657, AttrDefaultOidIndexId, on pg_attrdef using btree(oid oid_ops));

DECLARE_FOREIGN_KEY((adrelid, adnum), pg_attribute, (attrelid, attnum));


extern Oid	StoreAttrDefault(Relation rel, AttrNumber attnum,
							 Node *expr, bool is_internal,
							 bool add_column_mode);
extern void RemoveAttrDefault(Oid relid, AttrNumber attnum,
							  DropBehavior behavior,
							  bool complain, bool internal);
extern void RemoveAttrDefaultById(Oid attrdefId);

extern Oid	GetAttrDefaultOid(Oid relid, AttrNumber attnum);
extern ObjectAddress GetAttrDefaultColumnAddress(Oid attrdefoid);

#endif							/* PG_ATTRDEF_H */
