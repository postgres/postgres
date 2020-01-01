/*-------------------------------------------------------------------------
 *
 * pg_attrdef.h
 *	  definition of the "attribute defaults" system catalog (pg_attrdef)
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
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

	Oid			adrelid;		/* OID of table containing attribute */
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

#endif							/* PG_ATTRDEF_H */
