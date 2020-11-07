/* -------------------------------------------------------------------------
 *
 * pg_shseclabel.h
 *	  definition of the "shared security label" system catalog (pg_shseclabel)
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_shseclabel.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 * -------------------------------------------------------------------------
 */
#ifndef PG_SHSECLABEL_H
#define PG_SHSECLABEL_H

#include "catalog/genbki.h"
#include "catalog/pg_shseclabel_d.h"

/* ----------------
 *		pg_shseclabel definition. cpp turns this into
 *		typedef struct FormData_pg_shseclabel
 * ----------------
 */
CATALOG(pg_shseclabel,3592,SharedSecLabelRelationId) BKI_SHARED_RELATION BKI_ROWTYPE_OID(4066,SharedSecLabelRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	Oid			objoid;			/* OID of the shared object itself */
	Oid			classoid;		/* OID of table containing the shared object */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		provider BKI_FORCE_NOT_NULL;	/* name of label provider */
	text		label BKI_FORCE_NOT_NULL;	/* security label of the object */
#endif
} FormData_pg_shseclabel;

typedef FormData_pg_shseclabel * Form_pg_shseclabel;

DECLARE_TOAST(pg_shseclabel, 4060, 4061);
#define PgShseclabelToastTable 4060
#define PgShseclabelToastIndex 4061

DECLARE_UNIQUE_INDEX(pg_shseclabel_object_index, 3593, on pg_shseclabel using btree(objoid oid_ops, classoid oid_ops, provider text_ops));
#define SharedSecLabelObjectIndexId			3593

#endif							/* PG_SHSECLABEL_H */
